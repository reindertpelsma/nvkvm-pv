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
vacuous=""
for f in ../../src/qemu/*.c; do
    base="$(basename "$f")"
    case " $SKIP " in *" $base "*)
        printf '  %-28s skip (needs real QEMU headers)\n' "$base"; continue ;;
    esac
    # -Werror only for the implicit-declaration family: that is the specific
    # bug this gate is for, and the tree has pre-existing -Wsign-compare and
    # -Wunused-parameter warnings that are not worth failing a build over.
    # A FILE WHOSE BODY IS #if'd OUT PASSES THIS GATE TRIVIALLY, and used to be
    # reported as "ok" beside files that were really compiled.  MEASURED
    # 2026-08-29: nvkvm_present_egl.c wraps all 2035 lines in
    # `#if defined(CONFIG_OPENGL) && NVKVM_QEMU_GRAPHICS`, this script never
    # defines CONFIG_OPENGL, and so the entire display path -- the most
    # intricate code in the tree -- contributed a green line to every run while
    # nothing of it was parsed.  "8 files parsed clean" was not false so much as
    # meaningless, which is worse, because it was cited as evidence in review.
    #
    # Detect the cause rather than measuring the symptom: a file-level guard on
    # a macro this gate does not define.  Counting surviving preprocessor lines
    # was tried first and is not trustworthy -- for most files here `gcc -E`
    # fails outright on the stub headers, so "0 lines survived" is indistinguishable
    # from "conditioned out" and would have flagged everything.
    guard=""
    for m in CONFIG_OPENGL NVKVM_QEMU_GRAPHICS; do
        if grep -qE "^#if.*(defined\\($m\\)|[^_]$m)" "$f" 2>/dev/null; then
            case "$CFLAGS" in *"-D$m"*) ;; *) guard="$guard $m" ;; esac
        fi
    done
    if [ -n "$guard" ]; then
        printf '  %-28s NOT COVERED (body is behind%s, undefined here)\n' "$base" "$guard"
        vacuous="$vacuous $base"
        continue
    fi
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
    if [ -n "$vacuous" ]; then
        echo
        echo "  NOT COVERED, and not counted above --$vacuous"
        echo "  Their bodies are conditioned out (CONFIG_OPENGL / NVKVM_QEMU_GRAPHICS),"
        echo "  so this gate says NOTHING about them. Compile them against a configured"
        echo "  QEMU tree before treating the display path as reviewed."
    fi
else
    echo "QEMU SYNTAX CHECK FAILED — this tree will not build under GCC 14+."
fi
exit "$rc"
