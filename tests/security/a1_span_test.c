/*
 * a1_span_test.c — does the A-1 gate's contiguous walk stay strict?
 *
 * The multi-entry fix had to allow a registration that spans several
 * host-installed chunks (a 16 MiB cudaHostRegister is 8 of them).  The risk in
 * that change is over-relaxing it to "overlaps some entry", which would
 * authorise the uncovered remainder and reopen A-1 for everything past the
 * first chunk.  This pins the difference.
 *
 * Registers 16 MiB legitimately (8 chunks installed), then asks for an OS
 * descriptor three ways: fully inside the covered region, spanning past its
 * end, and wholly beyond it.  Only the first may pass the gate.
 *
 * REQUIRES A TEST MODULE.  A cooperative guest migrates before forwarding,
 * which installs whatever it names and would make every probe "covered".  Build
 * a module that forwards raw when the caller sets a magic flag, so the same
 * process can register legitimately AND probe raw:
 *
 *   cp -r /mnt/nvkvm/src /tmp/bp2/src && cd /tmp/bp2/src/guest
 *   sed -i "s|int mret = nvkvm_cpu_pages_migrate_range(|int mret = (p->flags == 0xA1A1A1A1u) ? 0 : nvkvm_cpu_pages_migrate_range(|" nvkvm_ioctl.c
 *   make KDIR=/lib/modules/$(uname -r)/build
 *   sudo rmmod nvkvm_guest && sudo insmod ./nvkvm-guest.ko
 *
 * Restore the stock module afterwards.
 *
 * Note the magic flag is not a valid NVOS02 flags value, so a probe the gate
 * ALLOWS is then rejected by RM itself (rc=-1).  That is why the verdict keys
 * on status 0x1e -- the gate's refusal -- and not on overall success.
 *
 * Measured on an RTX 3060 / 575.51.03: inside passes the gate, spanning-past and
 * beyond are both REFUSED by A-1.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

struct nvos21 { uint32_t r,p,n,c; uint64_t a; uint32_t s,pad; };
struct nvos02 { uint32_t r,p,n,c,flags,resv; uint64_t mem,limit; uint32_t st,pad1; int32_t fd; uint32_t pad0; };

int main(void)
{
    void *lib = dlopen("libcuda.so.1", RTLD_NOW);
    int (*cuInit)(unsigned), (*cuDeviceGet)(int*,int), (*cuCtxRetain)(void**,int);
    int (*cuCtxSetCurrent)(void*), (*cuReg)(void*,size_t,unsigned);
    if (!lib) { printf("SKIP: libcuda\n"); return 0; }
    *(void**)&cuInit=dlsym(lib,"cuInit"); *(void**)&cuDeviceGet=dlsym(lib,"cuDeviceGet");
    *(void**)&cuCtxRetain=dlsym(lib,"cuDevicePrimaryCtxRetain");
    *(void**)&cuCtxSetCurrent=dlsym(lib,"cuCtxSetCurrent");
    *(void**)&cuReg=dlsym(lib,"cuMemHostRegister");
    if (!cuInit || cuInit(0)) { printf("SKIP: cuInit\n"); return 0; }
    int dev=0; void *ctx=NULL; cuDeviceGet(&dev,0); cuCtxRetain(&ctx,dev); cuCtxSetCurrent(ctx);

    size_t REG = 16u<<20;
    /* Reserve 32 MiB so the over-long request names mapped guest memory --
     * otherwise the guest module would refuse it before the host sees it. */
    void *p = mmap(NULL, REG*2, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p==MAP_FAILED) { printf("mmap failed\n"); return 1; }
    memset(p, 0xa5, REG*2);
    int rc = cuReg(p, REG, 0x01);
    printf("legit cuMemHostRegister(16 MiB) -> %d  base=%p\n", rc, p);
    if (rc) { printf("SKIP: registration failed\n"); return 0; }

    int ctl = open("/dev/nvidiactl", O_RDWR);
    struct nvos21 a; memset(&a,0,sizeof a); a.c=0;
    ioctl(ctl,_IOWR('F',0x2b,struct nvos21),&a);
    printf("hClient=0x%x\n", a.n);

    struct { const char *name; uint64_t off, len; int expect_ok; } probes[] = {
        { "inside covered region (4 MiB @ +8 MiB)", 8u<<20,  4u<<20,  1 },
        { "SPANS PAST covered end (32 MiB @ +0)",   0,       32u<<20, 0 },
        { "wholly beyond covered end (4 MiB @ +20 MiB)", 20u<<20, 4u<<20, 0 },
    };
    int bad = 0;
    for (unsigned i=0;i<3;i++) {
        struct nvos02 m; memset(&m,0,sizeof m);
        m.r=a.n; m.p=a.n; m.n=0xa2000001u+i; m.c=0x71;
        m.flags=0xA1A1A1A1u;  /* test module: forward raw, skip migration */
        m.mem=(uint64_t)(uintptr_t)p + probes[i].off;
        m.limit=probes[i].len-1; m.fd=-1;
        int r2 = ioctl(ctl,_IOWR('F',0x27,struct nvos02),&m);
        /*
         * What matters is whether the A-1 GATE refused, not whether the driver
         * then liked the request.  The magic flag that makes the test module
         * forward raw is not a valid NVOS02 flags value, so anything the gate
         * lets through is rejected by RM afterwards (rc=-1).  Distinguish the
         * two: status 0x1e (NV_ERR_INVALID_ADDRESS) is the gate; anything else
         * means the gate passed it on.
         */
        int gate_refused = (m.st == 0x1e);
        printf("  %-42s rc=%d status=0x%-4x %s\n", probes[i].name, r2, m.st,
               gate_refused ? "REFUSED by A-1" : "passed the gate");
        if (gate_refused == probes[i].expect_ok) {
            printf("     ^^ UNEXPECTED (wanted %s)\n",
                   probes[i].expect_ok ? "passed" : "REFUSED");
            bad++;
        }
    }
    printf("A1_SPAN: %s\n", bad ? "FAIL" : "PASS");
    return bad!=0;
}
