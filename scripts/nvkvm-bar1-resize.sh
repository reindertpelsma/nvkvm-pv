#!/bin/sh
# Enlarge the NVIDIA BAR1 aperture at boot.
#
# WHY: BAR1 is an MMU-translated aperture with an allocator (eheap) over its
# address space, and nvkvm leaks BAR1 VA that is only reclaimed by tearing the
# driver down. At the pre-Resizable-BAR default of 256 MiB that becomes a host
# denial of service within an afternoon of testing. This does not fix the leak;
# it buys 32x the headroom so the leak stops being an outage.
#
# This is a RUNTIME resize (PCI config write), not a firmware change: it needs
# no BIOS setting and does not persist by itself, which is why this runs at boot.
# The kernel places the enlarged BAR above 4G on its own.
set -eu

TARGET_EXP="${NVKVM_BAR1_EXP:-13}"          # 2^13 MB = 8 GiB
TARGET_BYTES=$(( 1 << (TARGET_EXP + 20) ))

# shellcheck disable=SC2046  # word splitting is the point: one iteration per device
for dev in $(lspci -D -n -d 10de::0300 2>/dev/null | cut -d' ' -f1); do
    rf="/sys/bus/pci/devices/$dev/resource1_resize"
    [ -e "$rf" ] || { echo "$dev: no resource1_resize (kernel or device lacks ReBAR)"; continue; }

    # Region 1 is the SECOND line of `resource`: "start end flags", in hex.
    # Do NOT use awk strtonum() here -- that is a gawk extension and Ubuntu's
    # awk is mawk, where it silently yields 0, which made this script think the
    # BAR was 0 MiB and unload the driver on a box that needed no resize at all.
    # POSIX shell arithmetic understands the 0x prefix directly.
    # shellcheck disable=SC2046  # deliberate: split "start end flags" into $1 $2 $3
    set -- $(sed -n 2p "/sys/bus/pci/devices/$dev/resource" 2>/dev/null)
    cur=$(( ${2:-0} - ${1:-0} + 1 ))
    if [ "$cur" -ge "$TARGET_BYTES" ] 2>/dev/null; then
        echo "$dev: BAR1 already $((cur / 1024 / 1024)) MiB, nothing to do"
        continue
    fi

    # The resize is refused while a driver holds the device, so drop the NVIDIA
    # stack first. Order matters: drm/modeset depend on nvidia.
    loaded=0
    for m in nvidia_drm nvidia_modeset nvidia_uvm nvidia; do
        if lsmod | grep -q "^$m "; then loaded=1; rmmod "$m" 2>/dev/null || true; fi
    done

    if echo "$TARGET_EXP" > "$rf" 2>/dev/null; then
        # shellcheck disable=SC2046  # deliberate, as above
        set -- $(sed -n 2p "/sys/bus/pci/devices/$dev/resource")
        new=$(( $2 - $1 + 1 ))
        echo "$dev: BAR1 $((cur / 1024 / 1024)) MiB -> $((new / 1024 / 1024)) MiB"
    else
        echo "$dev: resize to $((TARGET_BYTES / 1024 / 1024)) MiB REFUSED -- no MMIO space above 4G, or firmware forbids it. Left at $((cur / 1024 / 1024)) MiB."
    fi

    # Put the driver back if we took it away. Failing to reload would leave the
    # box headless, which is worse than a small BAR.
    [ "$loaded" = 1 ] && modprobe nvidia_drm modeset=1 2>/dev/null || true
done
