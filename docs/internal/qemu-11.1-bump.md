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
| `CVE-2024-8612` | **NOT fixed upstream — but NOT reachable in our deployed config.** virtio-scsi/blk/crypto: an oversized `virtqueue_push()` leaks uninitialised bounce-buffer bytes to the guest (CVSS 3.8). Still vulnerable in sid `1:11.1.0+ds-2`, and the pattern is still in the pinned tree. The deployed guest attaches none of the three devices (its disk is `-device nvme`): see [Residual risk](#residual-risk--fixed--mitigated--exposed) §3b. |
| `CVE-2023-1386` | **NOT fixed. Still open upstream at 11.1.1.** 9pfs `passthrough` does not drop SUID/SGID when a guest writes an executable → host privilege escalation. Debian: *vulnerable* through sid. Whether we are exposed depends on `security_model`, and today **we are**: see [Residual risk](#residual-risk--fixed--mitigated--exposed) §3a. |
| `CVE-2026-0665` | **Fixed long before 11.1, and we are not exposed.** Off-by-one in `xen_physdev_map_pirq` — the *Xen-on-KVM guest-compat hypercall shim*, not core KVM acceleration. Not the same thing as "we use KVM". |

**Do not describe this bump as closing `CVE-2023-1386` or `CVE-2024-8612`.** It
does not. Both are residual risk, and the section below splits them into what is
mitigated by configuration and what is genuinely exposed — a configuration
mitigation is not a fix, and the one that mitigates 9pfs is **not on `main`
yet**. The 9p SUID/SGID behaviour must not be relied on as part of the
nvkvm-steamos sandbox boundary; see `SECURITY.md`.

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

## Residual risk: fixed / mitigated / exposed

The bump was justified partly by CVEs it does **not** fix. This section separates
the three cases so nobody reads a configuration mitigation as a code fix.

Evidence below is from the pinned tree (`v11.1.1`) and from
`/workspace/nvkvm-steamos`, checked on 2026-08-27.

### Category 1 — fixed by the bump

Everything in the "What the bump does close" tables above: the four 9pfs
resource-exhaustion / `O_TRUNC` bugs, the two core `hw/virtio/virtio.c` bugs,
the three virtio-gpu blob/2D bugs, plus `CVE-2026-48914` (virtio-blk) and
`CVE-2026-63319` (usbredir, not attached here). These are code fixes present in
`v11.1.1` and absent from `9.2.0`. Nothing in this category depends on how we
configure QEMU.

### Category 2 — NOT fixed upstream, but mitigated by configuration

**`CVE-2023-1386` — 9pfs does not drop SUID/SGID.**

Still unfixed upstream at `v11.1.1`, and the bump does not touch it. It is only
exploitable if a guest-created setuid file becomes a **real setuid file on the
host**, and that depends entirely on the export's `security_model`.

QEMU's local backend branches on the security model at every point where a mode
reaches the host. In the pinned tree:

| path | `mapped-xattr` (`V9FS_SM_MAPPED`) | `passthrough` / `none` |
|---|---|---|
| create a file | `hw/9pfs/9p-local.c:851` — `openat_file(dirfd, name, flags, fs_ctx->fmode)` | `hw/9pfs/9p-local.c:867` — `openat_file(dirfd, name, flags, credp->fc_mode)` |
| mknod | `hw/9pfs/9p-local.c:688` — `qemu_mknodat(..., fs_ctx->fmode \| S_IFREG, 0)` | `hw/9pfs/9p-local.c:703` — `qemu_mknodat(..., credp->fc_mode, credp->fc_rdev)` |
| chmod | `hw/9pfs/9p-local.c:654` — `local_set_xattrat()` | `hw/9pfs/9p-local.c:659` — `fchmodat_nofollow(dirfd, name, credp->fc_mode)` |

Under `passthrough`, the guest's mode reaches the host file, and
`local_set_cred_passthrough()` finishes the job at **`hw/9pfs/9p-local.c:458`**:

```c
return fchmodat_nofollow(dirfd, name, credp->fc_mode & 07777);
```

`07777` **includes `S_ISUID` (04000) and `S_ISGID` (02000)** — that is the CVE.

Under `mapped-xattr` the guest's mode is stored as data, in the
`user.virtfs.mode` xattr (`hw/9pfs/9p-local.c:426-431`), and the host file is
created with the export's own `fmode`. That `fmode` **cannot express setuid at
all**, because it is masked when the option is parsed —
**`hw/9pfs/9p-local.c:1597-1598`**:

```c
fse->fmode =
    qemu_opt_get_number(opts, "fmode", SM_LOCAL_MODE_BITS) & 0777;
```

`& 0777` strips the setuid/setgid bits, and the default `SM_LOCAL_MODE_BITS` is
`0600` (`fsdev/file-op-9p.h:32`). So on a `mapped-xattr` export a guest cannot
produce a setuid host file even if the operator deliberately tried to configure
one. The option string maps to that flag at `hw/9pfs/9p-local.c:1550-1552`
(`"mapped"` and `"mapped-xattr"` both set `V9FS_SM_MAPPED`); `"passthrough"` is
`hw/9pfs/9p-local.c:1548-1549`.

**So the mitigation is real and is enforced by QEMU itself — but see Category 3,
because it is not on the deployed branch yet.**

### Category 3 — NOT fixed and NOT mitigated

**3a. `CVE-2023-1386` on `main`, i.e. on what actually deploys today.**

The `passthrough` → `mapped-xattr` change is **commit `b26ae01` on branch
`audit-followups` only. It is not merged to `main`.** Verified:

```
$ git -C /workspace/nvkvm-steamos merge-base --is-ancestor b26ae01 main ; echo $?
1                                    # NOT an ancestor of main
$ git -C /workspace/nvkvm-steamos branch -a --contains b26ae01
* audit-followups
  remotes/origin/audit-followups
$ git -C /workspace/nvkvm-steamos show main:scripts/steamos-container-entrypoint.sh | sed -n 435p
    -virtfs "local,path=$DATA_DIR,mount_tag=data,security_model=passthrough" \
```

`main` and `origin/main` are both at `464f494`.

The exposed export is, by name:

> **`scripts/steamos-container-entrypoint.sh:435` on `main` — `mount_tag=data`,
> `security_model=passthrough`, writable**, `path=$DATA_DIR` (`:15`,
> `DATA_DIR="${NVKVM_STEAMOS_DATA_DIR:-/data}"`), backed by the compose volume
> `${NVKVM_STEAMOS_DATA:-steamos-data}:/data` (`docker-compose.yml:205`) — a
> named Docker volume by default, but a **real host directory** whenever the
> operator sets `NVKVM_STEAMOS_DATA=/absolute/host/path`. QEMU runs as uid 0 in
> that container.

A guest can drop a root-owned setuid binary into that directory and leave a host
privilege-escalation vector for whoever later runs it.

On branch `audit-followups` (`scripts/steamos-container-entrypoint.sh:443`) this
becomes Category 2. **Until `b26ae01` is merged, CVE-2023-1386 is Category 3 for
the deployed configuration.**

The other three deployed exports are *not* exposed, and it is worth writing down
why so nobody "fixes" them unnecessarily: they are `security_model=none` but
also `readonly=on`, and they export either the baked-in `/opt/nvkvm` or an
nvkvm-pv source checkout — there is no guest-writable path through them:

* `scripts/steamos-container-entrypoint.sh:434` — `mount_tag=nvkvm`, `/opt/nvkvm`, `readonly=on`
* `boot/run_steamos_nvkvm.sh:53` — `mount_tag=nvkvm`, `$SHARE`, `readonly=on`
* `install_steamos_vm.sh:253` — `mount_tag=nvkvm`, `$SHARE`, `readonly=on`

**3b. `CVE-2024-8612` — virtio bounce-buffer info leak. Not fixed; not reachable
in the deployed config; reachable in the dev harness.**

Not fixed upstream. Debian tracks `qemu` as *vulnerable* in every suite
including sid `1:11.1.0+ds-2`, and the pattern is still in the pinned tree:

* `hw/scsi/virtio-scsi.c:118` — `virtqueue_push(vq, &req->elem, req->qsgl.size + req->resp_iov.size);`
* `hw/block/virtio-blk.c:67` — `virtqueue_push(req->vq, &req->elem, req->in_len);`

Red Hat's page cites commit `637b0aa1395`, and that commit **is already in
`v9.2.0`** (`git tag --contains` → `v9.2.0`), so it is not something the bump
delivers; Debian's note is explicit that the existing commit addresses a symptom
and not the root cause.

The affected devices are **virtio-scsi, virtio-blk and virtio-crypto**. The
deployed nvkvm-steamos guest attaches **none** of them — its disk is emulated
NVMe:

```
scripts/steamos-container-entrypoint.sh:427-428
    -drive "file=$QCOW,format=qcow2,if=none,id=nvm0" \
    -device nvme,drive=nvm0,serial=nvkvmsteamos \
```

and `grep -rE 'virtio-scsi|virtio-crypto'` over the deployed scripts returns
nothing. So for the shipped configuration this CVE is **not reachable**.

The configuration that *would* be exposed is any guest given a virtio-blk or
virtio-scsi disk — and nvkvm-pv's own dev harness is exactly that:
`scripts/run_test_vm.sh` boots with `-drive file="$IMG",format=qcow2,if=virtio`,
which is virtio-blk. That harness is documented as trusted-guest-only and is not
a deployment, so this is a note, not a finding — but if the deployed guest ever
moves from `-device nvme` to virtio-blk, this CVE moves to Category 3a
alongside 9pfs.

### What to guard

1. **`b26ae01` must reach `main`.** Until it does, the writable `data` export
   ships as `passthrough` and CVE-2023-1386 is live.
2. **Guard against a revert to `passthrough`.** The mitigation is one word in
   one line, it lives in a shell script with no test asserting it, and the
   failure is completely silent — nothing observable changes in the guest when
   it flips back. A `grep -q 'mount_tag=data,security_model=mapped-xattr'`
   assertion in CI, or in the entrypoint itself, is the cheap way to keep it.
   `security_model=none` is equally unsafe for a *writable* export; the check
   should be an allowlist of `mapped-xattr`, not a denylist of `passthrough`.
3. **Do not let 9p passthrough be described as part of the sandbox boundary.**
   `SECURITY.md` should say that a `passthrough` export is a host-privilege
   path, independent of this CVE.
4. **If the guest disk ever becomes virtio-blk/virtio-scsi**, re-check
   CVE-2024-8612 before shipping.

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

**Upstream moved a lot of headers between 10.0 and 11.1**, and every one of
these is a hard compile error that only appears at step 8 of the build, roughly
twenty minutes in:

| was (9.2) | is (11.1.1) | files |
|---|---|---|
| `sysemu/runstate.h` | `system/runstate.h` | `nvkvm_display_relay.c` |
| `hw/qdev-core.h` | `hw/core/qdev.h` (upstream `d1000ecae2`) | `nvkvm_present_egl.c` |
| `hw/qdev-properties.h` | `hw/core/qdev-properties.h` | `virtio_nvgpu.c`, `virtio_nvgpu_pci.c` |
| `hw/boards.h` | `hw/core/boards.h` | `virtio_nvgpu.c`, `nvkvm_mmap_host.c` |
| `block/aio.h` | `qemu/aio.h` | `virtio_nvgpu.c` |
| `exec/memory.h` | `system/memory.h` | `virtio_nvgpu.c`, `virtio_nvgpu_pci.c` |
| `exec/address-spaces.h` | `system/address-spaces.h` | `virtio_nvgpu.c` |

Note `hw/qdev-core.h` is not merely moved, it is **renamed**: there is no
`qdev-core.h` anywhere in 11.1.1.

Finding them one build failure at a time is a bad trade at ~20 minutes a
cycle. The whole set was found in one pass by listing every `#include "..."`
in `src/qemu/`, `src/common/` and `src/abi/` and resolving each against
`git ls-tree -r v11.1.1`, with `include/` as the search prefix — worth
repeating verbatim on the next bump, before building anything:

```bash
git -C <qemu> ls-tree -r --name-only vNEW > /tmp/tree.txt
grep -rhoE '#include "[a-z0-9_/-]+\.h"' src/qemu src/common src/abi |
  sed 's/.*"\(.*\)"/\1/' | sort -u |
  while read h; do grep -qE "^(include/)?$h$" /tmp/tree.txt || echo "MISSING: $h"; done
```

(`qapi/qapi-commands-ui.h` shows up as missing and is a false positive — QAPI
headers are generated into the build directory, not the source tree.)

### The device sources needed a real API port, not just header paths

This was the largest single cost of the bump and it is **not** in `patches/` at
all — it is `src/qemu/`, the ~12,900 lines copied into `hw/misc/`. Because they
are copied rather than patched, `git apply` cannot warn about any of it; the
compiler is the only check, and it runs at step 8.

Everything below is mechanical — renames and one-for-one signature changes,
with no behavioural decision except where noted — but there were eleven of them:

**QOM / qdev**

* `class_init(ObjectClass *, void *data)` → `(ObjectClass *, const void *data)`
  (3 call sites). `TypeInfo.class_data` is `const void *` now.
* `DEFINE_PROP_END_OF_LIST()` was **removed** (upstream `5fcabe628b`).
  `device_class_set_props()` is now a macro that takes `ARRAY_SIZE(props)` and
  build-asserts that the *last* entry's `name` is non-NULL — so the old
  terminator is a compile error rather than a harmless extra. Property arrays
  also became `static const Property [];`.

**Poisoned identifier**

* `TARGET_PHYS_ADDR_SPACE_BITS` is now in `include/exec/poison.h`.
  `nvkvm_mmap_host.c` guarded its use with `#ifdef`, and had a comment
  explaining that the branch was already always false in `system_ss` — but
  `#pragma GCC poison` rejects the *mention* of the identifier, `#ifdef`
  included. The dead branch was deleted; behaviour is unchanged.

**Console / display entry points, renamed into a `qemu_console_*` namespace**

| 9.2 | 11.1.1 |
|---|---|
| `console_has_gl` | `qemu_console_has_gl` |
| `dpy_gfx_update` | `qemu_console_update` |
| `dpy_gl_ctx_create` / `_destroy` / `_make_current` | `qemu_console_gl_ctx_create` / `_destroy` / `_make_current` |
| `dpy_gl_scanout_dmabuf` / `dpy_gl_release_dmabuf` / `dpy_gl_update` | `qemu_console_gl_scanout_dmabuf` / `_gl_release_dmabuf` / `_gl_update` |
| `dpy_ui_info_supported` / `dpy_get_ui_info` / `dpy_set_ui_info` | `qemu_console_ui_info_supported` / `qemu_console_get_ui_info` / `qemu_console_set_ui_info` |
| `graphic_console_init` | `qemu_graphic_console_create` |
| `graphic_console_close` | `qemu_graphic_console_close` |
| `graphic_hw_update` | `qemu_console_hw_update` |

`qemu_console_get_ui_info()` also returns a `const QemuUIInfo *` now; the relay
already copied it, so that cost nothing.

**Two that are more than a rename**

* `GraphicHwOps.gfx_update` changed from `void (*)(void *)` to
  `bool (*)(void *)`: **true means the update completed synchronously**, false
  means "deferred, I will call `qemu_console_hw_update_done()`".
  `nvkvm_present_gfx_update()` publishes inline and finishes before it returns,
  so it returns `true` on every path. Returning `false` without ever calling
  `_update_done()` would hang whatever waits on the update — this is the one
  change here where getting it wrong is silent rather than a compile error.
* `qemu_input_event_send_key_qcode()` is **gone**. QEMU 11.1 carries Linux
  evdev codes in `QemuInputEvent` natively, and the sender is
  `qemu_input_event_send_key_linux(con, lnx, down)`. The broker's wire format
  was *already* an evdev code, so the relay's evdev→QKeyCode conversion is now
  unnecessary on the way in; the `qemu_input_map_linux_to_qcode` lookup is kept
  purely as the validity filter it always also was, so a key QEMU has no
  mapping for is still dropped rather than forwarded blind.
* `qemu_dmabuf_new()` became multi-plane: `offset`, `stride` and the fd are
  arrays with an explicit `num_planes`. Both nvkvm call sites scan out a
  single-plane buffer, so both pass one-element arrays and `num_planes = 1`.

**How to find these fast next time.** Do not discover them one 20-minute build
at a time. Compile *only* the nvkvm objects, with `ninja -k 0` so it does not
stop at the first failure, and read the whole list at once:

```bash
ninja -C <qemu>/build -k 0 $(for f in virtio_nvgpu virtio_nvgpu_pci nvkvm_objects \
    nvkvm_mmap_host nvkvm_handle nvkvm_isolate nvkvm_isolate_handlers \
    nvkvm_tables nvkvm_present_egl nvkvm_display_relay nvkvm_udmabuf; do \
    echo libsystem.a.p/hw_misc_$f.c.o; done)
```

Then `grep -oE "implicit declaration of function ‘[a-z_]+’"` gives the entire
rename list in one pass, and each old name's replacement is usually findable by
grepping the new `include/ui/console.h` for the same suffix.

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

**Build.** `NVKVM_QEMU_UI=1 scripts/build_qemu.sh --force` — the repo's own
path, from a fresh clone of `v11.1.1`, with GTK **and** SDL enabled so that
`0005`–`0009` are actually compiled rather than merely applied. Clean: zero
`FAILED` targets across all 3156.

All twelve patches applied to the fresh tree in order, and the new step-2b tag
guard reported `tree is at v11.1.1, as the patches expect`. Re-running
`--force` over the already-patched tree now works (`reset 14 upstream file(s)
to v11.1.1`, then twelve `applied:` lines) — that is the idempotency bug fixed
in this branch.

What the resulting binary proves, beyond "it linked":

```
$ qemu-system-x86_64 --version
QEMU emulator version 11.1.1 (v11.1.1-dirty)

$ qemu-system-x86_64 -device help | grep -i nvgpu
name "virtio-nvgpu-pci", bus PCI
name "virtio-nvgpu-pci-non-transitional", bus PCI
name "virtio-nvgpu-pci-transitional", bus PCI
name "virtio-nvgpu-device", bus virtio-bus
name "nvkvm-gpu", bus PCI, desc "nvkvm emulated NVIDIA GPU PCI identity ..."

$ qemu-system-x86_64 -display help | grep -iE 'nvkvm|^gtk|^sdl'
gtk
sdl
nvkvm-broker
```

`nvkvm-broker` appearing in `-display help` is the end-to-end check on patch
`0011`: the QAPI schema change survived 11.1's stricter QAPI codegen (including
the new ≤70-character doc-line rule) and produced a real display backend.
`gtk` and `sdl` being present is what makes `0005`–`0009` compiled code rather
than untested patch text.

**Beyond "it linked" — the patched code paths actually execute.** Three checks
run on a machine with no NVIDIA driver at all, which is exactly what makes the
failures informative:

```
$ qemu-system-x86_64 -display nvkvm-broker -machine none
qemu-system-x86_64: -display nvkvm-broker: Parameter 'socket' is missing
```

That message comes from the generated QAPI visitor for `DisplayNvkvmBroker`, so
`0011`'s schema is not merely present in `--help` text — it is being enforced.

```
$ qemu-system-x86_64 -display nvkvm-broker,socket=/tmp/no-such-socket -machine none
   (blocks, then killed by timeout)
```

The relay backend from `0012` was instantiated and went on to attempt the
connect; it is reached, not skipped.

```
$ qemu-system-x86_64 -machine q35 -device virtio-nvgpu-pci-non-transitional,id=nvkvm0 ...
{"QMP": {"version": {"qemu": {"micro": 1, "minor": 1, "major": 11}, ...}}}
qemu-system-x86_64: -device virtio-nvgpu-pci-non-transitional,id=nvkvm0:
    nvkvm: cannot open /dev/nvidiactl on host: No such file or directory
```

The device realizes under 11.1.1 and runs far enough into
`virtio_nvgpu_device_realize()` to try to open the GPU — the *correct* failure
on a driverless host. That exercises the ported `class_init`, the rewritten
property array and `device_class_set_props()`, and the qdev/QOM changes, at run
time rather than at compile time. What it does **not** show is anything past
the driver open; that is what the hardware run below is for.

**Unit tests.** `bash tests/unit/run_tests.sh` — all 16 suites built and ran,
no failures, and the `test_isolate` known-failing set came back with none
failing:

```
test_relay_wiring 22/22   test_objects 20/20   test_tables 17/17
test_kvm_slot 12/12       test_stub_ptr_sanitize 17/17
test_transport_ready 6/6  test_r1_type_dev 33/33  test_handle 11/11
test_drm_devinfo 67/67    test_relay_clip 35/35   test_relay_state 17/17
test_stub_window 27/27    test_ctrl_gate / test_open_scm / test_uidmap: ok
test_isolate: 9 cases run (expected 9), failing now: <none>
```

**Broker selftest.** `bash src/broker/selftest.sh` — **76 checks run, all
passed**, including the whole ATTACH validator and the input policy state
machine (grab on/off, key suppression under grab, focus-loss releasing both the
held key and the grab).

Note the selftest needs `make -C src/broker` first; it exits 2 with
`build first: make` otherwise, which is easy to mistake for a failure.

**Hardware.**

> ### Scope of the hardware result — read this before quoting it
>
> The rented box runs **NVIDIA driver 580.105.08, open kernel module**
> (`NVRM version: NVIDIA UNIX Open Kernel Module for x86_64 580.105.08`), on a
> **GeForce RTX 4070 Ti** (Ada), in a real VM (`systemd-detect-virt` = `kvm`,
> `/dev/kvm` present, 17 vCPU / 24 GB).
>
> **The physical PC this ships against runs 595.84.** So a pass here covers
> **one driver branch and one module flavour**, and neither is the shipping
> one.
>
> That distinction is not cosmetic for nvkvm. The device *forwards ioctls to
> the host driver*, so a large part of the surface under test is the host
> driver's ABI, not QEMU's — which is exactly why `docs/internal/`
> already carries `nvkms-allowlist-abi-drift.md`. A result on 580-open does
> not generalise to 595-proprietary for free.
>
> Write it as "passes on 580.105.08 / open module, RTX 4070 Ti", never as
> "passes on hardware". **The physical-PC run on 595.84 is still required and
> is the owner's to schedule; this is the first of two, not a substitute.**
>
> ### Attribution rule for any failure here
>
> A failure on this branch is only a *bump regression* if it does **not**
> reproduce on 9.2.0 **on this same box, same driver, same guest image**. That
> control is cheap — `main` still pins 9.2.0 — and
> `scripts/` + `onbox_control_92.sh` build it to a separate prefix
> (`/opt/qemu-nvkvm-92`) and run the identical checks. Run the control before
> calling anything a regression; a failure that reproduces on 9.2.0 is a
> driver-branch or box property and belongs in `known-limitations.md`, not in
> this bump's ledger.
