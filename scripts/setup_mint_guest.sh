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
# weston is the second: see the session block below for why Cinnamon's own
# session cannot drive the nvkvm head yet.
apt-get update
apt-get install -y --no-install-recommends openssh-server weston

# A real user, replacing the live 'mint' autologin account.
id -u $GUEST_USER >/dev/null 2>&1 || adduser --disabled-password --gecos "" $GUEST_USER
echo "$GUEST_USER:$GUEST_PASS" | chpasswd
usermod -aG sudo,adm,video,render $GUEST_USER
echo "$GUEST_USER ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/90-$GUEST_USER
chmod 440 /etc/sudoers.d/90-$GUEST_USER

# ── Graphical session ────────────────────────────────────────────────────
# NOT lightdm, and NOT Cinnamon's own session.  Both are blocked today:
#
#   * lightdm's GREETER is itself an Xorg server on the nvkvm head, so it hits
#     the glamor bug (drmmode_set_pixmap_bo -> glamor_egl_create_textured_
#     pixmap_from_gbm_bo -> "Failed to create pixmap" -> "failed to create
#     screen resources") and dies before any session starts.  Left enabled it
#     restart-loops indefinitely.
#
#   * Cinnamon's own Wayland session (cinnamon-wayland.desktop) gets much
#     further -- muffin brings up KMS on the nvkvm head fine -- but then calls
#     drmModeSetCursor, which fails with ENXIO because nvkvm exposes no cursor
#     plane, and muffin SEGVs inside its own disable_hw_cursor_for_crtc()
#     fallback.  See docs/internal/mint-guest-desktop.md.
#
# weston drives the same head, composites its own cursor, and is the path this
# repo already exercises.  With --xwayland every Mint GTK app runs on it.
mkdir -p /etc/systemd/system/getty@tty1.service.d
cat > /etc/systemd/system/getty@tty1.service.d/autologin.conf <<AL
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin $GUEST_USER --noclear %I \$TERM
AL

# Swappable so the Cinnamon session can be retried without re-editing the
# login path once the cursor-plane gap is closed.
cat > /home/$GUEST_USER/run-session.sh <<'RS'
#!/bin/bash
# Session launcher for VT1.  ~/session-choice picks which one:
#   cinnamon-x11  (default) - the real Mint Cinnamon desktop; see below
#   weston                  - bare weston + Xwayland; the control path
#   cinnamon                - Cinnamon's own Wayland session (crashes, see docs)
exec > ~/session.log 2>&1
echo "=== \$(date) session: \$(cat ~/session-choice 2>/dev/null) ==="
export XDG_SESSION_TYPE=wayland

# Pick the nvkvm card by DRIVER, never by index.  The VM boots with an emulated
# VGA present so GRUB and early kernel messages have somewhere to render, which
# means the guest has TWO DRM devices and bochs-drm usually enumerates first as
# card0.  A compositor that just takes card0 lands on bochs-drm and silently
# renders with llvmpipe: full speed, correct-looking screenshots, no GPU.
NVCARD=
for c in /sys/class/drm/card[0-9]*; do
    [ -e "\$c/device/driver" ] || continue
    if [ "\$(basename "\$(readlink -f "\$c/device/driver")")" = nvidia ]; then
        NVCARD=\$(basename "\$c"); break
    fi
done
echo "nvkvm DRM device: \${NVCARD:-NOT FOUND}"

# --idle-time=0 is load-bearing for an unattended head, and its absence does NOT
# look like a timeout.  weston's default is 300s of no INPUT -- animating clients
# do not count -- after which weston-desktop-shell LOCKS the session, draws its
# own "Unlock your desktop" dialog and stops repainting.  The nvkvm scanout then
# holds that last frame forever: screendump returns a plausible desktop that is
# byte-identical every time and reads exactly like a dead present path.
start_weston() {
    weston --backend=drm \${NVCARD:+--drm-device=\$NVCARD} --idle-time=0 "\$@" &
    WESTON_PID=\$!
    for _ in \$(seq 1 60); do
        [ -S "\${XDG_RUNTIME_DIR:-/run/user/\$(id -u)}/wayland-1" ] && return 0
        sleep 0.5
    done
    echo "weston socket never appeared"; return 1
}

case "\$(cat ~/session-choice 2>/dev/null)" in
  weston)   exec weston --backend=drm \${NVCARD:+--drm-device=\$NVCARD} \
                        --xwayland --idle-time=0 ;;
  cinnamon) exec cinnamon-session-cinnamon --wayland ;;
  *)
    # The real Mint desktop: Cinnamon's X11 session -- the one a stock Mint
    # install boots into -- inside a ROOTFUL Xwayland that is itself a client of
    # the weston session driving the nvkvm KMS head.
    #
    # Rootful is the trick.  weston's own --xwayland is ROOTLESS: X clients
    # become individual weston windows and there is no X root window for a
    # window manager to own, which is why running Cinnamon against it renders a
    # black box.  A rootful Xwayland owns a real root window, so a complete X
    # session (muffin + panel + desktop) runs inside it as it would on Xorg.
    #
    # This needs no Xorg, no modesetting driver and no glamor pixmap import --
    # which is what makes it work at all, since NVIDIA's EGL refuses the
    # EGL_NATIVE_PIXMAP_KHR import that Xorg's modesetting+glamor requires.
    start_weston || exit 1
    ~/start-cinnamon-x11.sh &
    wait \$WESTON_PID
    ;;
esac
RS

cat > /home/$GUEST_USER/start-cinnamon-x11.sh <<'CX'
#!/bin/bash
# Rootful Xwayland on the running weston session + Cinnamon's X11 session inside
# it.  Invoked by run-session.sh; safe to run by hand too.
exec > ~/cinnamon-x11.log 2>&1
set -x
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"

pkill -f "Xwayland :2" 2>/dev/null
sleep 1

Xwayland :2 -fullscreen -geometry 1920x1080 &
for _ in $(seq 1 60); do
    [ -S /tmp/.X11-unix/X2 ] && break
    sleep 0.5
done
[ -S /tmp/.X11-unix/X2 ] || { echo "Xwayland :2 never came up"; exit 1; }

export PATH="$HOME/bin:$PATH"     # for the cinnamon --x11 shim, see ~/bin/cinnamon
export DISPLAY=:2
export XDG_SESSION_TYPE=x11
export XDG_CURRENT_DESKTOP=X-Cinnamon
export XDG_SESSION_DESKTOP=cinnamon
export DESKTOP_SESSION=cinnamon
unset WAYLAND_DISPLAY             # so the session does not start as a wayland one

# Prove the X server is on the GPU before putting a desktop on it.  A silent
# llvmpipe fallback here would look completely normal in a screenshot.
glxinfo -B 2>&1 | grep -iE "vendor string|renderer string|OpenGL version" | head -3

exec cinnamon-session-cinnamon
CX

# muffin asks logind what session it is in.  Launched from the VT1 seat session
# it sees a seat with a VT, picks the NATIVE (KMS) backend, and then dies on
# TakeControl because weston -- which is driving that very KMS head -- already
# holds the seat:
#     Failed to create backend: Could not take control: ... EBUSY
# The session then comes up with no shell at all: no panel, no wallpaper, just
# whatever autostart apps mapped.  Note logind resolves a process to a session
# by CGROUP, so unsetting XDG_SESSION_ID/XDG_SEAT/XDG_VTNR does not help, and
# neither does a systemd --user scope.
#
# This session genuinely IS an X11 session -- it runs inside a rootful Xwayland
# that is a weston client -- so --x11 tells muffin the truth rather than
# overriding it.  cinnamon-launcher execvp()s "cinnamon" by PATH, so a shim
# ahead of /usr/bin is enough and nothing Mint ships is modified.
mkdir -p /home/$GUEST_USER/bin
cat > /home/$GUEST_USER/bin/cinnamon <<'SHIM'
#!/bin/sh
exec /usr/bin/cinnamon --x11 "$@"
SHIM
chmod +x /home/$GUEST_USER/bin/cinnamon
chmod +x /home/$GUEST_USER/run-session.sh /home/$GUEST_USER/start-cinnamon-x11.sh
echo cinnamon-x11 > /home/$GUEST_USER/session-choice

# The guard is NOT optional.  /usr/bin/cinnamon-session is a shell wrapper that,
# for a wayland session, re-execs itself through a LOGIN shell:
#     exec bash -c "exec -l '$SHELL' -c '$0 -l $*'"
# so the session inherits the user's login environment.  That login shell
# re-reads .bash_profile -- which would start the session again.  Without the
# guard that is an infinite exec chain: it spins at 100% CPU, never spawns a
# compositor, and prints absolutely nothing (symptom: a 0-byte session log).
# The guard survives because exec preserves the environment.
cat > /home/$GUEST_USER/.bash_profile <<'BP'
[ -f ~/.bashrc ] && . ~/.bashrc
if [ -z "$NVKVM_SESSION_LAUNCHED" ] && [ -z "$WAYLAND_DISPLAY" ] && [ "$XDG_VTNR" = 1 ]; then
  export NVKVM_SESSION_LAUNCHED=1
  exec ~/run-session.sh
fi
BP
chown -R $GUEST_USER:$GUEST_USER /home/$GUEST_USER/run-session.sh \
      /home/$GUEST_USER/start-cinnamon-x11.sh /home/$GUEST_USER/bin \
      /home/$GUEST_USER/session-choice /home/$GUEST_USER/.bash_profile

# Build and load the guest module at boot.  RequiresMountsFor is load-bearing:
# the 9p fstab entries are `nofail`, which removes them from local-fs.target's
# dependency set, so ordering After=local-fs.target guarantees nothing and the
# unit loses the race on reboot -- leaving the guest with no DRM node and no
# GPU.  Same bug, same fix as candidate 67a18e3 on the Ubuntu guest.
cat > /etc/systemd/system/nvkvm-guest.service <<'NG'
[Unit]
Description=Build and load the nvkvm guest module
After=local-fs.target
RequiresMountsFor=/mnt/nvkvm
Before=getty@tty1.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c 'lsmod | grep -q nvkvm_guest && exit 0; modprobe drm_shmem_helper 2>/dev/null; cd /mnt/nvkvm/src/guest && make KDIR=/lib/modules/$(uname -r)/build && insmod ./nvkvm-guest.ko'
# Nodes created by a late insmod are root:root 0600 with no by-path links --
# they miss the boot-time uevent flow.  Without this trigger no unprivileged
# compositor can open the card.
ExecStart=/bin/bash -c 'udevadm trigger --subsystem-match=drm; udevadm settle'

[Install]
WantedBy=multi-user.target
NG

systemctl disable lightdm 2>/dev/null || true
systemctl enable nvkvm-guest || true
systemctl enable ssh         || true
systemctl set-default multi-user.target || true

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
