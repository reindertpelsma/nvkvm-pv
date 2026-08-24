/*
 * test_objects.c — the RM object graph, and the wire/ABI struct sizes.
 *
 * WAS test_dispatch.c, and renamed on purpose.  It was named for
 * src/qemu/nvkvm_dispatch.c, which DEAD-1 established was unreachable and which
 * has been deleted; a suite that still announced itself as "test_dispatch" in
 * every run would have been one more thing reading as a live control.  See the
 * DEAD-1 banner further down for the 19 cases that went with that file, named
 * individually.
 *
 * What is left, and all of it is live:
 *   - nvkvm_objects.c's client/object graph: add, lookup, free, and the
 *     dependency cascade.  (Vestigial in production since the deletion --
 *     nothing populates it any more -- but still linked and still called from
 *     nvkvm_session_destroy; see the note there.)
 *   - the wire-protocol struct sizes both sides agree on.
 *   - the ABI struct sizes the driver expects, and the shm slot-size guard
 *     that depends on the largest of them.
 *
 * No GPU and no running VM.
 *
 * Build via the project Makefile:
 *   make -C tests/unit test_objects && tests/unit/test_objects
 * or, preferably, the whole suite:
 *   bash tests/unit/run_tests.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>

/* QEMU type stubs (provided by stubs/hw/virtio/virtio.h, included transitively) */
#include "../../src/qemu/virtio_nvgpu.h"

/* Pull in the ABI headers (already included via virtio_nvgpu.h) */

/* ── Tiny test framework ──────────────────────────────────────────────────── */

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
		fprintf(stderr, "  FAIL %s:%d: %s == %s got %lld expected %lld\n", \
			__FILE__, __LINE__, #a, #b, \
			(long long)(a), (long long)(b)); \
		__current_test_failed = 1; \
		return; \
	} \
} while (0)

#define ASSERT_NE(a, b) do { \
	if ((a) == (b)) { \
		fprintf(stderr, "  FAIL %s:%d: %s != %s, both are %lld\n", \
			__FILE__, __LINE__, #a, #b, (long long)(a)); \
		__current_test_failed = 1; \
		return; \
	} \
} while (0)

#define ASSERT_NULL(p) ASSERT_EQ((uintptr_t)(p), 0UL)
#define ASSERT_NOTNULL(p) ASSERT_NE((uintptr_t)(p), 0UL)
#define ASSERT_TRUE(e) ASSERT_NE((int)(e), 0)
#define ASSERT_FALSE(e) ASSERT_EQ((int)(e), 0)

struct _test { const char *name; void (*fn)(void); };
static struct _test __test_registry[512];
static int __test_count = 0;
static int __current_test_failed = 0;

int main(void) {
	int i;
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
	return tests_failed ? 1 : 0;
}

/*
 * ── DEAD-1, 2026-08-24 ── the param-size-table section was here, and so was
 * the NUMA_INFO / CARD_INFO size-derivation section: 19 cases in all.
 *
 * They tested nvkvm_ioctl_expected_param_size() in src/qemu/nvkvm_dispatch.c,
 * which had no caller outside this file and has been deleted along with the
 * rest of the unreachable dispatch/frontend pair.  What went with it, named so
 * the loss is a decision on the record rather than a number that quietly got
 * smaller:
 *
 *   test_param_size_rm_alloc_nvos21 / _nvos64 / _bad_size, _rm_control,
 *   _rm_free, _check_version, _unknown_ioctl, _uvm_initialize,
 *   _uvm_register_gpu, _card_info_zero, _card_info_too_large,
 *   _card_info_valid, test_numa_info_size_from_ioc_size_32 / _560,
 *   _size_zero_rejected, _size_arbitrary_nonzero,
 *   test_numa_info_old_sizeof_would_fail_560,
 *   test_card_info_max_entries_exactly, test_card_info_single_entry.
 *
 * None of them proved a live invariant.  The size table they exercised was a
 * THIRD copy: the guest has its own (nvkvm_guest_ioctl_size,
 * src/guest/nvkvm_ioctl.c) which is what actually sizes the buffer, and the
 * struct sizes themselves are pinned twice over -- by the ABI-struct section
 * below and by tests/abi_parity.  The live host-side bound on a guest-declared
 * size is slot_blob()'s C-1 check in virtio_nvgpu.c plus the per-gate minimums
 * that fail closed individually; there is no per-command size table on the live
 * QEMU path at all, so there was nothing here to retarget these at.
 *
 * WHAT IS NOW UNTESTED, stated plainly rather than left to be rediscovered:
 * the guest's size table, including the _IOC_SIZE-authoritative NUMA_INFO
 * behaviour these cases were written for (a 575.x driver sends 560 bytes where
 * sizeof(struct nv_ioctl_numa_info) is 32).  It is guest code, so the audit
 * methodology does not count it as a control -- but a regression there breaks
 * NUMA_INFO on 575.x again, and nothing would now say so.
 */

/* ── Tests: object graph ─────────────────────────────────────────────────── */

TEST(test_obj_add_and_lookup) {
	struct nvkvm_client *c = nvkvm_client_alloc(0x1234);
	ASSERT_NOTNULL(c);
	ASSERT_EQ(c->handle, 0x1234U);

	int ret = nvkvm_obj_add(c, 0xABCD, NV01_DEVICE_0,
				NV01_NULL_OBJECT, NULL);
	ASSERT_EQ(ret, 0);

	struct nvkvm_object *o = nvkvm_obj_lookup(c, 0xABCD);
	ASSERT_NOTNULL(o);
	ASSERT_EQ(o->handle, 0xABCDU);
	ASSERT_EQ(o->class_id, (uint32_t)NV01_DEVICE_0);

	nvkvm_client_free(NULL, c);
}

TEST(test_obj_lookup_missing) {
	struct nvkvm_client *c = nvkvm_client_alloc(0x1);
	struct nvkvm_object *o = nvkvm_obj_lookup(c, 0xDEAD);
	ASSERT_NULL(o);
	nvkvm_client_free(NULL, c);
}

TEST(test_obj_null_handle_rejected) {
	struct nvkvm_client *c = nvkvm_client_alloc(0x1);
	int ret = nvkvm_obj_add(c, NV01_NULL_OBJECT, NV01_DEVICE_0,
				NV01_NULL_OBJECT, NULL);
	ASSERT_NE(ret, 0);
	nvkvm_client_free(NULL, c);
}

TEST(test_obj_free_removes_object) {
	struct nvkvm_client *c = nvkvm_client_alloc(0x1);
	nvkvm_obj_add(c, 0x10, NV01_DEVICE_0, NV01_NULL_OBJECT, NULL);
	ASSERT_NOTNULL(nvkvm_obj_lookup(c, 0x10));
	nvkvm_obj_free(c, 0x10);
	ASSERT_NULL(nvkvm_obj_lookup(c, 0x10));
	nvkvm_client_free(NULL, c);
}

TEST(test_obj_free_nonexistent_noop) {
	struct nvkvm_client *c = nvkvm_client_alloc(0x1);
	/* Should not crash or return error — driver allows this */
	nvkvm_obj_free(c, 0xDEAD);
	nvkvm_client_free(NULL, c);
}

TEST(test_obj_dependency_cascade_free) {
	/*
	 * Setup: child depends on parent.
	 * Freeing parent must also free child.
	 *
	 *  parent (0x1) ← child (0x2)
	 */
	struct nvkvm_client *c = nvkvm_client_alloc(0x1);

	nvkvm_obj_add(c, 0x100, NV01_ROOT_CLIENT,
		      NV01_NULL_OBJECT, NULL);      /* parent */
	nvkvm_obj_add(c, 0x101, NV01_DEVICE_0,
		      0x100, NULL);                 /* child; parent=0x100 */

	ASSERT_NOTNULL(nvkvm_obj_lookup(c, 0x100));
	ASSERT_NOTNULL(nvkvm_obj_lookup(c, 0x101));

	/* Free parent → child should be freed too */
	nvkvm_obj_free(c, 0x100);

	ASSERT_NULL(nvkvm_obj_lookup(c, 0x100));
	ASSERT_NULL(nvkvm_obj_lookup(c, 0x101));

	nvkvm_client_free(NULL, c);
}

TEST(test_obj_dependency_deep_cascade) {
	/*
	 *  A (0x1) ← B (0x2) ← C (0x3)
	 * Freeing A must free B and C.
	 */
	struct nvkvm_client *c = nvkvm_client_alloc(0x1);
	nvkvm_obj_add(c, 0x1, NV01_ROOT_CLIENT, NV01_NULL_OBJECT, NULL);
	nvkvm_obj_add(c, 0x2, NV01_DEVICE_0,   0x1, NULL);
	nvkvm_obj_add(c, 0x3, NV20_SUBDEVICE_0, 0x2, NULL);

	nvkvm_obj_free(c, 0x1);

	ASSERT_NULL(nvkvm_obj_lookup(c, 0x1));
	ASSERT_NULL(nvkvm_obj_lookup(c, 0x2));
	ASSERT_NULL(nvkvm_obj_lookup(c, 0x3));

	nvkvm_client_free(NULL, c);
}

TEST(test_obj_add_dep_explicit) {
	/*
	 * B explicitly depends on A (not via parent).
	 * Free A → B freed too.
	 */
	struct nvkvm_client *c = nvkvm_client_alloc(0x1);
	nvkvm_obj_add(c, 0xA, NV01_DEVICE_0, NV01_NULL_OBJECT, NULL);
	nvkvm_obj_add(c, 0xB, NV20_SUBDEVICE_0, NV01_NULL_OBJECT, NULL);
	nvkvm_obj_add_dep(c, 0xB, 0xA);  /* B depends on A */

	nvkvm_obj_free(c, 0xA);
	ASSERT_NULL(nvkvm_obj_lookup(c, 0xA));
	ASSERT_NULL(nvkvm_obj_lookup(c, 0xB));

	nvkvm_client_free(NULL, c);
}

TEST(test_obj_free_leaf_does_not_cascade_to_parent) {
	/*
	 * Freeing a child must NOT free its parent.
	 */
	struct nvkvm_client *c = nvkvm_client_alloc(0x1);
	nvkvm_obj_add(c, 0x10, NV01_ROOT_CLIENT, NV01_NULL_OBJECT, NULL);
	nvkvm_obj_add(c, 0x11, NV01_DEVICE_0,   0x10, NULL);

	nvkvm_obj_free(c, 0x11);
	ASSERT_NULL(nvkvm_obj_lookup(c, 0x11));
	ASSERT_NOTNULL(nvkvm_obj_lookup(c, 0x10));  /* parent must survive */

	nvkvm_client_free(NULL, c);
}

TEST(test_obj_max_objects) {
	/* Fill the client table to capacity, check no overflows */
	struct nvkvm_client *c = nvkvm_client_alloc(0x1);
	int i, succeeded = 0;
	for (i = 1; i < NVKVM_MAX_OBJECTS_PER_CLIENT; i++) {
		if (nvkvm_obj_add(c, (uint32_t)i, NV01_DEVICE_0,
				  NV01_NULL_OBJECT, NULL) == 0)
			succeeded++;
	}
	/* All should succeed (each goes to a distinct slot) */
	ASSERT_EQ(succeeded, NVKVM_MAX_OBJECTS_PER_CLIENT - 1);
	nvkvm_client_free(NULL, c);
}

/* ── Tests: wire protocol struct sizes ────────────────────────────────────── */

TEST(test_proto_hdr_size) {
	/* nvkvm_hdr must be exactly 8 bytes so both sides agree on layout */
	ASSERT_EQ(sizeof(struct nvkvm_hdr), 8U);
}

TEST(test_proto_req_ioctl_size) {
	/* Fixed size known by both guest and host for request ring entry */
	ASSERT_EQ(sizeof(struct nvkvm_req_ioctl), 32U);
}

TEST(test_proto_shm_ctrl_size) {
	/* Control block fits in one page */
	ASSERT_TRUE(sizeof(struct nvkvm_shm_ctrl) <= 4096U);
}

/* ── Tests: ABI struct sizes match driver expectation ─────────────────────── */

/*
 * These are the struct sizes that the NVIDIA driver has used since R525.
 * If they drift, the ioctl forwarding will silently corrupt data.
 * Values taken from open-gpu-kernel-modules headers.
 */
TEST(test_nvos21_size) {
	/* 4x u32 (16) + u64 (8) + u32 (4) + 4 trailing pad = 32 on x86-64 */
	ASSERT_EQ(sizeof(struct nvos21_parameters), 32U);
}

TEST(test_nvos64_size) {
	ASSERT_EQ(sizeof(struct nvos64_parameters), 48U);
}

TEST(test_nvos54_size) {
	ASSERT_EQ(sizeof(struct nvos54_parameters), 32U);
}

TEST(test_nvos00_size) {
	ASSERT_EQ(sizeof(struct nvos00_parameters), 16U);
}

TEST(test_uvm_initialize_size) {
	ASSERT_EQ(sizeof(struct uvm_initialize_params), 16U);
}

/*
 * Slot-size guard: the SHM slot must be large enough to hold the largest
 * known ioctl parameter struct. If NVKVM_SHM_SLOT_MIN_SIZE < the largest
 * struct, something would silently get truncated.
 */

TEST(test_shm_slot_fits_largest_param) {
	/* nvos32_parameters is currently the largest frontend struct */
	ASSERT_TRUE(NVKVM_SHM_SLOT_MIN_SIZE >= sizeof(struct nvos32_parameters));
}

TEST(test_shm_slot_fits_numa_info_560) {
	/* 560-byte NUMA_INFO must fit in the minimum slot size */
	ASSERT_TRUE(NVKVM_SHM_SLOT_MIN_SIZE >= 560U);
}
