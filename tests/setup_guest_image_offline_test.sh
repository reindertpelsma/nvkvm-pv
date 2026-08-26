#!/usr/bin/env bash
# Exercise setup_guest.sh's cloud-image cache without network, /opt or a VM.
# The regression was an interrupted `wget -O FINAL`: FINAL existed afterward,
# so every rerun accepted truncated data and qemu-img convert failed forever.

set -u -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/bin" "$TMP/cache"

PASS=0
FAIL=0
ok()  { printf '  PASS %s\n' "$*"; PASS=$((PASS + 1)); }
bad() { printf '  FAIL %s\n' "$*"; FAIL=$((FAIL + 1)); }
assert_file_eq() {
    if cmp -s "$1" "$2"; then ok "$3"; else bad "$3"; fi
}

# The fake downloader understands exactly the two wget forms used by the real
# function: -qO- for SHA256SUMS, and --output-document= for the image.  In
# interrupt-once mode it leaves a prefix and fails, just like a dead SSH/Vast
# connection during the Ada setup.
cat >"$TMP/bin/wget" <<'STUB'
#!/usr/bin/env bash
set -u
if [ "${1:-}" = "-qO-" ]; then
    cat "$WGET_STUB_MANIFEST"
    exit 0
fi
out=""
resume=0
for arg in "$@"; do
    case "$arg" in
        --continue) resume=1 ;;
        --output-document=*) out="${arg#*=}" ;;
    esac
done
[ -n "$out" ] || exit 90
if [ "$resume" -eq 1 ]; then
    echo resume >>"$WGET_STUB_CALLS"
else
    echo fresh >>"$WGET_STUB_CALLS"
fi
if [ "${WGET_STUB_MODE:-normal}" = "interrupt-once" ] \
   && [ ! -e "$WGET_STUB_INTERRUPTED" ]; then
    head -c 97 "$WGET_STUB_SOURCE" >"$out"
    : >"$WGET_STUB_INTERRUPTED"
    exit 8
fi
if [ "$resume" -eq 1 ] && [ -e "$out" ]; then
    have="$(wc -c <"$out")"
    tail -c "+$((have + 1))" "$WGET_STUB_SOURCE" >>"$out"
else
    cp "$WGET_STUB_SOURCE" "$out"
fi
STUB

cat >"$TMP/bin/qemu-img" <<'STUB'
#!/usr/bin/env bash
echo check >>"$QEMU_STUB_CALLS"
cmp -s "${@: -1}" "$QEMU_STUB_VALID_IMAGE"
STUB
chmod +x "$TMP/bin/wget" "$TMP/bin/qemu-img"

export PATH="$TMP/bin:$PATH"
export WGET_STUB_SOURCE="$TMP/source.img"
export WGET_STUB_MANIFEST="$TMP/SHA256SUMS"
export WGET_STUB_CALLS="$TMP/wget.calls"
export WGET_STUB_INTERRUPTED="$TMP/interrupted"
export QEMU_STUB_CALLS="$TMP/qemu.calls"
export QEMU_STUB_VALID_IMAGE="$TMP/custom-valid.img"

# A repeatable non-empty fixture; sha256sum is the real host implementation.
awk 'BEGIN { for (i = 0; i < 2048; i++) printf "%c", 65 + (i % 23) }' \
    >"$WGET_STUB_SOURCE"
IMAGE_HASH="$(sha256sum "$WGET_STUB_SOURCE")"
IMAGE_HASH="${IMAGE_HASH%% *}"
printf '%s *image.img\n' "$IMAGE_HASH" >"$WGET_STUB_MANIFEST"
: >"$WGET_STUB_CALLS"
: >"$QEMU_STUB_CALLS"

export NVKVM_SETUP_GUEST_LIB=1
# shellcheck disable=SC1091
. "$REPO/scripts/setup_guest.sh"

echo "=== interrupted download is never published ==="
FINAL="$TMP/cache/image.img"
export WGET_STUB_MODE=interrupt-once
if download_cloud_image https://example.invalid/image.img "$FINAL" "" \
     https://example.invalid/SHA256SUMS; then
    bad "interrupted wget fails the setup"
else
    ok "interrupted wget fails the setup"
fi
[ ! -e "$FINAL" ] && ok "final cache path remains absent" \
    || bad "partial bytes leaked into final cache path"
[ -s "$FINAL.part" ] && ok "partial bytes remain resumable" \
    || bad "resumable staging file is missing"

echo "=== rerun resumes, verifies and atomically publishes ==="
export WGET_STUB_MODE=normal
if download_cloud_image https://example.invalid/image.img "$FINAL" "" \
     https://example.invalid/SHA256SUMS; then
    ok "rerun completes the staged download"
else
    bad "rerun failed"
fi
assert_file_eq "$FINAL" "$WGET_STUB_SOURCE" "published image matches SHA-256 source"
[ ! -e "$FINAL.part" ] && ok "staging path disappears after publication" \
    || bad "staging path survived publication"
[ "$(<"$FINAL.sha256")" = "$IMAGE_HASH  https://example.invalid/image.img" ] \
    && ok "verified checksum is cached beside the image" \
    || bad "checksum sidecar is wrong"

echo "=== an invalid legacy final path is not accepted by existence ==="
rm -f "$FINAL.part"
awk 'BEGIN { for (i = 0; i < 2048; i++) printf "Z" }' >"$FINAL"
: >"$WGET_STUB_CALLS"
if download_cloud_image https://example.invalid/image.img "$FINAL" "" \
     https://example.invalid/SHA256SUMS; then
    ok "invalid legacy cache is repaired"
else
    bad "invalid legacy cache was not repaired"
fi
assert_file_eq "$FINAL" "$WGET_STUB_SOURCE" "clean retry replaces same-sized corrupt data"
[ "$(tr '\n' ' ' <"$WGET_STUB_CALLS")" = "resume fresh " ] \
    && ok "failed resume triggers one byte-zero retry" \
    || bad "resume/fresh retry sequence was not exercised"

echo "=== custom URLs without a checksum get structural validation ==="
printf 'operator supplied qcow2\n' >"$QEMU_STUB_VALID_IMAGE"
CUSTOM="$TMP/cache/custom.img"
cp "$QEMU_STUB_VALID_IMAGE" "$CUSTOM"
: >"$QEMU_STUB_CALLS"
: >"$WGET_STUB_CALLS"
if download_cloud_image https://custom.invalid/disk.qcow2 "$CUSTOM" "" ""; then
    ok "valid custom cache is reusable"
else
    bad "valid custom cache was rejected"
fi
[ "$(wc -l <"$QEMU_STUB_CALLS")" -eq 1 ] \
    && ok "custom cache is validated on every invocation" \
    || bad "custom cache skipped qemu-img validation"
[ ! -s "$WGET_STUB_CALLS" ] && ok "valid custom cache is not downloaded again" \
    || bad "valid custom cache was downloaded again"

echo "=== malformed explicit checksums fail before network access ==="
: >"$WGET_STUB_CALLS"
if download_cloud_image https://custom.invalid/disk.qcow2 "$CUSTOM" bad ""; then
    bad "malformed checksum was accepted"
else
    ok "malformed checksum was rejected"
fi
[ ! -s "$WGET_STUB_CALLS" ] && ok "bad checksum fails before download" \
    || bad "bad checksum reached downloader"

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
