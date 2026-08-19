# Guest kernel support

`nvkvm-guest.ko` is an out-of-tree module built against the guest's own kernel
headers. That makes the **distro** irrelevant and the **kernel version**
decisive, which is the opposite of how this was described until 2026-08-19.

Every row below was produced by compiling the module against that distro's own
kernel headers with that distro's own toolchain, both graphics variants
(`NVKVM_GRAPHICS=1` and `0`). Reproduce the whole table in one command — no GPU
and no VM needed, just Docker:

```bash
bash tests/kernel_matrix.sh                 # default image set
bash tests/kernel_matrix.sh debian:13       # or specific images
```

## Measured

| distro | kernel | builds | first error |
|---|---|---|---|
| Ubuntu 22.04 | 5.15.209 | **no** | `class_create` requires 2 arguments |
| Debian 12 | 6.1.180 | **no** | `class_create` requires 2 arguments |
| **Ubuntu 24.04** | **6.8.12** | **yes** | — |
| Debian 13 | 6.12.101 | **no** | `virtio_find_vqs` incompatible pointer type |
| Ubuntu 25.04 | 6.14.11 | **no** | `virtio_find_vqs` incompatible pointer type |
| Fedora 41 | 6.17.10 | **no** | `virtio_find_vqs` incompatible pointer type |
| Fedora 42 | 6.19.14 | **no** | `virtio_find_vqs` incompatible pointer type |
| Ubuntu 24.04 + HWE | 7.0.0 | **no** | `virtio_find_vqs` incompatible pointer type |

Only 6.8 was built *and* booted *and* run through `tests/validate.sh`. The
others are compile results; a compile failure is conclusive, a compile success
would not have been.

## Why the window is narrow

Two in-kernel API changes bound it, one on each side. Neither is exotic — the
module simply has **no `LINUX_VERSION_CODE` guards anywhere**, so it targets
whatever kernel it was written against.

- **`class_create()` lost its `owner` argument in 6.4.** Below that it takes
  `(owner, name)`; the module passes one argument, so every kernel ≤ 6.3 fails
  at `src/guest/nvkvm_main.c:120`.
- **`virtio_find_vqs()` changed signature in 6.11**, moving the callback and
  name arrays into a descriptor struct. The module passes the old arrays, so
  every kernel ≥ that change fails at `src/guest/nvkvm_virtio.c:664`.

So the buildable range is **6.4 – 6.10**. That upper bound is *inferred* from
where the virtio change landed, not measured: 6.9, 6.10 and 6.11 were not
available as distro header packages to test against. The measured statement is
narrower — builds on 6.8, fails on 6.1 and below and on 6.12 and above.

Note also that the reported error is only the **first** one. Fixing
`virtio_find_vqs` may expose further breakage on newer kernels; `pud_large()` /
`pmd_large()` (used in `src/guest/nvkvm_mmap.c`) were removed in favour of
`pud_leaf()` / `pmd_leaf()` in that same era and are the obvious next candidate.

## Widening it

This is ordinary compatibility work, not a design limit: version-guarded
wrappers for the two (or more) changed APIs. The harness above makes the
iteration cheap — a full sweep is a few minutes and needs no GPU. What it
cannot do is prove the result *runs*; that needs booting a guest on the target
kernel, which `scripts/setup_guest.sh` supports via `NVKVM_GUEST_IMAGE_URL`.
