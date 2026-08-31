/*
 * fork_both_register.c -- the realistic multi-process DMA shape.
 *
 * Parent holds TWO buffers and has not touched CUDA:
 *     A = MAP_PRIVATE|MAP_ANONYMOUS   (ordinary heap)
 *     B = MAP_SHARED |MAP_ANONYMOUS   (deliberately shared)
 * It forks. THEN both processes independently bring CUDA up and register BOTH
 * buffers for DMA, at the same virtual addresses. The child writes to both.
 *
 * The contract, which the stock driver defines:
 *     A  -- parent must NOT see the child's write   (private stays private)
 *     B  -- parent MUST see the child's write       (shared stays shared)
 *
 * Registration must not change either answer. Run against the real driver
 * first; that result is the specification, not this file's opinion.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

typedef int CUresult; typedef int CUdevice; typedef void *CUcontext;
extern CUresult cuInit(unsigned);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuCtxCreate_v2(CUcontext *, unsigned, CUdevice);
extern CUresult cuMemHostRegister_v2(void *, size_t, unsigned);
extern CUresult cuMemHostUnregister(void *);

#define LEN   (64u * 1024u)
#define BASE  0xAAAAAAAAu
#define CPAT  0xCCCCCCCCu
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02

static unsigned cnt(volatile unsigned *p, unsigned v)
{ unsigned n=0; for (unsigned i=0;i<LEN/4;i++) if (p[i]==v) n++; return n; }

int main(void)
{
    volatile unsigned *A = mmap(NULL, LEN, PROT_READ|PROT_WRITE,
                                MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    volatile unsigned *B = mmap(NULL, LEN, PROT_READ|PROT_WRITE,
                                MAP_SHARED |MAP_ANONYMOUS, -1, 0);
    if (A==MAP_FAILED || B==MAP_FAILED) { perror("mmap"); return 2; }
    for (unsigned i=0;i<LEN/4;i++) { A[i]=BASE; B[i]=BASE; }
    printf("  A (private) @ %p   B (shared) @ %p   -- no CUDA yet\n", A, B);

    int pipefd[2]; if (pipe(pipefd)) return 2;
    pid_t pid = fork();
    if (pid < 0) return 2;

    if (pid == 0) {                                  /* ---- child ---- */
        CUdevice d; CUcontext c; unsigned char m[3] = {0,0,0};
        m[0] = !(cuInit(0) || cuDeviceGet(&d,0) || cuCtxCreate_v2(&c,0,d));
        if (m[0]) {
            m[1] = (cuMemHostRegister_v2((void*)A, LEN, CU_MEMHOSTREGISTER_DEVICEMAP) == 0);
            m[2] = (cuMemHostRegister_v2((void*)B, LEN, CU_MEMHOSTREGISTER_DEVICEMAP) == 0);
        }
        for (unsigned i=0;i<LEN/4;i++) { A[i]=CPAT; B[i]=CPAT; }   /* child writes both */
        if (write(pipefd[1], m, 3) != 3) _exit(3);
        fflush(stdout);
        _exit(0);
    }

    CUdevice d; CUcontext c;                          /* ---- parent ---- */
    int pc = !(cuInit(0) || cuDeviceGet(&d,0) || cuCtxCreate_v2(&c,0,d));
    int ra=0, rb=0;
    if (pc) {
        ra = (cuMemHostRegister_v2((void*)A, LEN, CU_MEMHOSTREGISTER_DEVICEMAP) == 0);
        rb = (cuMemHostRegister_v2((void*)B, LEN, CU_MEMHOSTREGISTER_DEVICEMAP) == 0);
    }
    unsigned char m[3]={0,0,0};
    if (read(pipefd[0], m, 3) != 3) { printf("  child died\n"); return 2; }
    int st=0; waitpid(pid,&st,0);

    unsigned a_seen = cnt(A, CPAT), b_seen = cnt(B, CPAT);
    printf("  parent: cuda=%d  registerA=%d registerB=%d\n", pc, ra, rb);
    printf("  child : cuda=%d  registerA=%d registerB=%d\n", m[0], m[1], m[2]);
    printf("  parent sees child's write:  A(private)=%u/%u   B(shared)=%u/%u\n",
           a_seen, LEN/4, b_seen, LEN/4);

    int ok_a = (a_seen == 0);            /* private: must NOT propagate */
    int ok_b = (b_seen == LEN/4);        /* shared:  MUST propagate     */
    printf("\n  A private-stays-private : %s\n", ok_a ? "OK" : "WRONG (leaked)");
    printf("  B shared-stays-shared   : %s\n", ok_b ? "OK" : "WRONG (did not propagate)");
    printf("  CUDA up in both         : %s\n", (pc && m[0]) ? "OK" : "NO");
    printf("  all four registrations  : %s\n",
           (ra&&rb&&m[1]&&m[2]) ? "OK" : "NOT ALL SUCCEEDED");

    printf("\nVERDICT: %s\n",
           (ok_a && ok_b && pc && m[0] && ra && rb && m[1] && m[2])
             ? "PASS" : "FAIL");
    return (ok_a && ok_b && pc && m[0] && ra && rb && m[1] && m[2]) ? 0 : 1;
}
