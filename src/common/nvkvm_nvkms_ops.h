/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_nvkms_ops.h — where each NVKMS command sits in `enum NvKmsIoctlCommand`
 * on a given host driver.
 *
 * `enum NvKmsIoctlCommand` (open-gpu-kernel-modules,
 * src/nvidia-modeset/interface/nvkms-api.h) is a plain unvalued enum, so a
 * member's POSITION is its wire value — and NVIDIA edits the list in the
 * middle, not only by appending.  A bare constant therefore names a different
 * command on different hosts.
 *
 * This table used to live inside src/qemu/nvkvm_nvkms_allowlist.h, where it
 * was derived for the allowlist's benefit.  It is here now because the guest
 * module and the isolate stub need the same numbering for a second, unrelated
 * reason: REGISTER_SURFACE's inner params embed plane fds that must be
 * translated across the VM boundary, and the code that does the translating
 * has to recognise the command first.  One table, three consumers.
 *
 * Plain int/unsigned only, and no includes: this is compiled into the guest
 * kernel module, into QEMU, and into the freestanding stub.
 *
 * MEASURED from `enum NvKmsIoctlCommand` at the vendor tags below.  Four
 * regimes, and note that ONE OF THE BOUNDARIES FALLS INSIDE A BRANCH, which is
 * why this takes major AND minor:
 *
 *   driver                 REGISTER  UNREGISTER  GRANT  vblank en/dis/accel
 *   -------------------------------------------------------------------------
 *   515, 525, 535             16         17        18       (absent)
 *   545                       16         17        18       (absent)
 *   550, 555, 560, 565        16         17        18       60 / 61 / 62
 *   570  minor <  207         16         17        18       60 / 61 / 62
 *   570  minor >= 207         17         18        19       61 / 62 / 63
 *   575, 580                  17         18        19       61 / 62 / 63
 *   590, 595, 610             17         18        19       60 / 61 / 62
 *
 * 570.207 INSERTED NVKMS_IOCTL_DECLARE_DYNAMIC_DPY_INTEREST at 16, pushing
 * everything above it up by one; 590 removed two members below 64, pulling the
 * tail back down.  The insertion lands between 570.195.03 and 570.207 — inside
 * a single NVKVM_ABI_570 profile bucket AND inside a single major — so neither
 * `nv->abi` nor the major alone can express it.
 *
 * An unrecognised or unparseable version reports `known = 0` and names nothing.
 * Extrapolating the numbering onto an unverified branch is exactly how
 * SET_FLIPLOCK_GROUP once got into the allowlist; adding a branch is one row
 * here, after its enum has actually been read.
 */
#ifndef NVKVM_NVKMS_OPS_H
#define NVKVM_NVKMS_OPS_H

struct nvkvm_nvkms_ops {
	int      known;            /* 0 when the branch has not been measured  */
	unsigned reg_surface;
	unsigned unreg_surface;
	int      vblank_enable;    /* -1 when the branch has no vblank-sem ops */
	int      vblank_disable;
	int      vblank_accel;
};

static inline struct nvkvm_nvkms_ops
nvkvm_nvkms_ops_for_version(unsigned major, unsigned minor)
{
	struct nvkvm_nvkms_ops o = { 0, 0, 0, -1, -1, -1 };
	/* 570 splits mid-branch: 570.195.03 and older number like 565, 570.207
	 * and newer number like 575.  Everything else is stable per major. */
	int shifted = (major >= 575) || (major == 570 && minor >= 207) ||
		      (major > 570 && major < 575);

	if (major >= 515 && major <= 549) {
		o.known = 1; o.reg_surface = 16; o.unreg_surface = 17;
	} else if (major >= 550 && major <= 574 && !shifted) {
		o.known = 1; o.reg_surface = 16; o.unreg_surface = 17;
		o.vblank_enable = 60; o.vblank_disable = 61; o.vblank_accel = 62;
	} else if (shifted && major <= 589) {
		o.known = 1; o.reg_surface = 17; o.unreg_surface = 18;
		o.vblank_enable = 61; o.vblank_disable = 62; o.vblank_accel = 63;
	} else if (major >= 590 && major <= 610) {
		o.known = 1; o.reg_surface = 17; o.unreg_surface = 18;
		o.vblank_enable = 60; o.vblank_disable = 61; o.vblank_accel = 62;
	}
	return o;
}

/*
 * The two values REGISTER_SURFACE has ever taken.  The stub cannot call
 * nvkvm_nvkms_ops_for_version() — it is handed an `abi_profile`, and the
 * 570.195/570.207 split falls INSIDE NVKVM_ABI_570 — so it matches against
 * both, which is exactly as tight.  On any one host only one of the two can
 * reach the stub at all: QEMU's allowlist admits REGISTER_SURFACE and
 * UNREGISTER_SURFACE and nothing else in this range, so on a <=570.195 host
 * value 17 is UNREGISTER_SURFACE (no plane fds, useFd is not set) and on a
 * >=570.207 host value 16 is DECLARE_DYNAMIC_DPY_INTEREST, which is denied.
 */
#define NVKVM_NVKMS_REG_SURFACE_UNSHIFTED 16u   /* 515 .. 570.195.03 */
#define NVKVM_NVKMS_REG_SURFACE_SHIFTED   17u   /* 570.207 .. 610    */

#endif /* NVKVM_NVKMS_OPS_H */
