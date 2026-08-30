/*
 * shared_view_desync.c -- is migrate_range's relocation safe on a mapping that
 * has MORE THAN ONE view?  No nesting involved: one guest, two processes.
 *
 * nvkvm makes a guest buffer visible to the host by copying its pages into a
 * memfd shared with the isolate and repointing the guest VMA at it
 * (VM_PFNMAP).  That is sound for a VMA with exactly one view.  This asks what
 * happens when a second view exists -- which is ordinary, not exotic:
 * MAP_SHARED|MAP_ANONYMOUS inherited across fork(), or a memfd shared between
 * cooperating processes.
 *
 * Protocol, parent and child sharing one MAP_SHARED mapping:
 *   1. parent fills the buffer with SENTINEL_A, both agree they see it
 *   2. parent calls cuMemHostRegister  <-- this is what triggers migration
 *   3. parent writes SENTINEL_B through its own pointer
 *   4. child reads
 *
 * PASS: child sees SENTINEL_B  -- the two views still refer to one buffer.
 * FAIL: child sees SENTINEL_A  -- the parent's view was relocated and the
 *       child was left behind, i.e. silent divergence with no error anywhere.
 *
 * Run it on plain nvkvm (L1). Run it on bare metal too: there is no migration
 * there, so bare metal is the control and MUST pass.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

/* driver API, declared locally so this needs no cuda.h */
typedef int CUresult; typedef int CUdevice; typedef void *CUcontext;
extern CUresult cuInit(unsigned);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuCtxCreate_v2(CUcontext *, unsigned, CUdevice);
extern CUresult cuMemHostRegister_v2(void *, size_t, unsigned);
extern CUresult cuMemHostUnregister(void *);

#define LEN        (256u * 1024u)
#define SENTINEL_A 0xAAAAAAAAu
#define SENTINEL_B 0x55555555u
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02

static void fill(volatile unsigned *p, unsigned v)
{
    for (unsigned i = 0; i < LEN / 4; i++) p[i] = v;
}
static unsigned count_eq(volatile unsigned *p, unsigned v)
{
    unsigned n = 0;
    for (unsigned i = 0; i < LEN / 4; i++) if (p[i] == v) n++;
    return n;
}

int main(void)
{
    /* Two views of ONE object: MAP_SHARED survives fork as a shared mapping. */
    volatile unsigned *buf = mmap(NULL, LEN, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); return 2; }

    /* Two pipes so the two processes step in lockstep; without this the child
     * can read before the parent has registered and the result means nothing. */
    int to_child[2], to_parent[2];
    if (pipe(to_child) || pipe(to_parent)) { perror("pipe"); return 2; }

    fill(buf, SENTINEL_A);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }

    if (pid == 0) {                       /* ---- child: the second view ---- */
        char c;
        /* confirm the shared mapping works at all before the interesting part */
        unsigned pre = count_eq(buf, SENTINEL_A);
        if (write(to_parent[1], "r", 1) != 1) _exit(3);
        if (read(to_child[0], &c, 1) != 1)    _exit(3);
        unsigned post_b = count_eq(buf, SENTINEL_B);
        unsigned post_a = count_eq(buf, SENTINEL_A);
        printf("  child: before register saw A in %u/%u words\n", pre, LEN/4);
        printf("  child: after  parent wrote B -> B in %u, still A in %u (of %u)\n",
               post_b, post_a, LEN/4);
        if (post_b == LEN/4)      printf("  child: PASS -- both views are one buffer\n");
        else if (post_a == LEN/4) printf("  child: FAIL -- STALE: parent's view was relocated, child left behind\n");
        else                      printf("  child: FAIL -- partially diverged\n");
        _exit(post_b == LEN/4 ? 0 : 1);
    }

    char c;                               /* ---- parent: the CUDA side ---- */
    if (read(to_parent[0], &c, 1) != 1) return 2;

    CUresult rc; CUdevice dev; CUcontext ctx;
    if ((rc = cuInit(0)))                     { printf("cuInit rc=%d\n", rc);        return 2; }
    if ((rc = cuDeviceGet(&dev, 0)))          { printf("cuDeviceGet rc=%d\n", rc);   return 2; }
    if ((rc = cuCtxCreate_v2(&ctx, 0, dev)))  { printf("cuCtxCreate rc=%d\n", rc);   return 2; }

    rc = cuMemHostRegister_v2((void *)buf, LEN, CU_MEMHOSTREGISTER_DEVICEMAP);
    printf("  parent: cuMemHostRegister rc=%d %s\n", rc,
           rc == 0 ? "(registered -- migration ran)" : "(REFUSED)");

    fill(buf, SENTINEL_B);                /* parent writes through ITS view */
    printf("  parent: wrote B; parent's own view has B in %u/%u words\n",
           count_eq(buf, SENTINEL_B), LEN/4);

    if (write(to_child[1], "g", 1) != 1) return 2;
    int st = 0; waitpid(pid, &st, 0);
    if (rc == 0) cuMemHostUnregister((void *)buf);

    int child_rc = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
    printf("\nVERDICT: %s\n", child_rc == 0 ? "PASS -- views stayed coherent"
                                            : "FAIL -- the two views diverged");
    return child_rc;
}
