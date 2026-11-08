/*
 * register_survives_fd_close.c -- does registered host memory keep its
 * contents when the process closes its GPU fds without unregistering?
 *
 * This is what cuda-checkpoint does: it releases the GPU by closing every
 * /dev/nvidia* fd of the process, while host allocations (and so registered
 * buffers) stay where they are. On bare metal the buffer is ordinary memory
 * the driver had pinned; closing the fds unpins it and the data is untouched.
 *
 * In an nvkvm guest, registration moved the buffer into a window memfd and
 * retyped the VMA to VM_PFNMAP. The last close zapped the window PTEs and gave
 * nothing back, so the VMA was left an empty VM_PFNMAP shell. How it
 * presented: the buffer read back as zeros (or faulted), and the restore after
 * a checkpoint failed with "OS_DESCRIPTOR migrate ... failed: -14", because
 * get_user_pages_fast() refuses a VM_PFNMAP range.
 *
 * Two shapes, each filled with a pattern and registered:
 *   - MAP_SHARED|MAP_ANONYMOUS
 *   - MAP_PRIVATE|MAP_ANONYMOUS (what malloc returns for a large block)
 * They sit between PROT_NONE guard pages, so the kernel cannot merge them into
 * one VMA. Then every /dev/nvidia* fd is closed and each buffer is read back,
 * written, and checked for a leftover "pf" (VM_PFNMAP) flag in
 * /proc/self/smaps.
 *
 * Run it on the host and in the guest with the same binary; they must agree.
 * libcuda is not usable after its fds are closed, so this exits with _exit()
 * and never calls into CUDA again.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <dirent.h>
#include <sys/mman.h>

typedef int CUresult; typedef int CUdevice; typedef void *CUcontext;
extern CUresult cuInit(unsigned);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuCtxCreate_v2(CUcontext *, unsigned, CUdevice);
extern CUresult cuMemHostRegister_v2(void *, size_t, unsigned);

#define LEN (256u*1024u)
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02

static sigjmp_buf jb; static volatile sig_atomic_t faulted;
static void onfault(int s){(void)s; faulted=1; siglongjmp(jb,1);}

struct buf { const char *name; volatile unsigned *p; size_t len; unsigned pat; };

static unsigned pattern(unsigned seed, unsigned i) { return seed ^ (i * 2654435761u); }

static void fill(struct buf *b, void *base, size_t len, unsigned seed, const char *name)
{
    b->name = name; b->p = base; b->len = len; b->pat = seed;
    for (unsigned i = 0; i < len/4; i++) b->p[i] = pattern(seed, i);
}

static int reg(struct buf *b)
{
    CUresult rc = cuMemHostRegister_v2((void *)b->p, b->len, CU_MEMHOSTREGISTER_DEVICEMAP);
    if (rc) { printf("  %-30s register REFUSED rc=%d\n", b->name, rc); return 1; }
    return 0;
}

/* Is the VMA that holds addr still VM_PFNMAP ("pf" in VmFlags)? -1 if not found. */
static int vma_is_pfnmap(const void *addr)
{
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) return -1;
    char line[512]; int in = 0, r = -1;
    unsigned long a = (unsigned long)addr;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx ", &lo, &hi) == 2 && strchr(line, '-') == line + strcspn(line, "-")) {
            in = (a >= lo && a < hi);
        } else if (in && !strncmp(line, "VmFlags:", 8)) {
            r = strstr(line, " pf") != NULL;
            break;
        }
    }
    fclose(f);
    return r;
}

static int close_gpu_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0, self = dirfd(d);
    struct dirent *e;
    while ((e = readdir(d))) {
        int fd = atoi(e->d_name);
        if (e->d_name[0] == '.' || fd == self) continue;
        char path[64], target[256];
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
        ssize_t k = readlink(path, target, sizeof target - 1);
        if (k <= 0) continue;
        target[k] = 0;
        if (!strncmp(target, "/dev/nvidia", 11)) { close(fd); n++; }
    }
    closedir(d);
    return n;
}

/* 0 intact and writable, 1 wrong data, 2 faulted */
static int check(struct buf *b)
{
    faulted = 0;
    volatile unsigned bad = 0, zero = 0, first = ~0u;   /* live across siglongjmp */
    if (sigsetjmp(jb, 1) == 0) {
        for (unsigned i = 0; i < b->len/4; i++) {
            unsigned v = b->p[i];
            if (v != pattern(b->pat, i)) { if (first == ~0u) first = i; bad++; if (!v) zero++; }
        }
        b->p[0] = 0x5A5A5A5A;
        if (b->p[0] != 0x5A5A5A5A) bad++;
    }
    int pf = vma_is_pfnmap((const void *)b->p);
    if (faulted)
        printf("  %-30s FAULTED reading or writing after the fds closed (VM_PFNMAP=%d)\n", b->name, pf);
    else if (bad)
        printf("  %-30s %u of %zu words wrong (%u zero), first at word %u (VM_PFNMAP=%d)\n",
               b->name, bad, b->len/4, zero, first, pf);
    else
        printf("  %-30s intact and writable (VM_PFNMAP=%d)\n", b->name, pf);
    return faulted ? 2 : (bad || pf == 1) ? 1 : 0;
}

int main(void)
{
    CUdevice dev; CUcontext ctx;
    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash must not swallow what came before it */
    if (cuInit(0) || cuDeviceGet(&dev,0) || cuCtxCreate_v2(&ctx,0,dev)) {
        printf("CUDA bring-up failed\n"); return 2;
    }
    enum { NB = 2 };
    struct buf b[NB];
    /* [guard][shared][guard][private][guard] */
    char *base = mmap(NULL, 5*LEN, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { printf("mmap failed\n"); return 2; }
    void *s = mmap(base + LEN, LEN, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
    void *p = mmap(base + 3*LEN, LEN, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
    if (s == MAP_FAILED || p == MAP_FAILED) { printf("mmap failed\n"); return 2; }
    fill(&b[0], s, LEN, 0x11111111u, "MAP_SHARED|ANONYMOUS");
    fill(&b[1], p, LEN, 0x22222222u, "MAP_PRIVATE|ANONYMOUS");
    for (int i = 0; i < NB; i++) if (reg(&b[i])) _exit(2);

    int n = close_gpu_fds();
    printf("closed %d /dev/nvidia* fds with %d registrations live\n", n, NB);
    if (n <= 0) { printf("VERDICT: SKIP -- no /dev/nvidia* fds to close\n"); fflush(stdout); _exit(2); }

    signal(SIGSEGV, onfault); signal(SIGBUS, onfault);   /* only around the checks */
    int worst = 0;
    for (int i = 0; i < NB; i++) { int r = check(&b[i]); if (r > worst) worst = r; }

    printf("\n");
    if (worst == 0) printf("VERDICT: PASS -- registered memory survives closing the GPU fds\n");
    else            printf("VERDICT: BUG -- registered memory was %s when the GPU fds closed\n",
                           worst == 2 ? "left unmapped" : "lost or left VM_PFNMAP");
    fflush(stdout);
    _exit(worst ? 1 : 0);
}
