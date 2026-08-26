/*
 * nvkvm_isolate_handlers.c — virtio request handlers for isolate/handle commands
 *
 * These handlers are invoked from the virtio TX queue dispatch when the guest
 * sends one of the NVKVM_REQ_* isolate/handle request types.
 *
 * Security: every handle_id and isolate_id is validated before use. Unknown
 * IDs cause the handler to return an error status; the caller in virtio_nvgpu.c
 * will panic the VM if these fields are structurally invalid (e.g., non-existent
 * session_id), but per-operation errors (ENOENT, EBUSY) are propagated normally.
 */

#include "qemu/osdep.h"
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "virtio_nvgpu.h"
#include "nvkvm_ctrl_allowlist.h"
#include "nvkvm_fe_alloc_allowlist.h"
#include "nvkvm_drm_allowlist.h"
#include "nvkvm_nvkms_allowlist.h"
#include "nvkvm_present_egl.h"
#include "nvkvm_display_relay.h"

/* ── Isolate mmap token table ────────────────────────────────────────────── */
/*
 * Each MMAP_ON_ISOLATE allocates one entry.  The token (index into this table)
 * is returned to the guest and used later for MUNMAP_ON_ISOLATE cleanup.
 *
 * Slot 0 is reserved (invalid token).  Tokens wrap in [1, MAX).
 */
#define NVKVM_ISO_MMAP_MAX  8192


struct nvkvm_iso_mmap_entry {
	bool     used;
	bool     stub_mirrored; /* true if isolate-side mmap was also installed */
	uint32_t isolate_id;
	uint64_t gva;        /* the GUEST's VA for this mapping (identity key,
			      * never an address anything maps at) */
	uint64_t stub_va;    /* U-9: where it actually lives in the isolate,
			      * inside that isolate's guest-mapping window */
	void    *qva;        /* QEMU host VA from mmap()  */
	size_t   len;
	int      kvm_slot;   /* KVM memory slot (-1 if none) */
	uint64_t gpa;
	uint32_t handle_id;  /* frontend handle the mapping was made from, so a
			      * close can tear the window extent down; 0 = none */
};

static struct nvkvm_iso_mmap_entry iso_mmap_tbl[NVKVM_ISO_MMAP_MAX];
static uint32_t iso_mmap_seq = 1;
static pthread_mutex_t iso_mmap_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── U-9: the host side of the isolate's guest-mapping window ──────────────
 *
 * The stub reserves one PROT_NONE MAP_NORESERVE region before it maps
 * anything else and refuses every guest-directed mapping outside it (see the
 * long comment on stub_window_contains() in src/stub/nvkvm_stub.c).  This is
 * the allocator that decides WHERE inside that region each mapping goes, and
 * it lives here rather than in the stub for two reasons: the stub is
 * freestanding with no allocator to speak of, and the decision is policy that
 * belongs on the trusted side of the socket.
 *
 * Shape follows nvkvm_sparse_gpa_alloc() deliberately -- bump pointer plus a
 * first-fit free list -- because it is the same problem one layer over.
 *
 * THE ONE THING THAT IS NOT A PLAIN ALLOCATION is the OS-descriptor case.
 * The guest's page migration installs a registered range in NVKVM_MIG_CHUNK
 * (2 MiB) pieces, one MMAP_ON_ISOLATE per chunk (src/guest/nvkvm_mmap.c), and
 * RmAllocOsDescriptor then pins ONE contiguous range covering all of them.
 * If those chunks land scattered through the window there is no single
 * address to hand the driver, so the A-1 gate would refuse every registration
 * above one chunk.  `run` reserves a contiguous span the first time a chunk
 * arrives with no adjacent predecessor and hands the rest of that span to the
 * chunks that follow, matched on guest-VA adjacency -- which is exactly how
 * the migration loop emits them (cbase = start + coff, strictly increasing).
 * Matching on the guest VA rather than on arrival order is what makes this
 * survive two guest threads registering concurrently.
 */
/* NVKVM_WIN_ALLOC_BEGIN -- extracted verbatim into tests/unit by the
 * win_alloc.inc rule.  Everything between the markers must stay pure: no
 * locks, no RPC, no glib, so it compiles into a hosted test binary and the
 * test pins the REAL allocator rather than a copy of it. */
#define NVKVM_WIN_FREE_MAX   256
#define NVKVM_WIN_RUN_MAX    64
/* == the guest's NVKVM_MIG_MAX_RANGE, the largest range one call migrates. */
#define NVKVM_WIN_RUN_SPAN   (2ULL << 30)

struct nvkvm_win_ext { uint64_t off, len; };

struct nvkvm_win_run {
	bool     used;
	uint64_t gva_end;   /* guest VA one past the last chunk placed */
	uint64_t next;      /* window offset where the next chunk goes */
	uint64_t end;       /* window offset one past this reservation */
	uint64_t seq;       /* for LRU retirement */
};

struct nvkvm_iso_window {
	bool     probed;    /* WINDOW_INFO answered (base may still be 0) */
	uint64_t base, size, cur;
	struct nvkvm_win_ext free[NVKVM_WIN_FREE_MAX];
	uint32_t free_n;
	struct nvkvm_win_run run[NVKVM_WIN_RUN_MAX];
	uint64_t run_seq;
};

/* Caller holds iso_win_lock. */
static void win_release_locked(struct nvkvm_iso_window *w,
			       uint64_t off, uint64_t len)
{
	if (!len)
		return;
	/* Coalesce with the bump pointer when the extent is the tail, so a
	 * map/unmap loop does not fragment the window one entry at a time. */
	if (off + len == w->cur) {
		w->cur = off;
		return;
	}
	if (w->free_n < NVKVM_WIN_FREE_MAX) {
		w->free[w->free_n].off = off;
		w->free[w->free_n].len = len;
		w->free_n++;
		return;
	}
	/* Free list full: leak the extent rather than lose track of it.  The
	 * window is 1 TiB against a 128 GiB ceiling on live mappings, so this
	 * is a capacity note, not a correctness one. */
	fprintf(stderr, "nvkvm: window free list full — leaking 0x%llx+0x%llx\n",
		(unsigned long long)off, (unsigned long long)len);
}

/* Caller holds iso_win_lock.  Returns a window offset, or UINT64_MAX. */
static uint64_t win_alloc_locked(struct nvkvm_iso_window *w, uint64_t len)
{
	if (!len || len > w->size)
		return UINT64_MAX;
	len = (len + 4095ULL) & ~4095ULL;
	if (!len)                                   /* round-up wrap */
		return UINT64_MAX;

	uint32_t best = w->free_n;
	for (uint32_t i = 0; i < w->free_n; i++) {
		if (w->free[i].len >= len &&
		    (best == w->free_n || w->free[i].len < w->free[best].len))
			best = i;
	}
	if (best < w->free_n) {
		uint64_t off = w->free[best].off;
		if (w->free[best].len == len)
			w->free[best] = w->free[--w->free_n];
		else {
			w->free[best].off += len;
			w->free[best].len -= len;
		}
		return off;
	}

	uint64_t off = (w->cur + 4095ULL) & ~4095ULL;
	if (off < w->cur || len > w->size - off)
		return UINT64_MAX;
	w->cur = off + len;
	return off;
}

/*
 * Place one mapping in the window.  Caller holds iso_win_lock.
 *
 * `gva` is the guest's VA for this mapping.  It is NEVER used as an address —
 * it is the adjacency key that lets a migration's chunks stay contiguous, and
 * the identity recorded in iso_mmap_tbl.  `contig` asks for the run
 * treatment; only the memory-handle mirrors (the OS-descriptor migration) set
 * it, because only they are later pinned as one range.
 *
 * Returns the window VA, or 0 on failure.  The three properties this must
 * hold, and that tests/unit/test_stub_window.c checks against this exact
 * source, are: every return value is inside [base, base+size); two live
 * placements never overlap; and consecutive guest-VA-adjacent `contig`
 * placements come back window-adjacent.
 */
static uint64_t win_place_locked(struct nvkvm_iso_window *w, uint64_t gva,
				 uint64_t len, bool contig)
{
	uint64_t va = 0;

	if (!w || !w->base || !w->size || !len)
		return 0;

	len = (len + 4095ULL) & ~4095ULL;
	if (!len)                                   /* round-up wrap */
		return 0;

	if (contig && gva) {
		struct nvkvm_win_run *r = NULL;
		for (uint32_t i = 0; i < NVKVM_WIN_RUN_MAX; i++) {
			if (w->run[i].used && w->run[i].gva_end == gva &&
			    len <= w->run[i].end - w->run[i].next) {
				r = &w->run[i];
				break;
			}
		}
		if (!r) {
			/* Start a run: take a free slot, else retire the least
			 * recently extended one (returning its unused tail). */
			struct nvkvm_win_run *victim = NULL;
			for (uint32_t i = 0; i < NVKVM_WIN_RUN_MAX; i++) {
				if (!w->run[i].used) { victim = &w->run[i]; break; }
				if (!victim || w->run[i].seq < victim->seq)
					victim = &w->run[i];
			}
			if (victim->used)
				win_release_locked(w, victim->next,
						   victim->end - victim->next);
			uint64_t span = NVKVM_WIN_RUN_SPAN;
			if (span < len)
				span = len;
			uint64_t off = win_alloc_locked(w, span);
			if (off == UINT64_MAX) {
				victim->used = false;
				return 0;
			}
			victim->used = true;
			victim->next = off;
			victim->end  = off + span;
			r = victim;
		}
		va = w->base + r->next;
		r->next   += len;
		r->gva_end = gva + len;
		r->seq     = ++w->run_seq;
		/* Reservation exactly consumed — retire it so the slot is free
		 * for the next registration. */
		if (r->next >= r->end)
			r->used = false;
		return va;
	}

	uint64_t off = win_alloc_locked(w, len);
	if (off != UINT64_MAX)
		va = w->base + off;
	return va;
}
/* NVKVM_WIN_ALLOC_END */

/* Lazily allocated, indexed the same way the isolate table is. */
static struct nvkvm_iso_window *iso_win[NVKVM_ISOLATE_MAX];
static pthread_mutex_t iso_win_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Locking + one-shot window probe around win_place_locked().
 */
static uint64_t nvkvm_win_place(struct nvkvm_isolate_table *t,
				uint32_t isolate_id, uint64_t gva,
				uint64_t len, bool contig)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX || !len)
		return 0;

	pthread_mutex_lock(&iso_win_lock);
	struct nvkvm_iso_window *w = iso_win[isolate_id % NVKVM_ISOLATE_MAX];
	if (!w) {
		w = g_new0(struct nvkvm_iso_window, 1);
		iso_win[isolate_id % NVKVM_ISOLATE_MAX] = w;
	}
	if (!w->probed) {
		uint64_t base = 0, size = 0;
		/* Dropping the lock around the RPC would let two threads probe
		 * at once; the probe is one round trip at isolate first use and
		 * every caller here is already serialised behind the isolate's
		 * own sync_cmd_lock, so hold it. */
		if (nvkvm_isolate_window_info(t, isolate_id, &base, &size) == 0) {
			w->base = base;
			w->size = size;
		}
		w->probed = true;
		if (!w->base || !w->size)
			fprintf(stderr,
				"nvkvm: isolate %u reported no guest-mapping "
				"window — every mapping into it will be "
				"refused (U-9)\n", isolate_id);
	}
	uint64_t va = win_place_locked(w, gva, len, contig);
	pthread_mutex_unlock(&iso_win_lock);
	return va;
}

/* Give a placed extent back.  Safe to call with a stub_va of 0. */
static void nvkvm_win_unplace(uint32_t isolate_id, uint64_t stub_va,
			      uint64_t len)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX ||
	    !stub_va || !len)
		return;
	pthread_mutex_lock(&iso_win_lock);
	struct nvkvm_iso_window *w = iso_win[isolate_id % NVKVM_ISOLATE_MAX];
	if (w && w->base && stub_va >= w->base &&
	    stub_va - w->base < w->size)
		win_release_locked(w, stub_va - w->base,
				   (len + 4095ULL) & ~4095ULL);
	pthread_mutex_unlock(&iso_win_lock);
}

/* Forget an isolate's window entirely (isolate died / was reaped). */
static void nvkvm_win_forget_isolate(uint32_t isolate_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return;
	pthread_mutex_lock(&iso_win_lock);
	g_free(iso_win[isolate_id % NVKVM_ISOLATE_MAX]);
	iso_win[isolate_id % NVKVM_ISOLATE_MAX] = NULL;
	pthread_mutex_unlock(&iso_win_lock);
}

/*
 * ── A-1: is [base, base+len) fully covered by ranges the host itself installed
 *        into this isolate's address space? ─────────────────────────────────
 *
 * The check behind the NV01_MEMORY_SYSTEM_OS_DESCRIPTOR gate below.  Only
 * entries with `stub_mirrored` count: that flag is set exactly when QEMU
 * MAP_FIXED'd a memfd it owns into the isolate at `gva`, which is the only way
 * a legitimate OS-descriptor range comes to exist (the guest's page-migration
 * path in src/guest/nvkvm_ioctl.c:409-440 routes every such range through
 * mmap_on_isolate first).  A QEMU-side mapping with no isolate-side mirror is
 * not addressable by the stub at all and must not authorise anything.
 *
 * MULTI-ENTRY BY NECESSITY.  The migration installs the range in
 * NVKVM_MIG_CHUNK (2 MiB) pieces, one mmap_on_isolate -- and therefore one
 * table entry -- per chunk (nvkvm_mmap.c:1402-1460).  So a 16 MiB
 * cudaHostRegister is EIGHT adjacent entries, not one.  An earlier version of
 * this function demanded containment in a single entry and would have refused
 * every registration above 2 MiB; the 2 MiB probe that "verified" it was
 * exactly one chunk and sailed past the bug.
 *
 * So the walk: find the entry containing the current offset, advance to its
 * end, repeat until the whole range is spanned.  A gap anywhere fails.  This is
 * deliberately NOT "overlaps some entry" -- that would authorise the uncovered
 * remainder and reopen the hole for everything past the first chunk.  It is a
 * union only over ranges the host installed, which is the property that matters
 * here; U-6's uvm_va_covers() refuses unions because there the ranges are
 * distinct driver objects, whereas here the chunks are one registration the
 * host itself split.
 *
 * Stale entries cannot pass: iso_mmap_free() clears `used` under the table lock
 * BEFORE the isolate-side munmap runs, and iso_mmap_alloc() rewrites every
 * field on reuse, so a freed or recycled slot never reads as live mirrored
 * memory.  The reverse window -- mapped but not yet recorded -- fails closed.
 *
 * This does NOT depend on req->session_id being truthful.  The lookup is keyed
 * on the isolate that will actually run the ioctl, so a guest that forges an
 * isolate_id is checked against that same isolate; there is no ordering of
 * guest-supplied values that makes the pin land somewhere the host did not
 * install.
 */
/*
 * U-9 CHANGED THE OUTPUT, NOT THE QUESTION.  The chunks no longer live at the
 * guest's own addresses -- they live wherever the window allocator put them --
 * so covering the range is no longer enough: the caller needs the address in
 * the ISOLATE that corresponds to the guest range, and that address has to be
 * one contiguous run, because RmAllocOsDescriptor pins one range.  So the walk
 * also carries the stub-side end forward and refuses a step that is not
 * stub-adjacent to the previous one, and hands back the translated base.
 *
 * Note what this does NOT make redundant.  The window bounds WHERE a mapping
 * may be; this table records WHAT was installed, for whom, and whether it is
 * still live.  Inside the window a guest can still name another handle's
 * extent, a freed extent, or a hole between two extents -- none of which the
 * window can see.  Both checks are load-bearing; see U-9 in
 * docs/internal/audit-guest-pointers.md.
 */
static bool iso_mmap_translate(uint32_t isolate_id, uint64_t base, uint64_t len,
			       uint64_t *stub_base_out)
{
	uint64_t cur, end, stub_base = 0, stub_next = 0;
	bool ok = false, first = true;

	if (stub_base_out)
		*stub_base_out = 0;
	if (!len || base + len < base)          /* zero, or u64 wrap */
		return false;
	end = base + len;

	pthread_mutex_lock(&iso_mmap_lock);
	for (cur = base;;) {
		const struct nvkvm_iso_mmap_entry *hit = NULL;
		uint64_t next;

		for (uint32_t i = 1; i < NVKVM_ISO_MMAP_MAX; i++) {
			const struct nvkvm_iso_mmap_entry *e = &iso_mmap_tbl[i];
			uint64_t e_end;

			if (!e->used || !e->stub_mirrored)
				continue;
			if (e->isolate_id != isolate_id || !e->gva || !e->len)
				continue;
			if (!e->stub_va)                /* not window-placed */
				continue;
			e_end = e->gva + (uint64_t)e->len;
			if (e_end < e->gva)             /* entry wraps: ignore */
				continue;
			if (cur >= e->gva && cur < e_end) {
				hit = e;
				break;
			}
		}
		if (!hit)                               /* gap -- not covered */
			break;
		if (first) {
			stub_base = hit->stub_va + (cur - hit->gva);
			first     = false;
		} else if (hit->stub_va != stub_next) {
			/* Covered in the guest's address space but scattered in
			 * the isolate's -- there is no single range to pin. */
			break;
		}
		stub_next = hit->stub_va + (uint64_t)hit->len;
		next = hit->gva + (uint64_t)hit->len;
		if (next <= cur)                        /* defensive: no progress */
			break;
		if (next >= end) {
			ok = true;
			break;
		}
		cur = next;
	}
	pthread_mutex_unlock(&iso_mmap_lock);
	if (ok && stub_base_out)
		*stub_base_out = stub_base;
	return ok;
}

static uint32_t iso_mmap_alloc(uint32_t isolate_id, uint64_t gva,
				uint64_t stub_va, void *qva,
				size_t len, int kvm_slot, uint64_t gpa,
				bool stub_mirrored, uint32_t handle_id)
{
	pthread_mutex_lock(&iso_mmap_lock);
	for (uint32_t i = 0; i < NVKVM_ISO_MMAP_MAX - 1; i++) {
		uint32_t tok = iso_mmap_seq;
		iso_mmap_seq = (iso_mmap_seq % (NVKVM_ISO_MMAP_MAX - 1)) + 1;
		if (!iso_mmap_tbl[tok].used) {
			iso_mmap_tbl[tok].used          = true;
			iso_mmap_tbl[tok].stub_mirrored = stub_mirrored;
			iso_mmap_tbl[tok].isolate_id    = isolate_id;
			iso_mmap_tbl[tok].gva           = gva;
			iso_mmap_tbl[tok].stub_va       = stub_va;
			iso_mmap_tbl[tok].qva           = qva;
			iso_mmap_tbl[tok].len           = len;
			iso_mmap_tbl[tok].kvm_slot      = kvm_slot;
			iso_mmap_tbl[tok].gpa           = gpa;
			iso_mmap_tbl[tok].handle_id     = handle_id;
			pthread_mutex_unlock(&iso_mmap_lock);
			return tok;
		}
	}
	pthread_mutex_unlock(&iso_mmap_lock);
	return 0; /* table full */
}

/*
 * S-2 (cross-isolate): detach a token, but ONLY for the isolate that owns it.
 *
 * iso_mmap_tbl is one VM-global array and the token is a bare index into it, so
 * "the caller named a live token" says nothing about whose mapping it is.  The
 * ownership test belongs here, under the same lock as the detach: doing it in
 * the caller would leave a window where two isolates race to free one entry.
 * owner_isolate_id == 0 is never a valid owner — callers must name themselves.
 */
static bool iso_mmap_free(uint32_t token, uint32_t owner_isolate_id,
			  struct nvkvm_iso_mmap_entry *out)
{
	if (token == 0 || token >= NVKVM_ISO_MMAP_MAX || owner_isolate_id == 0)
		return false;
	pthread_mutex_lock(&iso_mmap_lock);
	if (!iso_mmap_tbl[token].used ||
	    iso_mmap_tbl[token].isolate_id != owner_isolate_id) {
		pthread_mutex_unlock(&iso_mmap_lock);
		return false;
	}
	*out = iso_mmap_tbl[token];
	iso_mmap_tbl[token].used = false;
	pthread_mutex_unlock(&iso_mmap_lock);
	return true;
}

/* #80 (audit H-3/M-E): reclaim a killed isolate's still-mapped iso_mmap_tbl
 * entries (defined below, after the munmap helper it mirrors). */
static int nvkvm_iso_mmap_reap_isolate(VirtIONvgpu *nv, uint32_t isolate_id);

/* U-6 (audit-guest-pointers): forget every UVM VA range recorded for a handle.
 * Defined with the UVM schema below; declared here for the close handlers. */
void nvkvm_uvm_va_purge_handle(uint32_t handle_id);

/* Retire a dead isolate's RM_MAP_MEMORY VA records.  Defined with the mapva
 * table far below, but CALLED from nvkvm_req_kill_isolate() far above it, so
 * the declaration has to be here: the one next to the definition is below its
 * only caller, which is an implicit declaration -- a hard error since GCC 14. */
void nvkvm_mapva_forget_isolate(uint32_t iso);

/* ── Device enumeration ──────────────────────────────────────────────────── */

int nvkvm_req_list_nvidia_devices(VirtIONvgpu *nv,
				   struct nvkvm_req_list_nvidia_devices *req,
				   struct nvkvm_resp_list_nvidia_devices *resp)
{
	(void)nv;
	(void)req;

	memset(resp, 0, sizeof(*resp));

	/* Always include nvidiactl and nvidia-uvm */
	int n = 0;

	if (access("/dev/nvidiactl", F_OK) == 0) {
		resp->devices[n].dev_id = NVKVM_DEV_CTL;
		n++;
	}
	if (access("/dev/nvidia-uvm", F_OK) == 0) {
		resp->devices[n].dev_id = NVKVM_DEV_UVM;
		n++;
	}

	/* Scan /dev/nvidia0..15 */
	for (int i = 0; i < 16 && n < NVKVM_MAX_DEVICES; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/nvidia%d", i);
		if (access(path, F_OK) == 0) {
			resp->devices[n].dev_id = NVKVM_DEV_GPU(i);
			n++;
		}
	}

	resp->ndevices = (uint32_t)n;
	resp->status   = 0;
	return 0;
}

/* ── Handle open ─────────────────────────────────────────────────────────── */

/*
 * Look up the (first) isolate for a session. Sessions may eventually carry
 * multiple isolates (post-fork); Step 6 handles that lazily — for now the
 * guest opens one isolate per session before any /dev/nvidia* open and the
 * first slot is the active one.
 */
static uint32_t session_first_isolate(VirtIONvgpu *nv, uint32_t session_id)
{
	uint32_t iso_id = 0;
	pthread_mutex_lock(&nv->sessions_lock);
	struct nvkvm_session *s = nvkvm_session_find(nv, session_id);
	if (s) {
		pthread_mutex_lock(&s->lock);
		if (s->nisolates > 0)
			iso_id = s->isolate_ids[0];
		pthread_mutex_unlock(&s->lock);
	}
	pthread_mutex_unlock(&nv->sessions_lock);
	return iso_id;
}

/*
 * Does `session_id` actually own `isolate_id`?  The guest names an (isolate,
 * handle) PAIR in XISO_IMPORT, and those are two independent assertions — the
 * boundary must not take the pairing on faith just because each half is
 * individually well-formed.  Sessions record their isolates, and handles record
 * their session, so QEMU can check the guest's claim against its own bookkeeping
 * rather than relying on the target stub's handle_lookup to fail with -EBADF
 * (which it does, but that is the stub catching what the boundary should have).
 */
static bool session_has_isolate(VirtIONvgpu *nv, uint32_t session_id,
				uint32_t isolate_id)
{
	bool found = false;
	pthread_mutex_lock(&nv->sessions_lock);
	struct nvkvm_session *s = nvkvm_session_find(nv, session_id);
	if (s) {
		pthread_mutex_lock(&s->lock);
		for (int i = 0; i < s->nisolates; i++) {
			if (s->isolate_ids[i] == isolate_id) {
				found = true;
				break;
			}
		}
		pthread_mutex_unlock(&s->lock);
	}
	pthread_mutex_unlock(&nv->sessions_lock);
	return found;
}

int nvkvm_req_open_nvidia_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_open_nvidia_handle *req,
				  struct nvkvm_resp_open_nvidia_handle *resp)
{
	uint32_t handle_id = 0;
	int ret;

	/*
	 * Graphics gate (compute-only VMs): refuse to open the DRM render node
	 * or the NVKMS modeset device when graphics is disabled. This is the
	 * authoritative enforcement — the stub only ever opens devices QEMU
	 * grants a handle for, so a guest that ignores the cleared config bit
	 * still cannot reach them.
	 */
	if (!nv->graphics &&
	    ((int)req->dev_id == NVKVM_DEV_MODESET ||
	     ((int)req->dev_id >= NVKVM_DEV_DRM_RD(0) &&
	      (int)req->dev_id < NVKVM_DEV_DRM_RD(16)))) {
		resp->handle_id = 0;
		resp->status    = EPERM;
		return 0;
	}

	/*
	 * UVM stays opened in QEMU (driver enforces opener-does-mmap, and
	 * mmap is done in QEMU for KVM region installation). The other
	 * devices — /dev/nvidiactl, /dev/nvidia0..N, and the eventfd that
	 * stands in for the guest's libcuda eventfd — open inside the
	 * isolate so nvfp/mm lineage matches the process that runs RM
	 * ioctls. See docs/REFACTOR_PLAN.md §1 open-ownership table.
	 */
	if ((int)req->dev_id == NVKVM_DEV_UVM) {
		ret = nvkvm_handle_open_nvidia(&nv->handles,
					       req->session_id,
					       (int)req->dev_id,
					       (int)req->flags,
					       &handle_id);
		if (ret < 0)
			goto out;
		/*
		 * Stub swaps the SCM_RIGHTS-received UVM fd for one of its
		 * own pre-opened local UVM fds (file-owner-mm match for
		 * UVM_MM_INITIALIZE). We still need to send a RECEIVE_FD so
		 * the stub knows about the handle_id → local-fd mapping.
		 * If the session has no isolate yet, this is deferred until
		 * the guest creates one and re-issues COPY_HANDLE_TO_ISOLATE
		 * (legacy compat — Step 3d removes that fallback).
		 */
		{
			uint32_t iso = session_first_isolate(nv, req->session_id);
			if (iso != 0)
				nvkvm_isolate_send_handle(&nv->isolates,
							   &nv->handles,
							   iso, handle_id);
		}
		goto out;
	}

	uint32_t iso_id = session_first_isolate(nv, req->session_id);
	if (iso_id == 0) {
		/*
		 * No isolate yet. Guest must call CREATE_ISOLATE before the
		 * first non-UVM open. Returned to the guest so it can either
		 * reorder or fail the open syscall.
		 */
		ret = -ENOENT;
		goto out;
	}

	ret = nvkvm_handle_alloc_pending(&nv->handles, req->session_id,
					 (int)req->dev_id, &handle_id);
	if (ret < 0)
		goto out;

	int fd_from_scm = -1;
	ret = nvkvm_isolate_open_device(&nv->isolates, iso_id, handle_id,
					req->dev_id, req->flags,
					&fd_from_scm);
	if (ret < 0) {
		nvkvm_handle_abort_open(&nv->handles, handle_id);
		handle_id = 0;
		goto out;
	}

	ret = nvkvm_handle_attach_fd(&nv->handles, handle_id, fd_from_scm);
	if (ret < 0) {
		/* Shouldn't happen on a fresh slot; clean up if it does. */
		close(fd_from_scm);
		nvkvm_handle_abort_open(&nv->handles, handle_id);
		handle_id = 0;
		goto out;
	}

	/*
	 * Bump the isolate refcount to mirror what nvkvm_isolate_send_handle
	 * did in the legacy COPY_HANDLE_TO_ISOLATE flow: the stub now holds
	 * one copy of this fd (the original); QEMU holds the SCM_RIGHTS copy
	 * as qemu_fd. Close-handle must refuse until the isolate releases.
	 */
	nvkvm_handle_ref_isolate(&nv->handles, handle_id);
	ret = 0;

out:
	if (ret < 0) {
		resp->handle_id = 0;
		resp->status    = (uint32_t)-ret;
	} else {
		resp->handle_id = handle_id;
		resp->status    = 0;
	}
	return 0;
}

int nvkvm_req_open_memory_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_open_memory_handle *req,
				  struct nvkvm_resp_open_memory_handle *resp)
{
	uint32_t handle_id = 0;
	int ret = nvkvm_handle_open_memory(&nv->handles,
					   req->session_id,
					   req->size,
					   &handle_id);
	if (ret < 0) {
		resp->handle_id = 0;
		resp->status    = (uint32_t)-ret;
	} else {
		resp->handle_id = handle_id;
		resp->status    = 0;
	}
	return 0;
}

int nvkvm_req_close_handle(VirtIONvgpu *nv,
			    struct nvkvm_req_close_handle *req,
			    struct nvkvm_resp_close_handle *resp)
{
	/* U-6: the va_space dies with the fd — drop its VA-range ownership
	 * records so a recycled handle_id cannot inherit them. */
	nvkvm_uvm_va_purge_handle(req->handle_id);
	int ret = nvkvm_handle_close(&nv->handles, req->handle_id);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

/* ── Isolate lifecycle ───────────────────────────────────────────────────── */

int nvkvm_req_create_isolate(VirtIONvgpu *nv,
			      struct nvkvm_req_create_isolate *req,
			      struct nvkvm_resp_create_isolate *resp)
{
	uint32_t isolate_id = 0;
	int ret = nvkvm_isolate_create(&nv->isolates, req->session_id, nv, &isolate_id);
	if (ret < 0) {
		resp->isolate_id = 0;
		resp->status     = (uint32_t)-ret;
		return 0;
	}

	/*
	 * Find-or-create the QEMU-side session. The legacy NVKVM_REQ_OPEN
	 * used to create it as a side effect of the first device open; in
	 * the new flow CREATE_ISOLATE is the first request the guest sends
	 * for a fresh session, so we own the creation here.
	 */
	pthread_mutex_lock(&nv->sessions_lock);
	struct nvkvm_session *session = nvkvm_session_find(nv, req->session_id);
	pthread_mutex_unlock(&nv->sessions_lock);
	if (!session)
		session = nvkvm_session_create(nv, req->session_id);

	if (session) {
		pthread_mutex_lock(&session->lock);
		if (session->nisolates < 256)
			session->isolate_ids[session->nisolates++] = isolate_id;
		pthread_mutex_unlock(&session->lock);
	}

	resp->isolate_id = isolate_id;
	resp->status     = 0;
	return 0;
}

int nvkvm_req_kill_isolate(VirtIONvgpu *nv,
			    struct nvkvm_req_kill_isolate *req,
			    struct nvkvm_resp_kill_isolate *resp)
{
	int ret = nvkvm_isolate_kill(&nv->isolates, req->isolate_id);

	/*
	 * #80 (audit H-3/M-E): the isolate is now drained and dead.  Reclaim any
	 * GPU mappings it still held — the guest may have killed it (or gone
	 * silent) without sending MUNMAP_ON_ISOLATE, which previously leaked the
	 * GPA window space, KVM slots and iso_mmap_tbl entries irrecoverably.
	 */
	nvkvm_iso_mmap_reap_isolate(nv, req->isolate_id);

	/*
	 * S-4: drop this isolate's entries from the console's imported-buffer
	 * cache.  Nothing used to invalidate on isolate death, so a dead
	 * compositor's EGLImages kept its dma-buf (and the VRAM behind it)
	 * pinned for the life of the VM — and, because stub GEM ids restart at 1
	 * in every isolate, the NEXT compositor's bo 1 hit the dead one's cached
	 * import and displayed its pixels.
	 */
	nvkvm_present_forget_isolate(nv, req->isolate_id);
	/* Retire this isolate's MAP->VA entries too, or the table fills up
	 * with dead mappings over a long-lived VM's process churn. */
	nvkvm_mapva_forget_isolate(req->isolate_id);

	/*
	 * Walk every session and prune the killed isolate from its
	 * isolate_ids[] list. Without this, session_first_isolate
	 * later returns a stale (dead) isolate_id and the OPEN_DEVICE
	 * round-trip fails — the session can outlive its isolate in
	 * the test-cycle case (session_id is reused after the guest
	 * idr_remove + new alloc lands the same id).
	 *
	 * #80 (audit H-2/H-3): collect sessions whose LAST isolate just died so
	 * we can destroy them (close host fds + free RM objects + the struct)
	 * after dropping sessions_lock — nvkvm_session_destroy re-takes it.
	 */
	struct nvkvm_session *to_destroy[16];
	int n_destroy = 0;
	pthread_mutex_lock(&nv->sessions_lock);
	struct nvkvm_session *s;
	TAILQ_FOREACH(s, &nv->sessions, link) {
		pthread_mutex_lock(&s->lock);
		int dst = 0;
		for (int i = 0; i < s->nisolates; i++) {
			if (s->isolate_ids[i] != req->isolate_id) {
				s->isolate_ids[dst++] = s->isolate_ids[i];
			}
		}
		bool became_empty = (dst == 0 && s->nisolates > 0);
		s->nisolates = dst;
		pthread_mutex_unlock(&s->lock);
		if (became_empty && n_destroy < 16)
			to_destroy[n_destroy++] = s;
	}
	pthread_mutex_unlock(&nv->sessions_lock);

	for (int i = 0; i < n_destroy; i++)
		nvkvm_session_destroy(nv, to_destroy[i]);

	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

/*
 * NVKVM_REQ_INTERRUPT — a guest task blocked on a forwarded ioctl received a
 * signal.  Route a best-effort interrupt to the named isolate's worker.
 *
 * Access model: isolate_id is QEMU-managed and VM-scoped; a guest can only
 * name isolates this device created.  target_txn is the guest's own in-flight
 * ioctl — interrupting it is purely an intra-VM concern, so no cross-VM check
 * is needed (the guest kernel owns intra-VM policy).  We simply forward and
 * report whether the isolate was live.
 */
int nvkvm_req_interrupt(VirtIONvgpu *nv,
			struct nvkvm_req_interrupt *req,
			struct nvkvm_resp_interrupt *resp)
{
	int ret = nvkvm_isolate_interrupt(&nv->isolates,
					  req->isolate_id, req->target_txn);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

/* ── Command-buffer ring ─────────────────────────────────────────────────── */

int nvkvm_req_setup_ring(VirtIONvgpu *nv,
			 struct nvkvm_req_setup_ring *req,
			 struct nvkvm_resp_setup_ring *resp)
{
	uint32_t iso_id = session_first_isolate(nv, req->session_id);
	if (iso_id == 0) {
		resp->status = ENODEV;   /* no isolate yet → guest uses virtqueue */
		return 0;
	}
	uint64_t gpa = 0;
	uint32_t region = 0, resp_off = 0, ring_bytes = 0;
	int ret = nvkvm_isolate_ring_info(&nv->isolates, iso_id,
					  &gpa, &region, &resp_off, &ring_bytes);
	if (ret < 0 || gpa == 0) {
		resp->status = (ret < 0) ? (uint32_t)-ret : ENODEV;
		return 0;
	}
	resp->ring_gpa     = gpa;
	resp->region_size  = region;
	resp->req_off      = 0;
	resp->resp_off     = resp_off;
	resp->ring_bytes   = ring_bytes;
	resp->status       = 0;
	return 0;
}

int nvkvm_req_enter_loop(VirtIONvgpu *nv,
			 struct nvkvm_req_enter_loop *req,
			 struct nvkvm_resp_enter_loop *resp)
{
	uint32_t iso_id = session_first_isolate(nv, req->session_id);
	if (iso_id == 0) {
		resp->status = ENODEV;
		return 0;
	}
	uint64_t head = 0;
	int ret = nvkvm_isolate_enter_loop(&nv->isolates, iso_id,
					   req->idle_us, &head);
	resp->head   = head;
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

/* ── Handle distribution ────────────────────────────────────────────────── */

int nvkvm_req_copy_handle_to_isolate(VirtIONvgpu *nv,
				      struct nvkvm_req_copy_handle_to_isolate *req,
				      struct nvkvm_resp_copy_handle_to_isolate *resp)
{
	int ret = nvkvm_isolate_send_handle(&nv->isolates, &nv->handles,
					    req->isolate_id, req->handle_id);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

/* Defined below, next to the other iso_mmap reapers. */
static int nvkvm_iso_mmap_reap_handle(VirtIONvgpu *nv, uint32_t isolate_id,
				      uint32_t handle_id);

int nvkvm_req_close_handle_on_isolate(VirtIONvgpu *nv,
				       struct nvkvm_req_close_handle_on_isolate *req,
				       struct nvkvm_resp_close_handle_on_isolate *resp)
{
	/* U-6: see nvkvm_req_close_handle. */
	nvkvm_uvm_va_purge_handle(req->handle_id);
	/* Drop any window extent this handle still owns before the fd goes, so
	 * the guest cannot keep writing into memory the driver has recycled. */
	nvkvm_iso_mmap_reap_handle(nv, req->isolate_id, req->handle_id);
	int ret = nvkvm_isolate_close_handle(&nv->isolates, &nv->handles,
					     req->isolate_id, req->handle_id);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

/* ── Ioctl on isolate ────────────────────────────────────────────────────── */

/* ── Phase 3: UVM ioctl field schema + default-deny ──────────────────────────
 *
 * UVM ioctls execute in QEMU's (privileged) process, so we MUST NOT blindly
 * forward an arbitrary cmd the guest names.  This table is the allowlist: each
 * UVM cmd we will forward is listed with (a) a minimum param_size and (b) the
 * offsets of any embedded *frontend fd* fields (RMCtrlFD / UvmFD) that the
 * guest sanitizer rewrote into a handle_id and that the kernel will dereference
 * as an fd — those are translated to QEMU's local fd and restored on response.
 *
 * Field offsets/sizes are taken from the open kernel module / gVisor nvproxy
 * (pkg/abi/nvgpu/uvm.go): NvUUID is 16 bytes, Handle/NvU32 is 4 bytes, the
 * frontend fd is an NvS32 at a fixed offset in the struct header (before any
 * variable-length PerGPUAttributes array, so the offset is version-stable).
 *
 * DEFAULT-DENY: any UVM cmd absent from this table is refused, never forwarded.
 * Notably this denies UVM_TOOLS_READ/WRITE_PROCESS_MEMORY (62/63) — a
 * cross-process memory peek/poke primitive that has no place in our model.
 * min_size uses the base (pre-V550) struct size as a conservative floor; the
 * fd-field read is additionally guarded against the actual param_size.
 */
enum { NVKVM_UVM_FD_FIELD = 1 };

/*
 * ── U-6 (docs/internal/audit-guest-pointers.md): guest-supplied UVM VA ranges
 *
 * 15 of the rows below carry a (base, length) or (requestedBase, length) pair.
 * These are virtual addresses in the CALLING TASK'S mm — and the calling task
 * is QEMU (see the UVM branch of nvkvm_req_ioctl_on_isolate: UVM ioctls run in
 * QEMU's own process, not the isolate).  Nothing used to look at them.
 *
 * What an attacker could otherwise do: name any address in QEMU's address
 * space — the process that holds the KVM fd, every memslot, every isolate's
 * socket and the per-VM handle table.  The sharpest case is UVM_MIGRATE: when
 * UVM finds NO va_range covering the named range it falls through to the
 * pageable-memory path and migrates the CALLER'S OWN anonymous pages (i.e.
 * QEMU's heap, the 128 GiB sparse window that backs guest RAM-visible GPU
 * mappings), and UVM_MIGRATE_PARAMS.semaphoreAddress is an address the driver
 * WRITES semaphorePayload to on async completion.  This is the one finding in
 * that audit the Phase 0 isolate does not contain.
 *
 * The control: a host-side ownership table.  A UVM VA range is only usable if
 * it is one nvkvm itself established for THAT UVM handle — recorded when a
 * range-CREATING command (73 CREATE_EXTERNAL_RANGE, 68 ALLOC_SEMAPHORE_POOL,
 * 65 MAP_DYNAMIC_PARALLELISM_REGION, 27 REGISTER_CHANNEL) is accepted BY THE
 * DRIVER.  Every range-USING command must be fully contained in a recorded
 * range for the same handle; anything unresolvable is rejected with
 * NV_ERR_INVALID_ADDRESS and never reaches the driver.  Default-deny: a row
 * with no va_mode gets no VA treatment, and a cmd that is not in the table at
 * all was already refused above.
 *
 * Per-HANDLE, deliberately: each guest UVM fd gets its own QEMU-side
 * /dev/nvidia-uvm fd and therefore its own uvm_va_space, and two guest
 * processes legitimately pick the SAME base (measured: isolates 8 and 10 both
 * create 0x200000000).  A per-VM or process-wide VA reservation would break
 * that; range ownership is only meaningful inside one va_space.
 */
enum {
	NVKVM_UVM_VA_NONE   = 0, /* no guest VA range in this struct        */
	NVKVM_UVM_VA_CREATE = 1, /* establishes a range; record on success  */
	NVKVM_UVM_VA_USE    = 2, /* must be contained in a recorded range   */
	NVKVM_UVM_VA_FREE   = 3, /* must be contained; drops it on success  */
};

struct nvkvm_uvm_desc {
	uint32_t cmd;
	uint16_t min_size;
	uint16_t fd_off[2];   /* frontend-fd field byte offsets; 0xffff = none */
	uint16_t va_off;      /* byte offset of the u64 base (length at +8);
			       * 0xffff = this cmd carries no VA range      */
	uint8_t  va_mode;     /* NVKVM_UVM_VA_*                             */
};
/*
 * min_size is the EXACT struct size from our ABI (src/abi/uvm.h), verified
 * against OGKM rather than copied from a neighbouring nvproxy release.
 * REGISTER_GPU is 40 bytes at all 216 published OGKM tags, 515.43.04 through
 * 610.57.04 -- compiled and run at each tag on 2026-08-26, one layout with no
 * clone failure and no unmeasured cell; the committed evidence is
 * tests/abi_parity/ogkm_register_gpu.tsv.  The old 32-byte claim truncated
 * rmCtrlFd/hClient/hSmcPartRef and read rmCtrlFd as rmStatus.  Other ioctls
 * still differ from current gVisor layouts (REGISTER_CHANNEL 48 not 56;
 * MIGRATE 48 not 56).  The guest always sends
 * exactly this size, so "param_size < min_size" rejects only malformed calls.
 * fd_off lists every frontend fd the trusted path must resolve.  In particular
 * REGISTER_GPU.rmCtrlFd@24 is an input fd, not an output status word.
 */
static const struct nvkvm_uvm_desc nvkvm_uvm_schema[] = {
	/* The full UVM command set (open kernel module / gVisor nvproxy
	 * uvm.go).  min_size: cmds whose struct is defined in our ABI
	 * (src/abi/uvm.h) carry the exact sizeof.  REGISTER_GPU's was measured
	 * at all 216 published OGKM tags (see above); the rest are pinned to
	 * driver 575.51.03 and verified by sizeof there, NOT copied from
	 * gVisor's newer layouts — these are the layouts the guest actually
	 * sends, so "param_size < min" rejects only malformed calls.  The five cmds NOT
	 * in our ABI (44/45/53/65/66) carry min_size 0 (allow any size): we
	 * have no driver-verified layout for them and an over-strict guess
	 * already mis-denied REGISTER_GPU once; the kernel validates its own
	 * struct against the fixed shm slot regardless.  fd_off covers every
	 * embedded frontend fd whose layout has been verified against OGKM. */
	{ 0x30000001 /* UVM_INITIALIZE          */,  16, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 0x30000002 /* UVM_DEINITIALIZE        */,   8, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 23 /* UVM_CREATE_RANGE_GROUP          */,  16, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 24 /* UVM_DESTROY_RANGE_GROUP         */,  16, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 25 /* UVM_REGISTER_GPU_VASPACE        */,  32, { 16, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 26 /* UVM_UNREGISTER_GPU_VASPACE      */,  20, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 27 /* UVM_REGISTER_CHANNEL            */,  48, { 0xffff, 0xffff }, 32, NVKVM_UVM_VA_CREATE },
	{ 28 /* UVM_UNREGISTER_CHANNEL          */,  28, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 29 /* UVM_ENABLE_PEER_ACCESS          */,  40, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 30 /* UVM_DISABLE_PEER_ACCESS         */,  40, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 31 /* UVM_SET_RANGE_GROUP             */,  32, { 0xffff, 0xffff }, 8, NVKVM_UVM_VA_USE },
	{ 33 /* UVM_MAP_EXTERNAL_ALLOCATION     */, 9264, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 34 /* UVM_FREE                        */,  24, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_FREE },
	{ 37 /* UVM_REGISTER_GPU                */, NVKVM_UVM_REGISTER_GPU_SIZE,
		{ NVKVM_UVM_REGISTER_GPU_FD_OFF, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 38 /* UVM_UNREGISTER_GPU              */,  24, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 39 /* UVM_PAGEABLE_MEM_ACCESS         */,   8, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 42 /* UVM_SET_PREFERRED_LOCATION      */,  40, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 43 /* UVM_UNSET_PREFERRED_LOCATION    */,  24, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 44 /* UVM_ENABLE_READ_DUPLICATION     */,   0, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 45 /* UVM_DISABLE_READ_DUPLICATION    */,   0, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 46 /* UVM_SET_ACCESSED_BY             */,  40, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 47 /* UVM_UNSET_ACCESSED_BY           */,  40, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 51 /* UVM_MIGRATE                     */,  48, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 53 /* UVM_MIGRATE_RANGE_GROUP         */,   0, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 65 /* UVM_MAP_DYNAMIC_PARALLELISM_REGION */, 0, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_CREATE },
	{ 66 /* UVM_UNMAP_EXTERNAL              */,   0, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 68 /* UVM_ALLOC_SEMAPHORE_POOL        */, 9248, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_CREATE },
	{ 70 /* UVM_PAGEABLE_MEM_ACCESS_ON_GPU  */,  24, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 72 /* UVM_VALIDATE_VA_RANGE           */,  24, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 73 /* UVM_CREATE_EXTERNAL_RANGE       */,  24, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_CREATE },
	{ 75 /* UVM_MM_INITIALIZE               */,   8, { 0, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	/* Default-denied by omission: UVM_TOOLS_READ_PROCESS_MEMORY (62) and
	 * UVM_TOOLS_WRITE_PROCESS_MEMORY (63) — a cross-process memory
	 * peek/poke primitive with no place in our isolation model — plus any
	 * unknown/garbage cmd a malicious guest might name. */
};

static const struct nvkvm_uvm_desc *nvkvm_uvm_lookup(uint32_t cmd)
{
	for (size_t i = 0;
	     i < sizeof(nvkvm_uvm_schema) / sizeof(nvkvm_uvm_schema[0]); i++) {
		if (nvkvm_uvm_schema[i].cmd == cmd)
			return &nvkvm_uvm_schema[i];
	}
	return NULL;
}

/* ── U-6: the UVM VA-range ownership table ──────────────────────────────────
 *
 * One flat array, keyed by the UVM handle_id (== one QEMU-side /dev/nvidia-uvm
 * fd == one uvm_va_space).  Entries are added ONLY after the driver itself has
 * accepted a range-creating ioctl, so the table can never contain a range the
 * driver would have refused; it is a *narrowing* of the driver's own state,
 * never a widening.  Bounded and fail-closed: if the table is full a CREATE is
 * refused rather than silently untracked.
 */
#define NVKVM_UVM_VA_MAX 16384

/*
 * U-6 — floor for the bounce buffer the UVM ioctl actually runs on.  Must be
 * >= the largest UVM_*_PARAMS the driver copies: 9264 bytes
 * (UVM_MAP_EXTERNAL_ALLOCATION_PARAMS, V550 256-entry perGpuAttributes).
 * 16 KiB leaves headroom for a future driver growing a struct, on a path that
 * runs a few hundred times per process.
 */
#define NVKVM_UVM_BOUNCE_MIN 16384u

struct nvkvm_uvm_va_ent {
	uint32_t handle_id;   /* 0 = free slot */
	uint64_t base;
	uint64_t length;
};

static struct nvkvm_uvm_va_ent uvm_va_tbl[NVKVM_UVM_VA_MAX];
static uint32_t                uvm_va_used;
static pthread_mutex_t         uvm_va_lock = PTHREAD_MUTEX_INITIALIZER;

/* [base, base+length) with no wrap.  length == 0 is never a valid range. */
static bool uvm_va_sane(uint64_t base, uint64_t length)
{
	if (length == 0)
		return false;
	if (base + length < base)          /* u64 overflow */
		return false;
	if ((base | length) & 0xfffULL)    /* UVM works in pages */
		return false;
	return true;
}

/* Is there room to record one more range?  Checked BEFORE a range-creating
 * ioctl runs, so we never leave the driver holding a range the table cannot
 * describe (which would fail closed on every later use of it). */
static bool uvm_va_have_room(void)
{
	bool room;
	pthread_mutex_lock(&uvm_va_lock);
	room = uvm_va_used < NVKVM_UVM_VA_MAX;
	pthread_mutex_unlock(&uvm_va_lock);
	return room;
}

static bool uvm_va_add(uint32_t handle_id, uint64_t base, uint64_t length)
{
	bool ok = false;
	pthread_mutex_lock(&uvm_va_lock);
	/* Idempotent: the guest may legitimately re-create an identical range
	 * after freeing it, and the driver arbitrates that. */
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id == handle_id &&
		    uvm_va_tbl[i].base == base &&
		    uvm_va_tbl[i].length == length) {
			pthread_mutex_unlock(&uvm_va_lock);
			return true;
		}
	}
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id == 0) {
			uvm_va_tbl[i].handle_id = handle_id;
			uvm_va_tbl[i].base      = base;
			uvm_va_tbl[i].length    = length;
			uvm_va_used++;
			ok = true;
			break;
		}
	}
	pthread_mutex_unlock(&uvm_va_lock);
	return ok;
}

/* True iff [base, base+length) is fully inside ONE range recorded for this
 * handle.  Deliberately not a union-of-ranges test: UVM operations act on a
 * single va_range, and stitching adjacent entries together would let a guest
 * address across a boundary it never actually owns as one object. */
static bool uvm_va_covers(uint32_t handle_id, uint64_t base, uint64_t length)
{
	bool found = false;
	pthread_mutex_lock(&uvm_va_lock);
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id != handle_id)
			continue;
		if (base >= uvm_va_tbl[i].base &&
		    base + length <= uvm_va_tbl[i].base + uvm_va_tbl[i].length) {
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&uvm_va_lock);
	return found;
}

/*
 * Length of the range this handle recorded starting EXACTLY at `base`, or 0 if
 * it has no such range.
 *
 * UVM_FREE names a range by its base and leaves length 0 -- the driver looks
 * the range up by start address -- so there is no length in the ioctl to
 * validate against.  Recovering our own recorded length lets the ownership
 * check and the drop below run unchanged, and keeps the U-6 property: a base
 * we never recorded for this handle returns 0 and is refused exactly as before.
 */
static uint64_t uvm_va_len_at(uint32_t handle_id, uint64_t base)
{
	uint64_t len = 0;
	pthread_mutex_lock(&uvm_va_lock);
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id == handle_id &&
		    uvm_va_tbl[i].base == base) {
			len = uvm_va_tbl[i].length;
			break;
		}
	}
	pthread_mutex_unlock(&uvm_va_lock);
	return len;
}

/* Drop every entry of this handle fully inside the freed range. */
static void uvm_va_drop(uint32_t handle_id, uint64_t base, uint64_t length)
{
	pthread_mutex_lock(&uvm_va_lock);
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id != handle_id)
			continue;
		if (uvm_va_tbl[i].base >= base &&
		    uvm_va_tbl[i].base + uvm_va_tbl[i].length <= base + length) {
			uvm_va_tbl[i].handle_id = 0;
			if (uvm_va_used)
				uvm_va_used--;
		}
	}
	pthread_mutex_unlock(&uvm_va_lock);
}

/* Forget everything about a handle: its va_space is gone (UVM_INITIALIZE on a
 * recycled handle_id, UVM_DEINITIALIZE, or the fd being closed).  Also called
 * from the close-handle handlers so a long-lived VM cannot leak the table. */
void nvkvm_uvm_va_purge_handle(uint32_t handle_id)
{
	if (handle_id == 0)
		return;
	pthread_mutex_lock(&uvm_va_lock);
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id == handle_id) {
			uvm_va_tbl[i].handle_id = 0;
			if (uvm_va_used)
				uvm_va_used--;
		}
	}
	pthread_mutex_unlock(&uvm_va_lock);
}

/* ── Phase 4: per-VM RM client-handle allowlist ─────────────────────────────
 * Record every hClient this VM's isolates successfully use, and vet foreign
 * hClient references (e.g. DUP_OBJECT h_client_src) against the set.  See the
 * VirtIONvgpu.client_allow comment for why fds are not the boundary. */
static void nvkvm_client_allow_add(VirtIONvgpu *nv, uint32_t hc)
{
	if (hc == 0 || hc == (uint32_t)-1)
		return;
	pthread_mutex_lock(&nv->client_allow_lock);
	for (uint32_t i = 0; i < nv->client_allow_n; i++) {
		if (nv->client_allow[i] == hc) {
			pthread_mutex_unlock(&nv->client_allow_lock);
			return;
		}
	}
	if (nv->client_allow_n < NVKVM_CLIENT_ALLOWLIST_MAX)
		nv->client_allow[nv->client_allow_n++] = hc;
	pthread_mutex_unlock(&nv->client_allow_lock);
}

/*
 * #76 — is this RM control command allowed?  Default-deny (nvproxy parity).
 * The gate now lives in nvkvm_ctrl_allowlist.h next to its table, because the
 * isolate stub applies the same one on the ring path (U-1).
 */

/* #76b — frontend-ioctl NR allowlist (nvproxy parity, default-deny). */
static bool nvkvm_fe_nr_allowed(unsigned nr)
{
	for (size_t i = 0; i < NVKVM_FE_NR_ALLOWLIST_N; i++)
		if (nvkvm_fe_nr_allowlist[i] == nr)
			return true;
	return false;
}

/* #76b — RM_ALLOC class allowlist (nvproxy parity, default-deny). */
static bool nvkvm_alloc_class_allowed(uint32_t cls)
{
	for (size_t i = 0; i < NVKVM_ALLOC_CLASS_ALLOWLIST_N; i++)
		if (nvkvm_alloc_class_allowlist[i] == cls)
			return true;
	return false;
}

static bool nvkvm_client_allow_has(VirtIONvgpu *nv, uint32_t hc)
{
	bool found = false;
	pthread_mutex_lock(&nv->client_allow_lock);
	for (uint32_t i = 0; i < nv->client_allow_n; i++) {
		if (nv->client_allow[i] == hc) {
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&nv->client_allow_lock);
	return found;
}

/* NV2080_CTRL_GPU_PID_INFO is 72 bytes (pid@0, index@4, result@8, data@16 =
 * NV2080_CTRL_GPU_PID_INFO_VIDEO_MEMORY_USAGE_DATA[6×NvU64], smcSubscription@64);
 * pidInfoList[] starts at +8 in the params struct.  Verified via sizeof on the
 * 575 open-driver SDK headers. */
#define NVKVM_PIDINFO_STRIDE 72u

/* ── #66: per-process VRAM via QEMU's own init-ns admin subdevice ─────────────
 *
 * The stub services GET_PID_INFO from inside CLONE_NEWPID/NEWUSER, where the
 * driver attributes 0 bytes (caller-context). QEMU runs in the host init ns,
 * where GET_PID_INFO returns the real per-process VRAM (proven: host nvidia-smi
 * uses exactly this). So we keep a small admin RM client+device+subdevice in
 * QEMU's process and answer GET_PID_INFO from there, querying the validated
 * isolate's own host tids (never an arbitrary guest-named pid).
 */
#define NVADM_IOWR(nr, sz) \
	((unsigned long)(0xc0000000UL | ((unsigned long)(sz) << 16) | \
			 (0x46UL << 8) | (unsigned long)(nr)))
#define NV2080_CTRL_CMD_GPU_GET_PID_INFO 0x2080018eU

static int admin_rm_alloc(int fd, uint32_t h_root, uint32_t h_parent,
			  uint32_t h_new, uint32_t h_class, void *parms,
			  uint32_t *out)
{
	struct nvos21_parameters a = {
		.h_root = h_root, .h_object_parent = h_parent,
		.h_object_new = h_new, .h_class = h_class,
		.p_alloc_parms = (nvp64_t)(uintptr_t)parms,
	};
	if (ioctl(fd, NVADM_IOWR(NV_ESC_RM_ALLOC, sizeof a), &a) < 0)
		return -errno;
	if (a.status != 0)
		return -1;
	*out = a.h_object_new;
	return 0;
}

/* Lazily build QEMU's admin client→device→subdevice on GPU0.  admin_lock held. */
static int nvkvm_admin_ensure(VirtIONvgpu *nv)
{
	if (nv->admin_state != 0)
		return nv->admin_state == 1 ? 0 : -1;

	int ctl = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
	int gpu = open("/dev/nvidia0",  O_RDWR | O_CLOEXEC);
	uint32_t client = 0, dev = 0, sub = 0;
	struct nv0080_alloc_parameters dp = { .device_id = 0 };
	struct nv2080_alloc_parameters sp = { .sub_device_id = 0 };

	if (ctl < 0 || gpu < 0)
		goto fail;
	if (admin_rm_alloc(ctl, 0, 0, 0xad000001u, NV01_ROOT, NULL, &client))
		goto fail;
	if (admin_rm_alloc(ctl, client, client, 0xad000d00u,
			   NV01_DEVICE_0, &dp, &dev))
		goto fail;
	if (admin_rm_alloc(ctl, client, dev, 0xad002080u,
			   NV20_SUBDEVICE_0, &sp, &sub))
		goto fail;

	nv->admin_ctl_fd = ctl;
	nv->admin_gpu_fd = gpu;
	nv->admin_hclient = client;
	nv->admin_hsubdev = sub;
	nv->admin_state = 1;
	return 0;
fail:
	if (ctl >= 0) close(ctl);
	if (gpu >= 0) close(gpu);
	nv->admin_state = -1;
	return -1;
}

/*
 * Sum a per-pid VRAM metric (memPrivate + memSharedOwned) for the host process
 * group `tgid`, queried from QEMU's init-ns admin subdevice.  `index` selects
 * the metric (VIDEO_MEMORY_USAGE).  *any_out set if >=1 tid returned NV_OK.
 * Security: tgid is the validated isolate's own process; we only query tids
 * under it, never an arbitrary guest-named pid.
 */
static uint64_t nvkvm_admin_get_pid_mem(VirtIONvgpu *nv, pid_t tgid,
					uint32_t index, int *any_out)
{
	if (any_out)
		*any_out = 0;

	pthread_mutex_lock(&nv->admin_lock);
	if (nvkvm_admin_ensure(nv) != 0) {
		pthread_mutex_unlock(&nv->admin_lock);
		return 0;
	}
	int ctl = nv->admin_ctl_fd;
	uint32_t hcli = nv->admin_hclient, hsub = nv->admin_hsubdev;

	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/task", (int)tgid);
	DIR *d = opendir(path);
	if (!d) {
		pthread_mutex_unlock(&nv->admin_lock);
		return 0;
	}

	uint64_t sum = 0;
	int any = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		long tid = strtol(de->d_name, NULL, 10);
		if (tid <= 0)
			continue;

		/* The driver's NV2080_CTRL_GPU_GET_PID_INFO_PARAMS is a FIXED-size
		 * struct: pidInfoListCount@0, then pidInfoList[200]@8 inline
		 * (200 * 72 = 14400 → 14408 total).  A short buffer fails the
		 * kernel's paramsSize check (status != 0).  Send the full size with
		 * count=1 and only entry[0] populated. */
		uint8_t p[8 + 200 * NVKVM_PIDINFO_STRIDE];
		memset(p, 0, sizeof(p));
		uint32_t one = 1, t32 = (uint32_t)tid;
		memcpy(p + 0, &one, 4);
		memcpy(p + 8 + 0, &t32, 4);     /* entry.pid (init-ns host tid) */
		memcpy(p + 8 + 4, &index, 4);   /* entry.index                 */

		struct nvos54_parameters c = {
			.h_client = hcli, .h_object = hsub,
			.cmd = NV2080_CTRL_CMD_GPU_GET_PID_INFO,
			.params = (nvp64_t)(uintptr_t)p,
			.params_size = (uint32_t)sizeof(p),
		};
		int r = ioctl(ctl, NVADM_IOWR(NV_ESC_RM_CONTROL, sizeof c), &c);
		if (r < 0 || c.status != 0)
			continue;

		uint32_t result = 0;
		uint64_t priv = 0, shOwned = 0;
		memcpy(&result,  p + 8 + 8,  4);
		memcpy(&priv,    p + 8 + 16, 8);
		memcpy(&shOwned, p + 8 + 24, 8);
		if (result == 0) {              /* NV_OK */
			sum += priv + shOwned;
			any = 1;
		}
	}
	closedir(d);
	pthread_mutex_unlock(&nv->admin_lock);
	if (any_out)
		*any_out = any;
	return sum;
}

/*
 * S-3 (VMM abort): bytes-per-pixel for the scanout formats the virtual head is
 * allowed to flip.  Default-deny, like every other gate on this path: an
 * unknown fourcc is one whose pitch we cannot check, and an unchecked pitch is
 * what turns a present into a QEMU abort (see nvkvm_present_geom_ok).  The
 * guest head advertises XRGB8888/ARGB8888 only (src/guest/nvkvm_kms.c
 * nvkvm_pipe_formats[]); the BGR twins are listed because they are the same
 * 4-byte layout and cost nothing to accept.
 */
#define NVKVM_FOURCC(a, b, c, d) \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | \
	 ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static uint32_t nvkvm_present_bpp(uint32_t fourcc)
{
	switch (fourcc) {
	case NVKVM_FOURCC('X', 'R', '2', '4'):   /* DRM_FORMAT_XRGB8888 */
	case NVKVM_FOURCC('A', 'R', '2', '4'):   /* DRM_FORMAT_ARGB8888 */
	case NVKVM_FOURCC('X', 'B', '2', '4'):   /* DRM_FORMAT_XBGR8888 */
	case NVKVM_FOURCC('A', 'B', '2', '4'):   /* DRM_FORMAT_ABGR8888 */
		return 4;
	default:
		return 0;
	}
}

/*
 * Largest scanout we will accept.  The virtual head is a fixed 1080p mode, so
 * this is pure headroom; what it actually bounds is the host-side allocation a
 * flip triggers.  8192 keeps width*bpp and width*bpp*height comfortably inside
 * the `int` arithmetic QEMU's console code does on these values.
 */
#define NVKVM_PRESENT_MAX_DIM  8192u

/*
 * S-3 (VMM abort): PRESENT geometry is guest-controlled and lands, unexamined,
 * on qemu_console_resize() → qemu_create_displaysurface(), which computes
 * width*4 as a signed int and ABORTS the whole VMM when pixman refuses the
 * allocation — a guest-triggered kill of every other VM process sharing this
 * QEMU.  The GL branch hands the same numbers to qemu_dmabuf_new().  Note the
 * present path's own `surface_width(ds) != (int)w` test runs AFTER the resize,
 * i.e. after the allocation that would have aborted, so it is not a bound.
 *
 * Reject rather than clamp: a frame whose geometry does not describe its buffer
 * is not a frame we can fix up, and silently showing a different rectangle than
 * the guest flipped is its own bug.  `buf_size` is the real host dma-buf extent
 * (lseek SEEK_END) when known, 0 when not — with it this becomes an exact test
 * that the described image fits the memory behind it, which is also what keeps
 * the EGL import and the glReadPixels from running off the end.
 */
static bool nvkvm_present_geom_ok(const struct nvkvm_req_present *req,
				  uint64_t buf_size)
{
	uint32_t bpp = nvkvm_present_bpp(req->format);

	if (bpp == 0) {
		NVKVM_DBG("nvkvm present: DENY fourcc 0x%08x (not a scanout "
			  "format this head may flip)\n", req->format);
		return false;
	}
	if (req->width == 0 || req->height == 0 ||
	    req->width > NVKVM_PRESENT_MAX_DIM ||
	    req->height > NVKVM_PRESENT_MAX_DIM) {
		NVKVM_DBG("nvkvm present: DENY %ux%u (out of range)\n",
			  req->width, req->height);
		return false;
	}
	/* A row must hold the pixels it claims to. */
	if (req->pitch < (uint64_t)req->width * bpp) {
		NVKVM_DBG("nvkvm present: DENY pitch=%u < %u*%u\n",
			  req->pitch, req->width, bpp);
		return false;
	}
	if (req->pitch > (uint64_t)NVKVM_PRESENT_MAX_DIM * 4 * 4) {
		NVKVM_DBG("nvkvm present: DENY pitch=%u (absurd)\n", req->pitch);
		return false;
	}
	/* 64-bit product: two uint32s cannot wrap it, and buf_size below caps
	 * the whole image at what the host actually allocated. */
	uint64_t need = (uint64_t)req->pitch * req->height;
	if (buf_size && need > buf_size) {
		NVKVM_DBG("nvkvm present: DENY %ux%u pitch=%u needs %llu > "
			  "dma-buf %llu\n", req->width, req->height, req->pitch,
			  (unsigned long long)need,
			  (unsigned long long)buf_size);
		return false;
	}
	return true;
}

/*
 * PRESENT (#106 present path B) — the guest's virtual KMS head flipped a
 * scanout bo backed by a render-node GEM.  Ask the owning isolate's stub to
 * export it as a host dma-buf (PRIME_HANDLE_TO_FD) and route it to the host
 * display/codec.  This is the host/cross-VM boundary, so we validate hard:
 *   - graphics must be enabled (compute-only VMs never present);
 *   - the handle must be a render-node handle OWNED by this session, so a
 *     guest cannot coerce QEMU into PRIME-exporting an arbitrary fd (e.g. a
 *     /dev/nvidia0 control handle) — only its own DRM render GEMs.
 * The stub_handle is opaque to QEMU; the stub validates it against its own GEM
 * table when it runs the ioctl.
 */
int nvkvm_req_present(VirtIONvgpu *nv,
		      struct nvkvm_req_present *req,
		      struct nvkvm_resp_present *resp)
{
	resp->reserved = 0;

	if (!nv->graphics) {
		resp->status = EPERM;
		return 0;
	}

	struct nvkvm_handle *h = nvkvm_handle_get(&nv->handles, req->handle_id);
	if (!h || h->session_id != req->session_id ||
	    h->dev_id < NVKVM_DEV_DRM_RD(0) ||
	    h->dev_id >= NVKVM_DEV_DRM_RD(16)) {
		NVKVM_DBG("nvkvm present: bad handle %u (sess=%u dev=%d)\n",
			  req->handle_id, req->session_id, h ? h->dev_id : -1);
		resp->status = EINVAL;
		return 0;
	}

	/*
	 * S-3: geometry first, before anything is exported — this is the cheap,
	 * fd-free half of the check (the exact "does it fit the buffer" half
	 * needs the dma-buf, and runs once we have it, below).
	 */
	if (!nvkvm_present_geom_ok(req, 0)) {
		resp->status = EINVAL;
		return 0;
	}

	uint32_t iso_id = req->isolate_id ? req->isolate_id
					  : session_first_isolate(nv, req->session_id);
	if (iso_id == 0) {
		NVKVM_DBG("nvkvm present: no isolate (req_iso=%u sess=%u)\n",
			  req->isolate_id, req->session_id);
		resp->status = ENOENT;
		return 0;
	}

	/*
	 * S-2 (defence in depth): the handle→session half is checked above; this
	 * is the session→isolate half, the same pairing XISO_IMPORT verifies for
	 * both of its (isolate, handle) pairs.  A guest-supplied isolate_id that
	 * does not belong to the handle's session must not reach a stub even
	 * though the stub would resolve the GEM in its own table and answer
	 * -EBADF — that is the stub catching what the boundary should have.
	 * The isolate_id == 0 case takes iso_id from the session itself and so
	 * passes by construction.
	 */
	if (!session_has_isolate(nv, h->session_id, iso_id)) {
		NVKVM_DBG("nvkvm present: isolate/handle session mismatch "
			  "(iso=%u h=%u sess=%u)\n",
			  iso_id, req->handle_id, h->session_id);
		resp->status = EPERM;
		return 0;
	}

	int dmabuf_fd = -1;
	int r = nvkvm_isolate_present_export(&nv->isolates, iso_id,
					     req->handle_id, req->stub_handle,
					     &dmabuf_fd);
	if (r < 0 || dmabuf_fd < 0) {
		NVKVM_DBG("nvkvm present: export rc=%d iso=%u handle=%u gem=0x%x\n",
			  r, iso_id, req->handle_id, req->stub_handle);
		resp->status = (r < 0) ? (uint32_t)(-r) : EIO;
		return 0;
	}

	/*
	 * #106 verification: prove the host buffer crossed the boundary.  The
	 * dma-buf size is the real host allocation (block-linear scanout VRAM).
	 * (107 imports this as an EGLImage for capture/scanout; for now we close
	 * it per frame so no fd accumulates.)  One-shot fprintf so the proof is
	 * visible without NVKVM_DEBUG; per-frame detail under NVKVM_DBG.
	 */
	off_t sz = lseek(dmabuf_fd, 0, SEEK_END);
	/*
	 * S-3: now that the real host extent is known, re-run the geometry test
	 * against it.  The value was already being read here for logging; using
	 * it turns "the numbers are plausible" into "the described image fits
	 * the memory the host actually allocated", which is the property the
	 * EGL import and the readback downstream both depend on.
	 */
	if (!nvkvm_present_geom_ok(req, sz > 0 ? (uint64_t)sz : 0)) {
		close(dmabuf_fd);
		resp->status = EINVAL;
		return 0;
	}
	/*
	 * NVKVM_PRESENT_PROBE=1: report what is actually IN the buffer we just
	 * exported, over a plain CPU mapping.  This exists because "frames
	 * presented" counts handovers, not pixels: a correctly sized, correctly
	 * rotating, entirely empty buffer produces a black window while every
	 * counter in the path looks healthy.  Answering it without EGL keeps the
	 * question separate from how the frame is later displayed.
	 */
	if (getenv("NVKVM_PRESENT_PROBE") && sz > 0) {
		static unsigned probe_n;
		if ((probe_n++ % 60) == 0) {
			int probe_rw = getenv("NVKVM_PRESENT_PROBE")[0] == '2';
			void *m = mmap(NULL, (size_t)sz,
				       probe_rw ? (PROT_READ | PROT_WRITE)
						: PROT_READ,
				       MAP_SHARED, dmabuf_fd, 0);
			if (m == MAP_FAILED) {
				fprintf(stderr, "nvkvm present probe: gem=0x%x "
					"mmap failed: %s\n",
					req->stub_handle, strerror(errno));
			} else {
				const uint32_t *px = m;
				size_t npx = (size_t)sz / 4, nz = 0, first = 0;
				for (size_t k = 0; k < npx; k++) {
					if (px[k]) {
						if (!nz) {
							first = k;
						}
						nz++;
					}
				}
				fprintf(stderr, "nvkvm present probe: gem=0x%x size=%lld "
					"nonzero_px=%zu/%zu first=%zu px[first]=0x%08x\n",
					req->stub_handle, (long long)sz, nz, npx,
					nz ? first : 0, nz ? px[first] : 0);
				if (probe_rw) {
					/*
					 * Control: is this mapping a REAL view of the
					 * buffer, or does it merely succeed?  If a value
					 * written here does not read back, "all zeros"
					 * says nothing about what the guest rendered.
					 */
					uint32_t *w = m;
					w[0] = 0xA5A5F00Du;
					__sync_synchronize();
					fprintf(stderr, "nvkvm present probe: RW control "
						"wrote 0xA5A5F00D read back 0x%08x -> mapping %s\n",
						w[0],
						w[0] == 0xA5A5F00Du ? "IS real memory"
								    : "is NOT a real view");
				}
				munmap(m, (size_t)sz);
			}
		}
	}
	static bool logged_once;
	if (!logged_once) {
		logged_once = true;
		fprintf(stderr,
			"nvkvm present #106: host dma-buf fd=%d %ux%u pitch=%u "
			"fmt=0x%08x mod=0x%llx size=%lld (gem=0x%x)\n",
			dmabuf_fd, req->width, req->height, req->pitch,
			req->format, (unsigned long long)req->modifier,
			(long long)sz, req->stub_handle);
	}
	NVKVM_DBG("nvkvm present: dma-buf fd=%d %ux%u size=%lld gem=0x%x\n",
		  dmabuf_fd, req->width, req->height, (long long)sz,
		  req->stub_handle);

	/*
	 * #112 BROKER MODE: with -display nvkvm-broker there is no QEMU display
	 * at all.  Relay this same fd onward to the privileged broker over
	 * SCM_RIGHTS instead of consuming it — QEMU forwards a descriptor it was
	 * handed and touches no pixels, which is what lets a broker-mode QEMU run
	 * with no EGL, no GL and no /dev/dri/renderD* (docs/internal/
	 * display-broker-findings.md finding 7).
	 *
	 * Deliberately BEFORE nvkvm_present_submit(): that lives under
	 * #if defined(CONFIG_OPENGL) && NVKVM_QEMU_GRAPHICS and is a stub that
	 * returns false in a no-OpenGL build, so a relay placed after it would
	 * still work — but a relay placed INSIDE that file would not exist at
	 * all.  Keeping the call here keeps the broker path out of the OpenGL
	 * gate, which is the whole point.
	 *
	 * The capture path (#107) is repeated in this branch rather than shared
	 * with the fall-through below, because in broker mode we must not reach
	 * that code: relay_submit has taken the fd.  This is the "display +
	 * capture/NVENC" container profile — the one that still needs EGL and the
	 * render node, and still needs no display server.
	 */
	if (nvkvm_display_relay_active()) {
		const char *bcap = getenv("NVKVM_PRESENT_CAPTURE");

		if (bcap) {
			static unsigned bframe;
			if ((bframe++ % 30) == 0) {
				int cr = nvkvm_present_capture(dmabuf_fd,
							       req->width,
							       req->height,
							       req->pitch,
							       req->format,
							       req->modifier,
							       bcap);
				if (cr < 0)
					NVKVM_DBG("nvkvm present: capture rc=%d\n",
						  cr);
			}
		}
		if (nvkvm_display_relay_submit(nv, dmabuf_fd, req->width,
					       req->height, req->pitch,
					       req->format, req->modifier)) {
			resp->status = 0;
			return 0;
		}
	}

	/*
	 * #102: hand the frame to the live QEMU display window.  The console
	 * takes ownership of the dma-buf fd (retires it once presented), so on
	 * acceptance we must NOT close it here.  If no console is active
	 * (compute-only build, graphics=off, or no display backend), submit
	 * returns false and we fall through to close it ourselves.
	 */
	/*
	 * S-4: `stub_handle` is a GEM handle from the OWNING stub's own drm_file
	 * IDR — it starts at 1 in every isolate, so it is unique only within one
	 * isolate.  Pass the isolate id alongside it so the console's import
	 * cache can key on (isolate, bo) instead of colliding two compositors on
	 * bo 1 at the head's single fixed mode.
	 */
	if (nvkvm_present_submit(nv, dmabuf_fd, iso_id, req->stub_handle,
				 req->width, req->height,
				 req->pitch, req->format, req->modifier)) {
		resp->status = 0;
		return 0;
	}

	/*
	 * #107: capture the composited frame on the host.  Gated by
	 * NVKVM_PRESENT_CAPTURE=<path> (the readback is a synchronous glReadPixels
	 * — too costly to do every frame) and throttled to ~1/30 frames.  This is
	 * the interim "view it" mechanism on a headless host with no window.
	 */
	const char *cap = getenv("NVKVM_PRESENT_CAPTURE");
	if (cap) {
		static unsigned frame;
		if ((frame++ % 30) == 0) {
			int cr = nvkvm_present_capture(dmabuf_fd, req->width,
						       req->height, req->pitch,
						       req->format, req->modifier,
						       cap);
			if (cr < 0)
				NVKVM_DBG("nvkvm present: capture rc=%d\n", cr);
		}
	}
	close(dmabuf_fd);
	resp->status = 0;
	return 0;
}

/*
 * XISO_IMPORT (#110 cross-isolate dma-buf) — broker a GPU buffer owned by one
 * isolate into another (compositor importing a client bo).  Host/cross-VM
 * boundary, so validate hard, exactly like PRESENT:
 *   - graphics must be enabled;
 *   - BOTH the owner and importer handles must be render-node handles, so a
 *     guest cannot coerce QEMU into PRIME-exporting/importing an arbitrary fd.
 * Same-VM scoping is inherent: nv->handles / nv->isolates are this VM's only.
 * WHO may import (cross-UID / cross-container on the guest) is enforced
 * guest-side — the guest kernel gates which process holds the guest dma-buf fd
 * that drives this request (the access-model split: intra-VM rights = guest's).
 * Mechanism: owner stub PRIME_HANDLE_TO_FD → host dma-buf → importer stub
 * PRIME_FD_TO_HANDLE → local GEM, returned to the guest.  The dma-buf fd never
 * leaves QEMU's hands; only the same-VM stubs ever touch it.
 */
int nvkvm_req_xiso_import(VirtIONvgpu *nv,
			  struct nvkvm_req_xiso_import *req,
			  struct nvkvm_resp_xiso_import *resp)
{
	resp->gem_handle = 0;

	if (!nv->graphics) {
		resp->status = EPERM;
		return 0;
	}
	if (req->owner_isolate_id == 0 || req->importer_isolate_id == 0) {
		resp->status = ENOENT;
		return 0;
	}

	struct nvkvm_handle *oh = nvkvm_handle_get(&nv->handles, req->owner_handle_id);
	struct nvkvm_handle *ih = nvkvm_handle_get(&nv->handles, req->importer_handle_id);
	if (!oh || oh->dev_id < NVKVM_DEV_DRM_RD(0) ||
	    oh->dev_id >= NVKVM_DEV_DRM_RD(16) ||
	    !ih || ih->dev_id < NVKVM_DEV_DRM_RD(0) ||
	    ih->dev_id >= NVKVM_DEV_DRM_RD(16)) {
		NVKVM_DBG("nvkvm xiso: non-render handle (owner=%u imp=%u)\n",
			  req->owner_handle_id, req->importer_handle_id);
		resp->status = EINVAL;
		return 0;
	}

	/*
	 * The guest asserts two (isolate, handle) pairings; verify both against
	 * QEMU's own session bookkeeping.  A guest that names isolate X together
	 * with a handle belonging to some other session is either buggy or
	 * probing, and must not reach a stub either way.  Note this deliberately
	 * does NOT require owner and importer to share a session — differing
	 * sessions is the entire point of a cross-isolate share; what is checked
	 * is that each isolate genuinely belongs to the session owning the handle
	 * presented with it.  Cross-VM is already impossible: nv->handles and
	 * nv->sessions are this VM's alone.
	 */
	if (!session_has_isolate(nv, oh->session_id, req->owner_isolate_id) ||
	    !session_has_isolate(nv, ih->session_id, req->importer_isolate_id)) {
		NVKVM_DBG("nvkvm xiso: isolate/handle session mismatch "
			  "(owner iso=%u h=%u sess=%u; imp iso=%u h=%u sess=%u)\n",
			  req->owner_isolate_id, req->owner_handle_id, oh->session_id,
			  req->importer_isolate_id, req->importer_handle_id,
			  ih->session_id);
		resp->status = EPERM;
		return 0;
	}

	/* 1. Owner stub exports the bo as a host dma-buf (PRIME_HANDLE_TO_FD). */
	int dmabuf_fd = -1;
	int r = nvkvm_isolate_present_export(&nv->isolates, req->owner_isolate_id,
					     req->owner_handle_id,
					     req->owner_stub_handle, &dmabuf_fd);
	if (r < 0 || dmabuf_fd < 0) {
		NVKVM_DBG("nvkvm xiso: owner export rc=%d iso=%u gem=0x%x\n",
			  r, req->owner_isolate_id, req->owner_stub_handle);
		resp->status = (r < 0) ? (uint32_t)(-r) : EIO;
		return 0;
	}

	/* 2. Importer stub PRIME_FD_TO_HANDLEs it into a local GEM. */
	uint32_t gem = 0;
	r = nvkvm_isolate_xiso_import(&nv->isolates, req->importer_isolate_id,
				      req->importer_handle_id, dmabuf_fd, &gem);
	close(dmabuf_fd);
	if (r < 0) {
		NVKVM_DBG("nvkvm xiso: importer import rc=%d iso=%u\n",
			  r, req->importer_isolate_id);
		resp->status = (uint32_t)(-r);
		return 0;
	}
	NVKVM_DBG("nvkvm xiso: owner(iso=%u gem=0x%x) -> importer(iso=%u) gem=0x%x\n",
		  req->owner_isolate_id, req->owner_stub_handle,
		  req->importer_isolate_id, gem);
	resp->gem_handle = gem;
	resp->status = 0;
	return 0;
}


/*
 * ── RM_MAP_MEMORY / RM_UNMAP_MEMORY virtual-address table (A100 fix) ──────
 *
 * NV_ESC_RM_UNMAP_MEMORY carries the VA to unmap in NVOS34.p_linear_address.
 * The guest zeroes that field on purpose (src/guest/nvkvm_ioctl.c) -- a guest
 * VA is meaningless in the isolate's address space -- and its comment states
 * the contract: "host fills in from its map table".  That map table never
 * existed on the live path.  The only code that wrote the field host-side sits
 * inside nvkvm_dispatch.c's `#if 0` block, so every unmap reached the driver
 * with VA 0 and came back NV_ERR_OBJECT_NOT_FOUND (0x57).
 *
 * Measured on an A100 (GA100): that was the ONLY non-zero nvstatus in 1885
 * forwarded ioctls of a failing cuCtxCreate.  Consumer cards very likely hit
 * the same failed unmap and survive it, because the FREE that follows tears
 * the object down anyway.
 *
 * This is the missing half: remember the VA the driver hands back on a
 * successful MAP, and put it back on the matching UNMAP.
 *
 * Keyed on isolate_id as well as the object triple, deliberately: the table is
 * VM-global, and without that an isolate could name another isolate's mapping
 * and have the host unmap it.
 */
#define NVKVM_MAPVA_MAX 8192

struct nvkvm_mapva_ent {
	int      in_use;
	uint32_t isolate_id;
	uint32_t h_client, h_device, h_memory;
	uint64_t va;
	uint64_t seq;      /* insertion order, for LIFO matching */
};

static struct nvkvm_mapva_ent g_mapva[NVKVM_MAPVA_MAX];
static pthread_mutex_t g_mapva_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_mapva_seq;

static void nvkvm_mapva_record(uint32_t iso, uint32_t hc, uint32_t hd,
			       uint32_t hm, uint64_t va)
{
	int free_slot = -1;
	if (!va)
		return;
	pthread_mutex_lock(&g_mapva_lock);
	/*
	 * Always take a NEW slot; never overwrite an existing entry for the same
	 * object.  One object can be mapped more than once at different VAs
	 * (NVOS33 carries offset and length, so the same hMemory yields
	 * different addresses), and an earlier version of this code overwrote on
	 * remap.  That lost the first VA, so the next unmap of the FIRST mapping
	 * was handed the SECOND mapping's address and tore down the wrong
	 * region -- observed as vk_probe/gl_probe dying on SIGBUS while pure
	 * CUDA, which remaps far less, passed.
	 */
	for (int i = 0; i < NVKVM_MAPVA_MAX; i++) {
		if (!g_mapva[i].in_use) { free_slot = i; break; }
	}
	if (free_slot >= 0) {
		struct nvkvm_mapva_ent ne = { 1, iso, hc, hd, hm, va,
					      ++g_mapva_seq };
		g_mapva[free_slot] = ne;
	}
	/* Table full: drop it.  The unmap then fails exactly as it did before
	 * this fix -- degraded, not newly broken. */
	pthread_mutex_unlock(&g_mapva_lock);
}

/* Look up and CONSUME the mapping: an unmap retires it. */
static uint64_t nvkvm_mapva_take(uint32_t iso, uint32_t hc, uint32_t hd,
				 uint32_t hm)
{
	uint64_t va = 0, best = 0;
	int best_i = -1;
	pthread_mutex_lock(&g_mapva_lock);
	/*
	 * LIFO: take the MOST RECENT mapping of this object.  Map/unmap of the
	 * same object nests in practice, so newest-first is the pairing that
	 * matches; taking an arbitrary entry is what produced the wrong-region
	 * unmap described above.
	 */
	for (int i = 0; i < NVKVM_MAPVA_MAX; i++) {
		struct nvkvm_mapva_ent *e = &g_mapva[i];
		if (e->in_use && e->isolate_id == iso && e->h_client == hc &&
		    e->h_device == hd && e->h_memory == hm && e->seq > best) {
			best = e->seq;
			best_i = i;
		}
	}
	if (best_i >= 0) {
		va = g_mapva[best_i].va;
		g_mapva[best_i].in_use = 0;
	}
	pthread_mutex_unlock(&g_mapva_lock);
	return va;
}

void nvkvm_mapva_forget_isolate(uint32_t iso)
{
	pthread_mutex_lock(&g_mapva_lock);
	for (int i = 0; i < NVKVM_MAPVA_MAX; i++)
		if (g_mapva[i].in_use && g_mapva[i].isolate_id == iso)
			g_mapva[i].in_use = 0;
	pthread_mutex_unlock(&g_mapva_lock);
}

/* ── Cross-isolate RM export/import (CUDA VMM shareable handles) ────────────
 *
 * cuMemExportToShareableHandle(CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR) parks
 * an RM object on a freshly-opened /dev/nvidiactl fd via RM control 0x3d05 (fd
 * at inner offset 16).  The exporter passes that fd to a peer PROCESS over a
 * Unix socket with SCM_RIGHTS, and the peer calls 0x3d08
 * (GET_EXPORT_OBJECT_INFO) and then 0x3d06 (IMPORT_OBJECT_FROM_FD) on it, both
 * with the fd at inner offset 0.  That exchange is exactly NCCL's SHM transport
 * connection setup (v2.27.3 transport/shm.cc:590).
 *
 * Two guest processes are two isolates, and that is what makes this hard — the
 * same problem #110 solved for dma-bufs: an RM object minted in stub A is
 * meaningless in stub B.  The guest kernel rewrites the embedded fd to a
 * VM-global handle_id (so no guest fd number and no guest VA crosses the
 * boundary), but the fd behind that handle was opened by the EXPORTER's stub,
 * so the importer stub's handle_lookup() missed and the control reached the
 * driver carrying a bogus fd.  libcuda reported that as CUDA error 101,
 * CUDA_ERROR_INVALID_DEVICE.
 *
 * The answer is the one docs/internal/cross-isolate-sharing.md already
 * documents: QEMU is the broker.  QEMU holds a copy of every handle's fd
 * (struct nvkvm_handle.fd), so nothing new has to be invented and no new
 * isolate command is needed — ISOLATE_CMD_RECEIVE_FD relays a dup into the
 * importer stub, and the stub's existing handle_lookup() then resolves it on
 * the path that was already there for 0x3d05/0x3d06.
 *
 * Confinement is structural, exactly as for xiso: nv->handles hangs off the
 * per-guest VirtIONvgpu, so there is no namespace in which one guest could
 * name another guest's handle.  Within the VM, entitlement stays the guest
 * kernel's call and is enforced by fd possession — the importer can only name
 * this handle because it was handed a real fd for it through a real kernel
 * fd-passing mechanism, which cannot be forged the way a guessable cookie
 * could.  QEMU additionally re-derives the owning isolate from the handle
 * itself rather than trusting anything the guest said about it.
 */
static void nvkvm_xrm_materialise(VirtIONvgpu *nv, uint32_t iso_id,
				  uint32_t handle_id)
{
	struct nvkvm_handle *h;
	uint32_t owner_iso;

	if (iso_id == 0 || handle_id == 0)
		return;

	h = nvkvm_handle_get(&nv->handles, handle_id);
	if (!h || h->type != NVKVM_HANDLE_TYPE_NVIDIA || h->fd < 0)
		return;

	/*
	 * Re-derive the owner from QEMU's own bookkeeping.  Same isolate means
	 * the existing intra-stub path already works and there is nothing to
	 * broker — the overwhelmingly common case, and the one this must not
	 * perturb.
	 */
	owner_iso = session_first_isolate(nv, h->session_id);
	if (owner_iso == 0 || owner_iso == iso_id)
		return;

	/*
	 * Both isolates must belong to this VM.  session_first_isolate() only
	 * ever walks THIS nv's sessions, and the caller's iso_id was validated
	 * upstream, so this is confinement in depth rather than the only check.
	 */
	if (!session_has_isolate(nv, h->session_id, owner_iso))
		return;

	if (nvkvm_isolate_note_foreign_handle(&nv->isolates, iso_id, handle_id))
		return;   /* already relayed here (or table full) */

	uint64_t generation = 0;
	if (nvkvm_isolate_send_handle_generation(&nv->isolates, &nv->handles,
						 iso_id, handle_id,
						 &generation) == 0) {
		/*
		 * note() above reserved the entry before the blocking relay, so
		 * the record exists but does not yet name a generation.  Publish
		 * it only now that delivery succeeded.  If teardown won the race
		 * the reservation is gone, the delivered fd died with the
		 * isolate, and the reference the send took is ours to retire --
		 * nobody else will.
		 */
		if (!nvkvm_isolate_finalize_foreign_handle(&nv->isolates, iso_id,
							   handle_id, generation))
			nvkvm_handle_unref_isolate_generation(&nv->handles,
							      handle_id,
							      generation);
		NVKVM_DBG("nvkvm xrm: owner(iso=%u) -> importer(iso=%u) "
			  "handle=%u relayed\n", owner_iso, iso_id, handle_id);
	} else {
		/*
		 * Best-effort: leave the control to be forwarded and fail the
		 * way it did before rather than converting a relay miss into a
		 * new hard error, and drop the note so a retry can try again.
		 * send_handle_generation() has already rolled back its own
		 * reference, so there is nothing to unref here.
		 */
		nvkvm_isolate_forget_foreign_handle(&nv->isolates, iso_id,
						    handle_id, NULL);
	}
}

/*
 * If this RM_CONTROL is one of the export/import family, make the handle it
 * names resolvable in the calling isolate.  Offsets are MEASURED against the
 * host's own ioctl stream (tools/nv_ioctl_trace.c): 0x3d05 carries the fd at
 * inner offset 16, 0x3d06 and 0x3d08 at inner offset 0.
 */
static void nvkvm_xrm_prepare(VirtIONvgpu *nv,
			      struct nvkvm_req_ioctl_on_isolate *req,
			      void *param_buf, void *aux_buf)
{
	uint32_t icmd = 0;
	int      hoff = -1;
	int32_t  hid  = 0;

	if (_IOC_TYPE(req->cmd) != 'F' ||
	    _IOC_NR(req->cmd) != NV_ESC_RM_CONTROL || !aux_buf)
		return;
	if (!param_buf || req->param_size < 12)
		return;
	memcpy(&icmd, (char *)param_buf + 8, 4);

	if (icmd == 0x00003d05u && req->aux_size >= 20)
		hoff = 16;
	else if ((icmd == 0x00003d06u || icmd == 0x00003d08u) &&
		 req->aux_size >= 4)
		hoff = 0;
	if (hoff < 0)
		return;

	memcpy(&hid, (char *)aux_buf + hoff, 4);
	if (hid > 0)
		nvkvm_xrm_materialise(nv, req->isolate_id, (uint32_t)hid);
}

/*
 * ── R-1 ── does this command word's _IOC_TYPE match the device the handle was
 * actually opened on?
 *
 * The whole gate chain in nvkvm_req_ioctl_on_isolate() branches on
 * _IOC_TYPE(req->cmd) — the DRM NR allowlist on 'd', the NVKMS inner-cmdType
 * allowlist on the exact NVKMS wrapper cmd, and the frontend NR allowlist, U-3,
 * A-1, the alloc-class and control-cmd allowlists, the DUP_OBJECT source gate
 * and the H-3 hClient gate on 'F'.  The stub then branches on the same field
 * again (nvkvm_stub.c ptr_off, zero_nvos64_rights, clamp_inner_params_size, the
 * embedded-fd translation, the RM_IDLE_CHANNELS memset).  And _IOC_TYPE is a
 * field of a command word the GUEST composes, carried across the wire untouched
 * (src/guest/nvkvm_virtio.c:1343), while the fd it lands on comes from
 * req->handle_id — an independent guest-chosen value.  Nothing tied the two
 * together, so branch selection was attacker-controlled.
 *
 * A guest could aim a type-'d' cmd at a /dev/nvidiactl handle: the 'd' branch
 * runs the DRM allowlist (which admits nr 0x41, nvkvm_drm_allowlist.h:51),
 * every 'F'-keyed gate is skipped, and the M-A non-'F' default-deny is never
 * reached because 'd' IS a recognised type.  The NVIDIA frontend then dispatches
 * on _IOC_NR while ignoring _IOC_TYPE entirely, so nr 0x41 executes as
 * NV_ESC_RM_IDLE_CHANNELS.  VERIFIED in the vendor source rather than assumed:
 * nvidia_ioctl() reads only _IOC_NR/_IOC_SIZE and switches on
 * arg_cmd = _IOC_NR(cmd) — open-gpu-kernel-modules 580.159.04
 * kernel-open/nvidia/nv.c:2404-2405 and :2501; on 610.43.02 the added
 * nv_validate_ioctls() keys its table on (cmd & 0xFF) alone (:2421).  That
 * reopens G-2 (src/abi/nvgpu.h:402-410): the stub writes a host VA at offset 8,
 * which on NVOS30 is h_channel/num_channels, and p_clients/p_devices/p_channels
 * reach the driver exactly as the guest wrote them, because the memset that
 * clears [12,40) is 'F'-keyed.
 *
 * So key the decision on the real discriminator — the device this handle was
 * opened on — rather than on a proxy the guest controls.  That is A-1's
 * correction applied to branch selection itself.  One arm per device class,
 * default deny.
 *
 * It also closes R-2, the reverse direction ('F' aimed at a render node).
 * drm_ioctl() does reject a mismatched type outright — DRM_IOCTL_TYPE(cmd) !=
 * DRM_IOCTL_BASE → -ENOTTY, Linux drivers/gpu/drm/drm_ioctl.c:840-841, and
 * nvidia-drm routes through it (nv_drm_ioctl → drm_ioctl,
 * nvidia-drm-drv.c:1765) — so R-2 was already inert.  But that is an
 * out-of-tree control we do not own, and a symmetric check costs nothing.
 *
 * The mapping, taken from the tree and not from memory:
 *   NVKVM_DEV_CTL / NVKVM_DEV_GPU(n)  → 'F'  (nvkvm_proto.h:96,98; the frontend
 *        char devices — nvkvm_stub.c:1839-1858 maps these ids to
 *        nvidiactl / nvidiaN)
 *   NVKVM_DEV_DRM_RD(n)               → 'd'  (:99; dri/renderD128+n,
 *        nvkvm_stub.c:1864-1874)
 *   NVKVM_DEV_MODESET                 → the ONE NVKMS wrapper command
 *        (:100, :110 — _IOWR('m',0,NvKmsIoctlParams)); matched on the full
 *        command word, exactly as the NVKMS branch itself does
 *   NVKVM_DEV_UVM                     → nothing here.  UVM ioctls are answered
 *        and returned before this point, and a UVM command number is not an
 *        _IO() encoding at all.  A UVM handle that reaches here (fd < 0, i.e.
 *        broken) is denied, which is the right answer.
 *   NVKVM_DEV_EVENTFD (0xFF)          → NOTHING, and this was the open question
 *        R-1.3 raised.  eventfd_fops has no .unlocked_ioctl (fs/eventfd.c), so
 *        every ioctl on one returns -ENOTTY; and nothing in the guest ever
 *        opens a handle with this dev_id — the fd for NV01_EVENT_OS_EVENT is a
 *        /dev/nvidia* fd, not a generic eventfd, and the guest rewrites it to
 *        that ctx's handle_id (src/guest/nvkvm_main.c:2070-2098).  A guest can
 *        still ASK for one (nvkvm_req_open_nvidia_handle does not restrict
 *        dev_id), so the arm must exist.  It denies.
 *
 * TYPE_MEMORY handles must not be classified by dev_id at all: a memfd leaves
 * the field at its zero init (nvkvm_handle.c:231), which is numerically
 * NVKVM_DEV_CTL.  Require NVKVM_HANDLE_TYPE_NVIDIA explicitly.
 *
 * Extracted verbatim into tests/unit/ by an awk rule between the two marker
 * comments below, so the test pins THIS code and cannot drift from it.  Do not
 * reformat the markers.
 */
/* NVKVM_R1_TYPE_DEV_BEGIN */
static bool nvkvm_ioctl_type_matches_dev(int handle_type, int dev_id,
					 uint32_t cmd)
{
	unsigned ic_type = _IOC_TYPE(cmd);

	if (handle_type != NVKVM_HANDLE_TYPE_NVIDIA)
		return false;
	if (dev_id == NVKVM_DEV_CTL ||
	    (dev_id >= NVKVM_DEV_GPU(0) &&
	     dev_id <= NVKVM_DEV_GPU(NV_MINOR_DEVICE_NUMBER_REGULAR_MAX)))
		return ic_type == 'F';
	if (dev_id >= NVKVM_DEV_DRM_RD(0) && dev_id < NVKVM_DEV_DRM_RD(16))
		return ic_type == 'd';
	if (dev_id == NVKVM_DEV_MODESET)
		return cmd == NVKVM_NVKMS_IOCTL_CMD;
	return false;
}
/* NVKVM_R1_TYPE_DEV_END */

int nvkvm_req_ioctl_on_isolate(VirtIONvgpu *nv,
				struct nvkvm_req_ioctl_on_isolate *req,
				struct nvkvm_resp_ioctl_on_isolate *resp,
				void *param_buf, void *aux_buf)
{
	/*
	 * UVM ioctls run in QEMU's process, not the stub.  UVM binds its
	 * file's nvfp to the calling task's mm during UVM_INITIALIZE, and
	 * the matching mmap of /dev/nvidia-uvm must come from the same mm
	 * — which is QEMU (we install a KVM memory region at the resulting
	 * host VA so the guest sees the GPU memory at the right GPA).
	 *
	 * Command-buffer data is already explicitly copied via shm slots,
	 * so the kernel's copy_from_user reading param_buf from QEMU's
	 * address space gives the right bytes regardless of which process
	 * issues the ioctl.
	 *
	 * NOTE on access rights (intentionally NOT enforced here): intra-VM,
	 * per-guest-process access control (which process may touch which
	 * object) is emulated entirely by the guest kernel module — it owns the
	 * guest's pids/uids/namespaces/fds and is the authority.  QEMU must NOT
	 * second-guess it with a session-ownership check: doing so would wrongly
	 * reject a handle that the guest LEGITIMATELY shared into another isolate
	 * via a guest-commanded COPY_HANDLE_TO_ISOLATE (e.g. CUDA IPC), and it
	 * adds no security (malicious guest userspace is blocked by the guest
	 * module; a malicious guest kernel would just forge session_id).  QEMU's
	 * boundary is cross-VM / host-process (the per-VM handle table + hClient
	 * allowlist + no host-wide TYPE_ALL), not intra-VM.
	 */
	{
		struct nvkvm_handle *h =
			nvkvm_handle_get(&nv->handles, req->handle_id);
		if (h && h->dev_id == NVKVM_DEV_UVM && h->fd >= 0) {
			/* Phase 3: schema-gated forwarding.  Look the cmd up in
			 * the UVM allowlist; refuse anything not described. */
			const struct nvkvm_uvm_desc *d = nvkvm_uvm_lookup(req->cmd);
			if (!d) {
				NVKVM_DBG(
					"nvkvm: DENY unschemaed UVM ioctl cmd=0x%x "
					"(default-deny)\n", req->cmd);
				resp->retval     = (uint64_t)(int64_t)(-EPERM);
				resp->status     = 0;
				resp->nvstatus   = 0x57; /* NV_ERR_NOT_SUPPORTED */
				resp->fault_addr = 0;
				return 0;
			}
			/* #81: two of the schema's min_sizes are version-variant.
			 * The table carries the V550 (256 per-GPU-attribute)
			 * sizes — 9264 / 9248 — but on a driver <= 545 the guest
			 * legitimately sends the pre-V550 1200 / 1184.  Taking
			 * the floor from the table would DENY every valid
			 * MAP_EXTERNAL_ALLOCATION on a 535 host. Override from
			 * the active profile for exactly those two cmds. */
			uint32_t min_size = d->min_size;
			const struct nvkvm_abi_profile *prof =
				nv->abi ? nv->abi : nvkvm_abi_by_id(NVKVM_ABI_570);
			if (req->cmd == 33 /* UVM_MAP_EXTERNAL_ALLOCATION */)
				min_size = prof->uvm_map_ext_size;
			else if (req->cmd == 68 /* UVM_ALLOC_SEMAPHORE_POOL */)
				min_size = prof->uvm_sem_pool_size;

			if (req->param_size < min_size ||
			    (min_size > 0 && !param_buf)) {
				NVKVM_DBG(
					"nvkvm: DENY UVM cmd=0x%x short param_size=%u "
					"(<%u)\n", req->cmd, req->param_size,
					min_size);
				resp->retval     = (uint64_t)(int64_t)(-EINVAL);
				resp->status     = 0;
				resp->nvstatus   = 0x1f; /* NV_ERR_INVALID_ARGUMENT */
				resp->fault_addr = 0;
				return 0;
			}
			/*
			 * ── U-6 — validate the guest-supplied VA range ─────
			 *
			 * These are addresses in QEMU'S mm (this ioctl runs in
			 * QEMU's process).  Unvalidated, a guest names any
			 * address in the process that holds the KVM fd, the
			 * memslots and every isolate's socket — and for
			 * UVM_MIGRATE a range UVM does not own falls through to
			 * the pageable path, which migrates QEMU's OWN
			 * anonymous pages.  Require every range-USING command
			 * to name a range nvkvm recorded for THIS UVM handle
			 * when the driver accepted the corresponding
			 * range-CREATING command.  Anything unresolvable is
			 * refused here and never reaches the driver.
			 */
			uint64_t va_base = 0, va_len = 0;
			bool va_checked = false;
			if (d->va_off != 0xffff &&
			    d->va_mode != NVKVM_UVM_VA_NONE) {
				uint32_t off = d->va_off;
				if (!param_buf ||
				    req->param_size < (uint64_t)off + 16) {
					/* Cannot even read the pair → deny. */
					NVKVM_DBG("nvkvm: DENY UVM cmd=0x%x "
						  "param_size=%u too short for "
						  "VA range at +%u (U-6)\n",
						  req->cmd, req->param_size, off);
					resp->retval   = (uint64_t)(int64_t)(-EINVAL);
					resp->status   = 0;
					resp->nvstatus = 0x1f; /* INVALID_ARGUMENT */
					resp->fault_addr = 0;
					return 0;
				}
				memcpy(&va_base, (char *)param_buf + off, 8);
				memcpy(&va_len,  (char *)param_buf + off + 8, 8);
				/*
				 * UVM_FREE identifies the range by base alone;
				 * libcuda sends length 0 and the driver looks it
				 * up by start address.  Measured on an RTX 4070 Ti
				 * SUPER with driver 595.84: refusing that as a
				 * "malformed VA range" made the free fail, the
				 * range stayed live, and every later CUDA call in
				 * that context returned INVALID_VALUE -- surfacing
				 * as cuda_kernel_launch / cuda_matmul "setup rc=1"
				 * in tests/validate.sh.  Substitute the length we
				 * recorded for that base; an unrecorded base still
				 * yields 0 and is still refused.
				 */
				if (d->va_mode == NVKVM_UVM_VA_FREE &&
				    va_len == 0 && va_base != 0)
					va_len = uvm_va_len_at(req->handle_id,
							       va_base);
				/*
				 * (0,0) is the "no VA range" form — measured on
				 * UVM_REGISTER_CHANNEL, which libcuda issues both
				 * with and without a channel VA range.  Pass it
				 * through untracked; there is no address to abuse.
				 */
				if (va_base != 0 || va_len != 0) {
					va_checked = true;
					if (!uvm_va_sane(va_base, va_len)) {
						fprintf(stderr,
							"nvkvm: DENY UVM cmd=0x%x "
							"malformed VA range "
							"0x%llx+0x%llx (U-6)\n",
							req->cmd,
							(unsigned long long)va_base,
							(unsigned long long)va_len);
						resp->retval   = (uint64_t)(int64_t)(-EINVAL);
						resp->status   = 0;
						resp->nvstatus = 0x1e; /* NV_ERR_INVALID_ADDRESS */
						resp->fault_addr = 0;
						return 0;
					}
					if (d->va_mode == NVKVM_UVM_VA_CREATE &&
					    !uvm_va_have_room()) {
						fprintf(stderr,
							"nvkvm: DENY UVM cmd=0x%x "
							"VA ownership table full "
							"(U-6)\n", req->cmd);
						resp->retval   = (uint64_t)(int64_t)(-ENOMEM);
						resp->status   = 0;
						resp->nvstatus = 0x1a; /* NV_ERR_INSUFFICIENT_RESOURCES */
						resp->fault_addr = 0;
						return 0;
					}
					if ((d->va_mode == NVKVM_UVM_VA_USE ||
					     d->va_mode == NVKVM_UVM_VA_FREE) &&
					    !uvm_va_covers(req->handle_id,
							   va_base, va_len)) {
						fprintf(stderr,
							"nvkvm: DENY UVM cmd=0x%x "
							"VA range 0x%llx+0x%llx not "
							"owned by handle %u (U-6)\n",
							req->cmd,
							(unsigned long long)va_base,
							(unsigned long long)va_len,
							req->handle_id);
						resp->retval   = (uint64_t)(int64_t)(-EINVAL);
						resp->status   = 0;
						resp->nvstatus = 0x1e; /* NV_ERR_INVALID_ADDRESS */
						resp->fault_addr = 0;
						return 0;
					}
				}
			}

			/* A fresh UVM_INITIALIZE means a fresh va_space: drop
			 * anything a previous incarnation of this handle_id
			 * recorded, so a recycled id never inherits ownership. */
			if (req->cmd == 0x30000001 /* UVM_INITIALIZE */)
				nvkvm_uvm_va_purge_handle(req->handle_id);

			/* Translate each embedded frontend-fd field: the guest
			 * sanitizer rewrote the fd into a handle_id; swap to
			 * QEMU's local fd for the kernel, then restore the
			 * handle_id on response so libcuda sees what it sent. */
			uint32_t saved_val[2];
			int      saved_off[2];
			int      nsaved = 0;
			for (int k = 0; k < 2 && d->fd_off[k] != 0xffff; k++) {
				uint32_t off = d->fd_off[k];
				if (!param_buf || req->param_size < off + 4)
					continue;
				uint32_t hid;
				memcpy(&hid, (char *)param_buf + off, 4);
				if (hid == 0 || hid == (uint32_t)-1)
					continue;
				struct nvkvm_handle *hh =
					nvkvm_handle_get(&nv->handles, hid);
				if (!hh || hh->fd < 0)
					continue;
				saved_val[nsaved] = hid;
				saved_off[nsaved] = (int)off;
				nsaved++;
				uint32_t fd32 = (uint32_t)hh->fd;
				memcpy((char *)param_buf + off, &fd32, 4);
			}
			/* C-2: dup the target fd under the table lock so a
			 * concurrent CLOSE_HANDLE on the TX thread cannot
			 * close()+recycle this fd while we're mid-ioctl on the
			 * pool worker.  The dup keeps the struct file alive for
			 * the whole call; we close it immediately after. */
			int tfd = nvkvm_handle_acquire_fd(&nv->handles,
							  req->handle_id, NULL,
							  NULL);
			if (tfd < 0) {
				resp->retval     = (uint64_t)(int64_t)(-EBADF);
				resp->status     = 0;
				resp->nvstatus   = 0x1f; /* NV_ERR_INVALID_ARGUMENT */
				resp->fault_addr = 0;
				return 0;
			}
			/*
			 * ── U-6 — bounce buffer ───────────────────────────
			 *
			 * param_buf is a g_malloc of EXACTLY req->param_size
			 * (nvkvm_ioctl_work_fn), and req->param_size is the
			 * GUEST's idea of the struct size.  The UVM driver
			 * copies sizeof(ITS OWN struct) in and out.  Those
			 * disagree on the live path — measured: the guest sends
			 * 48 bytes for UVM_REGISTER_CHANNEL where the driver's
			 * UVM_REGISTER_CHANNEL_PARAMS is 56 and writes rmStatus
			 * at +48, i.e. 4 bytes past the allocation; UVM_MIGRATE
			 * is 48 vs the driver's 80, so the driver would read 32
			 * bytes of adjacent QEMU heap — including 8 bytes it
			 * would treat as semaphoreAddress, an address it WRITES
			 * to.  Run the ioctl on an over-sized ZEROED buffer so
			 * every byte the guest did not send is deterministically
			 * 0 and no driver access lands in QEMU's heap, then copy
			 * back only what the guest asked for.
			 */
			size_t bsz = req->param_size;
			if (bsz < NVKVM_UVM_BOUNCE_MIN)
				bsz = NVKVM_UVM_BOUNCE_MIN;
			void *bounce = g_malloc0(bsz);
			if (param_buf && req->param_size)
				memcpy(bounce, param_buf, req->param_size);
			/*
			 * U-6 — UVM_MIGRATE.semaphoreAddress is the ONE UVM
			 * field the driver dereferences for a WRITE (it stores
			 * semaphorePayload there on async completion).  Zero it
			 * unconditionally: the gate cannot be skipped by any
			 * guest-chosen value, which is what "enforced" means
			 * here.  This is invisible to a legitimate guest — the
			 * guest module's own UVM_MIGRATE struct is 48 bytes
			 * (src/abi/uvm.h) and stops before semaphoreAddress@40,
			 * so those bytes are its rmStatus/reserved being
			 * MISREAD as an address by the driver.  Async-semaphore
			 * migration completion is not part of the nvkvm path.
			 */
			if (req->cmd == 51 /* UVM_MIGRATE */ && bsz >= 52)
				memset((char *)bounce + 40, 0, 12);
			int r = ioctl(tfd, (unsigned long)req->cmd, bounce);
			int saved_errno = errno;
			close(tfd);
			/*
			 * U-6 — the driver's real verdict for the ownership
			 * table.  The `st` computed further down is read from
			 * param_size-4, which is only the rmStatus field for
			 * SOME UVM structs (for UVM_REGISTER_CHANNEL our 48-byte
			 * ABI struct ends in the high half of `length`).  Read
			 * rmStatus at the offset the DRIVER's struct puts it
			 * (ogkm 575.51.03 kernel-open/nvidia-uvm/uvm_ioctl.h),
			 * out of the bounce buffer that is guaranteed long
			 * enough to hold it.  0xffffffff = "unknown" and is
			 * treated as failure, so an unrecognised cmd can never
			 * add ownership.
			 */
			uint32_t va_rmstatus = 0xffffffffu;
			if (va_checked) {
				uint32_t so = 0xffffffffu;
				switch (req->cmd) {
				case 27: so = 48;   break; /* REGISTER_CHANNEL      */
				case 34: so = 16;   break; /* FREE                  */
				case 65: so = 36;   break; /* MAP_DYNAMIC_PARALLEL. */
				case 68: so = 9240; break; /* ALLOC_SEMAPHORE_POOL  */
				case 73: so = 16;   break; /* CREATE_EXTERNAL_RANGE */
				default: break;
				}
				if (so != 0xffffffffu && (size_t)so + 4 <= bsz)
					memcpy(&va_rmstatus, (char *)bounce + so, 4);
			}
			if (param_buf && req->param_size)
				memcpy(param_buf, bounce, req->param_size);
			g_free(bounce);
			for (int k = 0; k < nsaved; k++)
				memcpy((char *)param_buf + saved_off[k],
				       &saved_val[k], 4);
			uint32_t st = 0;
			/* UVM_*_PARAMS conventionally ends with rmStatus (u32).
			 * Read the last 4 bytes of the params struct. */
			if (param_buf && req->param_size >= 4) {
				memcpy(&st, (char *)param_buf + req->param_size - 4,
				       sizeof(st));
			}
			/*
			 * U-6 — maintain the ownership table from the DRIVER's
			 * own verdict, never from the guest's request.  A range
			 * is only recorded once the driver has accepted the
			 * ioctl that creates it, so the table is a narrowing of
			 * driver state and can never admit a range the driver
			 * would have refused.
			 */
			if (r == 0 && va_rmstatus == 0 && va_checked) {
				if (d->va_mode == NVKVM_UVM_VA_CREATE &&
				    !uvm_va_add(req->handle_id, va_base, va_len))
					fprintf(stderr,
						"nvkvm: WARN UVM VA table full — "
						"handle %u range 0x%llx+0x%llx "
						"untracked (U-6)\n",
						req->handle_id,
						(unsigned long long)va_base,
						(unsigned long long)va_len);
				else if (d->va_mode == NVKVM_UVM_VA_FREE)
					uvm_va_drop(req->handle_id, va_base, va_len);
			}
			if (req->cmd == 0x30000002 /* UVM_DEINITIALIZE */ && r == 0)
				nvkvm_uvm_va_purge_handle(req->handle_id);

			resp->retval     = (r < 0) ? (uint64_t)(int64_t)(-saved_errno) : 0;
			resp->status     = 0;
			resp->nvstatus   = st;
			resp->fault_addr = 0;
			return 0;
		}
	}

	/*
	 * ── R-1 ── bind the ioctl's _IOC_TYPE to the handle's REAL device
	 * BEFORE the 'd' / NVKMS / 'F' chain below makes its decision.  The
	 * rationale, the vendor-source verification and the full device→type
	 * mapping are on nvkvm_ioctl_type_matches_dev() above this function.
	 *
	 * Hard refusal (-EPERM), not the RM-shaped "succeed with a status in the
	 * params" signalling the alloc-class and A-1 gates use.  Those two sit
	 * on paths a legitimate workload reaches, where userspace treats a hard
	 * failure as fatal; a type/device mismatch is never legitimate, so there
	 * is nothing to degrade gracefully into.  Reject, never clamp.
	 */
	{
		struct nvkvm_handle *h =
			nvkvm_handle_get(&nv->handles, req->handle_id);

		if (!h || !nvkvm_ioctl_type_matches_dev(h->type, h->dev_id,
							req->cmd)) {
			unsigned ic_type = _IOC_TYPE(req->cmd);
			fprintf(stderr,
				"nvkvm: DENY ioctl cmd=0x%x type='%c' on handle %u "
				"(dev_id=%d, handle type=%d) — _IOC_TYPE does not "
				"match the handle's device (R-1)\n",
				req->cmd,
				(ic_type >= 0x20 && ic_type < 0x7f)
					? (char)ic_type : '?',
				req->handle_id,
				h ? h->dev_id : -1, h ? h->type : -1);
			resp->retval     = (uint64_t)(int64_t)(-EPERM);
			resp->status     = 0;
			resp->nvstatus   = 0x56; /* NV_ERR_NOT_SUPPORTED */
			resp->fault_addr = 0;
			return 0;
		}
	}

	/*
	 * M-A (audit 2026-05-30): default-deny any non-'F'-type cmd here.  UVM
	 * handles (type 0) already returned in the schema block above; every
	 * legitimate RM ioctl on nvidiactl/nvidia0 is _IOC_TYPE 'F'.  Without
	 * this, a guest crafting a cmd with a non-'F' type would skip ALL the
	 * frontend allowlists below (they all guard on type=='F') and fall
	 * straight through to the raw ioctl() in the stub — the kmd dispatches
	 * on _IOC_NR, so that could reach a denied privileged escape.
	 *
	 * NOTE (R-1): this arm sits AFTER the 'd' and NVKMS branches, so it only
	 * ever catches an UNRECOGNISED type; 'd' is recognised and never reaches
	 * it, which is why A-5's "reorder the deny" half would not have closed
	 * R-1 on its own.  What closes it is the type/dev_id cross-check above,
	 * which runs before this whole chain and has already established that
	 * the type matches the device.  This arm is now unreachable for a
	 * correctly-classified handle and is kept as belt-and-braces.
	 */
	/*
	 * NVKMS vblank-semaphore quota, set in the NVKMS branch below and acted
	 * on around the forwarded ioctl.  Declared out here because the
	 * reservation has to be taken before the ioctl and closed out after it,
	 * and both of those are well past the end of that branch.
	 */
	bool nvkms_vblank_enable  = false;
	bool nvkms_vblank_disable = false;

	/* Graphics gate (defense-in-depth; handle_open already blocks the device
	 * opens). Refuse all DRM ('d') and NVKMS ('m') ioctls on compute-only VMs. */
	if (!nv->graphics &&
	    (_IOC_TYPE(req->cmd) == 'd' || req->cmd == NVKVM_NVKMS_IOCTL_CMD)) {
		resp->retval     = (uint64_t)(int64_t)(-EPERM);
		resp->status     = 0;
		resp->nvstatus   = 0x56; /* NV_ERR_NOT_SUPPORTED */
		resp->fault_addr = 0;
		return 0;
	}

	if (_IOC_TYPE(req->cmd) == 'd') {
		/* nvidia-drm render node (graphics).  Default-deny: only the
		 * render/compute-relevant DRM ioctls are forwarded; display,
		 * modeset and permission surfaces are excluded.  Falls through to
		 * the generic forward path below (skips the 'F' frontend
		 * allowlists, which all guard on type=='F'). */
		if (!nvkvm_drm_nr_allowed(_IOC_NR(req->cmd))) {
			fprintf(stderr, "nvkvm: DENY drm ioctl nr=0x%02x\n",
				_IOC_NR(req->cmd));
			resp->retval     = (uint64_t)(int64_t)(-EACCES);
			resp->status     = 0;
			resp->nvstatus   = 0x56; /* NV_ERR_NOT_SUPPORTED */
			resp->fault_addr = 0;
			return 0;
		}
	} else if (req->cmd == NVKVM_NVKMS_IOCTL_CMD) {
		/* NVKMS (/dev/nvidia-modeset): the ONE allowed outer ioctl
		 * (_IOWR('m',0,NvKmsIoctlParams)).  Audit G-1: also default-deny
		 * on the INNER cmdType (wrapper {cmdType@0,size@4,address@8}) —
		 * otherwise a guest can drive any NVKMS command (incl. the
		 * cross-client GRANT/ACQUIRE/REVOKE_PERMISSIONS and GRANT_SURFACE
		 * verbs) on a host-global display device.  Allow only the
		 * cmdTypes a real Vulkan/EGL session issues; see
		 * nvkvm_nvkms_allowlist.h. */
		uint32_t nvkms_cmd = (param_buf && req->param_size >= 4)
			? *(const uint32_t *)param_buf : 0xffffffffu;
		uint32_t nvkms_sz = (param_buf && req->param_size >= 8)
			? *(const uint32_t *)((const char *)param_buf + 4) : 0;
		/* Key the gate on the host driver's MAJOR: NvKmsIoctlCommand is
		 * an unvalued enum that NVIDIA edits in the middle, so the same
		 * number names different commands on different branches.  The
		 * RM/UVM profile id cannot stand in for this -- the 570->575
		 * NVKMS renumbering falls inside the NVKVM_ABI_570 bucket. */
		unsigned nvkms_major = 0, nvkms_minor = 0, nvkms_patch = 0;
		nvkvm_abi_parse_version(nv->driver_version, &nvkms_major,
					&nvkms_minor, &nvkms_patch);
		bool nvkms_ok = nvkvm_nvkms_cmd_allowed_ver(nvkms_cmd, nvkms_major, nvkms_minor);
		if (getenv("NVKVM_NVKMS_TRACE"))
			fprintf(stderr, "nvkvm: nvkms cmdType=%u size=%u drv=%u %s\n",
				nvkms_cmd, nvkms_sz, nvkms_major,
				nvkms_ok ? "allow" : "DENY");
		if (!nvkms_ok) {
			fprintf(stderr,
				"nvkvm: DENY nvkms cmdType=%u size=%u (driver major %u)\n",
				nvkms_cmd, nvkms_sz, nvkms_major);
			resp->retval     = (uint64_t)(int64_t)(-EACCES);
			resp->status     = 0;
			resp->nvstatus   = 0x56; /* NV_ERR_NOT_SUPPORTED */
			resp->fault_addr = 0;
			return 0;
		}
		/* Which of the allowed cmdTypes are the vblank-semaphore
		 * enable/disable pair depends on the branch, for the same
		 * reason the gate itself does -- ask the same table. */
		struct nvkvm_nvkms_ops nvkms_ops =
			nvkvm_nvkms_ops_for_version(nvkms_major, nvkms_minor);
		if (nvkms_ops.known && nvkms_ops.vblank_enable >= 0) {
			nvkms_vblank_enable =
				(int32_t)nvkms_cmd == nvkms_ops.vblank_enable;
			nvkms_vblank_disable =
				(int32_t)nvkms_cmd == nvkms_ops.vblank_disable;
		}
	} else if (_IOC_TYPE(req->cmd) != 'F') {
		NVKVM_DBG("nvkvm: DENY non-'F' cmd 0x%x (type=0x%x)\n",
			  req->cmd, _IOC_TYPE(req->cmd));
		resp->retval     = (uint64_t)(int64_t)(-EPERM);
		resp->status     = 0;
		resp->nvstatus   = 0x56; /* NV_ERR_NOT_SUPPORTED */
		resp->fault_addr = 0;
		return 0;
	}

	/*
	 * REGISTER_FD now runs inside the isolate (stub) along with every
	 * other RM ioctl: the stub allocated the pClient (NV01_ROOT_CLIENT)
	 * when the gpu fd was opened, and rmclientValidate on the open
	 * driver compares pClient->pOSInfo with the calling task's nvfp.
	 * Running the ioctl from QEMU would fail strict validation. The
	 * stub translates ctl_fd from handle_id → its local fd before the
	 * kernel sees the ioctl (see fe_embedded_fd_off case 0xc9).
	 */

	/* DEBUG: dump structs at the QEMU layer right before forwarding
	 * to the isolate.  Only fd field should differ vs. guest's
	 * post-translate dump.  Same applies to NV01_EVENT_OS_EVENT. */
	if (_IOC_TYPE(req->cmd) == 'F' &&
	    (_IOC_NR(req->cmd) == 0xce || _IOC_NR(req->cmd) == 0xcf) &&
	    param_buf && req->param_size >= 16) {
		const uint8_t *p = param_buf;
		NVKVM_DBG(
			"nvkvm qemu pre 0x%x param[16]= "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x\n",
			_IOC_NR(req->cmd),
			p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
			p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
	}
	if (_IOC_TYPE(req->cmd) == 'F' && _IOC_NR(req->cmd) == 0x2b &&
	    param_buf && aux_buf &&
	    req->param_size >= 16 && req->aux_size >= 24) {
		uint32_t hclass;
		memcpy(&hclass, (char *)param_buf + 12, 4);
		if (hclass == 0x79) {
			const uint8_t *a = aux_buf;
			NVKVM_DBG(
				"nvkvm qemu pre 0x79 aux[24]= "
				"%02x %02x %02x %02x  %02x %02x %02x %02x "
				"%02x %02x %02x %02x  %02x %02x %02x %02x "
				"%02x %02x %02x %02x  %02x %02x %02x %02x\n",
				a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],
				a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],
				a[16],a[17],a[18],a[19],a[20],a[21],a[22],a[23]);
		}
	}

	/*
	 * #76b default-deny frontend-ioctl + alloc-class allowlists (nvproxy
	 * parity).  A 'F' ioctl whose NR is outside the known RM frontend set, or
	 * an RM_ALLOC of a class outside the permitted set, is refused before it
	 * reaches the host driver.  Host/cross-VM attack-surface control → QEMU.
	 */
	if (_IOC_TYPE(req->cmd) == 'F') {
		unsigned nr = _IOC_NR(req->cmd);
		if (!nvkvm_fe_nr_allowed(nr)) {
			fprintf(stderr, "nvkvm: DENY frontend ioctl nr=0x%02x\n", nr);
			resp->retval     = (uint64_t)(int64_t)(-EACCES);
			resp->status     = 0;
			resp->nvstatus   = 0x56; /* NV_ERR_NOT_SUPPORTED */
			resp->fault_addr = 0;
			return 0;
		}
		/*
		 * ── U-3 (docs/internal/audit-guest-pointers.md) ────────────
		 * NV_ESC_RM_VID_HEAP_CONTROL (nr 0x4a) carries NVOS32_PARAMETERS:
		 * a fixed prefix plus a 144-byte union selected by `function`
		 * (NvU32 at offset 8, verified against ogkm 575.51.03 nvos.h).
		 * nvkvm forwards the union opaquely.
		 *
		 * What an attacker could otherwise do: send
		 * function == NVOS32_FUNCTION_ALLOC_OS_DESCRIPTOR (27).  The
		 * driver then reads data.AllocOsDesc.descriptor (an NvP64 inside
		 * that union) and hands it straight to os_lock_user_pages()
		 * i.e. pin_user_pages(), with data.AllocOsDesc.limit as the
		 * length — pinning an arbitrary attacker-named address range in
		 * the isolate's address space, with the driver's only checks
		 * being page alignment and a limit+1 overflow test
		 * (escape.c:162, :142-150).  function == 19 (HW_ALLOC) likewise
		 * exposes data.HwAlloc.bindResultFunc and .pHandle, two further
		 * NvP64s (nvos.h:832-833).
		 *
		 * The guest declines to sanitise NVOS32 (src/guest/nvkvm_ioctl.c
		 * :456-460) on the premise that "the ALLOC_SIZE path has no
		 * embedded input pointer".  That premise is true only for
		 * function == 2, and the guest is untrusted anyway — so the
		 * constraint has to be enforced here.  gVisor's nvproxy takes
		 * exactly this position (rmVidHeapControl rejects every NVOS32
		 * whose Function != NVOS32_FUNCTION_ALLOC_SIZE).
		 *
		 * Default-deny, matching the frontend-NR / alloc-class /
		 * control-cmd allowlists around it.  Allowed:
		 *   2  NVOS32_FUNCTION_ALLOC_SIZE — the only NVOS32 function the
		 *      working stack uses.  It is the legacy heap allocation
		 *      libGLX_nvidia issues (src/abi/nvgpu.h:299-312); its union
		 *      arm (data.AllocSize) contains no input pointer at all —
		 *      `address` is [OUT] only.  Measured on this tree: a full
		 *      compute run (nvidia-smi + cuInit/cuCtxCreate + 8 MiB
		 *      HtoD/DtoH + a kernel launch) issues ZERO nr-0x4a ioctls,
		 *      so this allowance is the graphics path's, kept because
		 *      removing it would silently regress libGLX.
		 * Everything else — 3 FREE, 5 INFO, 6, 14, 15, 16, 18, 19
		 * HW_ALLOC, 20 HW_FREE, 27 ALLOC_OS_DESCRIPTOR and any value
		 * the driver may add — is refused.
		 *
		 * NOT affected: U-14's deliberate OS-descriptor path.  That one
		 * is NV_ESC_RM_ALLOC_MEMORY (nr 0x27) with
		 * hClass == NV01_MEMORY_SYSTEM_OS_DESCRIPTOR (0x71), where the
		 * guest migrates the range onto memfds the stub MAP_FIXEDs at
		 * the same VA.  Different ioctl, different struct, untouched by
		 * this gate.
		 */
		if (nr == 0x4a) {
			uint32_t fn = 0xffffffffu;
			if (param_buf && req->param_size >= 12)
				memcpy(&fn, (char *)param_buf + 8, 4);
			if (fn != 2 /* NVOS32_FUNCTION_ALLOC_SIZE */) {
				fprintf(stderr,
					"nvkvm: DENY RM_VID_HEAP_CONTROL "
					"function=%u (only ALLOC_SIZE=2 "
					"allowed, U-3)\n", fn);
				resp->retval     = (uint64_t)(int64_t)(-EACCES);
				resp->status     = 0;
				resp->nvstatus   = 0x56; /* NV_ERR_NOT_SUPPORTED */
				resp->fault_addr = 0;
				return 0;
			}
		}

		/* RM_ALLOC (nvos21/nvos64): hClass at param+12 (shared prefix).
		 *
		 * S-5: the short-buffer case must land in the DENY arm, not skip
		 * the gate.  It used to be a condition on entering the gate at
		 * all, so a guest that declared param_size < 16 was never asked
		 * which class it wanted — and the stub then zero-pads the buffer
		 * out to _IOC_SIZE (deliberately, G-8) and forwards it, so the
		 * driver saw a full, well-formed alloc that the allowlist never
		 * looked at.  Default to the sentinel the allowlist cannot
		 * contain instead, exactly as the VID_HEAP gate above and the
		 * NVKMS gate do: a param too short to hold the discriminator is
		 * a request we cannot classify, and unclassifiable is denied. */
		/*
		 * ── A-1 ── NV_ESC_RM_ALLOC_MEMORY (nr 0x27) with
		 * hClass == NV01_MEMORY_SYSTEM_OS_DESCRIPTOR (0x71).
		 *
		 * That class hands `pMemory` straight to os_lock_user_pages()
		 * i.e. pin_user_pages() ON THE CALLING TASK -- which is the
		 * stub.  An unvalidated pMemory therefore pins an arbitrary
		 * range of the STUB's address space (its heap, stack,
		 * libraries), and nr 0x4e can then map it back to the guest:
		 * a read/write window into a host process running as the same
		 * uid as QEMU, separated only by namespaces and seccomp.
		 *
		 * Why the existing gates did not catch it:
		 *   - the alloc-class allowlist (which DOES exclude 0x71,
		 *     nvkvm_fe_alloc_allowlist.h:12-15, deliberately and
		 *     following nvproxy) is applied under `if (nr == 0x2b)`
		 *     below -- 0x2b is NV_ESC_RM_ALLOC, a different ioctl.
		 *   - nvkvm_dispatch.c's `p->p_memory = 0` looks like the
		 *     sanitisation but is dead code: its only caller,
		 *     handle_ioctl(), had no callers of its own.  It is removed
		 *     in this change rather than left as a decoy.
		 *   - H-3's hClient-ownership gate applies, but is satisfied by
		 *     any legitimate prior allocation.
		 *
		 * Straight default-deny of 0x71 is not available: U-14's
		 * OS-descriptor path is a live feature (host registration /
		 * cudaHostRegister).  So this takes U-6's shape instead -- an
		 * ownership test against ranges THE HOST ITSELF established in
		 * that isolate.  The guest's own migration path installs every
		 * legitimate range through mmap_on_isolate, so the host already
		 * knows them all; anything else is refused before the ioctl is
		 * forwarded, never clamped.
		 *
		 * Unclassifiable is denied, per S-5: a param too short to hold
		 * the class discriminator, or too short to hold the range when
		 * the class IS 0x71, is a request we cannot check.
		 */
		if (nr == 0x27) {
			uint32_t cls = 0xffffffffu;
			bool     classifiable = param_buf && req->param_size >= 16;

			if (classifiable)
				memcpy(&cls, (char *)param_buf + 12, 4);

			if (!classifiable || cls == 0x71) {
				uint64_t base = 0, limit = 0, stub_base = 0;
				bool     ok   = false;

				if (classifiable && cls == 0x71 && param_buf &&
				    req->param_size >= 40) {
					memcpy(&base,  (char *)param_buf + 24, 8);
					memcpy(&limit, (char *)param_buf + 32, 8);
					/* `limit` is size-1 (the guest passes
					 * limit+1 as the length, see
					 * nvkvm_ioctl.c:428). */
					/* `limit` is size-1 and is guest-supplied,
					 * so guard the +1 before using it; the
					 * base+len wrap is checked inside
					 * iso_mmap_translate(). */
					ok = base && limit != UINT64_MAX &&
					     iso_mmap_translate(req->isolate_id,
								base, limit + 1,
								&stub_base);
				}

				if (!ok) {
					fprintf(stderr,
						"nvkvm: DENY OS_DESCRIPTOR "
						"0x%llx+0x%llx not host-installed "
						"in isolate %u (A-1)\n",
						(unsigned long long)base,
						(unsigned long long)(limit + 1),
						req->isolate_id);
					/* Same signalling shape as the alloc-class
					 * refusal below: the ioctl succeeds and
					 * the status field carries the error, which
					 * is what RM itself does.  NV_ERR_INVALID_-
					 * ADDRESS is what the driver returns for a
					 * descriptor it cannot pin. */
					/*
					 * Write the refusal into the params
					 * too, not just resp->nvstatus.
					 * MEASURED: with only resp->nvstatus
					 * set, the guest returned rc=0 and left
					 * NVOS02.status at 0, so a caller saw a
					 * SUCCESSFUL allocation and would go on
					 * to use an hMemory that was never
					 * created.  Unlike nr 0x4a, this ioctl's
					 * status field is how RM reports errors
					 * on this path, and the guest does not
					 * synthesise it from nvstatus.  A
					 * security refusal that reads as success
					 * is worse than no refusal.
					 */
					if (param_buf && req->param_size >=
					    offsetof(struct nv_ioctl_nvos02_parameters_with_fd,
						     status) + 4) {
						uint32_t st = 0x1e;
						memcpy((char *)param_buf +
						       offsetof(struct nv_ioctl_nvos02_parameters_with_fd,
								status),
						       &st, 4);
					}
					resp->retval     = 0;
					resp->status     = 0;
					resp->nvstatus   = 0x1e;
					resp->fault_addr = 0;
					return 0;
				}

				/*
				 * U-14 stops being an exception here.
				 *
				 * This pointer used to be forwarded verbatim,
				 * on purpose: the stub MAP_FIXED'd the migrated
				 * memfds at the guest's own VA, so
				 * RmAllocOsDescriptor -> pin_user_pages() on
				 * the stub's task found pages aliasing guest
				 * userspace.  That made NVOS02.pMemory the one
				 * guest pointer nvkvm knowingly handed to the
				 * host driver, and ARCHITECTURE.md's invariant
				 * had to be read with an asterisk.
				 *
				 * With the window, the chunks live at an
				 * address the HOST chose, so the number RM
				 * dereferences is now ours.  pMemory is IN-only
				 * for this class — the guest gets back an
				 * hMemory, never this field — so rewriting it
				 * is invisible to the guest, and the physical
				 * aliasing that makes the feature work is
				 * untouched: same memfds, same pages, only a
				 * different virtual address in a process the
				 * guest cannot see.
				 *
				 * iso_mmap_translate() has already established
				 * that [base, base+limit+1) is spanned by live
				 * mirrored entries of THIS isolate AND that
				 * they are contiguous in the window, so
				 * stub_base names one pinnable range.
				 */
				memcpy((char *)param_buf + 24, &stub_base, 8);
				NVKVM_DBG("nvkvm: OS_DESCRIPTOR gva=0x%llx -> "
					  "window 0x%llx (+0x%llx) iso=%u\n",
					  (unsigned long long)base,
					  (unsigned long long)stub_base,
					  (unsigned long long)(limit + 1),
					  req->isolate_id);
			}
		}

		if (nr == 0x2b) {
			uint32_t cls = 0xffffffffu;
			if (param_buf && req->param_size >= 16)
				memcpy(&cls, (char *)param_buf + 12, 4);
			if (!nvkvm_alloc_class_allowed(cls)) {
				fprintf(stderr, "nvkvm: DENY alloc class 0x%08x\n", cls);
				/*
				 * Report the refusal the way RM itself reports an
				 * unsupported class: the ioctl SUCCEEDS and the
				 * status field carries NV_ERR_NOT_SUPPORTED.
				 * Failing the ioctl outright (-EACCES) is a
				 * different, far more severe signal than anything
				 * the real driver produces, and userspace treats it
				 * as fatal rather than falling back -- on an H100
				 * that turned a denied NVA083_GRID_DISPLAYLESS into
				 * vkCreateDevice returning VK_ERROR_DEVICE_LOST.
				 * The allocation is still never forwarded; only the
				 * error signalling changes.
				 */
				resp->retval     = 0;
				resp->status     = 0;
				resp->nvstatus   = 0x56; /* NV_ERR_NOT_SUPPORTED */
				resp->fault_addr = 0;
				return 0;
			}
		}
	}

	/*
	 * #76 default-deny RM control-command allowlist (nvproxy parity).  Reject
	 * any control cmd outside the CUDA-compute surface before it reaches the
	 * host driver — closes reg-ops / HWPM / debug / fabric / power surfaces a
	 * guest could otherwise drive on any client it owns.  Also bound the inner
	 * params size (1 MiB) as nvproxy does (our 64K slots already cap it, but be
	 * explicit).
	 */
	/* S-5: as for RM_ALLOC above — `param_size >= 12` gated ENTRY to the
	 * check, so a short param_size skipped the only allowlist on this path
	 * entirely (the stub has none on the socket path, and zero-pads to
	 * _IOC_SIZE before forwarding).  The size test now selects the sentinel
	 * command instead of selecting whether to test at all; 0xffffffff is not
	 * a real RM control cmd and cannot be in the allowlist, so it denies. */
	if (_IOC_TYPE(req->cmd) == 'F' && _IOC_NR(req->cmd) == NV_ESC_RM_CONTROL) {
		uint32_t cc = 0xffffffffu;
		if (param_buf && req->param_size >= 12)
			memcpy(&cc, (char *)param_buf + 8, 4);
		if (!nvkvm_ctrl_cmd_allowed(cc) || req->aux_size > (1u << 20)) {
			fprintf(stderr, "nvkvm: DENY ctrl cmd 0x%08x "
				"(not in allowlist / oversize)\n", cc);
			/* As above: RM answers an unsupported control with a
			 * successful ioctl carrying NV_ERR_NOT_SUPPORTED, not
			 * with a failed ioctl.  The command is still never
			 * forwarded. */
			resp->retval     = 0;
			resp->status     = 0;
			resp->nvstatus   = 0x56; /* NV_ERR_NOT_SUPPORTED */
			resp->fault_addr = 0;
			return 0;
		}
	}

	/*
	 * Phase 4 gate — DUP_OBJECT cross-VM defense.  NVOS55 (verified 28-byte
	 * layout): h_client@0, h_parent@4, h_object@8, h_client_src@12,
	 * h_src_object@16.  The source client must be one THIS VM allocated;
	 * otherwise a guest (whose objects carry the Path-α TYPE_ALL DUP grant)
	 * could dup another VM's object by naming its (h_client_src,
	 * h_src_object).  h_client itself is the caller's own client (recorded
	 * post-success below), so we only need to vet the src.
	 */
	/*
	 * S-5: same shape as the two gates above — a param_size < 16 used to
	 * skip this gate outright, and the stub's zero-padding then handed the
	 * driver a complete NVOS55 the gate never inspected.  This one cannot
	 * use a sentinel: 0xffffffff is an ALLOWED value here (it means "no
	 * source client"), so a too-short param is refused explicitly instead.
	 */
	if (_IOC_TYPE(req->cmd) == 'F' &&
	    _IOC_NR(req->cmd) == NV_ESC_RM_DUP_OBJECT) {
		uint32_t h_client_src = 0;
		bool short_param = (!param_buf || req->param_size < 16);
		if (!short_param)
			memcpy(&h_client_src, (char *)param_buf + 12, 4);
		if (short_param ||
		    (h_client_src != 0 && h_client_src != (uint32_t)-1 &&
		     !nvkvm_client_allow_has(nv, h_client_src))) {
			NVKVM_DBG(
				"nvkvm: DENY DUP_OBJECT %s h_client_src=0x%x "
				"(not a client of this VM)\n",
				short_param ? "unreadable" : "foreign",
				h_client_src);
			resp->retval     = (uint64_t)(int64_t)(-EACCES);
			resp->status     = 0;
			resp->nvstatus   = 0x1f; /* NV_ERR_INVALID_ARGUMENT */
			resp->fault_addr = 0;
			return 0;
		}
	}

	/*
	 * Audit H-3: make the per-VM hClient allowlist authoritative for EVERY
	 * forwarded RM 'F' ioctl that carries an hClient at param offset 0
	 * (ALLOC/ALLOC_MEMORY/CONTROL/FREE/DUP/SHARE/MAP[_DMA]/UNMAP[_DMA]/
	 * VID_HEAP_CONTROL), not just DUP_OBJECT's source.  A guest must not be
	 * able to make the privileged stub name another VM's RM client.  Every
	 * client this VM creates is recorded at its root-client alloc (above), so
	 * a legitimate reference is always in the set; the only exemption is that
	 * client-creating alloc itself (param[0]=h_root=0, new client in hObjNew).
	 * Layered defense-in-depth on top of eliminating host-wide TYPE_ALL (H-2).
	 */
	if (_IOC_TYPE(req->cmd) == 'F' && nv->client_allow_n > 0) {
		unsigned nr = _IOC_NR(req->cmd);
		int hclient_at_0 =
			nr == NV_ESC_RM_ALLOC || nr == NV_ESC_RM_ALLOC_MEMORY ||
			nr == NV_ESC_RM_CONTROL || nr == NV_ESC_RM_FREE ||
			nr == NV_ESC_RM_DUP_OBJECT || nr == 0x35 /* SHARE */ ||
			nr == 0x4e /* MAP_MEMORY */ || nr == 0x4f /* UNMAP_MEMORY */ ||
			nr == 0x57 /* MAP_MEMORY_DMA */ ||
			nr == 0x58 /* UNMAP_MEMORY_DMA */ ||
			nr == 0x4a /* VID_HEAP_CONTROL */;
		if (hclient_at_0) {
			uint32_t hc = 0;
			/*
			 * Fail CLOSED on a param too short to hold the hClient.
			 * The `param_size >= 4` test used to sit in the outer
			 * condition, so a guest declaring a shorter param skipped
			 * this gate entirely rather than being refused by it —
			 * the same fail-open shape as the ALLOC-class and
			 * RM_CONTROL gates above (and the stub, which zero-pads
			 * up to _IOC_SIZE, would then have forwarded it).  Every
			 * NR listed here takes an hClient at offset 0 by
			 * definition, so a request that cannot contain one is
			 * malformed, not exempt.
			 */
			if (!param_buf || req->param_size < 4) {
				NVKVM_DBG(
					"nvkvm: DENY ioctl NR=0x%x param_size=%u "
					"too short for hClient\n",
					nr, (unsigned)req->param_size);
				resp->retval     = (uint64_t)(int64_t)(-EACCES);
				resp->status     = 0;
				resp->nvstatus   = 0x1f; /* NV_ERR_INVALID_ARGUMENT */
				resp->fault_addr = 0;
				return 0;
			}
			memcpy(&hc, (char *)param_buf, 4);
			int is_root_alloc = 0;
			if (nr == NV_ESC_RM_ALLOC && req->param_size >= 16) {
				uint32_t hObjNew = 0, hClass = 0;
				memcpy(&hObjNew, (char *)param_buf + 8, 4);
				memcpy(&hClass,  (char *)param_buf + 12, 4);
				is_root_alloc = (hObjNew == hc &&
						 (hClass == 0 || hClass == 0x41)) ||
						(hc == 0 &&
						 (hClass == 0 || hClass == 0x41));
			}
			if (!is_root_alloc && hc != 0 && hc != (uint32_t)-1 &&
			    !nvkvm_client_allow_has(nv, hc)) {
				NVKVM_DBG(
					"nvkvm: DENY ioctl NR=0x%x foreign hClient=0x%x "
					"(not a client of this VM)\n", nr, hc);
				resp->retval     = (uint64_t)(int64_t)(-EACCES);
				resp->status     = 0;
				resp->nvstatus   = 0x1f; /* NV_ERR_INVALID_ARGUMENT */
				resp->fault_addr = 0;
				return 0;
			}
		}
	}

	/*
	 * GET_PID_INFO (#66): NV2080_CTRL_CMD_GPU_GET_PID_INFO (inner cmd
	 * 0x2080018e) carries pidInfoList[] in aux_buf (count@0, 56-byte entries
	 * @8, pid@+0).  The guest tagged each pid it owns with 0x80000000|
	 * isolate_id (ns-filtered).  We validate the isolate belongs to THIS VM and
	 * write its stub-process tgid into the pid field so the baseline forward
	 * resolves a known process (NV_OK).  Because nvidia attributes vidmem to the
	 * worker *tid* that owns each RM client — and our pool spreads ownership
	 * across the stub's worker tids — the tgid query returns 0 bytes; we fix
	 * that up post-forward by summing per-tid (nvkvm_get_pid_info_sum).  QEMU
	 * thus validates pids against managed isolates: a guest can never make the
	 * privileged stub query an arbitrary host pid.  The guest restores its own
	 * pids into the response, so nvidia-smi sees its pid + the real usage.
	 */
	bool     gpi_active = false;
	uint32_t gpi_count  = 0;
	static __thread uint32_t gpi_iso[200];   /* per-entry isolate (0 = skip) */
	if (_IOC_TYPE(req->cmd) == 'F' &&
	    _IOC_NR(req->cmd) == NV_ESC_RM_CONTROL &&
	    param_buf && req->param_size >= 12 &&
	    aux_buf && req->aux_size >= 8 + NVKVM_PIDINFO_STRIDE) {
		uint32_t icmd = 0;
		memcpy(&icmd, (char *)param_buf + 8, 4);
		if (icmd == 0x2080018eu) {        /* NV2080_CTRL_CMD_GPU_GET_PID_INFO */
			uint32_t count = 0;
			memcpy(&count, aux_buf, 4);
			if (count > 200)
				count = 200;
			gpi_active = true;
			gpi_count  = count;
			for (uint32_t i = 0; i < count; i++) {
				uint32_t off = 8 + i * NVKVM_PIDINFO_STRIDE;
				uint32_t v = 0, repl = 0;
				gpi_iso[i] = 0;
				/* Require the FULL 72-byte entry to fit: the
				 * post-forward fixup writes result@off+8 and
				 * sum@off+16 (out to off+24).  Validating only
				 * off+4 here let a guest (aux_size=84,count=2)
				 * drive a ~20-byte OOB write in QEMU (audit H-A). */
				if ((uint64_t)off + NVKVM_PIDINFO_STRIDE >
				    req->aux_size) {
					gpi_count = i;
					break;
				}
				memcpy(&v, (char *)aux_buf + off, 4);
				if (v & 0x80000000u) {
					uint32_t iso = v & 0x7fffffffu;
					pid_t hp = nvkvm_isolate_host_pid(
						&nv->isolates, iso);
					if (hp > 0) {
						repl = (uint32_t)hp;
						gpi_iso[i] = iso;
					}
				}
				memcpy((char *)aux_buf + off, &repl, 4);
			}
		}
	}

	uint32_t nvstatus  = 0;
	uint64_t fault_addr = 0;
	/*
	 * UNMAP: put the remembered VA back before the driver sees it.  NVOS34 is
	 * {h_client, h_device, h_memory, reserved0, p_linear_address@16, ...}.
	 */
	if (_IOC_TYPE(req->cmd) == 'F' && _IOC_NR(req->cmd) == 0x4f &&
	    param_buf && req->param_size >= 24) {
		const uint32_t *w = (const uint32_t *)param_buf;
		uint64_t va = nvkvm_mapva_take(req->isolate_id, w[0], w[1], w[2]);
		if (va)
			memcpy((char *)param_buf + 16, &va, 8);
		/* No entry: leave the guest's zero.  Failing closed here keeps a
		 * forged triple from unmapping something we never mapped. */
	}

	/*
	 * Cross-isolate RM export/import: if this control names a handle whose
	 * fd lives in another isolate's stub, relay it in before forwarding.
	 */
	nvkvm_xrm_prepare(nv, req, param_buf, aux_buf);

	/*
	 * A successful ENABLE_VBLANK_SEM_CONTROL allocates a persistent host
	 * kernel object, pins and maps the semaphore surface, and adds work the
	 * host does on every physical vblank.  Nothing bounded how many of those
	 * a VM could accumulate.  Reserve VM-wide capacity BEFORE forwarding, so
	 * concurrent workers cannot all read the same under-limit count and all
	 * then succeed.
	 */
	uint64_t nvkms_vblank_reservation = 0;
	if (nvkms_vblank_enable) {
		nvkms_vblank_reservation = nvkvm_isolate_nvkms_vblank_reserve(
			&nv->isolates, req->isolate_id, req->handle_id);
		if (nvkms_vblank_reservation == 0) {
			resp->retval     = (uint64_t)(int64_t)(-ENOSPC);
			resp->status     = 0;
			resp->nvstatus   = 0;
			resp->fault_addr = 0;
			return 0;
		}
	}

	int ret = nvkvm_isolate_ioctl(&nv->isolates,
				      req->isolate_id,
				      req->handle_id,
				      req->cmd,
				      param_buf, req->param_size,
				      aux_buf,   req->aux_size,
				      req->flags,
				      &nvstatus,
				      &fault_addr);

	/*
	 * Close out the reservation.  The offsets are the inner NVKMS params
	 * block (aux_buf), read from the vendor header at every tag where these
	 * commands exist -- 550.54.14, 565.57.01, 570.195.03, 570.207,
	 * 575.51.03, 580.178.04, 590.48.01, 610.43.02:
	 *
	 *   Enable  {device@0, disp@4, ... , reply.vblankSemControlHandle@24}
	 *   Disable {device@0, disp@4, vblankSemControlHandle@8}
	 *
	 * The 550..570 Enable request carries an extra headMask before
	 * surfaceHandle; that fills the alignment hole ahead of the 8-byte
	 * surfaceOffset, so the request is 24 bytes and the reply sits at 24 in
	 * every branch.  nvKmsIoctl requires the exact per-command parameter
	 * size, so a short block cannot have succeeded -- the size guards below
	 * are belt-and-braces on our own read, not on the driver's.
	 */
	if (nvkms_vblank_enable) {
		uint32_t device = 0, disp = 0, control = 0;
		bool ok = ret == 0 && aux_buf && req->aux_size >= 28;
		if (ok) {
			memcpy(&device,  (char *)aux_buf + 0,  4);
			memcpy(&disp,    (char *)aux_buf + 4,  4);
			memcpy(&control, (char *)aux_buf + 24, 4);
		}
		nvkvm_isolate_nvkms_vblank_finish(
			&nv->isolates, req->isolate_id, nvkms_vblank_reservation,
			ok, device, disp, control);
	} else if (nvkms_vblank_disable && ret == 0 && aux_buf &&
		   req->aux_size >= 12) {
		uint32_t device, disp, control;
		memcpy(&device,  (char *)aux_buf + 0, 4);
		memcpy(&disp,    (char *)aux_buf + 4, 4);
		memcpy(&control, (char *)aux_buf + 8, 4);
		nvkvm_isolate_nvkms_vblank_retire(
			&nv->isolates, req->isolate_id, req->handle_id,
			device, disp, control);
	}

	/*
	 * MAP: on success the driver wrote the isolate-side VA into
	 * NVOS33.p_linear_address@32.  Remember it for the matching unmap.
	 */
	if (ret == 0 && nvstatus == 0 && _IOC_TYPE(req->cmd) == 'F' &&
	    _IOC_NR(req->cmd) == 0x4e && param_buf && req->param_size >= 40) {
		const uint32_t *w = (const uint32_t *)param_buf;
		uint64_t va;
		memcpy(&va, (const char *)param_buf + 32, 8);
		nvkvm_mapva_record(req->isolate_id, w[0], w[1], w[2], va);
	}

	resp->retval     = (ret < 0) ? (uint64_t)(int64_t)ret : (uint64_t)ret;
	resp->status     = (ret == -EFAULT && fault_addr) ? EFAULT : 0;
	resp->nvstatus   = nvstatus;
	resp->fault_addr = fault_addr;

	/*
	 * GET_PID_INFO fixup (#66): the baseline forward queried each entry's stub
	 * tgid and got NV_OK with 0 bytes (vidmem is tid-attributed).  For every
	 * entry that mapped to a validated isolate, sum the metric across that
	 * isolate's worker tids and overwrite the entry's data union, marking it
	 * NV_OK so nvidia-smi reports the real per-process usage.
	 */
	if (gpi_active && ret == 0) {
		for (uint32_t i = 0; i < gpi_count; i++) {
			if (gpi_iso[i] == 0)
				continue;
			uint32_t off = 8 + i * NVKVM_PIDINFO_STRIDE;
			pid_t tgid = nvkvm_isolate_host_pid(&nv->isolates,
							    gpi_iso[i]);
			if (tgid <= 0)
				continue;
			uint32_t index = 0;
			memcpy(&index, (char *)aux_buf + off + 4, 4);
			int any = 0;
			/* #66: query from QEMU's init-ns admin subdevice — the
			 * stub's pid-ns caller-context attributes 0 bytes. */
			uint64_t sum = nvkvm_admin_get_pid_mem(nv, tgid, index, &any);
			uint32_t result = any ? 0u : 0xffffu; /* NV_OK / NOT_FOUND */
			memcpy((char *)aux_buf + off + 8, &result, 4);
			memcpy((char *)aux_buf + off + 16, &sum, 8);
		}
	}

	/*
	 * Phase 4 — record this VM's RM client handles.  Every successful 'F'
	 * RM ioctl carries the owning hClient at param offset 0; recording it
	 * builds the per-VM allowlist the DUP_OBJECT gate above consults.  (A
	 * client this VM uses successfully is, by definition, this VM's.)
	 */
	if (ret == 0 && nvstatus == 0 &&
	    _IOC_TYPE(req->cmd) == 'F' && param_buf && req->param_size >= 4) {
		/* Only the NVOSxx structs whose first field is hClient: ALLOC,
		 * ALLOC_MEMORY, CONTROL, FREE, DUP_OBJECT, SHARE.  Every RM
		 * client performs at least an ALLOC, so this captures them all
		 * without recording stray words from header-less ioctls. */
		unsigned nr = _IOC_NR(req->cmd);
		if (nr == NV_ESC_RM_ALLOC || nr == NV_ESC_RM_ALLOC_MEMORY ||
		    nr == NV_ESC_RM_CONTROL || nr == NV_ESC_RM_FREE ||
		    nr == NV_ESC_RM_DUP_OBJECT || nr == 0x35 /* RM_SHARE */) {
			uint32_t hc = 0;
			memcpy(&hc, (char *)param_buf, 4);
			nvkvm_client_allow_add(nv, hc);   /* owning client (param[0]) */
			/* A root-client alloc (NV01_ROOT class 0, or 0x41
			 * NV01_ROOT_CLIENT) creates a NEW client whose
			 * kernel-assigned handle is written back to hObjNew
			 * (param[8]); h_root (param[0]) is 0 for that alloc.
			 * Record the new client now so the H-3 gate accepts the
			 * client's very first subsequent ioctl (which references
			 * it at param[0]). */
			if (nr == NV_ESC_RM_ALLOC && req->param_size >= 16) {
				uint32_t hObjNew = 0, hClass = 0;
				memcpy(&hObjNew, (char *)param_buf + 8, 4);
				memcpy(&hClass,  (char *)param_buf + 12, 4);
				if (hClass == 0 || hClass == 0x41)
					nvkvm_client_allow_add(nv, hObjNew);
			}
		}
	}

	/*
	 * Path α — explicit DUP_OBJECT grant for cross-process duplication.
	 *
	 * The kernel's default share policy is `RS_SHARE_TYPE_PID` which
	 * grants DUP_OBJECT only when the calling task's PID matches the
	 * resource owner's ProcID.  In our split-process model (libcuda's
	 * RM client allocated by the stub, UVM ioctls called from QEMU)
	 * the PIDs don't match, so UVM's kernel-internal client can't dup
	 * libcuda's VA space → NV_ERR_INSUFFICIENT_PERMISSIONS.
	 *
	 * Fix: right after the stub successfully allocates a class that we
	 * know UVM will need to dup, issue an NV_ESC_RM_SHARE on the new
	 * handle granting DUP_OBJECT scoped to QEMU's pid (RS_SHARE_TYPE_PID,
	 * see the share initializer below — was TYPE_ALL, host-wide, which let
	 * any host process dup a guessed handle).  The share runs on the stub
	 * fd so the owner check inside _serverShareResource matches (caller
	 * process == resource owner).
	 *
	 * Classes we share: FERMI_VASPACE_A (0x90f1) for now; add others as
	 * we hit further duplications.
	 */
	if (ret == 0 && nvstatus == 0 &&
	    _IOC_TYPE(req->cmd) == 'F' &&
	    (_IOC_NR(req->cmd) == NV_ESC_RM_ALLOC ||
	     _IOC_NR(req->cmd) == NV_ESC_RM_ALLOC_MEMORY) &&
	    param_buf && req->param_size >= 16) {
		uint32_t hClient = 0, hObjNew = 0, hClass = 0;
		memcpy(&hClient, (char *)param_buf +  0, sizeof(uint32_t));
		memcpy(&hObjNew, (char *)param_buf +  8, sizeof(uint32_t));
		if (_IOC_NR(req->cmd) == NV_ESC_RM_ALLOC) {
			memcpy(&hClass,  (char *)param_buf + 12, sizeof(uint32_t));
		} else {
			/*
			 * NVOS02 (nr 0x27) carries hClass at the same offset as
			 * NVOS21/NVOS64 -- h_root@0, h_object_parent@4,
			 * h_object_new@8, h_class@12 (src/abi/nvgpu.h,
			 * nv_ioctl_nvos02_parameters_with_fd) -- so read it,
			 * rather than asserting a class.
			 *
			 * This used to hardcode 0x40 with the comment
			 * "RM_ALLOC_MEMORY always allocates
			 * NV01_MEMORY_LOCAL_USER".  That is not true: the guest
			 * chooses the class, and 0x71 (OS descriptor) is exactly
			 * the case A-1 above exists for.  The hardcode was
			 * benign -- hClass only selects whether to skip the
			 * DUP_OBJECT grant for RM client objects, and every
			 * other class gets the grant regardless -- but it
			 * asserted something the code contradicted, which is the
			 * kind of false statement that hid A-1.
			 */
			memcpy(&hClass, (char *)param_buf + 12, sizeof(uint32_t));
		}

		/* Grant DUP_OBJECT on every successful RM_ALLOC.  UVM duplicates
		 * VA spaces (0x90f1), memory objects (0x40 = NV01_MEMORY_LOCAL_-
		 * USER), channels, and more — granting universally is simpler
		 * than maintaining a class allowlist, and harmless: the share
		 * only adds DUP_OBJECT, which RM-allocated resources have for
		 * their owner anyway.  Skip RM client objects (hObjNew == hClient
		 * AND hClass == NV01_ROOT_CLIENT) since rmapiAllocClient already
		 * REVOKEs DUP from TYPE_ALL on those for security. */
		int is_client_obj = (hObjNew == hClient && hClass == 0x0);
		int needs_share = !is_client_obj && hClass != 0;

		if (needs_share && hClient && hObjNew) {
			/* NVOS57_PARAMETERS layout (24 bytes):
			 *   u32 hClient
			 *   u32 hObject
			 *   u32 sharePolicy.target
			 *   u32 sharePolicy.accessMask (1 limb)
			 *   u16 sharePolicy.type
			 *   u8  sharePolicy.action
			 *   u8  pad
			 *   u32 status
			 * cmd = _IOWR('F', NV_ESC_RM_SHARE=0x35, 24) = 0xc0184635.
			 */
			struct {
				uint32_t hClient;
				uint32_t hObject;
				uint32_t target;
				uint32_t accessMask;
				uint16_t type;
				uint8_t  action;
				uint8_t  _pad;
				uint32_t status;
			} share = {
				.hClient    = hClient,
				.hObject    = hObjNew,
				/*
				 * Grant RS_ACCESS_DUP_OBJECT so UVM (the legitimate
				 * consumer, running in QEMU/the isolate) can dup this
				 * VA-space/memory object during cuCtxCreate's UVM map.
				 *
				 * Share type = RS_SHARE_TYPE_ALL.  This is NOT a
				 * cross-tenant hole: cross-VM/host containment comes
				 * from the handle NAMESPACE (reach-gating), not the
				 * share type.  A foreign client cannot RESOLVE another
				 * client's object — the dup fails at
				 * clientGetResourceRef (NV_ERR_OBJECT_NOT_FOUND, 0x57)
				 * BEFORE the share policy is consulted.  Proven by
				 * tests/security/poc_cross_proc_dup: an unprivileged
				 * host neighbour, with a valid device parent, naming
				 * the exact live (hClientSrc,hObjectSrc) of a guest
				 * VRAM object, is denied 0x57 EVEN UNDER TYPE_ALL —
				 * i.e. even when ALL grants it the DUP right, it still
				 * can't reach the object.  So the right is irrelevant
				 * to neighbours; only legitimate consumers can reach.
				 *
				 * This replaces the former TYPE_CLIENT(0xc1d00001) grant
				 * (H-2), which depended on a hardcoded "UVM is the first
				 * RM client" assumption that broke on any reboot/init-
				 * order change (stale handle → SHARE 0x33 → cuCtxCreate
				 * 800 → all GPU tests blocked).  H-2 guarded a
				 * theoretical hole the reach-gate already closes. */
				.target     = 0,     /* unused for TYPE_ALL */
				.accessMask = 0x1,   /* RS_ACCESS_DUP_OBJECT */
				.type       = 1,     /* RS_SHARE_TYPE_ALL */
				.action     = 0,     /* grant (no REVOKE/REQUIRE/COMPOSE) */
				.status     = 0,
			};
			uint32_t share_nvstatus = 0;
			uint64_t share_fault    = 0;
			int sret = nvkvm_isolate_ioctl(&nv->isolates,
						       req->isolate_id,
						       req->handle_id,
						       0xc0184635u,
						       &share, sizeof(share),
						       NULL, 0,
						       0,
						       &share_nvstatus,
						       &share_fault);
			NVKVM_DBG(
				"nvkvm: post-alloc SHARE hClient=0x%x hClass=0x%x "
				"hObj=0x%x ret=%d nvstatus=0x%x status=0x%x\n",
				hClient, hClass, hObjNew, sret, share_nvstatus,
				share.status);
		}
	}

	/* For RM_CONTROL, also extract the inner cmd at param offset 8 so we
	 * can see which control specifically returned a non-zero nvstatus. */
	uint32_t inner_cmd = 0;
	if (_IOC_NR(req->cmd) == NV_ESC_RM_CONTROL && param_buf &&
	    req->param_size >= 12) {
		memcpy(&inner_cmd, (char *)param_buf + 8, sizeof(uint32_t));
	}

	/* DIAG: for RM_MAP_MEMORY, dump all params so we can see what the
	 * kernel saw and what it returned. */
	/* S-6: the guard has to cover the LAST field read below, not the last
	 * one before it.  `fd` sits at offset 48 in the 56-byte NVOS33, so the
	 * memcpy touches bytes 48..51 — a param_size of exactly 48 read four
	 * bytes past the end of an allocation of exactly that size.  The memcpy
	 * is unconditional; only the print that follows was ever gated. */
	if (_IOC_NR(req->cmd) == NV_ESC_RM_MAP_MEMORY && param_buf &&
	    req->param_size >= 52) {
		uint32_t h_client = 0, h_device = 0, h_memory = 0;
		uint64_t offset = 0, length = 0, plinear = 0;
		uint32_t mm_status = 0, flags = 0;
		int32_t fd = 0;
		memcpy(&h_client, (char *)param_buf + 0,  sizeof(uint32_t));
		memcpy(&h_device, (char *)param_buf + 4,  sizeof(uint32_t));
		memcpy(&h_memory, (char *)param_buf + 8,  sizeof(uint32_t));
		memcpy(&offset,   (char *)param_buf + 16, sizeof(uint64_t));
		memcpy(&length,   (char *)param_buf + 24, sizeof(uint64_t));
		memcpy(&plinear,  (char *)param_buf + 32, sizeof(uint64_t));
		memcpy(&mm_status,(char *)param_buf + 40, sizeof(uint32_t));
		memcpy(&flags,    (char *)param_buf + 44, sizeof(uint32_t));
		memcpy(&fd,       (char *)param_buf + 48, sizeof(int32_t));
		NVKVM_DBG(
			"nvkvm: RM_MAP_MEMORY: h_client=0x%x h_device=0x%x "
			"h_memory=0x%x offset=0x%llx length=0x%llx flags=0x%x "
			"fd=%d -> pLinear=0x%llx status=0x%x\n",
			h_client, h_device, h_memory,
			(unsigned long long)offset, (unsigned long long)length,
			flags, fd, (unsigned long long)plinear, mm_status);
	}

	/* DIAG: for RM_ALLOC, dump hClient/hParent/hObjNew/hClass when
	 * nvstatus is non-zero, so we can see which class the driver
	 * rejected.  nvos21 has hClient, hParent, hObjNew, hClass at the
	 * start; nvos64 has the same layout for the first 16 bytes. */
	if (_IOC_NR(req->cmd) == NV_ESC_RM_ALLOC && nvstatus &&
	    param_buf && req->param_size >= 16) {
		uint32_t hClient = 0, hParent = 0, hObjNew = 0, hClass = 0;
		memcpy(&hClient, (char *)param_buf + 0,  sizeof(uint32_t));
		memcpy(&hParent, (char *)param_buf + 4,  sizeof(uint32_t));
		memcpy(&hObjNew, (char *)param_buf + 8,  sizeof(uint32_t));
		memcpy(&hClass,  (char *)param_buf + 12, sizeof(uint32_t));
		uint32_t aps = 0;
		if (req->param_size == sizeof(struct nvos64_parameters))
			memcpy(&aps, (char *)param_buf + 32, sizeof(uint32_t));
		NVKVM_DBG(
			"nvkvm: RM_ALLOC failed: hClient=0x%x hParent=0x%x "
			"hObjNew=0x%x hClass=0x%x alloc_parms_size=%u aux_size=%u "
			"nvstatus=0x%x\n",
			hClient, hParent, hObjNew, hClass, aps,
			req->aux_size, nvstatus);
		/* hex dump first 64 bytes of aux_buf (the alloc params themselves) */
		if (aux_buf && req->aux_size > 0) {
			const uint8_t *b = aux_buf;
			uint32_t n = req->aux_size < 64 ? req->aux_size : 64;
			char hex[256] = {0};
			for (uint32_t i = 0; i < n; i++)
				snprintf(hex + i*3, sizeof(hex)-i*3, "%02x ", b[i]);
			fprintf(stderr, "nvkvm: RM_ALLOC failed aux[%u]: %s\n",
				n, hex);
		}
	}

	/*
	 * Diagnostic: report what GET_CLASSLIST_V2 delivers to the guest.  V2
	 * inlines { NvU32 numClasses; NvU32 classList[] } in the aux buffer, and
	 * a truncated or filtered list here would silently steer the userspace
	 * driver to the wrong engine classes for the GPU's architecture -- the
	 * failure mode this was written to rule out on Hopper.  Cheap (a handful
	 * of calls per process) and invaluable when bringing up a new
	 * architecture, so it is kept, behind NVKVM_DEBUG.
	 */
	if (inner_cmd == 0x00800292u && aux_buf && req->aux_size >= 8) {
		const uint32_t *v = (const uint32_t *)aux_buf;
		uint32_t n = v[0];
		uint32_t cap = (req->aux_size - 4) / 4;
		if (n > cap) n = cap;
		int has_c86f = 0, has_c56f = 0, has_cbc0 = 0;
		for (uint32_t i = 0; i < n; i++) {
			if (v[1+i] == 0xc86f) has_c86f = 1;
			if (v[1+i] == 0xc56f) has_c56f = 1;
			if (v[1+i] == 0xcbc0) has_cbc0 = 1;
		}
		NVKVM_DBG(
			"nvkvm: CLASSLIST_V2 ret=%lld nvstatus=0x%x aux_size=%u "
			"numClasses=%u cap=%u hopper_gpfifo_c86f=%d "
			"ampere_gpfifo_c56f=%d hopper_compute_cbc0=%d\n",
			(long long)ret, nvstatus, req->aux_size, v[0], cap,
			has_c86f, has_c56f, has_cbc0);
	}

	if (inner_cmd) {
		NVKVM_DBG(
			"nvkvm: ioctl_on_isolate: isolate=%u handle=%u cmd=0x%x "
			"inner=0x%x ret=%lld nvstatus=0x%x fault=0x%llx\n",
			req->isolate_id, req->handle_id, req->cmd, inner_cmd,
			(long long)ret, nvstatus, (unsigned long long)fault_addr);
	} else {
		NVKVM_DBG(
			"nvkvm: ioctl_on_isolate: isolate=%u handle=%u cmd=0x%x "
			"ret=%lld nvstatus=0x%x fault=0x%llx\n",
			req->isolate_id, req->handle_id, req->cmd,
			(long long)ret, nvstatus, (unsigned long long)fault_addr);
	}

	/*
	 * A100 (GA100) debug: cuCtxCreate fails with a repeating
	 * RM_FREE(ok) -> RM_UNMAP_MEMORY(OBJECT_NOT_FOUND) pair. Dump the
	 * object handles on all three so the UNMAP can be correlated against
	 * the MAP that created it, and against the FREE just before it.
	 * NVOS34 (unmap) is {h_client,h_device,h_memory,rsvd,p_linear_address,...}
	 * NVOS33 (map)   is the same prefix, then offset/length/p_linear_address.
	 * NVOS00 (free)  is {h_root,h_object_parent,h_object_old,status}.
	 */
	if (nvkvm_debug_enabled && param_buf && _IOC_TYPE(req->cmd) == 'F') {
		unsigned nr = _IOC_NR(req->cmd);
		const uint32_t *w = (const uint32_t *)param_buf;
		if (nr == 0x4f && req->param_size >= 24) {          /* UNMAP */
			uint64_t va; memcpy(&va, (const char *)param_buf + 16, 8);
			NVKVM_DBG("nvkvm: A100DBG UNMAP hClient=0x%x hDevice=0x%x "
				  "hMemory=0x%x va=0x%llx nvstatus=0x%x\n",
				  w[0], w[1], w[2], (unsigned long long)va, nvstatus);
		} else if (nr == 0x4e && req->param_size >= 40) {   /* MAP */
			uint64_t off, len, va;
			memcpy(&off, (const char *)param_buf + 16, 8);
			memcpy(&len, (const char *)param_buf + 24, 8);
			memcpy(&va,  (const char *)param_buf + 32, 8);
			NVKVM_DBG("nvkvm: A100DBG MAP   hClient=0x%x hDevice=0x%x "
				  "hMemory=0x%x off=0x%llx len=0x%llx va=0x%llx nvstatus=0x%x\n",
				  w[0], w[1], w[2], (unsigned long long)off,
				  (unsigned long long)len, (unsigned long long)va, nvstatus);
		} else if (nr == 0x29 && req->param_size >= 16) {   /* FREE */
			NVKVM_DBG("nvkvm: A100DBG FREE  hRoot=0x%x hParent=0x%x "
				  "hObject=0x%x nvstatus=0x%x\n",
				  w[0], w[1], w[2], nvstatus);
		}
	}

	/* Trace UVM ioctls' rm_status field so we can see what the driver
	 * actually wrote back through the isolate path. */
	if (param_buf && req->param_size >= 8) {
		uint32_t rm_status_off = (uint32_t)-1;
		switch (req->cmd) {
		case 0x30000001: /* UVM_INITIALIZE: { __u64 flags; __u32 rm_status; ... } */
			rm_status_off = 8;
			break;
		case 0x30000002: /* UVM_DEINITIALIZE: { __u32 rm_status; } */
			rm_status_off = 0;
			break;
		case 75:         /* UVM_MM_INITIALIZE: { __s32 uvm_fd; __u32 rm_status; } */
			rm_status_off = 4;
			break;
		case 39:         /* UVM_PAGEABLE_MEM_ACCESS: { __u8 pageable_mem_access; __u32 rm_status; } */
			rm_status_off = 4;
			break;
		}
		if (rm_status_off != (uint32_t)-1 &&
		    req->param_size >= rm_status_off + 4) {
			uint32_t rmst = 0;
			memcpy(&rmst, (char *)param_buf + rm_status_off, 4);
			NVKVM_DBG(
				"nvkvm: ioctl_on_isolate UVM: cmd=0x%x rm_status=0x%x\n",
				req->cmd, rmst);
		}
	}
	return 0;
}

/* ── Mmap on isolate ─────────────────────────────────────────────────────── */

/*
 * Double-mmap implementation:
 *   1. QEMU mmaps the handle fd at any QVA (mmap(NULL)).
 *   2. QEMU registers GPA→QVA in KVM (KVM_SET_USER_MEMORY_REGION).
 *   3. QEMU sends MMAP command to isolate: map same fd at gva (MAP_FIXED).
 */


extern int nvkvm_kvm_vm_fd;

#ifndef KVM_SET_USER_MEMORY_REGION
#define NVKVM_KVMIO 0xAE
struct nvkvm_kvm_mem_region {
	uint32_t slot;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
};
#define KVM_SET_USER_MEMORY_REGION _IOW(NVKVM_KVMIO, 0x46, struct nvkvm_kvm_mem_region)
#endif

/*
 * Sentinel kvm_slot value meaning "this mapping lives inside the single
 * pre-installed 128 GiB sparse window — there is NO per-mmap KVM memslot to
 * remove on teardown; instead the device backing is restored to anonymous
 * pages so the window stays fully mapped for KVM."  Distinct from -1, which
 * means "legacy path, memslot install was attempted but failed/absent."
 */
#define NVKVM_IN_WINDOW_SLOT  (-2)

/*
 * KVM slot allocator is centralised in nvkvm_mmap_host.c via the
 * nvkvm_kvm_slot_alloc/release prototypes in virtio_nvgpu.h, shared with
 * nvkvm_mmap_create().  The stale `iso_kvm_slot_counter` monotonic
 * counter that used to live here — which overlapped with the mmap_host
 * counter's range past 100 and never recycled — has been removed.
 * Audit L5 follow-up.
 */

int nvkvm_req_mmap_on_isolate(VirtIONvgpu *nv,
			       struct nvkvm_req_mmap_on_isolate *req,
			       struct nvkvm_resp_mmap_on_isolate *resp)
{
	memset(resp, 0, sizeof(*resp));

	struct nvkvm_handle *h = nvkvm_handle_get(&nv->handles, req->handle_id);
	if (!h || h->fd < 0) {
		resp->status = EBADF;
		return 0;
	}

	/*
	 * S-2 (cross-isolate): the guest names an (isolate, handle) PAIR here,
	 * exactly as XISO_IMPORT does, and the two are independent assertions.
	 * `h->fd >= 0` alone only proves the handle exists SOMEWHERE in this VM
	 * — it says nothing about who owns it — so without this an isolate could
	 * map any other host-process isolate's live /dev/nvidia* fd into the
	 * window, at a GPA of its choosing, in the privileged QEMU process.
	 * Isolates are separate host processes, so that is the host-process
	 * boundary this layer is responsible for, not intra-VM policy; PRESENT
	 * and XISO_IMPORT already check the same two things (handle→session,
	 * session→isolate) and this handler simply did not.
	 *
	 * NOT uid-separated, whatever this comment used to say.  On the rung
	 * nvkvm_iso_auto_select() prefers, mode is NS|SECCOMP with no
	 * NVKVM_ISO_LAYER_UID, so use_uid is false, nvkvm_iso_drop_privilege()
	 * never runs, and nvkvm_map_child_userns() writes a bare
	 * "0 <geteuid()> 1" uid_map -- every isolate's in-namespace root maps to
	 * the SAME host uid, QEMU's own.  What separates them is the user/pid/
	 * mount/net/ipc/uts namespaces plus seccomp, which is a real boundary
	 * (sibling user namespaces cannot ptrace each other and CLONE_NEWPID
	 * hides the pids) but is not a uid boundary.  Distinct host uids appear
	 * only on the UID rung, which auto-select falls back to when
	 * CLONE_NEWUSER fails.  Do not reason about this layer as though a
	 * DAC check were backing it up.
	 */
	if (h->session_id != req->session_id ||
	    !session_has_isolate(nv, req->session_id, req->isolate_id)) {
		NVKVM_DBG("nvkvm: mmap_on_isolate: handle/isolate session "
			  "mismatch (h=%u h_sess=%u req_sess=%u iso=%u)\n",
			  req->handle_id, h->session_id, req->session_id,
			  req->isolate_id);
		resp->status = EPERM;
		return 0;
	}

	/*
	 * S-1 (oob-map): for a memory handle the object has a KNOWN extent (the
	 * ftruncate at creation), so check the guest's window against it.  This
	 * matters more than a normal bounds check: the in-window branch below
	 * prefaults every page from inside the VMM, so a mapping that runs past
	 * the memfd's last page is not a wrong result, it is a SIGBUS that kills
	 * QEMU.  Compare against the page-rounded size because mmap legitimately
	 * covers the whole final page of a non-page-multiple object.
	 * TYPE_NVIDIA handles are exempt: `offset` there is an RM mapping token
	 * with no relation to a file length (h->size is 0 and means "unknown").
	 */
	if (h->type == NVKVM_HANDLE_TYPE_MEMORY) {
		uint64_t obj_end = (h->size + 4095ULL) & ~4095ULL;
		if (obj_end < h->size ||                       /* round-up wrap */
		    req->offset > obj_end ||
		    req->length > obj_end - req->offset) {
			NVKVM_DBG("nvkvm: mmap_on_isolate: [0x%llx,+0x%llx) "
				  "outside memory handle %u (size=%llu)\n",
				  (unsigned long long)req->offset,
				  (unsigned long long)req->length,
				  req->handle_id,
				  (unsigned long long)h->size);
			resp->status = EINVAL;
			return 0;
		}
	}

	/* N-2: bound the raw length BEFORE the page-align round-up — a length
	 * near SIZE_MAX would otherwise wrap to a small page-multiple that
	 * passes the len<=sparse_size check below, giving the guest a mapping
	 * far smaller than it asked for. */
	if (req->length == 0 || req->length > nv->sparse_size) {
		resp->status = EINVAL;
		return 0;
	}
	size_t len = (size_t)req->length;
	len = (len + 4095UL) & ~4095UL;  /* page-align (no wrap: bounded above) */

	/*
	 * Audit M-1: this mmap runs in the privileged QEMU process against the
	 * real GPU device fd with a guest-controlled prot/len.  Mask prot to
	 * R/W only (never PROT_EXEC, matching the REALIZE path's
	 * NVKVM_REALIZE_PROT_MASK) and reject absurd lengths before we touch the
	 * device fd or the sparse window.  (Window allocation also rejects
	 * oversize, but bound here so we never hand a wild len to mmap().)
	 */
	req->prot &= (uint32_t)(PROT_READ | PROT_WRITE);
	if (len == 0 || len > nv->sparse_size) {
		resp->status = EINVAL;
		return 0;
	}

	/*
	 * Place the mapping inside the single pre-installed 128 GiB sparse
	 * window instead of allocating a fresh KVM memslot per mmap.  A single
	 * cuCtxCreate issues >1500 tiny (4 KB) device mmaps; one memslot each
	 * blows past both our pool and any sane slot count.  By MAP_FIXED'ing
	 * the device fd into the sparse window's VA range we reuse the one
	 * memslot nvkvm_sparse_init() already installed — zero per-mmap KVM
	 * ioctls.  Sparse/holey device regions inside one big prereserved KVM
	 * memory region are fully supported by KVM (per-page gup on fault).
	 *
	 * /dev/nvidia-uvm is the exception: its kernel mmap handler requires
	 *   vm_start == (vm_pgoff << PAGE_SHIFT)
	 * so QEMU must map it MAP_FIXED at req->offset, not at an arbitrary
	 * window VA.  UVM mappings are few, so the legacy per-mmap memslot is
	 * acceptable for them.
	 */
	void    *qva       = MAP_FAILED;
	uint64_t gpa       = 0;
	int      kvm_slot  = -1;
	bool     in_window = (h->dev_id != NVKVM_DEV_UVM);

	if (in_window) {
		gpa = nvkvm_sparse_gpa_alloc(nv, len);
		void *target = gpa ? nvkvm_gpa_to_vmm_va(nv, gpa, len) : NULL;
		if (!target) {
			NVKVM_DBG(
				"nvkvm: mmap_on_isolate: sparse window full "
				"(handle=%u len=%lu)\n",
				req->handle_id, (unsigned long)len);
			resp->status = ENOMEM;
			return 0;
		}
		/*
		 * Honour what the guest asked for.  KVM resolves a memslot HVA
		 * with get_user_pages()/hva_to_pfn_remapped(), and on a device
		 * VMA (VM_IO|VM_PFNMAP) that cannot produce a PFN it fails
		 * KVM_RUN with EFAULT -- unrecoverable, not a resumable MMIO
		 * exit.  But that is a *write* problem: a read fault only needs
		 * gup(write=0), which a read-only device VMA satisfies.
		 *
		 * This used to force PROT_READ|PROT_WRITE for every mapping,
		 * which broke read-only ones.  The driver honours a PROT_READ
		 * request with a read-only VMA; the forced-RW mprotect probe
		 * below then failed on it and the page was replaced with
		 * anonymous zeroes -- so whatever the driver publishes there
		 * (notifier/semaphore words the runtime polls) never reached the
		 * guest, and OpenCL read stale buffer contents with no error.
		 * See docs/reference/correctness.md.
		 */
		const bool want_write = (req->prot & PROT_WRITE) != 0;
		qva = mmap(target, len,
			   want_write ? (PROT_READ | PROT_WRITE) : PROT_READ,
			   MAP_SHARED | MAP_FIXED, h->fd, (off_t)req->offset);
		if (qva == MAP_FAILED) {
			int se = errno;
			NVKVM_DBG(
				"nvkvm: mmap_on_isolate(window) FAIL fd=%d "
				"dev_id=%d prot=0x%x len=%lu off=0x%lx "
				"gpa=0x%llx errno=%d (%s)\n",
				h->fd, h->dev_id, req->prot, (unsigned long)len,
				(unsigned long)req->offset,
				(unsigned long long)gpa, se, strerror(se));
			/* Restore the anonymous backing we just clobbered so the
			 * window stays fully mapped for KVM. */
			nvkvm_window_restore_anon(target, len);
			resp->status = (uint32_t)se;
			return 0;
		}
		NVKVM_DBG("nvkvm: WINMAP fd=%d dev_id=%d off=0x%lx len=%lu "
			  "gpa=0x%llx qva=%p req_prot=0x%x\n",
			  h->fd, h->dev_id, (unsigned long)req->offset,
			  (unsigned long)len, (unsigned long long)gpa, qva,
			  req->prot);
		/*
		 * The driver's own mmap handler may clear VM_WRITE (and
		 * VM_MAYWRITE) on the VMA it just gave us, regardless of the
		 * PROT_READ|PROT_WRITE we asked for.  Such a page cannot sit
		 * under the sparse window's memslot: KVM resolves a memslot HVA
		 * with get_user_pages(), and a guest store into a non-writable
		 * VMA fails gup(write=1), so KVM_RUN returns EFAULT -- fatal and
		 * unrecoverable, not a resumable MMIO exit.
		 *
		 * mprotect() is an O(1) probe for exactly this: it fails with
		 * EACCES when VM_MAYWRITE was cleared.  On such a page, undo the
		 * device mapping and let the GPA ride the window's anonymous
		 * backing instead -- the same model the UVM branch below uses.
		 * The isolate stub still holds the real device mapping in its own
		 * address space, and the GPU reaches the memory by DMA rather
		 * than through a QEMU CPU memslot.
		 */
		/*
		 * Pre-fault the mapping from QEMU.  The driver may leave the VMA
		 * with no PTEs installed (pagemap shows present=0 on an rw-s
		 * /dev/nvidia0 VMA).  KVM cannot populate those on demand: for a
		 * VM_IO|VM_PFNMAP VMA it falls back to fixup_user_fault(), which
		 * returns SIGBUS when there is no usable ->fault handler, and
		 * KVM_RUN then fails with EFAULT.  Touching each page here forces
		 * the PTEs in while we are still on a normal userspace fault path.
		 * Reads only -- never write, that would disturb device state.
		 */
		{
			volatile const uint8_t *p = (volatile const uint8_t *)qva;
			for (size_t o = 0; o < len; o += 4096)
				(void)p[o];
		}
		/*
		 * Only a mapping the guest wants to WRITE needs a writable host
		 * VMA.  For a read-only request there is nothing to probe: keep
		 * the real device page so its contents reach the guest.
		 */
		if (want_write && mprotect(qva, len, PROT_READ | PROT_WRITE) != 0) {
			NVKVM_DBG("nvkvm: WINMAP gpa=0x%llx len=%lu is "
				  "driver-readonly (%s) -- falling back to "
				  "anonymous window backing\n",
				  (unsigned long long)gpa, (unsigned long)len,
				  strerror(errno));
			nvkvm_window_restore_anon(qva, len);
		}
		/* No KVM ioctl: the sparse window's single memslot already maps
		 * [gpa, gpa+len) → this VA range. */
		kvm_slot = NVKVM_IN_WINDOW_SLOT;
	} else {
		/*
		 * /dev/nvidia-uvm.  The old approach mmap'd the UVM fd MAP_FIXED at
		 * req->offset in QEMU's address space and installed a per-mmap
		 * memslot.  That collides across concurrent processes: libcuda
		 * picks the same UVM VA in every process, and QEMU's single address
		 * space can only hold one mapping there, so the second process hits
		 * MAP_FIXED_NOREPLACE → EEXIST → cuCtxCreate fails (304).  (UVM also
		 * cannot be MAP_FIXED into the sparse window: its kernel mmap
		 * requires vm_start == (vm_pgoff<<PAGE_SHIFT).)
		 *
		 * Instead allocate the GPA from the sparse window and let it ride
		 * the window's anonymous backing — no QEMU-side device mmap, no
		 * per-mmap memslot, no cross-process VA collision (same model as the
		 * realize path).  The stub owns the real UVM mapping in its own
		 * per-process address space; the GPU reaches the memory via DMA, not
		 * a QEMU CPU memslot.
		 */
		gpa = nvkvm_sparse_gpa_alloc(nv, len);
		void *target = gpa ? nvkvm_gpa_to_vmm_va(nv, gpa, len) : NULL;
		if (!target) {
			NVKVM_DBG(
				"nvkvm: mmap_on_isolate(uvm): sparse window full "
				"(handle=%u len=%lu)\n",
				req->handle_id, (unsigned long)len);
			resp->status = ENOMEM;
			return 0;
		}
		qva      = target;             /* sparse-window VA (anon-backed) */
		kvm_slot = NVKVM_IN_WINDOW_SLOT;
	}

	/* Step 3: optionally mirror the mapping into the isolate's mm.
	 *
	 * The isolate-side mmap is only useful for the case where an NVIDIA
	 * ioctl dereferences a user VA pointing into this region while
	 * executing in the stub's process context.  All command-buffer
	 * traffic (NVOS54 params, alloc structs, etc) is already explicitly
	 * copied via shm slots, and the GPU itself reaches the memory via
	 * the KVM-installed GPA↔hostVA mapping — not through the stub's mm.
	 *
	 * For /dev/nvidia-uvm this mmap actively fails (EBADFD): the stub's
	 * UVM fd is per-process state in the kernel and may not be in the
	 * right uvm_fd_type when libcuda issues the mmap.  Skipping the
	 * mirror unblocks cuCtxCreate; if we ever discover an ioctl that
	 * does require the stub mm to back the VA, we'll mirror it then.
	 */
	int ret = 0;
	uint64_t stub_va = 0;
	struct nvkvm_handle *hd = nvkvm_handle_get(&nv->handles, req->handle_id);
	int do_stub_mirror = !hd || hd->dev_id != 1 /* NVKVM_DEV_UVM */;
	if (do_stub_mirror) {
		/*
		 * U-9: req->gva is the GUEST's address and is no longer where
		 * this gets mapped.  It arrives off the virtqueue untouched
		 * (nvkvm_mmap.c sets it to vma->vm_start), so MAP_FIXED'ing
		 * there put an arbitrary guest-chosen address on top of
		 * whatever the isolate had — its text, its stack, its job
		 * blobs.  Now the window allocator picks the address and the
		 * stub refuses anything outside its reservation; req->gva
		 * survives only as the identity recorded in iso_mmap_tbl and
		 * as the adjacency key that keeps a migration's 2 MiB chunks
		 * contiguous.
		 *
		 * `contig` is set for memory handles because those are the
		 * OS-descriptor migration's memfd chunks, and only they are
		 * later pinned as one range by RmAllocOsDescriptor.
		 */
		bool contig = hd && hd->type == NVKVM_HANDLE_TYPE_MEMORY;
		uint64_t place = nvkvm_win_place(&nv->isolates, req->isolate_id,
						 req->gva, len, contig);
		if (!place) {
			fprintf(stderr,
				"nvkvm: mmap_on_isolate: no room in isolate "
				"%u's guest-mapping window (len=%lu)\n",
				req->isolate_id, (unsigned long)len);
			ret = -ENOMEM;
		} else {
			ret = nvkvm_isolate_mmap(&nv->isolates,
						 req->isolate_id,
						 req->handle_id,
						 place, len, req->offset,
						 (int)req->prot,
						 (int)req->map_flags,
						 &stub_va);
			if (ret < 0)
				nvkvm_win_unplace(req->isolate_id, place, len);
			else if (!stub_va)
				stub_va = place;
		}
	}

	if (ret < 0) {
		if (kvm_slot == NVKVM_IN_WINDOW_SLOT) {
			/* Restore anon backing inside the window (no munmap — that
			 * would punch a hole in the sparse VMA). */
			nvkvm_window_restore_anon(qva, len);
		} else {
			if (kvm_slot >= 0 && nvkvm_kvm_vm_fd >= 0) {
				struct nvkvm_kvm_mem_region mr = {
					.slot = (uint32_t)kvm_slot,
					.memory_size = 0 };
				ioctl(nvkvm_kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &mr);
				nvkvm_kvm_slot_release(kvm_slot);
			}
			munmap(qva, len);
		}
		resp->status = (uint32_t)-ret;
		return 0;
	}

	/* Record for future MUNMAP_ON_ISOLATE */
	uint32_t token = iso_mmap_alloc(req->isolate_id, req->gva, stub_va, qva,
					len, kvm_slot, gpa,
					do_stub_mirror, req->handle_id);
	if (token == 0) {
		/*
		 * Table full: we cannot track this mapping for later teardown, so
		 * tear it down NOW and fail the request — otherwise the GPA extent
		 * + KVM slot + isolate mirror would leak (the guest gets a token it
		 * can never munmap).  Mirrors the munmap/reap reclaim path.
		 */
		fprintf(stderr, "nvkvm: iso_mmap_tbl full — undoing mapping\n");
		if (do_stub_mirror && stub_va) {
			nvkvm_isolate_munmap(&nv->isolates, req->isolate_id,
					     stub_va, (uint64_t)len);
			nvkvm_win_unplace(req->isolate_id, stub_va,
					  (uint64_t)len);
		}
		if (kvm_slot == NVKVM_IN_WINDOW_SLOT) {
			if (qva != MAP_FAILED && qva)
				nvkvm_window_restore_anon(qva, len);
		} else {
			if (kvm_slot >= 0 && nvkvm_kvm_vm_fd >= 0) {
				struct nvkvm_kvm_mem_region mr = {
					.slot        = (uint32_t)kvm_slot,
					.memory_size = 0,
				};
				ioctl(nvkvm_kvm_vm_fd,
				      KVM_SET_USER_MEMORY_REGION, &mr);
				nvkvm_kvm_slot_release(kvm_slot);
			}
			if (qva != MAP_FAILED && qva)
				munmap(qva, len);
		}
		if (gpa)
			nvkvm_sparse_gpa_free(nv, gpa, (size_t)len);
		resp->status = ENOMEM;
		return 0;
	}

	resp->mmap_token = token;
	resp->gpa_base   = gpa;
	resp->length     = (uint64_t)len;
	resp->status     = 0;
	return 0;
}

int nvkvm_req_munmap_on_isolate(VirtIONvgpu *nv,
				 struct nvkvm_req_munmap_on_isolate *req,
				 struct nvkvm_resp_munmap_on_isolate *resp)
{
	struct nvkvm_iso_mmap_entry e;

	/*
	 * S-2 (cross-isolate): the token is an index into one VM-global table,
	 * and isolates are separate HOST processes — the same boundary
	 * PRESENT/XISO_IMPORT validate against.  (Separate, but NOT uid-
	 * separated on the default rung; see the note in
	 * nvkvm_req_mmap_on_isolate above for what actually separates them.)
	 * Without this check any
	 * isolate could walk the token space and tear down a NEIGHBOUR's live GPU
	 * mapping: the teardown below runs entirely on the victim's recorded
	 * entry (its isolate, its GVA, its GPA extent), so a stranger's token
	 * unmaps in the victim's address space and returns the victim's window
	 * extent for reuse while it is still writing through it.
	 *
	 * iso_mmap_free() now refuses a token this isolate does not own, and
	 * reports it as ENOENT — the same answer as an unknown token, so probing
	 * cannot distinguish "not yours" from "not there".
	 *
	 * KNOWN GAP, do not read this as a closed boundary.  "This isolate" is
	 * req->isolate_id, which the GUEST supplies, and this request carries no
	 * other identity — struct nvkvm_req_munmap_on_isolate is
	 * { isolate_id, mmap_token } and nothing else.  So the test is
	 * "the token you named must belong to the isolate you named", not "…to
	 * you": a caller that names a neighbour's isolate_id together with that
	 * neighbour's token passes it.  Three of the five isolate_id-taking
	 * handlers additionally check session_has_isolate() against a session_id
	 * they take from QEMU's own handle table; there is no handle here to
	 * anchor to, and anchoring to the mapping entry's own handle_id would be
	 * circular (the entry's isolate_id is already required to match).
	 * Closing this needs a caller session_id ON THE WIRE — a protocol
	 * change, deliberately not bodged in here.  Note it would bound a
	 * malicious guest USERSPACE process only: the guest kernel module fills
	 * that field and is itself untrusted, so treat it as defence in depth,
	 * the same weight the neighbouring session_has_isolate() checks carry.
	 */
	if (req->isolate_id == 0) {
		resp->status = EINVAL;
		return 0;
	}
	if (!iso_mmap_free(req->mmap_token, req->isolate_id, &e)) {
		resp->status = ENOENT;
		return 0;
	}

	/* Tell the isolate to unmap the range, but only if we mirrored the
	 * mapping there in the first place.  U-9: the address is the WINDOW VA
	 * we recorded at mmap time, not e.gva — the stub refuses anything
	 * outside the window, so a guest address here would simply fail. */
	if (e.stub_mirrored && e.stub_va) {
		nvkvm_isolate_munmap(&nv->isolates, e.isolate_id, e.stub_va,
				     (uint64_t)e.len);
		nvkvm_win_unplace(e.isolate_id, e.stub_va, (uint64_t)e.len);
	}

	if (e.kvm_slot == NVKVM_IN_WINDOW_SLOT) {
		/* In-window mapping: restore anonymous backing so the sparse
		 * window stays fully mapped (the single memslot covers it).
		 * Do NOT munmap — that would punch a hole in the sparse VMA. */
		if (e.qva)
			nvkvm_window_restore_anon(e.qva, e.len);
	} else {
		/* Legacy path: remove the per-mmap KVM memslot, return it to the
		 * pool, and unmap the standalone QEMU host VA. */
		if (e.kvm_slot >= 0 && nvkvm_kvm_vm_fd >= 0) {
			struct nvkvm_kvm_mem_region mr = {
				.slot        = (uint32_t)e.kvm_slot,
				.memory_size = 0,
			};
			ioctl(nvkvm_kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &mr);
			nvkvm_kvm_slot_release(e.kvm_slot);
		}
		if (e.qva)
			munmap(e.qva, e.len);
	}

	/* #80/H-1: return the GPA extent to the window free-list so a
	 * mmap/munmap loop recycles window space instead of leaking it. */
	NVKVM_DBG("nvkvm: WINUNMAP token=%u gpa=0x%llx len=%lu slot=%d\n",
		  req->mmap_token, (unsigned long long)e.gpa,
		  (unsigned long)e.len, e.kvm_slot);
	if (e.gpa)
		nvkvm_sparse_gpa_free(nv, e.gpa, (size_t)e.len);

	resp->status = 0;
	return 0;
}

/*
 * Tear down every window mapping made from one frontend handle.
 *
 * A guest that frees an RM memory object closes its handle, but never sends
 * MUNMAP_ON_ISOLATE for the mappings made from it: before this, every window
 * extent survived until the isolate exited.  The driver then recycles that
 * device memory for the next allocation while the guest is still writing into
 * the stale extent -- the CPU reads back its own writes and the GPU reads
 * zeros, silently wrong results with no error anywhere.  See
 * docs/reference/correctness.md.
 */
static int nvkvm_iso_mmap_reap_handle(VirtIONvgpu *nv, uint32_t isolate_id,
				      uint32_t handle_id)
{
	int reaped = 0;
	if (!handle_id)
		return 0;
	pthread_mutex_lock(&iso_mmap_lock);
	for (uint32_t i = 1; i < NVKVM_ISO_MMAP_MAX; i++) {
		if (!iso_mmap_tbl[i].used ||
		    iso_mmap_tbl[i].isolate_id != isolate_id ||
		    iso_mmap_tbl[i].handle_id  != handle_id)
			continue;
		struct nvkvm_iso_mmap_entry e = iso_mmap_tbl[i];
		iso_mmap_tbl[i].used = false;
		pthread_mutex_unlock(&iso_mmap_lock);

		NVKVM_DBG("nvkvm: REAP_HANDLE handle=%u gpa=0x%llx len=%lu\n",
			  handle_id, (unsigned long long)e.gpa,
			  (unsigned long)e.len);

		if (e.kvm_slot == NVKVM_IN_WINDOW_SLOT) {
			if (e.qva)
				nvkvm_window_restore_anon(e.qva, e.len);
		} else {
			if (e.kvm_slot >= 0 && nvkvm_kvm_vm_fd >= 0) {
				struct nvkvm_kvm_mem_region mr = {
					.slot        = (uint32_t)e.kvm_slot,
					.memory_size = 0,
				};
				ioctl(nvkvm_kvm_vm_fd,
				      KVM_SET_USER_MEMORY_REGION, &mr);
				nvkvm_kvm_slot_release(e.kvm_slot);
			}
			if (e.qva)
				munmap(e.qva, e.len);
		}
		if (e.gpa)
			nvkvm_sparse_gpa_free(nv, e.gpa, (size_t)e.len);
		reaped++;
		pthread_mutex_lock(&iso_mmap_lock);
	}
	pthread_mutex_unlock(&iso_mmap_lock);
	return reaped;
}

/*
 * #80 (audit H-3/M-E): on isolate kill, reclaim every iso_mmap_tbl entry the
 * isolate still holds (guest killed/went silent without MUNMAP_ON_ISOLATE).
 * Mirrors nvkvm_req_munmap_on_isolate's per-entry teardown but skips the
 * isolate-side munmap (the isolate is gone).  Returns the count reclaimed.
 */
static int nvkvm_iso_mmap_reap_isolate(VirtIONvgpu *nv, uint32_t isolate_id)
{
	int reaped = 0;
	pthread_mutex_lock(&iso_mmap_lock);
	for (uint32_t i = 1; i < NVKVM_ISO_MMAP_MAX; i++) {
		if (!iso_mmap_tbl[i].used ||
		    iso_mmap_tbl[i].isolate_id != isolate_id)
			continue;
		struct nvkvm_iso_mmap_entry e = iso_mmap_tbl[i];
		iso_mmap_tbl[i].used = false;
		/* Drop the lock for the slow mmap/ioctl/munmap; the entry is
		 * already detached so nothing else can touch it. */
		pthread_mutex_unlock(&iso_mmap_lock);

		if (e.kvm_slot == NVKVM_IN_WINDOW_SLOT) {
			if (e.qva)
				nvkvm_window_restore_anon(e.qva, e.len);
		} else {
			if (e.kvm_slot >= 0 && nvkvm_kvm_vm_fd >= 0) {
				struct nvkvm_kvm_mem_region mr = {
					.slot        = (uint32_t)e.kvm_slot,
					.memory_size = 0,
				};
				ioctl(nvkvm_kvm_vm_fd,
				      KVM_SET_USER_MEMORY_REGION, &mr);
				nvkvm_kvm_slot_release(e.kvm_slot);
			}
			if (e.qva)
				munmap(e.qva, e.len);
		}
		if (e.gpa)
			nvkvm_sparse_gpa_free(nv, e.gpa, (size_t)e.len);
		reaped++;
		pthread_mutex_lock(&iso_mmap_lock);
	}
	pthread_mutex_unlock(&iso_mmap_lock);
	/* U-9: the isolate's address space is gone, so its window bookkeeping
	 * describes nothing.  Dropping it here also means a recycled isolate id
	 * re-probes rather than inheriting the dead isolate's window base. */
	nvkvm_win_forget_isolate(isolate_id);
	return reaped;
}

/* ── Poll on isolate ─────────────────────────────────────────────────────── */

int nvkvm_req_poll_on_isolate(VirtIONvgpu *nv,
			       struct nvkvm_req_poll_on_isolate *req,
			       struct nvkvm_resp_poll_on_isolate *resp)
{
	int ret = nvkvm_isolate_poll(&nv->isolates,
				     req->isolate_id,
				     req->handle_id,
				     req->events);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

int nvkvm_req_unpoll_on_isolate(VirtIONvgpu *nv,
				 struct nvkvm_req_unpoll_on_isolate *req,
				 struct nvkvm_resp_unpoll_on_isolate *resp)
{
	int ret = nvkvm_isolate_unpoll(&nv->isolates,
				       req->isolate_id,
				       req->handle_id);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

/* ── Memory handle I/O (CPU page migration) ──────────────────────────────── */

int nvkvm_req_write_memory_handle(VirtIONvgpu *nv,
				   struct nvkvm_req_write_memory_handle *req,
				   struct nvkvm_resp_write_memory_handle *resp,
				   void *data_buf)
{
	resp->status = 0;
	resp->reserved = 0;

	if (!data_buf || req->size == 0) {
		resp->status = EINVAL;
		return 0;
	}

	struct nvkvm_handle *h = nvkvm_handle_get(&nv->handles, req->handle_id);
	if (!h || h->fd < 0) {
		resp->status = EBADF;
		return 0;
	}
	/* N-1: only a memfd handle may be pwrite()'n. Reject device/eventfd
	 * (TYPE_NVIDIA) handles so a guest can't drive read/write fops + an
	 * arbitrary offset against a real /dev/nvidia* or eventfd fd. */
	if (h->type != NVKVM_HANDLE_TYPE_MEMORY) {
		resp->status = EBADF;
		return 0;
	}
	/*
	 * S-1 (oob): N-1 established WHICH fd may be written; this bounds WHERE.
	 * The offset is guest-chosen and pwrite past EOF silently GROWS the
	 * memfd, so without this a guest could inflate a one-page object to an
	 * arbitrary size (host memory it never asked for) and leave content
	 * outside the extent every other check reasons about.  h->size is the
	 * ftruncate at creation; the comparison is written as a subtraction so
	 * offset+size cannot wrap.
	 */
	if (req->offset > h->size || req->size > h->size - req->offset) {
		NVKVM_DBG("nvkvm: write_memory_handle %u: [0x%llx,+%u) outside "
			  "object (size=%llu)\n", req->handle_id,
			  (unsigned long long)req->offset, req->size,
			  (unsigned long long)h->size);
		resp->status = EINVAL;
		return 0;
	}

	ssize_t n = pwrite(h->fd, data_buf, req->size, (off_t)req->offset);
	if (n < 0) {
		resp->status = (uint32_t)errno;
	} else if ((uint32_t)n != req->size) {
		resp->status = EIO;
	}
	return 0;
}

int nvkvm_req_read_memory_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_read_memory_handle *req,
				  struct nvkvm_resp_read_memory_handle *resp,
				  void *data_buf)
{
	resp->status = 0;
	resp->reserved = 0;

	if (!data_buf || req->size == 0) {
		resp->status = EINVAL;
		return 0;
	}

	struct nvkvm_handle *h = nvkvm_handle_get(&nv->handles, req->handle_id);
	if (!h || h->fd < 0) {
		resp->status = EBADF;
		return 0;
	}
	/* N-1: only a memfd handle may be pread() — see write handler. */
	if (h->type != NVKVM_HANDLE_TYPE_MEMORY) {
		resp->status = EBADF;
		return 0;
	}
	/* S-1: bound the read to the object, as for the write handler.  A read
	 * past EOF only short-reads (already EIO here) rather than growing the
	 * file, but leaving the two sides of the same object under different
	 * rules is how the next caller ends up reasoning about the wrong one. */
	if (req->offset > h->size || req->size > h->size - req->offset) {
		NVKVM_DBG("nvkvm: read_memory_handle %u: [0x%llx,+%u) outside "
			  "object (size=%llu)\n", req->handle_id,
			  (unsigned long long)req->offset, req->size,
			  (unsigned long long)h->size);
		resp->status = EINVAL;
		return 0;
	}

	ssize_t n = pread(h->fd, data_buf, req->size, (off_t)req->offset);
	if (n < 0) {
		resp->status = (uint32_t)errno;
	} else if ((uint32_t)n != req->size) {
		resp->status = EIO;
	}
	return 0;
}

/* ── READ_HOST_FILE ──────────────────────────────────────────────────────────
 *
 * Live read of a host-side proc/sys file the guest doesn't have because
 * the real nvidia.ko isn't loaded in the VM.  File selection is by enum;
 * QEMU never trusts a guest-supplied path.
 *
 * The path table is the security boundary.  Files are read fresh on every
 * call so callers see live state.
 */
/* Discovered host GPU BDFs.  Populated once by scanning the host's own
 * /proc/driver/nvidia/gpus/ directory — the guest never supplies these, so a
 * guest gpu_index can only ever resolve to a real, host-enumerated GPU path. */
#define NVKVM_MAX_HOST_GPUS 16
#define NVKVM_BDF_LEN       12   /* "0000:00:07.0" */
static char  nvkvm_host_bdf[NVKVM_MAX_HOST_GPUS][NVKVM_BDF_LEN + 1];
static int   nvkvm_host_gpu_count = -1;   /* -1 = not yet discovered */

/* Strict BDF format check: DDDD:BB:DD.F (hex), exactly NVKVM_BDF_LEN chars.
 * Rejects "..", slashes, and anything that isn't a canonical PCI address —
 * defence in depth on top of the fact that these names come from readdir. */
static bool nvkvm_bdf_valid(const char *s)
{
	if (strlen(s) != NVKVM_BDF_LEN)
		return false;
	for (int i = 0; i < NVKVM_BDF_LEN; i++) {
		char c = s[i];
		if (i == 4 || i == 7) {            /* ':' positions */
			if (c != ':') return false;
		} else if (i == 10) {              /* '.' position  */
			if (c != '.') return false;
		} else {                           /* hex digit     */
			if (!((c >= '0' && c <= '9') ||
			      (c >= 'a' && c <= 'f') ||
			      (c >= 'A' && c <= 'F')))
				return false;
		}
	}
	return true;
}

static void nvkvm_discover_host_gpus(void)
{
	nvkvm_host_gpu_count = 0;
	DIR *d = opendir("/proc/driver/nvidia/gpus");
	if (!d)
		return;
	struct dirent *de;
	while ((de = readdir(d)) != NULL &&
	       nvkvm_host_gpu_count < NVKVM_MAX_HOST_GPUS) {
		if (!nvkvm_bdf_valid(de->d_name))
			continue;
		memcpy(nvkvm_host_bdf[nvkvm_host_gpu_count], de->d_name,
		       NVKVM_BDF_LEN + 1);
		nvkvm_host_gpu_count++;
	}
	closedir(d);
	/* readdir order is arbitrary; sort so gpu_index is stable across calls. */
	for (int i = 0; i < nvkvm_host_gpu_count; i++)
		for (int j = i + 1; j < nvkvm_host_gpu_count; j++)
			if (strcmp(nvkvm_host_bdf[j], nvkvm_host_bdf[i]) < 0) {
				char tmp[NVKVM_BDF_LEN + 1];
				memcpy(tmp, nvkvm_host_bdf[i], sizeof(tmp));
				memcpy(nvkvm_host_bdf[i], nvkvm_host_bdf[j], sizeof(tmp));
				memcpy(nvkvm_host_bdf[j], tmp, sizeof(tmp));
			}
}

/* Build the host path for a host-file request into `buf`.  Per-GPU files
 * resolve `gpu_index` against the discovered BDF list (never guest input).
 * Returns false if the id is unknown or the index is out of range. */
static bool nvkvm_hfile_path(uint32_t id, uint32_t gpu_index,
			     char *buf, size_t buflen)
{
	switch (id) {
	case NVKVM_HFILE_NVIDIA_PARAMS:
		return g_strlcpy(buf, "/proc/driver/nvidia/params", buflen) < buflen;
	case NVKVM_HFILE_NVIDIA_VERSION:
		/* The NVRM banner.  Read by nvidia-container-toolkit and by most
		 * driver-detection scripts; without it they conclude no driver is
		 * present.  It carries the driver version and build date, which the
		 * guest already knows -- nvidia-smi reports the same version, and the
		 * guest userspace has to be version-matched to the host to work at
		 * all -- so this discloses nothing new.
		 *
		 * Deliberately NOT exposed, because these do disclose host state the
		 * guest has no need for:
		 *   registry            host-wide driver parameter overrides
		 *   capabilities/       MIG and fabric-imex topology
		 *   suspend[_depth]     host power state, and writable
		 *   warnings/           host driver warnings
		 * Add a case here only with the same argument: the guest already had
		 * the information, or it cannot act on it. */
		return g_strlcpy(buf, "/proc/driver/nvidia/version", buflen) < buflen;
	case NVKVM_HFILE_NVIDIA_INITSTATE:
		return g_strlcpy(buf, "/sys/module/nvidia/initstate", buflen) < buflen;
	case NVKVM_HFILE_NVIDIA_UVM_INITSTATE:
		return g_strlcpy(buf, "/sys/module/nvidia_uvm/initstate", buflen) < buflen;
	case NVKVM_HFILE_NVIDIA_NUMA_STATUS:
	case NVKVM_HFILE_NVIDIA_INFORMATION:
	case NVKVM_HFILE_NVIDIA_REG_BASE: {
		if (nvkvm_host_gpu_count < 0)
			nvkvm_discover_host_gpus();
		if (gpu_index >= (uint32_t)nvkvm_host_gpu_count)
			return false;
		const char *leaf = (id == NVKVM_HFILE_NVIDIA_NUMA_STATUS) ? "numa_status"
				 : (id == NVKVM_HFILE_NVIDIA_INFORMATION)  ? "information"
				 :                                            "registry";
		int n = snprintf(buf, buflen, "/proc/driver/nvidia/gpus/%s/%s",
				 nvkvm_host_bdf[gpu_index], leaf);
		return n > 0 && (size_t)n < buflen;
	}
	default:
		return false;
	}
}

int nvkvm_req_read_host_file(VirtIONvgpu *nv,
			      struct nvkvm_req_read_host_file *req,
			      struct nvkvm_resp_read_host_file *resp,
			      void *shm_buf)
{
	(void)nv;
	memset(resp, 0, sizeof(*resp));

	if (!shm_buf || req->max_len == 0 ||
	    req->max_len > NVKVM_HFILE_MAX_SIZE) {
		resp->status = EINVAL;
		return 0;
	}

	char path[256];
	if (!nvkvm_hfile_path(req->file_id, req->gpu_index, path, sizeof(path))) {
		resp->status = EINVAL;
		return 0;
	}

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		resp->status = (uint32_t)errno;
		return 0;
	}

	uint32_t total = 0;
	while (total < req->max_len) {
		ssize_t n = read(fd, (char *)shm_buf + total,
				 (size_t)(req->max_len - total));
		if (n < 0) {
			if (errno == EINTR) continue;
			resp->status = (uint32_t)errno;
			close(fd);
			return 0;
		}
		if (n == 0) break;
		total += (uint32_t)n;
	}
	close(fd);

	resp->status = 0;
	resp->nbytes = total;
	return 0;
}

/* ── REALIZE_UVM_MAPPING ─────────────────────────────────────────────────────
 *
 * STATE_MACHINE_PLAN §8a — strict validation.  This handler runs in QEMU
 * (privileged) on behalf of a guest that we treat as adversarial.
 *
 * Threat model: any field can be attacker-controlled.  We must:
 *   1. Bound every count / size against caps defined in nvkvm_proto.h.
 *   2. Sanitize flags — strip everything outside the allowlist.
 *   3. Sanitize prot — strip everything outside R/W (no exec on GPU mmaps).
 *   4. For mode SEM_POOL: validate intent_size == sizeof(SEM_POOL_PARAMS).
 *   5. Validate the intent struct's base/length match req.length so a
 *      malicious guest can't trick the kernel into mapping the wrong VA.
 *   6. Allocate a fresh KVM GPA window — never trust offsets.
 *   7. Forward exact validated state+intent to the stub.
 */
struct nvkvm_kvm_mem_region_rl {
	uint32_t slot;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
};
#define NVKVM_KVMIO_RL  0xAE
#define KVM_SET_USER_MEMORY_REGION_RL \
	_IOW(NVKVM_KVMIO_RL, 0x46, struct nvkvm_kvm_mem_region_rl)

#define NVKVM_REALIZE_PROT_MASK    (PROT_READ | PROT_WRITE)
#define NVKVM_REALIZE_FLAGS_MASK   (MAP_SHARED | MAP_PRIVATE)
/* Cap intent blob: SEM_POOL is 9248 bytes; allow a small margin. */
#define NVKVM_REALIZE_INTENT_MAX   (64u * 1024u)

/* realize_kvm_slot_counter superseded by nvkvm_kvm_slot_alloc().  Audit L5. */

int nvkvm_req_realize_uvm_mapping(VirtIONvgpu *nv,
				   struct nvkvm_req_realize_uvm_mapping *req,
				   struct nvkvm_resp_realize_uvm_mapping *resp,
				   void *state_buf, void *intent_buf)
{
	memset(resp, 0, sizeof(*resp));

	/*
	 * S-2: REALIZE_UVM_MAPPING is MMAP_ON_ISOLATE's sibling -- it hands a
	 * guest-named isolate a mapping made from a guest-named fd handle -- but
	 * it was the one handler of the pair that took the guest's word for the
	 * pairing.  It read req->isolate_id straight through to
	 * nvkvm_isolate_realize_uvm_fd() and never looked at req->session_id or
	 * req->fd_handle_id at all, even though the guest already puts both on
	 * the wire (nvkvm_virtio.c:1546-1551).  So one isolate could drive a UVM
	 * realize -- and burn GPA window space, see the iso_mmap_alloc note
	 * below -- inside ANY other isolate in the VM, naming an fd handle it
	 * does not own.
	 *
	 * Same two independent assertions MMAP_ON_ISOLATE checks, in the same
	 * order and with the same answer: the handle must belong to the session
	 * the caller claims, and that session must own the isolate named.
	 * h->session_id is QEMU's own bookkeeping, not the guest's word for it.
	 */
	struct nvkvm_handle *fh = nvkvm_handle_get(&nv->handles, req->fd_handle_id);
	if (!fh ||
	    fh->session_id != req->session_id ||
	    !session_has_isolate(nv, req->session_id, req->isolate_id)) {
		NVKVM_DBG("nvkvm: realize_uvm: handle/isolate session mismatch "
			  "(fd_h=%u h_sess=%u req_sess=%u iso=%u)\n",
			  req->fd_handle_id, fh ? fh->session_id : 0,
			  req->session_id, req->isolate_id);
		resp->status = (uint32_t)-EPERM;
		return 0;
	}

	/* §8a.1 — pointer presence. */
	if (!state_buf || !intent_buf) {
		resp->status = (uint32_t)-EINVAL;
		return 0;
	}

	/* §8a.1 — state size is a fixed cap-bound struct. */
	const size_t state_size =
		sizeof(struct nvkvm_uvm_state_snapshot);

	/* §8a.4 — intent size bound + mode-specific exact match. */
	if (req->intent_size == 0 ||
	    req->intent_size > NVKVM_REALIZE_INTENT_MAX) {
		resp->status = (uint32_t)-EINVAL;
		return 0;
	}

	/* §8a.5 — validate per-mode intent shape. */
	struct nvkvm_uvm_state_snapshot *snap = state_buf;
	uint32_t n_gpus = le32_to_cpu(snap->n_gpus);
	uint32_t n_va_spaces = le32_to_cpu(snap->n_va_spaces);
	uint32_t n_range_groups = le32_to_cpu(snap->n_range_groups);
	if (n_gpus > NVKVM_UVM_MAX_REG_GPUS ||
	    n_va_spaces > NVKVM_UVM_MAX_VA_SPACES ||
	    n_range_groups > NVKVM_UVM_MAX_RANGE_GROUPS) {
		resp->status = (uint32_t)-EINVAL;
		return 0;
	}

	/* The snapshot is guest-controlled.  REGISTER_GPU and
	 * REGISTER_GPU_VASPACE both embed an RM control fd, represented on the wire
	 * by a QEMU handle id.  Prove every non-sentinel handle is a live nvidiactl
	 * handle owned by this session before the stub resolves it; otherwise a
	 * forged REALIZE request could borrow another session's RM client. */
	for (uint32_t i = 0; i < n_gpus; i++) {
		uint32_t hid = le32_to_cpu(snap->gpus[i].rm_ctrl_fd_handle_id);
		struct nvkvm_handle *h;

		if (hid == (uint32_t)-1)
			continue;
		h = nvkvm_handle_get(&nv->handles, hid);
		if (!h || h->session_id != req->session_id ||
		    h->type != NVKVM_HANDLE_TYPE_NVIDIA || h->dev_id != NVKVM_DEV_CTL) {
			resp->status = (uint32_t)-EPERM;
			return 0;
		}
	}
	for (uint32_t i = 0; i < n_va_spaces; i++) {
		uint32_t hid = le32_to_cpu(
			snap->va_spaces[i].rm_ctrl_fd_handle_id);
		struct nvkvm_handle *h;

		if (hid == (uint32_t)-1)
			continue;
		h = nvkvm_handle_get(&nv->handles, hid);
		if (!h || h->session_id != req->session_id ||
		    h->type != NVKVM_HANDLE_TYPE_NVIDIA || h->dev_id != NVKVM_DEV_CTL) {
			resp->status = (uint32_t)-EPERM;
			return 0;
		}
	}

	switch (req->mode) {
	case NVKVM_UVM_REALIZE_MODE_SEM_POOL: {
		if (req->intent_size !=
		    sizeof(struct uvm_alloc_semaphore_pool_params)) {
			resp->status = (uint32_t)-EINVAL;
			return 0;
		}
		struct uvm_alloc_semaphore_pool_params *p = intent_buf;
		if (p->length != req->length || p->base != req->gva) {
			resp->status = (uint32_t)-EINVAL;
			return 0;
		}
		p->rm_status = 0;
		break;
	}
	default:
		resp->status = (uint32_t)-ENOTSUP;
		return 0;
	}

	/* §8a.2/3 — sanitize prot+flags.  Strip anything outside allowlist. */
	uint32_t prot      = req->prot      & (uint32_t)NVKVM_REALIZE_PROT_MASK;
	uint32_t map_flags = req->map_flags & (uint32_t)NVKVM_REALIZE_FLAGS_MASK;
	if (prot == 0)
		prot = PROT_READ | PROT_WRITE;
	if ((map_flags & (MAP_SHARED | MAP_PRIVATE)) == 0)
		map_flags |= MAP_SHARED;

	/* Length must be page-aligned and within sane bounds. */
	uint64_t len = req->length;
	if (len == 0 || len > (1ULL << 40) || (len & 4095ULL)) {
		resp->status = (uint32_t)-EINVAL;
		return 0;
	}

	/* §8a.6 — allocate a fresh GPA from the single sparse window so the
	 * guest (which validates every returned GPA against that window)
	 * accepts it.  No per-mmap memslot is installed for realize — see the
	 * note below; the GPA rides the sparse window's pre-installed memslot
	 * (anonymous backing), matching the proven v0.1 behaviour. */
	uint64_t gpa = nvkvm_sparse_gpa_alloc(nv, (size_t)len);
	if (gpa == 0) {
		resp->status = (uint32_t)-ENOMEM;
		return 0;
	}

	/* §8a.7 — send to stub.  Stub does the actual /dev/nvidia-uvm work
	 * inside the isolate's mm. */
	uint64_t host_va = 0, out_len = 0, token = 0;
	uint32_t rm_status = 0;
	int ret = nvkvm_isolate_realize_uvm_fd(&nv->isolates,
					       req->isolate_id,
					       req->mode,
					       state_buf, (uint32_t)state_size,
					       intent_buf, req->intent_size,
					       prot, map_flags,
					       len, /*host_va_hint=*/0,
					       /*offset=*/0,
					       &host_va, &out_len,
					       &token, &rm_status);
	if (ret < 0 || host_va == 0) {
		/* FF-3 (security_audit_2026_06_01): free the window extent on the
		 * error path — otherwise every failed realize leaks GPA space. */
		nvkvm_sparse_gpa_free(nv, gpa, (size_t)len);
		resp->status    = (uint32_t)-ret;
		resp->rm_status = rm_status;
		return 0;
	}
	if (rm_status != 0) {
		/* Kernel rejected the intent — host_va may still be set if the
		 * mmap succeeded but a later step failed.  Treat as failure. */
		nvkvm_sparse_gpa_free(nv, gpa, (size_t)len);   /* FF-3: no leak on error */
		resp->rm_status = rm_status;
		resp->status    = (uint32_t)-EIO;
		return 0;
	}

	/* §8a.6 — NO per-mmap KVM memslot: host_va is a stub-process VA,
	 * invalid as a QEMU KVM userspace_addr.  See security-fixes commit.
	 * Master masked this via slot=1100 > KVM cap (install failed). */
	(void)host_va;

	/* FF-3 (security_audit_2026_06_01): record the realize extent in
	 * iso_mmap_tbl so the #80 kill-reaper reclaims its GPA-window space when
	 * the isolate dies.  It rides the sparse window's pre-installed memslot
	 * (IN_WINDOW_SLOT) and has no standalone QEMU qva, so the reaper's
	 * in-window branch simply sparse_gpa_free()s it — no munmap/slot touch.
	 * Previously this allocation was never tracked or freed → unprivileged
	 * guest realize churn exhausted the 128 GiB window (VM-wide GPU DoS). */
	uint32_t mmap_token = iso_mmap_alloc(req->isolate_id, gpa,
					     /*stub_va=*/0,
					     /*qva=*/NULL, (size_t)len,
					     NVKVM_IN_WINDOW_SLOT, gpa,
					     /*stub_mirrored=*/false,
					     /*handle_id=*/0);
	if (mmap_token == 0) {
		/*
		 * Table full.  This return was discarded with a (void) cast,
		 * which silently reintroduced the exact leak the comment above
		 * exists to prevent: the extent goes untracked, so the reaper
		 * never reclaims it, and the guest gets a GPA it can never
		 * release.  Once iso_mmap_tbl is full, EVERY subsequent realize
		 * leaked another extent -- turning a full table into the
		 * window-exhaustion DoS rather than a bounded failure.  The
		 * MMAP_ON_ISOLATE path a few hundred lines up checks this token
		 * and unwinds; this one must too.
		 *
		 * Unwinding a realize is just the GPA extent: it rides the
		 * sparse window's pre-installed memslot, so there is no per-mmap
		 * memslot to remove and no standalone QEMU qva to munmap (that
		 * is why the reaper's in-window branch only sparse_gpa_free()s
		 * it).  The stub-side mapping lives in the isolate's own mm with
		 * stub_mirrored=false -- normal MUNMAP_ON_ISOLATE would not
		 * touch it either -- and is reclaimed when the isolate exits.
		 */
		fprintf(stderr, "nvkvm: iso_mmap_tbl full — undoing realize\n");
		nvkvm_sparse_gpa_free(nv, gpa, (size_t)len);
		resp->status = (uint32_t)-ENOMEM;
		return 0;
	}

	resp->gpa_base      = gpa;
	resp->length        = len;
	resp->realize_token = token;
	resp->rm_status     = 0;
	resp->status        = 0;
	return 0;
}
