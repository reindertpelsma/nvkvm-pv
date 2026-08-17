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
 * It is NOT named here because it cannot be: the DKMS tree ships only
 * nvidia-modeset/nvkms-ioctl.h (the wrapper), and the NvKmsIoctlCommand enum
 * that would name 60 is not in any shipped header.  Treat it exactly like
 * 61/62 — required in practice, unaudited in principle — and name it against
 * nvkms-api.h if that enum ever becomes available.
 */
#ifndef NVKVM_NVKMS_ALLOWLIST_H
#define NVKVM_NVKMS_ALLOWLIST_H

#include <stdbool.h>
#include <stdint.h>

static inline bool nvkvm_nvkms_cmd_allowed(uint32_t cmd_type)
{
	switch (cmd_type) {
	case 0:   /* NVKMS_IOCTL_ALLOC_DEVICE      */
	case 1:   /* NVKMS_IOCTL_FREE_DEVICE       */
	case 17:  /* NVKMS_IOCTL_REGISTER_SURFACE  */
	case 18:  /* NVKMS_IOCTL_UNREGISTER_SURFACE*/
	case 60:  /* per-surface, 595+ ICD; see header note   */
	case 61:  /* query-class (captured)        */
	case 62:  /* query-class (captured)        */
		return true;
	default:
		return false;
	}
}

#endif /* NVKVM_NVKMS_ALLOWLIST_H */
