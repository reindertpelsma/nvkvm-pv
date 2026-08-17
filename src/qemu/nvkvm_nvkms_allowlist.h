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
	case 61:  /* query-class (captured)        */
	case 62:  /* query-class (captured)        */
		return true;
	default:
		return false;
	}
}

#endif /* NVKVM_NVKMS_ALLOWLIST_H */
