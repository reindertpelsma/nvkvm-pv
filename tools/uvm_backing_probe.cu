/* M4 — can the GPU be given a mapping over the kind of memory QEMU's sparse
 * window is made of (memfd, MAP_SHARED), and is it coherent with a SECOND
 * CPU mapping of the same pages at a DIFFERENT address?
 *
 * That second mapping is the model for the split we need:
 *   QEMU registers the pages at host VA H (and memslots them)
 *   the guest reaches the same pages at its own address via the GPA
 * If the GPU sees writes made through mapping #2, the backing works.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cuda_runtime.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

__global__ void bump(int *p, int n){ int i=blockIdx.x*blockDim.x+threadIdx.x; if(i<n) p[i]+=100; }

static int try_register(void *p, size_t sz, const char *what)
{
    cudaError_t e = cudaHostRegister(p, sz, cudaHostRegisterMapped);
    printf("  cudaHostRegister(%-22s) -> %d (%s)\n", what, (int)e, cudaGetErrorString(e));
    return e == cudaSuccess;
}

int main(void)
{
    const size_t SZ = 2u<<20;
    const int    N  = SZ/sizeof(int);
    printf("== M4 backing-object test ==\n");

    /* (1) baseline: private anonymous */
    void *anon = mmap(NULL, SZ, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    printf("\n[1] private anonymous @ %p\n", anon);
    if (try_register(anon, SZ, "private anon")) {
        void *dp=nullptr; cudaHostGetDevicePointer(&dp, anon, 0);
        printf("  devPtr=%p equal=%s\n", dp, dp==anon?"YES":"NO");
        cudaHostUnregister(anon);
    }
    munmap(anon, SZ);

    /* (2) THE case: memfd, MAP_SHARED -- what QEMU's window is */
    int fd = memfd_create("winsim", 0);
    if (fd < 0 || ftruncate(fd, SZ)) { printf("memfd setup failed\n"); return 1; }
    int *h1 = (int*)mmap(NULL, SZ, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    int *h2 = (int*)mmap(NULL, SZ, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    printf("\n[2] memfd MAP_SHARED: mapping#1 @ %p  mapping#2 @ %p (same pages, different VA)\n",
           (void*)h1, (void*)h2);
    if (h1==MAP_FAILED || h2==MAP_FAILED) { printf("mmap failed\n"); return 1; }

    if (!try_register(h1, SZ, "memfd MAP_SHARED")) {
        printf("RESULT: MEMFD_NOT_REGISTERABLE  (window pages cannot back a GPU mapping)\n");
        return 2;
    }
    void *dp = nullptr;
    cudaError_t ge = cudaHostGetDevicePointer(&dp, h1, 0);
    printf("  cudaHostGetDevicePointer -> %d  devPtr=%p  equal_to_mapping1=%s\n",
           (int)ge, dp, dp==(void*)h1?"YES":"NO");

    /* write through mapping #2 -- the "guest" view */
    for (int i=0;i<N;i++) h2[i] = i;
    /* GPU reads/writes through the registered object */
    bump<<<(N+255)/256,256>>>((int*)dp, N);
    printf("  kernel sync -> %d\n", (int)cudaDeviceSynchronize());

    /* read back through mapping #2 again */
    long bad=0; for (int i=0;i<N;i++) if (h2[i] != i+100) bad++;
    printf("  COHERENCE via mapping#2 (different CPU VA): %ld/%d mismatched\n", bad, N);

    cudaHostUnregister(h1);
    printf("RESULT: %s\n", bad==0 ? "MEMFD_BACKING_WORKS" : "MEMFD_BACKING_INCOHERENT");
    munmap(h1,SZ); munmap(h2,SZ); close(fd);
    return bad!=0;
}
