// sizes.go — cgo shim that measures C struct sizes at compile time.
// The _test.go file cannot use cgo directly in Go; cgo must be in a
// non-test source file.

package abi_parity

// #cgo CFLAGS: -I../../src
//
// #include "abi/nvgpu.h"
// #include "abi/uvm.h"
// #include "common/nvkvm_proto.h"
// #include <stddef.h>
//
// // RM / frontend structs
// static size_t sz_nvos21(void)  { return sizeof(struct nvos21_parameters); }
// static size_t sz_nvos64(void)  { return sizeof(struct nvos64_parameters); }
// static size_t sz_nvos00(void)  { return sizeof(struct nvos00_parameters); }
// static size_t sz_nvos54(void)  { return sizeof(struct nvos54_parameters); }
// static size_t sz_nvos55(void)  { return sizeof(struct nvos55_parameters); }
// static size_t sz_nvos57(void)  { return sizeof(struct nvos57_parameters); }
// static size_t sz_nvos32(void)  { return sizeof(struct nvos32_parameters); }
// static size_t sz_nvos46(void)  { return sizeof(struct nvos46_parameters); }
// static size_t sz_nvos47(void)  { return sizeof(struct nvos47_parameters); }
//
// // Per-hClass alloc-param structs (RM_ALLOC nested params)
// static size_t sz_nv0080(void)  { return sizeof(struct nv0080_alloc_parameters); }
// static size_t sz_nv2080(void)  { return sizeof(struct nv2080_alloc_parameters); }
// static size_t sz_nv00de_v545(void) { return sizeof(struct nv00de_alloc_parameters_v545); }
// static size_t sz_nv_vaspace(void)  { return sizeof(struct nv_vaspace_allocation_parameters); }
// static size_t sz_nv_memalloc(void) { return sizeof(struct nv_memory_allocation_params_v545); }
// static size_t sz_nv_memalloc_common(void) {
//     return offsetof(struct nv_memory_allocation_params_v545, size) +
//            sizeof(((struct nv_memory_allocation_params_v545 *)0)->size);
// }
//
// // ioctl wrapper structs
// static size_t sz_card_info(void)    { return sizeof(struct nv_ioctl_card_info); }
// static size_t sz_reg_fd(void)       { return sizeof(struct nv_ioctl_register_fd); }
// static size_t sz_alloc_os_ev(void)  { return sizeof(struct nv_ioctl_alloc_os_event); }
// static size_t sz_free_os_ev(void)   { return sizeof(struct nv_ioctl_free_os_event); }
// static size_t sz_rm_api_ver(void)   { return sizeof(struct nv_ioctl_rm_api_version); }
// static size_t sz_sys_params(void)   { return sizeof(struct nv_ioctl_sys_params); }
// static size_t sz_numa_info(void)    { return sizeof(struct nv_ioctl_numa_info); }
// static size_t sz_wait_open(void)    { return sizeof(struct nv_ioctl_wait_open_complete); }
// static size_t sz_map_mem_fd(void)   { return sizeof(struct nv_ioctl_nvos33_parameters_with_fd); }
// static size_t sz_unmap_mem(void)    { return sizeof(struct nv_ioctl_nvos34_parameters); }
// static size_t sz_alloc_mem_fd(void) { return sizeof(struct nv_ioctl_nvos02_parameters_with_fd); }
// static size_t sz_idle_ch(void)      { return sizeof(struct nv_ioctl_idle_channels); }
// static size_t sz_alloc_ctx(void)    { return sizeof(struct nv_ioctl_alloc_context_dma2); }
// static size_t sz_export_dma(void)   { return sizeof(struct nv_ioctl_export_to_dmabuf_fd); }
//
// // UVM structs
// static size_t sz_uvm_init(void)     { return sizeof(struct uvm_initialize_params); }
// static size_t sz_uvm_deinit(void)   { return sizeof(struct uvm_deinitialize_params); }
// static size_t sz_uvm_mm_init(void)  { return sizeof(struct uvm_mm_initialize_params); }
// static size_t sz_uvm_reg_gpu(void)  { return sizeof(struct uvm_register_gpu_params); }
// static size_t off_uvm_reg_gpu_fd(void) { return offsetof(struct uvm_register_gpu_params, rm_ctrl_fd); }
// static size_t off_uvm_reg_gpu_status(void) { return offsetof(struct uvm_register_gpu_params, rm_status); }
// static size_t sz_uvm_unreg_gpu(void){ return sizeof(struct uvm_unregister_gpu_params); }
// static size_t sz_uvm_reg_gv(void)   { return sizeof(struct uvm_register_gpu_vaspace_params); }
// static size_t sz_uvm_unreg_gv(void) { return sizeof(struct uvm_unregister_gpu_vaspace_params); }
// static size_t sz_uvm_reg_ch(void)   { return sizeof(struct uvm_register_channel_params); }
// static size_t sz_uvm_unreg_ch(void) { return sizeof(struct uvm_unregister_channel_params); }
// static size_t sz_uvm_create_rg(void){ return sizeof(struct uvm_create_range_group_params); }
// static size_t sz_uvm_dest_rg(void)  { return sizeof(struct uvm_destroy_range_group_params); }
// static size_t sz_uvm_set_rg(void)   { return sizeof(struct uvm_set_range_group_params); }
// static size_t sz_uvm_free(void)     { return sizeof(struct uvm_free_params); }
// static size_t sz_uvm_migrate(void)  { return sizeof(struct uvm_migrate_params); }
// static size_t sz_uvm_set_pref(void) { return sizeof(struct uvm_set_preferred_location_params); }
// static size_t sz_uvm_unset_pref(void){return sizeof(struct uvm_unset_preferred_location_params);}
// static size_t sz_uvm_set_ab(void)   { return sizeof(struct uvm_set_accessed_by_params); }
// static size_t sz_uvm_unset_ab(void) { return sizeof(struct uvm_unset_accessed_by_params); }
// static size_t sz_uvm_peer(void)     { return sizeof(struct uvm_enable_peer_access_params); }
// static size_t sz_uvm_ext_rng(void)  { return sizeof(struct uvm_create_external_range_params); }
// static size_t sz_uvm_val_va(void)   { return sizeof(struct uvm_validate_va_range_params); }
// static size_t sz_uvm_pageable(void) { return sizeof(struct uvm_pageable_mem_access_params); }
// static size_t sz_uvm_sema(void)     { return sizeof(struct uvm_alloc_semaphore_pool_params); }
//
// // Protocol structs
// static size_t sz_nvkvm_hdr(void)        { return sizeof(struct nvkvm_hdr); }
// static size_t sz_nvkvm_req_open(void)   { return sizeof(struct nvkvm_req_open); }
// static size_t sz_nvkvm_req_close(void)  { return sizeof(struct nvkvm_req_close); }
// static size_t sz_nvkvm_req_ioctl(void)  { return sizeof(struct nvkvm_req_ioctl); }
// static size_t sz_nvkvm_req_mmap(void)   { return sizeof(struct nvkvm_req_mmap); }
// static size_t sz_nvkvm_req_munmap(void) { return sizeof(struct nvkvm_req_munmap); }
// static size_t sz_nvkvm_resp_open(void)  { return sizeof(struct nvkvm_resp_open); }
// static size_t sz_nvkvm_resp_ioctl(void) { return sizeof(struct nvkvm_resp_ioctl); }
// static size_t sz_nvkvm_resp_mmap(void)  { return sizeof(struct nvkvm_resp_mmap); }
// static size_t sz_nvkvm_shm_ctrl(void)   { return sizeof(struct nvkvm_shm_ctrl); }
// static size_t sz_nvkvm_uvm_state(void)  { return sizeof(struct nvkvm_uvm_state_snapshot); }
//
// // NV_ESC ioctl number accessors (macros can't be used directly as C.NV_ESC_x
// // when they involve arithmetic, so wrap them in static functions)
// static int c_nv_esc_card_info(void)          { return NV_ESC_CARD_INFO; }
// static int c_nv_esc_register_fd(void)        { return NV_ESC_REGISTER_FD; }
// static int c_nv_esc_alloc_os_event(void)     { return NV_ESC_ALLOC_OS_EVENT; }
// static int c_nv_esc_free_os_event(void)      { return NV_ESC_FREE_OS_EVENT; }
// static int c_nv_esc_check_version_str(void)  { return NV_ESC_CHECK_VERSION_STR; }
// static int c_nv_esc_sys_params(void)         { return NV_ESC_SYS_PARAMS; }
// static int c_nv_esc_numa_info(void)          { return NV_ESC_NUMA_INFO; }
// static int c_nv_esc_wait_open_complete(void) { return NV_ESC_WAIT_OPEN_COMPLETE; }
// static int c_nv_esc_rm_free(void)            { return NV_ESC_RM_FREE; }
// static int c_nv_esc_rm_control(void)         { return NV_ESC_RM_CONTROL; }
// static int c_nv_esc_rm_alloc(void)           { return NV_ESC_RM_ALLOC; }
// static int c_nv_esc_rm_dup_object(void)      { return NV_ESC_RM_DUP_OBJECT; }
// static int c_nv_esc_rm_map_memory(void)      { return NV_ESC_RM_MAP_MEMORY; }
// static int c_nv_esc_rm_unmap_memory(void)    { return NV_ESC_RM_UNMAP_MEMORY; }
//
// // UVM command number accessors
// static unsigned int c_uvm_initialize(void)             { return UVM_INITIALIZE; }
// static unsigned int c_uvm_deinitialize(void)           { return UVM_DEINITIALIZE; }
// static unsigned int c_uvm_create_range_group(void)     { return UVM_CREATE_RANGE_GROUP; }
// static unsigned int c_uvm_destroy_range_group(void)    { return UVM_DESTROY_RANGE_GROUP; }
// static unsigned int c_uvm_register_gpu_vaspace(void)   { return UVM_REGISTER_GPU_VASPACE; }
// static unsigned int c_uvm_unregister_gpu_vaspace(void) { return UVM_UNREGISTER_GPU_VASPACE; }
// static unsigned int c_uvm_register_channel(void)       { return UVM_REGISTER_CHANNEL; }
// static unsigned int c_uvm_unregister_channel(void)     { return UVM_UNREGISTER_CHANNEL; }
// static unsigned int c_uvm_register_gpu(void)           { return UVM_REGISTER_GPU; }
// static unsigned int c_uvm_unregister_gpu(void)         { return UVM_UNREGISTER_GPU; }
// static unsigned int c_uvm_free(void)                   { return UVM_FREE; }
// static unsigned int c_uvm_migrate(void)                { return UVM_MIGRATE; }
// static unsigned int c_uvm_mm_initialize(void)          { return UVM_MM_INITIALIZE; }
import "C"

// Sizes exposes sizeof for each ABI struct as measured by the C compiler.
var Sizes = struct {
	// RM / frontend structs
	Nvos21 uintptr
	Nvos64 uintptr
	Nvos00 uintptr
	Nvos54 uintptr
	Nvos55 uintptr
	Nvos57 uintptr
	Nvos32 uintptr
	Nvos46 uintptr
	Nvos47 uintptr

	// Per-hClass alloc-param structs
	Nv0080           uintptr
	Nv2080           uintptr
	Nv00deV545       uintptr
	NvVaspace        uintptr
	NvMemAlloc       uintptr
	NvMemAllocCommon uintptr

	// ioctl wrapper structs
	CardInfo   uintptr
	RegFd      uintptr
	AllocOsEv  uintptr
	FreeOsEv   uintptr
	RmApiVer   uintptr
	SysParams  uintptr
	NumaInfo   uintptr
	WaitOpen   uintptr
	MapMemFd   uintptr
	UnmapMem   uintptr
	AllocMemFd uintptr
	IdleCh     uintptr
	AllocCtx   uintptr
	ExportDma  uintptr

	// UVM structs
	UvmInit            uintptr
	UvmDeinit          uintptr
	UvmMmInit          uintptr
	UvmRegGpu          uintptr
	UvmRegGpuFdOff     uintptr
	UvmRegGpuStatusOff uintptr
	UvmUnregGpu        uintptr
	UvmRegGv           uintptr
	UvmUnregGv         uintptr
	UvmRegCh           uintptr
	UvmUnregCh         uintptr
	UvmCreateRg        uintptr
	UvmDestRg          uintptr
	UvmSetRg           uintptr
	UvmFree            uintptr
	UvmMigrate         uintptr
	UvmSetPref         uintptr
	UvmUnsetPref       uintptr
	UvmSetAb           uintptr
	UvmUnsetAb         uintptr
	UvmPeer            uintptr
	UvmExtRng          uintptr
	UvmValVa           uintptr
	UvmPageable        uintptr
	UvmSema            uintptr

	// Protocol structs
	NvkvmHdr       uintptr
	NvkvmReqOpen   uintptr
	NvkvmReqClose  uintptr
	NvkvmReqIoctl  uintptr
	NvkvmReqMmap   uintptr
	NvkvmReqMunmap uintptr
	NvkvmRespOpen  uintptr
	NvkvmRespIoctl uintptr
	NvkvmRespMmap  uintptr
	NvkvmShmCtrl   uintptr
	NvkvmUvmState  uintptr
}{
	Nvos21: uintptr(C.sz_nvos21()),
	Nvos64: uintptr(C.sz_nvos64()),
	Nvos00: uintptr(C.sz_nvos00()),
	Nvos54: uintptr(C.sz_nvos54()),
	Nvos55: uintptr(C.sz_nvos55()),
	Nvos57: uintptr(C.sz_nvos57()),
	Nvos32: uintptr(C.sz_nvos32()),
	Nvos46: uintptr(C.sz_nvos46()),
	Nvos47: uintptr(C.sz_nvos47()),

	Nv0080:           uintptr(C.sz_nv0080()),
	Nv2080:           uintptr(C.sz_nv2080()),
	Nv00deV545:       uintptr(C.sz_nv00de_v545()),
	NvVaspace:        uintptr(C.sz_nv_vaspace()),
	NvMemAlloc:       uintptr(C.sz_nv_memalloc()),
	NvMemAllocCommon: uintptr(C.sz_nv_memalloc_common()),

	CardInfo:   uintptr(C.sz_card_info()),
	RegFd:      uintptr(C.sz_reg_fd()),
	AllocOsEv:  uintptr(C.sz_alloc_os_ev()),
	FreeOsEv:   uintptr(C.sz_free_os_ev()),
	RmApiVer:   uintptr(C.sz_rm_api_ver()),
	SysParams:  uintptr(C.sz_sys_params()),
	NumaInfo:   uintptr(C.sz_numa_info()),
	WaitOpen:   uintptr(C.sz_wait_open()),
	MapMemFd:   uintptr(C.sz_map_mem_fd()),
	UnmapMem:   uintptr(C.sz_unmap_mem()),
	AllocMemFd: uintptr(C.sz_alloc_mem_fd()),
	IdleCh:     uintptr(C.sz_idle_ch()),
	AllocCtx:   uintptr(C.sz_alloc_ctx()),
	ExportDma:  uintptr(C.sz_export_dma()),

	UvmInit:            uintptr(C.sz_uvm_init()),
	UvmDeinit:          uintptr(C.sz_uvm_deinit()),
	UvmMmInit:          uintptr(C.sz_uvm_mm_init()),
	UvmRegGpu:          uintptr(C.sz_uvm_reg_gpu()),
	UvmRegGpuFdOff:     uintptr(C.off_uvm_reg_gpu_fd()),
	UvmRegGpuStatusOff: uintptr(C.off_uvm_reg_gpu_status()),
	UvmUnregGpu:        uintptr(C.sz_uvm_unreg_gpu()),
	UvmRegGv:           uintptr(C.sz_uvm_reg_gv()),
	UvmUnregGv:         uintptr(C.sz_uvm_unreg_gv()),
	UvmRegCh:           uintptr(C.sz_uvm_reg_ch()),
	UvmUnregCh:         uintptr(C.sz_uvm_unreg_ch()),
	UvmCreateRg:        uintptr(C.sz_uvm_create_rg()),
	UvmDestRg:          uintptr(C.sz_uvm_dest_rg()),
	UvmSetRg:           uintptr(C.sz_uvm_set_rg()),
	UvmFree:            uintptr(C.sz_uvm_free()),
	UvmMigrate:         uintptr(C.sz_uvm_migrate()),
	UvmSetPref:         uintptr(C.sz_uvm_set_pref()),
	UvmUnsetPref:       uintptr(C.sz_uvm_unset_pref()),
	UvmSetAb:           uintptr(C.sz_uvm_set_ab()),
	UvmUnsetAb:         uintptr(C.sz_uvm_unset_ab()),
	UvmPeer:            uintptr(C.sz_uvm_peer()),
	UvmExtRng:          uintptr(C.sz_uvm_ext_rng()),
	UvmValVa:           uintptr(C.sz_uvm_val_va()),
	UvmPageable:        uintptr(C.sz_uvm_pageable()),
	UvmSema:            uintptr(C.sz_uvm_sema()),

	NvkvmHdr:       uintptr(C.sz_nvkvm_hdr()),
	NvkvmReqOpen:   uintptr(C.sz_nvkvm_req_open()),
	NvkvmReqClose:  uintptr(C.sz_nvkvm_req_close()),
	NvkvmReqIoctl:  uintptr(C.sz_nvkvm_req_ioctl()),
	NvkvmReqMmap:   uintptr(C.sz_nvkvm_req_mmap()),
	NvkvmReqMunmap: uintptr(C.sz_nvkvm_req_munmap()),
	NvkvmRespOpen:  uintptr(C.sz_nvkvm_resp_open()),
	NvkvmRespIoctl: uintptr(C.sz_nvkvm_resp_ioctl()),
	NvkvmRespMmap:  uintptr(C.sz_nvkvm_resp_mmap()),
	NvkvmShmCtrl:   uintptr(C.sz_nvkvm_shm_ctrl()),
	NvkvmUvmState:  uintptr(C.sz_nvkvm_uvm_state()),
}

// NvEscNums exposes the NV_ESC_* ioctl command numbers as measured by the C
// compiler from nvgpu.h.
var NvEscNums = struct {
	CardInfo         int
	RegisterFd       int
	AllocOsEvent     int
	FreeOsEvent      int
	CheckVersionStr  int
	SysParams        int
	NumaInfo         int
	WaitOpenComplete int
	RmFree           int
	RmControl        int
	RmAlloc          int
	RmDupObject      int
	RmMapMemory      int
	RmUnmapMemory    int
}{
	CardInfo:         int(C.c_nv_esc_card_info()),
	RegisterFd:       int(C.c_nv_esc_register_fd()),
	AllocOsEvent:     int(C.c_nv_esc_alloc_os_event()),
	FreeOsEvent:      int(C.c_nv_esc_free_os_event()),
	CheckVersionStr:  int(C.c_nv_esc_check_version_str()),
	SysParams:        int(C.c_nv_esc_sys_params()),
	NumaInfo:         int(C.c_nv_esc_numa_info()),
	WaitOpenComplete: int(C.c_nv_esc_wait_open_complete()),
	RmFree:           int(C.c_nv_esc_rm_free()),
	RmControl:        int(C.c_nv_esc_rm_control()),
	RmAlloc:          int(C.c_nv_esc_rm_alloc()),
	RmDupObject:      int(C.c_nv_esc_rm_dup_object()),
	RmMapMemory:      int(C.c_nv_esc_rm_map_memory()),
	RmUnmapMemory:    int(C.c_nv_esc_rm_unmap_memory()),
}

// UvmCmds exposes UVM_* command numbers as measured by the C compiler from
// uvm.h.
var UvmCmds = struct {
	Initialize           uint32
	Deinitialize         uint32
	CreateRangeGroup     uint32
	DestroyRangeGroup    uint32
	RegisterGpuVaspace   uint32
	UnregisterGpuVaspace uint32
	RegisterChannel      uint32
	UnregisterChannel    uint32
	RegisterGpu          uint32
	UnregisterGpu        uint32
	Free                 uint32
	Migrate              uint32
	MmInitialize         uint32
}{
	Initialize:           uint32(C.c_uvm_initialize()),
	Deinitialize:         uint32(C.c_uvm_deinitialize()),
	CreateRangeGroup:     uint32(C.c_uvm_create_range_group()),
	DestroyRangeGroup:    uint32(C.c_uvm_destroy_range_group()),
	RegisterGpuVaspace:   uint32(C.c_uvm_register_gpu_vaspace()),
	UnregisterGpuVaspace: uint32(C.c_uvm_unregister_gpu_vaspace()),
	RegisterChannel:      uint32(C.c_uvm_register_channel()),
	UnregisterChannel:    uint32(C.c_uvm_unregister_channel()),
	RegisterGpu:          uint32(C.c_uvm_register_gpu()),
	UnregisterGpu:        uint32(C.c_uvm_unregister_gpu()),
	Free:                 uint32(C.c_uvm_free()),
	Migrate:              uint32(C.c_uvm_migrate()),
	MmInitialize:         uint32(C.c_uvm_mm_initialize()),
}
