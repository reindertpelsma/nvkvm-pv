/*
 * fork_mapping_semantics.c -- does registration preserve the mapping's own
 * fork contract?
 *
 * Registration rewrites the VMA: migrate_range retypes it to VM_PFNMAP|VM_IO
 * over the host window and, for a COW mapping, sets VM_SHARED|VM_MAYSHARE.
 * That last part is what makes a MAP_PRIVATE buffer writable again after
 * registration -- but if it also makes it *shared*, an application that mapped
 * MAP_PRIVATE and forked would start seeing its child's writes. That is a
 * silent semantic break, and nothing else tests it.
 *
 * The contract fork already gives, which registration must not alter:
 *   MAP_SHARED   -> parent and child see each other's writes
 *   MAP_PRIVATE  -> each sees ONLY its own writes (copy-on-write)
 *
 * Both are checked here with the SAME sequence: register first, then fork,
 * then both sides write and both sides read. And both processes drive CUDA,
 * at deliberately overlapping VAs, since that is the realistic shape.
 *
 * PASS: shared behaves shared, private behaves private.
 * FAIL: private leaked (registration converted it), or shared did not share.
 *
 * NOT asserted: that the CHILD can drive CUDA. Measured on the stock NVIDIA
 * driver, a forked child cannot initialise CUDA once the parent holds a
 * context -- documented NVIDIA behaviour, nothing to do with nvkvm. Requiring
 * it made this test fail against the reference implementation. The child's
 * CUDA result is reported for information and does not affect the verdict;
 * two INDEPENDENT processes sharing an fd (the /dev/nvkvm-mem case) is a
 * different shape and is not what fork tests.
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

#define LEN   (256u * 1024u)
#define P_PAT 0x11111111u      /* parent writes this after fork */
#define C_PAT 0x22222222u      /* child writes this after fork  */
#define BASE  0xAAAAAAAAu      /* both start from this          */
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02

static unsigned count_eq(volatile unsigned *p, unsigned v)
{ unsigned n=0; for (unsigned i=0;i<LEN/4;i++) if (p[i]==v) n++; return n; }

/* returns 0 on the expected behaviour, 1 otherwise */
static int run_case(const char *name, int mapflags, int expect_shared)
{
    printf("\n--- %s (expect %s) ---\n", name, expect_shared ? "SHARED" : "PRIVATE/COW");

    volatile unsigned *buf = mmap(NULL, LEN, PROT_READ|PROT_WRITE, mapflags, -1, 0);
    if (buf == MAP_FAILED) { perror("  mmap"); return 1; }
    for (unsigned i=0;i<LEN/4;i++) buf[i] = BASE;

    CUdevice dev; CUcontext ctx;
    if (cuInit(0) || cuDeviceGet(&dev,0) || cuCtxCreate_v2(&ctx,0,dev)) {
        printf("  CUDA bring-up failed\n"); return 1;
    }
    CUresult rc = cuMemHostRegister_v2((void*)buf, LEN, CU_MEMHOSTREGISTER_DEVICEMAP);
    printf("  parent: register rc=%d %s\n", rc, rc?"REFUSED":"ok");
    if (rc) return 1;

    /* register THEN fork -- the ordering that exercises the conversion */
    int pipefd[2]; if (pipe(pipefd)) { perror("  pipe"); return 1; }
    pid_t pid = fork();
    if (pid < 0) { perror("  fork"); return 1; }

    if (pid == 0) {
        /* Child drives CUDA too, on the SAME VA -- the overlapping-VA case. */
        CUdevice d2; CUcontext c2;
        int cuda_ok = !(cuInit(0) || cuDeviceGet(&d2,0) || cuCtxCreate_v2(&c2,0,d2));
        for (unsigned i=0;i<LEN/4;i++) buf[i] = C_PAT;      /* child's write */
        unsigned char msg[2];
        msg[0] = cuda_ok;
        /* does the child see the parent's write? parent wrote before we did,
         * so under COW we must see none of it. */
        msg[1] = (count_eq(buf, P_PAT) > 0);
        if (write(pipefd[1], msg, 2) != 2) _exit(3);
        fflush(stdout);
        _exit(0);
    }

    for (unsigned i=0;i<LEN/4;i++) buf[i] = P_PAT;          /* parent's write */
    unsigned char msg[2] = {0,0};
    if (read(pipefd[0], msg, 2) != 2) { printf("  child died\n"); return 1; }
    int st=0; waitpid(pid,&st,0);

    unsigned parent_sees_child = count_eq(buf, C_PAT);
    printf("  child: CUDA up=%d, saw parent's pattern=%d\n", msg[0], msg[1]);
    printf("  parent: sees child's pattern in %u/%u words\n", parent_sees_child, LEN/4);

    cuMemHostUnregister((void*)buf);
    munmap((void*)buf, LEN);

    int shared_observed = (parent_sees_child > 0) || msg[1];
    if (shared_observed == expect_shared) {
        printf("  => OK: behaves %s (child CUDA %s -- informational, the stock\n"
               "     driver also refuses CUDA in a forked child)\n",
               expect_shared?"shared":"private", msg[0]?"up":"unavailable");
        return 0;
    }
    printf("  => WRONG: expected %s, observed %s\n",
           expect_shared?"shared":"private", shared_observed?"shared":"private");
    return 1;
}

int main(void)
{
    int a = run_case("MAP_SHARED|MAP_ANONYMOUS",  MAP_SHARED|MAP_ANONYMOUS,  1);
    int b = run_case("MAP_PRIVATE|MAP_ANONYMOUS", MAP_PRIVATE|MAP_ANONYMOUS, 0);
    printf("\nVERDICT: %s\n", (a==0 && b==0)
        ? "PASS -- registration preserves each mapping's fork contract"
        : "FAIL -- registration changed fork semantics");
    return (a==0 && b==0) ? 0 : 1;
}
