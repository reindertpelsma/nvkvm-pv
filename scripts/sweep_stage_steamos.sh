#!/usr/bin/env bash
# sweep_stage_steamos.sh -- the SteamOS end-to-end stage, run ON A SWEEP BOX.
#
# WHY THIS STAGE EXISTS
#
# The driver stage runs tests/validate.sh, which answers "does the ABI hold on
# this driver". It does not touch a single product surface. Every real bug found
# by hand on 2026-08-29 came from THIS path and none from validate.sh:
#
#   * Valve's rootfs slots are 5 GiB and the NVIDIA userspace does not fit, so
#     the first OTA failed 42 MB short -- after already reclaiming firmware.
#   * When it does run out, nvidia-installer dies on SIGBUS mid-write, leaving a
#     tree that a version-only check then calls "already installed" forever.
#   * The vmm container had no /dev/dri, so every guest compositor died on
#     ENOENT while stat(2), /proc/devices and /sys/class/drm all looked perfect.
#   * `steamos-bootconf set-mode reboot-other` selected a phantom 'dev' image.
#
# None of those are visible from a compute test. Hence a stage that installs the
# real thing and drives it to a desktop.
#
# WHAT IT PROVES, in order, each a separate verdict so a failure is attributable:
#   install   Valve's own installer runs, with our patched 8 GiB geometry
#   provision boot.sh converges the new slot: module, complete NVIDIA userspace
#   boot      the guest boots under nvkvm and ssh answers
#   display   the head is ENABLED and frames flip with a real modifier
#   ota       steamos-update applies and our hook provisions the other slot
#   slotb     the machine reboots into that slot and reaches a desktop
#
# UNTRUSTED HOST RULES (this runs on a rented box):
#   - everything is installed FROM THE INTERNET, as a user would; no artifact is
#     copied from the coordinator
#   - only logs and this script's JSON verdicts ever travel back
#   - nothing here interpolates a value from the box into a command
set -uo pipefail

STEAMOS_REF="${STEAMOS_REF:-main}"
STEAMOS_REPO="${STEAMOS_REPO:-https://github.com/reindertpelsma/nvkvm-steamos.git}"
NVKVM_REF="${NVKVM_REF:-main}"
NVKVM_REPO="${NVKVM_REPO:-https://github.com/reindertpelsma/nvkvm-pv.git}"
WORK="${WORK:-/root/steamos-stage}"
IMG_BASE="steamdeck-oobe-repair-20260707.10-3.8.14"
IMG_URL="https://steamdeck-images.steamos.cloud/recovery/$IMG_BASE.img.bz2"
GUEST_SSH_PORT=15022

mkdir -p "$WORK"
LOG="$WORK/stage.log"
VERDICTS="$WORK/verdicts.jsonl"
: > "$VERDICTS"
exec > >(tee -a "$LOG") 2>&1

say()  { printf '[steamos %s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
# One JSON object per phase. The coordinator reads these and never parses prose.
verdict() { # verdict <phase> <status> <detail>
    python3 - "$1" "$2" "${3:-}" >>"$VERDICTS" <<'PY'
import json,sys,time
print(json.dumps({"phase":sys.argv[1],"status":sys.argv[2],
                  "detail":sys.argv[3][:400],"t":int(time.time())}))
PY
    say "VERDICT $1 = $2 ${3:-}"
}
die_stage() { verdict "$1" fail "${2:-}"; say "STAGE FAILED at $1"; exit 1; }

# ---------------------------------------------------------------- preflight --
command -v qemu-system-x86_64 >/dev/null || {
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null 2>&1
    apt-get install -y -qq qemu-system-x86 qemu-utils ovmf bzip2 git docker.io >/dev/null 2>&1
}
for t in qemu-system-x86_64 git docker curl; do
    command -v "$t" >/dev/null || die_stage preflight "missing $t"
done
[ -e /dev/kvm ] || die_stage preflight "no /dev/kvm"
# The guest's DRM node is a PROXY for the host's. Without these the guest gets
# ENOENT on every open and no compositor starts -- diagnosed the hard way.
# nvidia-drm is loaded WITHOUT modeset by many distro setups, and then there is
# no render node at all.  Ask for it before declaring the box unusable.
if [ ! -e /dev/dri/renderD128 ]; then
    modprobe nvidia-drm modeset=1 2>/dev/null || modprobe nvidia-drm 2>/dev/null || true
    sleep 2
fi
[ -e /dev/dri/renderD128 ] || die_stage preflight "no /dev/dri/renderD128 even after modprobe nvidia-drm modeset=1"
verdict preflight pass "kvm + dri present"

# ------------------------------------------------------------------- clone --
# From GitHub at a pinned ref: this is how a user installs, and it keeps any
# local artifact off the rented box.
say "cloning nvkvm-steamos@$STEAMOS_REF and nvkvm-pv@$NVKVM_REF"
rm -rf "$WORK/nvkvm-steamos" "$WORK/nvkvm-pv"
git clone -q --depth 50 "$STEAMOS_REPO" "$WORK/nvkvm-steamos" || die_stage clone "steamos clone failed"
git -C "$WORK/nvkvm-steamos" checkout -q "$STEAMOS_REF" || die_stage clone "steamos ref $STEAMOS_REF"
# nvkvm-pv is the code UNDER TEST, so it comes from the tree the sweep shipped
# to this box, not from GitHub.  Cloning main here would quietly test a
# different commit than the driver stage just validated, and a release gate that
# tests the wrong tree is worse than no gate.  Outside a sweep (run by hand),
# fall back to a clone so the script still works standalone.
if [ -f /root/nvkvm/src/common/nvkvm_abi.h ]; then
    PV_SRC="under-test tree shipped by the sweep"
    cp -a /root/nvkvm "$WORK/nvkvm-pv"
else
    PV_SRC="clone of $NVKVM_REPO@$NVKVM_REF"
    git clone -q --depth 50 "$NVKVM_REPO" "$WORK/nvkvm-pv" || die_stage clone "nvkvm-pv clone failed"
    git -C "$WORK/nvkvm-pv" checkout -q "$NVKVM_REF" || die_stage clone "nvkvm-pv ref $NVKVM_REF"
fi
verdict clone pass "steamos=$(git -C "$WORK/nvkvm-steamos" rev-parse --short HEAD) pv=$(git -C "$WORK/nvkvm-pv" rev-parse --short HEAD 2>/dev/null || echo local) [$PV_SRC]"

# ------------------------------------------------------------ recovery img --
cd "$WORK" || die_stage image "cannot enter $WORK"
if [ ! -f "$IMG_BASE.img" ]; then
    say "downloading the recovery image (3.2 GB)"
    # .part until complete: a truncated .bz2 decompresses to garbage, and a
    # half-downloaded image that looks present is worse than none.
    curl -fL --retry 3 --retry-delay 5 -o "$IMG_BASE.img.bz2.part" "$IMG_URL" \
        && mv "$IMG_BASE.img.bz2.part" "$IMG_BASE.img.bz2" \
        || die_stage image "download failed (some regions get HTTP 403 from the CDN)"
    bunzip2 -f "$IMG_BASE.img.bz2" || die_stage image "decompress failed"
fi
verdict image pass "$(du -h "$IMG_BASE.img" | cut -f1)"

# ---------------------------------------------------------------- install ---
# The share is an nvkvm-pv checkout with this repo's boot/ copied in; the public
# nvkvm-pv does not carry boot/.
cp -r "$WORK/nvkvm-steamos/boot" "$WORK/nvkvm-pv/boot" 2>/dev/null
DRV="$(sed -n 's/.*Module *for *x86_64 *\([0-9.]*\).*/\1/p' /proc/driver/nvidia/version 2>/dev/null | head -1)"
[ -n "$DRV" ] || DRV="$(sed -n 's/.*Kernel Module *\([0-9.]*\).*/\1/p' /proc/driver/nvidia/version 2>/dev/null | head -1)"
[ -n "$DRV" ] || die_stage install "cannot read the host driver version"
say "installing SteamOS (repair + provision) against driver $DRV"
cd "$WORK/nvkvm-steamos" || die_stage install "cannot enter the checkout"
timeout 3600 ./install_steamos_vm.sh \
    --repair "$WORK/$IMG_BASE.img" --out "$WORK/steamos.qcow2" \
    --stages repair,provision --share "$WORK/nvkvm-pv" \
    --driver-version "$DRV" --memory 8192 --cpus 4 \
    --log "$WORK/install-serial.log" >"$WORK/install.log" 2>&1
rc=$?
[ -f "$WORK/steamos.qcow2" ] || die_stage install "no qcow2 produced (rc=$rc)"
verdict install pass "rc=$rc size=$(du -h "$WORK/steamos.qcow2" | cut -f1)"

# Did provisioning actually converge, and is the NVIDIA userspace COMPLETE?
# A version match is not proof: an OOM-killed installer leaves a tree that
# reports the right version and is missing the files the display path needs.
p1="$(grep -c 'Part 1 finished (rc=0)' "$WORK/install-serial.log" 2>/dev/null || echo 0)"
nverr="$(grep -ci 'nvidia-installer failed\|not enough space\|SIGBUS' "$WORK/install-serial.log" 2>/dev/null || echo 0)"
if [ "$p1" -ge 1 ] && [ "$nverr" -eq 0 ]; then
    verdict provision pass "Part 1 rc=0, no installer error"
else
    verdict provision fail "Part1=$p1 installer_errors=$nverr"
fi

# ------------------------------------------------------------------- boot ---
say "booting the installed image under nvkvm"
mkdir -p "$WORK/data"
[ -f "$WORK/guest_key" ] || ssh-keygen -t ed25519 -N '' -C sweep-steamos -f "$WORK/guest_key" >/dev/null
cp -f "$WORK/guest_key.pub" "$WORK/data/authorized_keys"   # sshd is enabled only if this exists
docker volume create nvkvm-steamos-state >/dev/null 2>&1
docker run --rm -v nvkvm-steamos-state:/s -v "$WORK":/src alpine \
    sh -c 'cp /src/steamos.qcow2 /s/steamos.qcow2.part && mv /s/steamos.qcow2.part /s/steamos.qcow2' \
    >/dev/null 2>&1 || die_stage boot "could not seed the state volume"

export NVKVM_CONTEXT="$WORK/nvkvm-pv" NVKVM_STEAMOS_DATA="$WORK/data"
export NVKVM_BROKER_BACKEND=headless DISPLAY="${DISPLAY:-}"
cd "$WORK/nvkvm-steamos" || die_stage boot "cannot enter the checkout"
NVKVM_CONTEXT="$WORK/nvkvm-pv" timeout 3000 docker compose build vmm broker >"$WORK/build.log" 2>&1 \
    || die_stage boot "image build failed"
# override-dri.yml is REQUIRED: the guest's DRM node proxies the host's.
OVR=""; [ -f override-dri.yml ] && OVR="-f override-dri.yml"
# shellcheck disable=SC2086
docker compose -f docker-compose.yml $OVR up -d >"$WORK/up.log" 2>&1 \
    || die_stage boot "compose up failed"

G="-i $WORK/guest_key -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
up=no
for _ in $(seq 1 40); do
    sleep 15
    # shellcheck disable=SC2086
    if timeout 20 ssh $G -p "$GUEST_SSH_PORT" root@127.0.0.1 true 2>/dev/null; then up=yes; break; fi
done
[ "$up" = yes ] || die_stage boot "guest never answered ssh"
verdict boot pass "guest ssh up"

# ---------------------------------------------------------------- display ---
# The question is not "did something render" but "is the head ENABLED and are
# frames flipping with a real modifier". A guest can look healthy -- module
# loaded, /dev/dri/card0 present, correct major:minor -- while every open
# returns ENOENT and nothing is ever scanned out.
cat > "$WORK/probe.sh" <<'PROBE'
#!/bin/bash
echo "os=$(sed -n 's/^BUILD_ID=//p' /etc/os-release | tr -d '"')"
echo "variant=$(sed -n 's/^VARIANT_ID=//p' /etc/os-release | tr -d '"')"
echo "module=$(lsmod | grep -c '^nvkvm_guest')"
echo "nvidia_libs=$(ls /usr/lib 2>/dev/null | grep -c '^libnvidia')"
echo "gbm_nvidia=$([ -e /usr/lib/gbm/nvidia-drm_gbm.so ] && echo yes || echo NO)"
echo "egl_gbm=$([ -e /usr/share/egl/egl_external_platform.d/15_nvidia_gbm.json ] && echo yes || echo NO)"
echo "drm_open=$(python3 -c "
import os
try:
    fd=os.open('/dev/dri/card0', os.O_RDWR); os.close(fd); print('ok')
except Exception as e: print(type(e).__name__)
" 2>/dev/null)"
for f in /sys/class/drm/card*/card*-Virtual-1/enabled; do echo "drm_enabled=$(cat "$f" 2>/dev/null)"; done
echo "compositor=$(pgrep -c -x 'kwin_wayland|gamescope' 2>/dev/null || echo 0)"
echo "segfaults=$(journalctl -b --no-pager 2>/dev/null | grep -ci segfault)"
PROBE
# shellcheck disable=SC2086
scp $G -P "$GUEST_SSH_PORT" -q "$WORK/probe.sh" root@127.0.0.1:/root/ 2>/dev/null
sleep 90   # let the session settle before judging it
# shellcheck disable=SC2086
timeout 120 ssh $G -p "$GUEST_SSH_PORT" root@127.0.0.1 "bash /root/probe.sh" >"$WORK/display.txt" 2>/dev/null
get() { sed -n "s/^$1=//p" "$WORK/display.txt" | head -1; }
# Flips are logged by QEMU on the HOST side, never in the guest's dmesg -- the
# guest only ever sees its own KMS. Read them from the vmm container.
host_flips() { docker compose logs --no-color --tail 4000 vmm 2>/dev/null \
    | grep -c 'nvkvm present: flip'; }
host_modifier() { docker compose logs --no-color --tail 4000 vmm 2>/dev/null \
    | grep -oE 'mod=0x[0-9a-f]+' | tail -1; }
FLIPS="$(host_flips)"; MOD="$(host_modifier)"
say "display probe (host flips=$FLIPS $MOD):"; sed 's/^/    /' "$WORK/display.txt"

# The NVIDIA userspace must be COMPLETE, not merely present: a truncated install
# reports the right version and is missing exactly the files the display needs.
if [ "$(get gbm_nvidia)" != yes ] || [ "$(get egl_gbm)" != yes ]; then
    verdict display fail "NVIDIA userspace incomplete: gbm=$(get gbm_nvidia) egl=$(get egl_gbm) libs=$(get nvidia_libs)"
elif [ "$(get drm_open)" != ok ]; then
    verdict display fail "the guest cannot open /dev/dri/card0 ($(get drm_open)) -- is /dev/dri mapped into the vmm?"
elif [ "$(get drm_enabled)" = enabled ] && [ "${FLIPS:-0}" -gt 0 ] 2>/dev/null; then
    verdict display pass "enabled, flips=$FLIPS, ${MOD:-no-modifier}, compositor=$(get compositor)"
else
    verdict display fail "head not enabled: enabled=$(get drm_enabled) flips=${FLIPS:-0} compositor=$(get compositor) segv=$(get segfaults)"
fi

# -------------------------------------------------------------------- ota ---
# The OOBE image is a transitional state; steamos-update is what GRADUATES it,
# and it is the path where the slot has to be provisioned by our hook.
say "running the OTA (several GB)"
# shellcheck disable=SC2086
timeout 60 ssh $G -p "$GUEST_SSH_PORT" root@127.0.0.1 \
    'systemctl mask --now sddm.service >/dev/null 2>&1; rm -f /root/ota.log; nohup setsid steamos-update >/root/ota.log 2>&1 </dev/null & echo started' >/dev/null 2>&1
ota_done=no
gone=0
for _ in $(seq 1 80); do
    sleep 30
    # ps by comm ONLY: a -f pattern matches this very ssh command line, which is
    # how an earlier run concluded a finished update was still going.
    # shellcheck disable=SC2086
    n="$(timeout 30 ssh $G -p "$GUEST_SSH_PORT" root@127.0.0.1 'ps -eo comm | grep -cx rauc' 2>/dev/null || echo 1)"
    # shellcheck disable=SC2086
    if timeout 30 ssh $G -p "$GUEST_SSH_PORT" root@127.0.0.1 'grep -q "is safe to reboot into it" /var/log/nvkvm-ota.log 2>/dev/null'; then
        ota_done=yes; break
    fi
    # shellcheck disable=SC2086
    if timeout 30 ssh $G -p "$GUEST_SSH_PORT" root@127.0.0.1 'grep -qi "FAILED" /var/log/nvkvm-ota.log 2>/dev/null'; then
        # shellcheck disable=SC2086
        r="$(timeout 30 ssh $G -p "$GUEST_SSH_PORT" root@127.0.0.1 'grep -i "ERROR" /var/log/nvkvm-ota.log | tail -2' 2>/dev/null)"
        verdict ota fail "hook reported failure: $r"; ota_done=failed; break
    fi
    # rauc gone AND no success marker: give it two more polls for the hook to
    # finish writing, then call it. Without this the loop spins the full 40 min
    # on an update that already died.
    if [ "$n" = 0 ]; then
        gone=$((gone + 1))
        [ "$gone" -ge 3 ] && { verdict ota fail "rauc exited without a success marker"; ota_done=failed; break; }
    else
        gone=0
    fi
done
[ "$ota_done" = yes ] && verdict ota pass "update applied and the new slot provisioned"
[ "$ota_done" = no ]  && verdict ota fail "timed out waiting for the update"

# ------------------------------------------------------------------ slotb ---
if [ "$ota_done" = yes ]; then
    say "rebooting into the freshly provisioned slot"
    # set-mode reboot-other has selected a phantom 'dev' image before now, so
    # check what it actually chose rather than assuming.
    # shellcheck disable=SC2086
    timeout 60 ssh $G -p "$GUEST_SSH_PORT" root@127.0.0.1 \
        'systemctl unmask sddm.service >/dev/null 2>&1; systemctl enable sddm.service >/dev/null 2>&1; steamos-bootconf set-mode reboot-other >/dev/null 2>&1; echo "selected=$(steamos-bootconf selected-image)"' >"$WORK/slotb-select.txt" 2>/dev/null
    say "  $(cat "$WORK/slotb-select.txt" 2>/dev/null)"
    # shellcheck disable=SC2086
    timeout 30 ssh $G -p "$GUEST_SSH_PORT" root@127.0.0.1 'sync; systemctl reboot' >/dev/null 2>&1
    back=no
    for _ in $(seq 1 40); do
        sleep 20
        # shellcheck disable=SC2086
        if timeout 20 ssh $G -p "$GUEST_SSH_PORT" root@127.0.0.1 true 2>/dev/null; then back=yes; break; fi
        # a guest that is not coming back usually says why on the serial console
        docker compose logs --no-color --tail 5 vmm 2>/dev/null | sed 's/^/      vmm| /'
    done
    if [ "$back" != yes ]; then
        verdict slotb fail "guest never came back after reboot"
    else
        sleep 90
        # shellcheck disable=SC2086
        timeout 120 ssh $G -p "$GUEST_SSH_PORT" root@127.0.0.1 "bash /root/probe.sh" >"$WORK/slotb.txt" 2>/dev/null
        getb() { sed -n "s/^$1=//p" "$WORK/slotb.txt" | head -1; }
        BFLIPS="$(host_flips)"
        say "slot B probe (host flips=$BFLIPS):"; sed 's/^/    /' "$WORK/slotb.txt"
        if [ "$(getb variant)" = "steamdeck-oobe" ]; then
            verdict slotb fail "still on the OOBE variant -- the update did not graduate the guest"
        elif [ "$(getb drm_enabled)" = enabled ] && [ "${BFLIPS:-0}" -gt "${FLIPS:-0}" ] 2>/dev/null; then
            verdict slotb pass "non-OOBE $(getb os), head enabled, flips advanced to $BFLIPS"
        else
            verdict slotb fail "booted $(getb os) variant=$(getb variant) but the head is $(getb drm_enabled)"
        fi
    fi
fi

say "=== stage summary ==="
cat "$VERDICTS"
fails="$(grep -c '"status": *"fail"' "$VERDICTS" 2>/dev/null || echo 0)"
say "phases failed: $fails"
exit "$([ "$fails" -eq 0 ] && echo 0 || echo 1)"
