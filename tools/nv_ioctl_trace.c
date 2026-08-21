/* nv_ioctl_trace.c -- LD_PRELOAD shim that logs the NVIDIA frontend ioctls a
 * process issues on the BARE-METAL HOST, in the same shape nvkvm's NVKVM_DEBUG
 * trace prints them in the guest.  The two logs can then be diffed directly to
 * find where a forwarded session first diverges from a native one.
 *
 *   cc -shared -fPIC -o nv_ioctl_trace.so tools/nv_ioctl_trace.c -ldl
 *   NVTRACE_LOG=/tmp/host.log LD_PRELOAD=./nv_ioctl_trace.so ./some_gpu_program
 *
 * Covers NV_ESC_RM_ALLOC (0x2b), NV_ESC_RM_CONTROL (0x2a) and NV_ESC_RM_FREE
 * (0x29) -- the object-lifetime calls.  Everything else is passed through
 * untouched and unlogged.
 *
 * open()/openat() on the driver's device nodes are logged too, in the SAME
 * stream.  That ordering is the point: an RM-only log shows two sides agreeing
 * on every call and still behaving differently, and the deciding difference
 * turns out to be WHICH NODE was opened and WHEN (e.g. the NVIDIA DDX opens
 * /dev/nvidia-modeset on bare metal and never does in a guest).  Interleaving
 * opens with the RM conversation makes that visible without a second strace.
 * Set NVTRACE_OPENS_ALL=1 to log every open, not just the driver nodes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int (*real_ioctl)(int, unsigned long, ...);
static FILE *lg;

/* Log this path?  Default: the NVIDIA/DRM device nodes and the driver's procfs
 * -- the set whose presence or absence changes what the caller does next. */
static int path_interesting(const char *path) {
    if (!path) return 0;
    if (getenv("NVTRACE_OPENS_ALL")) return 1;
    return strstr(path, "nvidia") != NULL
        || strncmp(path, "/dev/dri", 8) == 0
        || strcmp(path, "/dev/vga_arbiter") == 0;
}

static void open_log(void) {
    const char *p = getenv("NVTRACE_LOG");
    lg = p ? fopen(p, "w") : NULL;
    if (!lg) lg = stderr;
    setvbuf(lg, NULL, _IOLBF, 0);
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (!lg) open_log();

    int r = real_ioctl(fd, req, arg);

    unsigned type = (req >> 8) & 0xff;
    unsigned nr   = req & 0xff;
    unsigned size = (req >> 16) & 0x3fff;
    if (type != 'F' || !arg) return r;

    const uint32_t *w = arg;
    if (nr == 0x2b && size >= 16) {
        /* NVOS21 {hRoot,hParent,hNew,hClass,pParms,status}  (32B)
         * NVOS64 {hRoot,hParent,hNew,hClass,pParms,pRights,paramsSize,flags,status} (48B) */
        uint32_t aps = 0, status;
        if (size >= 48) { aps = w[8]; status = w[10]; } else { status = w[6]; }
        fprintf(lg, "TRACE ALLOC hRoot=0x%x hParent=0x%x hNew=0x%x "
                    "hClass=0x%04x aps=%u nvstatus=0x%x\n",
                w[0], w[1], w[2], w[3], aps, status);
    } else if (nr == 0x2a && size >= 24) {
        /* NVOS54 {hClient,hObject,cmd,flags,params(u64)@16,paramsSize@24,status@28} */
        uint64_t pparams; uint32_t psize;
        memcpy(&pparams, (const char *)arg + 16, 8);
        memcpy(&psize,   (const char *)arg + 24, 4);
        fprintf(lg, "TRACE CTRL  hClient=0x%x hObject=0x%x cmd=0x%08x nvstatus=0x%x",
                w[0], w[1], w[2], w[7]);
        /* Dump the params of the few controls whose OUT values decide what the
         * userspace driver does next.  Same bytes on both sides of the
         * boundary or the forwarded session is not equivalent. */
        /* NVTRACE_PARAMS raises the dump limit (default 64).  The limit matters:
         * a guest/host divergence can be decided by an OUT value in a control
         * whose params are larger than the default, and then both logs agree on
         * every command and status while the sessions still behave differently.
         * Set it high (e.g. 512) when hunting exactly that. */
        static uint32_t cap;
        if (!cap) {
            const char *e = getenv("NVTRACE_PARAMS");
            cap = e ? (uint32_t)strtoul(e, NULL, 0) : 64;
            if (cap > 4096) cap = 4096;
        }
        if (pparams && psize && psize <= cap) {
            const unsigned char *pb = (const unsigned char *)(uintptr_t)pparams;
            fprintf(lg, " params[%u]=", psize);
            for (uint32_t i = 0; i < psize; i++) fprintf(lg, "%02x", pb[i]);
        } else if (pparams && psize) {
            fprintf(lg, " params[%u]=<over NVTRACE_PARAMS cap>", psize);
        }
        fputc('\n', lg);
    } else if (nr == 0x29 && size >= 16) {
        /* NVOS00 {hRoot,hObjectParent,hObjectOld,status} */
        fprintf(lg, "TRACE FREE  hRoot=0x%x hParent=0x%x hObject=0x%x nvstatus=0x%x\n",
                w[0], w[1], w[2], w[3]);
    } else if (getenv("NVTRACE_ALL")) {
        /* Every other frontend ioctl, with the head of its parameter block.
         * The RM object calls alone were not enough to explain a divergence:
         * the guest and host agreed on all of them and still behaved
         * differently, which means the deciding value came through one of
         * these. */
        uint32_t n = size < 48 ? size : 48;
        fprintf(lg, "TRACE IOCTL nr=0x%02x size=%u ret=%d p=", nr, size, r);
        for (uint32_t i = 0; i < n; i++) fprintf(lg, "%02x", ((const unsigned char *)arg)[i]);
        fputc('\n', lg);
    }
    return r;
}

/* open()/openat() -- logged into the same stream as the ioctls so the order of
 * "which node was opened" against "which RM call was made" is readable. */
static void log_open(const char *path, int fd) {
    if (!path_interesting(path)) return;
    if (!lg) open_log();
    fprintf(lg, "TRACE OPEN  fd=%d path=%s%s\n", fd, path,
            fd < 0 ? "  -> FAILED" : "");
}

int open(const char *path, int flags, ...);
int open(const char *path, int flags, ...) {
    static int (*real_open)(const char *, int, ...);
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    int fd = real_open(path, flags, mode);
    log_open(path, fd);
    return fd;
}

int open64(const char *path, int flags, ...);
int open64(const char *path, int flags, ...) {
    static int (*real_open64)(const char *, int, ...);
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    if (!real_open64) real_open64 = dlsym(RTLD_NEXT, "open64");
    int fd = real_open64(path, flags, mode);
    log_open(path, fd);
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...) {
    static int (*real_openat)(int, const char *, int, ...);
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
    int fd = real_openat(dirfd, path, flags, mode);
    log_open(path, fd);
    return fd;
}

int openat64(int dirfd, const char *path, int flags, ...);
int openat64(int dirfd, const char *path, int flags, ...) {
    static int (*real_openat64)(int, const char *, int, ...);
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    if (!real_openat64) real_openat64 = dlsym(RTLD_NEXT, "openat64");
    int fd = real_openat64(dirfd, path, flags, mode);
    log_open(path, fd);
    return fd;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
    if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
    void *p = real_mmap(addr, len, prot, flags, fd, off);
    if (lg && fd >= 0 && getenv("NVTRACE_ALL"))
        fprintf(lg, "TRACE MMAP  fd=%d len=%zu prot=0x%x flags=0x%x off=0x%llx -> %s\n",
                fd, len, prot, flags, (unsigned long long)off,
                p == (void *)-1 ? "FAILED" : "ok");
    return p;
}
