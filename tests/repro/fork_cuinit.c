/*
 * fork_cuinit.c -- can a forked child initialise CUDA at all under nvkvm?
 *
 * cow_after_fork.c hangs at "child: cuInit..." under nvkvm while the same
 * binary completes against the stock driver. This strips everything else away:
 * no inherited buffer, no registration, just fork and then cuInit in the child.
 *
 * Also runs cuInit in a plain child (no fork-shared state) for contrast, and
 * prints progress before each call so a hang names the call it died in.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

typedef int CUresult; typedef int CUdevice; typedef void *CUcontext;
extern CUresult cuInit(unsigned);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuCtxCreate_v2(CUcontext *, unsigned, CUdevice);

static int cuda_bringup(const char *who)
{
    CUdevice dev; CUcontext ctx; CUresult rc;
    printf("  %s: cuInit ...\n", who);        fflush(stdout);
    if ((rc = cuInit(0)))                   { printf("  %s: cuInit rc=%d\n", who, rc); fflush(stdout); return 1; }
    printf("  %s: cuDeviceGet ...\n", who);   fflush(stdout);
    if ((rc = cuDeviceGet(&dev, 0)))        { printf("  %s: cuDeviceGet rc=%d\n", who, rc); fflush(stdout); return 1; }
    printf("  %s: cuCtxCreate ...\n", who);   fflush(stdout);
    if ((rc = cuCtxCreate_v2(&ctx, 0, dev))) { printf("  %s: cuCtxCreate rc=%d\n", who, rc); fflush(stdout); return 1; }
    printf("  %s: CUDA up OK\n", who);        fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    /* argv[1] == "direct": no fork, as a baseline that CUDA works at all here */
    if (argc > 1) return cuda_bringup("direct");

    pid_t pid = fork();
    if (pid == 0) { int r = cuda_bringup("child"); fflush(stdout); _exit(r); }

    int st = 0;
    printf("  parent: waiting for child (parent never touches CUDA)\n"); fflush(stdout);
    waitpid(pid, &st, 0);
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
    printf("\nchild exit=%d %s\n", rc,
           rc == 0 ? "-- fork then CUDA in the child WORKS" : "-- FAILED");
    return rc;
}
