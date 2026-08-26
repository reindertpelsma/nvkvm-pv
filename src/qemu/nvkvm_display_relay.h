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
 * TAKES OWNERSHIP of `dmabuf_fd` when it returns true.  The newest descriptor
 * remains open in QEMU until a later frame replaces it, so a reconnected broker
 * can be painted without waiting for another guest flip.  This remains true
 * while the broker socket is down or handshaking: broker mode consumes and
 * retains the newest frame instead of falling through to the GL display path.
 * Returns false only when broker mode is not in use or the fd is invalid; the
 * caller then retains ownership and may use the ordinary present path.
 *
 * Must be called with the BQL held.  The relay socket, its fd-handler
 * registration, and all connection state are deliberately main-loop-owned.
 * A future worker offload must marshal the complete submission back to that
 * owner rather than calling this function directly.  Sends are nonblocking, so
 * a slow or wedged broker costs a dropped frame rather than a stalled VM.
 */
/*
 * Can the broker's display show buffers of this (fourcc, modifier)?
 *   -1 not answered yet, 0 no -- read back instead, 1 yes -- zero-copy.
 *
 * The question is asked automatically on the first submit of each distinct
 * pair; this only reads the answer.  Cleared on disconnect, because a
 * reconnected broker may be a different display.
 */
int nvkvm_display_relay_format_verdict(uint32_t fourcc, uint64_t modifier);

bool nvkvm_display_relay_submit_flags(struct VirtIONvgpu *nv, int dmabuf_fd,
                                      uint32_t width, uint32_t height,
                                      uint32_t stride, uint32_t fourcc,
                                      uint64_t modifier, bool shm);

bool nvkvm_display_relay_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                                uint32_t width, uint32_t height,
                                uint32_t stride, uint32_t fourcc,
                                uint64_t modifier);

/* True whenever -display nvkvm-broker is in use, including reconnect windows.
 * Cheap and immutable after display initialisation; only for choosing the
 * broker present path. */
bool nvkvm_display_relay_active(void);

#endif /* NVKVM_DISPLAY_RELAY_H */
