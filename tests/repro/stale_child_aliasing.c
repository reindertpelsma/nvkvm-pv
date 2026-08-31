/*
 * stale_child_aliasing.c -- can a forked child's stale window mapping come to
 * alias a LATER registration?
 *
 * fork_outlives_registration.c showed a child keeps a readable window mapping
 * after the parent unregisters. That is safe only if the GPA is never handed
 * out again. It is not obviously so: nvkvm_sparse_gpa_release() maintains a
 * free list, and the quarantine (NVKVM_GPA_QUAR_MAX=64 extents,
 * NVKVM_GPA_QUAR_BYTES=64 MiB) only DELAYS reuse.
 *
 * So: register, fork, unregister, then churn enough registrations to drain the
 * quarantine and force reuse, each filled with a distinctive pattern. Then the
 * child reads its stale mapping.
 *
 *   child sees its own 0xAAAAAAAA  -> range not reused (or still quarantined)
 *   child faults                   -> mapping revoked; safe
 *   child sees 0xDEADBEEF          -> DISCLOSURE: it is reading another
 *                                     registration's data
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/wait.h>

typedef int CUresult; typedef int CUdevice; typedef void *CUcontext;
extern CUresult cuInit(unsigned);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuCtxCreate_v2(CUcontext *, unsigned, CUdevice);
extern CUresult cuMemHostRegister_v2(void *, size_t, unsigned);
extern CUresult cuMemHostUnregister(void *);

#define LEN        (4u * 1024u * 1024u)      /* 4 MiB: 20 of these > 64 MiB quarantine */
#define CHURN      24
#define SENTINEL_A 0xAAAAAAAAu
#define POISON     0xDEADBEEFu
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02

static sigjmp_buf jb; static volatile sig_atomic_t faulted;
static void onfault(int s){(void)s; faulted=1; siglongjmp(jb,1);}

int main(void)
{
    volatile unsigned *buf = mmap(NULL, LEN, PROT_READ|PROT_WRITE,
                                  MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); return 2; }
    for (unsigned i = 0; i < LEN/4; i++) buf[i] = SENTINEL_A;

    CUdevice dev; CUcontext ctx;
    if (cuInit(0) || cuDeviceGet(&dev,0) || cuCtxCreate_v2(&ctx,0,dev)) return 2;
    if (cuMemHostRegister_v2((void*)buf, LEN, CU_MEMHOSTREGISTER_DEVICEMAP)) return 2;
    printf("  registered 4 MiB, filled 0xAAAAAAAA\n");

    int tp[2], tc[2]; if (pipe(tp)||pipe(tc)) return 2;
    pid_t pid = fork();
    if (pid == 0) {
        char c;
        if (write(tp[1],"r",1)!=1) _exit(3);
        if (read(tc[0],&c,1)!=1)   _exit(3);
        signal(SIGSEGV,onfault); signal(SIGBUS,onfault);
        unsigned v=0, poison=0, own=0;
        if (sigsetjmp(jb,1)==0) {
            for (unsigned i=0;i<LEN/4;i++){ v=buf[i]; if(v==POISON)poison++; else if(v==SENTINEL_A)own++; }
            printf("  child: own=%u poison=%u other=%u (of %u words)\n",
                   own, poison, LEN/4-own-poison, LEN/4);
        } else printf("  child: faulted -- mapping revoked\n");
        fflush(stdout);
        _exit(faulted ? 0 : (poison ? 20 : 10));
    }

    char c; if (read(tp[0],&c,1)!=1) return 2;
    if (cuMemHostUnregister((void*)buf)) return 2;
    printf("  parent: unregistered; churning %d x 4 MiB to drain the quarantine\n", CHURN);

    for (int i = 0; i < CHURN; i++) {
        volatile unsigned *p = mmap(NULL, LEN, PROT_READ|PROT_WRITE,
                                    MAP_SHARED|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) break;
        for (unsigned k = 0; k < LEN/4; k++) p[k] = POISON;
        if (cuMemHostRegister_v2((void*)p, LEN, CU_MEMHOSTREGISTER_DEVICEMAP) == 0) {
            for (unsigned k = 0; k < LEN/4; k++) p[k] = POISON;  /* poison the window copy */
            cuMemHostUnregister((void*)p);
        }
        munmap((void*)p, LEN);
    }
    printf("  parent: churn done\n");
    if (write(tc[1],"g",1)!=1) return 2;

    int st=0; waitpid(pid,&st,0);
    int crc = WIFEXITED(st)?WEXITSTATUS(st):99;
    printf("\n");
    switch (crc) {
    case 0:  printf("VERDICT: SAFE -- stale mapping revoked\n"); break;
    case 10: printf("VERDICT: SAFE (observed) -- child still sees only its own data;\n"
                    "         the range was not re-handed during this run\n"); break;
    case 20: printf("VERDICT: **DISCLOSURE** -- child read another registration's data\n"); break;
    default: printf("VERDICT: child exited %d\n", crc);
    }
    return crc == 20 ? 1 : 0;
}
