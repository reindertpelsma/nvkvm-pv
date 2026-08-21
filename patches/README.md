# QEMU patches

Everything nvkvm changes in **upstream QEMU** is in this directory: five patch
files, 94 added lines and 2 removed, against the `v9.2.0` tag
(`ae35f033b874c627d81d51070187fbf55f0bf1a7`). Nothing else in the QEMU tree is
edited.

They exist as patches rather than as `sed` expressions in the build script for
one reason: a `git apply` is replicable by hand and a `sed` replacement is not.
A reader deciding whether to trust this can read four diffs; a maintainer
bumping the QEMU version resolves conflicts with ordinary tools instead of
rewriting editing logic; and "is it already applied?" is answered by
`git apply --reverse --check` rather than by grepping the tree for a comment
string, which used to mean that rewording a comment made a patch apply twice.

| patch | file | what it does |
|---|---|---|
| `0001-meson-register-virtio-nvgpu-sources.patch` | `hw/misc/meson.build` | lists the eleven nvkvm `.c` files and the `nvkvm_inc/` include dir, gated on `CONFIG_VIRTIO` |
| `0002-virtio-add-virtio-nvgpu-to-device-name-table.patch` | `hw/virtio/virtio.c` | adds `[50] = "virtio-nvgpu"` to `virtio_device_names[]`, which otherwise stops at 41 and aborts on our ID |
| `0003-egl-helpers-import-dmabuf-via-texstorage.patch` | `ui/egl-helpers.c` | imports dma-bufs with `glEGLImageTargetTexStorageEXT`; NVIDIA rejects the legacy OES bind for external-only images |
| `0004-console-do-not-abort-on-deviceless-console.patch` | `ui/console.c` | skips non-graphic consoles in `qemu_console_lookup_by_device()`, which otherwise aborts QEMU on `screendump` |
| `0005-gtk-switch-to-guest-display-when-it-goes-live.patch` | `include/ui/gtk.h`, `ui/gtk.c`, `ui/gtk-egl.c`, `ui/gtk-gl-area.c` | switches the GTK window to the guest's head the first time it presents real content, once, and only from the page the window opened on |

Each patch's commit message says *why* it is there. Read those before changing
one — several of them record a measurement (a driver version, an error code)
that is the whole justification for the change.

## Which of these should stop existing

Three of the five carry `ui/` changes, and `ui/` is the surface we would most
like to shed — it is generic front-end code, shared with every other QEMU user,
and the part a reviewer has least reason to trust us with.

**`0004` is an upstream QEMU bug, not an nvkvm-specific change.** The intent is
to submit it to qemu-devel. If you find it merged upstream, or you bump to a
QEMU that already has it, **delete the patch** — do not forward-port it. There
is nothing nvkvm-specific left in it once upstream has the fix. This is the one
with a clear path out, and it is worth actually sending.

`0003` is not upstreamable as written: it resolves the symbol rather than
checking for `EXT_EGL_image_storage` properly, and has only been tested against
NVIDIA. Fixing both would make it upstreamable — the underlying problem is not
ours alone.

`0005` is not upstreamable as written either, and should not be sent as-is: it
is nvkvm-specific policy ("the interesting console is the one that is not the
boot VGA") hardcoded into a generic front end, with an nvkvm-branded symbol and
log line. The gap it fills is general — any device whose console registers after
the VGA has this problem, and upstream gives a console no way to say "I have
something worth looking at". A `dpy_console_request_focus()`-shaped mechanism,
with the front end owning the policy, would be the upstreamable version. Until
someone writes that, this stays downstream.

`0001` and `0002` are downstream by nature: they register a device that is not
upstream and claim a virtio ID that is not assigned. They will never leave.

## What is *not* here

The device itself — ~12,900 lines across eleven `.c` files and their headers in
`src/qemu/` — is **copied** into `hw/misc/`, not patched in. It touches nothing
upstream, so a patch would be 12,900 lines of pure addition and no easier to
review than the files themselves. See [`docs/howto/build.md`](../docs/howto/build.md).

## Applying them

```bash
cd /opt/qemu-src
git apply --check /path/to/nvkvm/patches/*.patch   # all-or-nothing dry run
git apply         /path/to/nvkvm/patches/*.patch
```

`scripts/build_qemu.sh` does exactly this, plus the same `--reverse --check`
already-applied test for each patch so a re-run is a no-op.

`0005` only touches files that are compiled when QEMU is configured
`--enable-gtk` (`NVKVM_QEMU_UI=1`). The default headless build applies it and
never compiles a line of it.

## Regenerating them after a QEMU version bump

```bash
git clone --depth=1 --branch vX.Y.Z https://gitlab.com/qemu-project/qemu.git /tmp/qemu-next
cd /tmp/qemu-next
git checkout -b nvkvm-X.Y
git am /path/to/nvkvm/patches/*.patch    # fix conflicts, keep the messages
git format-patch --no-signature --zero-commit -o /path/to/nvkvm/patches vX.Y.Z..HEAD
```

Then rename the generated files back to the short names used here, and update
`QEMU_VERSION` in `scripts/build_qemu.sh`.
