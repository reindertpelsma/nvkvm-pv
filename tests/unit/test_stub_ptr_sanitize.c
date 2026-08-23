/*
 * test_stub_ptr_sanitize.c — the stub's guest-pointer sanitisation, per
 *                            docs/internal/audit-guest-pointers.md.
 *
 * U-5: the declared inner-params size must never exceed the aux blob the stub
 *      actually allocated.
 * U-7: NVOS64's second pointer (pRightsRequested) must never be forwarded.
 *
 * docs/internal/audit-guest-pointers.md, U-5.  The stub rewrites the pointer
 * field at offset 16 to name its own aux buffer (U-2), but the SIZE field that
 * tells RM how much to copy through that pointer used to be forwarded from the
 * guest untouched.  rmapiParamsCopyIn/Out copy that many bytes out of — and
 * back into — the address the stub just supplied, so a record carrying
 * aux_size = 8 with paramsSize = 0x100000 was a ~1 MiB over-read AND over-write
 * past the end of the blob.  Reachable on both stub paths, and the ring path is
 * the worse of the two: there the aux blob is a fixed uint8_t array on the
 * reader thread's own stack, not heap.
 *
 * The function under test is EXTRACTED FROM src/stub/nvkvm_stub.c at build time
 * (see the Makefile rule for u5_clamp.inc) rather than copied here, so this
 * suite cannot drift away from the code it is pinning.  The stub is freestanding
 * and cannot be linked into a hosted test binary, which is why it is extracted
 * rather than #included.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "stub_sanitize.inc"

static int tests_run, tests_passed;

static void chk(const char *name, unsigned type, unsigned nr, uint32_t psz,
		uint32_t declared, uint32_t aux, uint32_t want, unsigned off)
{
	unsigned char buf[64];
	uint32_t got;

	tests_run++;
	memset(buf, 0xAA, sizeof buf);
	memcpy(buf + off, &declared, sizeof declared);
	clamp_inner_params_size(type, nr, buf, psz, aux);
	memcpy(&got, buf + off, sizeof got);

	if (got == want) {
		tests_passed++;
		printf("  ok   %-44s declared=%-10u aux=%-8u -> %u\n",
		       name, declared, aux, got);
	} else {
		printf("  FAIL %-44s declared=%-10u aux=%-8u -> %u (want %u)\n",
		       name, declared, aux, got, want);
	}
}

static void chk_rights(const char *name, unsigned type, unsigned nr,
		       uint32_t psz, uint64_t ptr, uint64_t want)
{
	unsigned char buf[64];
	uint64_t got;

	tests_run++;
	memset(buf, 0xAA, sizeof buf);
	memcpy(buf + 24, &ptr, sizeof ptr);
	zero_nvos64_rights(type, nr, buf, psz);
	memcpy(&got, buf + 24, sizeof got);

	if (got == want) {
		tests_passed++;
		printf("  ok   %-44s 0x%016llx -> 0x%016llx\n",
		       name, (unsigned long long)ptr, (unsigned long long)got);
	} else {
		printf("  FAIL %-44s 0x%016llx -> 0x%016llx (want 0x%016llx)\n",
		       name, (unsigned long long)ptr, (unsigned long long)got,
		       (unsigned long long)want);
	}
}

int main(void)
{
	/* RM_CONTROL: NVOS54.paramsSize at offset 24, struct is 32 bytes. */
	puts("RM_CONTROL  NVOS54.paramsSize @24  ('F' nr 0x2a)");
	chk("the U-5 primitive: 1 MiB via an 8-byte blob",
	    'F', 0x2a, 32, 0x100000, 8, 8, 24);
	chk("equal sizes (the ordinary live path)",
	    'F', 0x2a, 32, 64, 64, 64, 24);
	chk("declared smaller than shipped is allowed",
	    'F', 0x2a, 32, 16, 64, 16, 24);
	chk("no aux blob must force size 0",
	    'F', 0x2a, 32, 32, 0, 0, 24);
	chk("UINT32_MAX",
	    'F', 0x2a, 32, 0xffffffffu, 8, 8, 24);

	/* RM_ALLOC: NVOS64.allocParmsSize at offset 32, struct is 48 bytes. */
	puts("\nRM_ALLOC  NVOS64.allocParmsSize @32  ('F' nr 0x2b)");
	chk("oversized declared alloc params",
	    'F', 0x2b, 48, 0x100000, 48, 48, 32);
	chk("guest-synced equal (the ap_size path)",
	    'F', 0x2b, 48, 48, 48, 48, 32);
	chk("caller's 0 on a probe-guessed window survives",
	    'F', 0x2b, 48, 0, 64, 0, 32);

	/*
	 * Everything else must be left strictly alone: these offsets hold real
	 * data in other layouts, and writing there would be its own bug.
	 */
	puts("\nLayouts carrying no size field must be untouched");
	chk("NVOS21 (RM_ALLOC, 32 bytes, no size field)",
	    'F', 0x2b, 32, 0x100000, 8, 0x100000, 32);
	chk("RM_CONTROL too short to hold offset 24",
	    'F', 0x2a, 24, 0x100000, 8, 0x100000, 24);
	chk("a DRM ioctl (type 'd'), not frontend",
	    'd', 0x2a, 32, 0x100000, 8, 0x100000, 24);
	chk("an unrelated frontend nr",
	    'F', 0x2c, 48, 0x100000, 8, 0x100000, 32);

	/*
	 * U-7.  NVOS64 carries pAllocParms@16 (rewritten by U-2) and
	 * pRightsRequested@24, which no rewrite reached.  The driver does a
	 * 16-byte copy_from_user through it when non-NULL; the value never
	 * returns to the guest, so it is an ASLR oracle rather than a read.
	 * NVOS21 is exactly 32 bytes and its offset 24 is { status, pad } —
	 * real data — hence the param_size > 32 discriminator.
	 */
	puts("\nU-7  NVOS64.pRightsRequested @24  ('F' nr 0x2b)");
	chk_rights("a guest-supplied pointer is zeroed",
		   'F', 0x2b, 48, 0xdeadbeefcafef00dull, 0);
	chk_rights("an already-zero field stays zero",
		   'F', 0x2b, 48, 0, 0);
	chk_rights("NVOS21 (32 bytes) must keep its status word",
		   'F', 0x2b, 32, 0xdeadbeefcafef00dull, 0xdeadbeefcafef00dull);
	chk_rights("RM_CONTROL is not an alloc, leave it alone",
		   'F', 0x2a, 48, 0xdeadbeefcafef00dull, 0xdeadbeefcafef00dull);
	chk_rights("a DRM ioctl (type 'd') is not an alloc",
		   'd', 0x2b, 48, 0xdeadbeefcafef00dull, 0xdeadbeefcafef00dull);

	printf("\n%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed != tests_run;
}
