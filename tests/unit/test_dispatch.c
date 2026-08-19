/*
 * test_dispatch.c — unit tests for the QEMU backend dispatch layer
 *
 * Tests the param size table, dispatch security boundaries, and object
 * graph logic without requiring a real GPU or running VM. Runs on the
 * host with no special hardware.
 *
 * Build:
 *   gcc -I../../src -o test_dispatch test_dispatch.c \
 *       ../../src/qemu/nvkvm_objects.c ../../src/qemu/nvkvm_dispatch.c \
 *       -lpthread -lglib-2.0 -lgobject-2.0 \
 *       $(pkg-config --cflags --libs glib-2.0)
 *
 * Or via the project Makefile:
 *   make test-unit
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

/* ── Stub for nvkvm_dispatch.c dependency ────────────────────────────────── */

/* We only need the size table function here, not the full dispatch. */
/* The helper grew an ABI-profile argument (struct layouts are keyed on the
 * host driver version).  Pin these expectations to one profile so the numbers
 * below stay meaningful; NVKVM_ABI_570 is the reference the dispatch layer
 * itself falls back to. */
#define nvkvm_ioctl_expected_param_size(cmd) \
	nvkvm_ioctl_expected_param_size((cmd), nvkvm_abi_by_id(NVKVM_ABI_570))

/* ── Tests: param size table ─────────────────────────────────────────────── */

TEST(test_param_size_rm_alloc_nvos21) {
	unsigned int cmd = _IOWR('F', NV_ESC_RM_ALLOC,
				 struct nvos21_parameters);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, sizeof(struct nvos21_parameters));
}

TEST(test_param_size_rm_alloc_nvos64) {
	unsigned int cmd = _IOWR('F', NV_ESC_RM_ALLOC,
				 struct nvos64_parameters);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, sizeof(struct nvos64_parameters));
}

TEST(test_param_size_rm_alloc_bad_size) {
	/* Made-up size that is neither NVOS21 nor NVOS64 */
	unsigned int cmd = _IOWR('F', NV_ESC_RM_ALLOC, uint64_t);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, (size_t)-1);
}

TEST(test_param_size_rm_control) {
	unsigned int cmd = _IOWR('F', NV_ESC_RM_CONTROL,
				 struct nvos54_parameters);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, sizeof(struct nvos54_parameters));
}

TEST(test_param_size_rm_free) {
	unsigned int cmd = _IOWR('F', NV_ESC_RM_FREE,
				 struct nvos00_parameters);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, sizeof(struct nvos00_parameters));
}

TEST(test_param_size_check_version) {
	unsigned int cmd = _IOWR('F', NV_ESC_CHECK_VERSION_STR,
				 struct nv_ioctl_rm_api_version);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, sizeof(struct nv_ioctl_rm_api_version));
}

TEST(test_param_size_unknown_ioctl) {
	/* Completely bogus ioctl number */
	size_t sz = nvkvm_ioctl_expected_param_size(0xDEADBEEF);
	ASSERT_EQ(sz, (size_t)-1);
}

TEST(test_param_size_uvm_initialize) {
	size_t sz = nvkvm_ioctl_expected_param_size(UVM_INITIALIZE);
	ASSERT_EQ(sz, sizeof(struct uvm_initialize_params));
}

TEST(test_param_size_uvm_register_gpu) {
	size_t sz = nvkvm_ioctl_expected_param_size(UVM_REGISTER_GPU);
	ASSERT_EQ(sz, sizeof(struct uvm_register_gpu_params));
}

TEST(test_param_size_card_info_zero) {
	/* IOC_SIZE == 0 → should be rejected */
	unsigned int cmd = _IOC(_IOC_READ | _IOC_WRITE, 'F',
				NV_ESC_CARD_INFO, 0);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, (size_t)-1);
}

TEST(test_param_size_card_info_too_large) {
	/* More than NV_IOCTL_CARD_INFO_MAX_ENTRIES entries → rejected */
	unsigned int cmd = _IOC(_IOC_READ | _IOC_WRITE, 'F',
				NV_ESC_CARD_INFO,
				sizeof(struct nv_ioctl_card_info) *
				(NV_IOCTL_CARD_INFO_MAX_ENTRIES + 1));
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, (size_t)-1);
}

TEST(test_param_size_card_info_valid) {
	unsigned int cmd = _IOC(_IOC_READ | _IOC_WRITE, 'F',
				NV_ESC_CARD_INFO,
				sizeof(struct nv_ioctl_card_info));
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, sizeof(struct nv_ioctl_card_info));
}

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
 * Bug: NV_ESC_NUMA_INFO param size was hard-coded to sizeof(struct
 * nv_ioctl_numa_info) = 32 bytes, but the driver 575.x struct grew to 560
 * bytes. The fix was to use _IOC_SIZE(cmd) instead.
 *
 * These tests verify the fixed behavior: the dispatch table accepts whatever
 * size the ioctl command encodes, not a hard-coded struct size.
 */

/* Helper: build a NUMA_INFO ioctl command encoding a specific byte count */
static unsigned int make_numa_info_cmd(unsigned int size_bytes)
{
	return _IOC(_IOC_READ | _IOC_WRITE, 'F', NV_ESC_NUMA_INFO, size_bytes);
}

TEST(test_numa_info_size_from_ioc_size_32) {
	/*
	 * Old (broken): sizeof(struct nv_ioctl_numa_info) == 32.
	 * Old behavior: only cmd with IOC_SIZE==32 was accepted.
	 * New behavior: IOC_SIZE is authoritative; 32 is valid.
	 */
	unsigned int cmd = make_numa_info_cmd(32);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	/* Must return exactly 32, not (size_t)-1 */
	ASSERT_EQ(sz, 32U);
}

TEST(test_numa_info_size_from_ioc_size_560) {
	/*
	 * Driver 575.x sends a 560-byte NUMA_INFO struct.
	 * Before the fix, this returned (size_t)-1 (unknown size),
	 * causing QEMU to reject the ioctl with EINVAL before forwarding.
	 */
	unsigned int cmd = make_numa_info_cmd(560);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, 560U);
}

TEST(test_numa_info_size_zero_rejected) {
	/* IOC_SIZE == 0 is invalid and must be rejected */
	unsigned int cmd = _IOC(_IOC_READ | _IOC_WRITE, 'F', NV_ESC_NUMA_INFO, 0);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, (size_t)-1);
}

TEST(test_numa_info_size_arbitrary_nonzero) {
	/* Any non-zero IOC_SIZE is accepted (driver versioning) */
	unsigned int cmd = make_numa_info_cmd(128);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, 128U);
}

TEST(test_numa_info_old_sizeof_would_fail_560) {
	/*
	 * Regression guard: if someone restores the old sizeof()-based check,
	 * this test would catch it.  The fix must NOT hardcode any specific size.
	 *
	 * Verify: expected size for cmd encoding 560 bytes != old sizeof (32).
	 */
	unsigned int cmd = make_numa_info_cmd(560);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	/* If this fails, it means the old bug was re-introduced */
	ASSERT_NE(sz, 32U);
	ASSERT_EQ(sz, 560U);
}

/*
 * CARD_INFO size handling also uses IOC_SIZE (correct pattern).
 * Test edge cases in CARD_INFO to ensure the pattern is consistent.
 */

TEST(test_card_info_max_entries_exactly) {
	/* Exactly the maximum allowed array size */
	size_t max_sz = sizeof(struct nv_ioctl_card_info) *
			NV_IOCTL_CARD_INFO_MAX_ENTRIES;
	unsigned int cmd = _IOC(_IOC_READ | _IOC_WRITE, 'F',
				NV_ESC_CARD_INFO, max_sz);
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, max_sz);
}

TEST(test_card_info_single_entry) {
	unsigned int cmd = _IOC(_IOC_READ | _IOC_WRITE, 'F',
				NV_ESC_CARD_INFO,
				sizeof(struct nv_ioctl_card_info));
	size_t sz = nvkvm_ioctl_expected_param_size(cmd);
	ASSERT_EQ(sz, sizeof(struct nv_ioctl_card_info));
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
