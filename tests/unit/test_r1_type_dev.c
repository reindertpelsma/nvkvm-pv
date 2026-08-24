/*
 * test_r1_type_dev.c — R-1: branch selection must key on the handle's real
 *                      device, not on the _IOC_TYPE the guest supplies.
 *
 * WHAT R-1 WAS.  Every host-side sanitiser in nvkvm_req_ioctl_on_isolate() and
 * in the stub's worker path is selected by _IOC_TYPE(cmd) — 'd' picks the DRM
 * NR allowlist, 'F' picks the frontend NR allowlist plus U-3, A-1, the
 * alloc-class and control-cmd allowlists, the DUP_OBJECT source gate and the
 * H-3 hClient gate, and the stub re-derives the same field to decide struct
 * layout (ptr_off), whether to clamp the inner-params size (U-5), whether to
 * zero NVOS64's pRightsRequested (U-7), whether to translate embedded fds, and
 * whether to run the NV_ESC_RM_IDLE_CHANNELS memset.  _IOC_TYPE is a field of a
 * command word the GUEST composes.  The fd the ioctl lands on comes from
 * req->handle_id, an independent guest-chosen value.  Nothing tied them.
 *
 * The concrete bypass: _IOWR('d', 0x41, ...) aimed at a /dev/nvidiactl handle.
 * 0x41 is simultaneously DRM_COMMAND_BASE+0x01 (allowed by the DRM allowlist)
 * and NV_ESC_RM_IDLE_CHANNELS (frontend).  The 'd' branch admits it, fourteen
 * 'F'-keyed gates never run, and the NVIDIA frontend dispatches on _IOC_NR
 * while ignoring _IOC_TYPE — reopening G-2, the NVOS30 pointer-array walk this
 * project already found and fixed once.
 *
 * WHAT THIS PINS.  The PROPERTY, not a log line and not a return code: for
 * every (handle type, dev_id) a handle can hold, at most one command shape is
 * accepted, and it is the one that device actually speaks.  Both halves of the
 * fix are asserted, and both are EXTRACTED FROM PRODUCTION SOURCE at build time
 * (see the r1_type_dev.inc / r1_stub_type_dev.inc rules in the Makefile) rather
 * than copied here, so this suite cannot drift away from the code it pins.  If
 * either extraction comes back empty the compile fails on an undefined symbol,
 * which is the failure mode we want.
 *
 * WHY IT IS A HOST-SIDE UNIT TEST AND NOT A GUEST PROBE.  tests/security's A-1
 * probes carry a warning that the guest MODULE refuses their negative cases
 * before the ioctl ever leaves the guest, so they pass identically with the
 * host gate removed.  R-1 has that failure mode twice over: the guest's own
 * size table refuses a non-'F' type (src/guest/nvkvm_ioctl.c:143) and its DRM
 * node is served by the in-guest drm_ioctl(), which rejects a non-'d' type
 * itself.  A probe run from guest userspace would therefore pass against a
 * completely ungated host.  This suite calls the host's decision function
 * directly; there is nothing in front of it to give a false pass, and no GPU is
 * needed.
 *
 *   make test_r1_type_dev && ./test_r1_type_dev
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>

#include "abi/nvgpu.h"
#include "nvkvm_proto.h"
#include "nvkvm_handle.h"
#include "nvkvm_drm_allowlist.h"

/* The QEMU half, lifted verbatim out of src/qemu/nvkvm_isolate_handlers.c. */
#include "r1_type_dev.inc"

/*
 * The stub half, lifted verbatim out of src/stub/nvkvm_stub.c.  It reads the
 * per-handle device through handle_dev_lookup(); the stub's own table is a
 * 64K array we do not want here, so the test supplies the lookup and drives it
 * with one variable.  NVKVM_DEV_UNKNOWN comes from the stub source too, so it
 * is declared the same way there and here would collide — take it from the
 * extraction instead by declaring the variable before the include.
 */
static uint32_t stub_dev_under_test;
static uint32_t handle_dev_lookup(uint32_t id)
{
	(void)id;
	return stub_dev_under_test;
}
#include "r1_stub_type_dev.inc"

static int tests_run, tests_passed;

/* One command word, one handle, one expected verdict. */
static void chk(const char *what, int htype, int dev, uint32_t cmd, bool want)
{
	bool got = nvkvm_ioctl_type_matches_dev(htype, dev, cmd);

	tests_run++;
	if (got == want) {
		tests_passed++;
		printf("  ok   %-52s dev=%-3d cmd=0x%08x -> %s\n",
		       what, dev, cmd, got ? "ALLOW" : "DENY");
	} else {
		printf("  FAIL %-52s dev=%-3d cmd=0x%08x -> %s (want %s)\n",
		       what, dev, cmd, got ? "ALLOW" : "DENY",
		       want ? "ALLOW" : "DENY");
	}
}

/* A whole class of (dev, cmd) pairs in one assertion. */
static void chk_all(const char *what, bool ok)
{
	tests_run++;
	if (ok) {
		tests_passed++;
		printf("  ok   %s\n", what);
	} else {
		printf("  FAIL %s\n", what);
	}
}

/* _IOWR(type, nr, 4 bytes) — the shape a guest would forge. */
static uint32_t iowr(unsigned type, unsigned nr, unsigned size)
{
	return (uint32_t)_IOC(_IOC_READ | _IOC_WRITE, type, nr, size);
}

int main(void)
{
	const int NV = NVKVM_HANDLE_TYPE_NVIDIA;
	const int MEM = NVKVM_HANDLE_TYPE_MEMORY;

	/* ── 1. The exploit itself, and its three siblings ──────────────────
	 * The four NRs that are in BOTH the DRM allowlist and the frontend
	 * ioctl table.  Each, typed 'd' and aimed at a frontend handle, used to
	 * skip every 'F'-keyed gate.  0x41 is the severe one (NVOS30 /
	 * RM_IDLE_CHANNELS / G-2); the other three bypass the H-3 hClient gate.
	 */
	printf("R-1: the type-confusion bypass\n");
	chk("'d' 0x41 (IDLE_CHANNELS as GEM_IMPORT) on nvidiactl",
	    NV, NVKVM_DEV_CTL, iowr('d', 0x41, 40), false);
	chk("'d' 0x4f (UNMAP_MEMORY as DMABUF_SUPPORTED) on nvidiactl",
	    NV, NVKVM_DEV_CTL, iowr('d', 0x4f, 32), false);
	chk("'d' 0x54 (ALLOC_CONTEXT_DMA2 as SEMSURF_CTX) on nvidiactl",
	    NV, NVKVM_DEV_CTL, iowr('d', 0x54, 40), false);
	chk("'d' 0x57 (MAP_MEMORY_DMA as SEMSURF_ATTACH) on nvidiactl",
	    NV, NVKVM_DEV_CTL, iowr('d', 0x57, 40), false);
	chk("'d' 0x41 on /dev/nvidia0 (the same, other frontend node)",
	    NV, NVKVM_DEV_GPU(0), iowr('d', 0x41, 40), false);

	/* ── 2. The reverse direction (R-2) ─────────────────────────────────
	 * 'F' with an NR the DRM allowlist deliberately EXCLUDES (G-3), aimed
	 * at a render node.  drm_ioctl() rejects a mismatched type itself
	 * (-ENOTTY), so this was inert — but that is a control in a kernel we
	 * do not own, and this assertion is what makes it ours.
	 */
	printf("\nR-2: the reverse direction\n");
	chk("'F' 0x4a (VID_HEAP_CONTROL as GEM_MAP_OFFSET) on renderD128",
	    NV, NVKVM_DEV_DRM_RD(0), iowr('F', 0x4a, 200), false);
	chk("'F' 0x4e (MAP_MEMORY as GEM_IDENTIFY_OBJECT) on renderD128",
	    NV, NVKVM_DEV_DRM_RD(0), iowr('F', 0x4e, 40), false);
	chk("'F' 0x2b (RM_ALLOC) on renderD128",
	    NV, NVKVM_DEV_DRM_RD(0), iowr('F', 0x2b, 48), false);

	/* ── 3. NVKMS in both directions ────────────────────────────────────
	 * The modeset device speaks exactly one command word.  Nothing else may
	 * reach it, and it may not reach anything else.
	 */
	printf("\nNVKMS\n");
	chk("the NVKMS wrapper cmd on /dev/nvidia-modeset (legitimate)",
	    NV, NVKVM_DEV_MODESET, NVKVM_NVKMS_IOCTL_CMD, true);
	chk("the NVKMS wrapper cmd on nvidiactl",
	    NV, NVKVM_DEV_CTL, NVKVM_NVKMS_IOCTL_CMD, false);
	chk("the NVKMS wrapper cmd on renderD128",
	    NV, NVKVM_DEV_DRM_RD(0), NVKVM_NVKMS_IOCTL_CMD, false);
	chk("'m' nr 1 (not the wrapper cmd) on /dev/nvidia-modeset",
	    NV, NVKVM_DEV_MODESET, iowr('m', 1, 16), false);
	chk("'F' 0x2a on /dev/nvidia-modeset",
	    NV, NVKVM_DEV_MODESET, iowr('F', 0x2a, 32), false);
	chk("'d' 0x41 on /dev/nvidia-modeset",
	    NV, NVKVM_DEV_MODESET, iowr('d', 0x41, 40), false);

	/* ── 4. The device classes that speak NO ioctl ──────────────────────
	 * NVKVM_DEV_EVENTFD is the decision R-1.3 asked for: eventfd_fops has
	 * no .unlocked_ioctl, so every ioctl on one is -ENOTTY, and no guest
	 * path opens a handle with this dev_id.  NVKVM_DEV_UVM is answered and
	 * returned before this gate; one that reaches it has fd < 0 and is
	 * broken.  A TYPE_MEMORY handle carries dev_id 0 by zero-init, which is
	 * numerically NVKVM_DEV_CTL — the check must NOT read that as
	 * /dev/nvidiactl.
	 */
	printf("\ndevices that speak no ioctl\n");
	chk("'F' 0x2a on an eventfd handle", NV, NVKVM_DEV_EVENTFD,
	    iowr('F', 0x2a, 32), false);
	chk("'d' 0x41 on an eventfd handle", NV, NVKVM_DEV_EVENTFD,
	    iowr('d', 0x41, 40), false);
	chk("'F' 0x2a on a UVM handle", NV, NVKVM_DEV_UVM,
	    iowr('F', 0x2a, 32), false);
	chk("UVM_INITIALIZE (raw cmd, type 0) on a UVM handle", NV,
	    NVKVM_DEV_UVM, 0x30000001u, false);
	chk("'F' 0x2a on a TYPE_MEMORY handle (memfd, dev_id 0)", MEM,
	    NVKVM_DEV_CTL, iowr('F', 0x2a, 32), false);
	chk("'d' 0x41 on a TYPE_MEMORY handle", MEM, NVKVM_DEV_CTL,
	    iowr('d', 0x41, 40), false);

	/* ── 5. The legitimate traffic must still pass ──────────────────────
	 * A gate that only satisfies the negative half has broken the product.
	 * Every frontend node speaks 'F'; every render node speaks 'd'.
	 */
	printf("\nlegitimate traffic\n");
	chk("'F' 0x2a (RM_CONTROL) on nvidiactl", NV, NVKVM_DEV_CTL,
	    iowr('F', 0x2a, 32), true);
	chk("'F' 0x2b (RM_ALLOC) on nvidiactl", NV, NVKVM_DEV_CTL,
	    iowr('F', 0x2b, 48), true);
	chk("'F' 0x41 (RM_IDLE_CHANNELS, its real home) on nvidiactl",
	    NV, NVKVM_DEV_CTL, iowr('F', 0x41, 40), true);
	chk("'F' 0x27 (RM_ALLOC_MEMORY) on /dev/nvidia0", NV,
	    NVKVM_DEV_GPU(0), iowr('F', 0x27, 48), true);
	chk("'d' 0x41 (GEM_IMPORT_NVKMS_MEMORY, its real home) on renderD128",
	    NV, NVKVM_DEV_DRM_RD(0), iowr('d', 0x41, 24), true);
	chk("'d' 0x00 (DRM_IOCTL_VERSION) on renderD128", NV,
	    NVKVM_DEV_DRM_RD(0), iowr('d', 0x00, 40), true);

	{
		bool all = true;
		for (int n = 0; n <= NV_MINOR_DEVICE_NUMBER_REGULAR_MAX; n++)
			if (!nvkvm_ioctl_type_matches_dev(NV, NVKVM_DEV_GPU(n),
							  iowr('F', 0x2a, 32)))
				all = false;
		chk_all("every /dev/nvidiaN (0..15) accepts 'F'", all);

		all = true;
		for (int n = 0; n < 16; n++)
			if (!nvkvm_ioctl_type_matches_dev(NV,
							  NVKVM_DEV_DRM_RD(n),
							  iowr('d', 0x43, 24)))
				all = false;
		chk_all("every renderD128+n (0..15) accepts 'd'", all);
	}

	/* ── 6. The property, exhaustively ──────────────────────────────────
	 * Not a spot check: sweep every dev_id a handle can hold against every
	 * _IOC_TYPE, and require that each device accepts AT MOST ONE type.
	 * This is what makes the suite an assertion about the property rather
	 * than about the cases somebody thought of.
	 */
	printf("\nexhaustive sweep\n");
	{
		bool at_most_one = true;
		bool mem_never   = true;
		int  worst_dev = -1, worst_n = 0;

		for (int dev = -1; dev < 300; dev++) {
			int n = 0;
			for (unsigned t = 0; t < 256; t++) {
				if (nvkvm_ioctl_type_matches_dev(NV, dev,
								 iowr(t, 0x2a, 32)))
					n++;
				if (nvkvm_ioctl_type_matches_dev(MEM, dev,
								 iowr(t, 0x2a, 32)))
					mem_never = false;
			}
			/* MODESET matches on the full command word, so the
			 * per-type probe above finds nothing there; count it
			 * separately. */
			if (nvkvm_ioctl_type_matches_dev(NV, dev,
							 NVKVM_NVKMS_IOCTL_CMD))
				n++;
			if (n > 1) {
				at_most_one = false;
				if (n > worst_n) { worst_n = n; worst_dev = dev; }
			}
		}
		if (!at_most_one)
			printf("       dev_id=%d accepted %d distinct types\n",
			       worst_dev, worst_n);
		chk_all("every dev_id accepts at most one command shape",
			at_most_one);
		chk_all("no TYPE_MEMORY handle accepts any ioctl at all",
			mem_never);
	}

	/*
	 * The whole DRM allowlist, cross-typed.  Re-derived from
	 * nvkvm_drm_nr_allowed() itself rather than from a list copied here, so
	 * widening the allowlist can never quietly widen the bypass: every NR
	 * the DRM branch admits must still be refused when it is aimed at a
	 * frontend handle.  That is the exploit's entry condition, closed for
	 * the whole admitted set and not just for 0x41.
	 */
	{
		bool all_denied = true;
		unsigned admitted = 0;

		for (unsigned nr = 0; nr < 256; nr++) {
			if (!nvkvm_drm_nr_allowed(nr))
				continue;
			admitted++;
			if (nvkvm_ioctl_type_matches_dev(NV, NVKVM_DEV_CTL,
							 iowr('d', nr, 40)) ||
			    nvkvm_ioctl_type_matches_dev(NV, NVKVM_DEV_GPU(0),
							 iowr('d', nr, 40)))
				all_denied = false;
		}
		printf("       (%u NRs admitted by the DRM allowlist)\n", admitted);
		chk_all("no DRM-allowlisted NR is reachable on a frontend handle",
			all_denied && admitted > 0);
	}

	/* ── 7. The stub's independent copy must agree ──────────────────────
	 * The stub re-derives job_type from the same guest command word and is
	 * the process that issues the syscall.  It also serves the RING path,
	 * which never passes through nvkvm_req_ioctl_on_isolate at all — so its
	 * copy is not redundant, and the two must not drift apart.  The stub is
	 * told a dev_id but never a handle TYPE, so it is compared against the
	 * QEMU decision for TYPE_NVIDIA only; that gap is documented at the
	 * function in nvkvm_stub.c.
	 */
	printf("\nstub half agrees with the QEMU half\n");
	{
		bool agree = true;
		int  bad_dev = -1;
		unsigned bad_type = 0;

		for (int dev = 0; dev < 300; dev++) {
			stub_dev_under_test = (uint32_t)dev;
			for (unsigned t = 0; t < 256; t++) {
				uint32_t cmd = iowr(t, 0x2a, 32);
				bool q = nvkvm_ioctl_type_matches_dev(NV, dev, cmd);
				bool s = handle_type_matches_dev(0, cmd) != 0;
				if (q != s) {
					agree = false;
					if (bad_dev < 0) {
						bad_dev  = dev;
						bad_type = t;
					}
				}
			}
			bool q = nvkvm_ioctl_type_matches_dev(NV, dev,
							      NVKVM_NVKMS_IOCTL_CMD);
			bool s = handle_type_matches_dev(0, NVKVM_NVKMS_IOCTL_CMD) != 0;
			if (q != s) {
				agree = false;
				if (bad_dev < 0) { bad_dev = dev; bad_type = 'm'; }
			}
		}
		if (!agree)
			printf("       first disagreement at dev_id=%d type=0x%02x\n",
			       bad_dev, bad_type);
		chk_all("stub and QEMU agree for every (dev_id, type)", agree);

		/* And the stub's fail-closed default: a handle whose device was
		 * never recorded (a slot never stored, or a short RECEIVE_FD)
		 * matches nothing. */
		stub_dev_under_test = NVKVM_DEV_UNKNOWN;
		bool none = true;
		for (unsigned t = 0; t < 256; t++)
			if (handle_type_matches_dev(0, iowr(t, 0x2a, 32)))
				none = false;
		if (handle_type_matches_dev(0, NVKVM_NVKMS_IOCTL_CMD))
			none = false;
		chk_all("stub denies every type on an unrecorded dev_id", none);
	}

	printf("\n%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
