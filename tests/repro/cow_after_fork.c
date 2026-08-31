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

    /*
     * No pipes. An earlier version used two for handshaking and never closed
     * the unused ends, so when the child exited early the parent's read()
     * never saw EOF and blocked forever -- which looked exactly like a hang
     * inside cuInit. waitpid() is all the synchronisation this needs: the
     * child does everything and exits, then the parent inspects its own COW
     * copy, which by then must be untouched.
     */
    pid_t pid = fork();

    if (pid == 0) {                 /* CHILD does all the CUDA work */
        CUdevice dev; CUcontext ctx; CUresult rc;
        printf("  child: cuInit ...\n");      fflush(stdout);
        if (cuInit(0))                  { printf("  child: cuInit FAILED\n");      fflush(stdout); _exit(2); }
        printf("  child: cuDeviceGet ...\n"); fflush(stdout);
        if (cuDeviceGet(&dev,0))        { printf("  child: cuDeviceGet FAILED\n"); fflush(stdout); _exit(2); }
        printf("  child: cuCtxCreate ...\n"); fflush(stdout);
        if (cuCtxCreate_v2(&ctx,0,dev)) { printf("  child: cuCtxCreate FAILED\n"); fflush(stdout); _exit(2); }

        rc = cuMemHostRegister_v2((void*)buf, LEN, CU_MEMHOSTREGISTER_DEVICEMAP);
        printf("  child: register inherited COW heap rc=%d %s\n", rc,
               rc == 0 ? "ALLOWED" : "REFUSED");
        fflush(stdout);   /* the write below may fault; do not lose this line */
        if (rc == 0) {
            printf("  child: writing through the registered buffer ...\n");
            fflush(stdout);
            for (unsigned i = 0; i < LEN/4; i++) buf[i] = SENTINEL_C;
            printf("  child: wrote C; own view C=%u/%u\n", count_eq(buf, SENTINEL_C), LEN/4);
            cuMemHostUnregister((void*)buf);
        }
        fflush(stdout);
        _exit(rc == 0 ? 0 : 1);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    int crc = WIFEXITED(st) ? WEXITSTATUS(st) : 99;

    unsigned a = count_eq(buf, SENTINEL_A), cc = count_eq(buf, SENTINEL_C);
    printf("  parent: own copy -> A=%u C=%u (of %u)\n", a, cc, LEN/4);

    printf("\n");
    if (crc == 0 && a == LEN/4 && cc == 0)
        printf("VERDICT: PASS -- child registered inherited COW heap; parent's copy intact\n");
    else if (crc == 1)
        printf("VERDICT: FAIL -- refused; ordinary fork-then-CUDA heap use regressed\n");
    else if (crc == 2)
        printf("VERDICT: child could not bring CUDA up at all\n");
    else
        printf("VERDICT: registered but COW did not hold (A=%u C=%u)\n", a, cc);
    return crc;
}
