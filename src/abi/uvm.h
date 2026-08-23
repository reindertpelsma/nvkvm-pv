/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * uvm.h — /dev/nvidia-uvm ioctl ABI
 *
 * Ported from gVisor pkg/abi/nvgpu/uvm.go and the open-gpu-kernel-modules
 * uvm_ioctl.h / uvm_linux_ioctl.h headers.
 */

#ifndef NVKVM_UVM_H
#define NVKVM_UVM_H

#include <linux/types.h>
#include "nvgpu.h"

/* ── UVM ioctl command numbers ───────────────────────────────────────────── */

/* From kernel-open/nvidia-uvm/uvm_linux_ioctl.h */
#define UVM_INITIALIZE                          0x30000001
#define UVM_DEINITIALIZE                        0x30000002

/* From kernel-open/nvidia-uvm/uvm_ioctl.h — base command numbers */
#define UVM_CREATE_RANGE_GROUP                  23
#define UVM_DESTROY_RANGE_GROUP                 24
#define UVM_REGISTER_GPU_VASPACE                25
#define UVM_UNREGISTER_GPU_VASPACE              26
#define UVM_REGISTER_CHANNEL                    27
#define UVM_UNREGISTER_CHANNEL                  28
#define UVM_ENABLE_PEER_ACCESS                  29
#define UVM_DISABLE_PEER_ACCESS                 30
#define UVM_SET_RANGE_GROUP                     31
#define UVM_MAP_EXTERNAL_ALLOCATION             33
#define UVM_FREE                                34
#define UVM_REGISTER_GPU                        37
#define UVM_UNREGISTER_GPU                      38
#define UVM_PAGEABLE_MEM_ACCESS                 39
#define UVM_SET_PREFERRED_LOCATION              42
#define UVM_UNSET_PREFERRED_LOCATION            43
#define UVM_ENABLE_READ_DUPLICATION             44
#define UVM_DISABLE_READ_DUPLICATION            45
#define UVM_SET_ACCESSED_BY                     46
#define UVM_UNSET_ACCESSED_BY                   47
#define UVM_MIGRATE                             51
#define UVM_MIGRATE_RANGE_GROUP                 53
#define UVM_TOOLS_READ_PROCESS_MEMORY           62
#define UVM_TOOLS_WRITE_PROCESS_MEMORY          63
#define UVM_MAP_DYNAMIC_PARALLELISM_REGION      65
#define UVM_UNMAP_EXTERNAL                      66
#define UVM_ALLOC_SEMAPHORE_POOL                68
#define UVM_PAGEABLE_MEM_ACCESS_ON_GPU          70
#define UVM_VALIDATE_VA_RANGE                   72
#define UVM_CREATE_EXTERNAL_RANGE               73
#define UVM_MM_INITIALIZE                       75

/* UVM_INITIALIZE flags */
#define UVM_INIT_FLAGS_MULTI_PROCESS_SHARING_MODE  0x2ULL

/* ── UVM ioctl parameter structs ─────────────────────────────────────────── */

struct uvm_initialize_params {
	__u64 flags;
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_deinitialize_params {
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_mm_initialize_params {
	__s32 uvm_fd;    /* IN:  fd_token of the primary UVM fd (UVM_INITIALIZE was called on it) */
	__u32 rm_status; /* OUT: NV_STATUS */
};

/* 16-byte UUID for GPU identification */
struct uvm_uuid {
	__u8 uuid[16];
};

struct uvm_register_gpu_params {
	struct uvm_uuid gpu_uuid;
	__u8   numa_enabled;
	__u8   reserved0[3];
	__s32  numa_node_id;
	__u32  rm_status;
	__u32  reserved1;
};

struct uvm_unregister_gpu_params {
	struct uvm_uuid gpu_uuid;
	__u32  rm_status;
	__u32  reserved;
};

struct uvm_register_gpu_vaspace_params {
	struct uvm_uuid gpu_uuid;
	nvhandle_t rm_ctrl_fd;    /* fd for /dev/nvidiactl                */
	nvhandle_t h_client;
	nvhandle_t h_va_space;
	__u32 rm_status;
};

struct uvm_unregister_gpu_vaspace_params {
	struct uvm_uuid gpu_uuid;
	__u32 rm_status;
};

struct uvm_register_channel_params {
	struct uvm_uuid gpu_uuid;
	nvhandle_t rm_ctrl_fd;
	nvhandle_t h_client;
	nvhandle_t h_channel;
	__u32 rm_status;
	__u64 base;
	__u64 length;
};

struct uvm_unregister_channel_params {
	struct uvm_uuid gpu_uuid;
	nvhandle_t h_client;
	nvhandle_t h_channel;
	__u32 rm_status;
};

struct uvm_create_range_group_params {
	__u64 range_group_id;
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_destroy_range_group_params {
	__u64 range_group_id;
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_set_range_group_params {
	__u64 range_group_id;
	__u64 base;
	__u64 length;
	__u32 rm_status;
	__u32 reserved;
};

/* Per-GPU mapping attributes — one entry per registered GPU.
 * From src/nvidia-uvm/uvm_ioctl.h: UvmGpuMappingAttributes.  36 bytes.
 */
struct uvm_gpu_mapping_attributes {
	struct uvm_uuid gpu_uuid;
	__u32 gpu_mapping_type;
	__u32 gpu_caching_type;
	__u32 gpu_format_type;
	__u32 gpu_element_bits;
	__u32 gpu_compression_type;
};

/* Driver 550.54.14+ uses the V550 layout: PerGPUAttributes[] became an
 * array of UVM_MAX_GPUS_V2 = 32 * 8 = 256 entries (was 1 entry pre-V550),
 * and GPUAttributesCount widened from u32 to u64.  Driver 575.51.03 uses
 * this layout, so we follow gVisor's
 * UVM_MAP_EXTERNAL_ALLOCATION_PARAMS_V550. */
#define UVM_MAX_GPUS_V2 (32 * 8)

struct uvm_map_external_allocation_params {
	__u64 base;
	__u64 length;
	__u64 offset;
	struct uvm_gpu_mapping_attributes per_gpu_attributes[UVM_MAX_GPUS_V2];
	__u64 gpu_attributes_count;
	__s32 rm_ctrl_fd;
	nvhandle_t h_client;
	nvhandle_t h_memory;
	__u32 rm_status;
};

struct uvm_free_params {
	__u64 base;
	__u64 length;
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_migrate_params {
	__u64 base;
	__u64 length;
	struct uvm_uuid dst_gpu_uuid;    /* zero → CPU                        */
	__u32 migrate_flags;
	nvhandle_t user_space_cpp_fd;
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_set_preferred_location_params {
	__u64 requested_base;
	__u64 length;
	struct uvm_uuid preferred_location_uuid;
	__s32 preferred_cpu_nid;
	__u32 rm_status;
};

struct uvm_unset_preferred_location_params {
	__u64 base;
	__u64 length;
	__u32 rm_status;
	__u32 reserved;
};

/*
 * UVM_ENABLE_READ_DUPLICATION (44) / UVM_DISABLE_READ_DUPLICATION (45).
 *
 * Layout from open-gpu-kernel-modules 580.105.08,
 * kernel-open/nvidia-uvm/uvm_ioctl.h:636-655 -- two NV_ALIGN_BYTES(8) NvU64s
 * and an NV_STATUS, i.e. 20 bytes, padded to 24.  libcuda issues 45 on a
 * freshly created managed range, so without these the guest's size table
 * refused it ("unknown ioctl cmd=0x2d") and cuMemAllocManaged failed --
 * the same shape of gap that UVM_UNMAP_EXTERNAL and
 * UVM_MAP_DYNAMIC_PARALLELISM_REGION had before them.
 */
struct uvm_enable_read_duplication_params {
	__u64 requested_base;
	__u64 length;
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_disable_read_duplication_params {
	__u64 requested_base;
	__u64 length;
	__u32 rm_status;
	__u32 reserved;
};

/*
 * UVM_MIGRATE_RANGE_GROUP (53).  uvm_ioctl.h:787-793: an NvU64 range-group id,
 * an NvProcessorUuid (16 bytes) and an NV_STATUS.
 */
struct uvm_migrate_range_group_params {
	__u64 range_group_id;
	struct uvm_uuid destination_uuid;
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_set_accessed_by_params {
	__u64 base;
	__u64 length;
	struct uvm_uuid accessor_uuid;
	__s32 accessor_cpu_nid;
	__u32 rm_status;
};

struct uvm_unset_accessed_by_params {
	__u64 base;
	__u64 length;
	struct uvm_uuid accessor_uuid;
	__s32 accessor_cpu_nid;
	__u32 rm_status;
};

struct uvm_enable_peer_access_params {
	struct uvm_uuid gpu_uuid_1;
	struct uvm_uuid gpu_uuid_2;
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_disable_peer_access_params {
	struct uvm_uuid gpu_uuid_1;
	struct uvm_uuid gpu_uuid_2;
	__u32 rm_status;
	__u32 reserved;
};

/*
 * UVM_MAP_DYNAMIC_PARALLELISM_REGION (65).
 *
 * libcuda calls this during context creation on an A100 (GA100).  It was not
 * handled at all, so nvkvm_ioctl_param_size() fell through to the FRONTEND NR
 * switch, where 65 == 0x41 == NV_ESC_RM_IDLE_CHANNELS -- the exact bare-UVM /
 * frontend NR collision the sanitizer's type gate exists to prevent.  Before
 * that gate existed the ioctl was silently forwarded as the wrong call
 * entirely; with the gate it correctly returns -ENOTTY, which is what exposed
 * the missing entry.
 *
 * Layout follows the same convention as the other GPU-scoped UVM params in
 * this header: {base, length, gpu_uuid, rm_status}, tail-padded to 8.
 */
struct uvm_map_dynamic_parallelism_region_params {
	__u64 base;
	__u64 length;
	struct uvm_uuid gpu_uuid;
	__u32 rm_status;
	__u32 reserved;
};

/*
 * UVM_UNMAP_EXTERNAL (66).
 *
 * The same shape of gap as UVM_MAP_DYNAMIC_PARALLELISM_REGION (65) above, one
 * NR over, and the tree was already ASYMMETRIC about it: QEMU carries the
 * host-side entry (src/qemu/nvkvm_isolate_handlers.c, NR 66,
 * NVKVM_UVM_VA_USE) and the guest treats it as a VALIDATE-cache invalidator
 * (src/guest/nvkvm_main.c, "Any UVM range teardown invalidates the VALIDATE
 * cache" (#94)) -- but nvkvm_ioctl_param_size() had no case for it, so the
 * size table was the one site that did not know the ioctl existed.
 *
 * How it presented: OBSERVED on an RTX PRO 6000 Blackwell (GB202, sm_120) on
 * 2026-08-23, as
 *     nvkvm: AUDIT unknown ioctl cmd=0x42 type=0x0 nr=0x42 iocsz=0
 * in the guest log, four times per run, on every driver tested (580.95.05,
 * 590.48.01, 610.57.04).  It FAILED SAFE rather than misrouting: with no size
 * entry the call reaches the sanitizer's _IOC_TYPE != 'F' gate and is refused,
 * which is exactly the outcome that gate was added for after the 65 bug.  So
 * nothing was corrupted (28/28 throughout) -- the UVM unmap simply never
 * reached the host.
 *
 * MEASURED, not derived: sizeof(UVM_UNMAP_EXTERNAL_PARAMS) == 40 with
 * base=0, length=8, gpuUuid=16, rmStatus=32, from a compiled offsetof probe
 * against open-gpu-kernel-modules at BOTH tag 580.95.05 and tag 610.57.04
 * (kernel-open/nvidia-uvm/uvm_ioctl.h:935-942 in each).  The two are
 * byte-identical, so this layout spans the 580 and 610 ABI profiles and does
 * not need a per-profile value.
 */
struct uvm_unmap_external_params {
	__u64 base;
	__u64 length;
	struct uvm_uuid gpu_uuid;
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_create_external_range_params {
	__u64 base;
	__u64 length;
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_validate_va_range_params {
	__u64 base;
	__u64 length;
	__u32 rm_status;
	__u32 reserved;
};

struct uvm_pageable_mem_access_params {
	__u8  pageable_access_supported;
	__u8  reserved[3];
	__u32 rm_status;
};

/* UVM_PAGEABLE_MEM_ACCESS_ON_GPU (cmd 70) — libcuda calls this right after
 * UVM_REGISTER_GPU; if the guest module returns -ENOTTY libcuda treats the
 * device as broken and gives up cuCtxCreate. */
struct uvm_pageable_mem_access_on_gpu_params {
	__u8  gpu_uuid[16];        /* IN  NvProcessorUuid */
	__u8  pageable_mem_access; /* OUT NvBool */
	__u8  reserved[3];
	__u32 rm_status;           /* OUT */
};

/* UVM_ALLOC_SEMAPHORE_POOL_PARAMS — real size is 9248 bytes.  The
 * earlier definition missed the per_gpu_attributes[UVM_MAX_GPUS_V2]
 * array (256 entries * 36 bytes = 9216 bytes), so our forwarding sent
 * a 32-byte buffer to the kernel.  The kernel reads sizeof(struct) =
 * 9248 bytes from that buffer, runs past the page, returns -EFAULT.
 * Diagnosed 2026-05-28 in the downstream cuCtxCreate=1 chain after
 * the DMA-COPY engineType fix. */
struct uvm_alloc_semaphore_pool_params {
	__u64 base;
	__u64 length;
	struct uvm_gpu_mapping_attributes per_gpu_attributes[UVM_MAX_GPUS_V2];
	__u64 gpu_attributes_count;
	__u32 rm_status;
	__u32 _pad0;
};

#endif /* NVKVM_UVM_H */
