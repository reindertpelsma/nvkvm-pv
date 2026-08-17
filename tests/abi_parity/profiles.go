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
