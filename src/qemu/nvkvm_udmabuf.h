/*
 * nvkvm_udmabuf.h — host-memory buffers that are ALSO dma-bufs.
 *
 * Why this exists: on a cross-vendor host (guest renders on NVIDIA, the
 * compositor scans out on an Intel/AMD iGPU) the guest's block-linear buffer
 * cannot be imported by the display GPU, and NVIDIA will not produce a LINEAR
 * one.  MEASURED on an RTX 4070 + driver 595.84: eglCreateImageKHR SUCCEEDS for
 * a LINEAR dma-buf (the import is lazy) but glEGLImageTargetTexture2DOES then
 * fails with GL_INVALID_OPERATION -- for a LINEAR buffer allocated by NVIDIA's
 * own GBM *and* for one allocated by the AMD iGPU.  A driver-chosen
 * block-linear buffer binds fine and is FBO-complete on the same code path, so
 * that is the driver refusing LINEAR, not a harness bug.
 *
 * NVIDIA has never refused a POINTER, though.  udmabuf lets one sealed memfd be
 * reached both ways:
 *     NVIDIA  -> glReadPixels into its mmap   (plain host memory, no dma-buf)
 *     the iGPU-> imports it as LINEAR dma-buf (zero copy)
 *
 * MEASURED end to end on the same box: the 4070 wrote 0xff407fbf into the mmap
 * (over a 0xAB poison) and the Raphael iGPU read back exactly 0xff407fbf
 * through the dma-buf.  One GPU transfer, no CPU copy, no compositor upload.
 *
 * /dev/udmabuf is root:kvm 0660 -- QEMU is already in that group for /dev/kvm,
 * so this needs no privilege it does not already hold.  QEMU also already
 * depends on udmabuf for virtio-gpu blob resources.
 */
#ifndef NVKVM_UDMABUF_H
#define NVKVM_UDMABUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nvkvm_udmabuf {
    void   *ptr;      /* mmap of the memfd: what the GPU reads back into  */
    size_t  size;     /* page-rounded                                     */
    int     memfd;    /* sealed; kept open because udmabuf pins it        */
    int     dmabuf;   /* what the broker/compositor imports (LINEAR)      */
};

/*
 * True when /dev/udmabuf can actually be opened.  Checked ONCE and cached, and
 * deliberately called at init rather than on the first frame: "the broker path
 * needs /dev/udmabuf" is a sentence someone can act on at startup, and a
 * mystery at frame 900.
 */
bool nvkvm_udmabuf_available(void);

/* Allocate `size` bytes reachable as both a pointer and a dma-buf.  Returns
 * false and leaves *out zeroed on failure; never partially initialised. */
bool nvkvm_udmabuf_alloc(struct nvkvm_udmabuf *out, size_t size);

/* Idempotent; safe on a zeroed struct. */
void nvkvm_udmabuf_free(struct nvkvm_udmabuf *b);

#endif /* NVKVM_UDMABUF_H */
