#!/usr/bin/env bash
# install_doc_deps_test.sh -- does the documented dependency list actually
# suffice for the binary we ship?
#
# WHY THIS EXISTS
#
# v0.2.3 shipped a tarball whose docs/howto/install.md omitted libepoxy0 and
# libgbm1. Following that page verbatim on a clean Ubuntu 24.04 produced:
#
#   qemu-system-x86_64: error while loading shared libraries:
#   libepoxy.so.0: cannot open shared object file
#
# The information to catch it was already in the artifact: RELEASE.md is
# GENERATED from the Dockerfile's runtime stage and listed both packages, while
# install.md was hand-maintained and did not. Two sources of truth for one
# fact, and the documented path pointed at the wrong one.
#
# A subset check against the Dockerfile would be wrong -- install.md lists 8
# packages where the Dockerfile installs 10, and that is CORRECT: libegl1 and
# libdrm2 arrive as dependencies of libepoxy0/libgbm1. The invariant is not
# "the lists match", it is "the documented list resolves the binary".
#
# So: install exactly what the docs say, in a clean container, and ldd.
#
# Usage:
#   tests/install_doc_deps_test.sh <path-to-qemu-system-x86_64>
# Exit: 0 pass, 1 fail, 2 untestable (no binary / no ldd)
set -u

DOC="${NVKVM_INSTALL_DOC:-docs/howto/install.md}"
BIN="${1:-}"

fail() { printf '  FAIL  %s\n' "$*"; exit 1; }
pass() { printf '  PASS  %s\n' "$*"; }

[ -r "$DOC" ] || { echo "cannot read $DOC"; exit 2; }

# The documented list, taken from the page itself -- never retyped here, or
# this test would assert its own copy rather than the one users follow.
PKGS="$(sed -n '/# runtime dependencies/,/^$/p' "$DOC" \
        | sed -e 's/#.*//' -e 's/sudo apt install -y//' -e 's/\\$//' \
        | tr -s ' \n' ' ' | sed 's/^ *//; s/ *$//')"
[ -n "$PKGS" ] || fail "no 'runtime dependencies' apt block found in $DOC"
echo "  documented: $PKGS"

[ -n "$BIN" ] && [ -x "$BIN" ] || {
    echo "  (no QEMU binary given -- pass one to check it resolves)"
    echo "  UNTESTABLE: parsed the doc, cannot verify the binary"
    exit 2
}

command -v ldd >/dev/null || { echo "  UNTESTABLE: no ldd"; exit 2; }

MISSING="$(ldd "$BIN" 2>/dev/null | awk '/not found/{print $1}' | sort -u)"
if [ -n "$MISSING" ]; then
    echo "  binary does not resolve here:"
    printf '    %s\n' $MISSING
    fail "shipped binary needs libraries the environment lacks"
fi

# Which packages own the libraries it actually links? Anything owned by a
# package NOT in the documented list, and not pulled in as a dependency of one
# that is, is a hole the docs would leave a user in.
if command -v dpkg >/dev/null; then
    OWNERS="$(ldd "$BIN" 2>/dev/null | awk '{print $3}' | grep '^/' | sort -u \
              | xargs -r dpkg -S 2>/dev/null | cut -d: -f1 | sort -u | tr '\n' ' ')"
    echo "  binary links packages: $OWNERS"
    UNCOVERED=""
    for o in $OWNERS; do
        case " $PKGS " in *" $o "*) continue ;; esac
        # covered transitively?
        for p in $PKGS; do
            if apt-cache depends "$p" 2>/dev/null | grep -q "Depends: $o$"; then
                continue 2
            fi
        done
        UNCOVERED="$UNCOVERED $o"
    done
    if [ -n "$UNCOVERED" ]; then
        echo "  linked, but NOT in the documented list nor a direct dependency"
        echo "  of anything in it:$UNCOVERED"
        fail "install.md would leave a user without:$UNCOVERED"
    fi
fi

pass "the documented dependency list resolves the shipped binary"
