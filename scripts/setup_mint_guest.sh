#!/usr/bin/env bash
# setup_mint_guest.sh — build a bootable Linux Mint qcow2 for the nvkvm test VM.
#
# WHY THIS EXISTS (and is not setup_guest.sh)
# -------------------------------------------
# setup_guest.sh consumes an Ubuntu *cloud image* and cloud-init.  Linux Mint
# ships no cloud image at all -- only a live ISO -- so there is nothing for
# cloud-init to seed.  The two ways out are (a) drive Mint's Ubiquity installer
# unattended, which is interactive-first and preseeds badly, or (b) do directly
# what the installer does: copy the live squashfs onto a disk, make it bootable,
# and undo the live-session bits.  This script is (b).  It is fully unattended
# and every step is inspectable, which (a) is not.
#
# Mint matters as a test target because it ships **Cinnamon on X11**, so it
# exercises the X11 path through nvkvm on a distro nobody tailored for this
# project.
#
# The live squashfs already contains linux-image + linux-headers, build-essential,
# grub-pc, mesa-utils and vulkan-tools, so the guest can build the nvkvm module
# and run tests/validate.sh with no extra packages.  Only openssh-server is
# missing and is installed from the network in the chroot.
#
# Usage:  sudo scripts/setup_mint_guest.sh [--iso PATH] [--img PATH] [--size 60G]
#
# Produces a qcow2 that boots under SeaBIOS (msdos label + grub-pc), autologs
# into Cinnamon, and accepts ssh as the user below.

set -euo pipefail

ISO="${ISO:-/srv/iso/linuxmint-22.3-cinnamon-64bit.iso}"
IMG="${IMG:-/opt/nvkvm-guest/mint-22.3.qcow2}"
SIZE="${SIZE:-60G}"
GUEST_USER="${GUEST_USER:-mint}"
GUEST_PASS="${GUEST_PASS:-mint}"
NBD="${NBD:-/dev/nbd1}"          # nbd0 left free for other tooling
ISO_MNT=/mnt/mint-iso
ROOT_MNT=/mnt/mint-root

while [ $# -gt 0 ]; do
    case "$1" in
        --iso)  ISO="$2";  shift 2 ;;
        --img)  IMG="$2";  shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        --user) GUEST_USER="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "must run as root" >&2; exit 1; }
[ -f "$ISO" ] || { echo "ISO not found: $ISO" >&2; exit 1; }

say() { echo "=== $* ==="; }

# ── teardown that runs on every exit path ────────────────────────────────
cleanup() {
    set +e
    for m in dev/pts dev proc sys run; do
        mountpoint -q "$ROOT_MNT/$m" && umount -l "$ROOT_MNT/$m"
    done
    mountpoint -q "$ROOT_MNT" && umount "$ROOT_MNT"
    qemu-nbd --disconnect "$NBD" >/dev/null 2>&1
    mountpoint -q "$ISO_MNT" && umount "$ISO_MNT"
}
trap cleanup EXIT

say "creating $IMG ($SIZE)"
mkdir -p "$(dirname "$IMG")"
rm -f "$IMG"
qemu-img create -f qcow2 "$IMG" "$SIZE" >/dev/null

modprobe nbd max_part=8
qemu-nbd --disconnect "$NBD" >/dev/null 2>&1 || true
qemu-nbd --connect="$NBD" "$IMG"
sleep 1

# msdos + grub-pc, because the test VM boots on SeaBIOS with no OVMF firmware.
say "partitioning"
parted -s "$NBD" mklabel msdos
parted -s "$NBD" mkpart primary ext4 1MiB 100%
parted -s "$NBD" set 1 boot on
sleep 1
partprobe "$NBD" || true
sleep 1
PART="${NBD}p1"
[ -b "$PART" ] || { echo "partition $PART did not appear" >&2; exit 1; }

mkfs.ext4 -q -L mintroot "$PART"
UUID=$(blkid -s UUID -o value "$PART")
say "root partition $PART  UUID=$UUID"

mkdir -p "$ROOT_MNT" "$ISO_MNT"
mount "$PART" "$ROOT_MNT"
mountpoint -q "$ISO_MNT" || mount -o loop,ro "$ISO" "$ISO_MNT"

say "unpacking filesystem.squashfs (~8.4 GB uncompressed, takes a few minutes)"
unsquashfs -f -d "$ROOT_MNT" "$ISO_MNT/casper/filesystem.squashfs" >/dev/null

say "preparing chroot"
mount --bind /dev     "$ROOT_MNT/dev"
mount --bind /dev/pts "$ROOT_MNT/dev/pts"
mount -t proc  proc   "$ROOT_MNT/proc"
mount -t sysfs sysfs  "$ROOT_MNT/sys"
mount -t tmpfs tmpfs  "$ROOT_MNT/run"
# The squashfs ships /etc/resolv.conf as a symlink into /run, which is empty
# here, so it is a DANGLING symlink and cp refuses to write through it.  Swap
# in a real file for the chroot and restore the symlink afterwards, so the
# installed system still gets its resolv.conf from systemd-resolved.
RESOLV_LINK=""
if [ -L "$ROOT_MNT/etc/resolv.conf" ]; then
    RESOLV_LINK=$(readlink "$ROOT_MNT/etc/resolv.conf")
    rm -f "$ROOT_MNT/etc/resolv.conf"
fi
cp -f /etc/resolv.conf "$ROOT_MNT/etc/resolv.conf"

cat > "$ROOT_MNT/etc/fstab" <<EOF
UUID=$UUID  /  ext4  errors=remount-ro  0  1
EOF

# grub needs an explicit map: it cannot guess that /dev/nbdN is (hd0).
mkdir -p "$ROOT_MNT/boot/grub"
cat > "$ROOT_MNT/boot/grub/device.map" <<EOF
(hd0) $NBD
EOF

cat > "$ROOT_MNT/tmp/inchroot.sh" <<EOF
set -eux
export DEBIAN_FRONTEND=noninteractive

# Undo the live session.  casper/ubiquity own the live boot path; leaving them
# installed leaves an installer icon on the desktop of a system that is already
# installed, and casper's initramfs hooks look for a squashfs that is not there.
apt-get purge -y ubiquity ubiquity-casper ubiquity-frontend-gtk \
    ubiquity-slideshow-mint ubiquity-ubuntu-artwork casper mint-live-session \
    2>/dev/null || true
apt-get -y autoremove --purge || true

# ssh is the only thing the live image lacks that this harness needs.
apt-get update
apt-get install -y --no-install-recommends openssh-server

# A real user, replacing the live 'mint' autologin account.
id -u $GUEST_USER >/dev/null 2>&1 || adduser --disabled-password --gecos "" $GUEST_USER
echo "$GUEST_USER:$GUEST_PASS" | chpasswd
usermod -aG sudo,adm,video,render $GUEST_USER
echo "$GUEST_USER ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/90-$GUEST_USER
chmod 440 /etc/sudoers.d/90-$GUEST_USER

# Autologin straight into Cinnamon: the point of this guest is a desktop on the
# physical display, and a greeter asking for a password is in the way.
mkdir -p /etc/lightdm/lightdm.conf.d
cat > /etc/lightdm/lightdm.conf.d/90-autologin.conf <<'LD'
[Seat:*]
autologin-user=$GUEST_USER
autologin-user-timeout=0
user-session=cinnamon
LD
systemctl enable lightdm  || true
systemctl enable ssh      || true
systemctl set-default graphical.target || true

echo "mint-nvkvm" > /etc/hostname
sed -i 's/^127.0.1.1.*/127.0.1.1\tmint-nvkvm/' /etc/hosts || echo "127.0.1.1 mint-nvkvm" >> /etc/hosts

# Serial console kept on: a guest that fails before X is only debuggable there.
sed -i 's#^GRUB_CMDLINE_LINUX_DEFAULT=.*#GRUB_CMDLINE_LINUX_DEFAULT="console=tty0 console=ttyS0,115200"#' /etc/default/grub
sed -i 's/^GRUB_TIMEOUT=.*/GRUB_TIMEOUT=2/' /etc/default/grub
sed -i 's/^GRUB_TIMEOUT_STYLE=.*/GRUB_TIMEOUT_STYLE=menu/' /etc/default/grub

# The test VM runs with -vga none: the guest display comes from the nvkvm
# present device, which only lights up once the guest KMS driver has loaded, so
# GRUB has NO video device at boot.  Mint leaves GRUB_TERMINAL unset, which
# makes 00_header emit "terminal_output gfxterm"; GRUB then stalls in video init
# before it ever loads a kernel, with nothing on any console to say so -- the
# guest just sits there with an empty serial log.  Pin GRUB to serial.
#
# This goes in grub.d rather than /etc/default/grub because Mint ships
# 50_linuxmint.cfg, which is sourced AFTER /etc/default/grub and sets
# GRUB_DISABLE_OS_PROBER=false; a value written to /etc/default/grub loses.
cat > /etc/default/grub.d/99-nvkvm.cfg <<'NVK'
GRUB_TERMINAL=serial
GRUB_SERIAL_COMMAND="serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1"
# os-prober here enumerates the HOST disks that qemu-nbd exposed during the
# build and adds menu entries for filesystems the guest will never see.
GRUB_DISABLE_OS_PROBER=true
NVK

grub-install --target=i386-pc --boot-directory=/boot --modules="part_msdos ext2" $NBD
update-grub
update-initramfs -u -k all
EOF

say "running chroot stage"
chroot "$ROOT_MNT" /bin/bash /tmp/inchroot.sh
rm -f "$ROOT_MNT/tmp/inchroot.sh"
if [ -n "$RESOLV_LINK" ]; then
    rm -f "$ROOT_MNT/etc/resolv.conf"
    ln -s "$RESOLV_LINK" "$ROOT_MNT/etc/resolv.conf"
fi

say "done: $IMG"
