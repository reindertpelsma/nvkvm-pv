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

	printf("test_ctrl_gate: PASS\n");
	return 0;
}
