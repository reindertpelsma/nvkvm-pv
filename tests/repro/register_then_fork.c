/*
 * register_then_fork.c -- does fork still work if the registration happens
 * FIRST?
 *
 * shared_view_desync.c shows the broken order: fork, then register. Two shmem
 * views exist at registration time, only the registering VMA is relocated, and
 * the other is left on the old pages. That is now refused outright
 * (page_mapcount() > 1).
 *
 * This is the other order. Register while there is exactly one view -- allowed
 * -- so the buffer moves into the window and the VMA becomes a window mapping.
 * THEN fork. The child inherits that mapping, so both processes should be
 * looking at the same window pages and stay coherent with no relocation left
 * to do.
 *
 * PASS: child sees the parent's post-fork writes.
 * FAIL: child is stale -> even this order is broken, and fork is unusable with
 *       registered memory at all.
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

#define LEN        (256u * 1024u)
#define SENTINEL_A 0xAAAAAAAAu
#define SENTINEL_B 0x55555555u
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02

static void fill(volatile unsigned *p, unsigned v)
{ for (unsigned i = 0; i < LEN/4; i++) p[i] = v; }
static unsigned count_eq(volatile unsigned *p, unsigned v)
{ unsigned n = 0; for (unsigned i = 0; i < LEN/4; i++) if (p[i] == v) n++; return n; }

int main(void)
{
    volatile unsigned *buf = mmap(NULL, LEN, PROT_READ|PROT_WRITE,
                                  MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); return 2; }
    fill(buf, SENTINEL_A);

    CUresult rc; CUdevice dev; CUcontext ctx;
    if ((rc = cuInit(0)))                    { printf("cuInit rc=%d\n", rc);      return 2; }
    if ((rc = cuDeviceGet(&dev, 0)))         { printf("cuDeviceGet rc=%d\n", rc); return 2; }
    if ((rc = cuCtxCreate_v2(&ctx, 0, dev))) { printf("cuCtxCreate rc=%d\n", rc); return 2; }

    /* Register FIRST -- exactly one view exists, so this is allowed. */
    rc = cuMemHostRegister_v2((void *)buf, LEN, CU_MEMHOSTREGISTER_DEVICEMAP);
    printf("  cuMemHostRegister (single view) rc=%d %s\n", rc,
           rc == 0 ? "(allowed -- buffer relocated to the window)" : "(REFUSED)");
    if (rc != 0) {
        printf("\nRESULT: cannot even register a single-view buffer; test says nothing\n");
        return 2;
    }

    int to_parent[2], to_child[2];
    if (pipe(to_parent) || pipe(to_child)) { perror("pipe"); return 2; }

    pid_t pid = fork();
    if (pid == 0) {                          /* child: inherited the window VMA */
        char c;
        unsigned pre = count_eq(buf, SENTINEL_A);
        if (write(to_parent[1], "r", 1) != 1) { fflush(stdout); _exit(3); }
        if (read(to_child[0], &c, 1) != 1)    { fflush(stdout); _exit(3); }
        unsigned b = count_eq(buf, SENTINEL_B), a = count_eq(buf, SENTINEL_A);
        printf("  child: before=%u/%u saw A;  after parent wrote B -> B=%u A=%u\n",
               pre, LEN/4, b, a);
        fflush(stdout);
        _exit(b == LEN/4 ? 0 : 1);
    }

    char c;
    if (read(to_parent[0], &c, 1) != 1) return 2;
    fill(buf, SENTINEL_B);                   /* parent writes through the window */
    printf("  parent: wrote B; own view has B in %u/%u\n", count_eq(buf, SENTINEL_B), LEN/4);
    if (write(to_child[1], "g", 1) != 1) return 2;
    int st = 0; waitpid(pid, &st, 0);

    int crc = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
    printf("\nVERDICT: %s\n", crc == 0
        ? "PASS -- register-then-fork stays coherent"
        : "FAIL -- the child diverged even in this order");
    return crc;
}
