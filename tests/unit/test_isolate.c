/*
 * test_isolate.c — unit tests for nvkvm_isolate.c (multi-inflight IOCTL path)
 *
 * Spawns mock_stub as the isolate process (NVKVM_STUB_PATH must point to it).
 * Tests:
 *   1. Lifecycle — create / kill
 *   2. Sequential IOCTL forwarding — retval and nvstatus correctness
 *   3. Concurrent IOCTLs — N threads all complete without races
 *   4. Out-of-order responses — mock_stub delays some IOCTLs so responses
 *      arrive out of order; txn_id matching must still be correct
 *   5. Kill while IOCTL in-flight — callers wake with transport error
 *   6. Sync commands (mmap, munmap, poll, unpoll) — sequential OK responses
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <poll.h>

/* Stub QEMU osdep for the isolate implementation */
#include "stubs/qemu/osdep.h"

#include "../../src/qemu/nvkvm_handle.h"
#include "../../src/qemu/nvkvm_isolate.h"

/* ── Minimal test framework ───────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
	static void test_##name(void); \
	__attribute__((constructor)) static void register_##name(void) { \
		(void)test_##name; } \
	static void test_##name(void)

#define RUN(name) do { \
	tests_run++; \
	fprintf(stderr, "[ RUN  ] " #name "\n"); \
	test_##name(); \
	tests_passed++; \
	fprintf(stderr, "[ PASS ] " #name "\n"); \
} while (0)

#define EXPECT_EQ(a, b) do { \
	long long _a = (long long)(a); \
	long long _b = (long long)(b); \
	if (_a != _b) { \
		fprintf(stderr, "  FAIL %s:%d: " #a " (%lld) != " #b " (%lld)\n", \
			__FILE__, __LINE__, _a, _b); \
		tests_failed++; tests_passed--; return; \
	} \
} while (0)

#define EXPECT_GE(a, b) do { \
	long long _a = (long long)(a); \
	long long _b = (long long)(b); \
	if (_a < _b) { \
		fprintf(stderr, "  FAIL %s:%d: " #a " (%lld) < " #b " (%lld)\n", \
			__FILE__, __LINE__, _a, _b); \
		tests_failed++; tests_passed--; return; \
	} \
} while (0)

/* ── Global fixture ───────────────────────────────────────────────────────── */

static struct nvkvm_handle_table  g_ht;
static struct nvkvm_isolate_table g_it;

static void fixture_init(void)
{
	nvkvm_handle_table_init(&g_ht);
	nvkvm_isolate_table_init(&g_it, &g_ht);
}

static void fixture_fini(void)
{
	nvkvm_isolate_table_fini(&g_it);
	nvkvm_handle_table_fini(&g_ht);
}

/* ── Test 1: lifecycle ───────────────────────────────────────────────────── */

TEST(lifecycle)
{
	fixture_init();

	uint32_t iso_id = 0;
	int ret = nvkvm_isolate_create(&g_it, /*session_id=*/1, NULL, &iso_id);
	EXPECT_EQ(ret, 0);
	EXPECT_GE(iso_id, 1);

	ret = nvkvm_isolate_kill(&g_it, iso_id);
	EXPECT_EQ(ret, 0);

	/* Double-kill must return ENOENT */
	ret = nvkvm_isolate_kill(&g_it, iso_id);
	EXPECT_EQ(ret, -ENOENT);

	fixture_fini();
}

/* ── Test 2: sequential IOCTL ────────────────────────────────────────────── */

TEST(sequential_ioctl)
{
	fixture_init();

	uint32_t iso_id = 0;
	EXPECT_EQ(nvkvm_isolate_create(&g_it, 1, NULL, &iso_id), 0);

	uint8_t params[16] = { 0 };
	uint32_t nvstatus  = 0xdead;
	uint64_t fault     = 0xbeef;

	int ret = nvkvm_isolate_ioctl(&g_it, iso_id, /*handle_id=*/0,
				      /*cmd=*/0xc020, params, sizeof(params),
				      NULL, 0, /*flags=*/0,
				      &nvstatus, &fault);
	EXPECT_EQ(ret, 0);
	EXPECT_EQ(nvstatus, 0);   /* stub returns 0 */
	EXPECT_EQ(fault, 0);      /* stub returns 0 */

	nvkvm_isolate_kill(&g_it, iso_id);
	fixture_fini();
}

/* ── Test 3: concurrent IOCTLs ───────────────────────────────────────────── */

#define CONCURRENT_THREADS  16

struct ioctl_thread_arg {
	uint32_t iso_id;
	int      thread_idx;
	int      result;
};

static void *ioctl_thread(void *arg)
{
	struct ioctl_thread_arg *a = arg;
	uint8_t params[8] = { 0 };
	uint32_t nvstatus = 0;
	uint64_t fault    = 0;

	a->result = nvkvm_isolate_ioctl(&g_it, a->iso_id,
					/*handle_id=*/0,
					/*cmd=*/0xdeadbeef,
					params, sizeof(params),
					NULL, 0, 0,
					&nvstatus, &fault);
	return NULL;
}

TEST(concurrent_ioctl)
{
	fixture_init();

	uint32_t iso_id = 0;
	EXPECT_EQ(nvkvm_isolate_create(&g_it, 2, NULL, &iso_id), 0);

	pthread_t threads[CONCURRENT_THREADS];
	struct ioctl_thread_arg args[CONCURRENT_THREADS];

	for (int i = 0; i < CONCURRENT_THREADS; i++) {
		args[i].iso_id     = iso_id;
		args[i].thread_idx = i;
		args[i].result     = -999;
		pthread_create(&threads[i], NULL, ioctl_thread, &args[i]);
	}
	for (int i = 0; i < CONCURRENT_THREADS; i++)
		pthread_join(threads[i], NULL);

	for (int i = 0; i < CONCURRENT_THREADS; i++)
		EXPECT_EQ(args[i].result, 0);

	nvkvm_isolate_kill(&g_it, iso_id);
	fixture_fini();
}

/* ── Test 4: out-of-order responses via mock_stub delay ─────────────────── */

/*
 * We send IOCTLs with handle_id values 0, 5, 10, 15, 20, ... (delay ms).
 * The stub delays each IOCTL by handle_id & 0xff milliseconds.
 * Responses arrive out of order; txn_id matching must route them correctly.
 * All callers must still return 0 (success) with no hang.
 */
#define DELAY_THREADS  8

static void *delayed_ioctl_thread(void *arg)
{
	struct ioctl_thread_arg *a = arg;
	uint8_t params[4] = { 0 };
	uint32_t nvstatus = 0;
	uint64_t fault    = 0;
	/* Encode delay in low byte of handle_id */
	uint32_t delay_handle = (uint32_t)(a->thread_idx * 5);

	a->result = nvkvm_isolate_ioctl(&g_it, a->iso_id,
					delay_handle, /*cmd=*/1,
					params, sizeof(params),
					NULL, 0, 0,
					&nvstatus, &fault);
	return NULL;
}

TEST(out_of_order_ioctl)
{
	fixture_init();

	uint32_t iso_id = 0;
	EXPECT_EQ(nvkvm_isolate_create(&g_it, 3, NULL, &iso_id), 0);

	pthread_t threads[DELAY_THREADS];
	struct ioctl_thread_arg args[DELAY_THREADS];

	for (int i = 0; i < DELAY_THREADS; i++) {
		args[i].iso_id     = iso_id;
		args[i].thread_idx = i;
		args[i].result     = -999;
	}
	/* Start threads in reverse order so highest-delay launches first */
	for (int i = DELAY_THREADS - 1; i >= 0; i--)
		pthread_create(&threads[i], NULL, delayed_ioctl_thread, &args[i]);
	for (int i = 0; i < DELAY_THREADS; i++)
		pthread_join(threads[i], NULL);

	for (int i = 0; i < DELAY_THREADS; i++)
		EXPECT_EQ(args[i].result, 0);

	nvkvm_isolate_kill(&g_it, iso_id);
	fixture_fini();
}

/* ── Test 5: kill while IOCTL in-flight ──────────────────────────────────── */

struct kill_race_arg {
	uint32_t iso_id;
	int      result;
};

static void *slow_ioctl_thread(void *arg)
{
	struct kill_race_arg *a = arg;
	uint8_t params[4] = { 0 };
	uint32_t nvstatus = 0;
	uint64_t fault    = 0;
	/* Request max delay (255ms) so the kill races with the response */
	a->result = nvkvm_isolate_ioctl(&g_it, a->iso_id,
					/*handle_id=*/255, /*cmd=*/99,
					params, sizeof(params),
					NULL, 0, 0,
					&nvstatus, &fault);
	return NULL;
}

TEST(kill_during_inflight)
{
	fixture_init();

	uint32_t iso_id = 0;
	EXPECT_EQ(nvkvm_isolate_create(&g_it, 4, NULL, &iso_id), 0);

	struct kill_race_arg arg = { .iso_id = iso_id, .result = -999 };
	pthread_t t;
	pthread_create(&t, NULL, slow_ioctl_thread, &arg);

	/* Give the IOCTL thread time to send the command */
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
	nanosleep(&ts, NULL);

	/* Kill the isolate while the IOCTL is in-flight */
	nvkvm_isolate_kill(&g_it, iso_id);

	pthread_join(t, NULL);

	/*
	 * The IOCTL must complete (not hang). The result is either success
	 * (if the stub responded before the kill) or a transport error
	 * (-ECONNRESET or similar). Either is acceptable.
	 */
	if (arg.result != 0 && arg.result != -ECONNRESET) {
		/* Any other negative errno is also a valid transport error */
		EXPECT_GE(0, arg.result); /* result < 0 */
	}

	fixture_fini();
}

/* ── Test 6: sync commands ───────────────────────────────────────────────── */

TEST(sync_mmap_munmap)
{
	fixture_init();

	uint32_t iso_id = 0;
	EXPECT_EQ(nvkvm_isolate_create(&g_it, 5, NULL, &iso_id), 0);

	/* Open a memory handle to send */
	uint32_t hid = 0;
	int ret = nvkvm_handle_open_memory(&g_ht, /*session_id=*/5,
					   /*size=*/4096, &hid);
	EXPECT_EQ(ret, 0);

	/* send_handle sends via SCM_RIGHTS + expects OK */
	ret = nvkvm_isolate_send_handle(&g_it, &g_ht, iso_id, hid);
	EXPECT_EQ(ret, 0);

	/* mmap */
	ret = nvkvm_isolate_mmap(&g_it, iso_id, hid,
				 /*gva=*/0x100000, /*len=*/4096,
				 /*offset=*/0, PROT_READ, MAP_SHARED);
	EXPECT_EQ(ret, 0);

	/* munmap */
	ret = nvkvm_isolate_munmap(&g_it, iso_id, 0x100000, 4096);
	EXPECT_EQ(ret, 0);

	/* close handle on isolate */
	ret = nvkvm_isolate_close_handle(&g_it, &g_ht, iso_id, hid);
	EXPECT_EQ(ret, 0);

	nvkvm_handle_close(&g_ht, hid);
	nvkvm_isolate_kill(&g_it, iso_id);
	fixture_fini();
}

/* ── Test 7: poll / unpoll ───────────────────────────────────────────────── */

TEST(poll_unpoll)
{
	fixture_init();

	uint32_t iso_id = 0;
	EXPECT_EQ(nvkvm_isolate_create(&g_it, 6, NULL, &iso_id), 0);

	uint32_t hid = 0;
	EXPECT_EQ(nvkvm_handle_open_memory(&g_ht, 6, 4096, &hid), 0);
	EXPECT_EQ(nvkvm_isolate_send_handle(&g_it, &g_ht, iso_id, hid), 0);

	EXPECT_EQ(nvkvm_isolate_poll(&g_it, iso_id, hid, POLLIN), 0);
	EXPECT_EQ(nvkvm_isolate_unpoll(&g_it, iso_id, hid), 0);

	nvkvm_isolate_close_handle(&g_it, &g_ht, iso_id, hid);
	nvkvm_handle_close(&g_ht, hid);
	nvkvm_isolate_kill(&g_it, iso_id);
	fixture_fini();
}

/* ── Test 8: cross-isolate relay bookkeeping across teardown ─────────────── */

TEST(foreign_relay_cleanup_on_kill)
{
	fixture_init();

	/*
	 * Install a process-free live slot by hand.  This exercises the
	 * bookkeeping in nvkvm_isolate_kill() without spawning mock_stub or
	 * waiting on a child, which is what the four known-failing cases above
	 * are stuck on.
	 */
	const uint32_t iso_id = 7;
	struct nvkvm_isolate *iso = &g_it.isolates[iso_id];
	iso->id = iso_id;
	iso->in_use = true;
	iso->alive = true;
	iso->pid = 0;
	iso->sock_fd = -1;
	iso->xrm_accepting = true;

	uint32_t hid = 0;
	EXPECT_EQ(nvkvm_handle_open_memory(&g_ht, 77, 4096, &hid), 0);
	struct nvkvm_handle *h = nvkvm_handle_get(&g_ht, hid);
	EXPECT_GE((uintptr_t)h, 1);
	uint64_t generation = h->generation;

	/*
	 * A reserved-but-undelivered relay is not the same as a delivered one:
	 * no handle reference exists for it yet, so forget() reports generation
	 * 0 and teardown must not try to drop one.
	 */
	uint64_t pending_generation = 99;
	EXPECT_EQ(nvkvm_isolate_note_foreign_handle(&g_it, iso_id, hid), false);
	EXPECT_EQ(nvkvm_isolate_forget_foreign_handle(&g_it, iso_id, hid,
						      &pending_generation), true);
	EXPECT_EQ(pending_generation, 0);

	/* Now the delivered case: reserve, pin, publish the generation. */
	EXPECT_EQ(nvkvm_isolate_note_foreign_handle(&g_it, iso_id, hid), false);
	EXPECT_EQ(nvkvm_handle_ref_isolate_generation(&g_ht, hid, generation), 0);
	EXPECT_EQ(nvkvm_isolate_finalize_foreign_handle(&g_it, iso_id, hid,
							generation), true);
	EXPECT_EQ(h->isolate_refcount, 1);

	/* Killing the importer must return the reference, or the owner's fd
	 * stays pinned open for the life of the VM. */
	EXPECT_EQ(nvkvm_isolate_kill(&g_it, iso_id), 0);
	EXPECT_EQ(h->isolate_refcount, 0);
	EXPECT_EQ(iso->xrm_n, 0);
	EXPECT_EQ(nvkvm_handle_close(&g_ht, hid), 0);

	fixture_fini();
}

/* ── Test 9: NVKMS vblank semaphore quota ────────────────────────────────── */

TEST(nvkms_vblank_quota_lifecycle)
{
	fixture_init();

	const uint32_t iso_id = 8;
	struct nvkvm_isolate *iso = &g_it.isolates[iso_id];
	iso->id = iso_id;
	iso->in_use = true;
	iso->alive = true;
	iso->pid = 0;
	iso->sock_fd = -1;
	iso->xrm_accepting = true;
	iso->nvkms_vblank_seq = 1;

	/* Fill the VM-wide budget, split across two modeset handles. */
	uint64_t reservations[NVKVM_NVKMS_VBLANK_MAX];
	for (unsigned i = 0; i < NVKVM_NVKMS_VBLANK_MAX; i++) {
		reservations[i] = nvkvm_isolate_nvkms_vblank_reserve(
			&g_it, iso_id, 100 + (i & 1));
		EXPECT_GE(reservations[i], 1);
		nvkvm_isolate_nvkms_vblank_finish(&g_it, iso_id, reservations[i],
						  true, 1, 2, 1000 + i);
	}
	/* The 65th enable is refused, which is the whole point. */
	EXPECT_EQ(nvkvm_isolate_nvkms_vblank_reserve(&g_it, iso_id, 100), 0);
	EXPECT_EQ(g_it.nvkms_vblank_total, NVKVM_NVKMS_VBLANK_MAX);

	/* A matching disable releases exactly one slot, and a failed enable
	 * gives its reservation straight back. */
	nvkvm_isolate_nvkms_vblank_retire(&g_it, iso_id, 100, 1, 2, 1000);
	uint64_t replacement = nvkvm_isolate_nvkms_vblank_reserve(&g_it, iso_id,
								  100);
	EXPECT_GE(replacement, 1);
	nvkvm_isolate_nvkms_vblank_finish(&g_it, iso_id, replacement, false,
					  0, 0, 0);
	EXPECT_EQ(g_it.nvkms_vblank_total, NVKVM_NVKMS_VBLANK_MAX - 1);

	/* Closing one modeset fd returns that fd's slots and nobody else's. */
	nvkvm_isolate_nvkms_vblank_purge_handle(&g_it, iso_id, 100);
	for (unsigned i = 0; i < NVKVM_NVKMS_VBLANK_MAX / 2; i++)
		EXPECT_GE(nvkvm_isolate_nvkms_vblank_reserve(&g_it, iso_id, 100),
			  1);
	EXPECT_EQ(nvkvm_isolate_nvkms_vblank_reserve(&g_it, iso_id, 100), 0);
	EXPECT_EQ(g_it.nvkms_vblank_total, NVKVM_NVKMS_VBLANK_MAX);

	/* Isolate death returns the rest, or a create/kill loop exhausts the
	 * budget permanently. */
	EXPECT_EQ(nvkvm_isolate_kill(&g_it, iso_id), 0);
	EXPECT_EQ(g_it.nvkms_vblank_total, 0);

	fixture_fini();
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
	/* Resolve the mock stub path */
	char *stub_path = getenv("NVKVM_STUB_PATH");
	if (!stub_path) {
		/* Default: assume mock_stub is next to this binary */
		char buf[512];
		ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if (n < 0) {
			fprintf(stderr, "Set NVKVM_STUB_PATH to the mock_stub binary\n");
			return 1;
		}
		buf[n] = '\0';
		/* Replace test binary name with mock_stub */
		char *slash = strrchr(buf, '/');
		if (slash) {
			*(slash + 1) = '\0';
			strncat(buf, "mock_stub", sizeof(buf) - strlen(buf) - 1);
		}
		setenv("NVKVM_STUB_PATH", buf, 1);
	}

	RUN(lifecycle);
	RUN(sequential_ioctl);
	RUN(concurrent_ioctl);
	RUN(out_of_order_ioctl);
	RUN(kill_during_inflight);
	RUN(sync_mmap_munmap);
	RUN(poll_unpoll);
	RUN(foreign_relay_cleanup_on_kill);
	RUN(nvkms_vblank_quota_lifecycle);

	fprintf(stderr, "\n%d/%d tests passed\n", tests_passed, tests_run);
	return tests_failed ? 1 : 0;
}
