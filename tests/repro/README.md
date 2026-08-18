# Reproducers

Small, self-validating programs that isolate a single defect. Each one is meant
to be run **on the host and in the guest with the same binary** — a difference
between the two is the finding; agreement means the behaviour is the GPU's or
the driver's, not nvkvm's.

| file | builds with | what it isolates |
|---|---|---|
| `vk_create_device.c` | `cc -o vkdev vk_create_device.c -lvulkan` | `vkCreateDevice` failing on Hopper (`VK_ERROR_DEVICE_LOST`) while enumeration succeeds |
| `opencl_correctness.c` | `cc -O2 -o clcorr opencl_correctness.c -lOpenCL` | OpenCL host-shared memory paths: `ALLOC_HOST_PTR`, `USE_HOST_PTR`, repeated map/unmap, `CL_UNORM_INT8` images |
| `opencl_map_churn.c` | `cc -O2 -o clchurn opencl_map_churn.c -lOpenCL` | the map/unmap corruption above, parameterised, and it classifies each wrong value as zero / stale-from-earlier-iteration / junk |

`opencl_map_churn.c` takes `<iters> <pinned> <clFinish> <log2(N)> <churn>`, e.g.

    ./clchurn 8 1 1 20 3     # 8 iterations, ALLOC_HOST_PTR, clFinish, 4 MB, 3 churn pairs

`churn` allocates and releases that many buffer pairs first, so their GPA window
extents go back on the free list before the real buffers are allocated. That is
what makes the corruption deterministic — without it the test usually passes.
