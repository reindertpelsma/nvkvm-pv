/*
 * nvkvm_ctrl_allowlist.h — default-deny RM control-command allowlist (#76).
 *
 * Generated, not hand-written. Provenance:
 *   - gVisor nvproxy 575-ABI compUtil-tagged control cmds (the CUDA-compute
 *     surface; graphics/video/profiling/fabric-only rows EXCLUDED) —
 *     docs/audits/nvproxy_control_allowlist.md
 *   - UNION our empirically-observed known-good set (cuInit/matmul/vec_add/
 *     big_memcpy/ioctl_fwd/nvidia-smi) — docs/audits/empirical_control_cmds.md
 *   - PLUS NV0080_CTRL_CMD_GPU_GET_VGX_CAPS (0x0080028e) and _GET_BRAND_CAPS
 *     (0x00800294): benign read-only caps queries libcuda issues at init that
 *     nvproxy's app never exercised.
 *
 * Two RULE-BASED passthroughs are handled in code, NOT this table (they cover
 * future cmds we haven't observed): GSP-legacy mask (cmd & 0x8000) and the
 * NV2081_BINAPI class ((cmd >> 16) == 0x2081) — both GSP-routed, no app
 * pointers. A 1 MiB inner-params size cap is also enforced in code.
 *
 * Anything not matched here or by those rules is DENIED (NV_ERR_NOT_SUPPORTED),
 * matching nvproxy's posture. This is a HOST/cross-VM attack-surface control
 * (reg-ops/HWPM/debug/fabric/power fall out automatically); it lives in QEMU
 * because the guest kernel module is untrusted.
 */
#ifndef NVKVM_CTRL_ALLOWLIST_H
#define NVKVM_CTRL_ALLOWLIST_H
#include <stdint.h>

static const uint32_t nvkvm_ctrl_allowlist[] = {
	0x00000101u,
	0x00000102u,
	0x00000127u,
	0x0000012bu,
	0x00000136u,
	0x0000013au,
	0x000001f0u,
	0x00000201u,
	0x00000202u,
	0x00000204u,
	0x00000205u,
	0x00000214u,
	0x00000215u,
	0x00000216u,
	0x0000021bu,
	0x00000275u,
	0x00000279u,
	0x0000027bu,
	0x00000288u,
	0x00000289u,
	0x00000290u,
	0x00000a04u,
	0x00000d01u,
	0x00000d04u,
	0x00410110u,
	0x00800201u,	/* NV0080_CTRL_CMD_GPU_GET_CLASSLIST — read-only class
			 * enumeration (NvU32List, marshalled by the info-list path).
			 * Enables NVENC engine discovery (libnvidia-encode); nvproxy
			 * maps it ctrlGetNvU32List/compUtil. Benign OUT-only list, no
			 * host/cross-VM escape. */
	0x00800280u,
	0x00800288u,
	0x00800289u,
	0x0080028bu,
	0x0080028eu,
	0x00800292u,
	0x00800294u,
	0x00801109u,	/* NV0080_CTRL_CMD_GR_GET_CAPS_V2 — inline caps (rmControlSimple);
			 * NVENC engine-caps query. CapGraphics|CapVideo in nvproxy. */
	0x00801307u,
	0x00801402u,
	0x0080170du,
	0x00801713u,	/* NV0080_CTRL_CMD_FIFO_GET_CAPS_V2 — inline caps (rmControlSimple);
			 * NVENC engine-caps query. CapVideo in nvproxy. */
	0x00801806u,
	0x0080180du,
	0x00801909u,
	0x00de0001u,
	0x00f80103u,
	0x00fd0101u,
	0x00fd0102u,
	0x00fd0104u,
	0x00fd0105u,
	0x20800102u,
	0x20800110u,
	0x20800111u,
	0x20800119u,
	0x2080012fu,
	0x20800131u,
	0x20800133u,
	0x2080013fu,
	0x20800142u,
	0x20800145u,
	0x20800146u,
	0x2080014au,
	0x2080014bu,
	0x20800156u,
	0x20800157u,
	0x2080016cu,	/* NV2080_CTRL_CMD_GPU_GET_ENCODER_CAPACITY — inline (rmControlSimple);
			 * NVENC InitializeEncoder query. CapVideo in nvproxy. */
	0x20800170u,
	0x2080018bu,
	0x2080018du,
	0x2080018eu,
	0x20800195u,
	0x208001a3u,
	0x20800301u,
	0x20800403u,
	0x20800406u,
	0x20800407u,
	0x20800513u,
	0x20800802u,
	0x2080110bu,
	0x20801201u,
	0x20801210u,
	0x20801218u,
	0x2080121bu,
	0x20801227u,
	0x2080122au,
	0x2080122bu,
	0x20801230u,
	0x20801303u,
	0x20801357u,
	0x20801358u,
	0x20801701u,
	0x20801702u,
	0x20801801u,
	0x20801802u,
	0x20801803u,
	0x20801823u,
	0x2080182au,
	0x2080182bu,
	0x2080200au,
	0x20802068u,
	0x20802209u,
	0x2080220cu,
	0x20802210u,
	0x20802a03u,
	0x20802a0au,
	0x20803001u,
	0x20803002u,
	0x20803125u,
	0x20803601u,
	0x20803801u,
	0x20808159u,
	0x20808162u,
	0x2080852eu,
	0x2080852fu,
	0x2080a612u,
	0x2080a618u,
	0x20810108u,
	0x208f1105u,
	0x503c0102u,
	0x503c0104u,
	0x503c0105u,
	0x83de0309u,
	0x83de030cu,
	0x83de0310u,
	0x906f0101u,
	0x906f0102u,
	0x90e60102u,
	0xa06c0101u,
	0xa06c0103u,
	0xa06c0105u,
	0xa06f0103u,
	0xc36f0108u,
	0xc56f010bu,
	0xcb330101u,
	0xcb330104u,
	0xcb33010bu,
	0xcb33010cu,
	/* #84 graphics (Vulkan/EGL) control surface — additional RM control
	 * commands libGLX_nvidia issues during device enumeration that the
	 * compute-only set above lacked.  Empirically captured from the host's
	 * own vulkaninfo RM ioctl stream (tools/nvtrace.c).  Still default-deny
	 * host/cross-VM: these are GR/subdevice/device query+config controls,
	 * no reg-ops/HWPM/debug/fabric. */
	0x00000301u,
	0x00003d05u,
	/*
	 * #110 dma-buf import: NV0000_CTRL_CMD_OS_UNIX_IMPORT_OBJECT_FROM_FD —
	 * the counterpart of EXPORT_OBJECT_TO_FD (0x3d05, already allowed).
	 * NVIDIA's EGL re-imports a render/scanout bo's exported memory object
	 * into its own RM client (via an nv-export fd) when a compositor capture
	 * or PRIME re-import happens.  The embedded fd is guest→handle_id
	 * translated (guest) and handle_id→stub-local-fd (stub); no guest VA or
	 * host fd ever crosses the boundary.  Same resource class as the export
	 * it pairs with — RmImportObject dups an existing memory object the stub
	 * already owns into the caller's client (intra-stub, accounted to the
	 * stub).  No reg-ops/HWPM/display. */
	0x00003d06u,
	0x00730101u,
	0x00801102u, /* NV0080 device controls */
	0x00801104u,
	0x00801301u,
	0x00801401u, /* NV0080_CTRL_CMD_HOST_GET_CAPS (#84 graphics) */
	0x00801701u,
	0x00801707u,
	0x00801b01u,
	0x00801c02u,
	0x00da0002u,
	0x00da0006u,
	0x20800123u, /* NV2080 subdevice controls */
	0x20800147u,
	0x20801206u,
	0x20801208u,
	0x20801301u,
	0x20801315u,
	0x20801320u,
	0x20801352u,
	0x20802a02u,
	0x20803d07u,
	0x2080a0d1u,
	0x20810107u,
	0x90960101u, /* NV9096 GR/zbc controls */
	0x90960106u,
	0x90960107u,
	0xa06f0104u, /* channel (GPFIFO) controls */
	0xc36f010au,
	/* #81 / 535 bring-up 2026-08-17: NVC36F_CTRL_GET_CLASS_ENGINEID.
	 * libcuda on driver 535.309.01 issues this during cuCtxCreate; libcuda
	 * on 575.51.03 does not, so it was missing from a table generated
	 * against the 575 ABI plus empirically-observed 575 traffic. Without it
	 * the ctrl was denied EACCES and cuCtxCreate failed
	 * CUDA_ERROR_OPERATING_SYSTEM (304):
	 *     nvkvm: DENY ctrl cmd 0xc36f0101 (not in allowlist / oversize)
	 *
	 * Safe by the same standard as the rest of the table, checked against
	 * open-gpu-kernel-modules 535.309.01 rather than assumed:
	 *   - ctrlc36f.h: NVC36F_CTRL_GET_CLASS_ENGINEID (0xc36f0101), a
	 *     read-only "which engine does this class run on" query.
	 *   - its params are NV906F_CTRL_GET_CLASS_ENGINEID_PARAMS: 16 bytes,
	 *     { NvHandle hObject; NvU32 classEngineID, classID, engineID; } —
	 *     one RM object handle in, three u32 out, NO embedded pointers and
	 *     no host resource named.
	 *   - the identical command is ALREADY allowed under its older class
	 *     id 0x906f0101 (NV906F_CTRL_GET_CLASS_ENGINEID, two rows above),
	 *     and two siblings on this same GPFIFO interface (0xc36f0108,
	 *     0xc36f010a) are already allowed. This grants no new capability.
	 * No reg-ops/HWPM/debug/fabric surface is opened. */
	0xc36f0101u,
};
#define NVKVM_CTRL_ALLOWLIST_N \
	(sizeof(nvkvm_ctrl_allowlist) / sizeof(nvkvm_ctrl_allowlist[0]))

#endif /* NVKVM_CTRL_ALLOWLIST_H */
