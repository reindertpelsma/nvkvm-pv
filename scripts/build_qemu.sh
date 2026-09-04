#!/usr/bin/env bash
# build_qemu.sh — clone QEMU 11.1, patch virtio-nvgpu into it, and build a
#                 minimal KVM-only QEMU binary at /opt/qemu-nvkvm.
#
# This script is a CONVENIENCE, not the mechanism.  Everything it does to
# upstream QEMU is twelve patch files in patches/, applied with `git apply`;
# everything it adds is a file copy.  docs/howto/build.md walks the identical
# sequence by hand, and you can follow it instead of running this — that is the
# point of keeping the delta as patches rather than as sed expressions.
#
# Guarded: if /opt/qemu-nvkvm/bin/qemu-system-x86_64 already exists the script
# prints a message and exits successfully WITHOUT rebuilding anything.  Pass
# --force to rebuild over an existing install -- you need it after editing
# anything under src/qemu/ or src/common/.

set -euo pipefail

QEMU_VERSION="11.1.1"
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

# The "already built" guard is CONTENT-ADDRESSED, not existence-only.
#
# MEASURED 2026-09-01: a --ssh sweep against a host that had been swept before
# reused a QEMU binary built 1h37m earlier, so a change to
# src/qemu/nvkvm_fe_alloc_allowlist.h was never compiled in -- while every
# record still carried the NEW tree's git sha. The run measured a binary that
# no commit corresponds to. On a rented box this cannot happen (the box is
# fresh, there is no binary), which is exactly why it survived: the manual-host
# axis is the first one that reuses a machine.
#
# Telling the operator to remember --force (as the old message did) is not a
# guard. The stamp is a hash of everything that ends up IN the binary: the
# EVERYTHING that reaches the binary, not just src/qemu: this script copies
# src/abi/*.h and src/common/*.h into the QEMU tree (see the 2026-07-04 note
# further down) and builds src/stub. A stamp over src/qemu alone would let an
# edit to src/abi/nvgpu.h -- where every GPU object class lives -- reuse a
# stale binary, which is the exact bug this guard exists to prevent.
nvkvm_build_stamp() {
    { find src/qemu src/common src/abi src/stub patches -type f \( -name '*.c' -o -name '*.h' -o -name '*.patch' -o -name 'Makefile' \) \
        -print0 2>/dev/null | sort -z | xargs -0 sha256sum 2>/dev/null
      printf '%s\n' "$QEMU_VERSION"
    } | sha256sum | cut -d' ' -f1
}
STAMP_FILE="$QEMU_PREFIX/.nvkvm-build-stamp"
WANT_STAMP="$(nvkvm_build_stamp)"

if [ -x "$QEMU_PREFIX/bin/qemu-system-x86_64" ] && [ "$NVKVM_FORCE" -eq 0 ]; then
    HAVE_STAMP="$(cat "$STAMP_FILE" 2>/dev/null || true)"
    if [ -n "$HAVE_STAMP" ] && [ "$HAVE_STAMP" = "$WANT_STAMP" ]; then
        echo "INFO: $QEMU_PREFIX/bin/qemu-system-x86_64 is current for these sources — skipping build."
        exit 0
    fi
    if [ -z "$HAVE_STAMP" ]; then
        echo "INFO: an existing binary carries no build stamp (built before stamping, or by hand)."
        echo "INFO: REBUILDING rather than trusting it -- a binary we cannot attribute is not a binary we can measure."
    else
        echo "INFO: the QEMU sources changed since this binary was built. REBUILDING."
        echo "INFO:   built from $HAVE_STAMP"
        echo "INFO:   sources are $WANT_STAMP"
    fi
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
    # PROBE for them too, not just name the packages.
    #
    # MISSING is computed from NVKVM_DEPS below, and the apt block is gated on
    # `[ -n "$MISSING" ]`.  With gtk/sdl absent from the probe list, a box whose
    # base deps are already satisfied -- i.e. anyone who followed install.md and
    # did the headless build first -- produced an EMPTY $MISSING, skipped the
    # apt block entirely, and never installed libgtk-3-dev/libsdl2-dev.
    # --install-deps became a silent no-op and meson died with
    # `Dependency "sdl2" not found`, which is the exact failure the comment
    # above says these packages exist to prevent.  Reported from a LeaderGPU
    # box, 2026-09-05, on the second build of the same tree.
    NVKVM_DEPS="$NVKVM_DEPS pc:gtk+-3.0 pc:sdl2"
    NVKVM_UI_DEPS_DEB="libgtk-3-dev libsdl2-dev"
    NVKVM_UI_DEPS_RPM="gtk3-devel SDL2-devel"
    NVKVM_UI_DEPS_ARCH="gtk3 sdl2"
    NVKVM_UI_DEPS_SUSE="gtk3-devel libSDL2-devel"
fi

# apt refuses to run while something else holds the dpkg lock, and on Debian
# and Ubuntu `unattended-upgrades` starts automatically on a freshly
# provisioned machine -- which is exactly when somebody first runs
# --install-deps. The raw failure is
#
#   E: Could not get lock /var/lib/dpkg/lock-frontend. It is held by
#      process NNNN (unattended-upgr)
#
# and nothing in it says that waiting sixty seconds would fix it. Measured on a
# rented Ubuntu 22.04 box, 2026-09-03: --install-deps died there and never
# reached the build. Wait, bounded, and say what is being waited for.
nvkvm_wait_dpkg_lock() {
    command -v flock >/dev/null 2>&1 || return 0   # not Debian-ish; nothing to do
    [ -e /var/lib/dpkg/lock-frontend ] || return 0
    local waited=0 limit=300
    while ! flock -n /var/lib/dpkg/lock-frontend -c true 2>/dev/null; do
        if [ "$waited" -eq 0 ]; then
            echo "  waiting for another package manager to release the dpkg lock"
            echo "  (usually unattended-upgrades on a freshly provisioned box; up to ${limit}s)"
        fi
        if [ "$waited" -ge "$limit" ]; then
            echo "ERROR: the dpkg lock was still held after ${limit}s." >&2
            echo "       Something else is installing packages. Wait for it to finish" >&2
            echo "       (systemctl status unattended-upgrades) and re-run." >&2
            return 1
        fi
        sleep 5
        waited=$((waited + 5))
    done
    [ "$waited" -gt 0 ] && echo "  dpkg lock released after ${waited}s"
    return 0
}

MISSING="$(nvkvm_missing)"
if [ -n "$MISSING" ] && [ "$NVKVM_INSTALL_DEPS" -eq 1 ] && [ "$(id -u)" -ne 0 ]; then
    # --install-deps runs the package manager directly.  Without this the first
    # thing a reader of install.md sees is a bare "apt-get: Permission denied"
    # from inside a build script, which reads like the script is broken rather
    # than under-privileged.  The usage text has always said "(needs root)".
    echo "ERROR: --install-deps installs packages and must run as root." >&2
    echo "       Re-run:  sudo bash $0 --install-deps" >&2
    echo "       Or install these yourself and drop the flag:$MISSING" >&2
    exit 1
fi
if [ -n "$MISSING" ] && [ "$NVKVM_INSTALL_DEPS" -eq 1 ]; then
    echo "  installing:$MISSING"
    case "$(nvkvm_distro)" in
        *debian*|*ubuntu*) nvkvm_wait_dpkg_lock && apt-get update -q && apt-get install -y \
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

# ── 2. Clone QEMU 11.1 stable ─────────────────────────────────────────────
if [ ! -d "$QEMU_SRC" ]; then
    echo "[2/9] Cloning QEMU $QEMU_VERSION..."
    git clone --depth=1 --branch "v${QEMU_VERSION}" \
        https://gitlab.com/qemu-project/qemu.git "$QEMU_SRC"
else
    echo "[2/9] QEMU source already present at $QEMU_SRC — skipping clone."
fi

# ── 2b. Guard: is that tree the exact commit the patches are written against? ──
#
# Added with the 9.2.0 -> 11.1.1 bump, because the failure it catches is the
# single most likely way to hit this upgrade badly.  The clone above is skipped
# whenever $QEMU_SRC merely EXISTS, so a box that built the old QEMU keeps its
# 9.2.0 tree, every one of the twelve patches then fails to apply, and the
# error you get blames the patches.
#
# IT NOW CHECKS THE COMMIT, NOT THE TAG, and the difference is the whole point.
# It used to run `git describe --tags --exact-match HEAD` -- which asks the tree
# we just cloned what tag it is on.  The clone was `--branch v11.1.1`, so the
# answer was always "the tag the server gave us", and the check could only ever
# agree with itself.  A git tag is a mutable pointer on a third-party host: if
# gitlab.com's tag moved, or a mirror or a local tree carried a v11.1.1 pointing
# somewhere else, the tree changed and this guard still said "as the patches
# expect".  A commit id cannot be re-pointed.
#
# The pin below was read from gitlab.com's API on 2026-08-29 for the v11.1.1
# tag.  That is the same trust root as the clone, so this does not
# independently authenticate upstream QEMU -- what it buys is that the tree
# STOPS CHANGING: a moved tag now fails the build loudly instead of silently
# swapping the hypervisor the guests run on.  Verifying QEMU's release signature
# would be the real anchor, and needs their signing key, which is a larger
# change than this file should make alone.
#
# Still only HEAD, not the working tree: patches are applied with `git apply`
# and never committed, so HEAD stays put for the whole build and a re-run is a
# no-op.  Unlike the tag check this FAILS CLOSED -- an unreadable commit id
# means we do not know what we are about to patch and compile into the thing
# that runs every guest, and "continue and let the patches decide" was how a
# check that never fired looked correct for a whole release cycle.
QEMU_COMMIT="c3d48b7d1e89604920e5b81b91140c2ad39a1943"   # tag v11.1.1
_qemu_head="$(git -C "$QEMU_SRC" rev-parse HEAD 2>/dev/null || true)"
if [ "$_qemu_head" = "$QEMU_COMMIT" ]; then
    echo "  tree is at $QEMU_COMMIT (v$QEMU_VERSION), as the patches expect."
else
    _qemu_tag="$(git -C "$QEMU_SRC" describe --tags --always HEAD 2>/dev/null || echo unknown)"
    cat >&2 <<EOF

ERROR: $QEMU_SRC is not the QEMU the patches in patches/ are written against.

  expected commit : $QEMU_COMMIT  (tag v$QEMU_VERSION)
  this tree is at : ${_qemu_head:-<not a git tree>}  ($_qemu_tag)

Usually a tree left over from an earlier nvkvm build -- the clone above is
skipped whenever the directory exists.  Remove it and re-run:

    rm -rf $QEMU_SRC && $0 --force

If you meant to build a DIFFERENT QEMU, change QEMU_VERSION and QEMU_COMMIT
together and re-check the twelve patches against it.  They are written against
this commit and nothing else, and a patch that applies with fuzz to a different
tree is how you get a QEMU that builds and then misbehaves under load.

EOF
    exit 1
fi

# ── 3. Apply the QEMU patch series ────────────────────────────────────────
#
# Everything nvkvm changes in *upstream* QEMU lives in patches/ as twelve
# ordinary patch files.  "What does this do to my QEMU?" is therefore answered
# by reading twelve diffs, not by reading this script and mentally executing the
# edits it generates.  Each patch carries a header saying why it exists;
# patches/README.md is the index, and docs/howto/build.md walks the same steps
# by hand.
#
# This step used to be sed plus inline `python3 - <<EOF` heredocs that mutated
# the four files it then touched in place.  Three things were wrong with that:
#   * it produced no diff, so the delta was not reviewable;
#   * it was not replicable by hand -- `git apply` is, a sed replacement is not;
#   * a QEMU version bump meant rewriting the editing logic rather than
#     resolving conflicts with ordinary tools.
#
# Idempotency is decided by RESETTING the upstream files the series touches
# back to the tag and re-applying (see the note at the loop below).  It used to
# be decided by `git apply --reverse --check` per patch, which is unsound as
# soon as two patches touch one region -- 0001 and 0012 both edit the same
# meson.build block -- and before that by grepping the tree for a comment
# string taken from the patch body, so rewording a comment silently made the
# patch apply a second time.
#
# The whole pending set is --check'ed as ONE unit before anything is written.
# git apply is all-or-nothing per invocation, so that is what guarantees a
# mismatch fails with the tree untouched rather than half-mutated.
echo "[3/9] Applying the QEMU patch series..."

PATCH_DIR="$REPO_ROOT/patches"
shopt -s nullglob
NVKVM_PATCHES=( "$PATCH_DIR"/*.patch )
shopt -u nullglob
if [ "${#NVKVM_PATCHES[@]}" -eq 0 ]; then
    echo "ERROR: no *.patch files found in $PATCH_DIR" >&2
    echo "  The QEMU delta lives there; without it the build produces a stock" >&2
    echo "  QEMU that does not know what virtio-nvgpu is." >&2
    exit 1
fi

cd "$QEMU_SRC"
#
# Apply INCREMENTALLY, not check-everything-then-apply-everything.
#
# The series is ordered, and later patches may touch lines that earlier ones
# introduce -- 0010 edits code that 0009 adds to ui/sdl2.c.  Validating every
# patch against the tree as it stands BEFORE applying any of them therefore
# fails for the first genuinely dependent patch in the series, with a message
# blaming the tree.  That is what happened: 0012 applies perfectly to a v11.1.1
# tree with 0001-0011 on it -- it appends to the meson.build file list that 0001
# creates -- and could never apply to a pristine one.
#
# Atomicity is kept by rolling back on failure instead of by batching: each
# patch is checked against the CURRENT tree, applied immediately, and recorded,
# so a failure can reverse exactly what this run did and leave the tree as it
# was found.
#
#
# ...and RESET the files the series touches before applying, rather than
# asking each patch "are you already applied?".
#
# Bump fix 2026-08-27.  The per-patch `git apply --reverse --check` test is not
# sound once two patches touch the same region: 0012 appends two lines INSIDE
# the meson.build block that 0001 adds, so on an already-patched tree 0001 no
# longer reverses cleanly -- the text it would remove is not the text it added.
# The script then declared the tree neither-applied-nor-appliable and refused,
# which meant `--force` (the documented way to rebuild after editing
# src/qemu/) failed on every tree it had itself produced.  It was not a version
# problem; it arrived with 0012 and would have bitten 9.2.0 identically.
#
# Resetting is sound where the reverse-check is not, and it is the same claim
# patches/README.md already makes: the QEMU tree is the tag plus this series
# and NOTHING else, so restoring the tag's copy of exactly the files the series
# names and re-applying is a no-op by construction.  Only tracked upstream
# files are touched -- the nvkvm sources copied into hw/misc/ are untracked and
# survive, as does the build directory.
#
NVKVM_TOUCHED=$(sed -n 's|^diff --git a/\(.*\) b/.*|\1|p' "${NVKVM_PATCHES[@]}" | sort -u)
if [ -n "$NVKVM_TOUCHED" ]; then
    # shellcheck disable=SC2086
    if ! git checkout -- $NVKVM_TOUCHED 2>/dev/null; then
        echo "ERROR: could not restore $QEMU_SRC to a pristine v$QEMU_VERSION." >&2
        echo "  Files: $(echo $NVKVM_TOUCHED | tr '\n' ' ')" >&2
        echo "  To start clean:  rm -rf $QEMU_SRC && $0 --force" >&2
        exit 1
    fi
    echo "  reset $(echo "$NVKVM_TOUCHED" | wc -l) upstream file(s) to v$QEMU_VERSION"
fi

NVKVM_APPLIED=()
NVKVM_NEW=0
for _p in "${NVKVM_PATCHES[@]}"; do
    _name="$(basename "$_p")"
    if git apply --check "$_p" 2>/dev/null; then
        git apply "$_p"
        NVKVM_APPLIED+=( "$_p" )
        NVKVM_NEW=$((NVKVM_NEW + 1))
        echo "  applied: $_name"
    else
        if [ "${#NVKVM_APPLIED[@]}" -gt 0 ]; then
            echo "  rolling back ${#NVKVM_APPLIED[@]} patch(es) applied this run..." >&2
            for (( _i=${#NVKVM_APPLIED[@]}-1; _i>=0; _i-- )); do
                git apply --reverse "${NVKVM_APPLIED[$_i]}" 2>/dev/null || true
            done
        fi
        cat >&2 <<EOF

ERROR: $_name does not apply to
       $QEMU_SRC

The files this series touches were just reset to v$QEMU_VERSION, so the tree
is pristine upstream plus the patches before this one.  That means:
  * the QEMU tree is not $QEMU_VERSION (the patches are written against the
    v$QEMU_VERSION tag and nothing else) -- step 2b checks this;
  * or the patch was edited and no longer matches upstream context.

Note the series is applied in order and later patches may depend on earlier
ones, so this is reported against the tree WITH its predecessors applied.

git's own reason follows.  To start clean:
    rm -rf $QEMU_SRC && $0 --force
EOF
        git apply --check "$_p" >&2 || true
        exit 1
    fi
done

if [ "$NVKVM_NEW" -ne "${#NVKVM_PATCHES[@]}" ]; then
    echo "ERROR: applied $NVKVM_NEW of ${#NVKVM_PATCHES[@]} patches." >&2
    exit 1
fi

# ── 4. Copy nvkvm QEMU source files into hw/misc/ ─────────────────────────
# The device itself is NOT a patch: it is ~12,900 lines of new files that touch
# nothing upstream, so a patch would be 12,900 lines of pure addition and no
# easier to review than the files it copies.  Patches are for the upstream
# files we modify; new files are copied.
echo "[4/9] Copying nvkvm QEMU source files to $QEMU_SRC/hw/misc/..."
cp "$REPO_ROOT/src/qemu/"*.c "$QEMU_SRC/hw/misc/"
cp "$REPO_ROOT/src/qemu/"*.h "$QEMU_SRC/hw/misc/"

# ── 5. Copy ABI / common headers into hw/misc/nvkvm_inc/ ──────────────────
echo "[5/9] Copying ABI and common headers to $QEMU_SRC/hw/misc/nvkvm_inc/..."
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

# ── 6. Fix include paths in the copied files ──────────────────────────────
echo "[6/9] Fixing include paths in copied files..."
# The nvkvm sources use relative paths like ../../src/common/foo.h and
# ../../src/abi/bar.h that are correct relative to src/qemu/ but wrong inside
# hw/misc/.  Rewrite EVERY such include (across all copied .c and .h) to the
# local nvkvm_inc/ sub-directory — generalized so new headers don't break it.
#
# These two seds survive the move to a patch series on purpose: they rewrite
# only files nvkvm owns and that this script just copied in, so they are part
# of "install our sources", not part of "modify QEMU".  Nothing upstream is
# touched here, and re-running them is a no-op because the copy is fresh.
#
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
# Rebuild fix 2026-07-04: QEMU's ./configure creates the build tree in
# ./build (out-of-tree); build.ninja is NOT in $QEMU_SRC.  Run ninja against it.
echo "[8/9] Building QEMU with ninja -j$(nproc)..."
BUILD_DIR="$QEMU_SRC/build"
[ -f "$BUILD_DIR/build.ninja" ] || BUILD_DIR="$QEMU_SRC"   # fallback for in-tree configs
ninja -C "$BUILD_DIR" -j"$(nproc)"

# ── 9. Install ────────────────────────────────────────────────────────────
echo "[9/9] Installing to $QEMU_PREFIX..."
ninja -C "$BUILD_DIR" install

echo ""
printf '%s\n' "$WANT_STAMP" > "$STAMP_FILE" 2>/dev/null || \
    echo "WARN: could not write $STAMP_FILE; the next run will rebuild rather than trust this binary."
echo "=== Build complete ==="
echo "Binary: $QEMU_PREFIX/bin/qemu-system-x86_64"
