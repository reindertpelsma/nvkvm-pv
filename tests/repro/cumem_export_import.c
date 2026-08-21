/*
 * cumem_export_import.c — minimal reproducer for the NCCL SHM transport failure
 *
 * NCCL's SHM transport (transport/shm.cc:590 in v2.27.3) does exactly this:
 * one rank exports a CUDA VMM allocation to a POSIX file descriptor, passes
 * that fd to a peer rank over a Unix socket with SCM_RIGHTS, and the peer
 * calls cuMemImportFromShareableHandle() on it.  In an nvkvm guest that import
 * fails with CUDA error 101 (invalid device ordinal); on the bare-metal host of
 * the same box it succeeds.  This program isolates that one exchange with no
 * NCCL, no torch and no vLLM in the picture.
 *
 * Two *separate* processes are the point: each guest process gets its own
 * nvkvm isolate (its own host stub process, its own RM client), and an RM
 * object minted in one stub is meaningless in another.  A fork() that shared
 * the parent's device fds would not exercise the same path.
 *
 * libcuda is dlopen'd and the entry points are declared here, so the repro
 * builds with nothing but a C compiler and libdl — no CUDA toolkit needed, and
 * the *same binary* runs on the host and in the guest.  A difference between
 * the two is the finding.
 *
 *   cc -O2 -o cumem_ei tests/repro/cumem_export_import.c -ldl
 *   ./cumem_ei                 # spawns both halves, prints one RESULT line
 *   ./cumem_ei export <sock>   # exporter half only
 *   ./cumem_ei import <sock>   # importer half only
 *
 * Options (env):
 *   REPRO_EXP_DEV=N / REPRO_IMP_DEV=N   device ordinals (default 0 and 1;
 *                      NCCL connects rank->rank across two GPUs).
 *   REPRO_SIZE=N       allocation size in bytes (rounded up to granularity).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <stdint.h>

/* ── libcuda ABI (mirrors cuda.h; kept local so no toolkit is required) ──── */
typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef unsigned long long CUmemGenericAllocationHandle;
typedef unsigned long long CUdeviceptr;

#define CU_MEM_ALLOCATION_TYPE_PINNED            0x1
#define CU_MEM_LOCATION_TYPE_DEVICE              0x1
#define CU_MEM_LOCATION_TYPE_HOST                0x2
#define CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR 0x1
#define CU_MEM_ALLOC_GRANULARITY_MINIMUM         0x0
#define CU_MEM_ACCESS_FLAGS_PROT_READWRITE       0x3

typedef struct { int type; int id; } CUmemLocation;

typedef struct {
    int type;                       /* CUmemAllocationType            */
    int requestedHandleTypes;       /* CUmemAllocationHandleType      */
    CUmemLocation location;
    void *win32HandleMetaData;
    struct {
        unsigned char  compressionType;
        unsigned char  gpuDirectRDMACapable;
        unsigned short usage;
        unsigned char  reserved[4];
    } allocFlags;
} CUmemAllocationProp;

typedef struct { CUmemLocation location; int flags; } CUmemAccessDesc;

static CUresult (*p_cuInit)(unsigned);
static CUresult (*p_cuDeviceGet)(CUdevice *, int);
static CUresult (*p_cuDeviceGetCount)(int *);
static CUresult (*p_cuDevicePrimaryCtxRetain)(CUcontext *, CUdevice);
static CUresult (*p_cuCtxSetCurrent)(CUcontext);
static CUresult (*p_cuMemGetAllocationGranularity)(size_t *, const CUmemAllocationProp *, int);
static CUresult (*p_cuMemCreate)(CUmemGenericAllocationHandle *, size_t, const CUmemAllocationProp *, unsigned long long);
static CUresult (*p_cuMemExportToShareableHandle)(void *, CUmemGenericAllocationHandle, int, unsigned long long);
static CUresult (*p_cuMemImportFromShareableHandle)(CUmemGenericAllocationHandle *, void *, int);
static CUresult (*p_cuMemAddressReserve)(CUdeviceptr *, size_t, size_t, CUdeviceptr, unsigned long long);
static CUresult (*p_cuMemMap)(CUdeviceptr, size_t, size_t, CUmemGenericAllocationHandle, unsigned long long);
static CUresult (*p_cuMemSetAccess)(CUdeviceptr, size_t, const CUmemAccessDesc *, size_t);
static CUresult (*p_cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
static CUresult (*p_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*p_cuCtxSynchronize)(void);
static CUresult (*p_cuGetErrorName)(CUresult, const char **);

/*
 * The CUDA driver API versions several entry points with a _v2/_v3 suffix and
 * hides it behind a #define in cuda.h (cuMemcpyHtoD -> cuMemcpyHtoD_v2).  dlsym
 * does not see that #define, and the unsuffixed symbol is the *old* ABI, which
 * fails with CUDA_ERROR_INVALID_CONTEXT against a primary context.  Try the
 * suffixed names first.
 */
static void *sym_versioned(void *h, const char *name)
{
    char buf[128];
    void *s;
    snprintf(buf, sizeof(buf), "%s_v3", name);
    if ((s = dlsym(h, buf))) return s;
    snprintf(buf, sizeof(buf), "%s_v2", name);
    if ((s = dlsym(h, buf))) return s;
    return dlsym(h, name);
}

#define LOADSYM(h, n) do {                                                 \
        *(void **)(&p_##n) = sym_versioned(h, #n);                         \
        if (!p_##n) { fprintf(stderr, "dlsym %s failed\n", #n); return 1; } \
    } while (0)

static int load_cuda(void)
{
    void *h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!h) h = dlopen("libcuda.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen libcuda failed: %s\n", dlerror()); return 1; }
    LOADSYM(h, cuInit);
    LOADSYM(h, cuDeviceGet);
    LOADSYM(h, cuDeviceGetCount);
    LOADSYM(h, cuDevicePrimaryCtxRetain);
    LOADSYM(h, cuCtxSetCurrent);
    LOADSYM(h, cuMemGetAllocationGranularity);
    LOADSYM(h, cuMemCreate);
    LOADSYM(h, cuMemExportToShareableHandle);
    LOADSYM(h, cuMemImportFromShareableHandle);
    LOADSYM(h, cuMemAddressReserve);
    LOADSYM(h, cuMemMap);
    LOADSYM(h, cuMemSetAccess);
    LOADSYM(h, cuMemcpyHtoD);
    LOADSYM(h, cuMemcpyDtoH);
    LOADSYM(h, cuCtxSynchronize);
    LOADSYM(h, cuGetErrorName);
    return 0;
}

static const char *cuerr(CUresult r)
{
    const char *s = NULL;
    if (p_cuGetErrorName) p_cuGetErrorName(r, &s);
    return s ? s : "?";
}

#define CHECK(tag, expr) do {                                              \
        CUresult _r = (expr);                                              \
        if (_r != 0) {                                                     \
            printf("%s: %s FAILED: %d (%s)\n", who, tag, _r, cuerr(_r));   \
            fflush(stdout);                                                \
            return _r;                                                     \
        }                                                                  \
    } while (0)

/* ── SCM_RIGHTS fd passing ───────────────────────────────────────────────── */
static int send_fd(int sock, int fd)
{
    struct msghdr msg = {0};
    struct iovec iov;
    char buf[1] = {'F'};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cm;

    iov.iov_base = buf; iov.iov_len = 1;
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    memset(cbuf, 0, sizeof(cbuf));
    msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
    cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

static int recv_fd(int sock)
{
    struct msghdr msg = {0};
    struct iovec iov;
    char buf[1];
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cm;
    int fd = -1;

    iov.iov_base = buf; iov.iov_len = 1;
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
    if (recvmsg(sock, &msg, 0) <= 0) return -1;
    for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    return fd;
}

static int envint(const char *n, int dflt)
{
    const char *v = getenv(n);
    return v && *v ? atoi(v) : dflt;
}

static void fill_prop(CUmemAllocationProp *prop, int dev)
{
    memset(prop, 0, sizeof(*prop));
    prop->type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop->requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    prop->location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop->location.id   = dev;
}

/* Retain the primary context on `dev` and make it current. */
static CUresult ctx_on(int dev, const char *who)
{
    CUdevice d; CUcontext c;
    CHECK("cuInit", p_cuInit(0));
    CHECK("cuDeviceGet", p_cuDeviceGet(&d, dev));
    CHECK("cuDevicePrimaryCtxRetain", p_cuDevicePrimaryCtxRetain(&c, d));
    CHECK("cuCtxSetCurrent", p_cuCtxSetCurrent(c));
    return 0;
}

#define PATTERN 0xA5

static int do_export(const char *sockpath)
{
    const char *who = "exporter";
    int dev = envint("REPRO_EXP_DEV", 0);
    CUmemAllocationProp prop;
    CUmemGenericAllocationHandle h;
    size_t gran = 0, size = (size_t)envint("REPRO_SIZE", 2 * 1024 * 1024);
    int fd = -1, lsock, csock;
    struct sockaddr_un sa;
    CUresult r;

    if (load_cuda()) return 1;
    if ((r = ctx_on(dev, who)) != 0) return r;

    fill_prop(&prop, dev);
    CHECK("cuMemGetAllocationGranularity",
          p_cuMemGetAllocationGranularity(&gran, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM));
    if (gran == 0) gran = 2 * 1024 * 1024;
    size = ((size + gran - 1) / gran) * gran;

    CHECK("cuMemCreate", p_cuMemCreate(&h, size, &prop, 0));
    printf("%s: dev=%d size=%zu granularity=%zu handle=0x%llx\n",
           who, dev, size, gran, (unsigned long long)h);

    /* Write a known pattern so the importer can prove it sees the SAME memory,
     * not merely that the import call returned success. */
    {
        CUdeviceptr va = 0;
        CUmemAccessDesc acc;
        unsigned char *src = malloc(size);
        memset(src, PATTERN, size);
        CHECK("cuMemAddressReserve", p_cuMemAddressReserve(&va, size, gran, 0, 0));
        CHECK("cuMemMap", p_cuMemMap(va, size, 0, h, 0));
        memset(&acc, 0, sizeof(acc));
        acc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
        acc.location.id   = dev;
        acc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
        CHECK("cuMemSetAccess", p_cuMemSetAccess(va, size, &acc, 1));
        CHECK("cuMemcpyHtoD", p_cuMemcpyHtoD(va, src, size));
        CHECK("cuCtxSynchronize", p_cuCtxSynchronize());
        free(src);
        printf("%s: wrote 0x%02x pattern over %zu bytes\n", who, PATTERN, size);
    }

    CHECK("cuMemExportToShareableHandle",
          p_cuMemExportToShareableHandle(&fd, h,
                                         CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR, 0));
    printf("%s: cuMemExportToShareableHandle OK fd=%d\n", who, fd);
    fflush(stdout);

    /* Hand the fd to the importer over a Unix socket, exactly as NCCL does. */
    lsock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lsock < 0) { perror("socket"); return 1; }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sockpath);
    unlink(sockpath);
    if (bind(lsock, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    if (listen(lsock, 1) < 0) { perror("listen"); return 1; }
    csock = accept(lsock, NULL, NULL);
    if (csock < 0) { perror("accept"); return 1; }
    if (send_fd(csock, fd) < 0) { perror("send_fd"); return 1; }
    printf("%s: sent fd over SCM_RIGHTS, waiting for importer\n", who);
    fflush(stdout);

    { char b; read(csock, &b, 1); }   /* keep the exporter (and fd) alive */
    close(csock); close(lsock); unlink(sockpath);
    return 0;
}

static int do_import(const char *sockpath)
{
    const char *who = "importer";
    int dev = envint("REPRO_IMP_DEV", 1);
    CUmemGenericAllocationHandle h = 0;
    int fd, sock, tries;
    struct sockaddr_un sa;
    CUresult r, ir;

    if (load_cuda()) return 1;
    { int n = 0; if (p_cuInit(0) == 0 && p_cuDeviceGetCount(&n) == 0 && dev >= n) dev = n - 1; }
    if ((r = ctx_on(dev, who)) != 0) return r;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sockpath);
    for (tries = 0; tries < 200; tries++) {
        if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0) break;
        usleep(50000);
    }
    if (tries == 200) { fprintf(stderr, "%s: connect timed out\n", who); return 1; }

    fd = recv_fd(sock);
    if (fd < 0) { fprintf(stderr, "%s: recv_fd failed\n", who); return 1; }
    printf("%s: dev=%d received fd=%d over SCM_RIGHTS\n", who, dev, fd);
    fflush(stdout);

    /* THE CALL UNDER TEST — NCCL v2.27.3 transport/shm.cc:590. */
    ir = p_cuMemImportFromShareableHandle(&h, (void *)(uintptr_t)fd,
                                          CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR);
    if (ir != 0) {
        printf("%s: cuMemImportFromShareableHandle FAILED: %d (%s)\n",
               who, ir, cuerr(ir));
        printf("RESULT cumem_export_import=FAIL import_err=%d(%s)\n", ir, cuerr(ir));
        fflush(stdout);
        { char b = 'x'; write(sock, &b, 1); }
        return 2;
    }
    printf("%s: cuMemImportFromShareableHandle OK handle=0x%llx\n",
           who, (unsigned long long)h);

    /* Import success alone is not proof: map it and check the exporter's
     * pattern is actually there, so a zero-filled or unrelated mapping is
     * reported as a failure rather than a pass. */
    {
        CUdeviceptr va = 0;
        CUmemAccessDesc acc;
        size_t gran = 2 * 1024 * 1024, size = (size_t)envint("REPRO_SIZE", 2 * 1024 * 1024);
        unsigned char *dst;
        size_t i, bad = 0;
        CUmemAllocationProp prop;

        fill_prop(&prop, dev);
        p_cuMemGetAllocationGranularity(&gran, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM);
        if (gran == 0) gran = 2 * 1024 * 1024;
        size = ((size + gran - 1) / gran) * gran;

        CHECK("cuMemAddressReserve", p_cuMemAddressReserve(&va, size, gran, 0, 0));
        CHECK("cuMemMap", p_cuMemMap(va, size, 0, h, 0));
        memset(&acc, 0, sizeof(acc));
        acc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
        acc.location.id   = dev;
        acc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
        CHECK("cuMemSetAccess", p_cuMemSetAccess(va, size, &acc, 1));
        dst = malloc(size);
        memset(dst, 0, size);
        CHECK("cuMemcpyDtoH", p_cuMemcpyDtoH(dst, va, size));
        CHECK("cuCtxSynchronize", p_cuCtxSynchronize());
        for (i = 0; i < size; i++) if (dst[i] != PATTERN) bad++;
        printf("%s: readback checked=%zu mismatch=%zu\n", who, size, bad);
        free(dst);
        if (bad) {
            printf("RESULT cumem_export_import=FAIL imported_but_wrong_bytes mismatch=%zu\n", bad);
            fflush(stdout);
            { char b = 'x'; write(sock, &b, 1); }
            return 3;
        }
    }

    printf("RESULT cumem_export_import=PASS\n");
    fflush(stdout);
    { char b = 'x'; write(sock, &b, 1); }
    return 0;
}

int main(int argc, char **argv)
{
    char sock[128];

    if (argc >= 2 && !strcmp(argv[1], "export"))
        return do_export(argc >= 3 ? argv[2] : "/tmp/cumem_ei.sock");
    if (argc >= 2 && !strcmp(argv[1], "import"))
        return do_import(argc >= 3 ? argv[2] : "/tmp/cumem_ei.sock");

    /* Default: spawn both halves as separate processes (separate isolates). */
    snprintf(sock, sizeof(sock), "/tmp/cumem_ei.%d.sock", (int)getpid());
    {
        pid_t e, i;
        int st_e = 0, st_i = 0;
        if ((e = fork()) == 0) { execl("/proc/self/exe", argv[0], "export", sock, (char *)NULL); _exit(127); }
        usleep(200000);
        if ((i = fork()) == 0) { execl("/proc/self/exe", argv[0], "import", sock, (char *)NULL); _exit(127); }
        waitpid(i, &st_i, 0);
        waitpid(e, &st_e, 0);
        unlink(sock);
        return WIFEXITED(st_i) ? WEXITSTATUS(st_i) : 1;
    }
}
