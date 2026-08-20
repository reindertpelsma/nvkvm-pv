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
| `gbm_egl_import.c` | `cc -o gbmtest gbm_egl_import.c $(pkg-config --cflags --libs gbm egl glesv2)` | the GBM bo -> dmabuf -> EGLImage -> texture -> FBO round trip that Xorg's glamor needs; separates the dmabuf import path from the `EGL_NATIVE_PIXMAP_KHR` one |

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

## `gbm_egl_import.c` — and the glamor retraction

Takes `<drm-node> <tag>`, e.g. `./gbmtest /dev/dri/renderD128 GUEST`. Run it on
the render node *and* the card node, on host and guest.

It exists because native Xorg on the nvkvm head dies with

    (EE) modeset(0): Failed to create pixmap
    (EE) Fatal server error: failed to create screen resources

which looked like an nvkvm dma-buf forwarding bug. It is not. All four
combinations — host/guest × card/render node — behave *identically*: same
modifier `0x300000000606014`, same stride, `gbm_bo_get_fd()` works, the
`EGL_LINUX_DMA_BUF_EXT` import works, the imported bo is a complete FBO that
renders and reads back — and `eglCreateImageKHR(..., EGL_NATIVE_PIXMAP_KHR, bo,
NULL)` fails with `EGL_BAD_PARAMETER` (0x300C) **on the host too**.

That last call is not incidental; it is verbatim what glamor does
(`glamor/glamor_egl.c`, `glamor_egl_create_textured_pixmap_from_gbm_bo()`),
reached from `drmmode_set_pixmap_bo()` in the modesetting driver, which prints
exactly that "Failed to create pixmap". So the failure is NVIDIA's EGL not
supporting `EGL_NATIVE_PIXMAP_KHR` import from a `gbm_bo` — the long-standing
reason Xorg's modesetting+glamor does not work on the proprietary NVIDIA driver
on bare metal either. nvkvm reproduces bare metal faithfully here.

Consistent with that, glamor under **Xwayland** works fine in the guest (it
imports via dmabuf, not native-pixmap): `glxinfo` through Xwayland reports the
RTX 4070 and Mint's GTK apps render accelerated. See
`docs/internal/mint-guest-desktop.md`.
