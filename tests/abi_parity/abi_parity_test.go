// abi_parity_test.go — ABI struct size parity tests
//
// Verifies that the C structs in src/abi/ match the sizes expected by the
// NVIDIA driver. These are the same checks gVisor does in
// pkg/sentry/devices/nvproxy/nvproxy_driver_parity_test.go and
// pkg/sentry/devices/nvproxy/nvproxy_test.go, but expressed for our
// C header definitions via cgo.
//
// Adapted from gVisor's nvproxy tests (copyright 2024 The gVisor Authors,
// Apache-2.0). See CREDITS for full attribution.
//
// Run:
//   cd tests/abi_parity
//   go test -v ./...

package abi_parity

import (
	"testing"
)

// sizeCase describes a struct whose size must match a known constant.
// Known sizes are taken from gVisor's nvgpu package and open-gpu-kernel-modules
// headers as of driver version 535.
type sizeCase struct {
	name     string
	gotSize  uintptr
	wantSize uintptr
}

func checkSizes(t *testing.T, cases []sizeCase) {
	t.Helper()
	for _, tc := range cases {
		if tc.gotSize != tc.wantSize {
			t.Errorf("%-45s: size=%d, want=%d (delta=%+d)",
				tc.name, tc.gotSize, tc.wantSize,
				int(tc.gotSize)-int(tc.wantSize))
		}
	}
}

// TestFrontendStructSizes verifies that our C struct definitions for the
// NVIDIA frontend ioctl parameter structs are the correct size.
//
// These sizes have been stable since ~R525. If they drift, ioctl forwarding
// will silently pass the wrong amount of data to the host driver.
//
// Sizes sourced from:
//   - gVisor pkg/abi/nvgpu/frontend.go (SizeBytes() calls)
//   - open-gpu-kernel-modules src/common/sdk/nvidia/inc/nvos.h
func TestFrontendStructSizes(t *testing.T) {
	checkSizes(t, []sizeCase{
		// Core RM parameter structs
		{"nvos21_parameters (RM_ALLOC v1)", Sizes.Nvos21, 32},
		{"nvos64_parameters (RM_ALLOC v2)", Sizes.Nvos64, 48},
		{"nvos00_parameters (RM_FREE)", Sizes.Nvos00, 16},
		{"nvos54_parameters (RM_CONTROL)", Sizes.Nvos54, 32},
		{"nvos55_parameters (RM_DUP_OBJECT)", Sizes.Nvos55, 28}, // 575 SDK: 7 fields
		{"nvos57_parameters (RM_SHARE)", Sizes.Nvos57, 16},
		// nvos32: real ABI is 184 (prefix + union). The earlier 88-byte def
		// truncated the AllocSize union (size/offset/address at +88), so the
		// legacy graphics allocation path (libGLX) got size=0 / no address and
		// bailed. status is at +20.
		{"nvos32_parameters (RM_VID_HEAP_CONTROL)", Sizes.Nvos32, 184},
		// nvos46_parameters: must be 56 — includes DmaOffset (out) at +40
		// and Status at +48. Earlier 40-byte def truncated the writeback,
		// so libcuda saw status=0 from a different field and OBJECT_NOT_FOUND
		// silently slipped through.
		{"nvos46_parameters (RM_MAP_MEMORY_DMA)", Sizes.Nvos46, 56},
		// nvos47_parameters: 48 bytes — flags @+16, dmaOffset @+24, size @+32,
		// status @+40. Earlier 20-byte def truncated the writeback and
		// libcuda read uninitialized stack as the unmap status, often
		// triggering CONTEXT_IS_DESTROYED on subsequent operations.
		{"nvos47_parameters (RM_UNMAP_MEMORY_DMA)", Sizes.Nvos47, 48},

		// Card info and system
		{"nv_ioctl_card_info", Sizes.CardInfo, 80},
		{"nv_ioctl_register_fd", Sizes.RegFd, 4},
		{"nv_ioctl_alloc_os_event", Sizes.AllocOsEv, 16},
		{"nv_ioctl_free_os_event", Sizes.FreeOsEv, 16},
		{"nv_ioctl_rm_api_version", Sizes.RmApiVer, 72},
		{"nv_ioctl_sys_params", Sizes.SysParams, 8},
		{"nv_ioctl_numa_info", Sizes.NumaInfo, 32},
		{"nv_ioctl_wait_open_complete", Sizes.WaitOpen, 8},

		// Memory management
		{"nv_ioctl_nvos33_parameters_with_fd (MAP_MEMORY)", Sizes.MapMemFd, 56},
		{"nv_ioctl_nvos34_parameters (UNMAP_MEMORY)", Sizes.UnmapMem, 32},
		// nv_ioctl_nvos02_parameters_with_fd: must be 56 — NVOS02_PARAMETERS
		// is 48 bytes (with Status at +40, Pad1 at +44), then FD (+48) and
		// Pad0 (+52). Earlier 48-byte def put FD at +44, overlapping Pad1
		// and reading kernel zero as the user's fd field.
		{"nv_ioctl_nvos02_parameters_with_fd (ALLOC_MEMORY)", Sizes.AllocMemFd, 56},
		// 56, not 40: the three NvP64 arrays end at +40, but flags/timeout/
		// status follow (+52) and the NvP64 members force align-to-8 → 56.
		// Confirmed against OGKM 580.159.04 nvos.h NVOS30_PARAMETERS and
		// gvisor nvproxy frontend.go (explicit Pad0 [4]byte).
		{"nv_ioctl_idle_channels", Sizes.IdleCh, 56},
		{"nv_ioctl_alloc_context_dma2", Sizes.AllocCtx, 56},
		{"nv_ioctl_export_to_dmabuf_fd", Sizes.ExportDma, 40},
	})
}

// TestAllocParamStructSizes verifies the per-hClass alloc-param structs
// embedded in nvos21/nvos64. The driver sizes the buffer it expects by
// hClass; if our size table is wrong, RM_ALLOC returns NV_ERR_INVALID_ARGUMENT
// (this was the cuInit blocker for class 0xDE prior to its addition).
func TestAllocParamStructSizes(t *testing.T) {
	checkSizes(t, []sizeCase{
		{"nv0080_alloc_parameters (NV01_DEVICE_0)", Sizes.Nv0080, 56},
		{"nv2080_alloc_parameters (NV20_SUBDEVICE_0)", Sizes.Nv2080, 4},
		{"nv00de_alloc_parameters_v545 (RM_USER_SHARED_DATA)", Sizes.Nv00deV545, 8},
		{"nv_vaspace_allocation_parameters (FERMI_VASPACE_A)", Sizes.NvVaspace, 48},
		{"nv_memory_allocation_params_v545 (NV50_MEMORY_VIRTUAL)", Sizes.NvMemAlloc, 128},
	})
}

// TestUVMStructSizes verifies that UVM ioctl parameter structs are correctly
// sized. These are more variable across driver versions than frontend structs.
func TestUVMStructSizes(t *testing.T) {
	checkSizes(t, []sizeCase{
		{"uvm_initialize_params", Sizes.UvmInit, 16},
		{"uvm_deinitialize_params", Sizes.UvmDeinit, 8},
		{"uvm_mm_initialize_params", Sizes.UvmMmInit, 8},
		{"uvm_register_gpu_params", Sizes.UvmRegGpu, 32},
		{"uvm_unregister_gpu_params", Sizes.UvmUnregGpu, 24},
		{"uvm_register_gpu_vaspace_params", Sizes.UvmRegGv, 32},
		{"uvm_unregister_gpu_vaspace_params", Sizes.UvmUnregGv, 20},
		{"uvm_register_channel_params", Sizes.UvmRegCh, 48},
		{"uvm_unregister_channel_params", Sizes.UvmUnregCh, 28}, // GPUUUID+HClient+HChannel+RMStatus (gvisor/575)
		{"uvm_create_range_group_params", Sizes.UvmCreateRg, 16},
		{"uvm_destroy_range_group_params", Sizes.UvmDestRg, 16},
		{"uvm_set_range_group_params", Sizes.UvmSetRg, 32},
		{"uvm_free_params", Sizes.UvmFree, 24},
		{"uvm_migrate_params", Sizes.UvmMigrate, 48},
		{"uvm_set_preferred_location_params", Sizes.UvmSetPref, 40},
		{"uvm_unset_preferred_location_params", Sizes.UvmUnsetPref, 24},
		{"uvm_set_accessed_by_params", Sizes.UvmSetAb, 40},
		{"uvm_unset_accessed_by_params", Sizes.UvmUnsetAb, 40},
		{"uvm_enable_peer_access_params", Sizes.UvmPeer, 40},
		{"uvm_create_external_range_params", Sizes.UvmExtRng, 24},
		{"uvm_validate_va_range_params", Sizes.UvmValVa, 24},
		{"uvm_pageable_mem_access_params", Sizes.UvmPageable, 8},
		{"uvm_alloc_semaphore_pool_params", Sizes.UvmSema, 9248}, // base+len+PerGPUAttributes[UVM_MAX_GPUS]+count+status
	})
}

// TestProtocolStructSizes verifies the wire protocol structs are the expected
// size. These are critical for the guest/host ABI — both sides must agree.
func TestProtocolStructSizes(t *testing.T) {
	checkSizes(t, []sizeCase{
		{"nvkvm_hdr (must be exactly 8)", Sizes.NvkvmHdr, 8},
		{"nvkvm_req_open", Sizes.NvkvmReqOpen, 16},
		{"nvkvm_req_close", Sizes.NvkvmReqClose, 8},
		{"nvkvm_req_ioctl (must be exactly 32)", Sizes.NvkvmReqIoctl, 32},
		{"nvkvm_req_mmap", Sizes.NvkvmReqMmap, 32},
		{"nvkvm_req_munmap", Sizes.NvkvmReqMunmap, 8},
		{"nvkvm_resp_open", Sizes.NvkvmRespOpen, 8},
		{"nvkvm_resp_ioctl", Sizes.NvkvmRespIoctl, 16},
		{"nvkvm_resp_mmap", Sizes.NvkvmRespMmap, 24},
		{"nvkvm_shm_ctrl (must fit in one page)", Sizes.NvkvmShmCtrl, 80},
	})
}

// TestIoctlNumberConstants verifies that our ioctl number constants are
// consistent with each other (they must all fit in the expected bit ranges).
func TestIoctlNumberConstants(t *testing.T) {
	cases := []struct {
		name string
		nr   int
	}{
		{"NV_ESC_CARD_INFO",          NvEscNums.CardInfo},
		{"NV_ESC_REGISTER_FD",        NvEscNums.RegisterFd},
		{"NV_ESC_ALLOC_OS_EVENT",     NvEscNums.AllocOsEvent},
		{"NV_ESC_FREE_OS_EVENT",      NvEscNums.FreeOsEvent},
		{"NV_ESC_CHECK_VERSION_STR",  NvEscNums.CheckVersionStr},
		{"NV_ESC_SYS_PARAMS",         NvEscNums.SysParams},
		{"NV_ESC_NUMA_INFO",          NvEscNums.NumaInfo},
		{"NV_ESC_WAIT_OPEN_COMPLETE", NvEscNums.WaitOpenComplete},
		{"NV_ESC_RM_FREE",            NvEscNums.RmFree},
		{"NV_ESC_RM_CONTROL",         NvEscNums.RmControl},
		{"NV_ESC_RM_ALLOC",           NvEscNums.RmAlloc},
		{"NV_ESC_RM_DUP_OBJECT",      NvEscNums.RmDupObject},
		{"NV_ESC_RM_MAP_MEMORY",      NvEscNums.RmMapMemory},
		{"NV_ESC_RM_UNMAP_MEMORY",    NvEscNums.RmUnmapMemory},
	}

	for _, tc := range cases {
		if tc.nr <= 0 || tc.nr > 0xFF {
			t.Errorf("%-30s: ioctl NR %d out of [1,255] range", tc.name, tc.nr)
		}
	}
}

// TestUVMIoctlNumbersDistinct verifies that no two UVM command numbers collide.
func TestUVMIoctlNumbersDistinct(t *testing.T) {
	cmds := map[uint32]string{
		UvmCmds.Initialize:           "UVM_INITIALIZE",
		UvmCmds.Deinitialize:         "UVM_DEINITIALIZE",
		UvmCmds.CreateRangeGroup:     "UVM_CREATE_RANGE_GROUP",
		UvmCmds.DestroyRangeGroup:    "UVM_DESTROY_RANGE_GROUP",
		UvmCmds.RegisterGpuVaspace:   "UVM_REGISTER_GPU_VASPACE",
		UvmCmds.UnregisterGpuVaspace: "UVM_UNREGISTER_GPU_VASPACE",
		UvmCmds.RegisterChannel:      "UVM_REGISTER_CHANNEL",
		UvmCmds.UnregisterChannel:    "UVM_UNREGISTER_CHANNEL",
		UvmCmds.RegisterGpu:          "UVM_REGISTER_GPU",
		UvmCmds.UnregisterGpu:        "UVM_UNREGISTER_GPU",
		UvmCmds.Free:                 "UVM_FREE",
		UvmCmds.Migrate:              "UVM_MIGRATE",
		UvmCmds.MmInitialize:         "UVM_MM_INITIALIZE",
	}
	// If the map was constructed without collision the count is correct
	if len(cmds) != 13 {
		t.Errorf("UVM command number collision detected: map has %d entries, expected 13", len(cmds))
	}
}
