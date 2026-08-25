#!/usr/bin/env bash
# nvkvm-recovery.sh — the IN-IMAGE half of the nvkvm SteamOS integration.
#
# Everything here must work with NO 9p share, NO network and NO nvkvm module,
# because the entire point of keeping it in the image is that it still runs when
# the share is the thing that is broken. It therefore has no dependency on
# /mnt/nvkvm and never sources anything from it.
#
# Subcommands:
#   boot      Fallback entry point when /mnt/nvkvm/boot/steamos_boot.sh is absent.
#             Diagnoses WHY the share is missing, then continues degraded.
#   plant     Provision nvkvm into the newly staged SteamOS image (update hook).
#   validate  Local nvkvm health check; no share required. NOT a desktop check
#             -- see the comment above validate() for what it can and cannot see.
#   menu      Interactive recovery menu.

set -uo pipefail

SHARE_TAG="${NVKVM_SHARE_TAG:-nvkvm}"
SHARE_MNT="${NVKVM_SHARE_MNT:-/run/nvkvm}"   # /mnt is a symlink to var/mnt in SteamOS; /var is reformatted on update
MODULE_NAME=nvkvm-guest
COUNTDOWN=30

log()  { printf '[nvkvm-recovery] %s\n' "$*" >&2; }   # stderr: keep stdout clean for return values
err()  { printf '[nvkvm-recovery] ERROR: %s\n' "$*" >&2; }

# ── The specific-diagnosis requirement ───────────────────────────────────────
# "the share is missing" has several very different causes with very different
# fixes; saying which one it is turns a 40-minute debug into a 2-minute one.
diagnose_9p() {
    echo
    err "The nvkvm 9p share is not usable at $SHARE_MNT."
    echo

    if ! grep -qw 9p /proc/filesystems 2>/dev/null; then
        if ! modprobe 9p 2>/dev/null || ! modprobe 9pnet_virtio 2>/dev/null; then
            err "CAUSE: the guest kernel has no 9p support and the modules will not load."
            err "FIX:   this SteamOS kernel is missing 9p/9pnet_virtio. Use virtio-9p"
            err "       drivers from the same kernel, or switch the share to virtiofs."
            return
        fi
    fi

    if [ ! -e /sys/bus/virtio/drivers/9pnet_virtio ]; then
        err "CAUSE: the 9p virtio driver is present but QEMU exported no 9p device."
        err "FIX:   the VM was started WITHOUT the share. Add to the QEMU command line:"
        err "         -virtfs local,path=/path/to/nvkvm-pv,mount_tag=$SHARE_TAG,security_model=none,readonly=on"
        return
    fi

    # A device exists -- so the tag is probably wrong.
    if ! mount -t 9p -o trans=virtio,version=9p2000.L,ro "$SHARE_TAG" "$SHARE_MNT" 2>/dev/null; then
        err "CAUSE: a 9p device exists but mounting tag '$SHARE_TAG' failed."
        err "FIX:   the mount_tag in the QEMU command line does not match '$SHARE_TAG'."
        err "       Check 'mount_tag=' in the -virtfs/-fsdev argument, or set"
        err "       NVKVM_SHARE_TAG in /etc/default/nvkvm to whatever the host uses."
        return
    fi

    # It mounted after all -- so the tag is fine and the content is wrong.
    if [ ! -x "$SHARE_MNT/boot/steamos_boot.sh" ]; then
        err "CAUSE: the share mounted, but $SHARE_MNT/boot/steamos_boot.sh is missing"
        err "       or not executable."
        err "FIX:   the host is sharing the wrong directory. It must be the root of a"
        err "       nvkvm-pv checkout that contains boot/steamos_boot.sh."
    fi
}

# The in-image half owns MOUNTING the share; the share half owns the logic. The
# stub unit cannot test for /run/nvkvm/boot/steamos_boot.sh directly, because at
# that point nothing has mounted anything there yet.
mount_share() {
    mountpoint -q "$SHARE_MNT" 2>/dev/null && return 0
    mkdir -p "$SHARE_MNT"
    modprobe 9pnet_virtio 2>/dev/null || true
    mount -t 9p -o trans=virtio,version=9p2000.L,ro,msize=512000 \
          "$SHARE_TAG" "$SHARE_MNT" 2>/dev/null
}

do_boot() {
    if grep -qw 'nvkvm.skip=1' /proc/cmdline 2>/dev/null; then
        log "nvkvm.skip=1 -- skipping nvkvm"
        exit 0
    fi
    if mount_share && [ -x "$SHARE_MNT/boot/steamos_boot.sh" ]; then
        log "share mounted -- handing over to $SHARE_MNT/boot/steamos_boot.sh"
        exec "$SHARE_MNT/boot/steamos_boot.sh" boot
    fi
    diagnose_9p

    # If the diagnosis ended up mounting a good share after all, use it.
    if [ -x "$SHARE_MNT/boot/steamos_boot.sh" ]; then
        log "share is usable after all -- handing over"
        exec "$SHARE_MNT/boot/steamos_boot.sh" boot
    fi

    # Degrade gracefully: try what we can locally, then continue (design note 4).
    modprobe "$MODULE_NAME" 2>/dev/null
    if validate; then
        log "nvkvm itself checks out: module loaded and not faulting, /dev/nvidiactl"
        log "opens, userspace installed. The share is only needed to UPDATE it."
        [ -n "$VALIDATE_UNVERIFIED" ] && err "could NOT verify:$VALIDATE_UNVERIFIED"
        log "NOT CHECKED: whether the desktop starts -- nothing here can see a compositor."
        exit 0
    fi
    err "Continuing WITHOUT nvkvm -- the desktop will start on the emulated VGA."
    err "Run 'nvkvm-recovery.sh menu' once logged in."
    local i
    for i in $(seq $COUNTDOWN -1 1); do
        printf '\r[nvkvm-recovery] continuing in %2ds ' "$i"; sleep 1
    done
    printf '\n'
    exit 0
}

# WHAT THIS CHECKS, AND WHAT IT CANNOT.
#
# It checks that nvkvm is loaded, is not faulting, that its device node can
# actually be OPENED, and that the userspace the machine needs is installed.
#
# It does NOT check the desktop, and it cannot: nothing here starts, watches or
# can even see a compositor. MEASURED, and the reason this comment is here: a
# validator in this project printed a success line while kwin was crash-looping
# and no desktop existed, and on another boot while the guest module was oopsing
# on every open of the device. Callers must report what was verified and name
# what was not.
#
# 0 ok | 2 protocol mismatch | 3 module will not load | 4 userspace broken
# 5 loaded but not usable | 6 no DRM node bound to the nvidia driver
VALIDATE_UNVERIFIED=""

# The one operation every GL/CUDA client performs first. `[ -e ]` proves a node
# exists; it proves nothing about opening it, and "the node is there" was true
# on the boot where every open took the kernel down. Subshell, so a failed
# redirection cannot take this script with it.
can_open_device() { ( exec 9<"$1" ) 2>/dev/null; }

validate() {
    VALIDATE_UNVERIFIED=""
    local d dmesg_ok=1
    d="$(dmesg 2>/dev/null)" || dmesg_ok=0
    [ -n "$d" ] || dmesg_ok=0
    if [ "$dmesg_ok" = 0 ]; then
        VALIDATE_UNVERIFIED="$VALIDATE_UNVERIFIED
  - the kernel log is unreadable, so the protocol-mismatch and module-fault
    checks did not actually run"
    fi

    if printf '%s\n' "$d" | grep -q 'protocol version mismatch'; then
        err "CLASS 2: nvkvm PROTOCOL VERSION MISMATCH"
        printf '%s\n' "$d" | grep 'protocol version mismatch' | tail -2 >&2
        err "guest module and host QEMU came from different nvkvm-pv commits;"
        err "rebuild both from the same checkout (that is what the 9p share is)."
        return 2
    fi
    if ! grep -qw "${MODULE_NAME//-/_}" /proc/modules 2>/dev/null; then
        err "CLASS 3: $MODULE_NAME is not loaded (built, but the kernel refused it,"
        err "or nvkvm's virtio device is missing from the QEMU command line)."
        printf '%s\n' "$d" | grep -i nvkvm | tail -3 >&2
        return 3
    fi
    # An oops or WARN inside the module annotates its frames `[nvkvm_guest]`;
    # `Modules linked in:` prints the bare name, so this does not match a
    # bystander. A fault marker is required as well.
    if [ "$dmesg_ok" = 1 ] \
       && printf '%s\n' "$d" | grep -qE '\[nvkvm_guest[] ]' \
       && printf '%s\n' "$d" | grep -qE 'BUG:|Oops:|kernel BUG at|general protection fault|WARNING: CPU'; then
        err "CLASS 5: the kernel FAULTED inside $MODULE_NAME -- loaded, and broken."
        printf '%s\n' "$d" | grep -E '\[nvkvm_guest[] ]|BUG:|Oops:' | tail -6 >&2
        return 5
    fi
    [ -e /dev/nvidiactl ] || { err "CLASS 4: loaded but /dev/nvidiactl missing"; return 4; }
    if ! can_open_device /dev/nvidiactl; then
        err "CLASS 5: /dev/nvidiactl exists but cannot be OPENED -- the first thing"
        err "every GL/CUDA client does fails. Usually nvkvm's virtio device is absent"
        err "from the QEMU command line (look for 'host GPU discovery failed')."
        printf '%s\n' "$d" | grep -i nvkvm | tail -5 >&2
        return 5
    fi
    # Profile-aware: a trimmed (steamos) profile has no libcuda by design, so
    # checking for it would report a healthy machine as broken. Check the
    # Vulkan/GL stack instead.
    if ldconfig -p 2>/dev/null | grep -q libcuda; then
        nvidia-smi -L >/dev/null 2>&1 || { err "CLASS 4: nvidia-smi cannot enumerate a GPU"; return 4; }
    else
        ldconfig -p 2>/dev/null | grep -q libGLX_nvidia \
            || { err "CLASS 4: libGLX_nvidia missing (Vulkan/GL stack incomplete)"; return 4; }
        # Loader searches /etc AND /usr/share; nvidia-installer writes to /etc.
        [ -e /etc/vulkan/icd.d/nvidia_icd.json ] || [ -e /usr/share/vulkan/icd.d/nvidia_icd.json ] \
            || { err "CLASS 4: NVIDIA Vulkan ICD manifest missing from both loader paths"; return 4; }
        # A compositor opens a DRM node, not /dev/nvidiactl. If none is bound to
        # the nvidia driver, the session cannot be on nvkvm however healthy the
        # rest looks. A prerequisite for the desktop -- not a check OF it.
        local c drv found=0
        for c in /sys/class/drm/card[0-9]*; do
            [ -e "$c/device/driver" ] || continue
            drv="$(basename "$(readlink -f "$c/device/driver")" 2>/dev/null)"
            [ "$drv" = nvidia ] && { found=1; break; }
        done
        [ "$found" = 1 ] || { err "CLASS 6: no /dev/dri node bound to the nvidia driver"; return 6; }
    fi
    return 0
}

# ── Update hook: provision the newly staged image ────────────────────────────
# Runs on the OLD, still-running system, where the 9p share IS mounted. Mounts
# the other slot and runs Part 1 against it with ROOT pointing at the new image,
# so the kernel module gets built against the NEW image's kernel.
do_plant() {
    mount_share || true
    if [ ! -x "$SHARE_MNT/boot/steamos_boot.sh" ]; then
        err "cannot provision the new image: share not available. The new image will"
        err "self-repair on its first boot instead (that is the design)."
        exit 0
    fi
    local other=/dev/disk/by-partsets/other/rootfs
    if [ ! -e "$other" ]; then
        err "no 'other' partset rootfs found -- is this an A/B install?"
        err "NOTE: the repair image is single-slot, so this path cannot be tested there."
        exit 0
    fi
    local mnt; mnt="$(mktemp -d /tmp/nvkvm-newroot.XXXXXX)"
    mount "$other" "$mnt" || { err "could not mount $other"; exit 0; }
    btrfs property set "$mnt" ro false 2>/dev/null
    mount --bind /proc "$mnt/proc"; mount --bind /sys "$mnt/sys"; mount --bind /dev "$mnt/dev"
    # The new image needs the share visible at the same path inside its chroot.
    mkdir -p "$mnt$SHARE_MNT"; mount --bind "$SHARE_MNT" "$mnt$SHARE_MNT"

    "$SHARE_MNT/boot/steamos_boot.sh" --install-only --root "$mnt"
    local rc=$?

    umount "$mnt$SHARE_MNT" 2>/dev/null
    umount "$mnt/proc" "$mnt/sys" "$mnt/dev" 2>/dev/null
    btrfs property set "$mnt" ro true 2>/dev/null
    umount "$mnt" 2>/dev/null; rmdir "$mnt" 2>/dev/null
    log "new-image provisioning finished (rc=$rc)"
    # Always exit 0: never let this affect anything upstream (design note 3).
    exit 0
}

do_menu() {
    while true; do
        echo
        echo "  nvkvm recovery"
        echo "  =============="
        # "HEALTHY" used to be printed here for a 0 from validate(), which
        # says nothing at all about the desktop. Say what was checked.
        if validate; then echo "  nvkvm: OK (module + device + userspace; the DESKTOP is NOT checked)"
        else                   echo "  nvkvm: NOT WORKING (class above)"; fi
        echo "   1) show diagnosis for the 9p share"
        echo "   2) re-run provisioning now"
        echo "   3) show nvkvm kernel log"
        echo "   4) validate (reports which of the three failure classes)"
        echo "   5) show nvkvm protocol negotiation lines"
        echo "   q) quit"
        read -rp "  > " a
        case "$a" in
            1) diagnose_9p ;;
            2) [ -x "$SHARE_MNT/boot/steamos_boot.sh" ] && "$SHARE_MNT/boot/steamos_boot.sh" --install-only || err "share unavailable" ;;
            3) journalctl -k -g nvkvm --no-pager | tail -40 ;;
            4) validate; case $? in
                 0) echo "  nvkvm OK -- and the desktop was NOT checked";;
                 2) echo "  FAILED: protocol mismatch";;
                 3) echo "  FAILED: module will not load";;
                 4) echo "  FAILED: userspace broken";;
                 5) echo "  FAILED: module loaded but not usable (faulted, or the device will not open)";;
                 6) echo "  FAILED: no DRM node bound to the nvidia driver";;
               esac ;;
            5) dmesg 2>/dev/null | grep -iE "nvkvm.*(protocol|negotiat)" | tail -5 ;;
            q|Q) return 0 ;;
        esac
    done
}

case "${1:-menu}" in
    boot)     do_boot ;;
    plant)    do_plant ;;
    validate) validate; exit $? ;;
    menu)     do_menu ;;
    *) echo "usage: $0 {boot|plant|validate|menu}"; exit 2 ;;
esac
