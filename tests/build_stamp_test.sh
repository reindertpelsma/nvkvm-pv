#!/usr/bin/env bash
# The QEMU build guard is content-addressed. This proves the stamp is sensitive
# to exactly the things that end up in the binary and to nothing else.
#
# Why it exists: MEASURED 2026-09-01, a --ssh sweep reused a QEMU binary built
# 1h37m before the tree it was measuring arrived, so an allowlist change was
# never compiled in -- while every record carried the new tree's git sha. An
# existence-only guard cannot catch that, and "remember --force" is not a guard.
set -u
cd "$(dirname "$0")/.."
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); printf '  PASS %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  FAIL %s\n' "$1"; }

# the same expression build_qemu.sh uses
stamp() {
    { find src/qemu patches -type f \( -name '*.c' -o -name '*.h' -o -name '*.patch' \) \
        -print0 2>/dev/null | sort -z | xargs -0 sha256sum 2>/dev/null
      printf '%s\n' "test-version"
    } | sha256sum | cut -d' ' -f1
}

A="$(stamp)"
[ -n "$A" ] && ok "stamp is non-empty" || bad "stamp is non-empty"
[ "$A" = "$(stamp)" ] && ok "stamp is deterministic" || bad "stamp is deterministic"

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
V="src/qemu/nvkvm_fe_alloc_allowlist.h"
if [ -f "$V" ]; then
    cp "$V" "$T/save"
    printf '\n/* stamp test */\n' >> "$V"
    [ "$(stamp)" != "$A" ] && ok "stamp changes when a src/qemu header changes" \
                           || bad "stamp changes when a src/qemu header changes"
    cp "$T/save" "$V"
    [ "$(stamp)" = "$A" ] && ok "stamp returns to its original value" \
                          || bad "stamp returns to its original value"
else
    printf '  SKIP %s not present\n' "$V"
fi

if [ -f README.md ]; then
    cp README.md "$T/rsave"
    printf '\nstamp test\n' >> README.md
    [ "$(stamp)" = "$A" ] && ok "stamp ignores files that do not reach the binary" \
                          || bad "stamp ignores files that do not reach the binary"
    cp "$T/rsave" README.md
fi

grep -q 'nvkvm_build_stamp' scripts/build_qemu.sh \
    && ok "build_qemu.sh still uses a content stamp" \
    || bad "build_qemu.sh still uses a content stamp"
grep -q 'already exists — skipping build' scripts/build_qemu.sh \
    && bad "the old existence-only guard message is back" \
    || ok "the existence-only guard is gone"

printf '\n%d/%d checks passed\n' "$PASS" "$((PASS+FAIL))"
[ "$FAIL" -eq 0 ]
