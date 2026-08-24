/*
 * nvkvm_display_relay.h — the broker-mode display path.
 *
 * DELIBERATELY OUTSIDE THE OpenGL GATE.  nvkvm_present_egl.c is compiled under
 *
 *     #if defined(CONFIG_OPENGL) && NVKVM_QEMU_GRAPHICS
 *
 * and everything in it — including nvkvm_present_submit() — compiles to a stub
 * that returns false when OpenGL is off.  The whole point of broker mode is
 * that QEMU needs no EGL, no GL and no /dev/dri/renderD*, so a QEMU built
 * WITHOUT OpenGL must still be able to display.  If the relay lived in that
 * file, --disable-opengl would compile the display away entirely.
 *
 * So this header and its .c are gated on CONFIG_LINUX alone.
 */
#ifndef NVKVM_DISPLAY_RELAY_H
#define NVKVM_DISPLAY_RELAY_H

#include <stdbool.h>
#include <stdint.h>

struct VirtIONvgpu;

/*
 * Relay one already-exported guest scanout dma-buf to the display broker.
 *
 * TAKES OWNERSHIP of `dmabuf_fd` when it returns true (it closes it once the
 * descriptor has been handed over; the broker holds its own copy from
 * SCM_RIGHTS).  Returns false when broker mode is not in use or the broker is
 * gone, in which case the caller still owns the fd and should fall through to
 * the ordinary present path.
 *
 * Safe to call from a virtio worker thread: the send is serialised by an
 * internal mutex and is non-blocking, so a slow or wedged broker costs a
 * dropped frame rather than a stalled vCPU.
 */
bool nvkvm_display_relay_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                                uint32_t width, uint32_t height,
                                uint32_t stride, uint32_t fourcc,
                                uint64_t modifier);

/* True when -display nvkvm-broker is in use and the socket is up.  Cheap; no
 * lock.  Only for deciding whether the relay is worth attempting. */
bool nvkvm_display_relay_active(void);

#endif /* NVKVM_DISPLAY_RELAY_H */
