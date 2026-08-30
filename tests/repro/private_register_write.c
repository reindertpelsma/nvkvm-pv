/*
 * private_register_write.c -- is a MAP_PRIVATE (ordinary heap) buffer still
 * writable after cuMemHostRegister? No fork involved.
 *
 * cow_after_fork.c segfaults writing to a registered COW-inherited buffer.
 * migrate_range clears VM_MAYWRITE for COW mappings and then recomputes
 * vm_page_prot from the new flags -- and for a private mapping without
 * VM_MAYWRITE that is read-only, so remap_pfn_range installs read-only PTEs
 * and the write fault has no way to resolve.
 *
 * If that is the mechanism it has nothing to do with fork: any MAP_PRIVATE
 * buffer -- which is what malloc returns for a large block -- should fault the
 * same way. This checks both shapes in one process.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>

typedef int CUresult; typedef int CUdevice; typedef void *CUcontext;
extern CUresult cuInit(unsigned);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuCtxCreate_v2(CUcontext *, unsigned, CUdevice);
extern CUresult cuMemHostRegister_v2(void *, size_t, unsigned);
extern CUresult cuMemHostUnregister(void *);

#define LEN (256u*1024u)
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02

static sigjmp_buf jb; static volatile sig_atomic_t faulted;
static void onfault(int s){(void)s; faulted=1; siglongjmp(jb,1);}

static int try_shape(const char *name, int flags)
{
    volatile unsigned *b = mmap(NULL, LEN, PROT_READ|PROT_WRITE, flags, -1, 0);
    if (b == MAP_FAILED) { printf("  %-26s mmap failed\n", name); return 1; }
    for (unsigned i=0;i<LEN/4;i++) b[i]=0xAAAAAAAA;

    CUresult rc = cuMemHostRegister_v2((void*)b, LEN, CU_MEMHOSTREGISTER_DEVICEMAP);
    if (rc) { printf("  %-26s register REFUSED rc=%d\n", name, rc); munmap((void*)b,LEN); return 1; }

    faulted = 0;
    int ok = 0;
    if (sigsetjmp(jb,1) == 0) {
        b[0] = 0x5A5A5A5A;                 /* the write that matters */
        ok = (b[0] == 0x5A5A5A5A);
        printf("  %-26s register OK, write OK (readback 0x%08x)\n", name, b[0]);
    } else {
        printf("  %-26s register OK, but WRITE SEGFAULTED\n", name);
    }
    cuMemHostUnregister((void*)b);
    munmap((void*)b, LEN);
    return faulted ? 2 : (ok ? 0 : 1);
}

int main(void)
{
    CUdevice dev; CUcontext ctx;
    if (cuInit(0) || cuDeviceGet(&dev,0) || cuCtxCreate_v2(&ctx,0,dev)) {
        printf("CUDA bring-up failed\n"); return 2;
    }
    signal(SIGSEGV, onfault); signal(SIGBUS, onfault);

    int a = try_shape("MAP_SHARED|ANONYMOUS",  MAP_SHARED|MAP_ANONYMOUS);
    int p = try_shape("MAP_PRIVATE|ANONYMOUS", MAP_PRIVATE|MAP_ANONYMOUS);

    printf("\n");
    if (a == 0 && p == 0) printf("VERDICT: PASS -- both shapes writable after registration\n");
    else if (p == 2)      printf("VERDICT: BUG -- MAP_PRIVATE (ordinary heap) is unwritable\n"
                                 "         after registration; no fork required\n");
    else                  printf("VERDICT: shared=%d private=%d\n", a, p);
    return (a==0 && p==0) ? 0 : 1;
}
