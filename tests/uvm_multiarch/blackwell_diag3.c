/*
 * blackwell_diag3.c — WHICH PTX kills the context, and does it happen on bare
 * metal too?
 *
 * Established: on an RTX 5060 (sm_120) the context is alive right after
 * cuCtxCreate and DEAD right after cuModuleLoadData — which returns 0.  No
 * kernel has been launched at that point, so nothing about the kernel, grid or
 * memory kind can be the cause; the JIT is.
 *
 * Two things left to separate:
 *   * WHICH PTX.  cuda_micro's module (`wr` + `noop`, store-only) JITs fine;
 *     managed_ladder's (`inc`, load-modify-store) is the suspect.
 *   * WHOSE BUG.  Run the identical binary on the BOX (bare metal, no nvkvm in
 *     the path) and in the guest.  If bare metal survives what the guest does
 *     not, it is nvkvm's; if both die, it is the driver's JIT on this silicon
 *     and nvkvm is only the messenger.
 *
 *   cc -O2 -o /tmp/bwd3 blackwell_diag3.c -ldl && /tmp/bwd3
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUmodule;

static CUresult (*cuInit)(unsigned);
static CUresult (*cuDeviceGet)(CUdevice *, int);
static CUresult (*cuDeviceGetName)(char *, int, CUdevice);
static CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*cuCtxDestroy)(CUcontext);
static CUresult (*cuModuleLoadData)(CUmodule *, const void *);
static CUresult (*cuCtxSynchronize)(void);
static CUresult (*cuGetErrorString)(CUresult, const char **);

static void *H;
static void *sym2(const char *base)
{
	char b[128]; void *s;
	snprintf(b, sizeof b, "%s_v2", base);
	s = dlsym(H, b);
	return s ? s : dlsym(H, base);
}
static const char *es(CUresult r)
{ const char *s = "?"; if (cuGetErrorString) cuGetErrorString(r, &s); return s ? s : "?"; }

/* managed_ladder's module, verbatim: one entry, load-modify-store. */
static const char ptx_inc[] =
".version 6.0\n.target sm_52\n.address_size 64\n"
".visible .entry inc(.param .u64 p, .param .u32 n){\n"
" .reg .pred %p<2>; .reg .b32 %r<8>; .reg .b64 %rd<5>;\n"
" ld.param.u64 %rd1,[p]; ld.param.u32 %r2,[n];\n"
" cvta.to.global.u64 %rd2,%rd1;\n"
" mov.u32 %r3,%ntid.x; mov.u32 %r4,%ctaid.x; mov.u32 %r5,%tid.x;\n"
" mad.lo.s32 %r1,%r4,%r3,%r5;\n"
" setp.ge.s32 %p1,%r1,%r2; @%p1 bra END;\n"
" mul.wide.s32 %rd3,%r1,4; add.s64 %rd4,%rd2,%rd3;\n"
" ld.global.u32 %r6,[%rd4]; add.s32 %r7,%r6,1; st.global.u32 [%rd4],%r7;\n"
"END: ret;\n}\n";

/* cuda_micro's module, verbatim: store-only, plus an empty entry. */
static const char ptx_wr[] =
".version 6.0\n.target sm_52\n.address_size 64\n"
".visible .entry wr(.param .u64 p, .param .u32 n){\n"
" .reg .pred %p<2>; .reg .b32 %r<6>; .reg .b64 %rd<5>;\n"
" ld.param.u64 %rd1,[p]; ld.param.u32 %r2,[n];\n"
" cvta.to.global.u64 %rd2,%rd1;\n"
" mov.u32 %r3,%ntid.x; mov.u32 %r4,%ctaid.x; mov.u32 %r5,%tid.x;\n"
" mad.lo.s32 %r1,%r4,%r3,%r5;\n"
" setp.ge.s32 %p1,%r1,%r2; @%p1 bra END;\n"
" mul.wide.s32 %rd3,%r1,4; add.s64 %rd4,%rd2,%rd3; st.global.u32 [%rd4],%r1;\n"
"END: ret;\n}\n"
".visible .entry noop(){ ret; }\n";

/* The same kernel as ptx_inc but declared for a newer virtual architecture. */
static const char ptx_inc75[] =
".version 6.4\n.target sm_75\n.address_size 64\n"
".visible .entry inc(.param .u64 p, .param .u32 n){\n"
" .reg .pred %p<2>; .reg .b32 %r<8>; .reg .b64 %rd<5>;\n"
" ld.param.u64 %rd1,[p]; ld.param.u32 %r2,[n];\n"
" cvta.to.global.u64 %rd2,%rd1;\n"
" mov.u32 %r3,%ntid.x; mov.u32 %r4,%ctaid.x; mov.u32 %r5,%tid.x;\n"
" mad.lo.s32 %r1,%r4,%r3,%r5;\n"
" setp.ge.s32 %p1,%r1,%r2; @%p1 bra END;\n"
" mul.wide.s32 %rd3,%r1,4; add.s64 %rd4,%rd2,%rd3;\n"
" ld.global.u32 %r6,[%rd4]; add.s32 %r7,%r6,1; st.global.u32 [%rd4],%r7;\n"
"END: ret;\n}\n";

/* A fresh context per case: a context killed by one JIT must not be blamed on
 * the next. */
static void case_of(const char *name, const char *ptx, CUdevice dev)
{
	CUcontext ctx = NULL; CUmodule m = NULL; int rc, before, after;

	if ((rc = cuCtxCreate(&ctx, 0, dev))) {
		printf("  %-24s cuCtxCreate=%d (%s)\n", name, rc, es(rc)); return; }
	before = cuCtxSynchronize();
	rc = cuModuleLoadData(&m, ptx);
	after = cuCtxSynchronize();
	printf("  %-24s sync-before=%d  cuModuleLoadData=%d  sync-after=%d (%s)\n",
	       name, before, rc, after, es(after));
	if (cuCtxDestroy) cuCtxDestroy(ctx);
}

int main(int argc, char **argv)
{
	CUdevice dev; int rc; char nm[128] = "?";
	int only = argc > 1 ? atoi(argv[1]) : -1;

	H = dlopen("libcuda.so.1", RTLD_NOW);
	if (!H) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
	cuInit = sym2("cuInit"); cuDeviceGet = sym2("cuDeviceGet");
	cuDeviceGetName = sym2("cuDeviceGetName");
	cuCtxCreate = sym2("cuCtxCreate"); cuCtxDestroy = sym2("cuCtxDestroy");
	cuModuleLoadData = sym2("cuModuleLoadData");
	cuCtxSynchronize = sym2("cuCtxSynchronize");
	cuGetErrorString = sym2("cuGetErrorString");

	if ((rc = cuInit(0)))            { printf("cuInit=%d\n", rc); return 1; }
	if ((rc = cuDeviceGet(&dev, 0))) { printf("cuDeviceGet=%d\n", rc); return 1; }
	if (cuDeviceGetName) cuDeviceGetName(nm, sizeof nm, dev);
	printf("device: %s\n", nm);

	/* ONE CASE PER PROCESS.  The first bad JIT poisons the whole process —
	 * every later cuCtxCreate hands back a context that is already destroyed —
	 * so running all three in one process makes cases 2 and 3 unreadable. */
	if (only == 0 || only < 0) case_of("ladder inc (sm_52)",  ptx_inc,   dev);
	if (only == 1 || only < 0) case_of("micro  wr+noop(52)",  ptx_wr,    dev);
	if (only == 2 || only < 0) case_of("ladder inc (sm_75)",  ptx_inc75, dev);
	return 0;
}
