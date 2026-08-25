#!/usr/bin/env bash
# steamos_boot.sh — single self-repairing nvkvm provisioning + boot script for
# SteamOS guests running under nvkvm-pv.
#
# One mechanism, run at every boot, that converges the system to a KNOWN state.
# There is no "fresh install" path and no "update" path: every step is
# check-then-act and is safe to re-run. It lives on the read-only 9p share, so
# changing this logic never means touching the SteamOS image.
#
# Modes
#   --install-only [--root DIR]   Part 1. Filesystem + network + NVIDIA probing.
#                                 Touches NO running-kernel state, so it is safe
#                                 inside a chroot of a DIFFERENT image -- which is
#                                 exactly what the update hook does. Give it the
#                                 new image's root and it provisions that image.
#   --driver-version VERSION      Optional host/container NVIDIA version. The
#                                 disposable installer passes this because it
#                                 has no loaded nvkvm module of its own. Without
#                                 it, read the version nvkvm exposes in procfs.
#   boot                          Part 2. Part 1, then load nvkvm, check it, and
#                                 return so the desktop can start. It does NOT
#                                 verify the desktop -- see validate() below for
#                                 exactly what is and is not established.
#   validate                      nvkvm health check only. Never a desktop check.
#
# Options
#   --root DIR              operate on this root instead of /
#   --profile steamos|compute   see PROFILES below
#   --old-run-file DIR      bind-mounted cache DIRECTORY of NVIDIA .run files
#
# PROFILES (deterministic, never chosen automatically)
#   steamos  (default)  Trims CUDA/OpenCL/NVVM/OptiX. Gaming stack only.
#                       Casualty: GPU PhysX. Vulkan, GL, DXVK, VKD3D, gamescope
#                       and DLSS (libnvidia-ngx) are UNAFFECTED.
#   compute             Installs everything. Nothing is trimmed.
# The profile is NEVER switched on the basis of free disk space. If the disk is
# too small the script fails and tells you to pick a profile, exactly like
# upstream's `Rerun with --trim-cuda`. Otherwise two machines running the same
# script would end up with different capabilities depending on how full they
# happened to be, and nobody could explain why.

set -uo pipefail   # deliberately NOT -e: this script repairs and reports.

# ── Constants ────────────────────────────────────────────────────────────────
NVKVM_SHARE_TAG="${NVKVM_SHARE_TAG:-nvkvm}"
# Mount point is /run/nvkvm, NOT /mnt/nvkvm. MEASURED: in SteamOS /mnt is a symlink
# to var/mnt, and /var is a rauc-managed partition that post-install.sh REFORMATS on
# every update. /run is tmpfs, always writable, exists before any partition is
# mounted, and never survives a boot -- exactly what a mount point should be.
NVKVM_SHARE_MNT="${NVKVM_SHARE_MNT:-/run/nvkvm}"
NVKVM_DATA_TAG="${NVKVM_DATA_TAG:-data}"
NVKVM_DATA_MNT=""
MODULE_NAME=nvkvm-guest
MODULE_MOD=nvkvm_guest

# The kernel-supplied paths validate() reads, named once. Overridable only so
# test/run_tests.sh can point them at a fake /proc and /sys and exercise the
# failure classes on a machine with no nvkvm at all; nothing on any real path
# sets these.
PROC_MODULES="${NVKVM_PROC_MODULES:-/proc/modules}"
DEV_NVIDIACTL="${NVKVM_DEV_NVIDIACTL:-/dev/nvidiactl}"
DRM_CLASS_DIR="${NVKVM_DRM_CLASS_DIR:-/sys/class/drm}"
# The Vulkan loader searches BOTH of these and nvidia-installer writes to /etc.
# MEASURED -- checking only /usr/share reports a healthy install as broken.
VULKAN_ICD_PATHS="${NVKVM_VULKAN_ICD_PATHS:-/etc/vulkan/icd.d/nvidia_icd.json /usr/share/vulkan/icd.d/nvidia_icd.json}"

# Upstream steamos-nvidia-installer's tested trim set, reused verbatim
# (steamos-nvidia-installer.sh:479). libnvidia-ngx is deliberately NOT here --
# that is DLSS and it must survive.
TRIM_RE='libcuda|libcudadebugger|libnvidia-nvvm|libnvidia-opencl|libnvoptix|nvidia-cuda-mps|OpenCL'

PROFILE="${NVKVM_PROFILE:-steamos}"
RUN_CACHE_DIR=""          # set by --old-run-file, else derived below
RUN_CACHE_KEEP=2
DRIVER_VERSION=""         # explicit host/container version, never hardcoded
ROOT=/
MODULE_REBUILT=0          # set when this run actually rebuilt the .ko
MODULE_LOAD_REFRESHED=0   # avoid unloading/reloading twice during `boot`

# All diagnostics go to STDERR. `log` used to write to stdout, and helpers return
# their result via stdout -- so `run="$(locate_or_fetch_run ...)"` captured the log
# line along with the path and every extraction failed with a confusing error.
log()  { printf '[nvkvm] %s\n' "$*" >&2; }
warn() { printf '[nvkvm] WARNING: %s\n' "$*" >&2; }
err()  { printf '[nvkvm] ERROR: %s\n' "$*" >&2; }

in_target() { if [ "$ROOT" = "/" ]; then "$@"; else chroot "$ROOT" "$@"; fi; }
rp() { if [ "$ROOT" = "/" ]; then printf '%s' "$1"; else printf '%s%s' "${ROOT%/}" "$1"; fi; }

# ── Read-only state: observe it, then restore whatever we observed ───────────
# Restoring only on the happy path leaves the filesystem writable after any
# mid-script failure, which is a worse state than the one we started in.
RO_PRIOR=unknown
capture_ro_state() {
    if [ "$ROOT" = "/" ]; then
        if steamos-readonly status >/dev/null 2>&1; then RO_PRIOR=ro; else RO_PRIOR=rw; fi
    else
        case "$(btrfs property get "$ROOT" ro 2>/dev/null)" in
            *true) RO_PRIOR=ro ;; *) RO_PRIOR=rw ;;
        esac
    fi
    log "rootfs was ${RO_PRIOR} on entry"
}
steamos_unlock() {
    if [ "$ROOT" = "/" ]; then
        steamos-readonly disable 2>/dev/null || {
            mount -o remount,rw / 2>/dev/null
            btrfs property set / ro false 2>/dev/null
        }
    else
        btrfs property set "$ROOT" ro false 2>/dev/null
    fi
}
steamos_lock() {
    if [ "$ROOT" = "/" ]; then
        steamos-readonly enable 2>/dev/null || btrfs property set / ro true 2>/dev/null
    else
        btrfs property set "$ROOT" ro true 2>/dev/null
    fi
}
restore_ro_state() {
    [ "$RO_PRIOR" = ro ] && steamos_lock
    return 0
}

# ── 9p share ─────────────────────────────────────────────────────────────────
mount_9p_share() {
    if mountpoint -q "$NVKVM_SHARE_MNT" 2>/dev/null; then return 0; fi
    mkdir -p "$NVKVM_SHARE_MNT"
    modprobe 9pnet_virtio 2>/dev/null || true
    mount -t 9p -o trans=virtio,version=9p2000.L,ro,msize=512000 \
          "$NVKVM_SHARE_TAG" "$NVKVM_SHARE_MNT" 2>/dev/null
}
repo_commit() {
    git -C "$NVKVM_SHARE_MNT" rev-parse HEAD 2>/dev/null \
        || cat "$NVKVM_SHARE_MNT/.nvkvm-source-id" 2>/dev/null
}

# The operator data share is independent of the read-only source share. Mount
# tag `data` at the interactive user's ~/data when present; a hand-written QEMU
# command without that tag must still boot. Do this only for the running root:
# mounting it below an offline update root would leave a nested mount that can
# prevent Valve's update machinery from unmounting the staged slot.
mount_data_share() {
    NVKVM_DATA_MNT=""
    [ "$ROOT" = / ] || { log "data: offline target; deferring the optional 9p mount to first boot"; return 1; }
    local u home
    for u in deck user; do
        home="$(awk -F: -v n="$u" '$1==n{print $6}' /etc/passwd)"
        [ -n "$home" ] && break
    done
    [ -n "${home:-}" ] || { warn "data: no deck/user home found; optional share not mounted"; return 1; }
    NVKVM_DATA_MNT="$home/data"
    mkdir -p "$NVKVM_DATA_MNT"
    if mountpoint -q "$NVKVM_DATA_MNT" 2>/dev/null; then return 0; fi
    modprobe 9pnet_virtio 2>/dev/null || true
    if mount -t 9p -o trans=virtio,version=9p2000.L,msize=512000 \
             "$NVKVM_DATA_TAG" "$NVKVM_DATA_MNT" 2>/dev/null; then
        log "data: mounted tag '$NVKVM_DATA_TAG' at $NVKVM_DATA_MNT"
        return 0
    fi
    NVKVM_DATA_MNT=""
    log "data: optional 9p tag '$NVKVM_DATA_TAG' absent; boot continues"
    return 1
}

# The record of which commit the module was built from lives NEXT TO the .ko, so
# the two cannot drift apart: delete the module and the record goes with it.
ko_path()        { printf '%s' "/usr/lib/modules/$1/updates/${MODULE_NAME}.ko"; }
ko_commit_path() { printf '%s' "$(ko_path "$1").commit"; }
built_commit()   { cat "$(rp "$(ko_commit_path "$1")")" 2>/dev/null; }

# ── Packages: query, never record ────────────────────────────────────────────
# What we installed is derived by diffing the installed set before and after,
# not by writing a list to disk. That means cleanup works correctly even on a
# root this script has never seen before.
pkgs_snapshot() { in_target sh -c 'pacman -Qq 2>/dev/null' | sort; }
PKGS_BEFORE=""

remove_added_packages() {
    local after added
    after="$(pkgs_snapshot)"
    added="$(comm -13 <(printf '%s\n' "$PKGS_BEFORE") <(printf '%s\n' "$after") 2>/dev/null)"
    [ -n "${added//[[:space:]]/}" ] || { log "no packages to remove"; return 0; }
    log "removing packages this run added: $(printf '%s' "$added" | tr '\n' ' ')"
    steamos_unlock
    in_target sh -c "pacman -Rns --noconfirm $(printf '%s' "$added" | tr '\n' ' ')" >/dev/null 2>&1 \
        || warn "could not fully remove the packages this run added"
}

ensure_pacman_keyring() {
    in_target sh -c 'pacman-key --list-keys >/dev/null 2>&1' && return 0
    log "initialising pacman keyring in the target root"
    in_target pacman-key --init >/dev/null 2>&1 || true
    local kr
    for kr in archlinux holo; do
        in_target sh -c "ls /usr/share/pacman/keyrings/${kr}.gpg >/dev/null 2>&1" \
            && in_target pacman-key --populate "$kr" >/dev/null 2>&1
    done
    in_target sh -c 'pacman-key --list-keys >/dev/null 2>&1' \
        || { err "pacman keyring could not be initialised"; return 1; }
}

# Minimum only -- deliberately NOT base-devel, which is a meta-package whose
# ~500 MB is mostly irrelevant to an out-of-tree module build on a 5 GB rootfs.
# Anything else is added reactively from real build failures.
ensure_build_deps() {
    local missing=()
    in_target sh -c 'command -v gcc  >/dev/null 2>&1' || missing+=(gcc)
    in_target sh -c 'command -v make >/dev/null 2>&1' || missing+=(make)
    # SteamOS ships glibc at runtime but omits its development headers. gcc can
    # therefore build the kernel module while every userspace validation probe
    # fails at its first #include <stdio.h>. Reinstalling the already-owned
    # glibc package restores those headers; the package snapshot logic will not
    # remove glibc afterward because it was present before this run.
    [ -r "$(rp /usr/include/stdio.h)" ] || missing+=(glibc)
    [ ${#missing[@]} -eq 0 ] && { log "core build tools present"; return 0; }
    steamos_unlock; ensure_pacman_keyring || return 1
    log "installing core build tools: ${missing[*]}"
    # Do not use --needed: glibc may be installed while its headers are absent.
    in_target pacman -S --noconfirm "${missing[@]}" \
        || { err "could not install core build tools"; return 1; }
}

pkg_for_build_failure() {
    local f="$1"
    grep -qE "flex: (command )?not found|Cannot find flex"  "$f" && { echo flex;   return 0; }
    grep -qE "bison: (command )?not found|Cannot find bison" "$f" && { echo bison;  return 0; }
    grep -qE "\bbc: (command )?not found"                    "$f" && { echo bc;     return 0; }
    grep -qE "pahole: (command )?not found|BTF: .*pahole"    "$f" && { echo pahole; return 0; }
    grep -qE "libelf|elfutils|gelf\.h"                       "$f" && { echo libelf; return 0; }
    grep -qE "openssl/|libssl|opensslconf\.h"                "$f" && { echo openssl;return 0; }
    return 1
}

# ── Kernel + module ──────────────────────────────────────────────────────────
target_kver() {
    local d
    # The kernel the image BOOTS, identified by vmlinuz. Do NOT pick "the one with
    # a build tree": installing headers creates a SECOND module directory for
    # whatever point release the repo currently ships, which has a build tree but
    # no vmlinuz, and building against it yields a module that loads nowhere.
    for d in "$(rp /usr/lib/modules)"/*; do
        [ -d "$d" ] && [ -e "$d/vmlinuz" ] && { basename "$d"; return 0; }
    done
    for d in "$(rp /usr/lib/modules)"/*; do
        [ -d "$d" ] && [ -e "$d/modules.dep" ] && { basename "$d"; return 0; }
    done
    return 1
}
target_dbpath() {
    # SteamOS sets DBPath = /usr/lib/holo/pacmandb/ , not /var/lib/pacman.
    local d; d="$(sed -n 's/^ *DBPath *= *//p' "$(rp /etc/pacman.conf)" 2>/dev/null | head -1)"
    printf '%s' "${d:-/var/lib/pacman/}"
}
kernel_pkg_nv() {
    local kver="$1" dbp d b
    dbp="$(rp "$(target_dbpath)")/local"; [ -d "$dbp" ] || return 1
    for d in "$dbp"/*/; do
        b="$(basename "$d")"
        case "$b" in *-headers-*|*firmware*|*rtw*) continue ;; esac
        grep -qs "usr/lib/modules/${kver%%-neptune*}" "$d/files" 2>/dev/null || continue
        printf '%s %s' "$(printf '%s' "$b" | sed -E 's/-[^-]+-[^-]+$//')" \
                       "$(printf '%s' "$b" | sed -E 's/^.*-([^-]+-[^-]+)$/\1/')"
        return 0
    done
    return 1
}

ensure_kernel_headers() {
    local kver="$1"
    [ -e "$(rp "/usr/lib/modules/$kver/build")" ] && { log "headers present for $kver"; return 0; }
    steamos_unlock; ensure_pacman_keyring || return 1

    # NEVER `pacman -S`. That resolves through the repo DATABASE, which indexes only
    # the newest build: asking for the image's exact version gives "target not
    # found" while the mirror POOL still serves that file with HTTP 200. Name the
    # file and fetch it, the way upstream does.
    local nv name verrel repo mirror url
    nv="$(kernel_pkg_nv "$kver")" || { err "kernel package for $kver not found in $(target_dbpath)"; return 1; }
    name="${nv%% *}"; verrel="${nv##* }"
    repo="$(awk -F'[][]' '/^\[jupiter-/{print $2; exit}' "$(rp /etc/pacman.conf)" 2>/dev/null)"
    mirror="$(awk '/^Server/{print $3; exit}' "$(rp /etc/pacman.d/mirrorlist)" 2>/dev/null)"
    [ -n "$repo" ] && [ -n "$mirror" ] || { err "could not read repo/mirror from the image"; return 1; }
    url="${mirror/\$repo/$repo}"; url="${url/\$arch/x86_64}/${name}-headers-${verrel}-x86_64.pkg.tar.zst"

    log "exact-match headers: $url"
    if ! in_target curl -sfIL --max-time 60 "$url" -o /dev/null; then
        err "Exact-match headers not in Valve's pool for kernel $kver (${name}-${verrel})."
        err "SteamOS must update to a kernel whose headers are still pooled. This"
        err "script runs at every boot and will build the module automatically once"
        err "that happens -- no manual step is needed."
        return 1
    fi
    in_target curl -sfL --max-time 600 "$url" -o /tmp/nvkvm-headers.pkg.tar.zst \
        || { err "could not download the headers package"; return 1; }
    in_target pacman -U --noconfirm --needed /tmp/nvkvm-headers.pkg.tar.zst \
        || { err "pacman -U failed for the exact-match headers"; return 1; }
    rm -f "$(rp /tmp/nvkvm-headers.pkg.tar.zst)"
    [ -e "$(rp "/usr/lib/modules/$kver/build")" ] \
        || { err "headers installed but $kver still has no build tree"; return 1; }
}

# The build area must not be a plain directory on /home: /home carries the ext4
# `casefold` feature and a casefolded directory breaks kernel builds. An ext4
# loopback image on /home gives us the space without the hazard.
BUILD_IMG_REL=/home/.nvkvm-build.img
BUILD_MNT_REL=/home/.nvkvm-build
BUILD_IMG_MB=1024
build_area_up() {
    local img mnt; img="$(rp "$BUILD_IMG_REL")"; mnt="$(rp "$BUILD_MNT_REL")"
    mkdir -p "$mnt" || return 1
    mountpoint -q "$mnt" && return 0
    rm -f "$img"
    dd if=/dev/zero of="$img" bs=1M count=0 seek="$BUILD_IMG_MB" status=none 2>/dev/null || return 1
    mkfs.ext4 -q -F "$img" >/dev/null 2>&1 || { err "mkfs on the build image failed"; return 1; }
    mount -o loop "$img" "$mnt" || { err "could not mount the build image"; return 1; }
    log "build area ready: $BUILD_MNT_REL (ext4 loopback, casefold-free)"
}
build_area_down() {
    local img mnt; img="$(rp "$BUILD_IMG_REL")"; mnt="$(rp "$BUILD_MNT_REL")"
    mountpoint -q "$mnt" && umount "$mnt" 2>/dev/null
    rm -f "$img"; rmdir "$mnt" 2>/dev/null || true
}

build_and_install_module() {
    local kver kdir src
    kver="$(target_kver)" || { err "no kernel under $(rp /usr/lib/modules)"; return 1; }
    ensure_kernel_headers "$kver" || return 1
    kdir="/usr/lib/modules/$kver/build"
    src="$NVKVM_SHARE_MNT/src"
    [ -d "$src/guest" ] || { err "nvkvm guest source not found at $src/guest"; return 1; }

    log "building $MODULE_NAME against $kver (KDIR=$kdir)"
    steamos_unlock
    build_area_up || return 1
    local scratch_rel="$BUILD_MNT_REL" scratch; scratch="$(rp "$scratch_rel")"
    # Stage the whole src/ tree: the guest sources include siblings by relative
    # path ("../../src/common/..."), so a flat copy of src/guest does not compile.
    cp -a "$src" "$scratch/src" 2>/dev/null || { err "could not stage source"; build_area_down; return 1; }
    local build_rel="$scratch_rel/src/guest" logf; logf="$(rp "$scratch_rel/build.log")"

    local rc=1 need
    local t0; t0="$(date +%s)"
    for _ in 1 2 3 4; do
        if in_target sh -c "make -C '$kdir' M='$build_rel' modules" >"$logf" 2>&1; then rc=0; break; fi
        if need="$(pkg_for_build_failure "$logf")"; then
            log "build needs '$need' -- installing and retrying"
            in_target pacman -S --noconfirm --needed "$need" \
                || { err "could not install $need"; build_area_down; return 1; }
        else
            err "build failed against $kver, cause is not a known missing tool:"
            tail -20 "$logf" >&2; build_area_down; return 1
        fi
    done
    [ $rc -eq 0 ] || { err "build still failing after reactive installs"; build_area_down; return 1; }
    log "module compile took $(( $(date +%s) - t0 ))s"

    local vm
    vm="$(modinfo -F vermagic "$scratch/src/guest/${MODULE_NAME}.ko" 2>/dev/null | awk '{print $1}')"
    if [ -n "$vm" ] && [ "$vm" != "$kver" ]; then
        err "built vermagic ($vm) != image kernel ($kver) -- refusing to install"
        build_area_down; return 1
    fi

    mkdir -p "$(rp "$(dirname "$(ko_path "$kver")")")"
    cp -f "$scratch/src/guest/${MODULE_NAME}.ko" "$(rp "$(ko_path "$kver")")" \
        || { build_area_down; return 1; }
    repo_commit > "$(rp "$(ko_commit_path "$kver")")" 2>/dev/null
    in_target depmod "$kver" || warn "depmod failed for $kver"
    build_area_down
    MODULE_REBUILT=1
    log "module built and installed for $kver"
}

# ── NVIDIA userspace ─────────────────────────────────────────────────────────
host_driver_version() {
    # An explicit value is used by the disposable Alpine installer. Otherwise
    # read from the RUNNING system on purpose: this is the HOST driver as nvkvm
    # exposes it, and it is what an A/B update target must match too.
    if [ -n "$DRIVER_VERSION" ]; then
        printf '%s\n' "$DRIVER_VERSION"
        return 0
    fi
    awk '{for(i=1;i<=NF;i++) if ($i ~ /^[0-9]+\.[0-9]+/) {print $i; exit}}' \
        /proc/driver/nvidia/version 2>/dev/null
}

load_nvkvm_module() {
    [ "$MODULE_LOAD_REFRESHED" = 0 ] || return 0
    if [ "$MODULE_REBUILT" != 1 ] && grep -qw "$MODULE_MOD" "$PROC_MODULES" 2>/dev/null; then
        MODULE_LOAD_REFRESHED=1
        return 0
    fi
    if [ "$MODULE_REBUILT" = 1 ] && grep -qw "$MODULE_MOD" "$PROC_MODULES" 2>/dev/null; then
        log "module was rebuilt this run -- unloading the resident copy first"
        rmmod "$MODULE_NAME" 2>/dev/null \
            || warn "could not unload the running module; a reboot may be needed"
    fi
    if modprobe "$MODULE_NAME" 2>/dev/null; then
        MODULE_LOAD_REFRESHED=1
        return 0
    fi
    warn "modprobe $MODULE_NAME failed"
    return 1
}
installed_userspace_version() {
    in_target sh -c 'ls /usr/lib/libnvidia-glcore.so.* 2>/dev/null | head -1' \
        | sed 's/.*libnvidia-glcore\.so\.//'
}
run_cache() {
    # Offline provisioning runs before SteamOS creates the deck account and its
    # home. Caching below /home/deck here creates that directory as root; the
    # first-boot homedir unit then leaves it root-owned and gamescope cannot
    # create ~/.config. Keep installer-owned state at the top of /home instead.
    printf '%s' "${RUN_CACHE_DIR:-/home/.nvkvm-nvidia-runs}"
}

locate_or_fetch_run() {
    local ver="$1"
    local fname="NVIDIA-Linux-x86_64-${ver}.run"
    local candidate cached
    for candidate in "$NVKVM_SHARE_MNT/nvidia/$fname" "$NVKVM_SHARE_MNT/$fname"; do
        [ -r "$candidate" ] && { log "using .run from the 9p share"; printf '%s' "$candidate"; return 0; }
    done
    mkdir -p "$(rp "$(run_cache)")" 2>/dev/null
    cached="$(rp "$(run_cache)/$fname")"
    if [ -r "$cached" ] && sh "$cached" --check >/dev/null 2>&1; then
        log "using cached .run"; printf '%s' "$cached"; return 0
    fi
    log "downloading NVIDIA $ver"
    if in_target curl -fL --no-progress-meter --retry 3 \
         -o "$(run_cache)/$fname" \
         "https://us.download.nvidia.com/XFree86/Linux-x86_64/${ver}/${fname}"; then
        if sh "$cached" --check >/dev/null 2>&1; then
            prune_run_cache; printf '%s' "$cached"; return 0
        fi
        err ".run failed its own integrity check"; rm -f "$cached"
    fi
    return 1
}
prune_run_cache() {
    local dir; dir="$(rp "$(run_cache)")"
    ls -1t "$dir"/NVIDIA-Linux-x86_64-*.run 2>/dev/null | tail -n +$((RUN_CACHE_KEEP+1)) \
        | while read -r f; do log "pruning old .run: $f"; rm -f "$f"; done
}

trim_cuda_files() {
    # Deterministic, profile-driven. Never triggered by free space.
    log "profile '$PROFILE': trimming CUDA/OpenCL/NVVM/OptiX (GPU PhysX is the casualty;"
    log "Vulkan, GL, DXVK, VKD3D, gamescope and DLSS/libnvidia-ngx are unaffected)"
    local d f n=0
    for d in /usr/lib /usr/lib32 /usr/bin /usr/share/vulkan/icd.d /etc/OpenCL; do
        [ -d "$(rp "$d")" ] || continue
        while IFS= read -r f; do
            case "$(basename "$f")" in libnvidia-ngx*) continue ;; esac
            rm -rf "$f" && n=$((n+1))
        done < <(find "$(rp "$d")" -maxdepth 2 \( -type f -o -type l -o -type d \) \
                   | grep -E "$TRIM_RE" 2>/dev/null)
    done
    log "trimmed $n CUDA/OpenCL paths"
}

drop_gsp_firmware() {
    # VERIFIED unused, not assumed: the guest has no nvidia.ko at all, and
    # `modinfo -F firmware nvkvm-guest.ko` is empty -- nothing in the guest can
    # request GSP firmware. nvkvm supplies the kernel side from the host.
    local d; d="$(rp /usr/lib/firmware/nvidia)"
    [ -d "$d" ] || return 0
    log "removing GSP firmware ($(du -sh "$d" 2>/dev/null | cut -f1)) -- no guest module requests it"
    rm -rf "$d"
}

# Re-install the files that a diverted TYPE contains but the trim LIST keeps.
# Driven by the .run's own .manifest so it stays correct across driver versions.
restore_diverted_keepers() {
    local payload="$1"
    [ -r "$payload/.manifest" ] || { warn "no .manifest in payload; cannot restore keepers"; return 0; }
    local n=0 f base dest
    while IFS= read -r f; do
        base="$(basename "$f")"
        # keep exactly what upstream's tested trim regex does NOT match
        printf '%s' "$base" | grep -qE "$TRIM_RE" && continue
        case "$f" in
            ./32/*) dest=/usr/lib32 ;;
            *)      dest=/usr/lib ;;
        esac
        [ -d "$(rp "$dest")" ] || continue
        if cp -f "$payload/$f" "$(rp "$dest/$base")" 2>/dev/null; then
            n=$((n+1))
        fi
    done < <(awk '$3=="CUDA_LIB" || $3=="OPENCL_LIB" {print $1}' "$payload/.manifest")
    log "restored $n library file(s) that the diverted types contain but the trim list keeps"
    # ldconfig recreates the SONAME symlinks (e.g. libnvidia-ml.so.1), which is
    # what nvidia-smi actually dlopens -- the bare .so dev symlink is not needed.
    in_target ldconfig
}

install_nvidia_userspace() {
    local run="$1"
    steamos_unlock
    local scratch
    if [ "$ROOT" = "/" ]; then scratch=/tmp/nvkvm-nvidia-extract
    else scratch="$(dirname "$(readlink -f "$ROOT")")/nvkvm-nvidia-extract"; fi
    rm -rf "$scratch"; mkdir -p "$scratch" || { err "could not create $scratch"; return 1; }

    # `-x` unpacks into ./NVIDIA-Linux-x86_64-<ver>/ in CWD. Do NOT pass --target:
    # the archive forwards it to nvidia-installer and the extraction silently fails.
    local runabs; runabs="$(readlink -f "$run")"
    ( cd "$scratch" && sh "$runabs" -x ) >/dev/null 2>&1
    local payload; payload="$(find "$scratch" -maxdepth 1 -type d -name 'NVIDIA-Linux-x86_64-*' | head -1)"
    [ -n "$payload" ] || { err "no payload directory after extraction"; rm -rf "$scratch"; return 1; }

    # Divert the bulk we do not want so it is never WRITTEN to the rootfs, rather
    # than installing it and deleting it afterwards. Deleting afterwards still
    # needs the peak space, which is what actually fails on a 5 GB rootfs.
    # --override-file-type-destination is keyed on the installer's own .manifest
    # file types and its documentation says it takes precedence over everything else.
    local divert; divert="$(rp /tmp/nvkvm-discard)"
    rm -rf "$divert"; mkdir -p "$divert"
    local ovr=""
    # FIRMWARE: VERIFIED unused -- the guest has no nvidia.ko at all and
    # `modinfo -F firmware nvkvm-guest.ko` is empty, so nothing can request GSP
    # firmware. ~100 MB, diverted on every profile.
    ovr="--override-file-type-destination=FIRMWARE:/tmp/nvkvm-discard"
    if [ "$PROFILE" = steamos ]; then
        # ~520 MB of CUDA/OpenCL that the gaming profile does not use.
        ovr="$ovr --override-file-type-destination=CUDA_LIB:/tmp/nvkvm-discard"
        ovr="$ovr --override-file-type-destination=OPENCL_LIB:/tmp/nvkvm-discard"
    fi

    local need_kb=1310720
    [ "$PROFILE" = steamos ] && need_kb=786432
    [ "${NVKVM_NO_COMPAT32:-0}" = "1" ] && need_kb=$((need_kb/2))
    local free_kb; free_kb="$(df -Pk "$(rp /usr)" 2>/dev/null | awk 'NR==2{print $4}')"
    if [ -n "$free_kb" ] && [ "$free_kb" -lt "$need_kb" ]; then
        err "not enough space for the NVIDIA userspace: free=$((free_kb/1024))M needed=$((need_kb/1024))M"
        err "Pick a smaller profile (--profile steamos trims CUDA/OpenCL), set"
        err "NVKVM_NO_COMPAT32=1, or grow the rootfs. The profile is NOT downgraded"
        err "automatically -- that would make two identical machines differ."
        rm -rf "$scratch"; return 1
    fi

    local inner=/tmp/nvidia-install
    mkdir -p "$(rp "$inner")"
    mount --bind "$payload" "$(rp "$inner")" \
        || { err "could not bind-mount the payload"; rm -rf "$scratch"; return 1; }

    log "running nvidia-installer (userspace only, profile=$PROFILE)"
    local extra=""
    [ "${NVKVM_NO_COMPAT32:-0}" = "1" ] && extra="--no-install-compat32-libs"
    local rc=0
    # Never hand-pick library files: a hand-picked list silently under-installs
    # libnvidia-glsi/tls/glcore/gpucomp and yields a Vulkan loader that enumerates
    # zero devices with no error until you strace it.
    in_target "$inner/nvidia-installer" \
        --silent --no-kernel-modules --no-kernel-module-source \
        --no-nouveau-check --no-x-check --no-rpms $extra $ovr \
        || { err "nvidia-installer failed"; rc=1; }
    if [ $rc -eq 0 ]; then
        # CRITICAL, and this MUST happen while the payload still exists: the
        # manifest's type granularity is COARSER than upstream's filename trim
        # list. `CUDA_LIB` contains libnvidia-ml (NVML), libnvidia-ptxjitcompiler
        # and libnvidia-sandboxutils -- none in the trim regex, all required.
        # MEASURED: diverting the whole CUDA_LIB type removed NVML and nvidia-smi
        # then failed with "NVIDIA-SMI couldn't find libnvidia-ml.so library in
        # your system" (exit 12), while Vulkan and gamescope kept working because
        # neither uses NVML. Divert the type for the space win, then restore the
        # keepers from the extracted payload, which is authoritative for content.
        restore_diverted_keepers "$payload"
    fi

    umount "$(rp "$inner")" 2>/dev/null
    rm -rf "$scratch" "$divert"
    [ $rc -eq 0 ] || return 1

    drop_gsp_firmware
    [ "$PROFILE" = steamos ] && trim_cuda_files

    in_target ldconfig
    local lib
    for lib in libnvidia-glsi libnvidia-tls libnvidia-glcore; do
        in_target sh -c "ls /usr/lib/${lib}.so.* >/dev/null 2>&1" \
            || { err "$lib missing after install"; rc=1; }
    done
    return $rc
}

# ── Optional SSH access, gated by the presence of a key file ─────────────────
# The OPERATOR drops authorized_keys into the separate `data` 9p share on the
# HOST. It is mounted at the interactive user's ~/data; its presence is the
# switch, and the private key never crosses into the guest.
# that file's presence IS the switch. No flag, no default to pick:
#   absent  -> sshd is not started at all
#   present -> the key is installed and sshd is enabled
# There is deliberately no state in which sshd runs with credentials nobody chose.
#
# PasswordAuthentication is forced OFF. Do NOT rely on "the deck user has no
# password": SteamOS actively prompts users to set one when enabling developer
# mode or sudo, which is exactly this audience -- the moment that password exists,
# password auth over ssh would go live and the safety property would silently
# stop holding. Turning it off makes the guarantee independent of that.
#
# Reachability: the guest is behind slirp NAT, so it is reachable ONLY once the
# operator also adds a QEMU hostfwd, e.g.
#   -netdev user,id=net0,hostfwd=tcp::15022-:22
configure_ssh() {
    local keysrc="${NVKVM_DATA_MNT:-/nonexistent}/authorized_keys"
    local rootkeysrc="${NVKVM_DATA_MNT:-/nonexistent}/root_authorized_keys"

    if [ ! -r "$keysrc" ] && [ ! -r "$rootkeysrc" ]; then
        log "ssh: no authorized_keys on the optional data share -- sshd left disabled"
        in_target systemctl disable sshd.service 2>/dev/null || true
        rm -f "$(rp /etc/sudoers.d/10-nvkvm-ssh)" \
              "$(rp /etc/sudoers.d/zz-nvkvm-ssh)"
        return 0
    fi

    steamos_unlock
    if ! in_target sh -c 'command -v sshd >/dev/null 2>&1'; then
        log "ssh: installing openssh"
        ensure_pacman_keyring || return 1
        in_target pacman -S --noconfirm --needed openssh \
            || { err "ssh: could not install openssh"; return 1; }
    else
        log "ssh: openssh already present in the image"
    fi

    # Host keys, generated once and only if missing, so the guest does not present
    # a different host key on every boot (which would warn on every connection).
    if ! in_target sh -c 'ls /etc/ssh/ssh_host_*_key >/dev/null 2>&1'; then
        log "ssh: generating host keys"
        in_target ssh-keygen -A >/dev/null 2>&1 || warn "ssh: ssh-keygen -A failed"
    else
        log "ssh: host keys already present, keeping them"
    fi

    mkdir -p "$(rp /etc/ssh/sshd_config.d)"
    cat > "$(rp /etc/ssh/sshd_config.d/10-nvkvm.conf)" <<'SSHCONF'
# Written by nvkvm's steamos_boot.sh.
# Key-based access only. PasswordAuthentication defaults to YES, and SteamOS
# prompts users to set a deck password (developer mode / sudo), so leaving it at
# the default would silently open password login the moment that happens.
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin prohibit-password
SSHCONF

    _install_authkeys() {   # <user> <source file>
        local u="$1" src="$2" home uid gid
        [ -r "$src" ] || return 0
        home="$(awk -F: -v u="$u" '$1==u{print $6}' "$(rp /etc/passwd)")"
        uid="$(awk -F: -v u="$u" '$1==u{print $3}' "$(rp /etc/passwd)")"
        gid="$(awk -F: -v u="$u" '$1==u{print $4}' "$(rp /etc/passwd)")"
        [ -n "$home" ] && [ -n "$uid" ] || { warn "ssh: no such user '$u'"; return 0; }
        local h; h="$(rp "$home")"
        [ -d "$h" ] || { warn "ssh: home '$home' for $u does not exist"; return 0; }
        mkdir -p "$h/.ssh"
        cp -f "$src" "$h/.ssh/authorized_keys"
        # sshd SILENTLY ignores keys with loose permissions -- a running sshd that
        # rejects a perfectly good key, with nothing useful client-side. Set these
        # explicitly; files copied off a 9p mount will not have them by default.
        chmod 700 "$h/.ssh"
        chmod 600 "$h/.ssh/authorized_keys"
        chown -R "$uid:$gid" "$h/.ssh"
        chmod go-w "$h"          # sshd also refuses a group/other-writable home
        log "ssh: installed authorized_keys for '$u' ($(wc -l < "$src") key line(s))"
    }

    # Interactive user, not root. SteamOS uses `deck`; fall back to `user`.
    local iu=""
    for cand in deck user; do
        awk -F: -v u="$cand" '$1==u{f=1} END{exit !f}' "$(rp /etc/passwd)" && { iu="$cand"; break; }
    done
    [ -n "$iu" ] && _install_authkeys "$iu" "$keysrc"

    # The container helper logs in as the interactive account. Give that
    # key-authenticated account non-interactive administrative access so host
    # automation can inspect kernel logs and repair the guest without planting
    # a password. Keep this coupled to the interactive user's key: a root-only
    # key does not silently change the deck/user sudo policy.
    if [ -n "$iu" ] && [ -r "$keysrc" ]; then
        if ! in_target sh -c 'command -v sudo >/dev/null 2>&1 && command -v visudo >/dev/null 2>&1'; then
            log "ssh: installing sudo for passwordless guest administration"
            ensure_pacman_keyring || return 1
            in_target pacman -S --noconfirm --needed sudo \
                || { err "ssh: could not install sudo"; return 1; }
        fi
        mkdir -p "$(rp /etc/sudoers.d)"
        rm -f "$(rp /etc/sudoers.d/10-nvkvm-ssh)"
        printf '%s ALL=(ALL:ALL) NOPASSWD: ALL\n' "$iu" \
            > "$(rp /etc/sudoers.d/zz-nvkvm-ssh)"
        chmod 0440 "$(rp /etc/sudoers.d/zz-nvkvm-ssh)"
        if ! in_target visudo -cf /etc/sudoers.d/zz-nvkvm-ssh >/dev/null; then
            rm -f "$(rp /etc/sudoers.d/zz-nvkvm-ssh)"
            err "ssh: generated sudoers policy failed validation"
            return 1
        fi
        log "ssh: '$iu' has key-gated passwordless sudo"
    else
        rm -f "$(rp /etc/sudoers.d/10-nvkvm-ssh)" \
              "$(rp /etc/sudoers.d/zz-nvkvm-ssh)"
    fi

    # Root access is a separate, explicit decision via a differently-named file.
    _install_authkeys root "$rootkeysrc"

    in_target systemctl enable sshd.service 2>/dev/null || warn "ssh: could not enable sshd.service"
    log "ssh: enabled. Reachable only once QEMU also has a hostfwd, e.g."
    log "ssh:   -netdev user,id=net0,hostfwd=tcp::15022-:22"
}

# ── Desktop config + in-image stub ───────────────────────────────────────────
write_sddm_session_config() {
    # The SteamOS recovery image may select `gamescope-wayland.desktop` as the
    # SDDM autologin session. That is not this deployment's desktop model:
    # Plasma/KWin owns the desktop and Steam launches gamescope for games.
    #
    # This is also operationally load-bearing on a VM. MEASURED on SteamOS
    # 3.8.14: gamescope-session.service is Type=notify with TimeoutStartSec=5.
    # Its nvkvm/Vulkan startup missed that deadline, systemd SIGKILLed gamescope
    # and both Xwaylands, and SDDM autologin retried every ~16 seconds. Each
    # retry created fresh NVIDIA RM clients; after enough forced teardown cycles
    # the host's 256 MiB BAR1 VA was exhausted and even bare-metal vkCreateDevice
    # failed with NV_ERR_NO_MEMORY until the NVIDIA driver was reset. Selecting
    # the intended Plasma session prevents that destructive retry loop.
    #
    # `zz-` deliberately wins over both the image's /usr/lib configuration and
    # /etc/sddm.conf.d/steamos.conf. Set only Session: SteamOS remains the owner
    # of the autologin user and relogin policy.
    local sddm_override
    sddm_override="$(rp /etc/sddm.conf.d/zz-nvkvm-plasma.conf)"
    if [ "$PROFILE" = steamos ] \
       && [ -e "$(rp /usr/share/wayland-sessions/plasma.desktop)" ]; then
        mkdir -p "$(dirname "$sddm_override")"
        cat > "$sddm_override" <<'SDDM'
[Autologin]
Session=plasma.desktop
SDDM
        log "desktop session: Plasma/KWin (gamescope remains available to Steam)"
    else
        rm -f "$sddm_override"
        [ "$PROFILE" != steamos ] \
            || warn "Plasma Wayland session is absent; leaving SteamOS's SDDM session unchanged"
    fi
}

write_desktop_config() {
    steamos_unlock
    mkdir -p "$(rp /etc/modules-load.d)" "$(rp /etc/modprobe.d)" "$(rp /etc/systemd/system)" "$(rp /etc/environment.d)"
    echo "$MODULE_NAME" > "$(rp /etc/modules-load.d/nvkvm.conf)"
    printf 'options nvkvm_guest privileged_modeset=0\n' > "$(rp /etc/modprobe.d/99-nvkvm.conf)"
    # Resolve the nvkvm DRM node by DRIVER NAME, never by index. environment.d is
    # the mechanism that actually reaches kwin_wayland (pam_systemd imports it at
    # login, before plasma-kwin_wayland.service starts); it cannot run a shell, so
    # a boot-time generator writes the value.
    cat > "$(rp /etc/systemd/system/nvkvm-drm-env.service)" <<'UNIT'
[Unit]
Description=Resolve the nvkvm DRM node and publish it to user sessions
DefaultDependencies=no
# MUST run after the module is loaded. MEASURED: ordering this only after
# udev-settle made it run BEFORE nvkvm-guest was loaded, so no card had driver
# "nvidia" yet, the loop matched nothing, and it exited 0 having written no file.
# The unit then showed "active" while /etc/environment.d was empty and KWin
# opened BOTH DRM nodes -- which is the EGL-init failure, presenting as success.
After=systemd-udev-settle.service nvkvm-boot.service systemd-modules-load.service
Wants=nvkvm-boot.service
Before=display-manager.service sddm.service

[Service]
Type=oneshot
RemainAfterExit=yes
# Poll briefly: modprobe and the DRM node's appearance are not instantaneous.
ExecStart=/bin/sh -c 'for i in $(seq 1 30); do for c in /sys/class/drm/card[0-9]*; do [ -e "$c/device/driver" ] || continue; case "$(basename "$(readlink -f "$c/device/driver")")" in nvidia) mkdir -p /etc/environment.d; echo "KWIN_DRM_DEVICES=/dev/dri/$(basename "$c")" > /etc/environment.d/90-nvkvm.conf; echo "nvkvm: selected /dev/dri/$(basename "$c")"; exit 0;; esac; done; sleep 1; done; echo "nvkvm: WARNING no DRM node with driver nvidia after 30s" >&2; exit 0'

[Install]
WantedBy=multi-user.target
UNIT
    # Cursor latency. MEASURED on SteamOS/Plasma (KWin Wayland) on a real RTX 4070:
    # pointer motion produced 621 GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT errors per 15s
    # (idle: 0), and the present rate did NOT rise -- so cursor updates were failing in
    # GL and never reaching the screen. That is the "mouse lags, keyboard is instant"
    # report, and it is Wayland-compositor-specific (the Mint/X11 guest never shows it).
    #
    # Cause is in nvkvm, not here: nvkvm's virtual KMS is a drm_simple_display_pipe, so
    # it exposes ONE primary plane and NO cursor plane (GETPLANERESOURCES returns 0).
    # With no cursor plane KWin drives the cursor through a GL path, and nvkvm_kms.c's
    # own comment records why that path cannot work -- "NVIDIA cannot use a LINEAR
    # dma-buf as an EGLImage render target" -- while the accept callback still lets
    # LINEAR through for cursors. Forcing KWin's software cursor keeps the cursor on the
    # normal, working block-linear scene path. A/B on the same guest, same session,
    # variable confirmed in kwin_wayland's /proc/<pid>/environ: 621 -> 0.
    #
    # This is a WORKAROUND for a real nvkvm KMS gap; the durable fix is a cursor plane.
    printf 'KWIN_FORCE_SW_CURSOR=1\n' > "$(rp /etc/environment.d/91-nvkvm-cursor.conf)"

    # do_install() is shared by live boot, fresh `--install-only --root`, and
    # the A/B update hook. Keeping this here makes every convergence reassert
    # Plasma instead of relying on a one-off edit to the current image.
    write_sddm_session_config

    in_target systemctl enable nvkvm-drm-env.service 2>/dev/null || true
    log "desktop configuration written"
}

# Copy ONE file from the share into the image, then PROVE it landed intact.
#
# install(1) and cp(1) create the destination with O_CREAT|O_TRUNC before they
# have written a single byte, so ANY failure after that point leaves a
# destination that exists, has the right name, and is EMPTY. MEASURED, on a
# deliberately-full ext2 image with /usr/local/sbin already present:
#
#   $ install -m 0755 src dst
#   install: cannot install 'src' to 'dst': No space left on device
#   $ ls -l dst
#   -rw------- 1 root root 0 ... dst
#
# and the SteamOS rootfs is the ~5 GB partition this whole script fights for
# room on. The same shape comes out of a short/failed read off the 9p share, and
# out of a source file on the share that is itself zero-length.
#
# A 0-byte /usr/local/sbin/nvkvm-recovery.sh is the worst outcome this script
# can produce: execve() on an empty file returns ENOEXEC, so nvkvm-boot.service
# fails 203/EXEC on EVERY boot; every `[ -e ]`/`ls` check says it is installed;
# and nothing anywhere says why. So verify against the source, and delete
# whatever does not match rather than leaving the remains in place -- an absent
# file is at least honest, and the next converge run re-plants it.
plant_file() {   # <src> <dst> <mode>
    local src="$1" dst="$2" mode="$3" ssz dsz
    [ -r "$src" ] || { err "plant: source not readable: $src"; return 1; }
    if [ ! -s "$src" ]; then
        err "plant: source is ZERO-LENGTH: $src"
        err "plant: the 9p share is broken; refusing to plant an empty $(basename "$dst")."
        return 1
    fi
    mkdir -p "$(dirname "$dst")" || { err "plant: could not create $(dirname "$dst")"; return 1; }
    if ! install -m "$mode" "$src" "$dst"; then
        err "plant: install failed for $dst -- removing the truncated remains."
        err "plant: usual cause is no room left on the target rootfs."
        rm -f "$dst"
        return 1
    fi
    # cmp, not just a size test: a short read off 9p can leave a file that is
    # non-empty and still wrong.
    if ! cmp -s "$src" "$dst"; then
        ssz="$(wc -c <"$src" 2>/dev/null)"; dsz="$(wc -c <"$dst" 2>/dev/null)"
        err "plant: $dst does not match its source (${dsz:-?} bytes planted, ${ssz:-?} expected) -- removing it."
        rm -f "$dst"
        return 1
    fi
    return 0
}

# ── Validation probes ────────────────────────────────────────────────────────
# nvkvm-pv's tests/validate.sh runs 30 checks and 24 of them are C probes it
# compiles at run time. This image has no compiler: remove_added_packages()
# strips gcc and make immediately below, because the toolchain and the NVIDIA
# userspace do not both fit on a 5 GB rootfs. MEASURED on the produced image --
# "30 total: 5 PASS, 1 FAIL, 24 SKIP", every skip "no C compiler on PATH". On
# the shipped artifact the validator could only ever check bring-up; it could
# never test CUDA, Vulkan, EGL or GL, and the result read as basically fine.
#
# So build the probes here, while gcc is still installed, and ship the binaries.
# They dlopen() libcuda/libvulkan/libEGL and link only -ldl and -lm, so at run
# time they need nothing but libc -- and building them IN the target root means
# that is the IMAGE's libc, which a host-side build could not promise.
#
# The placement is the point and is deliberately builder-agnostic: do_install()
# is what every path into an image runs -- the offline image builder,
# `nvkvm-recovery.sh plant` into a freshly staged slot, a live guest converging
# at boot, and whatever builder replaces the current one. Nothing here depends
# on any one builder's structure.
#
# Non-fatal by design. Without the probes validate.sh reports CANNOT VALIDATE
# rather than a false pass -- a bad outcome, but not a broken image, and not
# worth failing a provisioning run over.
NVKVM_PROBE_DIR_IN_IMAGE=/usr/local/lib/nvkvm/probes
build_validation_probes() {
    local src="$NVKVM_SHARE_MNT/tests/validate.sh" probe
    if [ ! -r "$src" ]; then
        warn "no tests/validate.sh on the share -- the image will ship with no validation"
        warn "probes, and validate.sh on it will report CANNOT VALIDATE."
        return 0
    fi
    for probe in cuda_probe vk_probe gl_probe; do
        [ -x "$(rp "$NVKVM_PROBE_DIR_IN_IMAGE/$probe")" ] || break
    done
    if [ "$probe" = gl_probe ] && [ -x "$(rp "$NVKVM_PROBE_DIR_IN_IMAGE/$probe")" ]; then
        log "validation probes already present in $NVKVM_PROBE_DIR_IN_IMAGE"
        return 0
    fi
    if ! in_target sh -c 'command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1'; then
        warn "no compiler in the target root; shipping without validation probes"
        return 0
    fi
    steamos_unlock
    # The share is NOT visible inside the chroot on the offline path (the bind
    # lives at the HOST's /run/nvkvm, and chroot resolves that path inside the
    # image). Stage the script in first, exactly as build_and_install_module()
    # stages the module sources, then run it from a path the chroot can see.
    local staged=/tmp/nvkvm-validate.sh
    mkdir -p "$(rp /tmp)" "$(rp "$NVKVM_PROBE_DIR_IN_IMAGE")"
    if ! cp -f "$src" "$(rp "$staged")"; then
        warn "could not stage validate.sh into the target root; no probes built"
        return 0
    fi
    chmod 0755 "$(rp "$staged")"
    if in_target bash "$staged" --build-probes "$NVKVM_PROBE_DIR_IN_IMAGE"; then
        log "validation probes built into $NVKVM_PROBE_DIR_IN_IMAGE (validate.sh needs no compiler on this image)"
    else
        warn "could not build the validation probes. validate.sh on this image will report"
        warn "CANNOT VALIDATE instead of testing CUDA/Vulkan/GL. Needs an nvkvm-pv whose"
        warn "tests/validate.sh understands --build-probes."
    fi
    rm -f "$(rp "$staged")"
}

install_stub() {
    local img_dir="$NVKVM_SHARE_MNT/boot/image"
    [ -d "$img_dir" ] || { err "in-image sources not found at $img_dir"; return 1; }
    steamos_unlock
    local rc=0
    plant_file "$img_dir/nvkvm-recovery.sh"       "$(rp /usr/local/sbin/nvkvm-recovery.sh)"             0755 || rc=1
    plant_file "$img_dir/nvkvm-boot.service"      "$(rp /etc/systemd/system/nvkvm-boot.service)"        0644 || rc=1
    plant_file "$img_dir/nvkvm-plant-stub.path"   "$(rp /etc/systemd/system/nvkvm-plant-stub.path)"     0644 || rc=1
    plant_file "$img_dir/nvkvm-plant-stub.service" "$(rp /etc/systemd/system/nvkvm-plant-stub.service)" 0644 || rc=1
    if [ "$rc" != 0 ]; then
        err "in-image stub NOT installed. Until this is fixed nvkvm-boot.service"
        err "cannot run, and a provisioned image will not converge on its own."
        return 1
    fi
    in_target systemctl enable nvkvm-boot.service nvkvm-plant-stub.path 2>/dev/null || true
    log "in-image stub + recovery installed (verified byte-for-byte against the share)"
}

# ── Part 1 ───────────────────────────────────────────────────────────────────
do_install() {
    local rc=0
    log "=== Part 1: converge (ROOT=$ROOT, profile=$PROFILE) ==="
    capture_ro_state
    trap restore_ro_state EXIT

    PKGS_BEFORE="$(pkgs_snapshot)"

    if ! mount_9p_share; then
        err "9p share '$NVKVM_SHARE_TAG' could not be mounted at $NVKVM_SHARE_MNT."
        err "Start QEMU with: -virtfs local,path=<nvkvm-pv>,mount_tag=$NVKVM_SHARE_TAG,security_model=none,readonly=on"
        return 1
    fi
    # Optional by design. A QEMU invocation without mount_tag=data must reach
    # the desktop; it merely has no ~/data and no key-triggered SSH access.
    mount_data_share || true
    local kver want have
    kver="$(target_kver)"
    want="$(repo_commit)"; have="$(built_commit "${kver:-none}")"
    if [ -z "$have" ] || [ "$have" != "$want" ]; then
        log "module out of date (built=${have:-none} repo=$want) -- rebuilding"
        # Toolchain is fetched ONLY when a build is actually needed. MEASURED: run
        # unconditionally it installs gcc+make (55 MB down / 215 MB on a rootfs with
        # ~500 MB free) on EVERY boot and then removes them again, and on a root with
        # no resolver it fails and pins Part 1's exit status at 1 forever -- so a
        # perfectly converged system permanently reports failure.
        ensure_build_deps || rc=1
        build_and_install_module || rc=1
    else
        log "module up to date at $want"
    fi

    # LAST CHANCE TO USE A COMPILER. Everything below this line, and everything
    # on the finished image, runs without one.
    build_validation_probes

    # Free the toolchain before the driver install -- they do not both fit.
    remove_added_packages

    # On a normal SteamOS boot there is no reason to pass a version: build/load
    # nvkvm first and let its procfs bridge report the host value. Install-only
    # cannot touch running-kernel state and was preflighted below instead.
    if [ "$CMD" = boot ] && [ "$ROOT" = / ] && [ -z "$(host_driver_version)" ]; then
        log "host driver version not visible yet -- loading nvkvm before NVIDIA userspace convergence"
        load_nvkvm_module || rc=1
    fi

    local hv iv
    hv="$(host_driver_version)"; iv="$(installed_userspace_version)"
    if [ -z "$hv" ]; then
        err "could not determine the host NVIDIA driver version"
        rc=1
    elif [ "$hv" != "$iv" ]; then
        log "NVIDIA userspace mismatch (host=$hv guest=${iv:-none}) -- installing"
        local run
        if run="$(locate_or_fetch_run "$hv")"; then
            install_nvidia_userspace "$run" || rc=1
        else
            err "could not obtain the NVIDIA .run for $hv"; rc=1
        fi
    else
        log "NVIDIA userspace matches host ($hv)"
    fi

    # Every converge, not just after an install. MEASURED on a real SteamOS A/B
    # install: the offline chroot's ldconfig did not persist, so /etc/ld.so.cache
    # still had no nvidia entries on first boot. Everything WORKED anyway (the libs
    # sit in /usr/lib, a default search dir, and the SONAME symlinks resolve), but
    # validate()'s `ldconfig -p | grep libGLX_nvidia` then reported CLASS 4 and
    # printed "The desktop will start on the emulated VGA" while the desktop was in
    # fact running on nvkvm. A green system reporting a hard failure is as bad as
    # the reverse, and this is the cheap, idempotent fix for it.
    in_target ldconfig 2>/dev/null || warn "ldconfig failed in the target root"

    write_desktop_config || rc=1
    configure_ssh || rc=1
    # NOT `|| true`. A silently-failed plant is how a 0-byte
    # /usr/local/sbin/nvkvm-recovery.sh ships: 203/EXEC on every boot, from a run
    # that reported rc=0. This does not endanger the update path -- the plant
    # unit declares SuccessExitStatus=0 1 and nvkvm-recovery.sh's own `plant`
    # subcommand still always exits 0 (design note 3).
    install_stub || rc=1
    prune_run_cache
    log "=== Part 1 finished (rc=$rc) ==="
    return $rc
}

# ── Validation ───────────────────────────────────────────────────────────────
# WHAT THIS FUNCTION CAN SEE, AND WHAT IT CANNOT. Read this before adding a
# caller that prints anything cheerful on the strength of a 0 from here.
#
# IT CHECKS
#   - the kernel log carries no nvkvm protocol-version mismatch      (CLASS 2)
#   - the guest module is loaded                                     (CLASS 3)
#   - the kernel has not faulted inside the guest module             (CLASS 5)
#   - /dev/nvidiactl exists AND can actually be OPENED               (CLASS 5)
#   - the NVIDIA userspace the chosen profile needs is installed     (CLASS 4)
#   - a DRM node bound to the nvidia driver exists (graphics only)   (CLASS 6)
#
# IT DOES NOT CHECK THE DESKTOP, AND IT STRUCTURALLY CANNOT.
# This runs from nvkvm-boot.service, which is ordered
# `Before=display-manager.service sddm.service`. At the moment it returns, no
# compositor has been started. It cannot know whether one comes up, whether it
# crash-loops, or whether the session lands on nvkvm's DRM node or the emulated
# VGA. MEASURED, twice in one week: this printed
# `validation OK -- handing over to the desktop` while kwin was crash-looping
# and no desktop existed, and on another boot while the guest module was
# oopsing on every open. Both times the message stopped people looking.
#
# So the rule for callers: report what was verified, name what was not, and
# never let the word "desktop" appear next to a 0 from this function.
#
# Return: 0 ok | 2 protocol mismatch | 3 module will not load
#         4 userspace broken | 5 module loaded but not usable
#         6 no DRM node bound to the nvidia driver
#
# VALIDATE_UNVERIFIED collects things this run could not observe at all (a
# restricted kernel log, say). It is NOT a failure — it is the part of the
# verdict that has no evidence behind it, and callers print it so a pass is
# never mistaken for a complete pass.
VALIDATE_UNVERIFIED=""

# The one operation the desktop is about to perform. `[ -e ]` proves a device
# node exists; it proves nothing about whether opening it works, and "the node
# is there" is exactly what was true on the boot where every open oopsed.
# Runs in a subshell so a failed redirection cannot take this script with it.
can_open_device() {   # <path>
    ( exec 9<"$1" ) 2>/dev/null
}

validate() {
    VALIDATE_UNVERIFIED=""
    local dmesgtxt dmesg_ok=1
    dmesgtxt="$(dmesg 2>/dev/null)" || dmesg_ok=0
    [ -n "$dmesgtxt" ] || dmesg_ok=0
    if [ "$dmesg_ok" = 0 ]; then
        # Silently finding nothing in a log you cannot read is indistinguishable
        # from finding nothing in a clean one. Say which it was.
        VALIDATE_UNVERIFIED="$VALIDATE_UNVERIFIED
  - the kernel log is unreadable (kernel.dmesg_restrict?), so the protocol-mismatch
    and module-fault checks below did not actually run"
    fi

    # Deliberately searches the WHOLE log, not `tail -400`: on a boot that logs
    # a lot the mismatch line scrolls out of a 400-line window and the check
    # silently passes.
    if printf '%s\n' "$dmesgtxt" | grep -q 'protocol version mismatch'; then
        err "CLASS 2: nvkvm PROTOCOL VERSION MISMATCH."
        printf '%s\n' "$dmesgtxt" | grep 'protocol version mismatch' | tail -2 >&2
        err "The guest module and the host QEMU were built from different commits."
        err "Rebuild BOTH from the same nvkvm-pv checkout (the 9p share is that checkout)."
        return 2
    fi
    if ! grep -qw "$MODULE_MOD" "$PROC_MODULES" 2>/dev/null; then
        err "CLASS 3: $MODULE_NAME is NOT LOADED."
        printf '%s\n' "$dmesgtxt" | grep -i nvkvm | tail -3 >&2
        err "Built, but the kernel refused it. Usually a vermagic/kernel mismatch"
        err "(check 'modinfo -F vermagic' against 'uname -r'), or nvkvm's virtio"
        err "device is absent from the QEMU command line."
        return 3
    fi

    # Loaded is not the same as working. An oops or WARN inside the module
    # annotates its frames with `[nvkvm_guest]`, which nothing else prints --
    # `Modules linked in:` lists the bare name, without brackets. Require a
    # fault marker as well, so a stray annotation cannot invent a failure.
    if [ "$dmesg_ok" = 1 ] \
       && printf '%s\n' "$dmesgtxt" | grep -qE '\[nvkvm_guest[] ]' \
       && printf '%s\n' "$dmesgtxt" | grep -qE 'BUG:|Oops:|kernel BUG at|general protection fault|WARNING: CPU'; then
        err "CLASS 5: the kernel FAULTED inside $MODULE_NAME. It is loaded and it is broken."
        printf '%s\n' "$dmesgtxt" | grep -E '\[nvkvm_guest[] ]|BUG:|Oops:' | tail -6 >&2
        err "Anything that opens the device may take the machine down. Do not treat"
        err "this system as working because the module is listed in /proc/modules."
        return 5
    fi

    [ -e "$DEV_NVIDIACTL" ] || { err "CLASS 4: module loaded but $DEV_NVIDIACTL is missing"; return 4; }

    # THE check that the desktop's first move actually works. This is the exact
    # operation that NULL-deref'd on a guest with no nvkvm virtio device, from
    # ksplashqml, seconds after this function had returned 0.
    log "opening $DEV_NVIDIACTL to check the module actually works"
    if ! can_open_device "$DEV_NVIDIACTL"; then
        err "CLASS 5: $DEV_NVIDIACTL exists but cannot be OPENED."
        printf '%s\n' "$dmesgtxt" | grep -i nvkvm | tail -5 >&2
        err "The module is loaded and its device node is there, but the thing every"
        err "GL/CUDA client does first fails. Usually nvkvm's virtio device is absent"
        err "from the QEMU command line (look for 'host GPU discovery failed')."
        return 5
    fi

    # Profile-aware. A trimmed profile HAS no libcuda, so checking for it would
    # report a healthy system as broken.
    if [ "$PROFILE" = compute ]; then
        ldconfig -p 2>/dev/null | grep -q libcuda \
            || { err "CLASS 4: profile=compute but libcuda is not in the linker cache"; return 4; }
        nvidia-smi -L >/dev/null 2>&1 \
            || { err "CLASS 4: nvidia-smi cannot enumerate a GPU"; return 4; }
    else
        ldconfig -p 2>/dev/null | grep -q libGLX_nvidia \
            || { err "CLASS 4: libGLX_nvidia missing (Vulkan/GL stack incomplete)"; return 4; }
        local icd icd_found=0
        for icd in $VULKAN_ICD_PATHS; do [ -e "$icd" ] && { icd_found=1; break; }; done
        [ "$icd_found" = 1 ] \
            || { err "CLASS 4: NVIDIA Vulkan ICD manifest missing from every loader path ($VULKAN_ICD_PATHS)"; return 4; }

        # The compositor opens a DRM node, not /dev/nvidiactl -- and
        # write_desktop_config()'s whole job is to point KWIN_DRM_DEVICES at the
        # one whose driver is "nvidia". If no such node exists, the session
        # cannot be on nvkvm no matter how healthy everything above looks. This
        # is a prerequisite for the desktop, not a check OF the desktop.
        local c drv found=0
        for c in "$DRM_CLASS_DIR"/card[0-9]*; do
            [ -e "$c/device/driver" ] || continue
            drv="$(basename "$(readlink -f "$c/device/driver")" 2>/dev/null)"
            [ "$drv" = nvidia ] && { found=1; break; }
        done
        if [ "$found" = 0 ]; then
            err "CLASS 6: no /dev/dri node is bound to the nvidia driver."
            err "Everything above passed, but a compositor has nothing of ours to open,"
            err "so the session would land on the emulated VGA. Check that the guest"
            err "module registered a DRM device (dmesg | grep -i drm)."
            return 6
        fi
    fi
    return 0
}

# What validate() actually established, in words, for a caller to print. Kept
# next to validate() so the two cannot drift: if you add a check up there, say
# so down here, and if you remove one, stop claiming it.
validate_verified_summary() {
    printf 'module loaded and not faulting, %s opens' "$DEV_NVIDIACTL"
    [ "$PROFILE" = compute ] && printf ', libcuda present, nvidia-smi enumerates a GPU' \
                             || printf ', GL/Vulkan userspace installed, a DRM node is bound to nvidia'
    printf '\n'
}

# ── Part 2 ───────────────────────────────────────────────────────────────────
do_boot() {
    log "=== Part 2: boot ==="
    if grep -qw 'nvkvm.skip=1' /proc/cmdline 2>/dev/null; then
        log "nvkvm.skip=1 -- skipping nvkvm entirely"; return 0
    fi
    do_install || warn "converge reported problems; continuing to validation"

    load_nvkvm_module || true

    if validate; then
        # Say what was actually established, and say -- every time, not only
        # when something looks wrong -- what this check is incapable of seeing.
        # The previous wording was "validation OK -- handing over to the
        # desktop", and it printed that while kwin was crash-looping and no
        # desktop existed. A validator that over-claims is worse than none,
        # because it stops people looking.
        log "nvkvm checks passed: $(validate_verified_summary)"
        if [ -n "$VALIDATE_UNVERIFIED" ]; then
            warn "...but this run could NOT verify:$VALIDATE_UNVERIFIED"
        fi
        log "NOT CHECKED: whether the desktop starts. This unit is ordered before"
        log "display-manager.service, so no compositor has run yet. If the screen"
        log "stays black, check the session, not nvkvm:"
        log "  systemctl status display-manager; journalctl -b -u display-manager"
        log "  journalctl -b --user-unit plasma-kwin_wayland   # as the desktop user"
        return 0
    fi

    err "nvkvm validation FAILED (class above). The desktop will start on the emulated VGA."
    err "Run 'nvkvm-recovery.sh menu', or boot with nvkvm.skip=1 to skip this entirely."
    local i
    for i in $(seq 30 -1 1); do printf '\r[nvkvm] continuing in %2ds ' "$i" >&2; sleep 1; done
    printf '\n' >&2
    return 0
}

# ── Entry point ──────────────────────────────────────────────────────────────
# Regression tests source the functions above and stop here. Production never
# sets this variable; keeping the guard next to argument parsing prevents a
# sourced test from provisioning the machine running the test.
if [ "${NVKVM_STEAMOS_BOOT_SOURCE_ONLY:-0}" = 1 ]; then
    return 0 2>/dev/null || exit 0
fi

CMD=boot
while [ $# -gt 0 ]; do
    case "$1" in
        --install-only) CMD=install; shift ;;
        boot)           CMD=boot; shift ;;
        validate)       CMD=validate; shift ;;
        --root)         ROOT="$2"; shift 2 ;;
        --profile)      PROFILE="$2"; shift 2 ;;
        --driver-version) DRIVER_VERSION="$2"; shift 2 ;;
        --old-run-file) RUN_CACHE_DIR="$2"; shift 2 ;;
        *) err "unknown argument: $1"; exit 2 ;;
    esac
done
case "$PROFILE" in steamos|compute) ;; *) err "unknown profile '$PROFILE'"; exit 2 ;; esac
if [ -n "$DRIVER_VERSION" ] && ! [[ "$DRIVER_VERSION" =~ ^[0-9]+([.][0-9]+)+$ ]]; then
    err "invalid --driver-version '$DRIVER_VERSION'"
    exit 2
fi
if [ "$CMD" = install ] && [ -z "$(host_driver_version)" ]; then
    err "--install-only requires --driver-version VERSION when /proc/driver/nvidia/version is unavailable"
    exit 1
fi

case "$CMD" in
    install)  do_install ;;
    boot)     do_boot ;;
    validate) validate ;;
esac
