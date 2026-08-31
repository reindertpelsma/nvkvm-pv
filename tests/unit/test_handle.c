/*
 * test_handle.c — unit tests for nvkvm_handle.c
 *
 * Tests the global handle table: alloc, refcount, close ordering.
 * No GPU required — nvidia handles are opened as O_PATH /dev/null stubs
 * when /dev/nvidiactl is unavailable; memory handles use memfd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>

/* Stub qemu/osdep.h for the handle implementation */
#include "stubs/qemu/osdep.h"

/* Include the handle implementation directly */
#include "../../src/qemu/nvkvm_handle.h"

/*
 * The handle table calls open() to open nvidia devices. For tests without a
 * real GPU we redirect open() to open /dev/null instead so the fd is valid.
 * We do this by defining a wrapper that intercepts the nvidia device paths.
 */
#include <dlfcn.h>

static int stub_dev_fd = -1;  /* fd opened once, duplicated for each handle */

/* ── Minimal test framework ───────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
	static void name(void); \
	__attribute__((constructor)) static void _reg_##name(void) { \
		__test_registry[__test_count++] = (struct _test){ #name, name }; \
	} \
	static void name(void)

#define ASSERT_EQ(a, b) do { \
	if ((a) != (b)) { \
		fprintf(stderr, "  FAIL %s:%d: %s == %s  got %lld  expected %lld\n", \
			__FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b)); \
		__current_test_failed = 1; \
		return; \
	} \
} while (0)

#define ASSERT_NE(a, b) do { \
	if ((a) == (b)) { \
		fprintf(stderr, "  FAIL %s:%d: %s != %s, both %lld\n", \
			__FILE__, __LINE__, #a, #b, (long long)(a)); \
		__current_test_failed = 1; \
		return; \
	} \
} while (0)

#define ASSERT_TRUE(e)    ASSERT_NE((int)(e), 0)
#define ASSERT_FALSE(e)   ASSERT_EQ((int)(e), 0)
#define ASSERT_NULL(p)    ASSERT_EQ((uintptr_t)(p), 0UL)
#define ASSERT_NOTNULL(p) ASSERT_NE((uintptr_t)(p), 0UL)

struct _test { const char *name; void (*fn)(void); };
static struct _test __test_registry[256];
static int __test_count = 0;
static int __current_test_failed = 0;

int main(void)
{
	int i;
	/* Open /dev/null as a substitute device fd */
	stub_dev_fd = open("/dev/null", O_RDWR);
	assert(stub_dev_fd >= 0);

	for (i = 0; i < __test_count; i++) {
		__current_test_failed = 0;
		tests_run++;
		printf("[ RUN  ] %s\n", __test_registry[i].name);
		__test_registry[i].fn();
		if (__current_test_failed) {
			printf("[ FAIL ] %s\n", __test_registry[i].name);
			tests_failed++;
		} else {
			printf("[ PASS ] %s\n", __test_registry[i].name);
			tests_passed++;
		}
	}
	printf("\n%d/%d tests passed", tests_passed, tests_run);
	if (tests_failed)
		printf(", %d FAILED", tests_failed);
	printf("\n");
	close(stub_dev_fd);
	return tests_failed ? 1 : 0;
}

/* ── Test helpers ─────────────────────────────────────────────────────────── */

/*
 * open_memory_handle_real: use memfd (always available) for testing.
 * This tests the handle table mechanics without needing a GPU.
 */
static int open_test_memory_handle(struct nvkvm_handle_table *t,
				    uint32_t session, uint32_t *id)
{
	return nvkvm_handle_open_memory(t, session, 4096, id);
}

/* ── Tests: basic alloc and lookup ─────────────────────────────────────────── */

TEST(test_open_memory_handle_succeeds)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	uint32_t id = 0;
	int ret = open_test_memory_handle(&t, 1, &id);

	ASSERT_EQ(ret, 0);
	ASSERT_NE(id, 0);

	struct nvkvm_handle *h = nvkvm_handle_get(&t, id);
	ASSERT_NOTNULL(h);
	ASSERT_EQ(h->type, NVKVM_HANDLE_TYPE_MEMORY);
	ASSERT_NE(h->fd, -1);

	nvkvm_handle_table_fini(&t);
}

TEST(test_multiple_handles_have_unique_ids)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	uint32_t id1, id2, id3;
	ASSERT_EQ(open_test_memory_handle(&t, 1, &id1), 0);
	ASSERT_EQ(open_test_memory_handle(&t, 1, &id2), 0);
	ASSERT_EQ(open_test_memory_handle(&t, 1, &id3), 0);

	ASSERT_NE(id1, id2);
	ASSERT_NE(id2, id3);
	ASSERT_NE(id1, id3);

	nvkvm_handle_table_fini(&t);
}

TEST(test_lookup_invalid_id_returns_null)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	ASSERT_NULL(nvkvm_handle_get(&t, 0));
	ASSERT_NULL(nvkvm_handle_get(&t, 99999));

	nvkvm_handle_table_fini(&t);
}

/* ── Tests: refcounting ────────────────────────────────────────────────────── */

TEST(test_close_without_refs_succeeds)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	uint32_t id;
	ASSERT_EQ(open_test_memory_handle(&t, 1, &id), 0);
	ASSERT_EQ(nvkvm_handle_close(&t, id), 0);
	/* After close, lookup must return NULL */
	ASSERT_NULL(nvkvm_handle_get(&t, id));

	nvkvm_handle_table_fini(&t);
}

TEST(test_close_with_ref_fails_ebusy)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	uint32_t id;
	ASSERT_EQ(open_test_memory_handle(&t, 1, &id), 0);
	ASSERT_EQ(nvkvm_handle_ref_isolate(&t, id), 0);

	/* Close must fail while an isolate holds the handle */
	int ret = nvkvm_handle_close(&t, id);
	ASSERT_EQ(ret, -EBUSY);

	/* Unref and retry */
	ASSERT_EQ(nvkvm_handle_unref_isolate(&t, id), 0);
	ASSERT_EQ(nvkvm_handle_close(&t, id), 0);

	nvkvm_handle_table_fini(&t);
}

TEST(test_ref_unref_multiple_isolates)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	uint32_t id;
	ASSERT_EQ(open_test_memory_handle(&t, 1, &id), 0);

	ASSERT_EQ(nvkvm_handle_ref_isolate(&t, id), 0);
	ASSERT_EQ(nvkvm_handle_ref_isolate(&t, id), 0);
	ASSERT_EQ(nvkvm_handle_ref_isolate(&t, id), 0);

	/* Still busy */
	ASSERT_EQ(nvkvm_handle_close(&t, id), -EBUSY);

	ASSERT_EQ(nvkvm_handle_unref_isolate(&t, id), 0);
	ASSERT_EQ(nvkvm_handle_unref_isolate(&t, id), 0);
	ASSERT_EQ(nvkvm_handle_unref_isolate(&t, id), 0);

	/* Now closeable */
	ASSERT_EQ(nvkvm_handle_close(&t, id), 0);

	nvkvm_handle_table_fini(&t);
}

/*
 * Generation qualification: a release must name the incarnation of the slot it
 * took a reference against, so a handle id that has been closed and reissued
 * cannot have its new occupant's count decremented by a stale releaser.
 */
TEST(test_generation_qualified_unref_rejects_stale_slot)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	uint32_t id;
	ASSERT_EQ(open_test_memory_handle(&t, 1, &id), 0);
	struct nvkvm_handle *h = nvkvm_handle_get(&t, id);
	ASSERT_NOTNULL(h);
	uint64_t generation = h->generation;
	ASSERT_NE(generation, 0);

	ASSERT_EQ(nvkvm_handle_ref_isolate_generation(&t, id, generation), 0);
	/* Wrong generation: refused, and the reference is still held. */
	ASSERT_EQ(nvkvm_handle_unref_isolate_generation(&t, id, generation + 1),
		  -EBADF);
	ASSERT_EQ(nvkvm_handle_close(&t, id), -EBUSY);
	ASSERT_EQ(nvkvm_handle_unref_isolate_generation(&t, id, generation), 0);
	ASSERT_EQ(nvkvm_handle_close(&t, id), 0);

	nvkvm_handle_table_fini(&t);
}

/*
 * The relay path needs the dup and the pin to happen together under the table
 * lock; if they can be separated, a close between them recycles the fd number.
 */
TEST(test_acquire_fd_ref_is_atomic_pin)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	uint32_t id;
	ASSERT_EQ(open_test_memory_handle(&t, 1, &id), 0);
	uint64_t generation = 0;
	int fd = nvkvm_handle_acquire_fd_ref_isolate(&t, id, NULL, &generation);
	ASSERT_NE(fd, -1);
	ASSERT_NE(generation, 0);
	/* The pin is already in place when the fd comes back. */
	ASSERT_EQ(nvkvm_handle_close(&t, id), -EBUSY);
	close(fd);
	ASSERT_EQ(nvkvm_handle_unref_isolate_generation(&t, id, generation), 0);
	ASSERT_EQ(nvkvm_handle_close(&t, id), 0);

	nvkvm_handle_table_fini(&t);
}

/* ── Tests: session close ──────────────────────────────────────────────────── */

TEST(test_close_session_frees_all_handles)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	uint32_t id1, id2;
	ASSERT_EQ(open_test_memory_handle(&t, 42, &id1), 0);
	ASSERT_EQ(open_test_memory_handle(&t, 42, &id2), 0);

	/* Session 99 handle must not be affected */
	uint32_t id3;
	ASSERT_EQ(open_test_memory_handle(&t, 99, &id3), 0);

	nvkvm_handle_close_session(&t, 42);

	ASSERT_NULL(nvkvm_handle_get(&t, id1));
	ASSERT_NULL(nvkvm_handle_get(&t, id2));
	ASSERT_NOTNULL(nvkvm_handle_get(&t, id3));  /* other session untouched */

	nvkvm_handle_table_fini(&t);
}

/*
 * Cross-session sharing (fork_both_register.c) means a handle created by
 * one session can be legitimately referenced by ANOTHER session's isolate.
 * If that owning session dies first, force-closing the fd underneath the
 * still-referencing isolate is a use-after-free in QEMU's own process.
 * nvkvm_handle_close_session() must skip (not close) any handle whose
 * isolate_refcount is still > 0, mirroring the guard nvkvm_handle_close()
 * already applies -- and once the sharer drops its reference, an ordinary
 * nvkvm_handle_close() (not a session close) must be able to reclaim it.
 */
TEST(test_close_session_skips_isolate_referenced_handle)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	uint32_t id;
	ASSERT_EQ(open_test_memory_handle(&t, 42, &id), 0);

	/* Simulate a sibling session's isolate (e.g. a fork()ed child sharing
	 * a MAP_SHARED buffer) holding a live reference on session 42's handle. */
	ASSERT_EQ(nvkvm_handle_ref_isolate(&t, id), 0);

	/* Session 42 (the creator) exits while the sharer is still live. */
	nvkvm_handle_close_session(&t, 42);

	/* The handle must still be open: closing it now would UAF the sharer. */
	struct nvkvm_handle *h = nvkvm_handle_get(&t, id);
	ASSERT_NOTNULL(h);
	ASSERT_EQ(h->fd >= 0, 1);

	/* Once the sharer drops its own reference, an ordinary close (as the
	 * sharer's own cp-release path would issue, naming the same handle_id,
	 * no session ownership required -- see nvkvm_handle_close()) reclaims
	 * it normally. */
	ASSERT_EQ(nvkvm_handle_unref_isolate(&t, id), 0);
	ASSERT_EQ(nvkvm_handle_close(&t, id), 0);
	ASSERT_NULL(nvkvm_handle_get(&t, id));

	nvkvm_handle_table_fini(&t);
}

/* ── Tests: operations on closed handle ─────────────────────────────────────── */

TEST(test_ops_on_closed_handle_fail)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	uint32_t id;
	ASSERT_EQ(open_test_memory_handle(&t, 1, &id), 0);
	ASSERT_EQ(nvkvm_handle_close(&t, id), 0);

	ASSERT_EQ(nvkvm_handle_ref_isolate(&t, id),   -EBADF);
	ASSERT_EQ(nvkvm_handle_unref_isolate(&t, id), -EBADF);
	ASSERT_EQ(nvkvm_handle_close(&t, id),         -EBADF);

	nvkvm_handle_table_fini(&t);
}

/* ── Tests: id=0 is always invalid ──────────────────────────────────────────── */

TEST(test_zero_handle_id_always_invalid)
{
	struct nvkvm_handle_table t;
	nvkvm_handle_table_init(&t);

	ASSERT_EQ(nvkvm_handle_ref_isolate(&t, 0),   -EBADF);
	ASSERT_EQ(nvkvm_handle_unref_isolate(&t, 0), -EBADF);
	ASSERT_EQ(nvkvm_handle_close(&t, 0),         -EBADF);
	ASSERT_NULL(nvkvm_handle_get(&t, 0));

	nvkvm_handle_table_fini(&t);
}
