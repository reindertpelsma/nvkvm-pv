#!/usr/bin/env bash
#
# tests/qemu_syntax_check.sh — compile-check src/qemu/ in about two seconds.
#
# WHY THIS EXISTS
#     4edada7: nvkvm_req_kill_isolate() called nvkvm_mapva_forget_isolate()
#     ~1000 lines above that function's forward declaration.  Older toolchains
#     only warn about an implicit declaration; GCC 14 promotes it to a hard
#     error, so `candidate` simply did not compile there -- and nobody found
#     out until someone built it on a box with a new compiler.
#
#     Nothing cheap caught it.  The unit suite does not compile
#     nvkvm_isolate_handlers.c (it links nvkvm_isolate.c, not the handlers),
#     and the only other thing that touches this file is scripts/build_qemu.sh,
#     which is a ~35-minute QEMU build.  A whole class of build breakage --
#     implicit declarations, missing headers, type errors -- was gated behind
#     half an hour of ninja.
#
#     `gcc -fsyntax-only` against the unit-test header stubs parses the same
#     source with the same compiler and stops before codegen.  It is not a
#     substitute for the real build; it is the two-second version that catches
#     the mistakes that actually happen.
#
# WHAT IT DOES NOT COVER
#     Three files need real QEMU headers that we do not stub -- hw/boards.h,
#     hw/qdev-properties.h, hw/virtio/virtio-pci.h -- and are SKIPPED here:
#
#         nvkvm_mmap_host.c  virtio_nvgpu.c  virtio_nvgpu_pci.c
#
#     Those are covered only by the full build (scripts/build_qemu.sh, which CI
#     runs nightly).  Stubbing QEMU's device model well enough to parse them is
#     a much bigger job than it is worth; better to state the gap than to
#     pretend the gate is total.
#
#   bash tests/qemu_syntax_check.sh
#   CC=gcc-14 bash tests/qemu_syntax_check.sh   # check under a newer toolchain
#
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/unit" || exit 1

CC="${CC:-gcc}"

# Deliberately NOT compiled here -- see WHAT IT DOES NOT COVER above.
SKIP="nvkvm_mmap_host.c virtio_nvgpu.c virtio_nvgpu_pci.c"

CFLAGS="-fsyntax-only -Wall -Wextra -D_GNU_SOURCE
        -Istubs -I../../src -I../../src/qemu -I../../src/common"

rc=0
checked=0
for f in ../../src/qemu/*.c; do
    base="$(basename "$f")"
    case " $SKIP " in *" $base "*)
        printf '  %-28s skip (needs real QEMU headers)\n' "$base"; continue ;;
    esac
    # -Werror only for the implicit-declaration family: that is the specific
    # bug this gate is for, and the tree has pre-existing -Wsign-compare and
    # -Wunused-parameter warnings that are not worth failing a build over.
    if out="$($CC $CFLAGS -Werror=implicit-function-declaration \
                          -Werror=implicit-int \
                          -Werror=int-conversion "$f" 2>&1)"; then
        printf '  %-28s ok\n' "$base"
    else
        printf '  %-28s FAILED\n' "$base"
        printf '%s\n' "$out" | grep -E 'error:' | sed 's/^/      /'
        rc=1
    fi
    checked=$((checked + 1))
done

echo
if [ "$rc" -eq 0 ]; then
    echo "QEMU SYNTAX CHECK OK — $checked file(s) parsed clean under $($CC -dumpversion 2>/dev/null || echo "$CC")."
else
    echo "QEMU SYNTAX CHECK FAILED — this tree will not build under GCC 14+."
fi
exit "$rc"
