/*
 * cow_after_fork.c -- is COW'd private memory still registerable after fork?
 *
 * The mapcount guard refuses a range something else maps. For MAP_SHARED that
 * is right: the sharing is real and relocating would desynchronise it. For
 * MAP_PRIVATE it would be wrong -- the sharing is copy-on-write, so the memory
 * is logically private and a write breaks it apart. Registering heap memory
 * after a fork is an ordinary thing to do, and refusing it would be a
 * regression.
 *
 * migrate_range GUPs with FOLL_WRITE, which should break COW before the
 * mapcount check runs. This verifies that end to end:
 *
 *   parent: malloc-like MAP_PRIVATE buffer, fill A, fork
 *   child:  cuMemHostRegister its inherited copy
 *
 * PASS: registration ALLOWED, and the parent's copy is untouched (COW held).
 * FAIL: -EINVAL -- the guard is over-rejecting ordinary private memory.
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
    /* MAP_PRIVATE|MAP_ANONYMOUS: what malloc gives you for a large block. */
    volatile unsigned *buf = mmap(NULL, LEN, PROT_READ|PROT_WRITE,
                                  MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); return 2; }
    for (unsigned i = 0; i < LEN/4; i++) buf[i] = SENTINEL_A;

    int tp[2], tc[2]; if (pipe(tp)||pipe(tc)) return 2;
    pid_t pid = fork();

    if (pid == 0) {                      /* child shares the pages COW */
        char c;
        CUdevice dev; CUcontext ctx;
        if (cuInit(0) || cuDeviceGet(&dev,0) || cuCtxCreate_v2(&ctx,0,dev)) { fflush(stdout); _exit(2); }
        CUresult rc = cuMemHostRegister_v2((void*)buf, LEN, CU_MEMHOSTREGISTER_DEVICEMAP);
        printf("  child: cuMemHostRegister on COW'd private memory rc=%d %s\n", rc,
               rc == 0 ? "ALLOWED" : "REFUSED");
        if (rc == 0) {
            /* write through the registered copy; the parent must not see it */
            for (unsigned i = 0; i < LEN/4; i++) buf[i] = SENTINEL_C;
            printf("  child: wrote C; own view has C in %u/%u\n",
                   count_eq(buf, SENTINEL_C), LEN/4);
            cuMemHostUnregister((void*)buf);
        }
        if (write(tp[1],"r",1)!=1) { fflush(stdout); _exit(3); }
        if (read(tc[0],&c,1)!=1)   { fflush(stdout); _exit(3); }
        fflush(stdout);
        _exit(rc == 0 ? 0 : 1);
    }

    char c; if (read(tp[0],&c,1)!=1) return 2;
    unsigned a = count_eq(buf, SENTINEL_A), cc = count_eq(buf, SENTINEL_C);
    printf("  parent: after child registered+wrote -> A=%u C=%u (of %u)\n", a, cc, LEN/4);
    if (write(tc[1],"g",1)!=1) return 2;
    int st=0; waitpid(pid,&st,0);
    int crc = WIFEXITED(st)?WEXITSTATUS(st):99;

    printf("\n");
    if (crc == 0 && a == LEN/4 && cc == 0)
        printf("VERDICT: PASS -- private memory registers after fork, COW intact\n");
    else if (crc == 1)
        printf("VERDICT: FAIL -- guard over-rejects ordinary COW private memory\n");
    else
        printf("VERDICT: unexpected (child rc=%d, parent A=%u C=%u)\n", crc, a, cc);
    return crc;
}
