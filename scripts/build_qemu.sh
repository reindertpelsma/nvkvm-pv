#!/usr/bin/env bash
# build_qemu.sh — clone QEMU 9.2, patch virtio-nvgpu into it, and build a
#                 minimal KVM-only QEMU binary at /opt/qemu-nvkvm.
#
# Guarded: if /opt/qemu-nvkvm/bin/qemu-system-x86_64 already exists the script
# prints a message and exits successfully WITHOUT rebuilding anything.  Pass
# --force to rebuild over an existing install -- you need it after editing
# anything under src/qemu/ or src/common/.

set -euo pipefail

QEMU_VERSION="9.2.0"
QEMU_SRC="/opt/qemu-src"
QEMU_PREFIX="/opt/qemu-nvkvm"
REPO_ROOT="$(realpath "$(dirname "$0")/..")"

# ── Guard: already built ───────────────────────────────────────────────────
# --force rebuilds even when the binary exists.  You need this after editing
# anything under src/qemu/ or src/common/: the guard below is what makes a plain
# re-run a no-op, so without it you will test the OLD device against NEW guest
# code and get a confusing mismatch (a new NVKVM_HFILE_* id, for instance, comes
# back as "Invalid argument" because the old binary's whitelist rejects it).
# The QEMU source tree and ninja build dir are reused, so a forced rebuild is
# incremental — minutes, not the full ~20.
NVKVM_FORCE=0
for arg in "$@"; do
    case "$arg" in
        -f|--force) NVKVM_FORCE=1 ;;
        -h|--help)
            echo "usage: $0 [--force]"
            echo "  --force   rebuild even if the binary already exists"
            exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [ -x "$QEMU_PREFIX/bin/qemu-system-x86_64" ] && [ "$NVKVM_FORCE" -eq 0 ]; then
    echo "INFO: $QEMU_PREFIX/bin/qemu-system-x86_64 already exists — skipping build."
    echo "INFO: re-run with --force to rebuild (needed after editing src/qemu/)."
    exit 0
fi
if [ "$NVKVM_FORCE" -eq 1 ]; then
    echo "INFO: --force given; rebuilding over the existing install."
fi

echo "=== nvkvm QEMU build ==="
echo "QEMU version : $QEMU_VERSION"
echo "Source tree  : $QEMU_SRC"
echo "Install path : $QEMU_PREFIX"
echo ""

# ── 1. Install build dependencies ─────────────────────────────────────────
echo "[1/9] Installing build dependencies..."
apt-get update -q
apt-get install -y \
    ninja-build \
    meson \
    libglib2.0-dev \
    libpixman-1-dev \
    python3 \
    `# QEMU 9.2 meson/configure needs these (rebuild fix 2026-07-04)` \
    python3-venv \
    python3-tomli \
    git \
    libslirp-dev \
    pkg-config \
    libattr1-dev \
    `# modeset present path (#102): OpenGL + headless EGL scanout` \
    libepoxy-dev \
    libgbm-dev \
    libegl-dev \
    libdrm-dev \
    `# the isolate stub is embedded via xxd -i (bench-rebuild fix 2026-07-29)` \
    xxd

# ── 1b. Build the isolate STUB and its embed header ───────────────────────
# Bench-rebuild fix 2026-07-29: this step did not exist, and its absence is
# SILENT. src/qemu/nvkvm_isolate.c embeds the stub behind
#     #ifdef NVKVM_STUB_EMBEDDED
#     #include "nvkvm_stub_bin.h"
# so with neither the define nor the generated header the QEMU build SUCCEEDS
# with stub_elf = NULL, stub_elf_len = 0, and silently falls back to
# /usr/lib/nvkvm/nvkvm_stub at runtime. On a fresh box that path does not
# exist, so fexecve fails and every isolate device-open returns -ENOENT:
#     nvkvm-gpu[GA106] M5.1: open ctl/gpu FAILED r1=-2 r2=-2 — forwarding OFF
# i.e. the device comes up with forwarding OFF and NOTHING says why. The failure
# looks like a missing /dev node (it is not — the nodes are present and
# world-writable) and it reproduces identically with NVKVM_ISOLATE_NO_HARDEN=1,
# which is what rules out the pivot_root / dev-dirfd path as the cause.
echo "[1b/9] Building the isolate stub (nvkvm_stub + nvkvm_stub_bin.h)..."
make -C "$REPO_ROOT/src/stub"
install -d /usr/lib/nvkvm
install -m 0755 "$REPO_ROOT/src/stub/nvkvm_stub" /usr/lib/nvkvm/nvkvm_stub
echo "  stub installed at /usr/lib/nvkvm/nvkvm_stub (runtime fallback)"

# ── 2. Clone QEMU 9.2 stable ──────────────────────────────────────────────
if [ ! -d "$QEMU_SRC" ]; then
    echo "[2/9] Cloning QEMU $QEMU_VERSION..."
    git clone --depth=1 --branch "v${QEMU_VERSION}" \
        https://gitlab.com/qemu-project/qemu.git "$QEMU_SRC"
else
    echo "[2/9] QEMU source already present at $QEMU_SRC — skipping clone."
fi

# ── 3. Copy nvkvm QEMU source files into hw/misc/ ─────────────────────────
echo "[3/9] Copying nvkvm QEMU source files to $QEMU_SRC/hw/misc/..."
cp "$REPO_ROOT/src/qemu/"*.c "$QEMU_SRC/hw/misc/"
cp "$REPO_ROOT/src/qemu/"*.h "$QEMU_SRC/hw/misc/"

# ── 4. Copy ABI / common headers into hw/misc/nvkvm_inc/ ──────────────────
echo "[4/9] Copying ABI and common headers to $QEMU_SRC/hw/misc/nvkvm_inc/..."
mkdir -p "$QEMU_SRC/hw/misc/nvkvm_inc"
# Rebuild fix 2026-07-04: the nvkvm .c/.h include SEVERAL headers from src/abi + src/common
# (nvgpu.h, uvm.h, nvkvm_proto.h AND nvkvm_abi.h, nvkvm_isolate_proto.h, nvkvm_ring.h, ...),
# not just the 3 hard-coded before — an incomplete copy fails the build.  Copy ALL of them.
cp "$REPO_ROOT/src/abi/"*.h    "$QEMU_SRC/hw/misc/nvkvm_inc/" 2>/dev/null || true
cp "$REPO_ROOT/src/common/"*.h "$QEMU_SRC/hw/misc/nvkvm_inc/" 2>/dev/null || true
# Linux type shim: replaces <linux/types.h> in the QEMU user-space build
# to avoid conflicts with QEMU's own type setup in qemu/osdep.h.
cp "$REPO_ROOT/src/qemu/nvkvm_linux_types.h" \
   "$QEMU_SRC/hw/misc/nvkvm_inc/linux_types_compat.h"
# The generated stub blob lives in src/stub/, NOT src/qemu/, so the *.h copy
# above does not pick it up. nvkvm_isolate.c includes it by bare name, so it
# must land in hw/misc/ alongside the sources (bench-rebuild fix 2026-07-29).
cp "$REPO_ROOT/src/stub/nvkvm_stub_bin.h" "$QEMU_SRC/hw/misc/"

# ── 5. Fix include paths in the copied files ──────────────────────────────
echo "[5/9] Fixing include paths in copied files..."
# The nvkvm sources use relative paths like ../../src/common/foo.h and
# ../../src/abi/bar.h that are correct relative to src/qemu/ but wrong inside
# hw/misc/.  Rewrite EVERY such include (across all copied .c and .h) to the
# local nvkvm_inc/ sub-directory — generalized so new headers don't break it.
# Rebuild fix 2026-07-19: the previous sed used '|' as BOTH the s/// delimiter AND
# inside the regex alternation (common|abi), so GNU sed parsed the alternation '|'
# as end-of-command -> "unknown option to `s'".  Use '#' as the delimiter instead.
sed -i -E \
    's#"\.\./\.\./src/(common|abi)/([A-Za-z0-9_]+\.h)"#"nvkvm_inc/\2"#g' \
    "$QEMU_SRC/hw/misc/"*.c "$QEMU_SRC/hw/misc/"*.h
# Replace <linux/types.h> in nvkvm_inc headers with our QEMU-compatible shim
# to avoid conflicts with QEMU's own qemu/osdep.h type setup.
sed -i \
    's|#include <linux/types.h>|#include "linux_types_compat.h"|g' \
    "$QEMU_SRC/hw/misc/nvkvm_inc/"*.h

# ── 6. Patch hw/misc/meson.build ─────────────────────────────────────────
echo "[6/9] Patching $QEMU_SRC/hw/misc/meson.build..."

MESON_BUILD="$QEMU_SRC/hw/misc/meson.build"

# Only patch once (idempotent).
if ! grep -q 'virtio_nvgpu.c' "$MESON_BUILD"; then
    # Insert the nvkvm block before the final line of the file.
    # We use a Python one-liner to keep things portable and avoid sed
    # multi-line headaches.
    python3 - "$MESON_BUILD" <<'PYEOF'
import sys

path = sys.argv[1]
with open(path, 'r') as fh:
    lines = fh.readlines()

nvkvm_block = """\

nvkvm_inc = include_directories('nvkvm_inc')

system_ss.add(when: ['CONFIG_VIRTIO'], if_true: files(
  'virtio_nvgpu.c',
  'virtio_nvgpu_pci.c',
  'nvkvm_dispatch.c',
  'nvkvm_frontend.c',
  'nvkvm_objects.c',
  'nvkvm_mmap_host.c',
  'nvkvm_handle.c',
  'nvkvm_isolate.c',
  'nvkvm_isolate_handlers.c',
  'nvkvm_tables.c',
  'nvkvm_present_egl.c',
))
"""

# Insert the block before the very last non-empty line.
insert_pos = len(lines)
for i in range(len(lines) - 1, -1, -1):
    if lines[i].strip():
        insert_pos = i
        break

lines.insert(insert_pos, nvkvm_block)

with open(path, 'w') as fh:
    fh.writelines(lines)

print("  meson.build patched successfully.")
PYEOF
else
    echo "  meson.build already contains virtio_nvgpu.c — skipping patch."
fi

# ── 6b. Patch hw/virtio/virtio.c — extend virtio_device_names table ──────────
# QEMU's virtio_device_names[] in virtio.c has entries only up to ID ~41.
# Our device type is 50, so we must extend the table; otherwise
# virtio_id_to_name() asserts "device_id < G_N_ELEMENTS(virtio_device_names)".
VIRTIO_C="$QEMU_SRC/hw/virtio/virtio.c"
if ! grep -q 'virtio-nvgpu' "$VIRTIO_C"; then
    # The table ends with a line like: [VIRTIO_ID_GPIO] = "virtio-gpio",
    # We append our entry right after it (before the closing brace).
    python3 - "$VIRTIO_C" <<'PYEOF'
import sys, re

path = sys.argv[1]
with open(path, 'r') as fh:
    text = fh.read()

# Rebuild fix 2026-07-04: insert our entry RIGHT AFTER the [VIRTIO_ID_GPIO] line
# inside virtio_device_names[].  The old code used text.rfind('};') which matched
# the LAST '};' in the file — the virtio_device_info TypeInfo, NOT the names table —
# corrupting an unrelated struct and breaking the build.  Anchor on the real entry.
# Rebuild fix 2026-07-19: in QEMU 9.2.0 the [VIRTIO_ID_GPIO] entry is the LAST in the
# initializer and has NO trailing comma ("virtio-gpio" then "};").  Make the trailing
# comma optional in the match, and always emit our own entries WITH the needed comma.
m = re.search(r'^([ \t]*)\[VIRTIO_ID_GPIO\][ \t]*=[ \t]*"virtio-gpio",?[ \t]*\n', text, re.M)
if not m:
    print("  ERROR: could not find [VIRTIO_ID_GPIO] entry in virtio_device_names[]", file=sys.stderr)
    sys.exit(1)
indent = m.group(1)
entry = '%s[VIRTIO_ID_GPIO] = "virtio-gpio",\n%s[50] = "virtio-nvgpu",\n' % (indent, indent)
text = text[:m.start()] + entry + text[m.end():]
with open(path, 'w') as fh:
    fh.write(text)
print("  virtio.c patched successfully (after VIRTIO_ID_GPIO).")
PYEOF
else
    echo "  virtio.c already contains virtio-nvgpu entry — skipping patch."
fi

# ── 7. Configure QEMU ─────────────────────────────────────────────────────
echo "[7/9] Configuring QEMU (target: x86_64-softmmu, KVM only)..."
cd "$QEMU_SRC"
./configure \
    --target-list=x86_64-softmmu \
    --enable-kvm \
    --disable-werror \
    --disable-sdl \
    --disable-gtk \
    --enable-opengl \
    `# opengl pulls in the egl-headless display + dpy_gl_scanout_dmabuf, the` \
    `# host-aligned present path (#102). virglrenderer stays off — the nvkvm` \
    `# present path scans out the guest render target's own dma-buf directly,` \
    `# it does not use virtio-gpu GL virgl.` \
    --disable-virglrenderer \
    --disable-vnc \
    `# Bench-rebuild fix 2026-07-29: WITHOUT this define nvkvm_isolate.c takes` \
    `# its #else branch (stub_elf = NULL) and the build still SUCCEEDS — the` \
    `# breakage only shows at runtime as "open ctl/gpu FAILED r1=-2 r2=-2".` \
    --extra-cflags=-DNVKVM_STUB_EMBEDDED \
    --prefix="$QEMU_PREFIX"

# ── 8. Build ──────────────────────────────────────────────────────────────
# Rebuild fix 2026-07-04: QEMU 9.2's ./configure creates the build tree in
# ./build (out-of-tree); build.ninja is NOT in $QEMU_SRC.  Run ninja against it.
echo "[8/9] Building QEMU with ninja -j$(nproc)..."
BUILD_DIR="$QEMU_SRC/build"
[ -f "$BUILD_DIR/build.ninja" ] || BUILD_DIR="$QEMU_SRC"   # fallback for in-tree configs
ninja -C "$BUILD_DIR" -j"$(nproc)"

# ── 9. Install ────────────────────────────────────────────────────────────
echo "[9/9] Installing to $QEMU_PREFIX..."
ninja -C "$BUILD_DIR" install

echo ""
echo "=== Build complete ==="
echo "Binary: $QEMU_PREFIX/bin/qemu-system-x86_64"
