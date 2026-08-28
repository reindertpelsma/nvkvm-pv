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

	printf("test_ctrl_gate: PASS\n");
	return 0;
}
