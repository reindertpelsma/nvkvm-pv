/*
 * test_kvm_slot.c — A-21, the KVM memslot allocator's release gate.
 *
 * docs/internal/audit-boundaries-2026-08-20.md §9:
 *
 *     A-21 — nvkvm_kvm_slot_release() does not validate the slot.  It does not
 *     check that the slot being released was ever allocated.
 *
 * Why that matters, and what these cases are actually measuring.  A slot
 * released twice was pushed onto the freelist twice, so the next two
 * nvkvm_kvm_slot_alloc() calls returned the SAME KVM memslot number to two
 * unrelated mappings.  Both then call KVM_SET_USER_MEMORY_REGION on it with
 * their own GPA and their own host VA, last writer wins, and a guest physical
 * range ends up pointing at another isolate's device memory.  Cross-isolate
 * aliasing arranged through the allocator, underneath every ownership gate
 * above it.
 *
 * So the property under test is not "release logs something".  It is:
 *
 *     NO TWO LIVE ALLOCATIONS EVER SHARE A SLOT NUMBER,
 *     for any sequence of releases, valid or not.
 *
 * That is what `alloc_distinct` below checks after every hostile release, and
 * it is what fails when the gate is removed.  A test that only counted
 * refusals would pass against a build that logs and then aliases anyway —
 * which is the false-pass the A-1 work hit twice.
 *
 * THE CODE UNDER TEST IS EXTRACTED from src/qemu/nvkvm_mmap_host.c at build
 * time (the kvm_slot_pool.inc rule in the Makefile), between the
 * NVKVM_KVM_SLOT_POOL_BEGIN/_END markers, rather than copied here — same
 * reason as test_stub_ptr_sanitize.c.  A copy drifts; an extraction cannot.
 * If the markers are lost the .inc is empty and this file fails to link.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "kvm_slot_pool.inc"

/* Sentinel from nvkvm_isolate_handlers.c: "mapping lives in the sparse window,
 * there is no per-mmap memslot".  It reaches release() through the shared
 * teardown paths and must be refused, not indexed with. */
#define IN_WINDOW_SLOT (-2)

static int tests_run, tests_passed;

static void ok(const char *name, bool cond, const char *detail)
{
	tests_run++;
	if (cond) {
		tests_passed++;
		printf("  ok   %-54s %s\n", name, detail ? detail : "");
	} else {
		printf("  FAIL %-54s %s\n", name, detail ? detail : "");
	}
}

/* Reset the pool between cases.  Touches the same statics the code does; this
 * is the one thing the test knows about the implementation, and it is
 * unavoidable for a file-static allocator. */
static void pool_reset(void)
{
	kvm_slot_water      = NVKVM_KVM_SLOT_BASE;
	kvm_slot_free_head  = 0;
	kvm_slot_in_use     = 0;
	kvm_slot_in_use_peak = 0;
	kvm_slot_alloc_count = 0;
	kvm_slot_free_count  = 0;
	kvm_slot_reject_count = 0;
	memset(kvm_slot_live, 0, sizeof kvm_slot_live);
	memset(kvm_slot_free_stack, 0, sizeof kvm_slot_free_stack);
}

/* Allocate n slots and report whether they are pairwise distinct and in range.
 * This is the invariant the whole finding is about. */
static bool alloc_distinct(int *out, int n)
{
	bool good = true;
	for (int i = 0; i < n; i++) {
		out[i] = nvkvm_kvm_slot_alloc();
		if (out[i] < NVKVM_KVM_SLOT_BASE ||
		    out[i] >= NVKVM_KVM_SLOT_BASE + NVKVM_KVM_SLOT_COUNT)
			good = false;
		for (int j = 0; j < i; j++)
			if (out[i] == out[j])
				good = false;
	}
	return good;
}

int main(void)
{
	int  s[8];
	char detail[160];

	printf("A-21  nvkvm_kvm_slot_release() — range and ownership\n\n");

	/* ── The happy path still works.  A gate that breaks recycling would
	 * exhaust the 448-slot pool after a few CUDA processes, which is the
	 * exact bug the freelist was added to fix. ───────────────────────── */
	pool_reset();
	{
		bool distinct = alloc_distinct(s, 3);
		snprintf(detail, sizeof detail, "%d %d %d", s[0], s[1], s[2]);
		ok("three allocations are distinct and in range", distinct,
		   detail);
	}

	pool_reset();
	{
		int a = nvkvm_kvm_slot_alloc();
		nvkvm_kvm_slot_release(a);
		int b = nvkvm_kvm_slot_alloc();
		snprintf(detail, sizeof detail, "alloc %d, release, alloc %d",
			 a, b);
		ok("a released slot is recycled", a == b, detail);
	}

	pool_reset();
	{
		int a = nvkvm_kvm_slot_alloc();
		nvkvm_kvm_slot_release(a);
		int in_use = -1;
		nvkvm_kvm_slot_stats(&in_use, NULL, NULL, NULL);
		snprintf(detail, sizeof detail, "in_use=%d", in_use);
		ok("in_use returns to zero after a valid release",
		   in_use == 0, detail);
	}

	/* ── THE FINDING.  Release the same slot twice, then allocate twice.
	 * Before the fix both allocations returned the double-released slot;
	 * that is the aliasing primitive.  This case fails if the ownership
	 * gate is removed. ─────────────────────────────────────────────── */
	pool_reset();
	{
		int a = nvkvm_kvm_slot_alloc();
		nvkvm_kvm_slot_release(a);
		nvkvm_kvm_slot_release(a);          /* <- the double release */
		bool distinct = alloc_distinct(s, 2);
		snprintf(detail, sizeof detail,
			 "released %d twice, then got %d and %d",
			 a, s[0], s[1]);
		ok("a double release cannot alias two allocations",
		   distinct, detail);
	}

	pool_reset();
	{
		int a = nvkvm_kvm_slot_alloc();
		nvkvm_kvm_slot_release(a);
		uint64_t before = nvkvm_kvm_slot_rejects();
		nvkvm_kvm_slot_release(a);
		uint64_t after = nvkvm_kvm_slot_rejects();
		snprintf(detail, sizeof detail, "rejects %llu -> %llu",
			 (unsigned long long)before,
			 (unsigned long long)after);
		ok("the second release is refused, not absorbed",
		   after == before + 1, detail);
	}

	/* Ten releases of one slot must not put ten copies in the freelist. */
	pool_reset();
	{
		int a = nvkvm_kvm_slot_alloc();
		for (int i = 0; i < 10; i++)
			nvkvm_kvm_slot_release(a);
		bool distinct = alloc_distinct(s, 8);
		snprintf(detail, sizeof detail,
			 "released %d ten times; 8 allocs all distinct: %s",
			 a, distinct ? "yes" : "NO");
		ok("repeated release does not stack the freelist",
		   distinct, detail);
	}

	/* ── A slot that was never handed out at all.  In range, so the old
	 * range check let it through, and it then collided with whatever the
	 * allocator handed out for real. ──────────────────────────────── */
	pool_reset();
	{
		const int never = NVKVM_KVM_SLOT_BASE + 100;
		int a = nvkvm_kvm_slot_alloc();
		nvkvm_kvm_slot_release(never);
		int b = nvkvm_kvm_slot_alloc();
		snprintf(detail, sizeof detail,
			 "released %d (never allocated), next alloc gave %d",
			 never, b);
		ok("releasing a never-allocated in-range slot is refused",
		   b != never && b != a, detail);
	}

	/* ── Out of pool.  Slot 0 is guest RAM; NVKVM_IN_WINDOW_SLOT is the
	 * "no memslot" sentinel that reaches the shared teardown paths. ── */
	pool_reset();
	{
		const int bad[] = { 0, 1, 63, -1, IN_WINDOW_SLOT,
				    NVKVM_KVM_SLOT_BASE + NVKVM_KVM_SLOT_COUNT,
				    100000 };
		uint64_t before = nvkvm_kvm_slot_rejects();
		for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
			nvkvm_kvm_slot_release(bad[i]);
		uint64_t after = nvkvm_kvm_slot_rejects();
		bool distinct = alloc_distinct(s, 4);
		snprintf(detail, sizeof detail,
			 "%u refused, then %d %d %d %d",
			 (unsigned)(sizeof bad / sizeof bad[0]),
			 s[0], s[1], s[2], s[3]);
		ok("out-of-pool releases are all refused",
		   after == before + sizeof bad / sizeof bad[0], detail);
		ok("and they do not contaminate the freelist", distinct, NULL);
	}

	/* ── Exhaustion still behaves: -1 when empty, and the pool comes back
	 * in full after everything is released exactly once. ───────────── */
	pool_reset();
	{
		static int all[NVKVM_KVM_SLOT_COUNT];
		bool all_ok = true;
		for (int i = 0; i < NVKVM_KVM_SLOT_COUNT; i++) {
			all[i] = nvkvm_kvm_slot_alloc();
			if (all[i] < 0) all_ok = false;
		}
		int overflow = nvkvm_kvm_slot_alloc();
		snprintf(detail, sizeof detail,
			 "%d allocated, next = %d",
			 NVKVM_KVM_SLOT_COUNT, overflow);
		ok("the pool exhausts at exactly its size",
		   all_ok && overflow == -1, detail);

		for (int i = 0; i < NVKVM_KVM_SLOT_COUNT; i++)
			nvkvm_kvm_slot_release(all[i]);
		int in_use = -1;
		nvkvm_kvm_slot_stats(&in_use, NULL, NULL, NULL);
		bool refilled = alloc_distinct(s, 4);
		snprintf(detail, sizeof detail, "in_use=%d, refill %d %d %d %d",
			 in_use, s[0], s[1], s[2], s[3]);
		ok("and refills completely after a full release",
		   in_use == 0 && refilled, detail);
	}

	/* ── A live slot must never be handed out twice even if the freelist
	 * is corrupted directly.  This is the alloc-side belt-and-braces
	 * gate; it is reached by planting a duplicate behind release()'s
	 * back, which is the only way in once release() is closed. ────── */
	pool_reset();
	{
		int a = nvkvm_kvm_slot_alloc();
		kvm_slot_free_stack[kvm_slot_free_head++] = a;  /* corrupt */
		int b = nvkvm_kvm_slot_alloc();
		snprintf(detail, sizeof detail,
			 "planted live slot %d on the freelist, alloc gave %d",
			 a, b);
		ok("alloc refuses a freelist entry that is already live",
		   b != a, detail);
	}

	printf("\n%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
