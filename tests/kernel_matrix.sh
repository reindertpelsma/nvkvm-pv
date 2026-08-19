#!/usr/bin/env bash
#
# tests/kernel_matrix.sh — does nvkvm-guest.ko build on other kernels?
#
# The guest module is built against the guest's own kernel headers, so the
# distro does not matter and the kernel version does.  This compiles the module
# against several distros' kernels, each with that distro's own toolchain, in
# throwaway Docker containers.  No GPU, no VM, no root on a target machine.
#
#   bash tests/kernel_matrix.sh                  # the default image set
#   bash tests/kernel_matrix.sh debian:13 fedora:42
#
# A compile FAILURE is conclusive: the module cannot be built there.  A compile
# PASS is necessary but not sufficient — it says nothing about whether the
# module loads or works, which needs a booted guest on that kernel.
#
# Results table: docs/reference/guest-kernels.md
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

IMAGES=("$@")
if [ ${#IMAGES[@]} -eq 0 ]; then
    IMAGES=(ubuntu:22.04 debian:12 ubuntu:24.04 debian:13 ubuntu:25.04 fedora:42)
fi

cat > "$WORK/harness.sh" <<'INNER'
#!/bin/bash
set -u
export DEBIAN_FRONTEND=noninteractive

if command -v apt-get >/dev/null 2>&1; then
    apt-get update -qq >/dev/null 2>&1
    apt-get install -y -q build-essential >/dev/null 2>&1
    apt-get install -y -q linux-headers-amd64 >/dev/null 2>&1 || \
    apt-get install -y -q linux-headers-generic >/dev/null 2>&1
elif command -v dnf >/dev/null 2>&1; then
    dnf install -y -q gcc make kernel-devel elfutils-libelf-devel >/dev/null 2>&1
elif command -v pacman >/dev/null 2>&1; then
    pacman -Sy --noconfirm --quiet base-devel linux-headers >/dev/null 2>&1
fi

KDIR=""
for c in /usr/src/linux-headers-*-generic /usr/src/linux-headers-*-amd64 \
         /usr/src/kernels/* /usr/lib/modules/*/build /usr/src/linux-headers-*; do
    [ -d "$c" ] && [ -f "$c/Makefile" ] && KDIR="$c" && break
done
[ -z "$KDIR" ] && { echo "RESULT|${DISTRO_TAG}|?|SKIP|no kernel headers in this image"; exit 0; }

KVER=$(make -s -C "$KDIR" kernelversion 2>/dev/null || basename "$KDIR")
GCCV=$(gcc -dumpversion 2>/dev/null || echo '?')

# Preserve the src/ layout: the module's includes are ../../src/{common,abi}.
mkdir -p /build/src && cp -r /src/. /build/src/ || exit 1

for G in 1 0; do
    OUT=$(make -C /build/src/guest KDIR="$KDIR" NVKVM_GRAPHICS=$G -j"$(nproc)" 2>&1)
    if [ -f /build/src/guest/nvkvm-guest.ko ]; then
        echo "RESULT|${DISTRO_TAG}|${KVER}|PASS|gcc ${GCCV}, graphics=${G}"
    else
        E=$(echo "$OUT" | grep -E 'error:' | head -1 | sed 's/|/ /g' | cut -c1-160)
        [ -z "$E" ] && E=$(echo "$OUT" | tail -1 | cut -c1-160)
        echo "RESULT|${DISTRO_TAG}|${KVER}|FAIL|graphics=${G}: ${E}"
    fi
    make -C /build/src/guest KDIR="$KDIR" clean >/dev/null 2>&1
done
INNER
chmod +x "$WORK/harness.sh"

printf '%-22s %-14s %-6s %s\n' DISTRO KERNEL BUILD DETAIL
printf '%-22s %-14s %-6s %s\n' ---------------------- -------------- ------ ------
rc=0
for img in "${IMAGES[@]}"; do
    out=$(timeout 1800 docker run --rm \
            -v "$REPO_ROOT/src:/src:ro" \
            -v "$WORK/harness.sh:/harness.sh:ro" \
            -e "DISTRO_TAG=$img" \
            "$img" bash /harness.sh 2>/dev/null | grep '^RESULT|')
    if [ -z "$out" ]; then
        printf '%-22s %-14s %-6s %s\n' "$img" "?" "ERROR" "container did not run"
        rc=1
        continue
    fi
    while IFS='|' read -r _ tag kver verdict detail; do
        printf '%-22s %-14s %-6s %s\n' "$tag" "$kver" "$verdict" "$detail"
        [ "$verdict" = FAIL ] && rc=1
    done <<< "$out"
done
exit $rc
