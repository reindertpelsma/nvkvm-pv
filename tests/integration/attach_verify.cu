/*
 * attach_verify — does per-stream attach give CORRECT RESULTS, not just success?
 *
 * UnifiedMemoryStreams prints "All Done!" and verifies nothing, so its
 * completion is not evidence.  This does the thing that matters: CPU fills a
 * managed buffer with known values, attaches it to a stream with
 * cudaMemAttachSingle, runs a kernel on THAT stream, synchronises, and checks
 * every element on the CPU.  Repeated over several buffers and streams so a
 * single lucky mapping cannot pass it.
 */
#include <cstdio>
#include <cuda_runtime.h>

__global__ void transform(float *p, int n, float k)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) p[i] = p[i] * 2.0f + k;
}

#define CK(x) do { cudaError_t e=(x); if (e!=cudaSuccess) { \
    printf("FAIL %s -> %d (%s)\n", #x, (int)e, cudaGetErrorString(e)); return 1; } } while(0)

int main(void)
{
    const int NBUF = 4;
    const int N = 1 << 20;                 /* 4 MiB of float */
    cudaStream_t s[NBUF];
    float *buf[NBUF];
    long bad = 0;

    for (int b = 0; b < NBUF; b++) {
        CK(cudaStreamCreate(&s[b]));
        CK(cudaMallocManaged(&buf[b], (size_t)N * sizeof(float)));
        for (int i = 0; i < N; i++) buf[b][i] = (float)(i % 1000) + (float)b;
        /* The call that used to fail with cudaErrorInvalidValue. */
        CK(cudaStreamAttachMemAsync(s[b], buf[b], 0, cudaMemAttachSingle));
    }
    CK(cudaDeviceSynchronize());

    for (int b = 0; b < NBUF; b++)
        transform<<<(N + 255) / 256, 256, 0, s[b]>>>(buf[b], N, (float)(b + 1));
    for (int b = 0; b < NBUF; b++) CK(cudaStreamSynchronize(s[b]));

    for (int b = 0; b < NBUF; b++) {
        for (int i = 0; i < N; i++) {
            float want = ((float)(i % 1000) + (float)b) * 2.0f + (float)(b + 1);
            if (buf[b][i] != want) { if (bad < 3)
                printf("  mismatch buf=%d i=%d got=%f want=%f\n", b, i, buf[b][i], want);
                bad++; }
        }
    }
    printf("attach_verify: %d buffers x %d elements, %ld mismatched\n", NBUF, N, bad);

    /* Re-attach GLOBAL and re-run, so both attach modes are exercised. */
    for (int b = 0; b < NBUF; b++)
        CK(cudaStreamAttachMemAsync(s[b], buf[b], 0, cudaMemAttachGlobal));
    CK(cudaDeviceSynchronize());
    for (int b = 0; b < NBUF; b++)
        transform<<<(N + 255) / 256, 256, 0, s[b]>>>(buf[b], N, 0.0f);
    CK(cudaDeviceSynchronize());
    long bad2 = 0;
    for (int b = 0; b < NBUF; b++)
        for (int i = 0; i < N; i++) {
            float prev = ((float)(i % 1000) + (float)b) * 2.0f + (float)(b + 1);
            if (buf[b][i] != prev * 2.0f) bad2++;
        }
    printf("attach_verify(global): %ld mismatched\n", bad2);

    for (int b = 0; b < NBUF; b++) { cudaFree(buf[b]); cudaStreamDestroy(s[b]); }
    printf("RESULT: %s\n", (bad == 0 && bad2 == 0) ? "CORRECT" : "WRONG_ANSWERS");
    return (bad || bad2) ? 1 : 0;
}
