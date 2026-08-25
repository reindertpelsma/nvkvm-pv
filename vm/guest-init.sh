#!/bin/sh
# guest-init.sh — PID 1 inside the disposable Alpine VM.
#
# This is NOT Alpine's own initramfs /init. It is delivered by appending a
# second cpio archive to Alpine's stock `initramfs-virt` (the kernel unpacks
# every concatenated cpio into one rootfs — the same mechanism early microcode
# loading uses), and selected with `rdinit=/nvkvm/guest-init.sh` on the kernel
# command line. Alpine's own /init is left untouched and never runs.
#
# Everything here is busybox + kmod. The heavy lifting is done by Valve's own
# tools, run inside a chroot on the SteamOS repair rootfs — so this script's
# only jobs are: mount the kernel filesystems, get the kernel modules that
# `initramfs-virt` does NOT ship (nvme, btrfs, ext4) out of `modloop-virt`,
# assemble the chroot, and run the requested stages.
#
# Kernel command line parameters (all optional):
#   nvkvm.stages=repair[,provision]   which stages to run (default: repair)
#   nvkvm.shell=1                     drop to a shell instead of running stages
#   nvkvm.shell_on_fail=1             drop to a shell if a stage fails
#
# Every interesting line is echoed to the serial console; the host script
# greps that log for the RESULT sentinel below.

set -u

# Alpine's initramfs ships /bin/busybox and a /bin/sh symlink and NOTHING else
# — no applet symlinks at all, and its busybox is not built with the standalone
# shell feature. So before anything, `mount`, `sleep`, `poweroff` etc. do not
# exist as commands. Alpine's own /init works around this by being careful; we
# just install the applet links into the initramfs tmpfs, which is writable.
PATH=/usr/bin:/usr/sbin:/bin:/sbin
export PATH
/bin/busybox --install -s 2>/dev/null || /bin/busybox mkdir -p /bin

RESULT_TAG="NVKVM-GUEST-RESULT"

log()  { echo "[guest] $*"; }
fail() { echo "[guest] FATAL: $*"; finish 1; }

finish() {
    rc="$1"
    stop_udev 2>/dev/null
    log "unmounting"
    umount "$CHROOT/tmp"  2>/dev/null
    umount "$CHROOT/run"  2>/dev/null
    umount "$CHROOT/sys/firmware/efi/efivars" 2>/dev/null
    umount "$CHROOT/sys"  2>/dev/null
    umount "$CHROOT/proc" 2>/dev/null
    umount "$CHROOT/dev/pts" 2>/dev/null
    umount "$CHROOT/dev"  2>/dev/null
    umount "$CHROOT/home" 2>/dev/null
    umount "$CHROOT/var"  2>/dev/null
    umount "$CHROOT"      2>/dev/null
    sync
    echo "$RESULT_TAG: $rc"
    # Give the serial console a moment to drain before the machine vanishes.
    sleep 1
    poweroff -f
    # poweroff -f should not return; if it does, PID 1 must not exit.
    while :; do sleep 60; done
}

CHROOT=/mnt/repair

# ---------------------------------------------------------------- kernel fs --
mount -t proc     none /proc     || fail "mount /proc"
mount -t sysfs    none /sys      || fail "mount /sys"
mount -t devtmpfs none /dev      || fail "mount /dev"
mkdir -p /dev/pts /dev/shm /run
mount -t devpts   none /dev/pts  2>/dev/null
mount -t tmpfs    none /dev/shm  2>/dev/null
mount -t tmpfs    none /run      || fail "mount /run"

# devtmpfs does not create these; udev/systemd normally does, and there is
# neither here. MEASURED: without /dev/fd, steamos-chroot dies on its very
# first line of real work with "/dev/fd/63: No such file or directory" —
# get_device_by_partlabel() feeds sfdisk to a `while read` through a bash
# process substitution, and process substitution IS /dev/fd. Cheap to create,
# fatal to omit, and nothing in Valve's tree needs changing.
ln -sf /proc/self/fd   /dev/fd
ln -sf /proc/self/fd/0 /dev/stdin
ln -sf /proc/self/fd/1 /dev/stdout
ln -sf /proc/self/fd/2 /dev/stderr

# Unbuffered-ish console: everything below goes to ttyS0 via /dev/console.
log "kernel $(uname -r)"
log "cmdline: $(cat /proc/cmdline)"

# ------------------------------------------------------------------- cmdline --
STAGES=repair
SHELL_ONLY=
SHELL_ON_FAIL=
PROFILE=steamos
NO_COMPAT32=
DRIVER_VERSION=
for arg in $(cat /proc/cmdline); do
    case "$arg" in
        nvkvm.stages=*)        STAGES="${arg#nvkvm.stages=}" ;;
        nvkvm.profile=*)       PROFILE="${arg#nvkvm.profile=}" ;;
        nvkvm.driver_version=*) DRIVER_VERSION="${arg#nvkvm.driver_version=}" ;;
        nvkvm.no_compat32=1)   NO_COMPAT32=1 ;;
        nvkvm.shell=1)         SHELL_ONLY=1 ;;
        nvkvm.shell_on_fail=1) SHELL_ON_FAIL=1 ;;
    esac
done

# ------------------------------------------------------------------ modules --
# initramfs-virt ships virtio_blk, squashfs, loop, overlay, vfat and the nls
# tables — but NOT nvme, btrfs or ext4, all three of which we need. They live
# in modloop-virt, which is a squashfs of the complete module tree *including*
# modules.dep, attached here as a virtio-blk disk. Mounting it and pointing
# /lib/modules at it makes ordinary modprobe and kernel autoload
# (request_module() from `mount -t btrfs`) both work. Hand-picking .ko files
# and insmod-ing them would mean re-deriving btrfs's dependency closure
# (zstd_compress, lzo_compress, raid6_pq, xor, libcrc32c, ...) on every Alpine
# kernel bump — don't.
modprobe -a virtio_pci virtio_blk squashfs loop 2>/dev/null

i=0
while [ ! -b /dev/vda ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
[ -b /dev/vda ] || fail "modloop disk /dev/vda never appeared"

# Identify the disks by what they contain, not by the order QEMU happened to
# enumerate them: modloop is the one that mounts as squashfs, the repair image
# is the one with a partition 3.
mkdir -p /.modloop
MODLOOP_DEV=
for d in /dev/vd?; do
    if mount -t squashfs -o ro "$d" /.modloop 2>/dev/null; then MODLOOP_DEV="$d"; break; fi
done
[ -n "$MODLOOP_DEV" ] || fail "no virtio disk mounts as squashfs — modloop-virt not attached?"
log "modloop is $MODLOOP_DEV"
[ -d "/.modloop/modules/$(uname -r)" ] \
    || fail "modloop does not carry modules for $(uname -r) — kernel/modloop mismatch"
mount -o bind /.modloop/modules /lib/modules || fail "bind /lib/modules"

modprobe -a nvme btrfs ext4 vfat || fail "modprobe nvme/btrfs/ext4/vfat"
log "modules loaded: $(cut -d' ' -f1 /proc/modules | tr '\n' ' ')"

# ------------------------------------------------------------------- disks ---
# /dev/vda  modloop-virt         (squashfs, ro)
# /dev/vdb  SteamOS repair image (raw, QEMU snapshot=on — writes are discarded,
#                                 the host's input file is opened read-only)
# /dev/nvme0n1  install target   (attached as NVMe *specifically* because
#                                 repair_device.sh hardcodes DISK=/dev/nvme0n1)
REPAIR_DEV=
for d in /dev/vd?; do
    [ "$d" = "$MODLOOP_DEV" ] && continue
    [ -b "${d}3" ] && REPAIR_DEV="$d" && break
done
[ -n "$REPAIR_DEV" ] || fail "no partitioned virtio disk found — repair image not attached?"
log "repair image is $REPAIR_DEV"
i=0
while [ ! -b /dev/nvme0n1 ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
[ -b /dev/nvme0n1 ] || fail "install target /dev/nvme0n1 did not appear"

# ------------------------------------------------------------------ chroot ---
# The chroot root must be the *block device* holding the repair rootfs, not an
# overlay: repair_device.sh finds its own source with `findmnt -n -o source /`
# and dd's that device into the target. An overlayfs root would report
# "overlay" there and imageroot would fail.
mkdir -p "$CHROOT"
mount -t btrfs "${REPAIR_DEV}3" "$CHROOT"      || fail "mount repair rootfs"
mount -t ext4  "${REPAIR_DEV}4" "$CHROOT/var"  || fail "mount repair var"
mount -t ext4  "${REPAIR_DEV}5" "$CHROOT/home" || fail "mount repair home"
# The repair rootfs is a read-only btrfs subvolume (ro=true), so /tmp needs to
# be real storage: steamos-chroot does `CHROOTDIR=$(mktemp -d)`.
mount -t tmpfs -o size=1G none "$CHROOT/tmp" || fail "mount chroot tmpfs"
mount -o bind /dev  "$CHROOT/dev"  || fail "bind /dev"
mount -t devpts none "$CHROOT/dev/pts" 2>/dev/null
mount -o bind /proc "$CHROOT/proc" || fail "bind /proc"
mount -o bind /sys  "$CHROOT/sys"  || fail "bind /sys"
mount -o bind /run  "$CHROOT/run"  || fail "bind /run"

# steamos-chroot unconditionally does
#     mount --bind /sys/firmware/efi/efivars $dir/sys/firmware/efi/efivars
# so that directory has to exist. It is created by the kernel's efivarfs
# registration, which only happens when we booted under UEFI — hence the host
# script boots this kernel with OVMF rather than SeaBIOS. A bind of /sys does
# not carry submounts, so faking it with a tmpfs over /sys/firmware does not
# work; real UEFI is the only clean fix.
if [ ! -d /sys/firmware/efi/efivars ]; then
    fail "/sys/firmware/efi/efivars missing — the VM did not boot under UEFI, and steamos-chroot will fail"
fi
# And efivarfs has to be MOUNTED, not merely present. MEASURED: with the
# directory there but nothing mounted on it, everything runs to the very last
# step and then steamcl-install's efibootmgr reports "EFI variables are not
# supported on this system" -> "ESP: Failed to create boot entry 0000" -> rc=1,
# after the whole install has already succeeded. The NVRAM entry it writes goes
# into the throwaway OVMF vars file and is discarded with the VM; what actually
# makes the image bootable is the removable-path copy that
# `--force-extra-removable` puts at /esp/efi/boot/bootx64.efi.
#
# It has to be mounted at the path INSIDE the chroot, not on the guest's own
# /sys: `mount -o bind /sys "$CHROOT/sys"` above is a plain bind, which does not
# carry submounts, so an efivarfs mounted on the guest's /sys/firmware/efi/efivars
# is invisible in there — and steamos-chroot then binds an empty sysfs directory
# into the inner chroot, which is exactly the failure this is fixing.
modprobe efivarfs 2>/dev/null
mount -t efivarfs none "$CHROOT/sys/firmware/efi/efivars" \
    || log "WARNING: could not mount efivarfs; steamcl-install will fail to create its boot entry"

# ------------------------------------------------------------------- stubs ---
# Two things repair_device.sh does are meaningless in a VM and would otherwise
# abort the run (it is `set -eu` with a `trap err ERR`, and its cmd() wrapper
# does not swallow return values):
#
#   * jupiter-biosupdate / jupiter-controller-update. MEASURED: both check
#     /sys/class/dmi/id/board_{vendor,name} for Valve/Jupiter|Galileo and exit 0
#     on anything else, so under QEMU they self-skip and need no stub at all.
#     Left documented here because it is the single most likely thing to break
#     on a future repair image.
#
#   * `systemctl poweroff|reboot` at the very end of prompt_reboot. There is no
#     systemd in this chroot, so this WOULD fail — after all the real work is
#     done, which is the worst kind of failure to have to distinguish. Stubbed.
#
# Stubs go in a tmpfs directory that is prepended to PATH, so Valve's scripts
# are used verbatim and nothing on any image is patched or forked.
STUBS=/run/nvkvm-stubs
mkdir -p "$STUBS"
cat > "$STUBS/systemctl" <<'EOF'
#!/bin/sh
echo "[stub] systemctl $*  (no systemd in this chroot; ignored)"
exit 0
EOF
chmod +x "$STUBS/systemctl"

CHROOT_PATH="$STUBS:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

# ------------------------------------------------------------------- udev ----
# Alpine's initramfs has no udev (it uses mdev/nlplug-findfs), and devtmpfs on
# its own populates neither /run/udev nor /dev/disk/*. Valve's tools assume a
# Deck, where udev is running, and they degrade in ways that are hard to read:
#
#   * MEASURED: `lsblk --pairs -o NAME,PKNAME,PARTTYPE,PARTUUID,FSTYPE,MOUNTPOINT`
#     returns EMPTY PARTTYPE/PARTUUID/FSTYPE without a udev database. steamcl-install's
#     find_esp() then falls through to its loose "just match /esp" branch, leaves
#     $esp_partuuid empty, and need_new_efi_boot_entry passes only two arguments to
#     check_boot_path — which is `set -eu`, so it dies with the maximally
#     unhelpful "steamcl-install: line 464: 3: unbound variable".
#   * /dev/disk/by-partsets/* never appears, which steamos-chroot uses whenever
#     it is called WITHOUT --disk.
#
# The fix is not to patch Valve's scripts but to give them the environment they
# expect: the repair image ships udevadm (systemd-udevd is a symlink to it), so
# run the real thing inside the chroot. It needs only /dev, /sys and /run, all
# of which are already mounted.
start_udev() {
    [ -x "$CHROOT/usr/bin/udevadm" ] || { log "WARNING: no udevadm in the repair image"; return 0; }
    chroot "$CHROOT" /usr/bin/udevadm --version >/dev/null 2>&1 || return 0
    chroot "$CHROOT" /usr/lib/systemd/systemd-udevd --daemon \
        || { log "WARNING: systemd-udevd would not start"; return 0; }
    chroot "$CHROOT" /usr/bin/udevadm trigger --action=add --subsystem-match=block
    chroot "$CHROOT" /usr/bin/udevadm settle --timeout=30
    log "udevd running; /dev/disk: $(ls "$CHROOT/dev/disk" 2>/dev/null | tr '\n' ' ')"
}
stop_udev() {
    chroot "$CHROOT" /usr/bin/udevadm control --exit >/dev/null 2>&1
}

run_in_chroot() {
    chroot "$CHROOT" /usr/bin/env -i \
        PATH="$CHROOT_PATH" \
        HOME=/root TERM=dumb \
        SHELL=/bin/bash \
        NOPROMPT=1 \
        FORCEBIOS=1 \
        "$@"
}

# --------------------------------------------------------------- provision ---
# Paths are inside the repair chroot; TARGET_MNT deliberately lives under /run
# and not /mnt, because in SteamOS /mnt is a symlink into /var — which rauc
# reformats on every update.
TARGET_MNT=/run/nvkvm-target
SHARE_MNT=/run/nvkvm

grow_home_partition() {
    gh_disk_sectors="$(cat /sys/class/block/nvme0n1/size 2>/dev/null)"
    gh_start_sectors="$(cat /sys/class/block/nvme0n1p8/start 2>/dev/null)"
    gh_size_sectors="$(cat /sys/class/block/nvme0n1p8/size 2>/dev/null)"
    for gh_value in "$gh_disk_sectors" "$gh_start_sectors" "$gh_size_sectors"; do
        case "$gh_value" in
            ''|*[!0-9]*) log "FATAL: could not read the SteamOS home-partition geometry"; return 1 ;;
        esac
    done
    gh_end_sectors=$((gh_start_sectors + gh_size_sectors))
    # Valve deliberately creates a 100 MiB home and leaves the rest of the
    # target unallocated for first-boot expansion. Provisioning happens before
    # that boot and needs room for the module build area and NVIDIA .run, so do
    # the same expansion offline. Leave a MiB of tolerance for the backup GPT.
    if [ $((gh_end_sectors + 2048)) -ge "$gh_disk_sectors" ]; then
        log "home partition already fills the install disk"
        return 0
    fi
    log "growing Valve's ${gh_size_sectors}-sector home partition to the end of the install disk"
    run_in_chroot parted --script /dev/nvme0n1 resizepart 8 100% \
        || { log "FATAL: could not grow /dev/nvme0n1p8"; return 1; }
    run_in_chroot partprobe /dev/nvme0n1 \
        || { log "FATAL: kernel did not accept the expanded partition table"; return 1; }
    chroot "$CHROOT" /usr/bin/udevadm settle --timeout=10 2>/dev/null
    run_in_chroot resize2fs /dev/nvme0n1p8 \
        || { log "FATAL: could not expand the home filesystem"; return 1; }
    gh_new_bytes="$(run_in_chroot blockdev --getsize64 /dev/nvme0n1p8 2>/dev/null)"
    log "home partition expanded to ${gh_new_bytes:-unknown} bytes"
}

provision_setup() {
    log "bringing up user-mode networking (pacman + the NVIDIA .run need it)"
    modprobe virtio_net 2>/dev/null
    ip link set lo up 2>/dev/null

    # systemd-udevd is already running for Valve's disk tooling. Loading
    # virtio_net therefore races its predictable-name rename: the interface can
    # become enp0s6 between `ip link set eth0 up` and udhcpc's lease script.
    # Bringing the link up is what triggered that rename in the measured VM, so
    # do it as a separate pass, settle, and only then run DHCP by final name.
    for n in /sys/class/net/*; do
        [ -e "$n" ] || continue
        ifname="${n##*/}"
        [ "$ifname" != lo ] || continue
        ip link set "$ifname" up 2>/dev/null
    done
    chroot "$CHROOT" /usr/bin/udevadm settle --timeout=10 2>/dev/null
    sleep 1
    for n in /sys/class/net/*; do
        [ -e "$n" ] || continue
        ifname="${n##*/}"
        [ "$ifname" != lo ] || continue
        udhcpc -i "$ifname" -q -n -t 5 >/dev/null 2>&1 && log "$ifname configured by DHCP"
    done
    log "IPv4: $(ip -4 -o addr show 2>/dev/null | tr '\n' ' ')"
    log "routes: $(ip -4 route show 2>/dev/null | tr '\n' ' ')"
    ip -4 route show 2>/dev/null | grep -q '^default ' \
        || { log "ERROR: DHCP did not install a default route"; return 1; }

    grow_home_partition || return 1

    # The read-only 9p share stands in for the one the guest gets at runtime.
    # It is mounted at $CHROOT/run/nvkvm rather than /run/nvkvm and then
    # bind-mounted, because a `mount --bind /run` does not carry submounts.
    modprobe -a 9p 9pnet_virtio 2>/dev/null
    mkdir -p "$CHROOT$SHARE_MNT"
    if ! mount -t 9p -o trans=virtio,version=9p2000.L,ro,msize=512000 \
                nvkvm "$CHROOT$SHARE_MNT" 2>/dev/null; then
        log "FATAL: could not mount the 9p share (tag 'nvkvm') — pass --share to the host script"
        return 1
    fi
    [ -x "$CHROOT$SHARE_MNT/boot/steamos_boot.sh" ] \
        || { log "FATAL: $SHARE_MNT/boot/steamos_boot.sh not found on the share"; return 1; }

    # The installed rootfs-A, plus the partitions steamos_boot.sh writes into.
    # /home matters: it is where the 1 GB module build area and the .run cache
    # land, and the 5 GiB rootfs has no room for either.
    mkdir -p "$CHROOT$TARGET_MNT"
    mount -t btrfs /dev/nvme0n1p4 "$CHROOT$TARGET_MNT" \
        || { log "FATAL: could not mount the installed rootfs-A (/dev/nvme0n1p4)"; return 1; }
    mount -t ext4 /dev/nvme0n1p8 "$CHROOT$TARGET_MNT/home" \
        || log "WARNING: could not mount the installed /home — build area falls back to the rootfs"

    # steamos_boot.sh chroots into the target for pacman/curl/nvidia-installer.
    mount -o bind /proc "$CHROOT$TARGET_MNT/proc" 2>/dev/null
    mount -o bind /sys  "$CHROOT$TARGET_MNT/sys"  2>/dev/null
    mount -o bind /dev  "$CHROOT$TARGET_MNT/dev"  2>/dev/null

    # A resolver for both chroots. The installer always uses QEMU user-mode
    # networking, whose DNS forwarder is 10.0.2.3. Do not copy the initramfs'
    # /etc/resolv.conf: it can contain the build host's stub or LAN resolver,
    # neither of which is necessarily reachable through slirp. Both /etc trees
    # are read-only, so bind a file from tmpfs over them instead.
    printf 'nameserver 10.0.2.3\n' > /run/resolv.conf
    mount -o bind /run/resolv.conf "$CHROOT/etc/resolv.conf" 2>/dev/null \
        || log "WARNING: no resolver in the repair chroot"
    mount -o bind /run/resolv.conf "$CHROOT$TARGET_MNT/etc/resolv.conf" 2>/dev/null \
        || log "WARNING: no resolver in the target chroot; pacman will fail to resolve"

    return 0
}

provision_teardown() {
    umount "$CHROOT$TARGET_MNT/etc/resolv.conf" 2>/dev/null
    umount "$CHROOT/etc/resolv.conf" 2>/dev/null
    umount "$CHROOT$TARGET_MNT/dev"  2>/dev/null
    umount "$CHROOT$TARGET_MNT/sys"  2>/dev/null
    umount "$CHROOT$TARGET_MNT/proc" 2>/dev/null
    umount "$CHROOT$TARGET_MNT/home" 2>/dev/null
    umount "$CHROOT$TARGET_MNT" 2>/dev/null
    umount "$CHROOT$SHARE_MNT" 2>/dev/null
}

# ------------------------------------------------------------------ stages ---
if [ -n "$SHELL_ONLY" ]; then
    start_udev
    # Best effort: if the target already carries an install and a share was
    # attached, wire up everything the provision stage would use, so the shell
    # is a place to debug provisioning rather than a bare initramfs prompt.
    if [ -b /dev/nvme0n1p4 ]; then
        provision_setup || log "provision_setup incomplete — see above"
    fi
    log "nvkvm.shell=1 — dropping to a shell (exit it to power off)"
    log "the SteamOS chroot is at $CHROOT; enter it with:"
    log "  chroot $CHROOT /usr/bin/env -i PATH=$CHROOT_PATH HOME=/root TERM=dumb /bin/bash"
    setsid cttyhack /bin/sh
    finish 0
fi

# ------------------------------------------------- make slot A the boot target
# repair_device.sh runs `steamos-bootconf create --image A|B`, which writes both
# slot configs with EVERY field zero — including boot-requested-at. MEASURED: an
# install left in that state boots as far as OVMF handing control to
# steamcl.efi and then STOPS DEAD. The chainloader blanks the screen, spins on
# CPU, and issues not one disk read; the machine never gets to grub. Setting
# boot-requested-at on slot A is the entire difference between that and a
# normal boot (verified by patching just that one field on the produced image:
# the guest immediately switched the display to 1280x800 and started writing
# tens of MB — i.e. it booted).
#
# On a Deck the reimage is followed by a reboot into an OOBE flow that presumably
# arranges this; headless we have to do it ourselves. Done with Valve's own
# steamos-bootconf, in Valve's own chroot, with the same --conf-dir/--efi-dir
# repair_device.sh passes to `create`, so this is still their tooling and not a
# hand-written config.
mark_slot_a_bootable() {
    log "marking slot A as the boot target (steamos-bootconf set-mode first-boot)"
    run_in_chroot steamos-chroot --no-overlay --disk /dev/nvme0n1 --partset A -- \
        steamos-bootconf set-mode first-boot --image A \
                         --conf-dir /esp/SteamOS/conf --efi-dir /efi
    brc=$?
    if [ "$brc" -ne 0 ]; then
        log "FATAL: steamos-bootconf set-mode first-boot failed ($brc);"
        log "FATAL: the image would install correctly and then hang in steamcl.efi."
        return 1
    fi
    run_in_chroot steamos-chroot --no-overlay --disk /dev/nvme0n1 --partset A -- \
        sh -c 'echo "--- /esp/SteamOS/conf/A.conf ---"; cat /esp/SteamOS/conf/A.conf'
    return 0
}

start_udev

rc=0
for stage in $(echo "$STAGES" | tr ',' ' '); do
    case "$stage" in
    repair)
        # Valve's own installer, unmodified, straight off the repair image.
        # NOPROMPT=1 suppresses zenity (see prompt_step); POWEROFF and
        # REBOOTPROMPT are deliberately left unset so the final prompt_reboot
        # is silent and hits the stubbed systemctl.
        log "=== stage: repair (repair_device.sh all) ==="
        run_in_chroot /home/deck/tools/repair_device.sh all
        rc=$?
        log "repair_device.sh all exited $rc"
        [ "$rc" -eq 0 ] && { mark_slot_a_bootable; rc=$?; }
        ;;
    provision)
        # Part 1 of steamos_boot.sh, run against the freshly installed
        # rootfs-A. `--install-only --root DIR` is documented as safe inside a
        # chroot of a DIFFERENT image (it is what the update hook does), so it
        # runs inside the repair chroot — which is where bash, pacman, chroot
        # and the rest of a real userspace live. The Alpine side is busybox
        # only and could not run it.
        log "=== stage: provision (steamos_boot.sh --install-only) ==="
        provision_setup || { rc=1; break; }
        run_in_chroot \
            NVKVM_PROFILE="$PROFILE" \
            ${NO_COMPAT32:+NVKVM_NO_COMPAT32=1} \
            /run/nvkvm/boot/steamos_boot.sh --install-only --root "$TARGET_MNT" \
                --profile "$PROFILE" --driver-version "$DRIVER_VERSION"
        rc=$?
        log "steamos_boot.sh --install-only exited $rc"
        provision_teardown
        ;;
    *)
        log "unknown stage '$stage'"
        rc=2
        ;;
    esac
    [ "$rc" -eq 0 ] || break
done

if [ "$rc" -ne 0 ] && [ -n "$SHELL_ON_FAIL" ]; then
    log "stage failed (rc=$rc) and nvkvm.shell_on_fail=1 — dropping to a shell"
    setsid cttyhack /bin/sh
fi

finish "$rc"
