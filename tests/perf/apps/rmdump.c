/* rmdump.c — LD_PRELOAD ioctl shim that dumps NVIDIA RM frontend ioctl structs
 * (request + response) so we can byte-compare the dma-buf import path host vs
 * guest (#110 BAD_ALLOC).  Dumps RM_CONTROL (0x2a) / RM_ALLOC (0x2b) NVOS
 * structs and their embedded params buffer, plus other 'F'-type ioctls.
 *
 *   build:  cc -O2 -shared -fPIC -o rmdump.so rmdump.c -ldl
 *   run:    NVKVM_RMDUMP=1 LD_PRELOAD=./rmdump.so ./dmabuf_import_probe ...
 *           (set NVKVM_RMDUMP_CMD=0x<hex> to dump only one RM_CONTROL cmd)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <stdarg.h>

static int (*real_ioctl)(int, unsigned long, ...);
static int enabled;
static unsigned want_cmd = 0xffffffff;  /* dump all RM_CONTROL cmds by default */

__attribute__((constructor)) static void init(void)
{
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    enabled = getenv("NVKVM_RMDUMP") != NULL;
    const char *c = getenv("NVKVM_RMDUMP_CMD");
    if (c) want_cmd = (unsigned)strtoul(c, NULL, 0);
}

static void hexdump(const char *tag, const void *p, size_t n)
{
    if (n > 256) n = 256;
    fprintf(stderr, "    %s[%zu]:", tag, n);
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) fprintf(stderr, " %02x", b[i]);
    fprintf(stderr, "\n");
}

/* NVOS54 (RM_CONTROL): hClient@0 hObject@4 cmd@8 _@12 params@16 paramsSize@24 status@28 */
struct nvos54 { uint32_t hClient, hObject, cmd, flags; uint64_t params; uint32_t paramsSize, status; };
/* NVOS21/64 (RM_ALLOC): hRoot@0 hObjectParent@4 hObjectNew@8 hClass@12 pAllocParms@16 ... status varies */

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
    unsigned type = (request >> 8) & 0xff, nr = request & 0xff, sz = (request >> 16) & 0x3fff;

    int dump = enabled && type == 0x46 && arg &&
               (nr == 0x2a || nr == 0x2b || nr == 0x4e || nr == 0x4a ||
                nr == 0x5c || nr == 0x5d || nr == 0xd4);
    struct nvos54 pre = {0};
    void *pbuf_pre = NULL; uint32_t psz = 0;
    if (dump && nr == 0x2a && sz >= 32) {
        memcpy(&pre, arg, sizeof(pre));
        psz = pre.paramsSize; if (psz > 4096) psz = 4096;
        if ((want_cmd == 0xffffffff || pre.cmd == want_cmd) && pre.params && psz) {
            pbuf_pre = malloc(psz); memcpy(pbuf_pre, (void *)(uintptr_t)pre.params, psz);
        }
    }

    int r = real_ioctl(fd, request, arg);

    if (dump && nr == 0x2a && sz >= 32) {
        struct nvos54 post; memcpy(&post, arg, sizeof(post));
        if (want_cmd == 0xffffffff || post.cmd == want_cmd) {
            fprintf(stderr, "RM_CONTROL fd=%d cmd=0x%08x hClient=0x%08x hObject=0x%08x "
                    "paramsSize=%u status=0x%x ret=%d\n",
                    fd, post.cmd, post.hClient, post.hObject, post.paramsSize, post.status, r);
            if (pbuf_pre) hexdump("REQ", pbuf_pre, psz);
            if (post.params && psz) hexdump("RSP", (void *)(uintptr_t)post.params, psz);
        }
    } else if (dump) {
        fprintf(stderr, "RM nr=0x%02x sz=%u ret=%d status_lo=0x%08x\n",
                nr, sz, r, *(uint32_t *)arg);
    }
    free(pbuf_pre);
    return r;
}
