/*
 * test_uvm_atomics_size.c -- the guest-side gate for Pascal's UVM 54/55.
 *
 * Commit 184cf69 added UVM_{ENABLE,DISABLE}_SYSTEM_WIDE_ATOMICS (54/55) to
 * the QEMU-side isolate schema (nvkvm_uvm_schema[] in
 * src/qemu/nvkvm_isolate_handlers.c) to fix Pascal's cuda_init failure.
 *
 * MEASURED 2026-09-01 on a Quadro P4000, that fix ALONE did not work: with
 * 184cf69 applied, the guest still logged
 *     nvkvm: AUDIT unknown ioctl cmd=0x37 type=0x0 nr=0x37 iocsz=0
 * (cmd 0x37 = 55 decimal) seven times during cuda_init, identical to the
 * pre-fix measurement, with no new dmesg activity afterward.
 *
 * The reason: nvkvm_ioctl() in src/guest/nvkvm_main.c calls
 * nvkvm_ioctl_param_size() (src/guest/nvkvm_ioctl.c) BEFORE anything is
 * forwarded to the host. That function had no case for 54/55, so it returned
 * (size_t)-1 ("unknown"), nvkvm_ioctl() logged the AUDIT line and returned
 * -ENOTTY straight back to the caller -- the QEMU-side schema commit 184cf69
 * patched is never reached at all. Two independent gates guard this ioctl;
 * only one of them had been fixed.
 *
 * This test pins that the guest-side gate ALSO recognizes 54/55, extracted
 * verbatim from the real switch in nvkvm_ioctl.c (see the Makefile rule for
 * uvm_atomics_cases.inc) so it cannot silently drift from the code that
 * actually ships. Revert the two `case` lines between the
 * NVKVM_UVM_ATOMICS_SIZE_BEGIN/END markers and this test fails at runtime
 * (test_param_size falls through to the unknown-ioctl sentinel) -- verified
 * by reverting src/guest/nvkvm_ioctl.c's two case lines and re-running.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "abi/uvm.h"

/* Mirrors nvkvm_ioctl_param_size()'s contract: (size_t)-1 for anything the
 * table does not know about. */
static size_t test_param_size(unsigned int cmd)
{
	switch (cmd) {
#include "uvm_atomics_cases.inc"
	default:
		return (size_t)-1;
	}
}

int main(void)
{
	size_t enable_sz  = test_param_size(UVM_ENABLE_SYSTEM_WIDE_ATOMICS);
	size_t disable_sz = test_param_size(UVM_DISABLE_SYSTEM_WIDE_ATOMICS);

	/* The actual regression: without the fix, both come back as the
	 * "unknown ioctl" sentinel and the guest module rejects the call with
	 * -ENOTTY before QEMU's schema is ever consulted. */
	assert(enable_sz != (size_t)-1);
	assert(disable_sz != (size_t)-1);

	assert(enable_sz == sizeof(struct uvm_enable_system_wide_atomics_params));
	assert(disable_sz == sizeof(struct uvm_disable_system_wide_atomics_params));

	/* An arbitrary unknown command must still fall through -- the switch
	 * has to be selective, not a blanket accept-anything that would pass
	 * this test vacuously. */
	assert(test_param_size(0xdead) == (size_t)-1);

	printf("  ok   UVM_ENABLE_SYSTEM_WIDE_ATOMICS  (54, cmd=0x%x) -> %zu bytes\n",
	       UVM_ENABLE_SYSTEM_WIDE_ATOMICS, enable_sz);
	printf("  ok   UVM_DISABLE_SYSTEM_WIDE_ATOMICS (55, cmd=0x%x) -> %zu bytes\n",
	       UVM_DISABLE_SYSTEM_WIDE_ATOMICS, disable_sz);
	printf("test_uvm_atomics_size: PASS\n");
	return 0;
}
