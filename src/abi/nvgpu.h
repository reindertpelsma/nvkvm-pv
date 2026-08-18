/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvgpu.h — Core NVIDIA GPU ABI types
 *
 * Ported from gVisor pkg/abi/nvgpu/nvgpu.go with additions from the
 * open-gpu-kernel-modules headers.
 *
 * These structs describe the kernel-driver ABI for /dev/nvidia*, /dev/nvidiactl
 * and /dev/nvidia-uvm. They must not be modified unless the corresponding
 * driver version support in nvkvm_dispatch.c is also updated.
 */

#ifndef NVGPU_H
#define NVGPU_H

#include <linux/types.h>

/* ── Device numbers ──────────────────────────────────────────────────────── */

#define NV_MAJOR_DEVICE_NUMBER                  195
#define NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE   255
#define NV_MINOR_DEVICE_NUMBER_MODESET          254
#define NV_MINOR_DEVICE_NUMBER_REGULAR_MAX      15
#define NVIDIA_UVM_PRIMARY_MINOR_NUMBER         0

/* ── Core handle types ───────────────────────────────────────────────────── */

typedef __u32 nvhandle_t;     /* NvHandle — opaque RM object handle  */
typedef __u32 nvclassid_t;    /* NvU32 class ID                      */
typedef __u64 nvp64_t;        /* NvP64 — 64-bit pointer-as-integer   */

#define NV01_NULL_OBJECT  0x00000000U

/* ── RM status codes ─────────────────────────────────────────────────────── */

#define NV_OK                   0x00000000U
#define NV_ERR_NOT_SUPPORTED    0x00000057U

/* ── Object class IDs ────────────────────────────────────────────────────── */

/* Root / client — libnvidia-ml uses class=0 (NV01_ROOT) for root allocation,
 * older apps may use NV01_ROOT_CLIENT=0x41.  Both create a root client. */
#define NV01_ROOT                           0x00000000U  /* same as NV01_NULL_OBJECT */
#define NV01_ROOT_CLIENT                    0x00000041U
/* Devices */
#define NV01_DEVICE_0                       0x00000080U
/* Subdevice */
#define NV20_SUBDEVICE_0                    0x00002080U
/* VA space */
#define FERMI_VASPACE_A                     0x000090F1U
/* Usermode */
#define TURING_USERMODE_A                   0x0000C461U
#define AMPERE_USERMODE_A                   0x0000C561U
#define HOPPER_USERMODE_A                   0x0000C661U
/* Channel groups */
#define KEPLER_CHANNEL_GROUP_A              0x0000A06CU
/* GPFIFO channels */
#define TURING_CHANNEL_GPFIFO_A             0x0000C46FU
#define AMPERE_CHANNEL_GPFIFO_A             0x0000C56FU
#define HOPPER_CHANNEL_GPFIFO_A             0x0000C86FU
/* Blackwell GPFIFO.  MEASURED: on an RTX 5090 (GB202, sm_120, driver 580.178.04)
 * libcuda allocates 0xC96F as the last RM_ALLOC of cuCtxCreate, with
 * nvos64.alloc_parms_size = 0 (it relies on the driver to size the buffer by
 * hClass).  Both ids are already in QEMU's alloc allowlist, so the alloc was
 * forwarded but — with no case in the guest's size-by-hClass switches — carried
 * 0 bytes of params, and the host RM answered NV_ERR_INVALID_ARGUMENT (0x1f),
 * surfacing as cuCtxCreate -> CUDA_ERROR_INVALID_VALUE (#101).
 * Ids confirmed against open-gpu-kernel-modules 580.95.05
 * (sdk/nvidia/inc/class/clc96f.h, clca6f.h).  Both take the SAME
 * NV_CHANNEL_ALLOC_PARAMS as the Turing/Ampere/Hopper GPFIFO classes —
 * alloc_channel.h defines exactly one struct and aliases it as
 * NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS for every GPFIFO class — so they
 * belong in the same nvkvm_prof()->chan_alloc_size group, not a new one. */
#define BLACKWELL_CHANNEL_GPFIFO_A          0x0000C96FU
#define BLACKWELL_CHANNEL_GPFIFO_B          0x0000CA6FU
/* Compute objects (hClass verified against the 575 open-driver SDK class
 * headers; the prior 0x*B1 codes were bogus). Use NV_GR_ALLOCATION_PARAMETERS. */
#define VOLTA_COMPUTE_A                     0x0000C3C0U
#define VOLTA_COMPUTE_B                     0x0000C4C0U
#define TURING_COMPUTE_A                    0x0000C5C0U
#define AMPERE_COMPUTE_A                    0x0000C6C0U
#define AMPERE_COMPUTE_B                    0x0000C7C0U
#define ADA_COMPUTE_A                       0x0000C9C0U
#define HOPPER_COMPUTE_A                    0x0000CBC0U
#define BLACKWELL_COMPUTE_A                 0x0000CDC0U
#define BLACKWELL_COMPUTE_B                 0x0000CEC0U
/* Graphics/3D objects (same NV_GR_ALLOCATION_PARAMETERS) */
#define VOLTA_A                             0x0000C397U
#define TURING_A                            0x0000C597U
#define AMPERE_A                            0x0000C697U
#define AMPERE_B                            0x0000C797U
#define ADA_A                               0x0000C997U
#define HOPPER_A                            0x0000CB97U
/* DMA copy */
#define VOLTA_DMA_COPY_A                    0x0000C3B5U
#define TURING_DMA_COPY_A                   0x0000C5B5U
#define AMPERE_DMA_COPY_A                   0x0000C6B5U
#define AMPERE_DMA_COPY_B                   0x0000C7B5U
#define HOPPER_DMA_COPY_A                   0x0000C8B5U
/* Blackwell copy engines.  BLACKWELL_DMA_COPY_A was previously recorded here as
 * 0xCBB5, which is not a class NVIDIA ships: there is no clcbb5.h in
 * open-gpu-kernel-modules, and 0xCBB5 is absent from QEMU's alloc allowlist
 * while 0xC9B5/0xCAB5 are present.  Corrected against OGKM 580.95.05
 * (sdk/nvidia/inc/class/clc9b5.h, clcab5.h).  The wrong id left the real
 * classes with no entry in the size-by-hClass switches — the same
 * allowlisted-but-unsized trap as BLACKWELL_CHANNEL_GPFIFO_A above, waiting on
 * the copy-engine alloc rather than the channel alloc (#101). */
#define BLACKWELL_DMA_COPY_A                0x0000C9B5U
#define BLACKWELL_DMA_COPY_B                0x0000CAB5U

/* NVB0B5_ALLOCATION_PARAMETERS — alloc params for all *_DMA_COPY_* classes
 * above.  libcuda passes engineType here to select a specific copy-engine
 * INSTANCE (NV2080_ENGINE_TYPE_COPY0/COPY1/COPY2/...).  Critical: if we
 * don't forward these 8 bytes to the kernel, it reads zeros, the
 * pParamToEngDescFn returns ENG_COPY(0) by default, the channel gets
 * bound to runlist 0 (GR runlist) and GPFIFO_SCHEDULE fails with
 * NV_ERR_NOT_READY — see [[gpfifo-schedule-runlist-bug]].  Diagnosed
 * 2026-05-28 via guest-vs-host dmesg of NV906F_CTRL_GET_CLASS_ENGINEID
 * returning engineID=9 (COPY0) on guest vs 10/11 on host. */
struct nvb0b5_allocation_parameters {
	__u32 version;
	__u32 engine_type;
};
/* NV_GR_ALLOCATION_PARAMETERS (nvos.h) — alloc params for every compute and
 * graphics/3D class (VOLTA..BLACKWELL _COMPUTE_A/B and _A/_B).  sizeof = 16,
 * verified on the 575 open-driver SDK.  The kernel binds these classes with
 * RS_OPTIONAL(NV_GR_ALLOCATION_PARAMETERS): libcuda may pass alloc_parms_size=0
 * and rely on by-class sizing — without an entry the ap_size==0 fallback sends
 * 0 bytes and the kernel rejects the alloc. */
struct nv_gr_allocation_parameters {
	__u32 version;
	__u32 flags;
	__u32 size;
	__u32 caps;
};
/* Memory classes */
#define NV01_MEMORY_SYSTEM                  0x0000003EU
#define NV01_MEMORY_LOCAL_USER              0x00000040U
#define NV01_MEMORY_VIRTUAL                 0x00000070U  /* NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS (24B) */
#define NV_SEMAPHORE_SURFACE                0x000000daU  /* NV_SEMAPHORE_SURFACE_ALLOC_PARAMETERS (16B) */
#define NV01_MEMORY_SYSTEM_OS_DESCRIPTOR    0x00000071U
#define NV50_MEMORY_VIRTUAL                 0x000050A0U
/* Events */
#define NV01_EVENT                          0x00000005U  /* NV0005 params; Data=fd (graphics path) */
#define NV01_EVENT_OS_EVENT                 0x00000079U
/* Graphics-path alloc classes that the Vulkan ICD allocates with
 * alloc_parms_size=0 (size-by-hClass), so the forwarder MUST know their param
 * sizes or the kernel rejects them NV_ERR_INVALID_ARGUMENT (#84). */
#define GF100_DISP_SW                       0x00009072U  /* NV9072_ALLOCATION_PARAMETERS (12B) */
#define NV_MEMORY_MAPPER                    0x000000feU  /* NV_MEMORY_MAPPER_ALLOCATION_PARAMS_V555 (24B) */
#define NV9072_ALLOC_PARAMS_SIZE            12U
#define NV_MEMORY_MAPPER_ALLOC_PARAMS_SIZE  24U
/* Context DMA */
#define NV01_CONTEXT_DMA                    0x00000002U
/* Subcontext */
#define FERMI_CONTEXT_SHARE_A               0x00009067U
/* GR debugger — libcuda creates one per context for cuda-gdb / Nsight hooks */
#define GT200_DEBUGGER                      0x000083DEU

/* NV83DE_ALLOC_PARAMETERS — alloc params for GT200_DEBUGGER.  Missing this
 * entry in the alloc-params size table caused RM_ALLOC to forward 0 bytes
 * of params, the kernel rejected with NV_ERR_INVALID_ARGUMENT, libcuda
 * tore the context down on the next cuMemAlloc → CUDA_ERROR_CONTEXT_IS_-
 * DESTROYED (709). */
struct nv83de_alloc_parameters {
	nvhandle_t h_debugger_client_obsolete;  /* must be 0 */
	nvhandle_t h_app_client;
	nvhandle_t h_class_3d_object;
};

/* ── RS_ACCESS_MASK ──────────────────────────────────────────────────────── */

struct rs_access_mask {
	__u32 limbs[1];    /* variable length; here just 1 limb for simplicity */
};

/* ── NV_ESC_RM_ALLOC parameter structs ───────────────────────────────────── */

/*
 * NVOS21_PARAMETERS — used when hRightsRequested == 0 / no access mask.
 * From src/common/sdk/nvidia/inc/nvos.h.
 */
struct nvos21_parameters {
	nvhandle_t h_root;
	nvhandle_t h_object_parent;
	nvhandle_t h_object_new;
	nvclassid_t h_class;
	nvp64_t    p_alloc_parms;   /* pointer to class-specific alloc struct */
	__u32      status;
	__u32      _pad;            /* trailing alignment pad — sizeof == 32 on x86-64 */
};

/*
 * NVOS64_PARAMETERS — used when hRightsRequested != NULL (newer drivers).
 *
 * Field order matches NVIDIA open-gpu-kernel-modules nvos.h NVOS64_PARAMETERS:
 *   hRoot(4), hObjectParent(4), hObjectNew(4), hClass(4),
 *   pAllocParms(8), pRightsRequested(8),
 *   paramsSize(4), flags(4), status(4), reserved(4)
 * = 48 bytes total. (Cross-checked with gVisor pkg/abi/nvgpu/frontend.go.)
 */
struct nvos64_parameters {
	nvhandle_t h_root;
	nvhandle_t h_object_parent;
	nvhandle_t h_object_new;
	nvclassid_t h_class;
	nvp64_t    p_alloc_parms;       /* pointer to class-specific alloc struct */
	nvp64_t    p_rights_requested;  /* pointer to RS_ACCESS_MASK (may be NULL) */
	__u32      alloc_parms_size;    /* size of alloc params buffer            */
	__u32      flags;
	__u32      status;
	__u32      reserved;
};

/* ── NV_ESC_RM_FREE ──────────────────────────────────────────────────────── */

struct nvos00_parameters {
	nvhandle_t h_root;
	nvhandle_t h_object_parent;
	nvhandle_t h_object_old;
	__u32      status;
};

/* ── NV_ESC_RM_CONTROL ───────────────────────────────────────────────────── */

struct nvos54_parameters {
	nvhandle_t h_client;
	nvhandle_t h_object;
	__u32      cmd;
	__u32      flags;
	nvp64_t    params;      /* pointer to command-specific param struct */
	__u32      params_size;
	__u32      status;
};

/* ── NV_ESC_RM_DUP_OBJECT ────────────────────────────────────────────────── */

/*
 * NVOS55_PARAMETERS — verified against the 575 open kernel module SDK
 * (src/common/sdk/nvidia/inc/nvos.h): 7 fields, 28 bytes.  An earlier version
 * of this struct had 9 fields (36 B) with phantom h_parent_client/h_src_parent
 * — wrong, but never caught because DUP_OBJECT is unexercised by matmul. The
 * Phase-4 DUP gate and the cross-process PoC depend on hClientSrc being at the
 * correct offset (12), so this layout matters.
 */
struct nvos55_parameters {
	nvhandle_t h_client;       /* [IN]    destination client handle      */
	nvhandle_t h_parent;       /* [IN]    parent of new object           */
	nvhandle_t h_object;       /* [INOUT] destination (new) object handle */
	nvhandle_t h_client_src;   /* [IN]    source client handle           */
	nvhandle_t h_src_object;   /* [IN]    source (old) object handle      */
	__u32      flags;
	__u32      status;
};

/* ── NV_ESC_RM_SHARE ─────────────────────────────────────────────────────── */

struct nvos57_parameters {
	nvhandle_t h_client;
	nvhandle_t h_object;
	__u32      share_policy;    /* RS_SHARE_POLICY */
	__u32      status;
};

/* ── NV_ESC_RM_ALLOC_MEMORY ─────────────────────────────────────────────── */

struct nv_ioctl_nvos02_parameters_with_fd {
	nvhandle_t h_root;
	nvhandle_t h_object_parent;
	nvhandle_t h_object_new;
	nvclassid_t h_class;
	__u32      flags;
	__u32      reserved;
	nvp64_t    p_memory;     /* host VA returned by driver           */
	__u64      limit;
	__u32      status;
	__u32      pad1;
	__s32      fd;           /* fd to associate with allocation      */
	__u32      pad0;
};

/* ── NV_ESC_RM_MAP_MEMORY ────────────────────────────────────────────────── */

struct nv_ioctl_nvos33_parameters_with_fd {
	nvhandle_t h_client;
	nvhandle_t h_device;
	nvhandle_t h_memory;
	__u32      reserved0;
	__u64      offset;
	__u64      length;
	nvp64_t    p_linear_address;  /* returned: mapped VA             */
	__u32      status;
	__u32      flags;
	__s32      fd;
	__u32      reserved1;
};

/* ── NV_ESC_RM_UNMAP_MEMORY ──────────────────────────────────────────────── */

struct nv_ioctl_nvos34_parameters {
	nvhandle_t h_client;
	nvhandle_t h_device;
	nvhandle_t h_memory;
	__u32      reserved0;
	nvp64_t    p_linear_address;  /* VA to unmap                     */
	__u32      status;
	__u32      flags;
};

/* ── NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO ─────────────────────────────────── */
/*    NVOS56_PARAMETERS, 40 bytes. */
struct nvos56_parameters {
	nvhandle_t h_client;
	nvhandle_t h_device;
	nvhandle_t h_memory;
	__u32      _pad0;
	nvp64_t    p_old_cpu_address;
	nvp64_t    p_new_cpu_address;
	__u32      status;
	__u32      _pad1;
};

/* ── NV_ESC_RM_VID_HEAP_CONTROL ──────────────────────────────────────────── */

/*
 * NVOS32_PARAMETERS — real ABI is 184 bytes: a fixed prefix followed by a large
 * union selected by `function`.  An earlier 88-byte definition with a scrambled
 * field order truncated it, losing the AllocSize union fields at offsets 88+
 * (size/alignment/offset/limit/address).  For the legacy graphics allocation
 * path (NVOS32_FUNCTION_ALLOC_SIZE, used by libGLX_nvidia — compute uses
 * RM_ALLOC memory classes instead) that meant the kernel got size=0 and never
 * returned the allocated address, so libGLX saw a bogus allocation and bailed.
 *
 * We forward NVOS32 opaquely, so only the fixed prefix matters here (status is
 * at offset 20); the union is a pass-through byte blob sized to the full 184.
 * Verified against host nvidia-drm 580 (_IOC_SIZE == 184).
 */
struct nvos32_parameters {
	nvhandle_t h_root;            /* 0  [IN]  */
	nvhandle_t h_object_parent;   /* 4  [IN]  */
	__u32      function;          /* 8  [IN]  */
	nvhandle_t h_vaspace;         /* 12 [IN]  */
	__u32      ivc_heap_number;   /* 16 [IN]  (NvS16 + pad; opaque)        */
	__u32      status;            /* 20 [OUT] */
	__u64      total;             /* 24 [OUT] */
	__u64      free;              /* 32 [OUT] */
	__u8       data[144];         /* 40..183  union (AllocSize/Info/...)    */
};

/* ── NV_ESC_RM_MAP_MEMORY_DMA ────────────────────────────────────────────── */

struct nvos46_parameters {
	nvhandle_t h_client;
	nvhandle_t h_device;
	nvhandle_t h_dma;
	nvhandle_t h_memory;
	__u64      offset;
	__u64      length;
	__u32      flags;
	__u32      pad0;
	__u64      dma_offset;   /* [out] GPU DMA address of the mapping */
	__u32      status;
	__u32      pad1;
};

/* ── NV_ESC_RM_UNMAP_MEMORY_DMA ──────────────────────────────────────────── */

struct nvos47_parameters {
	nvhandle_t h_client;
	nvhandle_t h_device;
	nvhandle_t h_dma;
	nvhandle_t h_memory;
	__u32      flags;
	__u32      pad0;
	__u64      dma_offset;  /* [in] from NV04_MAP_MEMORY_DMA */
	__u64      size;        /* [in] size to unmap (0 = full) */
	__u32      status;
	__u32      pad1;
};

/* ── NV_ESC_RM_IDLE_CHANNELS ─────────────────────────────────────────────── */

/*
 * NVOS30_PARAMETERS — 56 bytes.  Field order matches the driver exactly
 * (audit G-2: a previous 40-byte, pointers-first layout caused the host driver
 * to read 56B from a 40B buffer (OOB read of stub heap) and to interpret
 * guest scalar fields as the phClients/phDevices/phChannels pointers, then
 * dereference those guest-controlled values).  h_client/h_device/h_channel
 * carry the single-channel form; the p_* arrays carry the multi-channel form.
 * The guest sanitizer forces the single-channel form (num_channels = 0, arrays
 * dropped) so no guest pointer is ever forwarded — see nvkvm_sanitize_ioctl_params.
 */
struct nv_ioctl_idle_channels {
	nvhandle_t h_client;        /* [in] */
	nvhandle_t h_device;        /* [in] */
	nvhandle_t h_channel;       /* [in] single-channel form */
	__u32      num_channels;    /* [in] 0 = idle single h_channel */
	nvp64_t    p_clients;       /* [in] array of num_channels client handles */
	nvp64_t    p_devices;       /* [in] array of num_channels device handles */
	nvp64_t    p_channels;      /* [in] array of num_channels channel handles*/
	__u32      flags;           /* [in] */
	__u32      timeout;         /* [in] */
	__u32      status;          /* [out] */
	__u32      pad;             /* align to 56 */
};

/* ── NV_ESC_CARD_INFO ────────────────────────────────────────────────────── */

#define NV_IOCTL_CARD_INFO_BUS_TYPE_PCI  0x1
#define NV_IOCTL_CARD_INFO_MAX_ENTRIES   32

struct nv_pci_info {
	__u32 domain;
	__u8  bus;
	__u8  slot;
	__u8  function;
	__u8  reserved;
	__u16 vendor_id;
	__u16 device_id;
};

struct nv_ioctl_card_info {
	__u8            valid;
	__u8            reserved[3];
	struct nv_pci_info pci_info;
	__u32           gpu_id;
	__u16           interrupt_line;
	__u8            reserved2[2];
	__u64           reg_address;
	__u64           reg_size;
	__u64           fb_address;
	__u64           fb_size;
	__u32           minor_number;
	__u8            dev_name[14];  /* gVisor NvIoctlCardInfo.DevName is 14 bytes */
	__u8            reserved3[2];
	/* 4 bytes implicit trailing padding → sizeof == 80 on x86-64 */
};

/* ── NV_ESC_CHECK_VERSION_STR ────────────────────────────────────────────── */

#define NV_RM_API_VERSION_STRING_LENGTH  64

struct nv_ioctl_rm_api_version {
	__u32 cmd;
	__u32 reply;
	char  version_string[NV_RM_API_VERSION_STRING_LENGTH];
};

/* ── NV_ESC_SYS_PARAMS ───────────────────────────────────────────────────── */

struct nv_ioctl_sys_params {
	__u64 memory_size;
};

/* ── NV_ESC_ALLOC_OS_EVENT / NV_ESC_FREE_OS_EVENT ───────────────────────── */

struct nv_ioctl_alloc_os_event {
	nvhandle_t h_client;
	nvhandle_t h_device;
	__u32      fd;
	__u32      status;
};

struct nv_ioctl_free_os_event {
	nvhandle_t h_client;
	nvhandle_t h_device;
	__u32      fd;
	__u32      status;
};

/* ── NV_ESC_REGISTER_FD ──────────────────────────────────────────────────── */

struct nv_ioctl_register_fd {
	__s32 ctl_fd;    /* fd for /dev/nvidiactl to associate  */
};

/* ── NV_ESC_WAIT_OPEN_COMPLETE ───────────────────────────────────────────── */

struct nv_ioctl_wait_open_complete {
	__s32 rc;
	__u32 adapter_status;
};

/* ── NV_ESC_NUMA_INFO ────────────────────────────────────────────────────── */

struct nv_ioctl_numa_info {
	__s32  nid;
	__u32  status;
	__u64  memblock_size;
	__u64  numa_mem_addr;
	__u64  numa_mem_size;
};

/* ── NV_ESC_EXPORT_TO_DMABUF_FD ──────────────────────────────────────────── */

struct nv_ioctl_export_to_dmabuf_fd {
	nvhandle_t h_client;
	nvhandle_t h_memory;
	__u64      size;
	__u64      offset;
	__u32      map_type;
	__s32      fd;         /* result: dmabuf fd                     */
	__u32      b_allow_mmap;
	__u32      status;
};

/* ── NV_ESC_ALLOC_CONTEXT_DMA2 ───────────────────────────────────────────── */

struct nv_ioctl_alloc_context_dma2 {
	nvhandle_t h_client;
	nvhandle_t h_parent;
	nvhandle_t h_memory;
	nvclassid_t h_class;
	__u32      flags;
	__u32      attr;
	__u64      va_space;
	__u64      va_base;
	__u64      limit;
	nvhandle_t h_dma;
	__u32      status;
};

/* ── NV0080_ALLOC_PARAMETERS — alloc params for NV01_DEVICE_0 ────────────── */

struct nv0080_alloc_parameters {
	__u32      device_id;
	nvhandle_t h_client_share;
	nvhandle_t h_target_client;
	nvhandle_t h_target_device;
	__u32      flags;
	__u32      _pad0;
	__u64      va_space_size;
	__u64      va_start_internal;
	__u64      va_limit_internal;
	__u32      va_mode;
	__u32      _pad1;
};

/* ── NV2080_ALLOC_PARAMETERS — alloc params for NV20_SUBDEVICE_0 ─────────── */

struct nv2080_alloc_parameters {
	__u32      sub_device_id;
};

/* ── NV00DE_ALLOC_PARAMETERS — alloc params for RM_USER_SHARED_DATA (0xDE) ── */

#define RM_USER_SHARED_DATA 0x000000DEU

/* V545 layout (driver >= 545.23.06): a single uint64. Our target driver
 * 575.51.03 uses this layout. */
struct nv00de_alloc_parameters_v545 {
	__u64 polled_data_mask;
};

/* ── NV_VASPACE_ALLOCATION_PARAMETERS — for FERMI_VASPACE_A (0x90F1) ──────── */

/* Layout pre-580 driver (we target 575.51.03). */
struct nv_vaspace_allocation_parameters {
	__u32 index;
	__u32 flags;
	__u64 va_size;
	__u64 va_start_internal;
	__u64 va_limit_internal;
	__u32 big_page_size;
	__u32 _pad0;
	__u64 va_base;
};

/* ── NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS — for KEPLER_CHANNEL_GROUP_A ─── */
struct nv_channel_group_allocation_parameters {
	nvhandle_t h_object_error;
	nvhandle_t h_object_ecc_error;
	nvhandle_t h_va_space;
	__u32      engine_type;
	__u8       b_is_calling_context_vgpu_plugin;
	__u8       _pad0[3];
};

/* ── NV_CTXSHARE_ALLOCATION_PARAMETERS — for FERMI_CONTEXT_SHARE_A (0x9067) */
struct nv_ctxshare_allocation_parameters {
	nvhandle_t h_va_space;
	__u32      flags;
	__u32      subctx_id;
};

/* ── NV0005_ALLOC_PARAMETERS — for NV01_EVENT_OS_EVENT (0x79) and friends ── */
/*    Data field is actually an FD (eventfd) for NV01_EVENT_OS_EVENT — the
 *    driver calls osUserHandleToKernelPtr() to translate it.  That means
 *    libcuda passes a guest-userspace fd, the driver dereferences it in
 *    the isolate, and it fails.  See [[ioctl-nr-collision-bug]] for the
 *    related pattern. */
struct nv0005_alloc_parameters {
	nvhandle_t  h_parent_client;
	nvhandle_t  h_src_resource;
	nvclassid_t h_class;
	__u32       notify_index;
	__u64       data;  /* fd for NV01_EVENT_OS_EVENT */
};

/* ── NV_MEMORY_DESC_PARAMS — embedded in channel-alloc params ─────────────── */
struct nv_memory_desc_params {
	__u64 base;
	__u64 size;
	__u32 address_space;
	__u32 cache_attrib;
};

/* ── NV_CHANNEL_ALLOC_PARAMS_V570 — for TURING/AMPERE/HOPPER_CHANNEL_GPFIFO_A
 *     on driver >= 570 (we're 575.51.03).  See gVisor's
 *     NV_CHANNEL_ALLOC_PARAMS / _V570 — V570 adds TPCConfigID + pad. */
#define NV_MAX_SUBDEVICES                 8
#define NV_CC_CHAN_ALLOC_IV_SIZE_DWORD    3
#define NV_CC_CHAN_ALLOC_NONCE_SIZE_DWORD 8

struct nv_channel_alloc_params_v570 {
	nvhandle_t h_object_error;
	nvhandle_t h_object_buffer;
	__u64 gpfifo_offset;
	__u32 gpfifo_entries;
	__u32 flags;
	nvhandle_t h_context_share;
	nvhandle_t h_va_space;
	nvhandle_t h_userd_memory[NV_MAX_SUBDEVICES];
	__u64      userd_offset[NV_MAX_SUBDEVICES];
	__u32 engine_type;
	__u32 cid;
	__u32 sub_device_id;
	nvhandle_t h_object_ecc_error;
	struct nv_memory_desc_params instance_mem;
	struct nv_memory_desc_params userd_mem;
	struct nv_memory_desc_params ramfc_mem;
	struct nv_memory_desc_params mthdbuf_mem;
	nvhandle_t h_phys_channel_group;
	__u32 internal_flags;
	struct nv_memory_desc_params error_notifier_mem;
	struct nv_memory_desc_params ecc_error_notifier_mem;
	__u32 process_id;
	__u32 sub_process_id;
	__u32 encrypt_iv[NV_CC_CHAN_ALLOC_IV_SIZE_DWORD];
	__u32 decrypt_iv[NV_CC_CHAN_ALLOC_IV_SIZE_DWORD];
	__u32 hmac_nonce[NV_CC_CHAN_ALLOC_NONCE_SIZE_DWORD];
	/* V570 extension */
	__u32 tpc_config_id;
	__u32 _pad0;
};

/* ── NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS — for NV01_MEMORY_VIRTUAL (0x70) ──── */
/*    24 bytes; libGLX (EGL device enum) leaves nvos64.alloc_parms_size=0 and
 *    relies on the kernel sizing it by hClass, so our forwarding MUST supply
 *    this size for the inner params (incl hVASpace@16) to reach the kernel —
 *    otherwise the alloc fails NV_ERR_INVALID_ARGUMENT and graphics bails. */
struct nv_memory_virtual_allocation_params {
	__u64 offset;     /* [IN]     */
	__u64 limit;      /* [IN/OUT] */
	__u32 h_vaspace;  /* [IN]     */
};

/* ── NV_SEMAPHORE_SURFACE_ALLOC_PARAMETERS — for NV_SEMAPHORE_SURFACE (0xda) ── */
/*    16 bytes; from src/common/sdk/nvidia/inc/class/cl00da.h. Like
 *    NV01_MEMORY_VIRTUAL, libGLX (EGL device enum) allocates this with
 *    nvos64.alloc_parms_size=0, so the forwarder MUST size it by hClass — else
 *    the inner params never reach the kernel and the alloc fails
 *    NV_ERR_INVALID_ARGUMENT (0x1f), which crashes libnvidia-eglcore later.
 *    h_semaphore_mem / h_max_submitted_mem are RM object handles in the
 *    client's namespace (forwarded verbatim, not fd handle_ids). */
struct nv_semaphore_surface_alloc_parameters {
	__u32 h_semaphore_mem;     /* [IN] */
	__u32 h_max_submitted_mem; /* [IN] */
	__u64 flags;               /* [IN] */
};

/* ── NV_CONTEXT_DMA_ALLOCATION_PARAMS — for NV01_CONTEXT_DMA (0x0002) ──────── */
/*    32 bytes; from src/common/sdk/nvidia/inc/nvos.h. The NVIDIA video stack
 *    (NVENC) binds a context-DMA over a memory object and allocs it with
 *    nvos.alloc_parms_size=0, relying on class sizing — so like NV01_MEMORY_VIRTUAL
 *    and NV_SEMAPHORE_SURFACE the forwarder MUST size it by hClass, else the inner
 *    params never reach the kernel and the alloc fails NV_ERR_INVALID_ARGUMENT
 *    (0x1f), and NVENC's InitializeEncoder bails ("EncodeAPI Internal Error", #99).
 *    h_subdevice / h_memory are RM object handles in the client namespace
 *    (forwarded verbatim, not fd handle_ids). */
struct nv_context_dma_allocation_params {
	__u32 h_subdevice;  /* [IN] @0  */
	__u32 flags;        /* [IN] @4  */
	__u32 h_memory;     /* [IN] @8  */
	__u32 pad;          /*      @12 */
	__u64 offset;       /* [IN] @16 */
	__u64 limit;        /* [IN] @24 */
};

/* ── NVA0BC_ALLOC_PARAMETERS — for NVENC_SW_SESSION (0xa0bc) ──────────────── */
/*    20 bytes; from class/cla0bc.h. NVENC allocs this session-tracking object
 *    (what nvidia-smi reads for encoder-session stats) with alloc_parms_size=0;
 *    same class-sizing path as NV01_CONTEXT_DMA. Non-fatal to encode if missing,
 *    but the alloc fails INVALID_ARGUMENT and the session never registers (#99).
 *    h_mem is an RM object handle (client namespace, forwarded verbatim). */
#define NVENC_SW_SESSION 0x0000a0bcU
struct nva0bc_alloc_parameters {
	__u32 codec_type;    /* [IN] @0  */
	__u32 h_resolution;  /* [IN] @4  */
	__u32 v_resolution;  /* [IN] @8  */
	__u32 version;       /* [IN] @12 */
	__u32 h_mem;         /* [IN] @16 */
};

/* ── NV503C_ALLOC_PARAMETERS — for NV50_THIRD_PARTY_P2P (0x503c) ──────────── */
/*    4 bytes; from class/cl503c.h.  MEASURED with
 *      gcc probe.c -I<ogkm>/src/common/sdk/nvidia/inc ... ; sizeof(...) == 4
 *    against open-gpu-kernel-modules tag 575.51.03 (the exact host driver of
 *    the Ada bring-up box).  Do NOT hand-derive this.
 *
 *    WHY IT MATTERS (Ada bring-up, 2026-08-17): libcuda allocates this object
 *    during cuInit with alloc_parms_size=0, relying on the driver to size the
 *    buffer by hClass — the same pattern as NV01_MEMORY_VIRTUAL (#84) and
 *    NV01_CONTEXT_DMA (#99).  The class was in QEMU's alloc-class allowlist but
 *    NOT in either of the guest's two size-by-hClass switches, so the forwarder
 *    copied ZERO bytes of params and the host RM answered NV_ERR_NOT_SUPPORTED
 *    (0x56).  libcuda turned that into cuInit -> CUDA_ERROR_NOT_SUPPORTED (801)
 *    and the guest could not initialise CUDA at all. */
#define NV50_THIRD_PARTY_P2P 0x0000503cU
struct nv503c_alloc_parameters {
	__u32 flags;         /* [IN] @0 — NV503C_ALLOC_PARAMETERS_FLAGS_TYPE */
};

/* ── NV_MEMORY_ALLOCATION_PARAMS — for NV50_MEMORY_VIRTUAL (0x50A0) and ──── */
/*    several other generic memory classes. V545 layout (driver >= 545.23.06,
 *    matches our 575.51.03): adds numa_node + pad. */
struct nv_memory_allocation_params_v545 {
	__u32 owner;
	__u32 type;
	__u32 flags;
	__u32 width;
	__u32 height;
	__s32 pitch;
	__u32 attr;
	__u32 attr2;
	__u32 format;
	__u32 compr_covg;
	__u32 zcull_covg;
	__u32 _pad0;
	__u64 range_lo;
	__u64 range_hi;
	__u64 size;
	__u64 alignment;
	__u64 offset;
	__u64 limit;
	nvp64_t  address;
	__u32 ctag_offset;
	nvhandle_t h_va_space;
	__u32 internal_flags;
	__u32 tag;
	__s32 numa_node;       /* added in V545 */
	__u32 _pad1;
};

/* ── NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION (0x101) ────────────────────── */

#define NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION 0x00000101U

/* NV_DECLARE_ALIGNED(NvP64, 8) fields: 4-byte pad before each 8-byte pointer */
struct nv0000_ctrl_system_get_build_version_params {
	__u32    size_of_strings;
	__u32    _pad0;
	nvp64_t  p_driver_version_buffer;  /* out: e.g. "575.51.03" */
	nvp64_t  p_version_buffer;         /* out: numeric version   */
	nvp64_t  p_title_buffer;           /* out: display title     */
	__u32    changelist_number;        /* out */
	__u32    official_changelist_number; /* out */
};

/* ── Commands with embedded InfoList pointer (NvxxxCtrlXxxGetInfoParams) ── */
/*
 * These RM_CONTROL inner commands share a common preamble:
 *   uint32 info_list_size;  // offset 0
 *   uint32 _pad;            // offset 4
 *   nvp64  info_list;       // offset 8   (pointer to info_list_size * 8 bytes)
 *
 * Each entry is two uint32: (index, data). CUDA pre-fills the index fields and
 * reads the data fields after the call. The guest sanitizer must carry the
 * list contents through the aux slot since the host has no access to guest VAs.
 *
 * Pulled from gVisor pkg/abi/nvgpu/ctrl.go.
 */
#define NV0041_CTRL_CMD_GET_SURFACE_INFO  0x00410110U
#define NV0080_CTRL_CMD_GR_GET_INFO       0x00801104U
#define NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST 0x0080170dU
#define NV2080_CTRL_CMD_BIOS_GET_INFO     0x20800802U
#define NV2080_CTRL_CMD_GR_GET_INFO       0x20801201U
#define NV2080_CTRL_CMD_FB_GET_INFO       0x20801301U
#define NV2080_CTRL_CMD_BUS_GET_INFO      0x20801802U
#define NVXXX_CTRL_XXX_INFO_ENTRY_SIZE    8U

/*
 * NV2080_CTRL_CMD_GPU_GET_ENGINES (non-V2) embeds { NvU32 engineCount@0; NvP64
 * engineList@8 } where engineList points at a NvU32[engineCount] array the
 * driver writes through (engineCount is IN=capacity / OUT=actual). Same
 * preamble shape as GetInfo but the entries are 4-byte engine IDs, so the list
 * handler uses an entry size of 4. The V2 form (0x20800170) inlines the array
 * and needs no handling. cl2080.h / ctrl2080gpu.h.
 */
#define NV2080_CTRL_CMD_GPU_GET_ENGINES   0x20800123U

/*
 * NV0080_CTRL_CMD_GPU_GET_CLASSLIST (non-V2) embeds { NvU32 numClasses@0; NvP64
 * classList@8 } where classList points at a NvU32[numClasses] array of object
 * class IDs the driver writes through (numClasses is IN=capacity / OUT=actual).
 * Same {size@0, pad, ptr@8} preamble as GET_ENGINES, entries are 4-byte class
 * IDs → list entry size 4. libcuda uses the inline V2 (0x800292); the NVIDIA
 * video stack (libnvidia-encode/NVENC) uses THIS pointer form to discover the
 * encoder engine class — without it NVENC reports "unsupported device". ctrl0080gpu.h.
 */
#define NV0080_CTRL_CMD_GPU_GET_CLASSLIST 0x00800201U

/*
 * Device-level (NV0080) GET_CAPS family. These embed an
 * NvxxxCtrlXxxGetCapsParams preamble { NvU32 capsTblSize@0; NvP64 capsTbl@8 }
 * — STRUCTURALLY identical to the GetInfo preamble (u32@0, ptr@8) but the
 * size field counts capability-table BYTES, not 8-byte info entries. The
 * list handler must therefore use an entry size of 1 for these, not
 * NVXXX_CTRL_XXX_INFO_ENTRY_SIZE. See open-kernel-module
 * ctrl0080{gr,fb,host,fifo,msenc,bsp}.h.
 */
#define NV0080_CTRL_CMD_GR_GET_CAPS      0x00801102U
#define NV0080_CTRL_CMD_FB_GET_CAPS      0x00801301U
#define NV0080_CTRL_CMD_HOST_GET_CAPS    0x00801401U
#define NV0080_CTRL_CMD_FIFO_GET_CAPS    0x00801701U
#define NV0080_CTRL_CMD_MSENC_GET_CAPS   0x00801b01U
#define NV0080_CTRL_CMD_BSP_GET_CAPS_V2  0x00801c02U

/* ── NV_ESC_RM_CONTROL command IDs used in tests ─────────────────────────── */

#define NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES 0x00800280U

struct nv0080_ctrl_gpu_get_num_subdevices_params {
	__u32 num_sub_devices;
};

/* ── NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE (0x800289) ──────────────── */

#define NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE 0x00800289U

#define NV0080_CTRL_GPU_VIRTUALIZATION_MODE_NONE            0U
#define NV0080_CTRL_GPU_VIRTUALIZATION_MODE_VGX             1U
#define NV0080_CTRL_GPU_VIRTUALIZATION_MODE_HOST_VGPU       2U
#define NV0080_CTRL_GPU_VIRTUALIZATION_MODE_HOST_VSGA       3U
#define NV0080_CTRL_GPU_VIRTUALIZATION_MODE_PASSTHROUGHGUEST 4U

struct nv0080_ctrl_gpu_get_virtualization_mode_params {
	__u32 virtualization_mode;
	__u32 b_is_grid_licensed;
};

/* ── NV2080_CTRL_CMD_TIMER_GET_GPU_CPU_TIME_CORRELATION_INFO (0x20800406) ─ */

#define NV2080_CTRL_CMD_TIMER_GET_GPU_CPU_TIME_CORRELATION_INFO 0x20800406U

/* ── NV2080_CTRL_CMD_MC_GET_ARCH_INFO (0x20801701) ──────────────────────── */

#define NV2080_CTRL_CMD_MC_GET_ARCH_INFO    0x20801701U

/* ── NV2080_CTRL_CMD_GPU_GET_GID_INFO (0x2080014a) ──────────────────────── */

#define NV2080_CTRL_CMD_GPU_GET_GID_INFO    0x2080014aU

/* ── NV2080_CTRL_CMD_GPU_GET_NAME_STRING (0x20800110) ────────────────────── */

#define NV2080_CTRL_CMD_GPU_GET_NAME_STRING 0x20800110U

/* ── NV2080_CTRL_CMD_GPU_GET_INFO_V2 (0x20800102) ───────────────────────── */

#define NV2080_CTRL_CMD_GPU_GET_INFO_V2     0x20800102U

/* ── Frontend ioctl numbers (IOC_NR portion only) ────────────────────────── */

#define NV_IOCTL_BASE                           200
#define NV_ESC_CARD_INFO                        (NV_IOCTL_BASE + 0)
#define NV_ESC_REGISTER_FD                      (NV_IOCTL_BASE + 1)
#define NV_ESC_ALLOC_OS_EVENT                   (NV_IOCTL_BASE + 6)
#define NV_ESC_FREE_OS_EVENT                    (NV_IOCTL_BASE + 7)
#define NV_ESC_CHECK_VERSION_STR                (NV_IOCTL_BASE + 10)
#define NV_ESC_ATTACH_GPUS_TO_FD                (NV_IOCTL_BASE + 12)
#define NV_ESC_SYS_PARAMS                       (NV_IOCTL_BASE + 14)
#define NV_ESC_NUMA_INFO                        (NV_IOCTL_BASE + 15)
#define NV_ESC_WAIT_OPEN_COMPLETE               (NV_IOCTL_BASE + 18)

#define NV_ESC_RM_ALLOC_MEMORY                  0x27
#define NV_ESC_RM_FREE                          0x29
#define NV_ESC_RM_CONTROL                       0x2a
#define NV_ESC_RM_ALLOC                         0x2b
#define NV_ESC_RM_DUP_OBJECT                    0x34
#define NV_ESC_RM_SHARE                         0x35
#define NV_ESC_RM_IDLE_CHANNELS                 0x41
#define NV_ESC_RM_VID_HEAP_CONTROL              0x4a
#define NV_ESC_RM_MAP_MEMORY                    0x4e
#define NV_ESC_RM_UNMAP_MEMORY                  0x4f
#define NV_ESC_RM_ALLOC_CONTEXT_DMA2            0x54
#define NV_ESC_RM_MAP_MEMORY_DMA                0x57
#define NV_ESC_RM_UNMAP_MEMORY_DMA              0x58
#define NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO    0x5e
#define NV_ESC_EXPORT_TO_DMABUF_FD              0x70

#endif /* NVGPU_H */
