/*
 * unit_stubs.c — symbols the unit tests need that live in virtio_nvgpu.c.
 *
 * virtio_nvgpu.c is the QEMU device model and cannot be linked into a plain
 * host test binary, so the handful of globals it owns are defined here
 * instead.  Keep this file to definitions only: anything with behaviour
 * belongs in the test that depends on it.
 */

/* Gate behind NVKVM_DEBUG in the real build; always quiet under test. */
int nvkvm_debug_enabled;

/*
 * Backend entry points nvkvm_isolate.c calls but test_isolate.c does not
 * exercise.  They live in the QEMU device model / the GPA-window allocator,
 * neither of which links into a host test binary.
 */
#include <stddef.h>
#include <stdint.h>
#include "../../src/qemu/virtio_nvgpu.h"

void nvkvm_virtio_push_evt(VirtIONvgpu *nv, uint32_t isolate_id,
                           uint32_t handle_id, uint32_t revents)
{
	(void)nv; (void)isolate_id; (void)handle_id; (void)revents;
}

void nvkvm_sparse_gpa_free(VirtIONvgpu *nv, uint64_t gpa, size_t size)
{
	(void)nv; (void)gpa; (void)size;
}

int nvkvm_window_restore_anon(void *qva, size_t len)
{
	(void)qva; (void)len;
	return 0;
}

uint64_t nvkvm_sparse_gpa_alloc(VirtIONvgpu *nv, size_t size)
{
	(void)nv; (void)size;
	return 0;   /* 0 == allocation failed, which callers already handle */
}

void *nvkvm_gpa_to_vmm_va(VirtIONvgpu *nv, uint64_t gpa, size_t size)
{
	(void)nv; (void)gpa; (void)size;
	return NULL;
}

/*
 * The isolate teardown path retires any frame a dying isolate still had in
 * flight (S-4).  The unit build links nvkvm_isolate.c without the EGL present
 * path, so stub it -- there is no display here to forget anything from.
 */
void nvkvm_present_forget_isolate(VirtIONvgpu *nv, uint32_t isolate_id)
{
	(void)nv; (void)isolate_id;
}
