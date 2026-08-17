/*
 * test_tables.c — unit tests for nvkvm_tables.c
 *
 * Covers the four-table model + GPA-window sub-allocator.
 * Uses pipe()/memfd_create only; no /dev/nvidia* dependency.
 *
 * What's exercised here (mapping to REFACTOR_PLAN.md R-table):
 *   - alloc/lookup/close + generation-id stale rejection (I4)
 *   - concurrent close vs in-flight ioctl (R3 / R13)
 *   - GPA allocator fragmentation + coalesce (R10)
 *   - punch-hole adjacency consolidation, double-punch no-op (R14)
 *   - cleanup-order refusal: close-handle while mmaps exist (I2)
 *   - cross-isolate handle clone first-use race (R12)
 *   - generation rollover smoke
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../src/qemu/nvkvm_tables.h"

/* ── Minimal test framework ───────────────────────────────────────────── */

static int tests_run = 0, tests_passed = 0, tests_failed = 0;
static int __cur_failed = 0;

#define ASSERT_EQ(a, b) do { \
	long long _a = (long long)(a), _b = (long long)(b); \
	if (_a != _b) { \
		fprintf(stderr, "  FAIL %s:%d: %s == %s  got %lld  want %lld\n", \
			__FILE__, __LINE__, #a, #b, _a, _b); \
		__cur_failed = 1; return; \
	} \
} while (0)
#define ASSERT_NE(a, b) do { \
	long long _a = (long long)(a), _b = (long long)(b); \
	if (_a == _b) { \
		fprintf(stderr, "  FAIL %s:%d: %s != %s  both %lld\n", \
			__FILE__, __LINE__, #a, #b, _a); \
		__cur_failed = 1; return; \
	} \
} while (0)
#define ASSERT_TRUE(e)  ASSERT_NE((int)(e), 0)
#define ASSERT_FALSE(e) ASSERT_EQ((int)(e), 0)

struct test { const char *name; void (*fn)(void); };
static struct test reg[256]; static int n_reg = 0;

#define TEST(name) \
	static void name(void); \
	__attribute__((constructor)) static void _reg_##name(void) { \
		reg[n_reg++] = (struct test){ #name, name }; \
	} \
	static void name(void)

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Open a stub fd we can hand to handle_attach_qemu_fd in tests.
 * Reuse memfd_create so we have a real, closable, dup-able fd. */
static int make_stub_fd(void)
{
	int fd = memfd_create("test-stub", MFD_CLOEXEC);
	assert(fd >= 0);
	return fd;
}

/* ── 1. Basic alloc + lookup + close ───────────────────────────────────── */

TEST(handle_alloc_then_attach_then_acquire)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);

	uint32_t id = 0;
	ASSERT_EQ(nvkvm_tables_handle_alloc(&t, NVKVM_HND_NVIDIACTL, &id), 0);
	ASSERT_NE(id, 0u);

	/* Not yet ready — acquire must fail with -EBUSY. */
	struct nvkvm_handle_entry *e = NULL;
	ASSERT_EQ(nvkvm_tables_handle_acquire(&t, id, &e), -EBUSY);

	int fd = make_stub_fd();
	ASSERT_EQ(nvkvm_tables_handle_attach_qemu_fd(&t, id, fd), 0);

	/* Now acquire works. */
	ASSERT_EQ(nvkvm_tables_handle_acquire(&t, id, &e), 0);
	ASSERT_EQ(e->handle_id, id);
	ASSERT_EQ(e->qemu_fd, fd);
	ASSERT_EQ(e->type, NVKVM_HND_NVIDIACTL);
	ASSERT_TRUE(e->ready);
	ASSERT_EQ(e->refcount, 1u);

	nvkvm_tables_handle_release(&t, id);
	ASSERT_EQ(nvkvm_tables_handle_close(&t, id), 0);

	/* Lookup after close fails. */
	ASSERT_EQ(nvkvm_tables_handle_acquire(&t, id, &e), -ENOENT);

	nvkvm_tables_fini(&t);
}

TEST(handle_attach_twice_rejected)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	uint32_t id; nvkvm_tables_handle_alloc(&t, NVKVM_HND_NVIDIACTL, &id);
	ASSERT_EQ(nvkvm_tables_handle_attach_qemu_fd(&t, id, make_stub_fd()), 0);
	ASSERT_EQ(nvkvm_tables_handle_attach_qemu_fd(&t, id, make_stub_fd()),
		   -EALREADY);
	nvkvm_tables_handle_close(&t, id);
	nvkvm_tables_fini(&t);
}

TEST(handle_abort_open_rolls_back)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	uint32_t id; nvkvm_tables_handle_alloc(&t, NVKVM_HND_NVIDIACTL, &id);
	/* Caller decides open failed; abort. */
	ASSERT_EQ(nvkvm_tables_handle_abort_open(&t, id), 0);
	struct nvkvm_handle_entry *e;
	ASSERT_EQ(nvkvm_tables_handle_acquire(&t, id, &e), -ENOENT);
	nvkvm_tables_fini(&t);
}

/* ── 2. Generation-id stale rejection (I4) ─────────────────────────────── */

TEST(stale_id_after_reuse_fails_lookup)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);

	uint32_t id1; nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &id1);
	nvkvm_tables_handle_attach_qemu_fd(&t, id1, make_stub_fd());
	nvkvm_tables_handle_close(&t, id1);

	/* Fill up to the first cycle so we reuse a slot. */
	uint32_t id2 = 0;
	for (int i = 0; i < NVKVM_TABLES_MAX_HANDLES + 8; i++) {
		nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &id2);
		nvkvm_tables_handle_attach_qemu_fd(&t, id2, make_stub_fd());
		nvkvm_tables_handle_close(&t, id2);
	}
	/* id1 must remain unrecoverable forever even though its slot has
	 * been reused. */
	struct nvkvm_handle_entry *e;
	ASSERT_EQ(nvkvm_tables_handle_acquire(&t, id1, &e), -ENOENT);

	nvkvm_tables_fini(&t);
}

/* ── 3. close-while-mmap-exists is refused (I2) ────────────────────────── */

TEST(close_handle_with_mmap_rejected)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	uint32_t win;
	ASSERT_EQ(nvkvm_tables_window_create(&t, 64 * 1024 * 1024, -1, &win),
		   0);

	uint32_t id; nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &id);
	nvkvm_tables_handle_attach_qemu_fd(&t, id, make_stub_fd());

	uint64_t gpa; uint32_t mid;
	ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, id, 0, 4096,
					   PROT_READ | PROT_WRITE,
					   &gpa, &mid), 0);

	/* Close while mmap exists → -EBUSY. */
	ASSERT_EQ(nvkvm_tables_handle_close(&t, id), -EBUSY);

	ASSERT_EQ(nvkvm_tables_mmap_free(&t, mid), 0);
	ASSERT_EQ(nvkvm_tables_handle_close(&t, id), 0);

	nvkvm_tables_window_destroy(&t, win, -1);
	nvkvm_tables_fini(&t);
}

TEST(close_handle_with_iso_link_rejected)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);

	uint32_t hnd; nvkvm_tables_handle_alloc(&t, NVKVM_HND_NVIDIACTL, &hnd);
	nvkvm_tables_handle_attach_qemu_fd(&t, hnd, make_stub_fd());

	uint32_t iso;
	int comm[2]; pipe(comm);
	nvkvm_tables_isolate_alloc(&t, 1234, comm[0], &iso);

	ASSERT_EQ(nvkvm_tables_iso_hnd_link(&t, iso, hnd, 42), 0);
	ASSERT_EQ(nvkvm_tables_handle_close(&t, hnd), -EBUSY);

	ASSERT_EQ(nvkvm_tables_iso_hnd_unlink(&t, iso, hnd), 0);
	ASSERT_EQ(nvkvm_tables_handle_close(&t, hnd), 0);

	nvkvm_tables_isolate_close(&t, iso);
	close(comm[1]);
	nvkvm_tables_fini(&t);
}

/* ── 4. Concurrent close vs in-flight op (R3 / R13) ────────────────────── */

struct close_race_args {
	struct nvkvm_tables *t;
	uint32_t id;
	atomic_int acquired;
	atomic_int close_done;
};

static void *holder_thread(void *p)
{
	struct close_race_args *a = p;
	struct nvkvm_handle_entry *e;
	int r = nvkvm_tables_handle_acquire(a->t, a->id, &e);
	if (r != 0) return (void *)(intptr_t)r;
	atomic_store(&a->acquired, 1);
	/* Simulate slow ioctl. */
	for (int i = 0; i < 100; i++) {
		usleep(1000);
		if (atomic_load(&a->close_done)) {
			/* Close must NOT have completed while we hold ref. */
			fprintf(stderr,
				"  FAIL: close completed while holder had ref\n");
			__cur_failed = 1;
		}
	}
	nvkvm_tables_handle_release(a->t, a->id);
	return NULL;
}

TEST(close_drains_inflight_refcount)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	uint32_t id; nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &id);
	nvkvm_tables_handle_attach_qemu_fd(&t, id, make_stub_fd());

	struct close_race_args a = { .t = &t, .id = id };
	pthread_t th;
	pthread_create(&th, NULL, holder_thread, &a);
	while (!atomic_load(&a.acquired))
		usleep(100);

	/* Now spawn a close — it should block until holder releases. */
	pthread_t th_close;
	void *closer(void *_)
	{
		(void)_;
		nvkvm_tables_handle_close(&t, id);
		atomic_store(&a.close_done, 1);
		return NULL;
	}
	pthread_create(&th_close, NULL, closer, NULL);

	pthread_join(th, NULL);
	pthread_join(th_close, NULL);
	ASSERT_TRUE(atomic_load(&a.close_done));

	struct nvkvm_handle_entry *e;
	ASSERT_EQ(nvkvm_tables_handle_acquire(&t, id, &e), -ENOENT);

	nvkvm_tables_fini(&t);
}

TEST(acquire_during_close_returns_ebusy)
{
	/* A close that has started (ready=false) but is waiting for an
	 * in-flight ref must reject new acquirers with -EBUSY, never
	 * -ENOENT (the handle still logically exists). */
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	uint32_t id; nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &id);
	nvkvm_tables_handle_attach_qemu_fd(&t, id, make_stub_fd());

	struct nvkvm_handle_entry *e1;
	ASSERT_EQ(nvkvm_tables_handle_acquire(&t, id, &e1), 0);

	/* Start a close in another thread; it'll block on refcount. */
	atomic_int close_started = 0, close_done = 0;
	void *closer(void *_)
	{
		(void)_;
		atomic_store(&close_started, 1);
		nvkvm_tables_handle_close(&t, id);
		atomic_store(&close_done, 1);
		return NULL;
	}
	pthread_t th; pthread_create(&th, NULL, closer, NULL);
	while (!atomic_load(&close_started)) usleep(100);
	/* Give close time to mark ready=false. */
	usleep(50 * 1000);

	struct nvkvm_handle_entry *e2;
	int r = nvkvm_tables_handle_acquire(&t, id, &e2);
	ASSERT_EQ(r, -EBUSY);
	ASSERT_FALSE(atomic_load(&close_done));

	nvkvm_tables_handle_release(&t, id);
	pthread_join(th, NULL);
	nvkvm_tables_fini(&t);
}

/* ── 5. GPA allocator fragmentation + coalesce (R10) ──────────────────── */

TEST(gpa_alloc_basic)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	uint32_t win;
	ASSERT_EQ(nvkvm_tables_window_create(&t, 16 * 1024 * 1024, -1, &win),
		   0);
	uint32_t hnd; nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &hnd);
	nvkvm_tables_handle_attach_qemu_fd(&t, hnd, make_stub_fd());

	uint64_t gpa1, gpa2, gpa3;
	uint32_t m1, m2, m3;
	ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, hnd, 0, 4096, 7, &gpa1, &m1), 0);
	ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, hnd, 0, 8192, 7, &gpa2, &m2), 0);
	ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, hnd, 0, 4096, 7, &gpa3, &m3), 0);
	/* Sequential GPAs */
	ASSERT_EQ(gpa2, gpa1 + 4096);
	ASSERT_EQ(gpa3, gpa2 + 8192);

	/* Free middle, then re-alloc 8 KiB — should reuse the freed slot. */
	ASSERT_EQ(nvkvm_tables_mmap_free(&t, m2), 0);
	uint64_t gpa4; uint32_t m4;
	ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, hnd, 0, 8192, 7, &gpa4, &m4), 0);
	ASSERT_EQ(gpa4, gpa1 + 4096);

	nvkvm_tables_mmap_free(&t, m1);
	nvkvm_tables_mmap_free(&t, m3);
	nvkvm_tables_mmap_free(&t, m4);
	nvkvm_tables_handle_close(&t, hnd);
	nvkvm_tables_window_destroy(&t, win, -1);
	nvkvm_tables_fini(&t);
}

TEST(gpa_alloc_churn_coalesces)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	uint32_t win;
	ASSERT_EQ(nvkvm_tables_window_create(&t, 4 * 1024 * 1024, -1, &win),
		   0);
	uint32_t hnd; nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &hnd);
	nvkvm_tables_handle_attach_qemu_fd(&t, hnd, make_stub_fd());

	enum { N = 256 };
	uint64_t gpa[N]; uint32_t mid[N];
	for (int i = 0; i < N; i++)
		ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, hnd, 0, 4096, 7,
						   &gpa[i], &mid[i]), 0);
	/* Free in scrambled order. */
	for (int i = 0; i < N; i++) {
		int j = (i * 37 + 11) % N;
		if (mid[j]) {
			ASSERT_EQ(nvkvm_tables_mmap_free(&t, mid[j]), 0);
			mid[j] = 0;
		}
	}
	/* After full free, a 4 MiB allocation should succeed (coalesce ok). */
	uint64_t gpa_big; uint32_t mid_big;
	ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, hnd, 0, 4 * 1024 * 1024, 7,
					   &gpa_big, &mid_big), 0);

	nvkvm_tables_mmap_free(&t, mid_big);
	nvkvm_tables_handle_close(&t, hnd);
	nvkvm_tables_window_destroy(&t, win, -1);
	nvkvm_tables_fini(&t);
}

TEST(gpa_window_exhaustion)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	uint32_t win;
	ASSERT_EQ(nvkvm_tables_window_create(&t, 16 * 1024, -1, &win), 0);
	uint32_t hnd; nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &hnd);
	nvkvm_tables_handle_attach_qemu_fd(&t, hnd, make_stub_fd());

	uint64_t gpa; uint32_t m;
	ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, hnd, 0, 8192, 7, &gpa, &m), 0);
	ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, hnd, 0, 8192, 7, &gpa, &m), 0);
	/* Window is 16 KiB, both 8 KiB alloc'd → next must -ENOSPC. */
	uint64_t gpa3; uint32_t m3;
	ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, hnd, 0, 8192, 7, &gpa3, &m3),
		   -ENOSPC);

	nvkvm_tables_handle_close(&t, hnd);
	nvkvm_tables_window_destroy(&t, win, -1);
	nvkvm_tables_fini(&t);
}

/* ── 6. Cross-isolate iso↔hnd link M:N ─────────────────────────────────── */

TEST(iso_hnd_link_multiple_isolates)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);

	uint32_t hnd; nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &hnd);
	nvkvm_tables_handle_attach_qemu_fd(&t, hnd, make_stub_fd());

	int p[2]; pipe(p);
	uint32_t iso1, iso2;
	nvkvm_tables_isolate_alloc(&t, 1, p[0], &iso1);
	nvkvm_tables_isolate_alloc(&t, 2, p[1], &iso2);

	ASSERT_EQ(nvkvm_tables_iso_hnd_link(&t, iso1, hnd, 10), 0);
	ASSERT_EQ(nvkvm_tables_iso_hnd_link(&t, iso2, hnd, 20), 0);
	ASSERT_EQ(nvkvm_tables_iso_hnd_link(&t, iso1, hnd, 10), -EEXIST);

	int fd;
	ASSERT_EQ(nvkvm_tables_iso_hnd_lookup_fd(&t, iso1, hnd, &fd), 0);
	ASSERT_EQ(fd, 10);
	ASSERT_EQ(nvkvm_tables_iso_hnd_lookup_fd(&t, iso2, hnd, &fd), 0);
	ASSERT_EQ(fd, 20);
	ASSERT_EQ(nvkvm_tables_iso_hnd_count_for_handle(&t, hnd), 2);

	nvkvm_tables_iso_hnd_unlink(&t, iso1, hnd);
	nvkvm_tables_iso_hnd_unlink(&t, iso2, hnd);
	nvkvm_tables_isolate_close(&t, iso1);
	nvkvm_tables_isolate_close(&t, iso2);
	nvkvm_tables_handle_close(&t, hnd);
	nvkvm_tables_fini(&t);
}

/* ── 7. Cross-isolate first-use race (R12) ─────────────────────────────── */

struct clone_race_args {
	struct nvkvm_tables *t;
	uint32_t isolate_id;
	uint32_t handle_id;
	pthread_barrier_t *start;
	int      stub_fd_to_use;
	atomic_int eexist_count;
	atomic_int ok_count;
};
static void *cloner(void *p)
{
	struct clone_race_args *a = p;
	pthread_barrier_wait(a->start);
	int r = nvkvm_tables_iso_hnd_link(a->t, a->isolate_id, a->handle_id,
					   a->stub_fd_to_use);
	if (r == 0) atomic_fetch_add(&a->ok_count, 1);
	else if (r == -EEXIST) atomic_fetch_add(&a->eexist_count, 1);
	return NULL;
}

TEST(iso_hnd_link_concurrent_first_use)
{
	/* Two threads "first-use" the same (iso, hnd) pair simultaneously.
	 * Exactly one wins; the other observes -EEXIST. */
	struct nvkvm_tables t; nvkvm_tables_init(&t);

	uint32_t hnd; nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &hnd);
	nvkvm_tables_handle_attach_qemu_fd(&t, hnd, make_stub_fd());
	int p[2]; pipe(p);
	uint32_t iso;
	nvkvm_tables_isolate_alloc(&t, 1, p[0], &iso);

	enum { TH = 16 };
	pthread_barrier_t bar; pthread_barrier_init(&bar, NULL, TH);
	pthread_t th[TH];
	struct clone_race_args a = { .t = &t, .isolate_id = iso,
				      .handle_id = hnd, .start = &bar,
				      .stub_fd_to_use = 99 };
	for (int i = 0; i < TH; i++)
		pthread_create(&th[i], NULL, cloner, &a);
	for (int i = 0; i < TH; i++) pthread_join(th[i], NULL);
	ASSERT_EQ(atomic_load(&a.ok_count), 1);
	ASSERT_EQ(atomic_load(&a.eexist_count), TH - 1);
	pthread_barrier_destroy(&bar);

	nvkvm_tables_iso_hnd_unlink(&t, iso, hnd);
	nvkvm_tables_isolate_close(&t, iso);
	nvkvm_tables_handle_close(&t, hnd);
	close(p[1]);
	nvkvm_tables_fini(&t);
}

/* ── 8. Punch-hole adjacency (R14) ─────────────────────────────────────── */

/* This test verifies the property described in the plan: when we free
 * an mmap, the free list coalesces with adjacent free regions, and a
 * subsequent allocator query for a region as large as the union must
 * succeed.  We can't directly inspect the punched-hole state of the
 * memfd from userspace easily, but successful re-allocation of the
 * coalesced range is the relevant observable. */
TEST(mmap_free_coalesces_adjacent)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	uint32_t win;
	ASSERT_EQ(nvkvm_tables_window_create(&t, 64 * 1024, -1, &win), 0);
	uint32_t hnd; nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &hnd);
	nvkvm_tables_handle_attach_qemu_fd(&t, hnd, make_stub_fd());

	uint64_t g[4]; uint32_t m[4];
	for (int i = 0; i < 4; i++)
		ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, hnd, 0, 8192, 7,
						   &g[i], &m[i]), 0);
	/* Free out of order so coalescing in both directions is exercised. */
	ASSERT_EQ(nvkvm_tables_mmap_free(&t, m[2]), 0);
	ASSERT_EQ(nvkvm_tables_mmap_free(&t, m[0]), 0);
	ASSERT_EQ(nvkvm_tables_mmap_free(&t, m[1]), 0);
	/* Now g[0..2] are all free. Allocator must accept a single 24 KiB
	 * request, proving the 3 adjacent 8 KiB ranges coalesced. */
	uint64_t big; uint32_t mid;
	ASSERT_EQ(nvkvm_tables_mmap_alloc(&t, hnd, 0, 24 * 1024, 7,
					   &big, &mid), 0);
	ASSERT_EQ(big, g[0]);

	nvkvm_tables_mmap_free(&t, m[3]);
	nvkvm_tables_mmap_free(&t, mid);
	nvkvm_tables_handle_close(&t, hnd);
	nvkvm_tables_window_destroy(&t, win, -1);
	nvkvm_tables_fini(&t);
}

TEST(mmap_double_free_is_enoent)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	uint32_t win;
	ASSERT_EQ(nvkvm_tables_window_create(&t, 64 * 1024, -1, &win), 0);
	uint32_t hnd; nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &hnd);
	nvkvm_tables_handle_attach_qemu_fd(&t, hnd, make_stub_fd());

	uint64_t g; uint32_t m;
	nvkvm_tables_mmap_alloc(&t, hnd, 0, 4096, 7, &g, &m);
	ASSERT_EQ(nvkvm_tables_mmap_free(&t, m), 0);
	ASSERT_EQ(nvkvm_tables_mmap_free(&t, m), -ENOENT);

	nvkvm_tables_handle_close(&t, hnd);
	nvkvm_tables_window_destroy(&t, win, -1);
	nvkvm_tables_fini(&t);
}

/* ── 9. Isolate kill mid-op (R6) ───────────────────────────────────────── */

TEST(isolate_mark_dead_unblocks_only_that_isolate)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	int p1[2], p2[2]; pipe(p1); pipe(p2);
	uint32_t iso1, iso2;
	nvkvm_tables_isolate_alloc(&t, 100, p1[0], &iso1);
	nvkvm_tables_isolate_alloc(&t, 200, p2[0], &iso2);
	ASSERT_EQ(nvkvm_tables_isolate_mark_dead(&t, iso1), 0);

	/* iso2 must still be alive. */
	pthread_mutex_lock(&t.isolates.lock);
	bool iso1_alive = t.isolates.slots[1].alive;
	bool iso2_alive = t.isolates.slots[2].alive;
	pthread_mutex_unlock(&t.isolates.lock);
	ASSERT_FALSE(iso1_alive);
	ASSERT_TRUE(iso2_alive);

	nvkvm_tables_isolate_close(&t, iso1);
	nvkvm_tables_isolate_close(&t, iso2);
	close(p1[1]); close(p2[1]);
	nvkvm_tables_fini(&t);
}

/* ── 10. Generation rollover smoke ─────────────────────────────────────── */

TEST(generation_rollover_smoke)
{
	struct nvkvm_tables t; nvkvm_tables_init(&t);
	/* Force gen counter to advance many times for slot 1 by allocating
	 * one handle, closing, repeatedly. After enough rounds, the gen
	 * value packs into the high bits of the id without collisions. */
	uint32_t prev = 0;
	for (int i = 0; i < 1000; i++) {
		uint32_t id;
		ASSERT_EQ(nvkvm_tables_handle_alloc(&t, NVKVM_HND_MEMFD, &id), 0);
		ASSERT_NE(id, prev);
		nvkvm_tables_handle_attach_qemu_fd(&t, id, make_stub_fd());
		nvkvm_tables_handle_close(&t, id);
		prev = id;
	}
	nvkvm_tables_fini(&t);
}

/* ── Driver ────────────────────────────────────────────────────────────── */

int main(void)
{
	for (int i = 0; i < n_reg; i++) {
		__cur_failed = 0;
		tests_run++;
		printf("[ RUN  ] %s\n", reg[i].name);
		reg[i].fn();
		if (__cur_failed) { printf("[ FAIL ] %s\n", reg[i].name); tests_failed++; }
		else              { printf("[ PASS ] %s\n", reg[i].name); tests_passed++; }
	}
	printf("\n%d/%d tests passed", tests_passed, tests_run);
	if (tests_failed) printf(", %d FAILED", tests_failed);
	printf("\n");
	return tests_failed ? 1 : 0;
}
