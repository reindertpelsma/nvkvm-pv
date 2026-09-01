/*
 * test_ctrl_gate.c — the default-deny RM-control gate (#76, U-1).
 *
 * The gate exists in one place, nvkvm_ctrl_allowlist.h, because two host-side
 * components apply it: QEMU on the virtqueue path and the isolate stub on the
 * command-buffer ring.  Until 2026-08-19 the ring had no gate at all (U-1), so
 * a guest that flipped its own ring_enable module parameter reached the host
 * driver with commands QEMU would have refused.
 *
 * These assertions are about POLICY, not plumbing: they pin the shape of the
 * default-deny decision so that widening it has to be deliberate.  A GPU is
 * not needed and no ioctl is issued.
 *
 *   make test_ctrl_gate && ./test_ctrl_gate
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "nvkvm_ctrl_allowlist.h"

int main(void)
{
	/* The table is the allowlist: every entry in it must be allowed. */
	for (unsigned i = 0; i < NVKVM_CTRL_ALLOWLIST_N; i++)
		assert(nvkvm_ctrl_cmd_allowed(nvkvm_ctrl_allowlist[i]));
	printf("  ok   all %u table entries allowed\n",
	       (unsigned)NVKVM_CTRL_ALLOWLIST_N);

	/* The two rule-based passthroughs, which cover GSP-routed commands that
	 * carry no app pointers and so are not enumerated in the table. */
	assert(nvkvm_ctrl_cmd_allowed(0x00008000u));   /* RM_GSS_LEGACY_MASK  */
	assert(nvkvm_ctrl_cmd_allowed(0x20818001u));   /* legacy mask + 2081  */
	assert(nvkvm_ctrl_cmd_allowed(0x20810001u));   /* NV2081_BINAPI class */
	printf("  ok   GSS-legacy and NV2081_BINAPI rules pass through\n");

	/* Default-deny.  These are the classes the gate exists to keep out --
	 * reg-ops, HWPM, debug -- plus values that must not be special-cased. */
	static const uint32_t denied[] = {
		0x00000000u,   /* nothing at all                      */
		0x20800122u,   /* NV2080_CTRL_CMD_GPU_REG_OP-shaped   */
		0xffffffffu & ~0x8000u,  /* all bits but the legacy mask */
		0x00002082u,   /* one off the BINAPI class            */
		0xdeadbeefu & ~0x8000u,
	};
	for (unsigned i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
		if (nvkvm_ctrl_cmd_allowed(denied[i])) {
			printf("  FAIL cmd 0x%08x is ALLOWED\n", denied[i]);
			return 1;
		}
	}
	printf("  ok   %u unlisted commands denied\n",
	       (unsigned)(sizeof(denied) / sizeof(denied[0])));

	/* ── gamescope scheduling controls (2026-08-28) ──────────────────
	 *
	 * SteamOS's gamescope session issues five controls while configuring
	 * interleaved/realtime runlist scheduling.  One is unprivileged in
	 * OGKM and is forwarded; four are privileged on ALL 216 supported
	 * tags (NVOC accessRight = 0x2 = RS_ACCESS_NICE, which resolves to
	 * capable(CAP_SYS_NICE)) and are answered NV_OK without ever being
	 * forwarded.  These assertions pin that split so it cannot silently
	 * become "allowed" later.  See
	 * docs/reference/gamescope-scheduling-controls.md.
	 */
	assert(nvkvm_ctrl_cmd_allowed(0x20801109u));   /* FIFO_GET_INFO      */
	assert(!nvkvm_ctrl_cmd_noop(0x20801109u));     /* ...really forwarded*/
	printf("  ok   FIFO_GET_INFO (0x20801109) is forwarded, not no-op'd\n");

	static const uint32_t noop_cmds[] = {
		0x20801115u,   /* FIFO_RUNLIST_SET_SCHED_POLICY */
		0xa06f0111u,   /* RESTART_RUNLIST               */
		0xa06f0109u,   /* SET_INTERLEAVE_LEVEL          */
		0xa06c0110u,   /* MAKE_REALTIME                 */
	};
	for (unsigned i = 0; i < sizeof(noop_cmds) / sizeof(noop_cmds[0]); i++) {
		/* Answered locally... */
		if (!nvkvm_ctrl_cmd_noop(noop_cmds[i])) {
			printf("  FAIL cmd 0x%08x is not no-op'd\n", noop_cmds[i]);
			return 1;
		}
		/* ...and NEVER forwardable.  This is the assertion that
		 * matters: a privileged command must not reach the driver, so
		 * the no-op set and the allowlist must stay disjoint. */
		if (nvkvm_ctrl_cmd_allowed(noop_cmds[i])) {
			printf("  FAIL cmd 0x%08x is ALLOWED (must be no-op "
			       "only -- it needs CAP_SYS_NICE on bare metal)\n",
			       noop_cmds[i]);
			return 1;
		}
	}
	printf("  ok   %u privileged scheduling cmds no-op'd and not allowed\n",
	       (unsigned)(sizeof(noop_cmds) / sizeof(noop_cmds[0])));

	/* The two sets must be disjoint in general, not just for the four
	 * above: a future edit that adds a no-op cmd to the allowlist table
	 * would silently start forwarding it. */
	for (unsigned i = 0; i < NVKVM_CTRL_NOOP_N; i++) {
		if (nvkvm_ctrl_cmd_allowed(nvkvm_ctrl_noop[i])) {
			printf("  FAIL no-op cmd 0x%08x is also allowlisted\n",
			       nvkvm_ctrl_noop[i]);
			return 1;
		}
	}
	printf("  ok   no-op set and allowlist are disjoint (%u no-op entries)\n",
	       (unsigned)NVKVM_CTRL_NOOP_N);

	/* The readback commands that would expose the lie stay denied. */
	assert(!nvkvm_ctrl_cmd_allowed(0xa06c0108u));  /* GET_INTERLEAVE_LEVEL */
	assert(!nvkvm_ctrl_cmd_allowed(0xa06f0110u));  /* GET_INTERLEAVE_LEVEL */
	assert(!nvkvm_ctrl_cmd_noop(0xa06c0108u));
	assert(!nvkvm_ctrl_cmd_noop(0xa06f0110u));
	printf("  ok   GET_INTERLEAVE_LEVEL readbacks stay denied\n");

	/* Nothing else may be no-op'd -- the mechanism is a lie to the guest
	 * and its blast radius is exactly these four commands. */
	assert(!nvkvm_ctrl_cmd_noop(0x00000000u));
	assert(!nvkvm_ctrl_cmd_noop(0xa06c0101u));   /* a real allowed sibling */
	assert(!nvkvm_ctrl_cmd_noop(0x00008000u));   /* GSS-legacy mask        */
	assert(NVKVM_CTRL_NOOP_N == 4);
	printf("  ok   no-op set is exactly the 4 audited commands\n");

	/* NV9096 ZBC: the family must be CONSISTENT.
	 *
	 * This table is generated from nvproxy's compUtil (CUDA-COMPUTE) tagged
	 * set, and a compute workload never sets a colour clear -- so we
	 * inherited the two getters and the STENCIL setter while the COLOUR and
	 * DEPTH setters were denied. Allowing a stencil clear but not a colour
	 * clear is not a boundary anyone chose. OBSERVED 2026-09-01: RDR2 on the
	 * SteamOS guest died at ERR_GFX_INIT with exactly one denial,
	 * 0x90960102, at the moment it launched.
	 *
	 * This is also the DRIVER-INDEPENDENCE check the fix needs.
	 * nvkvm_ctrl_cmd_allowed() takes a cmd and nothing else -- no driver, no
	 * ABI profile, no size -- so a row that is allowed is allowed on every
	 * driver. The only size rule is a global 1 MiB inner-params cap, which a
	 * ZBC struct of a few dozen bytes cannot reach. If anyone ever makes the
	 * gate driver-dependent, this block is where that should get caught. */
	assert(nvkvm_ctrl_cmd_allowed(0x90960101u));  /* GET_ZBC_CLEAR_TABLE      */
	assert(nvkvm_ctrl_cmd_allowed(0x90960102u));  /* SET_ZBC_COLOR_CLEAR      */
	assert(nvkvm_ctrl_cmd_allowed(0x90960103u));  /* SET_ZBC_DEPTH_CLEAR      */
	assert(nvkvm_ctrl_cmd_allowed(0x90960106u));  /* GET_ZBC_CLEAR_TABLE_SIZE */
	assert(nvkvm_ctrl_cmd_allowed(0x90960107u));  /* SET_ZBC_STENCIL_CLEAR    */
	printf("  ok   NV9096 ZBC family is consistently allowed (5 commands)\n");

	/* ...and we did NOT open the class. Only the five audited rows above are
	 * allowed; the rest of 0x9096 stays denied. A wildcard here would be a
	 * much larger change than the bug called for. */
	assert(!nvkvm_ctrl_cmd_allowed(0x90960104u));
	assert(!nvkvm_ctrl_cmd_allowed(0x90960105u));
	assert(!nvkvm_ctrl_cmd_allowed(0x90960108u));
	assert(!nvkvm_ctrl_cmd_allowed(0x909601ffu));
	assert(!nvkvm_ctrl_cmd_allowed(0x90960000u));
	printf("  ok   the rest of the 0x9096 class stays denied (no wildcard)\n");

	/* The gate is a pure function of the command id: same input, same answer,
	 * every time. Nothing driver- or state-dependent may creep in. */
	for (int rep = 0; rep < 3; rep++) {
		assert(nvkvm_ctrl_cmd_allowed(0x90960102u));
		assert(!nvkvm_ctrl_cmd_allowed(0x90960104u));
	}
	printf("  ok   gate answers are stable across repeated calls\n");

	printf("test_ctrl_gate: PASS\n");
	return 0;
}
