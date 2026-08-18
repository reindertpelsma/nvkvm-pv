/* SPDX-License-Identifier: GPL-2.0
 *
 * multi_gpu.c — does nvkvm expose and actually *drive* more than one GPU?
 *
 * validate.sh only ever touches device 0 (cuDeviceGet(&dev, 0)).  This probe
 * walks every device libcuda reports and, on each one, creates a context,
 * round-trips memory, JITs PTX and launches a kernel whose output is checked
 * element-by-element.  Enumerating a device proves nothing; only a verified
 * kernel result does.
 *
 * Modes:
 *   (no args)      probe every device libcuda reports
 *   --device N     probe only ordinal N (used for the CUDA_VISIBLE_DEVICES
 *                  concurrency test: two processes, one GPU each)
 *   --spin SEC     after the checked launch, keep relaunching for SEC seconds
 *                  so a concurrent run genuinely overlaps
 *
 * Output is line-oriented "CHECK|name|status|detail" like validate.sh.
 *
 * cc -O2 -o multi_gpu multi_gpu.c -ldl
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <dlfcn.h>
#include <time.h>
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
static CUresult (*p_cuDeviceGetAttribute)(int *, int, CUdevice);
static CUresult (*p_cuDeviceGetPCIBusId)(char *, int, CUdevice);
static CUresult (*p_cuDeviceTotalMem)(size_t *, CUdevice);
static CUresult (*p_cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*p_cuCtxDestroy)(CUcontext);
static CUresult (*p_cuCtxSetCurrent)(CUcontext);
static CUresult (*p_cuCtxGetDevice)(CUdevice *);
static CUresult (*p_cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*p_cuMemFree)(CUdeviceptr);
static CUresult (*p_cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
static CUresult (*p_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*p_cuMemsetD8)(CUdeviceptr, unsigned char, size_t);
static CUresult (*p_cuModuleLoadData)(CUmodule *, const void *);
static CUresult (*p_cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
static CUresult (*p_cuModuleUnload)(CUmodule);
static CUresult (*p_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                    unsigned, unsigned, unsigned, unsigned,
                                    CUstream, void **, void **);
static CUresult (*p_cuCtxSynchronize)(void);
static CUresult (*p_cuGetErrorName)(CUresult, const char **);

#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR 75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR 76
#define CU_DEVICE_ATTRIBUTE_PCI_BUS_ID     33
#define CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID  34
#define CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID  50
#define CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT 16

static void *H;
static void *sym2(const char *base) {
    char b[128]; void *s;
    snprintf(b, sizeof b, "%s_v2", base);
    s = dlsym(H, b);
    if (!s) s = dlsym(H, base);
    return s;
}
static const char *errname(CUresult r) {
    const char *n = NULL;
    if (p_cuGetErrorName && p_cuGetErrorName(r, &n) == 0 && n) return n;
    return "?";
}
static void emit(const char *name, const char *status, const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    printf("CHECK|%s|%s|%s\n", name, status, buf); fflush(stdout);
}

/* vec_add: c[i] = a[i] + b[i]; identical to the one validate.sh JITs. */
static const char vec_add_ptx[] =
"//\n"
".version 7.5\n"
".target sm_60\n"
".address_size 64\n"
"\n"
".visible .entry vec_add(\n"
"    .param .u64 vec_add_param_0,\n"
"    .param .u64 vec_add_param_1,\n"
"    .param .u64 vec_add_param_2,\n"
"    .param .u32 vec_add_param_3\n"
")\n"
"{\n"
"    .reg .pred  %p<2>;\n"
"    .reg .b32   %r<8>;\n"
"    .reg .b64   %rd<11>;\n"
"\n"
"    ld.param.u64    %rd1, [vec_add_param_0];\n"
"    ld.param.u64    %rd2, [vec_add_param_1];\n"
"    ld.param.u64    %rd3, [vec_add_param_2];\n"
"    ld.param.u32    %r2,  [vec_add_param_3];\n"
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

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Exercise one device end to end.  Returns 0 on success. */
static int probe_device(int ord, double spin_s)
{
    char tag[64];
    CUdevice dev = 0;
    CUcontext ctx = NULL;
    CUresult r;
    char name[256] = {0};
    char busid[64] = {0};
    int cc_maj = 0, cc_min = 0, smcount = 0;
    size_t totalmem = 0;

    snprintf(tag, sizeof tag, "gpu%d", ord);

    r = p_cuDeviceGet(&dev, ord);
    if (r) { emit(tag, "FAIL", "cuDeviceGet(%d) rc=%d (%s)", ord, r, errname(r)); return 1; }

    if (p_cuDeviceGetName) p_cuDeviceGetName(name, sizeof name, dev);
    if (p_cuDeviceGetPCIBusId) p_cuDeviceGetPCIBusId(busid, sizeof busid, dev);
    if (p_cuDeviceGetAttribute) {
        p_cuDeviceGetAttribute(&cc_maj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
        p_cuDeviceGetAttribute(&cc_min, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
        p_cuDeviceGetAttribute(&smcount, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev);
    }
    if (p_cuDeviceTotalMem) p_cuDeviceTotalMem(&totalmem, dev);

    emit(tag, "INFO", "handle=%d name='%s' pci=%s sm_%d%d smcount=%d totalmem=%zuMiB",
         (int)dev, name, busid[0] ? busid : "?", cc_maj, cc_min, smcount,
         totalmem >> 20);

    /* ---- context ---- */
    r = p_cuCtxCreate(&ctx, 0, dev);
    if (r) {
        emit(tag, "FAIL", "cuCtxCreate rc=%d (%s) -- device enumerates but no context",
             r, errname(r));
        return 1;
    }
    /* Confirm the context really landed on the device we asked for. */
    if (p_cuCtxGetDevice) {
        CUdevice back = -1;
        if (p_cuCtxGetDevice(&back) == 0 && back != dev) {
            emit(tag, "FAIL", "cuCtxGetDevice returned %d, asked for %d -- WRONG DEVICE",
                 (int)back, (int)dev);
            p_cuCtxDestroy(ctx);
            return 1;
        }
    }

    /* ---- 8 MiB round trip ---- */
    const size_t NB = 8u * 1024u * 1024u;
    unsigned char *src = malloc(NB), *dst = malloc(NB);
    CUdeviceptr d = 0;
    if (src && dst) {
        uint32_t s = 0x12345678u ^ (uint32_t)(ord * 0x9E3779B9u);
        for (size_t i = 0; i < NB; i++) { s = s * 1103515245u + 12345u; src[i] = (unsigned char)(s >> 16); }
        memset(dst, 0xCD, NB);
        r = p_cuMemAlloc(&d, NB);
        if (r) {
            emit(tag, "FAIL", "cuMemAlloc(%zu) rc=%d (%s)", NB, r, errname(r));
            p_cuCtxDestroy(ctx); free(src); free(dst); return 1;
        }
        CUresult rh = p_cuMemcpyHtoD(d, src, NB);
        CUresult rd = p_cuMemcpyDtoH(dst, d, NB);
        if (rh || rd) {
            emit(tag, "FAIL", "HtoD rc=%d DtoH rc=%d", rh, rd);
            p_cuMemFree(d); p_cuCtxDestroy(ctx); free(src); free(dst); return 1;
        }
        if (memcmp(src, dst, NB)) {
            size_t off = 0; while (off < NB && src[off] == dst[off]) off++;
            emit(tag, "FAIL", "8 MiB round trip differs, first at %zu", off);
            p_cuMemFree(d); p_cuCtxDestroy(ctx); free(src); free(dst); return 1;
        }
        emit(tag, "PASS", "memcpy 8 MiB HtoD/DtoH byte-exact on device %d", ord);
        p_cuMemFree(d);
        free(src); free(dst);
    }

    /* ---- PTX JIT + checked kernel ---- */
    CUmodule mod = NULL;
    r = p_cuModuleLoadData(&mod, vec_add_ptx);
    if (r) {
        emit(tag, "FAIL", "cuModuleLoadData(PTX) rc=%d (%s)", r, errname(r));
        p_cuCtxDestroy(ctx); return 1;
    }
    CUfunction fn = NULL;
    r = p_cuModuleGetFunction(&fn, mod, "vec_add");
    if (r) {
        emit(tag, "FAIL", "cuModuleGetFunction rc=%d (%s)", r, errname(r));
        p_cuModuleUnload(mod); p_cuCtxDestroy(ctx); return 1;
    }

    const int N = 1 << 20;
    size_t bytes = (size_t)N * sizeof(int);
    int *ha = malloc(bytes), *hb = malloc(bytes), *hc = malloc(bytes);
    CUdeviceptr da = 0, db = 0, dc = 0;
    int rc = 1;
    if (ha && hb && hc) {
        /* Per-device operands: device 1's expected result differs from
         * device 0's, so a launch silently routed to the wrong GPU (or a
         * stale buffer) cannot compare equal. */
        int bias = (ord + 1) * 1000003;
        for (int i = 0; i < N; i++) { ha[i] = i + bias; hb[i] = 2 * i - bias; hc[i] = -1; }
        CUresult e = 0;
        e |= p_cuMemAlloc(&da, bytes); e |= p_cuMemAlloc(&db, bytes); e |= p_cuMemAlloc(&dc, bytes);
        e |= p_cuMemcpyHtoD(da, ha, bytes); e |= p_cuMemcpyHtoD(db, hb, bytes);
        if (p_cuMemsetD8) e |= p_cuMemsetD8(dc, 0, bytes);
        if (e) {
            emit(tag, "FAIL", "kernel setup rc=%d", e);
        } else {
            int n = N;
            void *args[] = { &da, &db, &dc, &n };
            unsigned block = 256, grid = (unsigned)((N + block - 1) / block);
            r = p_cuLaunchKernel(fn, grid, 1, 1, block, 1, 1, 0, NULL, args, NULL);
            CUresult rs = p_cuCtxSynchronize();
            if (r || rs) {
                emit(tag, "FAIL", "cuLaunchKernel rc=%d sync rc=%d (%s)", r, rs, errname(r ? r : rs));
            } else if (p_cuMemcpyDtoH(hc, dc, bytes)) {
                emit(tag, "FAIL", "result DtoH failed");
            } else {
                long bad = 0; int firstbad = -1;
                for (int i = 0; i < N; i++) {
                    if (hc[i] != ha[i] + hb[i]) { if (!bad) firstbad = i; bad++; }
                }
                if (bad) {
                    emit(tag, "FAIL", "kernel result wrong: %ld/%d elements, first at %d (got %d want %d)",
                         bad, N, firstbad, hc[firstbad], ha[firstbad] + hb[firstbad]);
                } else {
                    emit(tag, "PASS", "vec_add kernel on device %d: %d/%d elements correct (bias=%d)",
                         ord, N, N, bias);
                    rc = 0;
                }
            }
        }
        /* keep the device busy so a concurrent peer really overlaps */
        if (rc == 0 && spin_s > 0) {
            double t0 = now_s(); long iters = 0;
            int n = N;
            void *args[] = { &da, &db, &dc, &n };
            unsigned block = 256, grid = (unsigned)((N + block - 1) / block);
            while (now_s() - t0 < spin_s) {
                if (p_cuLaunchKernel(fn, grid, 1, 1, block, 1, 1, 0, NULL, args, NULL)) { rc = 1; break; }
                if (p_cuCtxSynchronize()) { rc = 1; break; }
                iters++;
            }
            if (rc == 0 && p_cuMemcpyDtoH(hc, dc, bytes) == 0) {
                long bad = 0;
                for (int i = 0; i < N; i++) if (hc[i] != ha[i] + hb[i]) bad++;
                if (bad) { emit(tag, "FAIL", "spin result wrong: %ld bad", bad); rc = 1; }
                else emit(tag, "PASS", "spin %.1fs on device %d: %ld launches, final result still correct",
                          spin_s, ord, iters);
            } else if (rc) {
                emit(tag, "FAIL", "spin loop failed on device %d", ord);
            }
        }
        if (da) p_cuMemFree(da);
        if (db) p_cuMemFree(db);
        if (dc) p_cuMemFree(dc);
        free(ha); free(hb); free(hc);
    }
    p_cuModuleUnload(mod);
    p_cuCtxDestroy(ctx);
    return rc;
}

int main(int argc, char **argv)
{
    int only = -1;
    double spin = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) only = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spin") && i + 1 < argc) spin = atof(argv[++i]);
    }

    H = dlopen("libcuda.so.1", RTLD_NOW);
    if (!H) H = dlopen("libcuda.so", RTLD_NOW);
    if (!H) { emit("libcuda", "FAIL", "dlopen: %s", dlerror()); return 1; }
    emit("libcuda", "PASS", "loaded");

    p_cuInit               = dlsym(H, "cuInit");
    p_cuDriverGetVersion   = dlsym(H, "cuDriverGetVersion");
    p_cuDeviceGetCount     = dlsym(H, "cuDeviceGetCount");
    p_cuDeviceGet          = dlsym(H, "cuDeviceGet");
    p_cuDeviceGetName      = dlsym(H, "cuDeviceGetName");
    p_cuDeviceGetAttribute = dlsym(H, "cuDeviceGetAttribute");
    p_cuDeviceGetPCIBusId  = dlsym(H, "cuDeviceGetPCIBusId");
    p_cuDeviceTotalMem     = sym2("cuDeviceTotalMem");
    p_cuCtxCreate          = sym2("cuCtxCreate");
    p_cuCtxDestroy         = sym2("cuCtxDestroy");
    p_cuCtxSetCurrent      = dlsym(H, "cuCtxSetCurrent");
    p_cuCtxGetDevice       = dlsym(H, "cuCtxGetDevice");
    p_cuMemAlloc           = sym2("cuMemAlloc");
    p_cuMemFree            = sym2("cuMemFree");
    p_cuMemcpyHtoD         = sym2("cuMemcpyHtoD");
    p_cuMemcpyDtoH         = sym2("cuMemcpyDtoH");
    p_cuMemsetD8           = sym2("cuMemsetD8");
    p_cuModuleLoadData     = dlsym(H, "cuModuleLoadData");
    p_cuModuleGetFunction  = dlsym(H, "cuModuleGetFunction");
    p_cuModuleUnload       = dlsym(H, "cuModuleUnload");
    p_cuLaunchKernel       = dlsym(H, "cuLaunchKernel");
    p_cuCtxSynchronize     = dlsym(H, "cuCtxSynchronize");
    p_cuGetErrorName       = dlsym(H, "cuGetErrorName");

    if (!p_cuInit || !p_cuDeviceGetCount || !p_cuCtxCreate || !p_cuMemAlloc ||
        !p_cuLaunchKernel || !p_cuModuleLoadData) {
        emit("libcuda", "FAIL", "required symbols missing");
        return 1;
    }

    CUresult r = p_cuInit(0);
    if (r) { emit("cuda_init", "FAIL", "cuInit rc=%d (%s)", r, errname(r)); return 1; }
    emit("cuda_init", "PASS", "rc=0");

    if (p_cuDriverGetVersion) {
        int v = 0; p_cuDriverGetVersion(&v);
        emit("driver_version", "INFO", "%d.%d (%d)", v / 1000, (v % 1000) / 10, v);
    }

    int ndev = 0;
    r = p_cuDeviceGetCount(&ndev);
    if (r) { emit("device_count", "FAIL", "rc=%d (%s)", r, errname(r)); return 1; }
    emit("device_count", "INFO", "%d", ndev);

    const char *vis = getenv("CUDA_VISIBLE_DEVICES");
    emit("cuda_visible_devices", "INFO", "%s", vis ? vis : "(unset)");

    int fails = 0, ran = 0;
    if (only >= 0) {
        if (only >= ndev) {
            emit("device_count", "FAIL", "asked for ordinal %d but only %d device(s) visible", only, ndev);
            return 1;
        }
        fails += probe_device(only, spin); ran++;
    } else {
        for (int i = 0; i < ndev; i++) { fails += probe_device(i, spin); ran++; }
    }

    emit("summary", fails ? "FAIL" : "PASS", "%d/%d device(s) fully usable", ran - fails, ran);
    return fails ? 1 : 0;
}
