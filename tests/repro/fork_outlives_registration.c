/*
 * fork_outlives_registration.c -- what does a forked child hold after the
 * parent unregisters?
 *
 * register-then-fork is coherent (register_then_fork.c). That leaves the child
 * holding a mapping of WINDOW memory it never registered. This asks the
 * lifetime question:
 *
 *   parent: mmap -> cuMemHostRegister -> fork -> cuMemHostUnregister
 *   child:  reads the buffer afterwards
 *
 * Acceptable outcomes:
 *   - the child's mapping is revoked (SIGSEGV/SIGBUS on access), or
 *   - the child still reads its own data and the window range stays reserved
 *     while it does.
 *
 * Unacceptable: the child reads window memory after nvkvm considers it free,
 * because that range can then be handed to another registration -- which is
 * cross-process disclosure, strictly worse than corrupting one process's own
 * buffer.
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

#define LEN        (256u * 1024u)
#define SENTINEL_A 0xAAAAAAAAu
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02

static sigjmp_buf jb;
static volatile sig_atomic_t faulted;
static void onfault(int s) { (void)s; faulted = 1; siglongjmp(jb, 1); }

int main(void)
{
    volatile unsigned *buf = mmap(NULL, LEN, PROT_READ|PROT_WRITE,
                                  MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); return 2; }
    for (unsigned i = 0; i < LEN/4; i++) buf[i] = SENTINEL_A;

    CUresult rc; CUdevice dev; CUcontext ctx;
    if (cuInit(0) || cuDeviceGet(&dev,0) || cuCtxCreate_v2(&ctx,0,dev)) return 2;
    rc = cuMemHostRegister_v2((void *)buf, LEN, CU_MEMHOSTREGISTER_DEVICEMAP);
    printf("  register rc=%d\n", rc);
    if (rc) return 2;

    int to_parent[2], to_child[2];
    if (pipe(to_parent) || pipe(to_child)) return 2;

    pid_t pid = fork();
    if (pid == 0) {
        char c;
        if (write(to_parent[1], "r", 1) != 1) _exit(3);
        if (read(to_child[0], &c, 1) != 1)    _exit(3);   /* parent has unregistered */

        signal(SIGSEGV, onfault); signal(SIGBUS, onfault);
        unsigned v = 0;
        if (sigsetjmp(jb, 1) == 0) {
            v = buf[0];
            printf("  child: read AFTER parent unregistered -> 0x%08x  %s\n", v,
                   v == SENTINEL_A ? "(its own data)" : "(NOT its data!)");
        } else {
            printf("  child: access faulted after unregister (mapping revoked)\n");
        }
        fflush(stdout);
        _exit(faulted ? 0 : (v == SENTINEL_A ? 10 : 20));
    }

    char c;
    if (read(to_parent[0], &c, 1) != 1) return 2;
    rc = cuMemHostUnregister((void *)buf);
    printf("  parent: cuMemHostUnregister rc=%d\n", rc);
    if (write(to_child[1], "g", 1) != 1) return 2;

    int st = 0; waitpid(pid, &st, 0);
    int crc = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
    printf("\n");
    switch (crc) {
    case 0:  printf("VERDICT: SAFE -- child's mapping was revoked with the registration\n"); break;
    case 10: printf("VERDICT: child still reads its OWN data. Safe only if the window\n"
                    "         range stays reserved while that mapping lives -- otherwise\n"
                    "         the range can be re-handed and this becomes disclosure.\n"); break;
    case 20: printf("VERDICT: UNSAFE -- child read data that is not its own\n"); break;
    default: printf("VERDICT: child exited %d (killed by signal / error)\n", crc);
    }
    return crc == 20 ? 1 : 0;
}
