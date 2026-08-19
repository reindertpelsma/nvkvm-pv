# Reproducers

Small, self-validating programs that isolate a single defect. Each one is meant
to be run **on the host and in the guest with the same binary** — a difference
between the two is the finding; agreement means the behaviour is the GPU's or
the driver's, not nvkvm's.

| file | builds with | what it isolates |
|---|---|---|
| `vk_create_device.c` | `cc -o vkdev vk_create_device.c -lvulkan` | `vkCreateDevice` failing on Hopper (`VK_ERROR_DEVICE_LOST`) while enumeration succeeds |
| `opencl_correctness.c` | `cc -O2 -o clcorr opencl_correctness.c -lOpenCL` | OpenCL host-shared memory paths: `ALLOC_HOST_PTR`, `USE_HOST_PTR`, repeated map/unmap, `CL_UNORM_INT8` images |
| `opencl_input_visibility.c` | `cc -O2 -o clvis opencl_input_visibility.c -lOpenCL` | whether the guest CPU and the GPU see the *same* memory, and what makes them diverge |
| `opencl_map_churn.c` | `cc -O2 -o clchurn opencl_map_churn.c -lOpenCL` | the map/unmap corruption above, parameterised, and it classifies each wrong value as zero / stale-from-earlier-iteration / junk |

`opencl_map_churn.c` takes `<iters> <pinned> <clFinish> <log2(N)> <churn>`, e.g.

    ./clchurn 8 1 1 20 3     # 8 iterations, ALLOC_HOST_PTR, clFinish, 4 MB, 3 churn pairs

`churn` allocates and releases that many buffer pairs first, so their GPA window
extents go back on the free list before the real buffers are allocated. That is
what makes the corruption deterministic — without it the test usually passes.

`opencl_input_visibility.c` takes `<churn> [log2(N)] [read_only_flags] [churn_mode]`
and is the one that isolates the cause. It writes a pattern from the CPU, reads
it back on the CPU, then has the GPU copy it — so a failure says *which* of the
two views is wrong rather than just "the answer differs".

    ./clvis 3 20 1 1     # churn buffers are mapped, written and RELEASED -> GPU sees zeros
    ./clvis 3 20 1 3     # identical, except the churn buffers are NOT released -> correct

`churn_mode` is the bisect: `0` allocate/release only, `1` also CPU-map and
write them, `2` also run a kernel, `3` map and write but never release. Only the
modes that **release** a previously mapped buffer corrupt the next one.
