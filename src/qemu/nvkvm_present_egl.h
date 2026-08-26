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
/* Re-point the scanout console's "device" link at `dev`.
 *
 * graphic_console_init() binds the console to the device that registered it —
 * for us the inner virtio device, which carries no user-visible id.  But
 * `-device virtio-nvgpu-pci-...,id=X` puts X on the PCI PROXY, and QEMU's
 * screendump resolves X to the proxy and then asks which console is bound to
 * it.  Without this the answer is "none" and screendump cannot name our
 * console at all.  hw/display/virtio-gpu-pci.c does exactly the same re-point
 * for the same reason; this is that idiom, not a workaround. */
void nvkvm_present_console_set_device(struct VirtIONvgpu *nv,
                                      struct DeviceState *dev);
/* Hand a freshly exported scanout dma-buf to the console.  TAKES OWNERSHIP of
 * `dmabuf_fd` (closes it when the frame is retired).  Returns true if the fd
 * was accepted (caller must not close it), false if the console is inactive
 * (caller still owns the fd). */
/*
 * Cross-vendor present.  Same frame, but read back through the guest's GPU into
 * a LINEAR udmabuf and submitted to the display broker, for hosts whose
 * compositor cannot import the guest's tiling.  Returns false if the frame was
 * not taken (no udmabuf, no present context) and the caller still owns the fd.
 */
bool nvkvm_present_submit_readback(struct VirtIONvgpu *nv, int dmabuf_fd,
                                   uint32_t owner_isolate_id, uint32_t buf_key,
                                   uint32_t width, uint32_t height,
                                   uint32_t stride, uint32_t fourcc,
                                   uint64_t modifier);

bool nvkvm_present_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                          uint32_t owner_isolate_id, uint32_t buf_key,
                          uint32_t width, uint32_t height, uint32_t stride,
                          uint32_t fourcc, uint64_t modifier);

/* S-4: forget every cached import belonging to a dead isolate.  Safe to call
 * from a virtio worker thread: it only records the request, and the drop
 * itself happens on the main loop where the GL context lives.  No-op in the
 * compute-only build. */
void nvkvm_present_forget_isolate(struct VirtIONvgpu *nv,
                                  uint32_t isolate_id);

#endif /* NVKVM_PRESENT_EGL_H */
