/*
 * nvkvm_tables.c — implementation of the four-table model.
 *
 * Locking discipline:
 *
 *   - Each table has its own mutex.
 *   - Cross-table operations (e.g. close-handle) take locks in fixed order:
 *       windows → mmaps → iso_hnd → handles → isolates
 *     (innermost first). Callers must not hold a lock from later in the
 *     order when entering an earlier one — keeps the deadlock graph acyclic.
 *
 *   - Handle entries also carry their own per-entry condvar
 *     (refcount_zero_cv) used by close()/drain. The condvar is signalled
 *     under the handles table lock.
 *
 *   - Generation-tagged IDs: low 12 bits = slot index, high 20 bits = epoch.
 *     Stale ids from a previous owner fail lookup with -ENOENT. This makes
 *     ID reuse safe — a freed slot's id space is guaranteed distinct from
 *     the next allocation.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nvkvm_tables.h"

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

#include <assert.h>
#include <linux/falloc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Use linux/kvm.h if available for KVM_SET_USER_MEMORY_REGION; tests don't
 * have it. We gate on a compile-time symbol set by the test build. */
#ifndef NVKVM_TABLES_NO_KVM
#  include <linux/kvm.h>
#endif

/* ── Generation-tagged ID encoding ──────────────────────────────────────── */

#define ID_SLOT_BITS  12
#define ID_SLOT_MASK  ((1u << ID_SLOT_BITS) - 1u)
#define ID_GEN_SHIFT  ID_SLOT_BITS

static inline uint32_t id_pack(uint16_t gen, uint16_t slot)
{
	return ((uint32_t)gen << ID_GEN_SHIFT) | ((uint32_t)slot & ID_SLOT_MASK);
}
static inline uint16_t id_slot(uint32_t id) { return id & ID_SLOT_MASK; }
static inline uint16_t id_gen(uint32_t id) { return (uint16_t)(id >> ID_GEN_SHIFT); }

/* ── Forward declarations of internal helpers ──────────────────────────── */

static int  window_add_free_region(struct nvkvm_gpa_window *w,
				    uint64_t off, uint64_t size);
static void window_destroy_locked(struct nvkvm_gpa_window *w, int kvm_vm_fd);

/* ── Host physical address width ───────────────────────────────────────── */

/*
 * Host MAXPHYADDR from CPUID leaf 0x80000008 EAX[7:0], /proc/cpuinfo as a
 * fallback.  Kept self-contained: this file is also built standalone by
 * tests/unit, so it must not pull in the QEMU device headers.
 */
static uint32_t tables_host_phys_bits(void)
{
#if defined(__x86_64__) || defined(__i386__)
	uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

	if (__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) &&
	    eax >= 0x80000008u &&
	    __get_cpuid(0x80000008u, &eax, &ebx, &ecx, &edx)) {
		uint32_t bits = eax & 0xffu;
		if (bits >= 32 && bits <= 64)
			return bits;
	}
#endif
	{
		FILE *f = fopen("/proc/cpuinfo", "r");
		char line[256];
		uint32_t out = 0;

		if (f) {
			while (fgets(line, sizeof(line), f)) {
				unsigned int b = 0;
				const char *colon;

				if (strncmp(line, "address sizes", 13) != 0)
					continue;
				colon = strchr(line, ':');
				if (colon &&
				    sscanf(colon + 1, " %u bits physical", &b) == 1 &&
				    b >= 32 && b <= 64) {
					out = b;
					break;
				}
			}
			fclose(f);
		}
		if (out)
			return out;
	}
	return 36;   /* x86-64 architectural minimum */
}

static uint64_t tables_default_gpa_base(void)
{
	uint32_t bits = tables_host_phys_bits();
	uint64_t half;

	if (bits > 52)
		bits = 52;
	half = 1ULL << (bits - 1);
	return half < (1ULL << 40) ? half : (1ULL << 40);
}

/* ── nvkvm_tables_init / fini ──────────────────────────────────────────── */

void nvkvm_tables_init(struct nvkvm_tables *t)
{
	memset(t, 0, sizeof(*t));
	pthread_mutex_init(&t->handles.lock,  NULL);
	pthread_mutex_init(&t->isolates.lock, NULL);
	pthread_mutex_init(&t->iso_hnd.lock,  NULL);
	pthread_mutex_init(&t->mmaps.lock,    NULL);
	pthread_mutex_init(&t->windows.lock,  NULL);

	for (int i = 0; i < NVKVM_TABLES_MAX_HANDLES; i++) {
		pthread_cond_init(&t->handles.slots[i].refcount_zero_cv, NULL);
		t->handles.slots[i].qemu_fd = -1;
	}
	t->handles.next_slot  = 1;   /* slot 0 reserved → id 0 is "invalid" */
	t->isolates.next_slot = 1;
	t->mmaps.next_slot    = 1;

	/* GPA windows: choose a base above any plausible guest RAM extent but
	 * still inside the host's addressable GPA range.
	 *
	 * This used to be a flat 1 TiB, which needs 41 physical address bits and
	 * so is rejected by KVM on any consumer part (mobile Intel is 39 bits /
	 * 512 GiB) — the same portability bug as the device's own windows.
	 * Derive it from the host width instead: half the addressable space,
	 * capped at 1 TiB so wide hosts keep the historical layout. */
	t->windows.next_gpa_base = tables_default_gpa_base();
}

void nvkvm_tables_fini(struct nvkvm_tables *t)
{
	/* Tear down windows first (they own memfds + mappings). */
	pthread_mutex_lock(&t->windows.lock);
	for (int i = 0; i < NVKVM_TABLES_MAX_WINDOWS; i++) {
		if (t->windows.windows[i].in_use)
			window_destroy_locked(&t->windows.windows[i], -1);
	}
	pthread_mutex_unlock(&t->windows.lock);

	for (int i = 0; i < NVKVM_TABLES_MAX_HANDLES; i++) {
		if (t->handles.slots[i].in_use &&
		    t->handles.slots[i].qemu_fd >= 0)
			close(t->handles.slots[i].qemu_fd);
		pthread_cond_destroy(&t->handles.slots[i].refcount_zero_cv);
	}
	for (int i = 0; i < NVKVM_TABLES_MAX_ISOLATES; i++) {
		if (t->isolates.slots[i].in_use &&
		    t->isolates.slots[i].comm_fd >= 0)
			close(t->isolates.slots[i].comm_fd);
	}

	pthread_mutex_destroy(&t->handles.lock);
	pthread_mutex_destroy(&t->isolates.lock);
	pthread_mutex_destroy(&t->iso_hnd.lock);
	pthread_mutex_destroy(&t->mmaps.lock);
	pthread_mutex_destroy(&t->windows.lock);
}

/* ── Handle table ──────────────────────────────────────────────────────── */

int nvkvm_tables_handle_alloc(struct nvkvm_tables *t,
			       enum nvkvm_handle_type type,
			       uint32_t *out_handle_id)
{
	if (type == NVKVM_HND_INVALID || !out_handle_id)
		return -EINVAL;

	pthread_mutex_lock(&t->handles.lock);
	for (uint32_t i = 0; i < NVKVM_TABLES_MAX_HANDLES; i++) {
		uint32_t s = (t->handles.next_slot + i) % NVKVM_TABLES_MAX_HANDLES;
		if (s == 0) continue;           /* reserved */
		struct nvkvm_handle_entry *e = &t->handles.slots[s];
		if (e->in_use) continue;

		/* Bump generation so the new id can never collide with an
		 * old, stale id pointing at this slot. */
		t->handles.generation[s]++;
		uint32_t id = id_pack(t->handles.generation[s], (uint16_t)s);

		e->in_use       = true;
		e->handle_id    = id;
		e->type         = type;
		e->qemu_fd      = -1;
		e->ready        = false;
		e->poll_enabled = false;
		e->refcount     = 0;

		t->handles.next_slot = (s + 1) % NVKVM_TABLES_MAX_HANDLES;
		if (t->handles.next_slot == 0)
			t->handles.next_slot = 1;
		*out_handle_id = id;
		pthread_mutex_unlock(&t->handles.lock);
		return 0;
	}
	pthread_mutex_unlock(&t->handles.lock);
	return -ENOSPC;
}

/* Look up by id under the table lock. Returns the entry or NULL.
 * Caller must hold &t->handles.lock. */
static struct nvkvm_handle_entry *
handle_lookup_locked(struct nvkvm_tables *t, uint32_t handle_id)
{
	uint16_t s = id_slot(handle_id);
	if (s == 0 || s >= NVKVM_TABLES_MAX_HANDLES)
		return NULL;
	struct nvkvm_handle_entry *e = &t->handles.slots[s];
	if (!e->in_use || e->handle_id != handle_id)
		return NULL;
	return e;
}

int nvkvm_tables_handle_attach_qemu_fd(struct nvkvm_tables *t,
					uint32_t handle_id, int qemu_fd)
{
	if (qemu_fd < 0) return -EINVAL;

	pthread_mutex_lock(&t->handles.lock);
	struct nvkvm_handle_entry *e = handle_lookup_locked(t, handle_id);
	if (!e) {
		pthread_mutex_unlock(&t->handles.lock);
		return -ENOENT;
	}
	if (e->ready) {
		pthread_mutex_unlock(&t->handles.lock);
		return -EALREADY;
	}
	e->qemu_fd = qemu_fd;
	e->ready   = true;
	pthread_mutex_unlock(&t->handles.lock);
	return 0;
}

int nvkvm_tables_handle_abort_open(struct nvkvm_tables *t, uint32_t handle_id)
{
	pthread_mutex_lock(&t->handles.lock);
	struct nvkvm_handle_entry *e = handle_lookup_locked(t, handle_id);
	if (!e) {
		pthread_mutex_unlock(&t->handles.lock);
		return -ENOENT;
	}
	if (e->ready) {
		/* Already past open transaction — caller should close()
		 * properly, not abort. */
		pthread_mutex_unlock(&t->handles.lock);
		return -EALREADY;
	}
	int fd = e->qemu_fd;
	memset(e, 0, sizeof(*e));
	e->qemu_fd = -1;
	pthread_mutex_unlock(&t->handles.lock);
	if (fd >= 0) close(fd);
	return 0;
}

int nvkvm_tables_handle_acquire(struct nvkvm_tables *t, uint32_t handle_id,
				 struct nvkvm_handle_entry **out_entry)
{
	pthread_mutex_lock(&t->handles.lock);
	struct nvkvm_handle_entry *e = handle_lookup_locked(t, handle_id);
	if (!e) {
		pthread_mutex_unlock(&t->handles.lock);
		return -ENOENT;
	}
	if (!e->ready) {
		pthread_mutex_unlock(&t->handles.lock);
		return -EBUSY;
	}
	e->refcount++;
	*out_entry = e;
	pthread_mutex_unlock(&t->handles.lock);
	return 0;
}

void nvkvm_tables_handle_release(struct nvkvm_tables *t, uint32_t handle_id)
{
	pthread_mutex_lock(&t->handles.lock);
	struct nvkvm_handle_entry *e = handle_lookup_locked(t, handle_id);
	if (!e || e->refcount == 0) {
		pthread_mutex_unlock(&t->handles.lock);
		return;
	}
	e->refcount--;
	if (e->refcount == 0)
		pthread_cond_broadcast(&e->refcount_zero_cv);
	pthread_mutex_unlock(&t->handles.lock);
}

int nvkvm_tables_handle_close(struct nvkvm_tables *t, uint32_t handle_id)
{
	/* Dependency check: no mmaps, no iso↔handle links. */
	if (nvkvm_tables_mmap_count_for_handle(t, handle_id) > 0)
		return -EBUSY;
	if (nvkvm_tables_iso_hnd_count_for_handle(t, handle_id) > 0)
		return -EBUSY;

	pthread_mutex_lock(&t->handles.lock);
	struct nvkvm_handle_entry *e = handle_lookup_locked(t, handle_id);
	if (!e) {
		pthread_mutex_unlock(&t->handles.lock);
		return -ENOENT;
	}
	/* Mark closing (idempotent if already closing). */
	e->ready = false;

	/* Drain refcount. New acquirers see ready=false and bail. */
	while (e->refcount > 0)
		pthread_cond_wait(&e->refcount_zero_cv, &t->handles.lock);

	int fd = e->qemu_fd;
	memset(e, 0, sizeof(*e));
	e->qemu_fd = -1;
	pthread_mutex_unlock(&t->handles.lock);

	if (fd >= 0) close(fd);
	return 0;
}

/* ── Isolate table ─────────────────────────────────────────────────────── */

int nvkvm_tables_isolate_alloc(struct nvkvm_tables *t, pid_t pid, int comm_fd,
				uint32_t *out_isolate_id)
{
	if (!out_isolate_id) return -EINVAL;

	pthread_mutex_lock(&t->isolates.lock);
	for (uint32_t i = 0; i < NVKVM_TABLES_MAX_ISOLATES; i++) {
		uint32_t s = (t->isolates.next_slot + i) % NVKVM_TABLES_MAX_ISOLATES;
		if (s == 0) continue;
		struct nvkvm_isolate_entry *e = &t->isolates.slots[s];
		if (e->in_use) continue;

		t->isolates.generation[s]++;
		uint32_t id = id_pack(t->isolates.generation[s], (uint16_t)s);

		e->in_use     = true;
		e->isolate_id = id;
		e->pid        = pid;
		e->comm_fd    = comm_fd;
		e->alive      = true;
		t->isolates.next_slot = (s + 1) % NVKVM_TABLES_MAX_ISOLATES;
		if (t->isolates.next_slot == 0)
			t->isolates.next_slot = 1;
		*out_isolate_id = id;
		pthread_mutex_unlock(&t->isolates.lock);
		return 0;
	}
	pthread_mutex_unlock(&t->isolates.lock);
	return -ENOSPC;
}

static struct nvkvm_isolate_entry *
isolate_lookup_locked(struct nvkvm_tables *t, uint32_t isolate_id)
{
	uint16_t s = id_slot(isolate_id);
	if (s == 0 || s >= NVKVM_TABLES_MAX_ISOLATES) return NULL;
	struct nvkvm_isolate_entry *e = &t->isolates.slots[s];
	if (!e->in_use || e->isolate_id != isolate_id) return NULL;
	return e;
}

int nvkvm_tables_isolate_mark_dead(struct nvkvm_tables *t, uint32_t isolate_id)
{
	pthread_mutex_lock(&t->isolates.lock);
	struct nvkvm_isolate_entry *e = isolate_lookup_locked(t, isolate_id);
	if (!e) { pthread_mutex_unlock(&t->isolates.lock); return -ENOENT; }
	e->alive = false;
	pthread_mutex_unlock(&t->isolates.lock);
	return 0;
}

int nvkvm_tables_isolate_close(struct nvkvm_tables *t, uint32_t isolate_id)
{
	/* Caller is expected to have unlinked all iso↔handle entries first. */
	pthread_mutex_lock(&t->isolates.lock);
	struct nvkvm_isolate_entry *e = isolate_lookup_locked(t, isolate_id);
	if (!e) { pthread_mutex_unlock(&t->isolates.lock); return -ENOENT; }
	int fd = e->comm_fd;
	memset(e, 0, sizeof(*e));
	e->comm_fd = -1;
	pthread_mutex_unlock(&t->isolates.lock);
	if (fd >= 0) close(fd);
	return 0;
}

/* ── Iso ↔ Hnd map (M:N) ───────────────────────────────────────────────── */

int nvkvm_tables_iso_hnd_link(struct nvkvm_tables *t,
			       uint32_t isolate_id, uint32_t handle_id,
			       int fd_on_isolate)
{
	if (fd_on_isolate < 0) return -EINVAL;

	pthread_mutex_lock(&t->iso_hnd.lock);
	/* Duplicate check */
	for (int i = 0; i < NVKVM_TABLES_MAX_ISO_HND_LINKS; i++) {
		if (t->iso_hnd.slots[i].in_use &&
		    t->iso_hnd.slots[i].isolate_id == isolate_id &&
		    t->iso_hnd.slots[i].handle_id == handle_id) {
			pthread_mutex_unlock(&t->iso_hnd.lock);
			return -EEXIST;
		}
	}
	/* Find a free slot */
	for (int i = 0; i < NVKVM_TABLES_MAX_ISO_HND_LINKS; i++) {
		if (!t->iso_hnd.slots[i].in_use) {
			t->iso_hnd.slots[i] = (struct nvkvm_iso_hnd_entry){
				.in_use        = true,
				.isolate_id    = isolate_id,
				.handle_id     = handle_id,
				.fd_on_isolate = fd_on_isolate,
			};
			pthread_mutex_unlock(&t->iso_hnd.lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&t->iso_hnd.lock);
	return -ENOSPC;
}

int nvkvm_tables_iso_hnd_unlink(struct nvkvm_tables *t,
				 uint32_t isolate_id, uint32_t handle_id)
{
	pthread_mutex_lock(&t->iso_hnd.lock);
	for (int i = 0; i < NVKVM_TABLES_MAX_ISO_HND_LINKS; i++) {
		if (t->iso_hnd.slots[i].in_use &&
		    t->iso_hnd.slots[i].isolate_id == isolate_id &&
		    t->iso_hnd.slots[i].handle_id == handle_id) {
			memset(&t->iso_hnd.slots[i], 0,
			       sizeof(t->iso_hnd.slots[i]));
			pthread_mutex_unlock(&t->iso_hnd.lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&t->iso_hnd.lock);
	return -ENOENT;
}

int nvkvm_tables_iso_hnd_lookup_fd(struct nvkvm_tables *t,
				    uint32_t isolate_id, uint32_t handle_id,
				    int *out_fd)
{
	if (!out_fd) return -EINVAL;
	pthread_mutex_lock(&t->iso_hnd.lock);
	for (int i = 0; i < NVKVM_TABLES_MAX_ISO_HND_LINKS; i++) {
		if (t->iso_hnd.slots[i].in_use &&
		    t->iso_hnd.slots[i].isolate_id == isolate_id &&
		    t->iso_hnd.slots[i].handle_id == handle_id) {
			*out_fd = t->iso_hnd.slots[i].fd_on_isolate;
			pthread_mutex_unlock(&t->iso_hnd.lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&t->iso_hnd.lock);
	return -ENOENT;
}

int nvkvm_tables_iso_hnd_count_for_handle(struct nvkvm_tables *t,
					   uint32_t handle_id)
{
	int n = 0;
	pthread_mutex_lock(&t->iso_hnd.lock);
	for (int i = 0; i < NVKVM_TABLES_MAX_ISO_HND_LINKS; i++) {
		if (t->iso_hnd.slots[i].in_use &&
		    t->iso_hnd.slots[i].handle_id == handle_id)
			n++;
	}
	pthread_mutex_unlock(&t->iso_hnd.lock);
	return n;
}

/* ── GPA window sub-allocator ──────────────────────────────────────────── */

static uint64_t align_up(uint64_t v, uint64_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static int window_add_free_region(struct nvkvm_gpa_window *w,
				   uint64_t off, uint64_t size)
{
	if (size == 0) return 0;
	struct nvkvm_window_free_region *r = malloc(sizeof(*r));
	if (!r) return -ENOMEM;
	r->offset = off;
	r->size   = size;

	/* Insert sorted by offset, coalesce on insertion. */
	struct nvkvm_window_free_region **pp = &w->free_head;
	while (*pp && (*pp)->offset < off)
		pp = &(*pp)->next;
	r->next = *pp;
	*pp = r;

	/* Coalesce with successor */
	if (r->next && r->offset + r->size == r->next->offset) {
		struct nvkvm_window_free_region *n = r->next;
		r->size += n->size;
		r->next = n->next;
		free(n);
	}
	/* Coalesce with predecessor */
	struct nvkvm_window_free_region *p = w->free_head;
	if (p != r) {
		while (p && p->next != r) p = p->next;
		if (p && p->offset + p->size == r->offset) {
			p->size += r->size;
			p->next = r->next;
			free(r);
		}
	}
	return 0;
}

static int window_take_region(struct nvkvm_gpa_window *w, uint64_t size,
			       uint64_t *out_offset)
{
	struct nvkvm_window_free_region **pp = &w->free_head;
	while (*pp) {
		if ((*pp)->size >= size) {
			*out_offset = (*pp)->offset;
			(*pp)->offset += size;
			(*pp)->size   -= size;
			if ((*pp)->size == 0) {
				struct nvkvm_window_free_region *gone = *pp;
				*pp = gone->next;
				free(gone);
			}
			return 0;
		}
		pp = &(*pp)->next;
	}
	return -ENOSPC;
}

static void window_destroy_locked(struct nvkvm_gpa_window *w, int kvm_vm_fd)
{
	if (!w->in_use) return;

#ifndef NVKVM_TABLES_NO_KVM
	if (kvm_vm_fd >= 0 && w->kvm_slot >= 0) {
		struct kvm_userspace_memory_region mr = {
			.slot            = (uint32_t)w->kvm_slot,
			.flags           = 0,
			.guest_phys_addr = w->gpa_base,
			.memory_size     = 0,
			.userspace_addr  = (uint64_t)(uintptr_t)w->qva,
		};
		ioctl(kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &mr);
	}
#else
	(void)kvm_vm_fd;
#endif
	if (w->qva && w->qva != MAP_FAILED)
		munmap(w->qva, w->size);
	if (w->memfd >= 0)
		close(w->memfd);

	struct nvkvm_window_free_region *f = w->free_head;
	while (f) { struct nvkvm_window_free_region *n = f->next; free(f); f = n; }
	memset(w, 0, sizeof(*w));
	w->memfd    = -1;
	w->kvm_slot = -1;
}

int nvkvm_tables_window_create(struct nvkvm_tables *t, uint64_t size,
				int kvm_vm_fd, uint32_t *out_window_id)
{
	if (!out_window_id || size == 0) return -EINVAL;
	size = align_up(size, NVKVM_TABLES_PAGE_SIZE);

	pthread_mutex_lock(&t->windows.lock);
	uint16_t s = 0;
	for (uint16_t i = 1; i < NVKVM_TABLES_MAX_WINDOWS; i++) {
		if (!t->windows.windows[i].in_use) { s = i; break; }
	}
	if (s == 0) {
		pthread_mutex_unlock(&t->windows.lock);
		return -ENOSPC;
	}

	struct nvkvm_gpa_window *w = &t->windows.windows[s];
	memset(w, 0, sizeof(*w));
	w->memfd    = -1;
	w->kvm_slot = -1;

	/* memfd_create with HUGETLB is nicer for large windows but adds
	 * dependencies; default to plain anonymous-ish memfd. */
	w->memfd = memfd_create("nvkvm-gpa-win", MFD_CLOEXEC);
	if (w->memfd < 0) {
		int e = -errno;
		pthread_mutex_unlock(&t->windows.lock);
		return e;
	}
	if (ftruncate(w->memfd, (off_t)size) < 0) {
		int e = -errno;
		close(w->memfd); w->memfd = -1;
		pthread_mutex_unlock(&t->windows.lock);
		return e;
	}

	w->qva = mmap(NULL, size, PROT_READ | PROT_WRITE,
		      MAP_SHARED, w->memfd, 0);
	if (w->qva == MAP_FAILED) {
		int e = -errno;
		close(w->memfd); w->memfd = -1;
		pthread_mutex_unlock(&t->windows.lock);
		return e;
	}

	w->in_use   = true;
	w->gpa_base = t->windows.next_gpa_base;
	w->size     = size;
	t->windows.next_gpa_base += size;
	t->windows.generation[s]++;
	w->window_id = id_pack(t->windows.generation[s], s);

	if (window_add_free_region(w, 0, size) != 0) {
		window_destroy_locked(w, -1);
		pthread_mutex_unlock(&t->windows.lock);
		return -ENOMEM;
	}

#ifndef NVKVM_TABLES_NO_KVM
	if (kvm_vm_fd >= 0) {
		/* Use a slot # = window slot index for simplicity. */
		struct kvm_userspace_memory_region mr = {
			.slot            = s,
			.flags           = 0,
			.guest_phys_addr = w->gpa_base,
			.memory_size     = size,
			.userspace_addr  = (uint64_t)(uintptr_t)w->qva,
		};
		if (ioctl(kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &mr) < 0) {
			int e = -errno;
			window_destroy_locked(w, -1);
			pthread_mutex_unlock(&t->windows.lock);
			return e;
		}
		w->kvm_slot = (int)s;
	}
#else
	(void)kvm_vm_fd;
#endif

	*out_window_id = w->window_id;
	pthread_mutex_unlock(&t->windows.lock);
	return 0;
}

int nvkvm_tables_window_destroy(struct nvkvm_tables *t,
				 uint32_t window_id, int kvm_vm_fd)
{
	uint16_t s = id_slot(window_id);
	if (s == 0 || s >= NVKVM_TABLES_MAX_WINDOWS) return -ENOENT;
	pthread_mutex_lock(&t->windows.lock);
	struct nvkvm_gpa_window *w = &t->windows.windows[s];
	if (!w->in_use || w->window_id != window_id) {
		pthread_mutex_unlock(&t->windows.lock);
		return -ENOENT;
	}
	window_destroy_locked(w, kvm_vm_fd);
	pthread_mutex_unlock(&t->windows.lock);
	return 0;
}

/* Try to allocate `size` bytes from any window; create one lazily if none. */
static int windows_take(struct nvkvm_tables *t, uint64_t size,
			 uint32_t *out_window_id,
			 uint64_t *out_window_offset,
			 void **out_vmm_va,
			 uint64_t *out_gpa)
{
	pthread_mutex_lock(&t->windows.lock);
	for (int i = 1; i < NVKVM_TABLES_MAX_WINDOWS; i++) {
		struct nvkvm_gpa_window *w = &t->windows.windows[i];
		if (!w->in_use) continue;
		uint64_t off;
		if (window_take_region(w, size, &off) == 0) {
			*out_window_id     = w->window_id;
			*out_window_offset = off;
			*out_vmm_va        = (char *)w->qva + off;
			*out_gpa           = w->gpa_base + off;
			pthread_mutex_unlock(&t->windows.lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&t->windows.lock);
	return -ENOSPC;
}

/* Adjacency-aware punch: find the largest contiguous free region containing
 * [off, off+size) inside the window (i.e. after the just-freed region was
 * coalesced into the free list) and punch the whole thing. Punching a region
 * that was already punched is a kernel-side no-op. */
static int window_punch_around(struct nvkvm_gpa_window *w, uint64_t off,
				uint64_t size)
{
	/* Find the free region containing this offset (post-coalesce). */
	struct nvkvm_window_free_region *r = w->free_head;
	while (r) {
		if (r->offset <= off && off + size <= r->offset + r->size)
			break;
		r = r->next;
	}
	uint64_t p_off = off, p_size = size;
	if (r) { p_off = r->offset; p_size = r->size; }

	p_off = align_up(p_off, NVKVM_TABLES_PAGE_SIZE);
	p_size = (p_size & ~(NVKVM_TABLES_PAGE_SIZE - 1));
	if (p_size == 0) return 0;

	int rc = fallocate(w->memfd,
			   FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			   (off_t)p_off, (off_t)p_size);
	if (rc < 0 && errno != EINVAL && errno != EOPNOTSUPP)
		return -errno;
	return 0;
}

int nvkvm_tables_mmap_alloc(struct nvkvm_tables *t,
			     uint32_t handle_id, uint64_t handle_offset,
			     uint64_t size, int prot,
			     uint64_t *out_gpa, uint32_t *out_mmap_id)
{
	if (!out_gpa || !out_mmap_id || size == 0)
		return -EINVAL;
	size = align_up(size, NVKVM_TABLES_PAGE_SIZE);

	/* Verify handle exists (don't refcount — mmap-table is its own
	 * dependency edge; close-handle rejects while mmaps exist). */
	pthread_mutex_lock(&t->handles.lock);
	struct nvkvm_handle_entry *he = handle_lookup_locked(t, handle_id);
	bool ok = he && he->ready;
	pthread_mutex_unlock(&t->handles.lock);
	if (!ok) return -ENOENT;

	uint32_t win_id;
	uint64_t win_off, gpa;
	void *vmm_va;
	int rc = windows_take(t, size, &win_id, &win_off, &vmm_va, &gpa);
	if (rc < 0) return rc;

	/* Allocate mmap slot */
	pthread_mutex_lock(&t->mmaps.lock);
	uint16_t mslot = 0;
	for (uint32_t i = 0; i < NVKVM_TABLES_MAX_MMAPS; i++) {
		uint32_t s = (t->mmaps.next_slot + i) % NVKVM_TABLES_MAX_MMAPS;
		if (s == 0) continue;
		if (!t->mmaps.slots[s].in_use) { mslot = (uint16_t)s; break; }
	}
	if (mslot == 0) {
		pthread_mutex_unlock(&t->mmaps.lock);
		/* Return the GPA space we took. */
		pthread_mutex_lock(&t->windows.lock);
		for (int i = 1; i < NVKVM_TABLES_MAX_WINDOWS; i++) {
			if (t->windows.windows[i].in_use &&
			    t->windows.windows[i].window_id == win_id) {
				window_add_free_region(&t->windows.windows[i],
						       win_off, size);
				break;
			}
		}
		pthread_mutex_unlock(&t->windows.lock);
		return -ENOSPC;
	}

	t->mmaps.generation[mslot]++;
	uint32_t mid = id_pack(t->mmaps.generation[mslot], mslot);
	t->mmaps.slots[mslot] = (struct nvkvm_mmap_entry){
		.in_use        = true,
		.mmap_id       = mid,
		.handle_id     = handle_id,
		.handle_offset = handle_offset,
		.vmm_va        = vmm_va,
		.gpa           = gpa,
		.size          = size,
		.prot          = prot,
		.window_id     = win_id,
		.window_offset = win_off,
	};
	t->mmaps.next_slot = (mslot + 1) % NVKVM_TABLES_MAX_MMAPS;
	if (t->mmaps.next_slot == 0) t->mmaps.next_slot = 1;
	pthread_mutex_unlock(&t->mmaps.lock);

	*out_gpa     = gpa;
	*out_mmap_id = mid;
	return 0;
}

int nvkvm_tables_mmap_free(struct nvkvm_tables *t, uint32_t mmap_id)
{
	uint16_t s = id_slot(mmap_id);
	if (s == 0 || s >= NVKVM_TABLES_MAX_MMAPS) return -ENOENT;

	pthread_mutex_lock(&t->mmaps.lock);
	struct nvkvm_mmap_entry *m = &t->mmaps.slots[s];
	if (!m->in_use || m->mmap_id != mmap_id) {
		pthread_mutex_unlock(&t->mmaps.lock);
		return -ENOENT;
	}
	uint32_t win_id  = m->window_id;
	uint64_t win_off = m->window_offset;
	uint64_t size    = m->size;
	memset(m, 0, sizeof(*m));
	pthread_mutex_unlock(&t->mmaps.lock);

	/* Return offset to window's free list, then punch hole with ±1
	 * adjacency. We coalesce free regions first so the punch covers
	 * the largest contiguous now-free range. */
	pthread_mutex_lock(&t->windows.lock);
	for (int i = 1; i < NVKVM_TABLES_MAX_WINDOWS; i++) {
		struct nvkvm_gpa_window *w = &t->windows.windows[i];
		if (w->in_use && w->window_id == win_id) {
			window_add_free_region(w, win_off, size);
			window_punch_around(w, win_off, size);
			break;
		}
	}
	pthread_mutex_unlock(&t->windows.lock);
	return 0;
}

int nvkvm_tables_mmap_count_for_handle(struct nvkvm_tables *t,
					uint32_t handle_id)
{
	int n = 0;
	pthread_mutex_lock(&t->mmaps.lock);
	for (int i = 0; i < NVKVM_TABLES_MAX_MMAPS; i++)
		if (t->mmaps.slots[i].in_use &&
		    t->mmaps.slots[i].handle_id == handle_id)
			n++;
	pthread_mutex_unlock(&t->mmaps.lock);
	return n;
}
