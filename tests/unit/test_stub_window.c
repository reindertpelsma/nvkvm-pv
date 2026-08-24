/*
 * test_stub_window.c — U-9, the isolate's guest-mapping window.
 *
 * docs/internal/audit-guest-pointers.md §U-9:
 *
 *     ISOLATE_CMD_MMAP / ISOLATE_CMD_MUNMAP take a raw guest VA. ... `gva`
 *     originates in `nvkvm_req_mmap_on_isolate.gva` off the virtqueue and
 *     reaches `nvkvm_isolate_mmap` untouched -- it is stored and passed,
 *     never bounded.
 *
 * WHAT IS ACTUALLY BEING MEASURED HERE, and why it is not "the DENY line
 * appears".  The isolate's address space is deliberately shared with the
 * guest in places (U-14: the memfd aliasing is what makes cudaHostRegister
 * work), so the property is not "no guest-directed mapping exists in the
 * stub".  It is:
 *
 *     NO ADDRESS OUTSIDE THE RESERVATION IS EVER ACCEPTED, and
 *     NO TWO LIVE PLACEMENTS INSIDE IT EVER OVERLAP,
 *
 * for any inputs, including the hostile arithmetic (u64 wrap, zero length,
 * length larger than the window) that is where a bounds check of this shape
 * actually breaks.  A test that fed it only sensible addresses would pass
 * against a `return 1;` and would have measured nothing.
 *
 * THE LESSON THIS FILE INHERITS.  The A-1 work produced two green results
 * that measured something other than the gate; one of them passed because the
 * GUEST MODULE refused the probe before the ioctl ever left the guest, so it
 * would have passed identically with the gate deleted.  The defence here is
 * the same one tests/unit/test_kvm_slot.c uses: the code under test is
 * EXTRACTED FROM THE REAL SOURCE at build time, not copied, and every case
 * asserts a property of that code's output rather than a side effect.
 *
 *   - stub_window_contains()  <- src/stub/nvkvm_stub.c  (stub_window.inc)
 *   - the window allocator    <- src/qemu/nvkvm_isolate_handlers.c
 *                                (win_alloc.inc)
 *
 * Both extractions are marker-delimited.  If a marker is lost the .inc comes
 * out empty and this file fails to LINK, which is the failure mode we want:
 * a suite that silently stops testing anything is the thing run_tests.sh's
 * pinned counts exist to catch.
 *
 * The stub is freestanding (-nostdlib -ffreestanding) and carries its own
 * entry point, so it cannot be linked into a hosted test binary — extraction
 * is not a stylistic preference here, it is the only way to test the real
 * function at all.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The QEMU-side allocator's extraction pulls in fprintf/UINT64_MAX only. */
#include "stub_window.inc"
#include "win_alloc.inc"

#ifndef STUB_WINDOW_SIZE
#error "stub_window.inc did not carry STUB_WINDOW_SIZE — extraction is broken"
#endif
#ifndef NVKVM_WIN_RUN_SPAN
#error "win_alloc.inc did not carry NVKVM_WIN_RUN_SPAN — extraction is broken"
#endif

static int tests_run, tests_passed;

static void ok(const char *name, bool cond, const char *detail)
{
	tests_run++;
	if (cond) {
		tests_passed++;
		printf("  ok   %-56s %s\n", name, detail ? detail : "");
	} else {
		printf("  FAIL %-56s %s\n", name, detail ? detail : "");
	}
}

/* ── Part A: the gate itself ───────────────────────────────────────────────
 *
 * A plausible-looking window base: high in the mmap region, which is where
 * the kernel actually places it (measured on the reference host: a static-PIE
 * image and its subsequent mmaps land around 0x70-0x7e_xxxx_xxxx_xxxx).
 */
#define WIN_BASE  0x7300000000000ULL
#define WIN_SIZE  STUB_WINDOW_SIZE
#define PAGE      0x1000ULL

static void gate(const char *name, uint64_t addr, uint64_t len, int want)
{
	char detail[160];
	int got = stub_window_contains(addr, len);
	snprintf(detail, sizeof detail, "addr=0x%llx len=0x%llx -> %d",
		 (unsigned long long)addr, (unsigned long long)len, got);
	ok(name, got == want, detail);
}

static void part_a(void)
{
	printf("U-9  stub_window_contains() — the reject-or-map decision\n\n");

	g_win_base = WIN_BASE;
	g_win_size = WIN_SIZE;

	/* ── Inside.  A gate that refuses these breaks every GPU mapping, so
	 * these are as load-bearing as the refusals. ─────────────────────── */
	gate("the whole window",            WIN_BASE, WIN_SIZE, 1);
	gate("the first page",              WIN_BASE, PAGE, 1);
	gate("the last page",               WIN_BASE + WIN_SIZE - PAGE, PAGE, 1);
	gate("a 1 GiB span in the middle",  WIN_BASE + (4ULL << 30),
	     1ULL << 30, 1);

	/* ── Outside, the ordinary way. ──────────────────────────────────── */
	gate("one byte below the base",     WIN_BASE - 1, PAGE, 0);
	gate("one page below the base",     WIN_BASE - PAGE, PAGE, 0);
	gate("exactly at the end",          WIN_BASE + WIN_SIZE, PAGE, 0);
	gate("one page past the end",       WIN_BASE + WIN_SIZE + PAGE, PAGE, 0);

	/* ── Straddling an edge.  This is the case a naive `addr >= base`
	 * test gets right and a naive `addr < base+size` test gets wrong; both
	 * ends must be checked, not just the one the happy path exercises. ── */
	gate("straddling the bottom edge",  WIN_BASE - PAGE, 2 * PAGE, 0);
	gate("straddling the top edge",     WIN_BASE + WIN_SIZE - PAGE,
	     2 * PAGE, 0);

	/* ── The arithmetic.  A bounds check written as
	 *     addr >= base && addr + len <= base + size
	 * accepts every one of these, because both sums wrap.  This is where
	 * this species of gate actually fails, and it is why the cases below
	 * are the ones to keep if any are ever dropped. ──────────────────── */
	gate("zero length",                 WIN_BASE, 0, 0);
	gate("length that wraps u64",       WIN_BASE, UINT64_MAX, 0);
	gate("address that wraps u64",      UINT64_MAX - PAGE, 2 * PAGE, 0);
	gate("addr+len wraps back into it", UINT64_MAX - PAGE,
	     WIN_BASE + 2 * PAGE, 0);
	gate("length one past the window",  WIN_BASE, WIN_SIZE + 1, 0);
	gate("length far past the window",  WIN_BASE, WIN_SIZE << 4, 0);

	/* ── No window at all.  stub_window_init() is fatal on failure, so
	 * this state should be unreachable in production — which is exactly
	 * why it must fail CLOSED rather than be assumed away. ───────────── */
	g_win_base = 0;
	g_win_size = 0;
	gate("no window: nothing is inside it",  WIN_BASE, PAGE, 0);
	gate("no window: not even address 0",    0, PAGE, 0);
	g_win_base = WIN_BASE;
	g_win_size = 0;
	gate("zero-size window admits nothing",  WIN_BASE, PAGE, 0);

	/* Restore for part B's cross-check. */
	g_win_base = WIN_BASE;
	g_win_size = WIN_SIZE;
}

/* ── Part B: the allocator that decides what the gate will see ─────────────
 *
 * A gate is only worth what the thing feeding it is worth.  If the window
 * allocator can hand back an address outside the window, or the same address
 * twice, the stub either refuses a legitimate mapping or silently maps one
 * mapping on top of another — so these are the properties that make the gate
 * mean something, and they are checked against the real allocator source.
 */
#define LIVE_MAX 4096
struct live { uint64_t va, len; };
static struct live live[LIVE_MAX];
static uint32_t    live_n;

static struct nvkvm_iso_window W;

static void win_reset(uint64_t size)
{
	memset(&W, 0, sizeof W);
	W.base   = WIN_BASE;
	W.size   = size;
	W.probed = true;
	live_n   = 0;
}

/* Record a placement and report whether it is inside the window AND disjoint
 * from everything already live.  Returns false on the first violation. */
static bool live_add(uint64_t va, uint64_t len)
{
	len = (len + 4095ULL) & ~4095ULL;
	if (va < W.base || len > W.size || va > W.base + (W.size - len))
		return false;
	for (uint32_t i = 0; i < live_n; i++)
		if (va < live[i].va + live[i].len && live[i].va < va + len)
			return false;
	if (live_n < LIVE_MAX) {
		live[live_n].va  = va;
		live[live_n].len = len;
		live_n++;
	}
	return true;
}

static void live_del(uint64_t va)
{
	for (uint32_t i = 0; i < live_n; i++)
		if (live[i].va == va) { live[i] = live[--live_n]; return; }
}

static void part_b(void)
{
	char detail[160];

	printf("\nU-9  the window allocator — what the gate is fed\n\n");

	/* ── Every placement is inside the window, and no two live placements
	 * overlap.  1500 tiny mappings is not an arbitrary number: one
	 * cuCtxCreate issues >1500 4 KiB device mmaps (see the note on the
	 * sparse window in nvkvm_req_mmap_on_isolate). ───────────────────── */
	win_reset(WIN_SIZE);
	{
		bool good = true;
		uint64_t first = 0, last = 0;
		for (int i = 0; i < 1500 && good; i++) {
			uint64_t va = win_place_locked(&W, 0, PAGE, false);
			if (!va) { good = false; break; }
			if (!first) first = va;
			last = va;
			good = live_add(va, PAGE);
		}
		snprintf(detail, sizeof detail, "%u live, 0x%llx..0x%llx",
			 live_n, (unsigned long long)first,
			 (unsigned long long)last);
		ok("1500 device-sized mappings: in window, disjoint",
		   good, detail);
	}

	/* ── Cross-check against the gate.  Anything the allocator returns
	 * must be something the stub will accept; if these two ever disagree
	 * the product is broken in one direction or insecure in the other. ── */
	{
		bool good = true;
		for (uint32_t i = 0; i < live_n; i++)
			if (!stub_window_contains(live[i].va, live[i].len))
				good = false;
		snprintf(detail, sizeof detail, "%u placements", live_n);
		ok("every placement passes stub_window_contains()", good, detail);
	}

	/* ── The OS-descriptor run.  The guest's migration installs a
	 * registration in 2 MiB chunks at strictly increasing, adjacent guest
	 * VAs (nvkvm_mmap.c: cbase = start + coff), and RmAllocOsDescriptor
	 * then pins ONE range covering all of them — so the chunks must come
	 * back window-adjacent or there is no address to hand the driver. ── */
	win_reset(WIN_SIZE);
	{
		const uint64_t CHUNK = 2ULL << 20;
		const uint64_t GVA   = 0x7f1122200000ULL;   /* a guest address */
		uint64_t prev = 0, first = 0;
		bool good = true;
		for (int i = 0; i < 8 && good; i++) {
			uint64_t va = win_place_locked(&W, GVA + i * CHUNK,
						       CHUNK, true);
			if (!va || !live_add(va, CHUNK)) { good = false; break; }
			if (i == 0) first = va;
			else if (va != prev + CHUNK) good = false;
			prev = va;
		}
		snprintf(detail, sizeof detail,
			 "16 MiB in 8 chunks from 0x%llx", (unsigned long long)first);
		ok("a 16 MiB registration lands contiguously", good, detail);
	}

	/* ── The same, INTERLEAVED with unrelated mappings.  This is the case
	 * a greedy bump allocator gets wrong: two guest threads registering at
	 * once, or any device mmap arriving mid-migration, would split the run
	 * and the A-1 gate would then refuse a legitimate registration.  The
	 * run reservation is what makes this hold. ───────────────────────── */
	win_reset(WIN_SIZE);
	{
		const uint64_t CHUNK = 2ULL << 20;
		const uint64_t GVA_A = 0x7f1122200000ULL;
		const uint64_t GVA_B = 0x7e0044400000ULL;
		uint64_t pa = 0, pb = 0, fa = 0, fb = 0;
		bool good = true;

		for (int i = 0; i < 8 && good; i++) {
			uint64_t a = win_place_locked(&W, GVA_A + i * CHUNK,
						      CHUNK, true);
			/* something else entirely, between the chunks */
			uint64_t x = win_place_locked(&W, 0, PAGE, false);
			uint64_t b = win_place_locked(&W, GVA_B + i * CHUNK,
						      CHUNK, true);
			if (!a || !b || !x) { good = false; break; }
			if (!live_add(a, CHUNK) || !live_add(x, PAGE) ||
			    !live_add(b, CHUNK)) { good = false; break; }
			if (i == 0) { fa = a; fb = b; }
			else if (a != pa + CHUNK || b != pb + CHUNK) good = false;
			pa = a; pb = b;
		}
		snprintf(detail, sizeof detail, "A@0x%llx B@0x%llx, disjoint",
			 (unsigned long long)fa, (unsigned long long)fb);
		ok("two interleaved registrations each stay contiguous",
		   good, detail);
	}

	/* ── Release and reuse.  Without this a map/unmap loop walks the bump
	 * pointer to the end of the window and the isolate stops being able to
	 * map anything — the same bug the sparse GPA window's free list was
	 * added to fix (#80/H-1). ────────────────────────────────────────── */
	win_reset(WIN_SIZE);
	{
		bool good = true;
		uint64_t high = 0;
		for (int i = 0; i < 20000 && good; i++) {
			uint64_t va = win_place_locked(&W, 0, PAGE, false);
			if (!va || !live_add(va, PAGE)) { good = false; break; }
			if (va > high) high = va;
			win_release_locked(&W, va - W.base, PAGE);
			live_del(va);
		}
		snprintf(detail, sizeof detail,
			 "20000 map/unmap, high water 0x%llx (+%llu KiB)",
			 (unsigned long long)high,
			 (unsigned long long)((high - W.base) >> 10));
		ok("map/unmap churn does not walk the window",
		   good && (high - W.base) < (1ULL << 20), detail);
	}

	/* ── Exhaustion fails CLOSED.  A full window must return 0, never an
	 * address past the end — the stub would refuse that anyway, but an
	 * allocator that produces one is a bug that only shows up as an
	 * unexplained DENY in production. ────────────────────────────────── */
	win_reset(64ULL << 20);          /* a deliberately tiny window */
	{
		bool good = true, hit_end = false;
		for (int i = 0; i < 64 && good; i++) {
			uint64_t va = win_place_locked(&W, 0, 4ULL << 20, false);
			if (!va) { hit_end = true; break; }
			good = live_add(va, 4ULL << 20);
		}
		snprintf(detail, sizeof detail, "%u placed before refusal",
			 live_n);
		ok("exhaustion returns 0, never an out-of-window address",
		   good && hit_end, detail);
	}

	/* ── An oversize request is refused outright rather than truncated.
	 * REJECT, NEVER CLAMP is the convention every gate in this tree
	 * follows, and it matters most here: a clamped mapping is one the
	 * caller does not know the bounds of. ────────────────────────────── */
	win_reset(WIN_SIZE);
	{
		uint64_t va = win_place_locked(&W, 0, WIN_SIZE + PAGE, false);
		snprintf(detail, sizeof detail, "-> 0x%llx",
			 (unsigned long long)va);
		ok("a request larger than the window is refused", va == 0,
		   detail);
	}

	/* ── A window that was never probed places nothing.  This is the
	 * fail-closed direction for an isolate whose WINDOW_INFO round trip
	 * failed: no base means no placement, not placement at 0. ────────── */
	{
		struct nvkvm_iso_window empty;
		memset(&empty, 0, sizeof empty);
		uint64_t va = win_place_locked(&empty, 0, PAGE, false);
		ok("an unprobed window places nothing", va == 0, "-> 0");
	}
}

int main(void)
{
	part_a();
	part_b();
	printf("\n%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
