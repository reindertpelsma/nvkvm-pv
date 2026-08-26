/* nvkvm_udmabuf.c — see nvkvm_udmabuf.h for why this exists. */
#define _GNU_SOURCE
#include "nvkvm_udmabuf.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/udmabuf.h>

/* Genuine errors go straight to stderr and are always visible -- the same rule
 * nvkvm_log.h states for DENY decisions.  Deliberately NOT QEMU's
 * warn_report(), so this file stays buildable in the standalone unit tests. */
#define UDMABUF_WARN(...) \
    do { fprintf(stderr, "nvkvm present: " __VA_ARGS__); fputc('\n', stderr); } while (0)

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS  1033
#define F_SEAL_SHRINK 0x0002
#endif

static int nvkvm_udmabuf_dev(void)
{
    static int fd = -2;               /* -2 = not tried, -1 = unavailable */

    if (fd == -2) {
        fd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            UDMABUF_WARN("/dev/udmabuf: %s -- the zero-copy cross-vendor path is "
                         "unavailable; a compositor on a different GPU than the "
                         "guest's will need the copying fallback",
                         strerror(errno));
        }
    }
    return fd;
}

bool nvkvm_udmabuf_available(void)
{
    return nvkvm_udmabuf_dev() >= 0;
}

bool nvkvm_udmabuf_alloc(struct nvkvm_udmabuf *out, size_t size)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    struct udmabuf_create create;
    int dev, mfd = -1, dbuf = -1;
    void *ptr = MAP_FAILED;

    memset(out, 0, sizeof(*out));
    if (size == 0) {
        return false;
    }
    /* UDMABUF_CREATE requires a page-multiple size; round UP so the caller
     * always gets at least what it asked for. */
    size = (size + page - 1) & ~(page - 1);

    dev = nvkvm_udmabuf_dev();
    if (dev < 0) {
        return false;
    }

    mfd = memfd_create("nvkvm-present", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (mfd < 0) {
        UDMABUF_WARN("memfd_create: %s", strerror(errno));
        goto fail;
    }
    if (ftruncate(mfd, (off_t)size) < 0) {
        UDMABUF_WARN("ftruncate(%zu): %s", size, strerror(errno));
        goto fail;
    }
    /* udmabuf REQUIRES F_SEAL_SHRINK: it pins the pages, so the backing store
     * must not be able to shrink underneath the importing device. */
    if (fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
        UDMABUF_WARN("F_SEAL_SHRINK: %s", strerror(errno));
        goto fail;
    }

    memset(&create, 0, sizeof(create));
    create.memfd  = (uint32_t)mfd;
    create.flags  = 0;
    create.offset = 0;
    create.size   = size;
    dbuf = ioctl(dev, UDMABUF_CREATE, &create);
    if (dbuf < 0) {
        UDMABUF_WARN("UDMABUF_CREATE(%zu): %s",
                       size, strerror(errno));
        goto fail;
    }

    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (ptr == MAP_FAILED) {
        UDMABUF_WARN("mmap(%zu): %s", size, strerror(errno));
        goto fail;
    }

    out->ptr    = ptr;
    out->size   = size;
    out->memfd  = mfd;
    out->dmabuf = dbuf;
    return true;

fail:
    if (ptr != MAP_FAILED) {
        munmap(ptr, size);
    }
    if (dbuf >= 0) {
        close(dbuf);
    }
    if (mfd >= 0) {
        close(mfd);
    }
    memset(out, 0, sizeof(*out));
    return false;
}

void nvkvm_udmabuf_free(struct nvkvm_udmabuf *b)
{
    if (!b || !b->size) {
        return;
    }
    if (b->ptr) {
        munmap(b->ptr, b->size);
    }
    if (b->dmabuf >= 0) {
        close(b->dmabuf);
    }
    if (b->memfd >= 0) {
        close(b->memfd);
    }
    memset(b, 0, sizeof(*b));
}
