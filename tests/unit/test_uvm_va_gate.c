/*
 * test_uvm_va_gate.c — the U-6 UVM VA-ownership gate (src/qemu/nvkvm_uvm_va.h).
 *
 * WHAT THIS PINS, AND WHY IT IS NOT A LOG-LINE TEST
 *
 * The gate makes two decisions per UVM ioctl that names a VA range:
 *
 *   1. IS THE RANGE OWNED?  Only a range this VM's own CREATE recorded, for the
 *      SAME UVM handle, may be named by a later command.  That is U-6, and it
 *      is the property that keeps a guest from pointing UVM_MIGRATE at QEMU's
 *      own heap (docs/internal/audit-guest-pointers.md).
 *
 *   2. IF IT IS REFUSED, IS THAT A SECURITY EVENT?  UVM_VALIDATE_VA_RANGE (72)
 *      only ASKS whether an extent is a range; the driver's own answer for an
 *      unowned extent is NV_ERR_INVALID_ADDRESS (uvm_va_range.c:762-777), so a
 *      refusal there is an answer, not a blocked operation.  Every managed
 *      allocation on Turing/Ampere/Ada/Blackwell was writing one of those into
 *      the DENY log, which is the channel that exists to make guest probes
 *      visible.
 *
 * The assertions below are on the DECISIONS — uvm_va_covers() and the schema's
 * classification — not on any string.  A test that grepped for "DENY" would
 * pass just as happily if the ownership check had been deleted, and this tree
 * has already produced tests that measured the wrong thing.  So: the refusal
 * itself is asserted to be UNCHANGED for every mode (an unowned range is still
 * refused, cmd 72 included), and only the channel is asserted to differ.
 *
 *   make test_uvm_va_gate && ./test_uvm_va_gate
 */
#include <assert.h>
#include <stdio.h>

#include "nvkvm_uvm_va.h"

#define OK(fmt, ...) do { printf("  ok   " fmt "\n", ##__VA_ARGS__); n_ok++; } while (0)

static int n_ok;

/* One 2 MiB range, the shape the guest's managed-memory fallback creates:
 * UVM_CREATE_EXTERNAL_RANGE(base=G, length=vma_len) on the fd that got the
 * mmap (src/guest/nvkvm_uvm_ext.c). */
#define H_A     7u
#define H_B     9u
#define BASE_A  0x7f0000000000ull
#define LEN_A   0x200000ull      /* 2 MiB */
#define BASE_B  (BASE_A + LEN_A) /* the adjacent range, deliberately touching */

int main(void)
{
	const struct nvkvm_uvm_desc *d;

	/* ── 1. the schema says what each command does with its range ────── */
	d = nvkvm_uvm_lookup(73);       /* CREATE_EXTERNAL_RANGE */
	assert(d && d->va_mode == NVKVM_UVM_VA_CREATE);
	d = nvkvm_uvm_lookup(34);       /* FREE */
	assert(d && d->va_mode == NVKVM_UVM_VA_FREE);
	d = nvkvm_uvm_lookup(51);       /* MIGRATE — the reason U-6 exists */
	assert(d && d->va_mode == NVKVM_UVM_VA_USE);
	d = nvkvm_uvm_lookup(72);       /* VALIDATE_VA_RANGE */
	assert(d && d->va_mode == NVKVM_UVM_VA_QUERY);
	assert(nvkvm_uvm_lookup(62) == NULL);   /* TOOLS_READ_PROCESS_MEMORY */
	assert(nvkvm_uvm_lookup(63) == NULL);   /* TOOLS_WRITE_PROCESS_MEMORY */
	OK("schema: 73=CREATE 34=FREE 51=USE 72=QUERY, 62/63 default-denied");

	/* Every mode that names a range it did not create is still checked for
	 * ownership.  QUERY is a LOG classification, not an exemption — if this
	 * ever reads false for 72, the gate has been removed, not relaxed. */
	assert(nvkvm_uvm_mode_needs_ownership(NVKVM_UVM_VA_USE));
	assert(nvkvm_uvm_mode_needs_ownership(NVKVM_UVM_VA_FREE));
	assert(nvkvm_uvm_mode_needs_ownership(NVKVM_UVM_VA_QUERY));
	assert(!nvkvm_uvm_mode_needs_ownership(NVKVM_UVM_VA_CREATE));
	assert(!nvkvm_uvm_mode_needs_ownership(NVKVM_UVM_VA_NONE));
	OK("USE, FREE and QUERY all require ownership; CREATE/NONE do not");

	/* ── 2. the legitimate case: a recorded range answers YES ────────── */
	assert(uvm_va_have_room());
	assert(uvm_va_add(H_A, BASE_A, LEN_A));

	/* Exactly what libcuda validates after the fallback's
	 * CREATE_EXTERNAL_RANGE: the same base, the same length, the same
	 * handle.  This is the case whose refusal would be a real bug. */
	assert(uvm_va_covers(H_A, BASE_A, LEN_A));
	OK("VALIDATE of the exact recorded extent is owned (the legitimate case)");

	/* Sub-extents of it too: U-6 is containment, deliberately WEAKER than
	 * the driver's exact-match, so it never refuses what the driver would
	 * accept.  (libcuda suballocates ~32 x 64 KiB inside one 2 MiB range.) */
	assert(uvm_va_covers(H_A, BASE_A, 0x10000ull));
	assert(uvm_va_covers(H_A, BASE_A + 0x10000ull, 0x10000ull));
	assert(uvm_va_covers(H_A, BASE_A + LEN_A - 0x1000ull, 0x1000ull));
	OK("sub-extents of a recorded range are owned (64 KiB suballocations)");

	/* ── 3. an unowned range is still refused — every mode ───────────── */
	assert(!uvm_va_covers(H_A, BASE_A - 0x1000ull, LEN_A));   /* starts before */
	assert(!uvm_va_covers(H_A, BASE_A, LEN_A + 0x1000ull));   /* ends after   */
	assert(!uvm_va_covers(H_A, 0x1000ull, 0x1000ull));        /* never seen   */
	assert(!uvm_va_covers(H_A, BASE_A, 0));                   /* zero length  */
	OK("an extent that overruns, underruns or misses the range is refused");

	/* A different UVM handle is a different uvm_va_space: ownership does
	 * not carry across, and two guests legitimately pick the same base. */
	assert(!uvm_va_covers(H_B, BASE_A, LEN_A));
	assert(uvm_va_add(H_B, BASE_A, LEN_A));
	assert(uvm_va_covers(H_B, BASE_A, LEN_A));
	OK("ownership is per-handle; the same base on another handle is separate");

	/* Two adjacent ranges do NOT stitch into one.  This is where U-6
	 * differs from A-1's iso_mmap_covers(), and the difference is
	 * deliberate: A-1's chunks are one host-installed registration the host
	 * itself split, whereas these are two distinct driver objects.  The
	 * driver agrees — uvm_api_validate_va_range() requires ONE va_range
	 * matching exactly, so a spanning request is refused there too, and
	 * unioning here would forward a call the driver refuses anyway. */
	assert(uvm_va_add(H_A, BASE_B, LEN_A));
	assert(uvm_va_covers(H_A, BASE_B, LEN_A));
	assert(!uvm_va_covers(H_A, BASE_A, LEN_A * 2));
	OK("a request spanning two adjacent ranges is refused, not unioned");

	/* ── 4. the classification, which is all the fix changed ─────────── */
	assert(nvkvm_uvm_refusal_is_security(NVKVM_UVM_VA_USE));
	assert(nvkvm_uvm_refusal_is_security(NVKVM_UVM_VA_FREE));
	assert(!nvkvm_uvm_refusal_is_security(NVKVM_UVM_VA_QUERY));
	assert(!nvkvm_uvm_refusal_is_security(nvkvm_uvm_lookup(72)->va_mode));
	assert(nvkvm_uvm_refusal_is_security(nvkvm_uvm_lookup(51)->va_mode));
	assert(nvkvm_uvm_refusal_is_security(nvkvm_uvm_lookup(42)->va_mode));
	OK("refusing 72 is not a security event; refusing 51/42 still is");

	/* ── 5. teardown: a freed or purged range stops being owned ──────── */
	uvm_va_drop(H_A, BASE_A, LEN_A);
	assert(!uvm_va_covers(H_A, BASE_A, LEN_A));
	assert(uvm_va_covers(H_A, BASE_B, LEN_A));    /* the neighbour survives */
	assert(uvm_va_covers(H_B, BASE_A, LEN_A));    /* the other handle too   */
	assert(uvm_va_len_at(H_A, BASE_B) == LEN_A);  /* UVM_FREE's length=0    */
	assert(uvm_va_len_at(H_A, BASE_A) == 0);
	OK("a freed range is no longer owned; neighbours and other handles are");

	uvm_va_purge_handle(H_A);
	assert(!uvm_va_covers(H_A, BASE_B, LEN_A));
	assert(uvm_va_covers(H_B, BASE_A, LEN_A));
	uvm_va_purge_handle(H_B);
	assert(!uvm_va_covers(H_B, BASE_A, LEN_A));
	OK("purging a handle forgets its ranges and only its ranges");

	/* ── 6. malformed ranges never reach the table ───────────────────── */
	assert(!uvm_va_sane(BASE_A, 0));                       /* zero length  */
	assert(!uvm_va_sane(0xfffffffffffff000ull, 0x2000ull));/* u64 wrap     */
	assert(!uvm_va_sane(BASE_A + 1, 0x1000ull));           /* unaligned    */
	assert(!uvm_va_sane(BASE_A, 0x1001ull));               /* unaligned len*/
	assert(uvm_va_sane(BASE_A, 0x1000ull));
	OK("zero-length, wrapping and unaligned ranges are malformed");

	printf("%d/%d tests passed\n", n_ok, n_ok);
	return 0;
}
