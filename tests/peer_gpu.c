/* SPDX-License-Identifier: GPL-2.0
 *
 * peer_gpu.c — does data actually move BETWEEN GPUs under nvkvm?
 *
 * Every previous multi-GPU test in this project proved that N GPUs each work
 * in isolation.  None of them ever moved a byte from one GPU to another.
 * That is a different set of forwarding paths: peer-capability queries, the
 * two-context memcpy entry points, and — when P2P is actually available —
 * one GPU's BAR mapped into another's address space so a kernel on A can
 * dereference a pointer resident on B.
 *
 * IMPORTANT, and the reason this probe exists in this shape:
 *   cuDeviceCanAccessPeer() returning 0 is NOT a bug.  Consumer GeForce parts
 *   routinely report no P2P over PCIe at all.  The only way to tell "nvkvm
 *   dropped the capability" from "this hardware never had it" is to run the
 *   SAME binary on the bare-metal host and diff the matrices.  So this probe
 *   is built to run in both places and print a machine-diffable matrix.
 *
 * Where P2P is unavailable, cuMemcpyPeer still works — CUDA stages it through
 * host memory.  That path is still real forwarding and must still be
 * byte-exact, so it is checked either way.
 *
 * Modes:
 *   (no args)     run the whole ladder over every device
 *   --bytes N     transfer size (default 1 MiB)
 *   --quiet       matrix + summary only
 *
 * Output is line-oriented "CHECK|name|status|detail" like validate.sh, plus
 * "MATRIX|..." lines that are meant to be diffed host-vs-guest.
 *
 * cc -O2 -o peer_gpu peer_gpu.c -ldl
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUmodule;
typedef void *CUfunction;
typedef void *CUstream;
typedef unsigned long long CUdeviceptr;

static CUresult (*p_cuInit)(unsigned);
static CUresult (*p_cuDriverGetVersion)(int *);
static CUresult (*p_cuDeviceGetCount)(int *);
static CUresult (*p_cuDeviceGet)(CUdevice *, int);
static CUresult (*p_cuDeviceGetName)(char *, int, CUdevice);
static CUresult (*p_cuDeviceGetPCIBusId)(char *, int, CUdevice);
static CUresult (*p_cuDeviceCanAccessPeer)(int *, CUdevice, CUdevice);
static CUresult (*p_cuDeviceGetP2PAttribute)(int *, int, CUdevice, CUdevice);
static CUresult (*p_cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*p_cuCtxDestroy)(CUcontext);
static CUresult (*p_cuCtxSetCurrent)(CUcontext);
static CUresult (*p_cuCtxSynchronize)(void);
static CUresult (*p_cuCtxEnablePeerAccess)(CUcontext, unsigned);
static CUresult (*p_cuCtxDisablePeerAccess)(CUcontext);
static CUresult (*p_cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*p_cuMemFree)(CUdeviceptr);
static CUresult (*p_cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
static CUresult (*p_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*p_cuMemcpyPeer)(CUdeviceptr, CUcontext, CUdeviceptr, CUcontext, size_t);
static CUresult (*p_cuMemcpyPeerAsync)(CUdeviceptr, CUcontext, CUdeviceptr, CUcontext, size_t, CUstream);
static CUresult (*p_cuMemsetD8)(CUdeviceptr, unsigned char, size_t);
static CUresult (*p_cuStreamCreate)(CUstream *, unsigned);
static CUresult (*p_cuStreamSynchronize)(CUstream);
static CUresult (*p_cuStreamDestroy)(CUstream);
static CUresult (*p_cuModuleLoadData)(CUmodule *, const void *);
static CUresult (*p_cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
static CUresult (*p_cuModuleUnload)(CUmodule);
static CUresult (*p_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                    unsigned, unsigned, unsigned, unsigned,
                                    CUstream, void **, void **);
static CUresult (*p_cuGetErrorName)(CUresult, const char **);

#define MAXDEV 16

static int  n_pass, n_fail, n_skip, quiet;

static void emit(const char *name, const char *status, const char *fmt, ...)
{
    char detail[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);

    if (!strcmp(status, "PASS")) n_pass++;
    else if (!strcmp(status, "FAIL")) n_fail++;
    else n_skip++;

    if (quiet && !strcmp(status, "PASS")) return;
    printf("CHECK|%s|%s|%s\n", name, status, detail);
    fflush(stdout);
}

static const char *errname(CUresult r)
{
    const char *s = NULL;
    if (p_cuGetErrorName && p_cuGetErrorName(r, &s) == 0 && s) return s;
    return "?";
}

/* peer_add: c[i] = a[i] + b[i].  Launched in context A with `b` deliberately
 * pointing at memory resident on device B, so a successful checked result is
 * proof that a kernel on one GPU dereferenced another GPU's memory. */
static const char peer_add_ptx[] =
"//\n"
".version 7.5\n"
".target sm_60\n"
".address_size 64\n"
"\n"
".visible .entry peer_add(\n"
"    .param .u64 peer_add_param_0,\n"
"    .param .u64 peer_add_param_1,\n"
"    .param .u64 peer_add_param_2,\n"
"    .param .u32 peer_add_param_3\n"
")\n"
"{\n"
"    .reg .pred  %p<2>;\n"
"    .reg .b32   %r<8>;\n"
"    .reg .b64   %rd<11>;\n"
"\n"
"    ld.param.u64    %rd1, [peer_add_param_0];\n"
"    ld.param.u64    %rd2, [peer_add_param_1];\n"
"    ld.param.u64    %rd3, [peer_add_param_2];\n"
"    ld.param.u32    %r2,  [peer_add_param_3];\n"
"    mov.u32         %r3, %ntid.x;\n"
"    mov.u32         %r4, %ctaid.x;\n"
"    mov.u32         %r5, %tid.x;\n"
"    mad.lo.s32      %r1, %r3, %r4, %r5;\n"
"    setp.ge.s32     %p1, %r1, %r2;\n"
"    @%p1 bra        $L__BB0_2;\n"
"\n"
"    cvta.to.global.u64  %rd4, %rd1;\n"
"    mul.wide.s32        %rd5, %r1, 4;\n"
"    add.s64             %rd6, %rd4, %rd5;\n"
"    ld.global.u32       %r6, [%rd6];\n"
"    cvta.to.global.u64  %rd7, %rd2;\n"
"    add.s64             %rd8, %rd7, %rd5;\n"
"    ld.global.u32       %r7, [%rd8];\n"
"    add.s32             %r7, %r7, %r6;\n"
"    cvta.to.global.u64  %rd9, %rd3;\n"
"    add.s64             %rd10, %rd9, %rd5;\n"
"    st.global.u32       [%rd10], %r7;\n"
"\n"
"$L__BB0_2:\n"
"    ret;\n"
"}\n";

/* Per-device fill pattern.  The high byte carries the source ordinal, so a
 * copy that silently came from the wrong GPU cannot compare equal. */
static void fill_pattern(uint32_t *buf, int n, int dev)
{
    for (int i = 0; i < n; i++)
        buf[i] = ((uint32_t)(dev + 1) << 24) ^ (uint32_t)(i * 2654435761u);
}

static int cmp_pattern(const uint32_t *buf, int n, int dev, int *first_bad)
{
    for (int i = 0; i < n; i++) {
        uint32_t want = ((uint32_t)(dev + 1) << 24) ^ (uint32_t)(i * 2654435761u);
        if (buf[i] != want) { *first_bad = i; return 0; }
    }
    return 1;
}

int main(int argc, char **argv)
{
    size_t bytes = 1u << 20;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bytes") && i + 1 < argc) bytes = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
    }
    int n_elem = (int)(bytes / 4);
    bytes = (size_t)n_elem * 4;

    void *h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!h) h = dlopen("libcuda.so", RTLD_NOW);
    if (!h) { printf("CHECK|libcuda|SKIP|dlopen failed: %s\n", dlerror()); return 3; }

#define SYM(n) do { p_##n = dlsym(h, #n); } while (0)
    SYM(cuInit); SYM(cuDriverGetVersion); SYM(cuDeviceGetCount); SYM(cuDeviceGet);
    SYM(cuDeviceGetName); SYM(cuDeviceGetPCIBusId); SYM(cuDeviceCanAccessPeer);
    SYM(cuDeviceGetP2PAttribute);
    SYM(cuCtxDestroy); SYM(cuCtxSetCurrent); SYM(cuCtxSynchronize);
    SYM(cuCtxEnablePeerAccess); SYM(cuCtxDisablePeerAccess);
    SYM(cuMemAlloc); SYM(cuMemFree); SYM(cuMemcpyHtoD); SYM(cuMemcpyDtoH);
    SYM(cuMemcpyPeer); SYM(cuMemcpyPeerAsync); SYM(cuMemsetD8);
    SYM(cuStreamCreate); SYM(cuStreamSynchronize); SYM(cuStreamDestroy);
    SYM(cuModuleLoadData); SYM(cuModuleGetFunction); SYM(cuModuleUnload);
    SYM(cuLaunchKernel); SYM(cuGetErrorName);
    /* versioned symbols: the unsuffixed names are stubs in modern libcuda */
    p_cuCtxCreate  = dlsym(h, "cuCtxCreate_v2");  if (!p_cuCtxCreate)  SYM(cuCtxCreate);
    p_cuMemAlloc   = dlsym(h, "cuMemAlloc_v2");   if (!p_cuMemAlloc)   SYM(cuMemAlloc);
    p_cuMemFree    = dlsym(h, "cuMemFree_v2");    if (!p_cuMemFree)    SYM(cuMemFree);
    p_cuMemcpyHtoD = dlsym(h, "cuMemcpyHtoD_v2"); if (!p_cuMemcpyHtoD) SYM(cuMemcpyHtoD);
    p_cuMemcpyDtoH = dlsym(h, "cuMemcpyDtoH_v2"); if (!p_cuMemcpyDtoH) SYM(cuMemcpyDtoH);
    p_cuMemsetD8   = dlsym(h, "cuMemsetD8_v2");   if (!p_cuMemsetD8)   SYM(cuMemsetD8);
    p_cuCtxDestroy = dlsym(h, "cuCtxDestroy_v2"); if (!p_cuCtxDestroy) SYM(cuCtxDestroy);
#undef SYM

    if (!p_cuInit || !p_cuDeviceCanAccessPeer || !p_cuMemcpyPeer) {
        printf("CHECK|libcuda_syms|FAIL|missing required symbols in libcuda\n");
        return 3;
    }

    CUresult r = p_cuInit(0);
    if (r) { emit("cuInit", "FAIL", "rc=%d (%s)", r, errname(r)); return 1; }

    int drv = 0;
    if (p_cuDriverGetVersion) p_cuDriverGetVersion(&drv);

    int ndev = 0;
    r = p_cuDeviceGetCount(&ndev);
    if (r) { emit("cuDeviceGetCount", "FAIL", "rc=%d (%s)", r, errname(r)); return 1; }
    printf("INFO|driver_version|%d\n", drv);
    printf("INFO|device_count|%d\n", ndev);
    if (ndev < 2) {
        emit("peer_tests", "SKIP", "need >=2 devices, have %d", ndev);
        printf("TOTAL|pass=%d fail=%d skip=%d\n", n_pass, n_fail, n_skip);
        return n_fail ? 1 : 2;
    }
    if (ndev > MAXDEV) ndev = MAXDEV;

    CUdevice dev[MAXDEV];
    CUcontext ctx[MAXDEV];
    for (int i = 0; i < ndev; i++) {
        char name[128] = "?", bus[32] = "?";
        r = p_cuDeviceGet(&dev[i], i);
        if (r) { emit("cuDeviceGet", "FAIL", "dev %d rc=%d (%s)", i, r, errname(r)); return 1; }
        if (p_cuDeviceGetName) p_cuDeviceGetName(name, sizeof name, dev[i]);
        if (p_cuDeviceGetPCIBusId) p_cuDeviceGetPCIBusId(bus, sizeof bus, dev[i]);
        printf("INFO|device%d|%s|%s\n", i, name, bus);
        r = p_cuCtxCreate(&ctx[i], 0, dev[i]);
        if (r) { emit("cuCtxCreate", "FAIL", "dev %d rc=%d (%s)", i, r, errname(r)); return 1; }
    }
    fflush(stdout);

    /* ---------------------------------------------------------------- *
     * 1. peer capability matrix — the thing to diff host vs guest       *
     * ---------------------------------------------------------------- */
    int can[MAXDEV][MAXDEV];
    memset(can, 0, sizeof can);
    int any_p2p = 0, query_fail = 0;
    for (int a = 0; a < ndev; a++) {
        for (int b = 0; b < ndev; b++) {
            if (a == b) { can[a][b] = -1; continue; }
            int c = 0;
            r = p_cuDeviceCanAccessPeer(&c, dev[a], dev[b]);
            if (r) {
                can[a][b] = -2; query_fail++;
                emit("canAccessPeer_query", "FAIL", "%d->%d rc=%d (%s)", a, b, r, errname(r));
            } else {
                can[a][b] = c;
                if (c) any_p2p = 1;
            }
        }
    }
    for (int a = 0; a < ndev; a++) {
        char line[256]; int off = 0;
        off += snprintf(line + off, sizeof line - off, "MATRIX|canAccessPeer|src=%d|", a);
        for (int b = 0; b < ndev; b++)
            off += snprintf(line + off, sizeof line - off, "%s%s",
                            can[a][b] == -1 ? "self" : can[a][b] == -2 ? "ERR" :
                            can[a][b] ? "1" : "0",
                            b == ndev - 1 ? "" : ",");
        printf("%s\n", line);
    }
    if (!query_fail)
        emit("canAccessPeer_matrix", "PASS", "queried %d ordered pairs, p2p_available=%s",
             ndev * (ndev - 1), any_p2p ? "yes" : "no (expected on consumer PCIe parts)");
    printf("INFO|p2p_available|%s\n", any_p2p ? "yes" : "no");
    fflush(stdout);

    /* ---------------------------------------------------------------- *
     * 2. host-staged control: D2H then H2D between every ordered pair   *
     * ---------------------------------------------------------------- */
    uint32_t *hsrc = malloc(bytes), *hdst = malloc(bytes);
    if (!hsrc || !hdst) { emit("malloc", "FAIL", "host buffers"); return 3; }

    CUdeviceptr buf[MAXDEV];
    for (int i = 0; i < ndev; i++) {
        p_cuCtxSetCurrent(ctx[i]);
        r = p_cuMemAlloc(&buf[i], bytes);
        if (r) { emit("cuMemAlloc", "FAIL", "dev %d rc=%d (%s)", i, r, errname(r)); return 1; }
        fill_pattern(hsrc, n_elem, i);
        r = p_cuMemcpyHtoD(buf[i], hsrc, bytes);
        if (r) { emit("cuMemcpyHtoD", "FAIL", "dev %d rc=%d (%s)", i, r, errname(r)); return 1; }
    }

    for (int a = 0; a < ndev; a++) {
        for (int b = 0; b < ndev; b++) {
            if (a == b) continue;
            char tag[64]; snprintf(tag, sizeof tag, "d2d_via_host_%d_to_%d", a, b);
            /* pull a's pattern to host, push into b, read back, verify it is a's */
            p_cuCtxSetCurrent(ctx[a]);
            memset(hdst, 0, bytes);
            r = p_cuMemcpyDtoH(hdst, buf[a], bytes);
            if (r) { emit(tag, "FAIL", "DtoH rc=%d (%s)", r, errname(r)); continue; }
            p_cuCtxSetCurrent(ctx[b]);
            r = p_cuMemcpyHtoD(buf[b], hdst, bytes);
            if (r) { emit(tag, "FAIL", "HtoD rc=%d (%s)", r, errname(r)); continue; }
            memset(hdst, 0, bytes);
            r = p_cuMemcpyDtoH(hdst, buf[b], bytes);
            if (r) { emit(tag, "FAIL", "readback rc=%d (%s)", r, errname(r)); continue; }
            int bad = -1;
            if (!cmp_pattern(hdst, n_elem, a, &bad))
                emit(tag, "FAIL", "mismatch at elem %d: got 0x%08x", bad, hdst[bad]);
            else
                emit(tag, "PASS", "%zu bytes byte-exact via host", bytes);
            /* restore b's own pattern for later phases */
            fill_pattern(hsrc, n_elem, b);
            p_cuMemcpyHtoD(buf[b], hsrc, bytes);
        }
    }
    fflush(stdout);

    /* ---------------------------------------------------------------- *
     * 3. cuMemcpyPeer over every ordered pair, byte-exact               *
     * ---------------------------------------------------------------- */
    for (int a = 0; a < ndev; a++) {
        for (int b = 0; b < ndev; b++) {
            if (a == b) continue;
            char tag[64]; snprintf(tag, sizeof tag, "cuMemcpyPeer_%d_to_%d", a, b);
            /* poison dst so a no-op copy cannot pass */
            p_cuCtxSetCurrent(ctx[b]);
            p_cuMemsetD8(buf[b], 0xA5, bytes);
            p_cuCtxSynchronize();

            r = p_cuMemcpyPeer(buf[b], ctx[b], buf[a], ctx[a], bytes);
            if (r) { emit(tag, "FAIL", "rc=%d (%s)", r, errname(r)); goto restore_a;
            }
            p_cuCtxSetCurrent(ctx[b]);
            p_cuCtxSynchronize();
            memset(hdst, 0, bytes);
            r = p_cuMemcpyDtoH(hdst, buf[b], bytes);
            if (r) { emit(tag, "FAIL", "readback rc=%d (%s)", r, errname(r)); goto restore_a; }
            {
                int bad = -1;
                if (!cmp_pattern(hdst, n_elem, a, &bad))
                    emit(tag, "FAIL", "mismatch at elem %d: got 0x%08x (src dev %d pattern expected)",
                         bad, hdst[bad], a);
                else
                    emit(tag, "PASS", "%zu bytes byte-exact, p2p=%s", bytes,
                         can[b][a] == 1 ? "direct" : "staged");
            }
        restore_a:
            fill_pattern(hsrc, n_elem, b);
            p_cuCtxSetCurrent(ctx[b]);
            p_cuMemcpyHtoD(buf[b], hsrc, bytes);
        }
    }
    fflush(stdout);

    /* ---------------------------------------------------------------- *
     * 4. cuMemcpyPeerAsync over every ordered pair                      *
     * ---------------------------------------------------------------- */
    for (int a = 0; a < ndev; a++) {
        for (int b = 0; b < ndev; b++) {
            if (a == b) continue;
            char tag[64]; snprintf(tag, sizeof tag, "cuMemcpyPeerAsync_%d_to_%d", a, b);
            CUstream st = NULL;
            p_cuCtxSetCurrent(ctx[b]);
            p_cuMemsetD8(buf[b], 0x5A, bytes);
            p_cuCtxSynchronize();
            r = p_cuStreamCreate(&st, 0);
            if (r) { emit(tag, "SKIP", "cuStreamCreate rc=%d (%s)", r, errname(r)); continue; }

            r = p_cuMemcpyPeerAsync(buf[b], ctx[b], buf[a], ctx[a], bytes, st);
            if (r) { emit(tag, "FAIL", "rc=%d (%s)", r, errname(r)); p_cuStreamDestroy(st); goto restore_b; }
            r = p_cuStreamSynchronize(st);
            if (r) { emit(tag, "FAIL", "streamSync rc=%d (%s)", r, errname(r)); p_cuStreamDestroy(st); goto restore_b; }
            p_cuStreamDestroy(st);
            memset(hdst, 0, bytes);
            r = p_cuMemcpyDtoH(hdst, buf[b], bytes);
            if (r) { emit(tag, "FAIL", "readback rc=%d (%s)", r, errname(r)); goto restore_b; }
            {
                int bad = -1;
                if (!cmp_pattern(hdst, n_elem, a, &bad))
                    emit(tag, "FAIL", "mismatch at elem %d: got 0x%08x", bad, hdst[bad]);
                else
                    emit(tag, "PASS", "%zu bytes byte-exact async", bytes);
            }
        restore_b:
            fill_pattern(hsrc, n_elem, b);
            p_cuCtxSetCurrent(ctx[b]);
            p_cuMemcpyHtoD(buf[b], hsrc, bytes);
        }
    }
    fflush(stdout);

    /* ---------------------------------------------------------------- *
     * 5. enable peer access + kernel on A dereferencing B's memory      *
     *    (only meaningful where canAccessPeer said yes)                 *
     * ---------------------------------------------------------------- */
    if (!any_p2p) {
        emit("peer_kernel", "SKIP",
             "no ordered pair reports P2P; nothing to enable (matches consumer PCIe hardware)");
    } else {
        for (int a = 0; a < ndev; a++) {
            for (int b = 0; b < ndev; b++) {
                if (a == b || can[a][b] != 1) continue;
                char tag[64]; snprintf(tag, sizeof tag, "peer_kernel_%d_reads_%d", a, b);

                p_cuCtxSetCurrent(ctx[a]);
                r = p_cuCtxEnablePeerAccess(ctx[b], 0);
                if (r && r != 704 /* ALREADY_ENABLED */) {
                    emit(tag, "FAIL", "cuCtxEnablePeerAccess rc=%d (%s)", r, errname(r));
                    continue;
                }
                CUmodule mod = NULL; CUfunction fn = NULL;
                r = p_cuModuleLoadData(&mod, peer_add_ptx);
                if (r) { emit(tag, "FAIL", "cuModuleLoadData rc=%d (%s)", r, errname(r)); goto disable; }
                r = p_cuModuleGetFunction(&fn, mod, "peer_add");
                if (r) { emit(tag, "FAIL", "cuModuleGetFunction rc=%d (%s)", r, errname(r)); goto disable; }

                CUdeviceptr out = 0;
                r = p_cuMemAlloc(&out, bytes);
                if (r) { emit(tag, "FAIL", "cuMemAlloc out rc=%d (%s)", r, errname(r)); goto disable; }
                p_cuMemsetD8(out, 0, bytes);

                /* a's own buffer + b's REMOTE buffer -> out on a */
                void *args[4]; int n = n_elem;
                CUdeviceptr pa = buf[a], pb = buf[b], po = out;
                args[0] = &pa; args[1] = &pb; args[2] = &po; args[3] = &n;
                unsigned threads = 256, blocks = (n + threads - 1) / threads;
                r = p_cuLaunchKernel(fn, blocks, 1, 1, threads, 1, 1, 0, NULL, args, NULL);
                if (r) { emit(tag, "FAIL", "cuLaunchKernel rc=%d (%s)", r, errname(r)); p_cuMemFree(out); goto disable; }
                r = p_cuCtxSynchronize();
                if (r) { emit(tag, "FAIL", "cuCtxSynchronize rc=%d (%s)", r, errname(r)); p_cuMemFree(out); goto disable; }

                memset(hdst, 0, bytes);
                r = p_cuMemcpyDtoH(hdst, out, bytes);
                if (r) { emit(tag, "FAIL", "readback rc=%d (%s)", r, errname(r)); p_cuMemFree(out); goto disable; }
                {
                    int bad = -1;
                    for (int i = 0; i < n_elem; i++) {
                        uint32_t va = ((uint32_t)(a + 1) << 24) ^ (uint32_t)(i * 2654435761u);
                        uint32_t vb = ((uint32_t)(b + 1) << 24) ^ (uint32_t)(i * 2654435761u);
                        if (hdst[i] != (uint32_t)(va + vb)) { bad = i; break; }
                    }
                    if (bad >= 0)
                        emit(tag, "FAIL", "mismatch at elem %d: got 0x%08x", bad, hdst[bad]);
                    else
                        emit(tag, "PASS", "kernel on dev %d read %zu bytes resident on dev %d, every element verified",
                             a, bytes, b);
                }
                p_cuMemFree(out);
            disable:
                if (mod) p_cuModuleUnload(mod);
                p_cuCtxSetCurrent(ctx[a]);
                if (p_cuCtxDisablePeerAccess) p_cuCtxDisablePeerAccess(ctx[b]);
            }
        }
    }

    for (int i = 0; i < ndev; i++) {
        p_cuCtxSetCurrent(ctx[i]);
        p_cuMemFree(buf[i]);
        p_cuCtxDestroy(ctx[i]);
    }
    free(hsrc); free(hdst);

    printf("TOTAL|pass=%d fail=%d skip=%d\n", n_pass, n_fail, n_skip);
    printf("VERDICT|%s\n", n_fail ? "FAIL" : "PASS");
    return n_fail ? 1 : 0;
}
