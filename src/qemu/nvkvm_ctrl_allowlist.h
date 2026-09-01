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
 * EXCLUSIONS — rows nvproxy's compUtil set HAS that we deliberately drop.
 * These must not come back on the next regeneration:
 *   - 0x20800513 NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2.  nvproxy adds it
 *     at 575.51.02 as ctrlHandler(rmControlSimple, compUtil), so a straight
 *     regeneration WILL re-emit it.  Do not.  Its host handler
 *     (subdeviceCtrlCmdThermalSystemExecuteV2_IMPL,
 *     src/nvidia/src/kernel/gpu/subdevice/subdevice_ctrl_gpu_kernel.c) opens
 *     with an unbounded write loop over a fixed 0x20-entry array:
 *         for (NvU32 i = 0; i < pParams->instructionListSize; i++)
 *                 pParams->instructionList[i].executed = NV_FALSE;
 *     instructionListSize is a caller-supplied NvU32 that nothing validates,
 *     the loop runs BEFORE the clientAPIVersion check that would otherwise
 *     bounce a malformed request to physical RM, and the export table entry
 *     is flags=0x8 (RMCTRL_FLAGS_NON_PRIVILEGED) accessRight=0x0 — i.e. any
 *     client can reach it.  A guest sets instructionListSize = 0xffffffff and
 *     walks off the end of a 1432-byte kernel allocation.  We have no
 *     per-command parameter validation (see above: only the 1 MiB aux cap), so
 *     nothing here would catch it, and thermal readback is telemetry that no
 *     compute path depends on.  If a workload turns out to need this, the fix
 *     is a bounded per-command check (reject instructionListSize > 0x20) at
 *     the gate in nvkvm_isolate_handlers.c — NOT restoring the bare row.
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
	0x20800802u,
	/*
	 * #gamescope 2026-08-28: NV2080_CTRL_CMD_FIFO_GET_INFO.  SteamOS's
	 * gamescope session issues this while configuring interleaved runlist
	 * scheduling; it was the one command in that sequence that is genuinely
	 * unprivileged, so it is the one that is genuinely forwarded.
	 *
	 * UNPRIVILEGED, checked against ALL 216 supported OGKM tags (515.43.04
	 * through 610.57.04), not just the branch in front of us:
	 *   - its NVOC export entry carries accessRight = 0x0 on every one of the
	 *     216 tags -- i.e. an EMPTY rightsRequired mask, so rsAccessCheckRights()
	 *     returns NV_OK at its first line and no capability is consulted
	 *     (src/nvidia/src/libraries/resserv/src/rs_access_map.c:193-194).
	 *     Contrast the four scheduling commands gamescope issues alongside it,
	 *     which are accessRight = 0x2 (RS_ACCESS_NICE) on all 216 tags and are
	 *     therefore NOT here -- see the no-op table below.
	 *   - it carries RMCTRL_FLAGS_NON_PRIVILEGED and never
	 *     RMCTRL_FLAGS_PRIVILEGED on any tag.
	 *   - read-only query: fills in a caller-supplied index/data table.
	 *
	 * NO EMBEDDED POINTER -- this is why it needs no marshalling and must NOT
	 * be added to nvkvm_ctrl_list_entry_size().  Despite the "GET_INFO" name it
	 * is NOT the { u32 count@0; pad; NvP64 ptr@8 } shape that GR_GET_INFO /
	 * FB_GET_INFO / BUS_GET_INFO use.  FINN inlines the table:
	 *     { NvU32 fifoInfoTblSize;
	 *       NV2080_CTRL_FIFO_INFO fifoInfoTbl[256];   <- INLINE, not NvP64
	 *       NvU32 engineType; }
	 * = 4 + 256*8 + 4 = 2056 bytes, flat, no guest VA anywhere in it, and far
	 * under the 1 MiB aux cap.  That struct body is BYTE-IDENTICAL on all 216
	 * tags (swept 2026-08-28; MAX_ENTRIES is 256 on every one), so there is no
	 * branch where a guest VA appears here and no version-variant layout to
	 * profile.  Evidence: tests/abi_parity/ogkm_gamescope_ctrl_audit.tsv,
	 * reasoning in docs/reference/gamescope-scheduling-controls.md.
	 */
	0x20801109u,
	0x2080110bu,	/* NV2080_CTRL_CMD_FIFO_DISABLE_CHANNELS.  Carries an NvP64
			 * pRunlistPreemptEvent at params offset 16 that nvkvm
			 * forwards VERBATIM -- no rewrite reaches inside the aux
			 * blob for this cmd.  Safe anyway: the handler's first
			 * statement rejects the whole control with
			 * NV_ERR_INSUFFICIENT_PERMISSIONS when the field is
			 * non-NULL and privLevel < RS_PRIV_LEVEL_KERNEL, which no
			 * ioctl client can ever be (escape.c:375 caps it at
			 * USER_ROOT).  Verified 515 through 610.  See U-13 in
			 * docs/internal/audit-guest-pointers.md -- including why a
			 * defensive clear here would be a downgrade.  NOTE: its
			 * hClientList[64] at offset 24 is a separate, still-open
			 * question (the cross-VM hClient blind spot). */
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
	/*
	 * NV0000_CTRL_CMD_OS_UNIX_GET_EXPORT_OBJECT_INFO — the query half of the
	 * same export/import family, sitting between 0x3d05 and 0x3d06.  It
	 * takes the nv-export fd at inner offset 0 (translated guest→handle_id
	 * →stub-local fd exactly like 0x3d06) and returns the deviceInstance /
	 * maxObjects the exported objects are parented by.
	 *
	 * JUSTIFICATION (measured, not assumed): with tools/nv_ioctl_trace.c on
	 * the BARE-METAL host, cuMemImportFromShareableHandle issues 0x3d08
	 * BEFORE 0x3d06 on every import — host trace is 0x3d05 (exporter) then
	 * 0x3d08, 0x3d06 (importer).  In the guest this control was the first
	 * thing denied, six times per import, and libcuda then failed the import
	 * with CUDA_ERROR_INVALID_DEVICE without ever issuing 0x3d06.  It is a
	 * read-only query about an fd the caller already possesses; it names no
	 * object the caller does not already hold and carries no reg-ops/HWPM/
	 * display surface.
	 *
	 * NOTE for anyone re-treading this: allowing 0x3d08 *alone* does NOT fix
	 * the NCCL SHM bug (that was tried and recorded in tests/BOOT_MATRIX.md).
	 * It is necessary but not sufficient — the fd it names still has to be
	 * relayed into the importing isolate, which is the other half of the fix
	 * (nvkvm_xrm_materialise in nvkvm_isolate_handlers.c).
	 */
	0x00003d08u,
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
	/* GET_CLASS_ENGINEID on the KEPLER-era channel classes.
	 *
	 * MEASURED 2026-09-01 on a GTX 750 Ti (GM107), driver 535.309.01:
	 *     nvkvm: DENY ctrl cmd 0xa16f0101 (not in allowlist / oversize)
	 * and validate.sh reported 22P/6F/7S with cuda_ctx_create FAILING and
	 * cuda_htod_dtoh_8mib / cuda_memset_d8 / cuda_ptx_jit all SKIPPED behind
	 * it. Whole bring-up passes; the context will not create.
	 *
	 * This is the SAME BUG as #81, one generation further back, and the note
	 * at 0xc36f0101 below already describes it: libcuda on 535.309.01 issues
	 * GET_CLASS_ENGINEID during cuCtxCreate (libcuda on 575 does not), so a
	 * table generated against the 575 ABI never saw it. There it was the Volta
	 * channel class; a GM107 uses the Kepler channel classes instead, so it
	 * needs the identical command under 0xa06f/0xa16f.
	 *
	 * Safe by the same standard, checked and not assumed: the id is FINN-derived
	 * as (GPFIFO_INTERFACE_ID << 8) | MESSAGE_ID, so every *_CHANNEL_GPFIFO_*
	 * class spells this command 0101 and they share one params struct,
	 * NV906F_CTRL_GET_CLASS_ENGINEID_PARAMS (ctrl906f.h): 16 bytes,
	 * { NvHandle hObject; NvU32 classEngineID, classID, engineID; } -- one RM
	 * object handle in, three u32 out, NO embedded pointers, no host resource
	 * named. The identical command is ALREADY allowed under 0x906f0101 and
	 * 0xc36f0101, which is the precedent this row follows exactly. */
	0xa06f0101u,  /* KEPLER_CHANNEL_GPFIFO_A GET_CLASS_ENGINEID */
	0xa16f0101u,  /* KEPLER_CHANNEL_GPFIFO_B GET_CLASS_ENGINEID -- the id MEASURED denied */
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

/*
 * The gate itself, next to the table it reads, because TWO host-side
 * components apply it: QEMU on the virtqueue path, and the isolate stub on the
 * command-buffer ring (U-1).  It lived only in QEMU until 2026-08-19, which is
 * exactly why the ring path had no gate at all.  One definition, one place to
 * change, no copy to drift.
 *
 * Returns non-zero if the command may be forwarded.  `int` and not `bool`: the
 * stub is freestanding and does not pull in <stdbool.h>.
 *
 * ── Audit 2026-08-29 (serious): the two rule-based passthroughs ──────────
 *
 * `cmd & 0x8000` sits AHEAD of a table whose every one of its 167 rows is
 * justified individually, and admits 2^31 command numbers on a bit that is a
 * routing hint (GSP legacy), not a safety property.  The class wildcard admits
 * all of 0x2081.  The header at the top of this file asserts both are
 * "GSP-routed, no app pointers"; docs/internal/audit-guest-pointers.md already
 * records that this assertion is not verifiable from the open tree and was not
 * verified.  So the file's own documentation says default-deny and the gate
 * does something else for half the command space.
 *
 * NOT narrowed here, deliberately.  Narrowing needs evidence about which
 * commands real libcuda/nvidia-smi traffic actually relies on these two rules
 * for, and getting that wrong denies a control the guest needs and surfaces as
 * a CUDA_ERROR_OPERATING_SYSTEM with no obvious cause -- which is exactly how
 * 0xc36f0101 was found.  That evidence needs a GPU, and there is none here.
 *
 * What IS done: say WHICH rule admitted a command, so a single run enumerates
 * the set the wildcards are load-bearing for and the narrowing stops being a
 * guess.  Verdicts are unchanged -- every caller tests `!allowed`, and all
 * three non-zero returns are equally non-zero.
 */
#define NVKVM_CTRL_ALLOW_TABLE     1  /* an explicit, justified table row     */
#define NVKVM_CTRL_ALLOW_GSS       2  /* the cmd & 0x8000 wildcard            */
#define NVKVM_CTRL_ALLOW_BINAPI    3  /* the 0x2081-class wildcard            */

static inline int nvkvm_ctrl_cmd_allowed(uint32_t cmd)
{
	/*
	 * Table first, wildcards second.  The verdict is identical either way
	 * (a row that also matches a wildcard was allowed before and is allowed
	 * now), but the REPORTED reason is not: with the wildcards first, the
	 * four rows that also carry bit 15 -- 0x20808159, 0x20808162,
	 * 0x2080852e, 0x2080852f -- would be logged as wildcard-only and make
	 * the wildcard look more load-bearing than it is.
	 */
	for (unsigned i = 0; i < NVKVM_CTRL_ALLOWLIST_N; i++)
		if (nvkvm_ctrl_allowlist[i] == cmd)
			return NVKVM_CTRL_ALLOW_TABLE;
	if (cmd & 0x8000u)                       /* RM_GSS_LEGACY_MASK */
		return NVKVM_CTRL_ALLOW_GSS;
	if (((cmd >> 16) & 0xffffu) == 0x2081u)  /* NV2081_BINAPI class */
		return NVKVM_CTRL_ALLOW_BINAPI;
	return 0;
}

/*
 * ── Answered locally, NEVER forwarded: the gamescope scheduling controls ────
 *
 * These are NOT in the allowlist above and must never be added to it.  The
 * allowlist means exactly one thing -- "this command may be forwarded to the
 * host driver" -- and these four must not be.  They are a separate, smaller
 * idea: commands QEMU answers itself with NV_OK, having done nothing.
 *
 * WHY THEY EXIST.  SteamOS's gamescope session configures interleaved /
 * realtime runlist scheduling for low-latency compositing.  It gets Vulkan,
 * selects `connector Virtual-1` and `1920x1080@60Hz`, then issues the sequence
 * below.  Denied, it retries forever until systemd's start timeout kills and
 * restarts it -- the session never comes up.  This is a PERFORMANCE
 * configuration path, not error recovery.
 *
 * WHY NOT JUST ALLOW THEM.  All four are privileged in OGKM, on every one of
 * the 216 supported tags.  The chain, which is identical at 515.43.04 and at
 * 610.57.04:
 *
 *   1. Each one's NVOC export entry carries accessRight = 0x2.  That field is
 *      not an index -- serverControl_InitCookie() memcpys it straight into
 *      rightsRequired.limbs[0] (src/nvidia/src/kernel/rmapi/resource.c:173-175),
 *      so it is a ONE-LIMB ACCESS MASK.  Bit 1 is RS_ACCESS_NICE
 *      (rs_access.h:60, == 1 on all 216 tags).
 *   2. rmControlCmdExecute() calls rsAccessCheckRights() with that mask
 *      (src/nvidia/src/kernel/rmapi/control.c:748-751); insufficient rights
 *      return NV_ERR_INSUFFICIENT_PERMISSIONS
 *      (.../resserv/src/rs_access_map.c:203-206).
 *   3. RS_ACCESS_NICE's metadata is ALLOW_PRIVILEGED | UNCACHED_CHECK and
 *      notably NOT ALLOW_OWNER (.../resserv/src/rs_access_rights.c:46-49, all
 *      216 tags) -- owning the channel grants you nothing here, and
 *      UNCACHED_CHECK re-runs the check on every call so it cannot be cached
 *      into existence.
 *   4. It is granted only to privLevel >= RS_PRIV_LEVEL_USER_ROOT
 *      (.../resserv/src/rs_access_map.c:506-511) or by the resource access
 *      callback, which resolves to capable(CAP_SYS_NICE) on every tag --
 *      via os_allow_priority_override() on 515.43.04..560.35.03 and via
 *      os_check_access(RS_ACCESS_NICE) on 565.57.01..610.57.04
 *      (kernel-open/nvidia/os-interface.c).
 *
 * So the answer to "could an unprivileged user issue this on bare metal?" is
 * NO, on all 216 tags, with no sub-range where it is unprivileged.  NVIDIA
 * says so in prose too: SET_SCHED_POLICY is "a global setting, only privileged
 * clients will be allowed to set it. Regular clients will get
 * NV_ERR_INSUFFICIENT_PERMISSIONS" (ctrl2080fifo.h), RESTART_RUNLIST "may
 * cause certain low priority channels to starve ... it is only available to
 * privileged clients" (ctrla06fgpfifo.h), and SET_INTERLEAVE_LEVEL "For safety
 * reasons, setting this property requires PRIVILEGED user level" (ctrla06c.h).
 *
 * The intuition that lowering your own priority might be free the way nice(2)
 * is DOES NOT HOLD, and it is worth being explicit because it is the obvious
 * thing to hope for.  rightsRequired is a per-methodId constant read from the
 * export table and checked in control.c BEFORE the handler ever sees the
 * value, so setting INTERLEAVE_LEVEL_LOW is gated exactly as hard as setting
 * _HIGH.  There is no value-dependent path to carve out.
 *
 * Two of them are also cross-VM hazards, which is this gate's actual remit:
 * SET_SCHED_POLICY sets the scheduling policy for ALL runlists on the subdevice
 * and LOCKS it -- "that policy cannot be changed to a different one unless all
 * clients which set it have either restored the policy ... or died" -- so one
 * guest could pin the GPU-global scheduler against the host and every other VM.
 * MAKE_REALTIME promotes a TSG above every non-realtime channel on the runlist
 * and forces the others preemptible.
 *
 * WHY NO-OP RATHER THAN DENY.  Correctness does not depend on any of them.
 * They are scheduling PRIORITY and preemption LATENCY hints: the same work
 * runs, in the same order within a channel, to the same result -- it may just
 * start a timeslice later.  So we take the third option and lie.
 *
 * THE GUEST IS BEING LIED TO.  Say it plainly: we return NV_OK and do nothing.
 * gamescope believes it configured CHANNEL_INTERLEAVED scheduling, promoted its
 * compositing TSG to realtime and kicked a runlist restart; none of that
 * happened, and its channels keep DEFAULT policy and default interleave level.
 * THE COST IS COMPOSITING LATENCY, AND NOTHING ELSE -- frames may be scheduled
 * a timeslice later than gamescope intends under GPU contention.  No rendering
 * is wrong, no frame is dropped, no state is corrupted.
 *
 * The lie is also not observable through this interface: all four params are
 * INPUT-only (there is no output field to forge), FIFO_GET_INFO -- the one
 * query in the sequence we do forward -- has no index that reports scheduling
 * policy or interleave level (indices 0..0xa, ctrl2080fifo.h), and the two
 * readback commands that WOULD expose it, GET_INTERLEAVE_LEVEL 0xa06c0108 and
 * 0xa06f0110, are not in the allowlist and stay denied.
 *
 * Lying is also strictly SAFER here than forwarding would be.  In namespace/uid
 * isolation the stub has no capabilities and a non-root uid, so a forwarded
 * command would come back NV_ERR_INSUFFICIENT_PERMISSIONS anyway and gamescope
 * would still spin -- allowing buys nothing.  In the weaker `seccomp`/`none`
 * rungs (nvkvm_isolate_uid.h:107-116) the stub keeps QEMU's uid and caps, so
 * against a root QEMU a forwarded SET_SCHED_POLICY would SUCCEED and hand the
 * guest the GPU-global scheduler lock described above.  Not forwarding removes
 * that rung dependency entirely.
 *
 * NOTHING IS NEEDED IN THE STUB.  ring_ctrl_must_punt() (src/stub/nvkvm_stub.c)
 * punts any cmd for which nvkvm_ctrl_cmd_allowed() is false, and these are
 * false there by construction, so the ring bounces them to the virtqueue and
 * the single answer below is the only one.  PUNT means "not executed", so
 * there is no path on which one of these both no-ops and runs.
 *
 * Full verdict, per-tag evidence and the argument against this choice:
 * docs/reference/gamescope-scheduling-controls.md.
 */
static const uint32_t nvkvm_ctrl_noop[] = {
	0x20801115u,	/* NV2080_CTRL_CMD_FIFO_RUNLIST_SET_SCHED_POLICY
			 * { NvU32 flags; NvU32 schedPolicy; } -- IN only.
			 * accessRight=0x2 (NICE) on all 216 tags.  GPU-GLOBAL
			 * and lockable: the strongest deny of the four. */
	0xa06f0111u,	/* NVA06F_CTRL_CMD_RESTART_RUNLIST
			 * { NvBool bForceRestart; NvBool bBypassWait; } -- IN
			 * only.  accessRight=0x2 on all 216 tags.  Preempts the
			 * running channel; NVIDIA notes it can starve low
			 * priority channels.  No-op = no early preemption, the
			 * runlist turns over on normal timeslice expiry. */
	0xa06f0109u,	/* NVA06F_CTRL_CMD_SET_INTERLEAVE_LEVEL
			 * { NvU32 channelInterleaveLevel; } -- IN only.
			 * accessRight=0x2 on all 216 tags, for LOW as much as
			 * for HIGH (see above).  No-op = channel keeps its
			 * default interleave level. */
	0xa06c0110u,	/* NVA06C_CTRL_CMD_MAKE_REALTIME
			 * { NvBool bRealtime; } -- IN only.  accessRight=0x2 on
			 * all 216 tags.  No-op = TSG stays non-realtime. */
};
#define NVKVM_CTRL_NOOP_N \
	(sizeof(nvkvm_ctrl_noop) / sizeof(nvkvm_ctrl_noop[0]))

/*
 * Returns non-zero if the command must be answered NV_OK without ever being
 * forwarded.  Checked BEFORE nvkvm_ctrl_cmd_allowed(); the two sets are
 * disjoint and a build-time check in tests/unit/test_ctrl_gate.c keeps them so.
 */
static inline int nvkvm_ctrl_cmd_noop(uint32_t cmd)
{
	for (unsigned i = 0; i < NVKVM_CTRL_NOOP_N; i++)
		if (nvkvm_ctrl_noop[i] == cmd)
			return 1;
	return 0;
}

#endif /* NVKVM_CTRL_ALLOWLIST_H */
