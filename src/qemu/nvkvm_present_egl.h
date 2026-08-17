/*
 * nvkvm_present_egl.h — host-side dma-buf capture for the present path (#107).
 */
#ifndef NVKVM_PRESENT_EGL_H
#define NVKVM_PRESENT_EGL_H

#include <stdint.h>

/*
 * Import the dma-buf `dmabuf_fd` (a guest-composited scanout buffer, geometry
 * as given) as a host GL texture, read it back, and write it to `out_path` as a
 * binary PPM.  Does not take ownership of dmabuf_fd (dups internally).
 * Returns 0 on success, -errno otherwise (-ENOTSUP if built without OpenGL or
 * host EGL/dma-buf import is unavailable).
 */
int nvkvm_present_capture(int dmabuf_fd, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t fourcc, uint64_t modifier,
                          const char *out_path);

/*
 * Live QEMU-window present (#102 step 0).  Registers a QemuConsole for the
 * nvkvm device so the guest's GPU-composited scanout can be shown in a real
 * QEMU display (gtk/sdl/vnc) instead of only captured to a file.  The guest's
 * exported scanout dma-buf is pushed INTO the host display; nothing about the
 * host display ever flows back toward the guest (one-way boundary).
 *
 * Two display paths, auto-selected:
 *   - zero-copy GL: when the active UI console has GL and is driven by the same
 *     (NVIDIA) GPU as the buffer, hand the dma-buf straight to the UI's GL
 *     importer (dpy_gl_scanout_dmabuf) — full performance, no readback.
 *   - readback: otherwise import the dma-buf on our private NVIDIA EGL context,
 *     glReadPixels into a CPU DisplaySurface, and push that to QEMU's 2D
 *     display (dpy_gfx_*) — always works regardless of the host's display GPU.
 * Override with NVKVM_PRESENT_MODE=gl|readback.
 *
 * Opaque-typed (void*) so virtio_nvgpu.c need not pull in ui/console.h; these
 * are no-ops in the compute-only (!NVKVM_QEMU_GRAPHICS) build.
 */
struct VirtIONvgpu;
struct DeviceState;
int  nvkvm_present_console_init(struct DeviceState *dev, struct VirtIONvgpu *nv);
void nvkvm_present_console_fini(struct VirtIONvgpu *nv);
/* Hand a freshly exported scanout dma-buf to the console.  TAKES OWNERSHIP of
 * `dmabuf_fd` (closes it when the frame is retired).  Returns true if the fd
 * was accepted (caller must not close it), false if the console is inactive
 * (caller still owns the fd). */
bool nvkvm_present_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                          uint32_t width, uint32_t height, uint32_t stride,
                          uint32_t fourcc, uint64_t modifier);

#endif /* NVKVM_PRESENT_EGL_H */
