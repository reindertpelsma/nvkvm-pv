// profiles.go — cgo shim exposing src/common/nvkvm_abi.h's version->profile
// selection to the Go tests.  As with sizes.go, cgo cannot live in a _test.go
// file, so the bridge goes here.

package abi_parity

// #cgo CFLAGS: -I../../src
//
// #include "common/nvkvm_abi.h"
// #include <stdlib.h>
//
// // Fill a 9-slot array in the same field order the test uses.
// static void profile_for_version(const char *vs, unsigned *out) {
//     const struct nvkvm_abi_profile *p = nvkvm_abi_for_version(vs);
//     out[0] = p->uvm_map_ext_size;
//     out[1] = p->uvm_map_ext_fd_off;
//     out[2] = p->uvm_sem_pool_size;
//     out[3] = p->chan_alloc_size;
//     out[4] = p->vaspace_alloc_size;
//     out[5] = p->mem_alloc_size;
//     out[6] = p->nv00de_alloc_size;
//     out[7] = p->nvos46_size;
//     out[8] = p->nvos46_status_off;
// }
//
// static unsigned profile_id_for_version(const char *vs) {
//     return nvkvm_abi_for_version(vs)->id;
// }
//
// // The version-INVARIANT UVM_REGISTER_GPU wire constants.  These are the
// // numbers the QEMU/guest/stub code actually uses on the wire; they are not
// // part of struct nvkvm_abi_profile precisely because the OGKM sweep found no
// // version boundary in them.  Exposed here so a test can hold them against the
// // committed measurement fixture rather than against our own src/abi/uvm.h.
// static unsigned c_reg_gpu_wire_size(void)   { return NVKVM_UVM_REGISTER_GPU_SIZE; }
// static unsigned c_reg_gpu_wire_fd_off(void) { return NVKVM_UVM_REGISTER_GPU_FD_OFF; }
// static unsigned c_reg_gpu_wire_st_off(void) { return NVKVM_UVM_REGISTER_GPU_STATUS_OFF; }
import "C"

import "unsafe"

// ProfileForVersion returns the nine profile fields nvkvm would use for a host
// driver reporting the given version string, in the order named by fieldNames.
func ProfileForVersion(version string) [9]uint32 {
	cs := C.CString(version)
	defer C.free(unsafe.Pointer(cs))

	var buf [9]C.uint
	C.profile_for_version(cs, &buf[0])

	var out [9]uint32
	for i := range buf {
		out[i] = uint32(buf[i])
	}
	return out
}

// ProfileIDForVersion returns the nvkvm_abi_id selected for a version string.
func ProfileIDForVersion(version string) uint32 {
	cs := C.CString(version)
	defer C.free(unsafe.Pointer(cs))
	return uint32(C.profile_id_for_version(cs))
}

// RegisterGPUWire exposes the NVKVM_UVM_REGISTER_GPU_* constants from
// src/common/nvkvm_abi.h -- the values QEMU, the guest module and the stub all
// put on the wire for UVM_REGISTER_GPU.  TestUVMRegisterGPUWireConstants holds
// these against ogkm_register_gpu.tsv, the committed sweep of NVIDIA's own
// headers, so the assertion is conformance to the driver rather than agreement
// with another file in this repo.
var RegisterGPUWire = struct {
	Size      uint32
	FdOff     uint32
	StatusOff uint32
}{
	Size:      uint32(C.c_reg_gpu_wire_size()),
	FdOff:     uint32(C.c_reg_gpu_wire_fd_off()),
	StatusOff: uint32(C.c_reg_gpu_wire_st_off()),
}
