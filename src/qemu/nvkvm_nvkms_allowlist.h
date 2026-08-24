/*
 * nvkvm_nvkms_allowlist.h — default-deny allowlist for the inner cmdType of the
 * NVKMS wrapper ioctl (/dev/nvidia-modeset).
 *
 * Audit 2026-05-31 G-1: the wrapper
 *   NVKMS_IOCTL_CMD = _IOWR('m',0,{u32 cmdType; u32 size; u64 address;})
 * can carry ANY NVKMS command to a host-GLOBAL, privileged display device —
 * including the cross-client permission/sharing verbs (GRANT/ACQUIRE/REVOKE_
 * PERMISSIONS, GRANT/ACQUIRE/RELEASE_SURFACE), swap-groups and framelock.  The
 * branch in nvkvm_isolate_handlers.c only gated the OUTER ioctl, never the
 * inner cmdType.  This is the trust boundary: a malicious guest crafts the
 * wrapper directly, so the allowlist must live here (QEMU), not in the guest.
 *
 * The allowed set was captured live from a real Vulkan/EGL session
 * (vulkaninfo enumerate + device create): cmdTypes 0,1,17,18,61,62 — and
 * NONE of the permission/sharing verbs appear.  Default-deny everything else.
 *
 * INTERIM: the real fix is to stop forwarding NVKMS entirely and emulate a
 * virtual head in the guest (docs/design/virtual_modeset.md); this allowlist
 * shrinks the reachable kernel surface until then.  cmdTypes 61/62 are the
 * two query-class commands the ICD issues during enumeration; identify them
 * precisely against nvkms-api.h before relying on this long-term.
 *
 * 2026-08-17 — cmdType 60 added.  A list captured on one driver branch is a
 * list that expires.  Branches 595+ issue a cmdType this 575-era capture never
 * saw, and denying it broke ALL offscreen GL in the guest: every colour
 * attachment came back GL_FRAMEBUFFER_UNSUPPORTED (0x8CDD) with glGetError()
 * clean, on 595.84 and 610.43.02, while the same probe passed on bare metal on
 * the same box, GPU and driver.  See tests/BOOT_MATRIX.md, "offscreen GL
 * rendering is broken on 595 and 610".
 *
 * What is actually known about 60, all of it measured on 610.43.02 (an
 * instrumented build logging every wrapper, allowed or denied):
 *   - the ICD issues it exactly once per offscreen context, and it carries a
 *     32-byte params block (cmdType=60 size=32);
 *   - it lands BETWEEN REGISTER_SURFACE and UNREGISTER_SURFACE, so it acts on
 *     a just-registered surface — the sequence is 0/1440, 17/152, 60/32, 18/16;
 *   - 61 and 62 are NOT issued at all by the 610 ICD, so 60 is not an addition
 *     to that pair so much as its replacement;
 *   - denying it returns -EACCES/NV_ERR_NOT_SUPPORTED and the ICD responds by
 *     unregistering the surface and declaring every format unrenderable.
 * 2026-08-21 — the cmdTypes ARE nameable, and now are.  The DKMS tree ships
 * only nvidia-modeset/nvkms-ioctl.h (the wrapper), which is why the numbers sat
 * unnamed here — but NVIDIA/open-gpu-kernel-modules carries the
 * NvKmsIoctlCommand enum at src/nvidia-modeset/interface/nvkms-api.h, tagged per
 * driver release, and it is a plain unvalued enum so position IS the wire value.
 * Read off the 575.51.03 tag:
 *
 *     60 = NVKMS_IOCTL_SET_FLIPLOCK_GROUP
 *     61 = NVKMS_IOCTL_ENABLE_VBLANK_SEM_CONTROL
 *     62 = NVKMS_IOCTL_DISABLE_VBLANK_SEM_CONTROL
 *
 * So 61/62 are not "query-class" as guessed above; they bracket a vblank
 * semaphore control around a registered surface, which is consistent with where
 * they land in the observed 0/17/60/18 (and 17/61/18/62) sequences.
 *
 * 2026-08-21, LATER, and it invalidates the paragraph above: "it grows by
 * APPENDING, so old values are stable" is FALSE, and the names just given are
 * the 575 names attached to a cmdType that was measured on 610.  The enum has
 * been edited mid-list at least twice.  515->575 INSERTS CHECK_LUT_NOTIFIER at
 * 13 (everything >= 13 shifts +1); 580->610 DELETES EXPORT_VRR_SEMAPHORE_SURFACE
 * at 56 and VRR_SIGNAL_SEMAPHORE at 64 (everything above shifts down).  So:
 *
 *   value  515.105.01           575/580                  610.43.02
 *     17   UNREGISTER_SURFACE   REGISTER_SURFACE         REGISTER_SURFACE
 *     18   GRANT_SURFACE (!)    UNREGISTER_SURFACE       UNREGISTER_SURFACE
 *     60   (out of range)       SET_FLIPLOCK_GROUP       ENABLE_VBLANK_SEM_CONTROL
 *     61   (out of range)       ENABLE_VBLANK_SEM_CTRL   DISABLE_VBLANK_SEM_CONTROL
 *     62   (out of range)       DISABLE_VBLANK_SEM_CTRL  ACCEL_VBLANK_SEM_CONTROLS
 *
 * Two things follow.  The cmdType 60 investigated above was measured on 610, so
 * it is ENABLE_VBLANK_SEM_CONTROL, not SET_FLIPLOCK_GROUP -- which fits the
 * observed 0/17/60/18 sequence far better and explains why 610 does not issue
 * 61/62.  And on a 515/520 host this list admits GRANT_SURFACE, one of the very
 * cross-client sharing verbs the top of this file says must not be reachable.
 *
 * This gate is therefore only trustworthy on 575/580, where it was captured.
 * The fix is not a renumbering -- any fixed table is wrong on some branch -- it
 * is the per-version ABI-profile plumbing the UVM gate already has
 * (struct nvkvm_abi_profile, src/common/nvkvm_abi.h), with the allowlist
 * expressed in NAMES resolved through the profile.  Written up, with the full
 * vendor-tag table and the shape of the fix, in
 * docs/internal/nvkms-allowlist-abi-drift.md.  Deliberately NOT changed here:
 * renumbering for 610 would break 575/580, and widening is never the answer.
 *
 * WHAT IS DELIBERATELY *NOT* HERE, and why (2026-08-21)
 * ----------------------------------------------------
 * cmdType 33 = NVKMS_IOCTL_DECLARE_EVENT_INTEREST is the first NVKMS command
 * the NVIDIA Xorg DDX issues, and denying it is exactly what stops the DDX:
 * it opens /dev/nvidia-modeset, gets -EACCES on this one call, closes the fd
 * and prints "Failed to select a display subsystem".  Adding 33 is therefore
 * tempting and is still WRONG, because it does not make the DDX work — it
 * moves the failure one rung down a ladder that ends somewhere we cannot go.
 * Measured on an RTX 3070 / 575.51.03 by widening the list one cmdType at a
 * time:
 *
 *     33 DECLARE_EVENT_INTEREST            -> then wants
 *      0 ALLOC_DEVICE (already allowed; succeeds) -> then wants
 *      2 QUERY_DISP                        -> then wants
 *      3 QUERY_CONNECTOR_STATIC_DATA x6      (one per HOST connector)
 *      5 QUERY_DPY_STATIC_DATA             -> ...
 *
 * i.e. the DDX is walking the *host's* physical display topology, because the
 * host's NVKMS is the only display subsystem behind this device.  That is the
 * architectural boundary, not an allowlist entry short of working.  A guest DDX
 * can only be satisfied by a virtual NVKMS that answers with nvkvm's own head,
 * never by forwarding further.  See docs/internal/mint-guest-desktop.md for the
 * full ladder and the log excerpts.
 */
#ifndef NVKVM_NVKMS_ALLOWLIST_H
#define NVKVM_NVKMS_ALLOWLIST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* The per-driver-version command numbering lives in src/common now:
 * the guest module and the stub need the same table, to recognise
 * REGISTER_SURFACE for plane-fd translation. */
#include "../../src/common/nvkvm_nvkms_ops.h"

/* Investigation escape hatch, OFF unless the env var is set, and never a
 * default: NVKVM_NVKMS_EXTRA_ALLOW="33,34" widens the list for one QEMU run.
 * It exists because the list above is a live capture that expires with every
 * driver branch (see the 2026-08-17 note), and bisecting which cmdType a new
 * client needs otherwise means a QEMU rebuild per guess.  Anything learned
 * this way belongs in the switch below, named, not left to the env var. */
static inline bool nvkvm_nvkms_extra_allow(uint32_t cmd_type)
{
	const char *s = getenv("NVKVM_NVKMS_EXTRA_ALLOW");
	if (!s)
		return false;
	while (*s) {
		char *end;
		unsigned long v = strtoul(s, &end, 0);
		if (end == s)
			break;
		if ((uint32_t)v == cmd_type)
			return true;
		s = (*end == ',') ? end + 1 : end;
	}
	return false;
}

/*
 * The allowlist is keyed on the DRIVER VERSION, because `enum NvKmsIoctlCommand`
 * is a plain unvalued enum -- position IS the wire value -- and NVIDIA edits it
 * in the MIDDLE, not only by appending.  A bare `case 18:` therefore names a
 * different command on different hosts.
 *
 * Measured from `enum NvKmsIoctlCommand` in
 * src/nvidia-modeset/interface/nvkms-api.h at each vendor tag (515.105.01,
 * 525.105.17, 535.104.05, 545.23.06, 550.54.14, 565.57.01, 570.86.16,
 * 575.51.03, 580.178.04, 590.48.01, 595.84, 610.43.02).  Three regimes:
 *
 *   driver             REGISTER  UNREGISTER  GRANT   vblank en/dis/accel  FLIPLOCK
 *   ------------------------------------------------------------------------
 *   515, 525, 535         16         17        18       (absent)          -
 *   545                   16         17        18       (absent)         59
 *   550, 555, 560, 565    16         17        18       60 / 61 / 62     59
 *   570  minor <  207     16         17        18       60 / 61 / 62     59
 *   570  minor >= 207     17         18        19       61 / 62 / 63     60
 *   575, 580              17         18        19       61 / 62 / 63     60
 *   590, 595, 610         17         18        19       60 / 61 / 62     59
 *
 * The renumbering lands INSIDE the 570 branch, between 570.195.03 and 570.207 --
 * not at 570->575 -- so this takes major AND minor.  Every other major measured
 * is internally consistent from its first tag to its last.
 * 570.207 INSERTED NVKMS_IOCTL_DECLARE_DYNAMIC_DPY_INTEREST at 16, pushing
 * everything above it up by one; 590 removed two members below 64, pulling the
 * tail back down.  The insertion falls inside a single NVKVM_ABI_570 profile
 * bucket AND inside a single major, so neither `nv->abi` nor the major alone can
 * express it -- that is why this takes major and minor.
 *
 * What the old hardcoded list {0,1,17,18,60,61,62} actually admitted:
 *
 *   - on 515..574 value 18 is GRANT_SURFACE -- one of the cross-client sharing
 *     verbs this gate exists to deny, and named as such in its own header
 *     comment -- while REGISTER_SURFACE (16) was NOT admitted, so the gate both
 *     leaked the dangerous command and blocked the intended one;
 *   - on 575..589 value 60 is SET_FLIPLOCK_GROUP, a host-global display
 *     operation reachable after the accepted ALLOC_DEVICE;
 *   - only on 590+ was it correct, which is where it was captured.
 *
 * SET_FLIPLOCK_GROUP and GRANT_SURFACE are never named here in any regime.
 *
 * An unrecognised or unparseable version admits ALLOC_DEVICE/FREE_DEVICE only
 * (indices 0 and 1, which have never moved) and denies the rest.  That is
 * deliberate: extrapolating the numbering onto an unverified branch is exactly
 * how SET_FLIPLOCK_GROUP got in.  Adding a branch is one row in
 * src/common/nvkvm_nvkms_ops.h, once its enum has actually been read.
 * NVKVM_NVKMS_EXTRA_ALLOW remains the one-run escape hatch for bringing a new
 * branch up.
 *
 * The table itself MOVED to src/common/nvkvm_nvkms_ops.h (unchanged), because
 * the guest module and the stub need the same numbering to recognise
 * REGISTER_SURFACE and translate the plane fds embedded in its inner params.
 */
static inline bool nvkvm_nvkms_cmd_allowed_ver(uint32_t cmd_type,
					       unsigned major, unsigned minor)
{
	struct nvkvm_nvkms_ops o;

	if (nvkvm_nvkms_extra_allow(cmd_type))
		return true;

	/* ALLOC_DEVICE / FREE_DEVICE sit at 0 and 1 in every tag measured. */
	if (cmd_type == 0 || cmd_type == 1)
		return true;

	o = nvkvm_nvkms_ops_for_version(major, minor);
	if (!o.known)
		return false;

	if (cmd_type == o.reg_surface || cmd_type == o.unreg_surface)
		return true;

	if (o.vblank_enable >= 0 &&
	    ((int32_t)cmd_type == o.vblank_enable ||
	     (int32_t)cmd_type == o.vblank_disable ||
	     (int32_t)cmd_type == o.vblank_accel))
		return true;

	return false;
}

#endif /* NVKVM_NVKMS_ALLOWLIST_H */
