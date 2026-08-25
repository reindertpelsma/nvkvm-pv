#!/usr/bin/env bash
# install_steamos_vm.sh — produce a REAL, dual-slot SteamOS install by letting
# Valve's own installer run inside a disposable VM.
#
#   ./install_steamos_vm.sh --repair steamdeck-oobe-repair-*.img --out steamos.qcow2
#
# ── Why this exists, and why it is not build_steamos_image.sh ────────────────
#
# build_steamos_image.sh modifies Valve's *recovery* image in place. That image
# carries an A-slot-only layout (esp / efi-A / rootfs-A / var-A / home), so the
# thing it produces cannot take a SteamOS OTA update, and the nvkvm update hook
# is therefore untestable on it. It also needs root on the host for losetup,
# mount, chroot and a mount namespace, and works hard to contain the failures
# that come with them (leaked loop devices, half-unmounted trees, an agent
# holding a mount open).
#
# This script instead boots a throwaway Alpine VM and runs Valve's
# repair_device.sh inside it against a blank disk attached as NVMe. That gets
# all three of:
#
#   1. The host stays UNPRIVILEGED. No losetup, no mount, no chroot, no mount
#      namespace — just qemu, which needs only membership of the `kvm` group.
#      The entire class of "the last run left something mounted" failures is
#      deleted rather than contained.
#   2. Valve's installer does the work, so we stop reimplementing (and drifting
#      from) casefold / -T huge / steamos-bootconf / steamcl-install /
#      grub-mkimage.
#   3. The output is a genuine dual-slot A/B install — rootfs-A AND rootfs-B,
#      efi-A/efi-B, var-A/var-B — which is what makes the update hook testable.
#
# ── How the guest is bootstrapped ───────────────────────────────────────────
#
# Alpine netboot's vmlinuz-virt + initramfs-virt are standalone files; there is
# no Alpine disk image to build or maintain, because the initramfs is already a
# complete busybox userspace in RAM and all we ever do is chroot. vm/guest-init.sh
# is delivered by appending a second cpio archive to that initramfs — the kernel
# unpacks every concatenated cpio into one rootfs, the same mechanism early
# microcode loading uses — so there is no 9p mount and no extra disk needed just
# to read our own script. See vm/guest-init.sh for the guest side.
#
# The input repair image is attached with QEMU's snapshot=on: the guest sees a
# writable disk, QEMU opens the backing file read-only and throws every write
# away. The input image is never modified.
#
# Options
#   --repair FILE     pristine SteamOS repair/recovery .img (required)
#   --out FILE        output qcow2 (default: steamos-installed.qcow2)
#   --size SIZE       size of the install target (default: 64G). Valve's layout
#                     needs ~11 GiB; the installer expands Valve's initial
#                     100 MiB /home into the remaining games budget before
#                     provisioning. qcow2 is sparse, so this costs nothing up front.
#   --alpine-dir DIR  cache for the Alpine netboot files (default: ./.alpine-netboot)
#   --alpine-ver VER  Alpine release to fetch (default: 3.24.1)
#   --stages LIST     guest stages, comma separated (default: repair)
#                       repair     — Valve's repair_device.sh all, i.e. the
#                                    actual dual-slot SteamOS install
#                       provision  — steamos_boot.sh --install-only against the
#                                    rootfs-A that `repair` just wrote. Needs
#                                    --share, and an NVIDIA host (see below).
#   --share DIR       an nvkvm-pv checkout, exported read-only over 9p with the
#                     tag `nvkvm` — the same tag and mount point (/run/nvkvm)
#                     the guest gets at runtime. Required by `provision`.
#   --profile NAME    steamos (default) | compute, passed to steamos_boot.sh
#   --driver-version VERSION  NVIDIA host/container driver to provision. If
#                     omitted, read dynamically from /proc/driver/nvidia/version.
#   --no-compat32     set NVKVM_NO_COMPAT32=1 for steamos_boot.sh
#   --memory MB       guest RAM (default: 4096)
#   --cpus N          guest vCPUs (default: min(4, nproc))
#   --shell           boot the guest to an interactive shell instead of
#                     installing. The single most useful debugging tool here.
#   --shell-on-fail   drop to that shell only if a stage fails
#   --log FILE        serial log (default: <out>.log)
#   --keep-target     do not delete a failed target image
#   -h, --help        this header
#
# Exit codes
#   0 success   2 usage   3 missing host tool   4 no /dev/kvm
#   5 input problem   6 download/verify failed   7 qemu failed
#   8 the guest ran but a stage failed

set -uo pipefail

E_USAGE=2 E_TOOL=3 E_KVM=4 E_INPUT=5 E_FETCH=6 E_QEMU=7 E_GUEST=8

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

log()  { printf '[vm-install] %s\n' "$*" >&2; }
warn() { printf '[vm-install] WARNING: %s\n' "$*" >&2; }
die()  { local rc=$1; shift; printf '[vm-install] ERROR: %s\n' "$*" >&2; exit "$rc"; }

# ── args ────────────────────────────────────────────────────────────────────
REPAIR="" OUT="steamos-installed.qcow2" SIZE="64G"
ALPINE_DIR="" ALPINE_VER="3.24.1"
STAGES="repair" MEMORY=4096 CPUS="" LOGFILE=""
SHELL_MODE=0 SHELL_ON_FAIL=0 KEEP_TARGET=0
SHARE="" PROFILE="steamos" NO_COMPAT32=0
DRIVER_VERSION="${NVKVM_DRIVER_VERSION:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --repair)        REPAIR="${2:?--repair needs a path}"; shift 2 ;;
        --out)           OUT="${2:?--out needs a path}"; shift 2 ;;
        --size)          SIZE="${2:?--size needs a value}"; shift 2 ;;
        --alpine-dir)    ALPINE_DIR="${2:?--alpine-dir needs a path}"; shift 2 ;;
        --alpine-ver)    ALPINE_VER="${2:?--alpine-ver needs a version}"; shift 2 ;;
        --stages)        STAGES="${2:?--stages needs a list}"; shift 2 ;;
        --share)         SHARE="${2:?--share needs a path}"; shift 2 ;;
        --profile)       PROFILE="${2:?--profile needs a name}"; shift 2 ;;
        --driver-version) DRIVER_VERSION="${2:?--driver-version needs a value}"; shift 2 ;;
        --no-compat32)   NO_COMPAT32=1; shift ;;
        --memory)        MEMORY="${2:?--memory needs MB}"; shift 2 ;;
        --cpus)          CPUS="${2:?--cpus needs a number}"; shift 2 ;;
        --log)           LOGFILE="${2:?--log needs a path}"; shift 2 ;;
        --shell)         SHELL_MODE=1; shift ;;
        --shell-on-fail) SHELL_ON_FAIL=1; shift ;;
        --keep-target)   KEEP_TARGET=1; shift ;;
        -h|--help)       sed -n '2,/^set -uo/p' "${BASH_SOURCE[0]}" | sed 's/^# \?//;$d'; exit 0 ;;
        *)               die $E_USAGE "unknown argument: $1" ;;
    esac
done

[ -n "$REPAIR" ]  || die $E_USAGE "--repair <pristine repair .img> is required"
[ -f "$REPAIR" ]  || die $E_INPUT "--repair file not found: $REPAIR"
REPAIR="$(cd -- "$(dirname -- "$REPAIR")" && pwd)/$(basename -- "$REPAIR")"
[ -n "$ALPINE_DIR" ] || ALPINE_DIR="$HERE/.alpine-netboot"
[ -n "$LOGFILE" ]    || LOGFILE="$OUT.log"
[ -n "$CPUS" ]       || { CPUS=$(nproc 2>/dev/null || echo 2); [ "$CPUS" -gt 4 ] && CPUS=4; }
case "$PROFILE" in steamos|compute) ;; *) die $E_USAGE "unknown profile '$PROFILE' (steamos|compute)" ;; esac
if [[ ",$STAGES," == *,provision,* ]] && [ -z "$SHARE" ]; then
    die $E_USAGE "--stages provision needs --share <nvkvm-pv checkout>"
fi
if [[ ",$STAGES," == *,provision,* ]]; then
    if [ -z "$DRIVER_VERSION" ] && [ -r /proc/driver/nvidia/version ]; then
        DRIVER_VERSION="$(awk '{for(i=1;i<=NF;i++) if ($i ~ /^[0-9]+\.[0-9]+/) {print $i; exit}}' /proc/driver/nvidia/version)"
    fi
    [[ "$DRIVER_VERSION" =~ ^[0-9]+([.][0-9]+)+$ ]] \
        || die $E_INPUT "provisioning needs a valid NVIDIA driver version; pass --driver-version or expose /proc/driver/nvidia/version"
    log "host NVIDIA: $DRIVER_VERSION"
fi

# ── preflight: tools, KVM, firmware ─────────────────────────────────────────
for t in qemu-system-x86_64 qemu-img cpio gzip curl tar; do
    command -v "$t" >/dev/null || die $E_TOOL "missing host tool: $t"
done

# The whole point of this rewrite: no root, no loop devices, no mounts. The one
# privilege needed is read/write on /dev/kvm, which is a group membership.
[ -c /dev/kvm ]  || die $E_KVM "/dev/kvm not present — this host cannot run KVM (nested virt off?)"
[ -w /dev/kvm ]  || die $E_KVM "/dev/kvm is not writable by $(id -un) — add yourself to the 'kvm' group and re-login"

# steamos-chroot unconditionally bind-mounts /sys/firmware/efi/efivars, so the
# guest kernel has to have booted under UEFI for that directory to exist. Hence
# OVMF rather than SeaBIOS. (A tmpfs faking the path does not work: steamos-chroot
# uses `mount --bind /sys`, which does not carry submounts.)
OVMF_CODE=""
for c in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
         /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2-ovmf/x64/OVMF_CODE.fd \
         /usr/share/qemu/edk2-x86_64-code.fd; do
    [ -f "$c" ] && { OVMF_CODE="$c"; break; }
done
[ -n "$OVMF_CODE" ] || die $E_TOOL "no OVMF firmware found (install the 'ovmf' / 'edk2-ovmf' package)"
OVMF_VARS=""
for v in "${OVMF_CODE%CODE*}VARS${OVMF_CODE##*CODE}" \
         /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd \
         /usr/share/edk2/ovmf/OVMF_VARS.fd /usr/share/edk2-ovmf/x64/OVMF_VARS.fd \
         /usr/share/qemu/edk2-i386-vars.fd; do
    [ -f "$v" ] && { OVMF_VARS="$v"; break; }
done
[ -n "$OVMF_VARS" ] || die $E_TOOL "found $OVMF_CODE but no matching OVMF VARS template"
log "firmware: $OVMF_CODE"

# ── Alpine netboot: vmlinuz-virt + initramfs-virt + modloop-virt ────────────
# These are plain downloadable files. There is no Alpine image to build: the
# initramfs IS the userspace. modloop-virt is needed because initramfs-virt
# ships virtio_blk/squashfs/loop/vfat but NOT nvme, btrfs or ext4 — VERIFIED by
# unpacking it, see vm/guest-init.sh. It is attached as a disk and mounted as
# the module tree, complete with modules.dep, so plain modprobe and kernel
# autoload both work; hand-picking .ko files and insmod-ing them would mean
# re-deriving btrfs's dependency closure on every Alpine kernel bump.
ALPINE_BRANCH="v${ALPINE_VER%.*}"
TARBALL="alpine-netboot-${ALPINE_VER}-x86_64.tar.gz"
BASEURL="https://dl-cdn.alpinelinux.org/alpine/${ALPINE_BRANCH}/releases/x86_64"
mkdir -p "$ALPINE_DIR" || die $E_FETCH "cannot create $ALPINE_DIR"

if [ ! -f "$ALPINE_DIR/boot/vmlinuz-virt" ] \
   || [ ! -f "$ALPINE_DIR/boot/initramfs-virt" ] \
   || [ ! -f "$ALPINE_DIR/boot/modloop-virt" ]; then
    log "fetching $TARBALL"
    curl -fsSL -o "$ALPINE_DIR/$TARBALL" "$BASEURL/$TARBALL" \
        || die $E_FETCH "could not download $BASEURL/$TARBALL"
    curl -fsSL -o "$ALPINE_DIR/$TARBALL.sha256" "$BASEURL/$TARBALL.sha256" \
        || warn "no .sha256 published for $TARBALL — skipping verification"
    if [ -s "$ALPINE_DIR/$TARBALL.sha256" ] && command -v sha256sum >/dev/null; then
        ( cd "$ALPINE_DIR" && sha256sum -c "$TARBALL.sha256" >/dev/null ) \
            || die $E_FETCH "checksum mismatch on $TARBALL"
        log "checksum ok"
    fi
    tar -xzf "$ALPINE_DIR/$TARBALL" -C "$ALPINE_DIR" \
        boot/vmlinuz-virt boot/initramfs-virt boot/modloop-virt \
        || die $E_FETCH "could not unpack $TARBALL"
fi

# ── work dir ────────────────────────────────────────────────────────────────
WORK="$(mktemp -d "${TMPDIR:-/tmp}/nvkvm-vm-install.XXXXXX")" || die $E_TOOL "mktemp failed"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# ── combined initramfs = Alpine's + our cpio ────────────────────────────────
GUEST_INIT="$HERE/vm/guest-init.sh"
[ -f "$GUEST_INIT" ] || die $E_INPUT "guest init not found: $GUEST_INIT"
mkdir -p "$WORK/cpio/nvkvm"
cp "$GUEST_INIT" "$WORK/cpio/nvkvm/guest-init.sh"
chmod +x "$WORK/cpio/nvkvm/guest-init.sh"
( cd "$WORK/cpio" && find . -print0 | cpio --null -o -H newc --owner root:root 2>/dev/null | gzip -9 ) \
    > "$WORK/custom.cpio.gz" || die $E_TOOL "could not build the custom cpio"
cat "$ALPINE_DIR/boot/initramfs-virt" "$WORK/custom.cpio.gz" > "$WORK/combined.img" \
    || die $E_TOOL "could not concatenate the initramfs"

cp "$OVMF_VARS" "$WORK/ovmf_vars.fd" || die $E_TOOL "could not copy OVMF vars"
chmod u+w "$WORK/ovmf_vars.fd"

# ── the install target ──────────────────────────────────────────────────────
if [ -e "$OUT" ]; then
    # --shell is for poking at an install that already exists, so reuse it there;
    # any other mode would be silently destroying someone's image.
    [ "$SHELL_MODE" = 1 ] || die $E_INPUT "refusing to overwrite existing $OUT (move it aside)"
    log "--shell: attaching the existing $OUT instead of creating one"
else
    qemu-img create -f qcow2 "$OUT" "$SIZE" >/dev/null || die $E_TOOL "qemu-img create failed"
fi

# ── kernel command line ─────────────────────────────────────────────────────
CMDLINE="console=ttyS0,115200 rdinit=/nvkvm/guest-init.sh nvkvm.stages=$STAGES nvkvm.profile=$PROFILE"
[ -n "$DRIVER_VERSION" ] && CMDLINE="$CMDLINE nvkvm.driver_version=$DRIVER_VERSION"
[ "$NO_COMPAT32" = 1 ]   && CMDLINE="$CMDLINE nvkvm.no_compat32=1"
[ "$SHELL_MODE" = 1 ]    && CMDLINE="$CMDLINE nvkvm.shell=1"
[ "$SHELL_ON_FAIL" = 1 ] && CMDLINE="$CMDLINE nvkvm.shell_on_fail=1"

# The share and a network are only needed by `provision`. Attached
# unconditionally when a --share is given so `--shell` sessions have them too.
EXTRA=()
if [ -n "$SHARE" ]; then
    [ -d "$SHARE" ] || die $E_INPUT "--share is not a directory: $SHARE"
    [ -x "$SHARE/boot/steamos_boot.sh" ] \
        || die $E_INPUT "--share does not look like an nvkvm-pv checkout (no boot/steamos_boot.sh): $SHARE"
    # security_model=none keeps this unprivileged: no chown/mknod attempted,
    # and readonly=on means the guest cannot write to your checkout.
    EXTRA+=( -virtfs "local,path=$SHARE,mount_tag=nvkvm,security_model=none,readonly=on" )
fi
if [[ ",$STAGES," == *,provision,* ]] || [ "$SHELL_MODE" = 1 ]; then
    # User-mode networking: no bridge, no tap, no root. pacman (kernel headers)
    # and the NVIDIA .run download both need it.
    EXTRA+=( -netdev "user,id=net0" -device "virtio-net-pci,netdev=net0" )
fi

QEMU=(
    qemu-system-x86_64
    -machine "q35,accel=kvm" -cpu host -smp "$CPUS" -m "$MEMORY"
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,unit=1,file=$WORK/ovmf_vars.fd"
    -kernel "$ALPINE_DIR/boot/vmlinuz-virt"
    -initrd "$WORK/combined.img"
    -append "$CMDLINE"
    # modloop: the kernel module tree, read-only
    -drive "file=$ALPINE_DIR/boot/modloop-virt,format=raw,if=none,id=modloop,readonly=on"
    -device "virtio-blk-pci,drive=modloop"
    # the repair image. snapshot=on -> backing file opened read-only, all guest
    # writes land in a throwaway overlay. The input is NEVER modified.
    -drive "file=$REPAIR,format=raw,if=none,id=repair,snapshot=on"
    -device "virtio-blk-pci,drive=repair"
    # the install target, attached as NVMe *specifically* because
    # repair_device.sh hardcodes DISK=/dev/nvme0n1 (a plain assignment at line
    # 17, not env-overridable). As NVMe it lands at exactly that path with no
    # ambiguity and no patching of Valve's script.
    -drive "file=$OUT,format=qcow2,if=none,id=target,cache=unsafe"
    -device "nvme,drive=target,serial=nvkvmtarget"
    -display none -monitor none -no-reboot
    "${EXTRA[@]+"${EXTRA[@]}"}"
)

log "target: $OUT ($SIZE)"
log "stages: $STAGES"
log "serial log: $LOGFILE"

start=$(date +%s)
if [ "$SHELL_MODE" = 1 ] || [ "$SHELL_ON_FAIL" = 1 ]; then
    # Interactive: give the guest the terminal, and tee so the log still exists.
    "${QEMU[@]}" -serial mon:stdio 2>&1 | tee "$LOGFILE"
    qrc=${PIPESTATUS[0]}
else
    "${QEMU[@]}" -serial "file:$LOGFILE"
    qrc=$?
fi
elapsed=$(( $(date +%s) - start ))

[ "$qrc" = 0 ] || die $E_QEMU "qemu exited $qrc after ${elapsed}s — see $LOGFILE"

# The guest reports its own verdict on the serial console; there is no other
# channel out of a machine that has already powered itself off.
verdict="$(grep -a 'NVKVM-GUEST-RESULT:' "$LOGFILE" | tail -1 | tr -d '\r')"
if [ -z "$verdict" ]; then
    [ "$KEEP_TARGET" = 1 ] || rm -f "$OUT"
    die $E_GUEST "the guest never reported a result (it died or hung) — see $LOGFILE"
fi
rc="${verdict##*: }"
if [ "$rc" != 0 ]; then
    [ "$KEEP_TARGET" = 1 ] || rm -f "$OUT"
    die $E_GUEST "guest stage failed (rc=$rc) after ${elapsed}s — see $LOGFILE"
fi

log "done in ${elapsed}s: $OUT"
log "partition layout:"
qemu-img info "$OUT" >&2 || true
