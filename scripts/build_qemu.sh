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
REPO_ROOT="$(realpath "$(dirname "$0")/..")"

# Where the QEMU tree is cloned and the binary installed.  /opt when we can
# write it (the container case, and the historical default); otherwise a
# per-user prefix, so the whole build works rootless.  Override either
# explicitly with NVKVM_QEMU_SRC / NVKVM_QEMU_PREFIX.
nvkvm_default_prefix() {
    if [ -w /opt ] || [ "$(id -u)" -eq 0 ]; then
        echo "/opt"
    else
        echo "${XDG_DATA_HOME:-$HOME/.local/share}/nvkvm"
    fi
}
NVKVM_BASE="$(nvkvm_default_prefix)"
QEMU_SRC="${NVKVM_QEMU_SRC:-$NVKVM_BASE/qemu-src}"
QEMU_PREFIX="${NVKVM_QEMU_PREFIX:-$NVKVM_BASE/qemu-nvkvm}"

# ── Guard: host architecture ──────────────────────────────────────────────
# Checked here as well as at compile time so the failure arrives in one line,
# before ~10 minutes of QEMU build, rather than as a wall of assembler errors.
_arch="$(uname -m)"
if [ "$_arch" != "x86_64" ]; then
    echo "build_qemu.sh: unsupported host architecture '$_arch' -- nvkvm is x86-64 only." >&2
    echo "  The isolate stub uses x86-64 assembly, x86-64 syscall numbers, an" >&2
    echo "  AUDIT_ARCH_X86_64 seccomp filter and a CPUID-based MAXPHYADDR probe." >&2
    echo "  This is a deliberate limit, not an oversight: see src/stub/nvkvm_stub.c." >&2
    exit 1
fi

# ── Guard: already built ───────────────────────────────────────────────────
# --force rebuilds even when the binary exists.  You need this after editing
# anything under src/qemu/ or src/common/: the guard below is what makes a plain
# re-run a no-op, so without it you will test the OLD device against NEW guest
# code and get a confusing mismatch (a new NVKVM_HFILE_* id, for instance, comes
# back as "Invalid argument" because the old binary's whitelist rejects it).
# The QEMU source tree and ninja build dir are reused, so a forced rebuild is
# incremental — minutes, not the full ~20.
NVKVM_FORCE=0
NVKVM_ARG_INSTALL_DEPS=0
for arg in "$@"; do
    case "$arg" in
        -f|--force) NVKVM_FORCE=1 ;;
        --install-deps) NVKVM_ARG_INSTALL_DEPS=1 ;;
        -h|--help)
            echo "usage: $0 [--force] [--install-deps]"
            echo "  --force          rebuild even if the binary already exists"
            echo "  --install-deps   install missing build deps (needs root)"
            echo ""
            echo "env: NVKVM_QEMU_PREFIX  install path (default /opt/qemu-nvkvm,"
            echo "                        or ~/.local/share/nvkvm when /opt is not writable)"
            echo "     NVKVM_QEMU_SRC     QEMU source tree path"
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

# ── 1. Build dependencies ─────────────────────────────────────────────────
#
# This script does NOT install anything by default.  It is a convenience
# wrapper around an ordinary QEMU build (docs/howto/build.md walks the same
# steps by hand), and a build helper that silently runs a package manager as
# root is both surprising and Debian-only, which is what this used to be.
#
# So: probe for what the build actually needs, and if something is missing say
# exactly what, with the package names for the distro we appear to be on.
# --install-deps opts back in to installing them.
echo "[1/9] Checking build dependencies..."

nvkvm_distro() {
    if [ -r /etc/os-release ]; then . /etc/os-release; echo "${ID_LIKE:-$ID}"; fi
}

# Each entry: "<probe-kind>:<probe-arg>".  cmd = an executable on PATH,
# pc = a pkg-config module (which is what QEMU's configure actually looks for).
NVKVM_DEPS="cmd:git cmd:ninja cmd:meson cmd:pkg-config cmd:python3 cmd:xxd
pc:glib-2.0 pc:pixman-1 pc:slirp pc:epoxy pc:gbm pc:egl pc:libdrm"

nvkvm_missing() {
    local miss="" kind arg
    for d in $NVKVM_DEPS; do
        kind="${d%%:*}"; arg="${d#*:}"
        case "$kind" in
            cmd) command -v "$arg" >/dev/null 2>&1 || miss="$miss $arg" ;;
            pc)  pkg-config --exists "$arg" 2>/dev/null || miss="$miss $arg(dev)" ;;
        esac
    done
    echo "$miss"
}

# Best-effort package names.  Distros rename things; the probe list above is
# the authority on WHAT is needed, this is a convenience for the common cases.
nvkvm_pkg_hint() {
    case "$(nvkvm_distro)" in
        *debian*|*ubuntu*)
            echo "apt install ninja-build meson libglib2.0-dev libpixman-1-dev \\
     python3 python3-venv python3-tomli git libslirp-dev pkg-config \\
     libattr1-dev libepoxy-dev libgbm-dev libegl-dev libdrm-dev xxd" ;;
        *fedora*|*rhel*|*centos*)
            echo "dnf install ninja-build meson glib2-devel pixman-devel python3 \\
     git libslirp-devel pkgconf-pkg-config libattr-devel libepoxy-devel \\
     mesa-libgbm-devel mesa-libEGL-devel libdrm-devel vim-common" ;;
        *arch*)
            echo "pacman -S ninja meson glib2 pixman python git libslirp \\
     pkgconf attr libepoxy mesa libdrm xxd base-devel" ;;
        *suse*)
            echo "zypper install ninja meson glib2-devel libpixman-1-0-devel \\
     python3 git libslirp-devel pkg-config libattr-devel libepoxy-devel \\
     Mesa-libgbm-devel Mesa-libEGL-devel libdrm-devel vim" ;;
        *)  echo "(unrecognised distro -- install the equivalents of: ninja meson \\
     glib2 pixman slirp epoxy gbm egl libdrm pkg-config python3 git xxd)" ;;
    esac
}

NVKVM_INSTALL_DEPS=0
case "${NVKVM_BUILD_INSTALL_DEPS:-0}" in 1|yes|true) NVKVM_INSTALL_DEPS=1 ;; esac
[ "${NVKVM_ARG_INSTALL_DEPS:-0}" -eq 1 ] && NVKVM_INSTALL_DEPS=1

# The window backends need their own dev packages.  NVKVM_QEMU_UI=1 turns on
# --enable-gtk/--enable-sdl below, and without these the configure step dies
# with `Dependency "sdl2" not found` on an otherwise perfectly prepared box --
# measured on a clean Ubuntu 22.04.  Only pulled in when the UI is requested,
# so headless deployments keep their smaller dependency set.
NVKVM_UI_DEPS_DEB=""
NVKVM_UI_DEPS_RPM=""
NVKVM_UI_DEPS_ARCH=""
NVKVM_UI_DEPS_SUSE=""
if [ "${NVKVM_QEMU_UI:-0}" = "1" ]; then
    NVKVM_UI_DEPS_DEB="libgtk-3-dev libsdl2-dev"
    NVKVM_UI_DEPS_RPM="gtk3-devel SDL2-devel"
    NVKVM_UI_DEPS_ARCH="gtk3 sdl2"
    NVKVM_UI_DEPS_SUSE="gtk3-devel libSDL2-devel"
fi

MISSING="$(nvkvm_missing)"
if [ -n "$MISSING" ] && [ "$NVKVM_INSTALL_DEPS" -eq 1 ]; then
    echo "  installing:$MISSING"
    case "$(nvkvm_distro)" in
        *debian*|*ubuntu*) apt-get update -q && apt-get install -y \
            ninja-build meson libglib2.0-dev libpixman-1-dev python3 \
            python3-venv python3-tomli git libslirp-dev pkg-config \
            libattr1-dev libepoxy-dev libgbm-dev libegl-dev libdrm-dev xxd \
            $NVKVM_UI_DEPS_DEB ;;
        *fedora*|*rhel*|*centos*) dnf install -y \
            ninja-build meson glib2-devel pixman-devel python3 git \
            libslirp-devel pkgconf-pkg-config libattr-devel libepoxy-devel \
            mesa-libgbm-devel mesa-libEGL-devel libdrm-devel vim-common \
            $NVKVM_UI_DEPS_RPM ;;
        *arch*) pacman -S --noconfirm --needed ninja meson glib2 pixman python \
            git libslirp pkgconf attr libepoxy mesa libdrm base-devel \
            $NVKVM_UI_DEPS_ARCH ;;
        *suse*) zypper install -y ninja meson glib2-devel libpixman-1-0-devel \
            python3 git libslirp-devel pkg-config libattr-devel \
            libepoxy-devel Mesa-libgbm-devel Mesa-libEGL-devel libdrm-devel vim \
            $NVKVM_UI_DEPS_SUSE ;;
        *) echo "ERROR: --install-deps: unrecognised distro" >&2; exit 1 ;;
    esac
    MISSING="$(nvkvm_missing)"
fi

if [ -n "$MISSING" ]; then
    cat >&2 <<EOF

ERROR: missing build dependencies:$MISSING

Install them and re-run, or pass --install-deps to have this script do it
(which needs root).  On this system that is roughly:

  $(nvkvm_pkg_hint)

The build itself needs no root: it installs to
  $QEMU_PREFIX
Set NVKVM_QEMU_PREFIX to change that.  docs/howto/build.md does the same
steps by hand if you would rather not run this script at all.
EOF
    exit 1
fi
echo "  all present"

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
# The stub is EMBEDDED in the QEMU binary (nvkvm_stub_bin.h, below), so this
# copy is only the runtime fallback for a QEMU built without the embed.  Skip
# it rather than fail when /usr/lib is not writable -- that is the whole
# difference between "needs root" and "does not".
if install -d /usr/lib/nvkvm 2>/dev/null &&
   install -m 0755 "$REPO_ROOT/src/stub/nvkvm_stub" /usr/lib/nvkvm/nvkvm_stub 2>/dev/null; then
    echo "  stub installed at /usr/lib/nvkvm/nvkvm_stub (runtime fallback)"
else
    echo "  /usr/lib/nvkvm not writable — skipping the fallback copy."
    echo "  Harmless: the stub is embedded in the QEMU binary. Set"
    echo "  NVKVM_STUB_PATH=$REPO_ROOT/src/stub/nvkvm_stub if you ever need it."
fi

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

# ── 6b. Patch ui/egl-helpers.c: import dma-bufs with TexStorage ───────────
#
# QEMU's egl_dmabuf_import_texture() binds an imported dma-buf with
# glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, ...).  NVIDIA hands the guest's
# scanout buffers out as *external-only* EGLImages and rejects that call:
# measured on an RTX 4070 / 595.84, the 2D bind returns GL_INVALID_OPERATION
# (0x0502) while glEGLImageTargetTexStorageEXT (GL_EXT_EGL_image_storage)
# succeeds on the very same image.  Without this, -display gtk,gl=on imports
# every frame, drops it, and shows a black window -- so the only usable path is
# NVKVM_PRESENT_MODE=readback, which costs a full ~8MB glReadPixels per frame on
# the main loop and cannot reach 60fps at 1080p.
#
# Same fix as nvkvm_import_dmabuf_tex() in src/qemu/nvkvm_present_egl.c.
echo "[6b/9] Patching ui/egl-helpers.c for TexStorage dma-buf import..."
if grep -q "glEGLImageTargetTexStorageEXT" "$QEMU_SRC/ui/egl-helpers.c"; then
    echo "  egl-helpers.c already patched -- skipping."
else
    python3 - "$QEMU_SRC/ui/egl-helpers.c" <<'EGLPATCH'
import sys
path = sys.argv[1]
src = open(path).read()
old = """    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);
    eglDestroyImageKHR(qemu_egl_display, image);"""
new = """    {
        /* nvkvm: NVIDIA rejects the legacy OES bind for external-only
         * dma-buf images (GL_INVALID_OPERATION); EXT_EGL_image_storage
         * takes the same image as immutable GL_TEXTURE_2D storage. */
        static PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC nvkvm_tex_storage;
        static bool nvkvm_looked_up;
        if (!nvkvm_looked_up) {
            nvkvm_looked_up = true;
            nvkvm_tex_storage = (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)
                eglGetProcAddress("glEGLImageTargetTexStorageEXT");
        }
        if (nvkvm_tex_storage) {
            nvkvm_tex_storage(GL_TEXTURE_2D, (GLeglImageOES)image, NULL);
        } else {
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);
        }
    }
    eglDestroyImageKHR(qemu_egl_display, image);"""
if old not in src:
    sys.exit("egl-helpers.c: expected bind sequence not found")
open(path, 'w').write(src.replace(old, new, 1))
print("  ui/egl-helpers.c patched.")
EGLPATCH
fi

# ── 6c. Patch ui/console.c: do not abort when a console has no "device" ──────
#
# `screendump <file> nvkvm0` killed QEMU outright:
#
#   Unexpected error in object_property_find_err() at ../qom/object.c:1349:
#   Property 'qemu-fixed-text-console.device' not found
#
# qemu_console_lookup_by_device() walks EVERY console and reads the "device"
# link with &error_abort.  But "device" (and "head") are class properties of
# qemu-graphic-console only -- text consoles never have them -- so the walk
# aborts the moment it steps over a text console on the way to the console it
# was asked for.  The sibling loop at qemu_graphic_console_lookup_unused()
# already guards with QEMU_IS_GRAPHIC_CONSOLE(); this one simply forgot.
#
# It is order-dependent, not device-specific: a VGA whose console happens to be
# first in the list is found before the walk reaches any text console and so
# survives, which is exactly why this went unnoticed.  nvkvm's console is
# registered later and sits behind one.
#
# Non-graphic consoles can never be the answer here anyway, so skipping them is
# both the minimal fix and the upstream-shaped one.
echo "[6c/9] Patching ui/console.c for console lookup abort..."
if grep -q "nvkvm: .device. and .head. are class properties" "$QEMU_SRC/ui/console.c"; then
    echo "  console.c already patched -- skipping."
else
    python3 - "$QEMU_SRC/ui/console.c" <<'CONPATCH'
import sys
path = sys.argv[1]
src = open(path).read()
old = """    QTAILQ_FOREACH(con, &consoles, next) {
        obj = object_property_get_link(OBJECT(con),
                                       "device", &error_abort);
        if (DEVICE(obj) != dev) {
            continue;
        }
        h = object_property_get_uint(OBJECT(con),
                                     "head", &error_abort);"""
new = """    QTAILQ_FOREACH(con, &consoles, next) {
        /* nvkvm: "device" and "head" are class properties of
         * qemu-graphic-console ONLY.  Text consoles do not have them, and both
         * reads below pass &error_abort -- so walking past a text console on
         * the way to the requested one aborts QEMU ("Property
         * 'qemu-fixed-text-console.device' not found").  A device whose
         * console happens to come first is found before the walk gets there,
         * which is why this is order-dependent rather than device-specific.
         * A non-graphic console can never be the match, so skip it. */
        if (!QEMU_IS_GRAPHIC_CONSOLE(con)) {
            continue;
        }
        obj = object_property_get_link(OBJECT(con),
                                       "device", &error_abort);
        if (DEVICE(obj) != dev) {
            continue;
        }
        h = object_property_get_uint(OBJECT(con),
                                     "head", &error_abort);"""
if old not in src:
    sys.exit("console.c: qemu_console_lookup_by_device() body not found as expected")
open(path, 'w').write(src.replace(old, new, 1))
print("  ui/console.c patched.")
CONPATCH
fi

# ── 7. Configure QEMU ─────────────────────────────────────────────────────
#
# Window backends are OFF by default: the normal deployment is headless (a
# container or a remote host), where GTK/SDL only add build dependencies and
# attack surface for a window nobody can see.  Set NVKVM_QEMU_UI=1 to build
# them in -- needed to run the guest desktop in a real window on a machine
# with a physical display:
#
#   NVKVM_QEMU_UI=1 scripts/build_qemu.sh --force
#   qemu-system-x86_64 ... -display gtk,gl=on
#
# The present path (#102) is the same either way; -display only decides where
# the composited guest frame lands.
if [ "${NVKVM_QEMU_UI:-0}" = "1" ]; then
    NVKVM_UI_FLAGS="--enable-gtk --enable-sdl"
    echo "  NVKVM_QEMU_UI=1 -> building with GTK + SDL window backends."
else
    NVKVM_UI_FLAGS="--disable-sdl --disable-gtk"
fi
echo "[7/9] Configuring QEMU (target: x86_64-softmmu, KVM only)..."
cd "$QEMU_SRC"
./configure \
    --target-list=x86_64-softmmu \
    --enable-kvm \
    --disable-werror \
    $NVKVM_UI_FLAGS \
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
