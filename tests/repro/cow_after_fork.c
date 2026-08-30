/*
 * cow_after_fork.c -- heap allocated before CUDA exists, then registered by a
 * forked child.
 *
 * The realistic shape: a process that knows nothing about CUDA allocates heap,
 * forks, and the CHILD initialises CUDA and registers the inherited buffer.
 * The pages are shared copy-on-write at that moment, so a naive
 * "is anything else mapping this?" check would refuse it -- but the sharing is
 * COW, the memory is logically private, and a write breaks it apart. The stock
 * NVIDIA driver handles this correctly, so nvkvm must too.
 *
 * Run the same binary against the real driver as a control.
 *
 * PASS: child's registration allowed, and the parent's copy is untouched by
 *       the child's writes (COW held).
 * FAIL: refused -- an ordinary pattern regressed.
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

#define LEN        (256u * 1024u)
#define SENTINEL_A 0xAAAAAAAAu
#define SENTINEL_C 0xCCCCCCCCu
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02

static unsigned count_eq(volatile unsigned *p, unsigned v)
{ unsigned n=0; for (unsigned i=0;i<LEN/4;i++) if (p[i]==v) n++; return n; }

int main(void)
{
    /* Heap, allocated before anything CUDA-shaped exists. */
    volatile unsigned *buf = mmap(NULL, LEN, PROT_READ|PROT_WRITE,
                                  MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); return 2; }
    for (unsigned i = 0; i < LEN/4; i++) buf[i] = SENTINEL_A;

    int tp[2], tc[2]; if (pipe(tp)||pipe(tc)) return 2;
    pid_t pid = fork();

    if (pid == 0) {                 /* CHILD does all the CUDA work */
        CUdevice dev; CUcontext ctx; CUresult rc;
        printf("  child: cuInit...\n"); fflush(stdout);
        if (cuInit(0))                    { printf("  child: cuInit FAILED\n");      fflush(stdout); _exit(2); }
        if (cuDeviceGet(&dev,0))          { printf("  child: cuDeviceGet FAILED\n"); fflush(stdout); _exit(2); }
        if (cuCtxCreate_v2(&ctx,0,dev))   { printf("  child: cuCtxCreate FAILED\n"); fflush(stdout); _exit(2); }

        rc = cuMemHostRegister_v2((void*)buf, LEN, CU_MEMHOSTREGISTER_DEVICEMAP);
        printf("  child: register inherited COW heap rc=%d %s\n", rc,
               rc == 0 ? "ALLOWED" : "REFUSED");
        if (rc == 0) {
            for (unsigned i = 0; i < LEN/4; i++) buf[i] = SENTINEL_C;
            printf("  child: wrote C; own view C=%u/%u\n", count_eq(buf, SENTINEL_C), LEN/4);
            cuMemHostUnregister((void*)buf);
        }
        fflush(stdout);
        if (write(tp[1], rc == 0 ? "y" : "n", 1) != 1) _exit(3);
        char c; if (read(tc[0],&c,1)!=1) _exit(3);
        _exit(rc == 0 ? 0 : 1);
    }

    char v;
    if (read(tp[0],&v,1)!=1) { printf("  parent: child died before reporting\n"); return 2; }
    unsigned a = count_eq(buf, SENTINEL_A), cc = count_eq(buf, SENTINEL_C);
    printf("  parent: own copy -> A=%u C=%u (of %u)\n", a, cc, LEN/4);
    if (write(tc[1],"g",1)!=1) return 2;
    int st=0; waitpid(pid,&st,0);
    int crc = WIFEXITED(st)?WEXITSTATUS(st):99;

    printf("\n");
    if (v == 'y' && a == LEN/4 && cc == 0)
        printf("VERDICT: PASS -- child registered inherited COW heap; parent's copy intact\n");
    else if (v == 'n')
        printf("VERDICT: FAIL -- refused; ordinary fork-then-CUDA heap use regressed\n");
    else
        printf("VERDICT: registered but COW did not hold (parent A=%u C=%u)\n", a, cc);
    return crc;
}
