# QEMU 9.2.0 → 11.1.1

**Date:** 2026-08-27 · **Branch:** `qemu-bump` · **Status:** built and unit-tested;
hardware verification recorded at the bottom. Not merged to `main`.

|  | old | new |
|---|---|---|
| version | `9.2.0` | `11.1.1` |
| tag commit | `55162f76c8d6698c22d9e805f29b1faf15d9724a` | `5e35f26695645b20931e10d8567c7e0169e62c07` |
| released | 2024-12-10 | 2026-08-26 |

## Why 11.1.1

`https://www.qemu.org/download/` lists **11.1.1 (26 August 2026)** as the latest
stable release, and it is the newest `vX.Y.Z` tag in
`gitlab.com/qemu-project/qemu` — confirmed independently with
`git ls-remote --tags`. It is a point release on the current stable series
(`stable-11.1`), not an `-rc`, and not a distro package version. Debian sid's
`1:11.1.0+ds-2` is *older* than this by one stable point release.

QEMU numbers by time, not by feature: the major version increments on the first
release of each calendar year. 9.2.0 is therefore two major cycles — about
twenty months — behind.

## CVE delta

Verified against NVD/MITRE, Red Hat's CVE pages and the Debian security tracker's
`source-package/qemu` table, not taken on trust.

### The five IDs this work was handed, checked one by one

Three are fixed by the bump. **Two are not**, and that matters more than the three.

| CVE | verdict |
|---|---|
| `CVE-2026-48914` | **Fixed by the bump.** virtio-blk missing `VIRTIO_BLK_T_SCSI_CMD` size check → heap OOB write. Fixed upstream `aeea0c2804`, released in **11.0.2**. Relevant: we use virtio-blk. |
| `CVE-2026-63319` | **Fixed by the bump, but we are not exposed.** usbredir infinite loop / divide-by-zero via `max_packet_size=0`. Fixed `a002485bfe`, released in **11.0.3**. We attach no `usb-redir` chardev. (MITRE's central record was still unsynced at time of checking; Ubuntu's CNA page and the upstream commit confirm it is genuine.) |
| `CVE-2024-8612` | **NOT fixed. Still open upstream at 11.1.1.** virtio-scsi/blk/crypto: an oversized `virtqueue_push()` can write uninitialised bounce-buffer bytes back to the guest (info leak, CVSS 3.8). Debian marks it *vulnerable* in every suite including sid `1:11.1.0+ds-2`, `no-dsa (Minor issue)`. Red Hat: no fixed version. |
| `CVE-2023-1386` | **NOT fixed. Still open upstream at 11.1.1.** 9pfs passthrough does not drop SUID/SGID when a guest-local user writes an executable → privilege escalation. Debian: *vulnerable* through sid, status "postponed… revisit when fixed upstream". **This is the one from the original justification that mounts into every nvkvm-steamos guest, and the bump does not close it.** |
| `CVE-2026-0665` | **Fixed long before 11.1, and we are not exposed.** Off-by-one in `xen_physdev_map_pirq` — the *Xen-on-KVM guest-compat hypercall shim*, not core KVM acceleration. Not the same thing as "we use KVM". |

**Do not describe this bump as closing `CVE-2023-1386` or `CVE-2024-8612`.** It
does not. They are residual risk, unchanged. In particular the 9p SUID/SGID
behaviour must not be relied on as part of the nvkvm-steamos sandbox boundary —
see `SECURITY.md`.

### What the bump does close, in subsystems we actually build

Grouped by the code we compile. Fix versions are upstream release tags.

**9pfs / virtio-9p** — nvkvm-steamos mounts a 9p share into every guest, so this
is the highest-value group:

| CVE | what | fixed in |
|---|---|---|
| `CVE-2026-9238` | unbounded `Treaddir` allocation → memory exhaustion | 11.0.3 |
| `CVE-2026-8348` | missing xattr FID limit → memory exhaustion | 11.0.3 |
| `CVE-2026-48004` | missing rename lock in `v9fs_co_readdir_many` (race) | ~11.0.2 |
| `CVE-2026-63318` | `O_TRUNC` bypass on a read-only export | 11.0.3 |

**core virtio** — `hw/virtio/virtio.c` is shared by every virtio device,
including `virtio-nvgpu`:

| CVE | what | fixed in |
|---|---|---|
| `CVE-2026-16457` | packed-vring `virtio_queue_empty()` infinite loop (DoS) | 11.1.0 |
| `CVE-2026-50626` | queue-size not validated against allocated max → OOB r/w with `VIRTIO_F_IN_ORDER` | 11.1.0 |

**virtio-gpu blob / 2D** — the same dma-buf and EGL import machinery the present
path uses. These are *not* virgl-specific, so `--disable-virglrenderer` does not
exclude them:

| CVE | what | fixed in |
|---|---|---|
| `CVE-2026-66021` | guest-controlled `blob_size` unchecked against iov backing → OOB read on display refresh | ≤ 11.1.1 |
| `CVE-2026-6502` | resource-blob command handling flaw | ≤ 11.1.1 |
| `CVE-2026-3886` | overflow check bypassed allocating a 2D image | ≤ 11.1.1 |

**Other block/net virtio:** `CVE-2026-5761` (zoned virtio-blk OOM),
`CVE-2026-5763` (virtio-scsi cdb size mismatch), `CVE-2026-50624` (virtio-rng
host UAF).

### Not applicable to this build

Worth writing down so the next person does not re-derive it. **All** `ui/` CVEs
found in the 9.2.0→11.1.1 window are VNC-specific — `CVE-2026-61475`,
`CVE-2026-8343`, `CVE-2026-48003`, `CVE-2026-48002`, `CVE-2026-15578`, and
`CVE-2025-11234` (websocket UAF, reachable only through VNC's listener) — and we
configure `--disable-vnc`. Nothing in the window touches `ui/console.c`,
`ui/egl-helpers.c`, `ui/gtk*.c` or `ui/sdl2*.c`.

Also out of scope because the device is not built or not attached: USB
(`CVE-2026-15705`, `CVE-2026-16043`, `CVE-2026-3890`, `CVE-2024-8354`), the UEFI
vars device (10 IDs), virtio-snd, PCIe SR-IOV, e1000, LSI53C895A, block/vmdk and
block/dmg, hyperv syndbg, virtio-crypto asym, and `libvhost-user`
(`CVE-2026-6425` — vhost-user backend processes only).

## Per-patch outcome

Twelve in, twelve out. **Nothing was dropped**, which was not the expected
result: `0004` was the one carrying an explicit "delete me when upstream fixes
this" instruction, and upstream has not.

| patch | outcome | detail |
|---|---|---|
| `0001` meson sources | **rebased, clean** | no conflict. `hw/misc/meson.build` still has the `# HPPA devices` / `CONFIG_LASI` shape the patch anchors on. |
| `0002` virtio name table | **rebased, conflict resolved** | upstream appended `[VIRTIO_ID_SPI] = "virtio-spi"` (ID 45) as the new last entry, so the trailing-comma hunk moved from the `GPIO` line to the `SPI` line. Checked `include/standard-headers/linux/virtio_ids.h` at 11.1.1: **ID 50 is still unassigned** (highest is SPI at 45), so the squat is still safe and the patch is still required — `virtio_id_to_name()`'s `assert(device_id < G_N_ELEMENTS(...))` still fires without it. Commit message corrected (it said "stops at 41"). |
| `0003` egl-helpers texstorage | **rebased, clean** | still required. `egl_dmabuf_import_texture()` in 11.1.1 still binds with `glEGLImageTargetTexture2DOES()` and upstream has no `EXT_EGL_image_storage` path. |
| `0004` console lookup abort | **rebased, clean — NOT dropped** | the README instructs deleting this if upstream fixed it. **Upstream has not.** `qemu_console_lookup_by_device()` at 11.1.1 is byte-for-byte the 9.2 function: still `object_property_get_link(..., "device", &error_abort)` for every console including text ones, still no `QEMU_IS_GRAPHIC_CONSOLE()` guard (its sibling `qemu_graphic_console_lookup_unused()` still has one). Carried, and still worth sending to qemu-devel. |
| `0005` gtk autoswitch | **rebased, clean** | no conflict. `surface_is_placeholder()` still lives in `include/ui/surface.h`. |
| `0006` gtk no implicit grab | **rebased, clean** | upstream still has the grab-on-first-left-click block this removes, unchanged. |
| `0007` gtk grab switches device | **rebased, clean** | `qmp_query_mice()` and `bool qemu_mouse_set(int, Error **)` both unchanged at 11.1.1. Still "not known to work" — that is unchanged by this bump, and the patch header still says so. |
| `0008` sdl2 guest head | **REWRITTEN — see below** | the only substantive change of the bump. |
| `0009` sdl2 grab | **rebased, conflict resolved** | conflict was in the include block only, and it was pure churn: `include/sysemu/` → `include/system/` (10.0), and upstream dropped `ui/win32-kbd-hook.h` along with its last use in `ui/sdl2.c`. This patch only mentions `win32_kbd_set_grab()` in a comment, so the include was dropped rather than restored. The grab logic itself did not conflict. |
| `0010` kvm EFAULT retry | **rebased, clean** | `kvm_cpu_exec()`'s `do { } while (ret == 0)` loop and its `if (!(run_ret == -EFAULT && run->exit_reason == KVM_EXIT_MEMORY_FAULT))` branch are structurally identical at 11.1.1, so the retry lands in the same place with the same meaning. **Also given a commit message** — it had none at all before (the file in `patches/` was a bare diff with no mail header, which is why `git am` could not be used for the rebase). |
| `0011` qapi broker display | **rebased, clean** | `Since:` tags corrected from `9.2` to `11.1`. |
| `0012` meson relay | **rebased, clean** | one line on top of `0001`. |

### `0008` — what upstream took over

Upstream commit
[`52053b7e0a0e285ce3448b830053b05fb0a9b1a8`](https://gitlab.com/qemu-project/qemu/-/commit/52053b7e0a0e285ce3448b830053b05fb0a9b1a8)
*"ui/sdl2: Implement dpy dmabuf functions"* (Pierre-Eric Pelloux-Prayer, first
released in **v10.1.0**) added exactly what two of this patch's three claimed
upstream gaps were about.

The `patches/README.md` note that "`0008` is two things wearing one number, and
only one of them is ours" is now resolved in upstream's favour for most of it.

**Dropped as redundant** — upstream now provides all of this:

* `sdl2_gl_scanout_dmabuf()`, `sdl2_gl_release_dmabuf()`, `sdl2_gl_has_dmabuf()`
  as *new* functions;
* their three `dcl_gl_ops` entries and their three prototypes in
  `include/ui/sdl2.h` — `dcl_gl_ops` is now **byte-identical to upstream's**,
  including `.dpy_gl_update = sdl2_gl_scanout_flush`;
* the `bool has_dmabuf` field on `struct sdl2_console`;
* the bare `qemu_egl_display = eglGetCurrentDisplay()` read in
  `sdl2_window_create()`.

**Kept, because upstream's version is still not sufficient here:**

* `sdl2_gl_ensure_window()`. Upstream's `sdl2_gl_scanout_dmabuf()` calls
  `SDL_GL_MakeCurrent(scon->real_window, ...)` and never creates a window, so a
  console whose only content is a GL scanout still gets none. Unfixed at 11.1.1.
* the per-console `scon->edpy` and `sdl2_gl_bind_window_egl()`. Upstream writes
  the window's `EGLDisplay` into the **global** `qemu_egl_display`, which
  `hw/misc/nvkvm_present_egl.c` already owns and re-pairs with its own
  render-node context every readback frame. Resolved by filling the global only
  when it is still `EGL_NO_DISPLAY` — which preserves upstream's behaviour for a
  stock run and keeps `qemu_egl_has_dmabuf()` answering correctly in
  `sdl2_gl_console_init()` — and borrowing it around the import otherwise.
* the real geometry. Upstream hardcodes `y0_top = false` and `x/y = 0,0` and
  ignores `backing_width`/`backing_height`; we pass what the `QemuDmaBuf`
  carries.
* a `release_dmabuf` that makes the context current before `glDeleteTextures`
  and clears `guest_fb.dmabuf`. Upstream's is a bare
  `egl_dmabuf_release_texture()`.
* the `!scon->real_window` guard in `sdl2_gl_scanout_flush()` — upstream still
  calls `SDL_GetWindowSize(NULL, ...)` there.
* `nvkvm_sdl2_maybe_raise()`, which was always nvkvm policy, not a gap.

Upstream's `if (qemu_dmabuf_get_allow_fences(dmabuf)) guest_fb.dmabuf = dmabuf;`
was **folded into** our version so the patch stays a strict superset. Nothing in
`ui/sdl2*` reads that field back yet — only the GTK backend does — but our
`release_dmabuf` clears it, and diverging from upstream silently is how the next
rebase goes wrong.

> **The trap here is worth remembering.** `git rebase`'s 3-way merge reported
> `include/ui/sdl2.h`, `ui/sdl2-gl.c` and `ui/sdl2-2d.c` as *auto-merged*, i.e.
> success. What it had actually produced was two `sdl2_gl_scanout_dmabuf()`
> definitions, two `sdl2_gl_release_dmabuf()`, two `sdl2_gl_has_dmabuf()`, two
> `has_dmabuf` fields and a duplicated ops-table block — none of which is a
> textual conflict, all of which is a compile error at best. **A clean rebase of
> a downstream patch series proves nothing on its own.** Build it, and build it
> with `NVKVM_QEMU_UI=1`, or `0005`–`0009` are never compiled at all.

The reduced remainder also changes what is worth sending to qemu-devel: from
"SDL has no dma-buf scanout" (upstream did it) down to `sdl2_gl_ensure_window()`
and the NULL-window guard in `scanout_flush()`.

## Changes outside `patches/`

The nvkvm device is **copied** into `hw/misc/`, not patched in, so nothing
validates it against the new tree until the compiler does.

* `src/qemu/nvkvm_display_relay.c` — `#include "sysemu/runstate.h"` →
  `"system/runstate.h"`. Upstream renamed `include/sysemu/` to
  `include/system/` in 10.0 and `include/sysemu/` does not exist at all in
  11.1.1. (The file's own comment had predicted this.) No other file under
  `src/` referenced `sysemu/` outside a comment.

* `scripts/build_qemu.sh` —
  * `QEMU_VERSION="9.2.0"` → `"11.1.1"`;
  * comments saying "QEMU 9.2", "ten patch files" (there are twelve), and the
    stale worked example about `0010` depending on `0009` (now `0012` on
    `0001`);
  * **new step 2b: a tag guard.** The clone is skipped whenever `$QEMU_SRC`
    merely *exists*, so any box that has built nvkvm before keeps its 9.2.0
    tree, all twelve patches then fail to apply, and the error blames the
    patches. The guard compares `git describe --tags --exact-match HEAD`
    against `v$QEMU_VERSION` and prints the `rm -rf && --force` fix. It warns
    rather than fails when the tag cannot be determined, since the patch-apply
    step is the real gate.

`docs/howto/build.md`, `README.md`, `CONTRIBUTING.md` and the two workflow
files under `.github/` had prose references to "QEMU 9.2"; updated.

## Verification

**Rebase provenance.** The series was reconstructed as twelve commits on
`v9.2.0` (all twelve apply cleanly there — the stated baseline is real), then
`git rebase`d onto `v11.1.1`. That is the recipe now written into
`patches/README.md`.

**Build.** `NVKVM_QEMU_UI=1 scripts/build_qemu.sh --force`, i.e. the repo's own
path, from a fresh clone, with GTK **and** SDL enabled so that `0005`–`0009` are
actually compiled.

**Unit tests.** See the branch's test record below.

**Hardware.** See below.
