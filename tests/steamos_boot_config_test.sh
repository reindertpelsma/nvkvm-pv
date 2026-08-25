#!/usr/bin/env bash
# Regression guard for the desktop policy shared by fresh provisioning, normal
# boot convergence, and SteamOS A/B update provisioning.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOOT_SCRIPT="$REPO_ROOT/boot/steamos_boot.sh"
INSTALLER_GUEST="$REPO_ROOT/vm/guest-init.sh"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

fail() {
    printf 'steamos_boot_config_test: FAIL: %s\n' "$*" >&2
    exit 1
}

export NVKVM_STEAMOS_BOOT_SOURCE_ONLY=1
# shellcheck source=../boot/steamos_boot.sh
source "$BOOT_SCRIPT"

ROOT="$TEST_ROOT"
export PROFILE=steamos
mkdir -p "$ROOT/usr/share/wayland-sessions"
: > "$ROOT/usr/share/wayland-sessions/plasma.desktop"
write_sddm_session_config

OVERRIDE="$ROOT/etc/sddm.conf.d/zz-nvkvm-plasma.conf"
[ -f "$OVERRIDE" ] || fail "SteamOS provisioning did not create the SDDM override"
EXPECTED=$'[Autologin]\nSession=plasma.desktop'
[ "$(cat "$OVERRIDE")" = "$EXPECTED" ] \
    || fail "the SDDM override does not select plasma.desktop exactly"

# A profile switch must remove policy that no longer belongs to this image.
PROFILE=compute
write_sddm_session_config
[ ! -e "$OVERRIDE" ] || fail "compute convergence left a stale SteamOS override"

# Do not write a session name the target image cannot resolve.
PROFILE=steamos
rm -f "$ROOT/usr/share/wayland-sessions/plasma.desktop"
write_sddm_session_config
[ ! -e "$OVERRIDE" ] || fail "provisioning selected an absent Plasma session"

# Pin the setup call chain: the disposable Alpine builder must provision the
# newly installed root through the same --install-only implementation tested
# above. A manual post-boot edit would not satisfy this assertion.
grep -qF '/run/nvkvm/boot/steamos_boot.sh --install-only --root "$TARGET_MNT"' \
    "$INSTALLER_GUEST" \
    || fail "fresh-image setup no longer calls steamos_boot.sh --install-only --root"
grep -qF 'write_desktop_config || rc=1' "$BOOT_SCRIPT" \
    || fail "do_install no longer converges desktop configuration"

printf 'steamos_boot_config_test: PASS\n'
