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
# The vast label every instance this run creates carries.  Overridable because
# reap_strays() destroys BY LABEL: two sweeps running concurrently under the
# same label can reap each other's boxes mid-build.  Give each concurrent run
# its own label (and each its own --out) and that cannot happen.
SWEEP_LABEL="${NVKVM_SWEEP_LABEL:-nvkvm-sweep}"
STOP_FILE="/tmp/nvkvm-sweep.stop"
KNOWN_BAD_FILE="$REPO/scripts/sweep-known-bad-machines.txt"

ARCHES=""
ALL_ARCHES=0
GPU_FILTER=""
DRIVERS_REQ=""
DRIVER_CACHE_DIR="${NVKVM_SWEEP_DRIVER_CACHE:-}"
PRESET="boundary"
MIN_DRIVERS=5
MAX_DPH=0.50
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
        --all-arches)   ALL_ARCHES=1; shift ;;
        --gpu)          GPU_FILTER="$2"; shift 2 ;;
        --drivers)      DRIVERS_REQ="$2"; shift 2 ;;
        --driver-cache) DRIVER_CACHE_DIR="$2"; shift 2 ;;
        --preset)       PRESET="$2"; shift 2 ;;
        --min-drivers)  MIN_DRIVERS="$2"; shift 2 ;;
        --max-dph)      MAX_DPH="$2"; shift 2 ;;
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

# ---------------------------------------------------------------------------
# small utilities
# ---------------------------------------------------------------------------
say()  { printf '%s\n' "$*"; }
info() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
warn() { printf '[%s] !! %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }
die()  { printf '\n%s: FATAL: %s\n' "$SELF" "$*" >&2; exit "${2:-3}"; }

if [ -n "$DRIVER_CACHE_DIR" ]; then
    [ -d "$DRIVER_CACHE_DIR" ] \
        || die "driver cache is not a directory: $DRIVER_CACHE_DIR" 3
    DRIVER_CACHE_DIR="$(cd -- "$DRIVER_CACHE_DIR" && pwd -P)" \
        || die "cannot resolve driver cache: $DRIVER_CACHE_DIR" 3
fi

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
build_abiq() {
    local src="$REPO/src/common/nvkvm_abi.h"
    [ -f "$src" ] || { warn "no $src -- expected ABI profiles will read '?'"; return 1; }
    command -v cc >/dev/null || { warn "no cc -- expected ABI profiles will read '?'"; return 1; }
    ABIQ="$(mktemp -u /tmp/nvkvm-abiq.XXXXXX)"
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
    ids="$(vj show instances | python3 -c '
import json,sys
try: d = json.load(sys.stdin)
except Exception: sys.exit(0)
if isinstance(d, dict): d = d.get("instances", []) or []
for i in d:
    if (i.get("label") or "") == sys.argv[1]:
        print(i.get("id"), i.get("gpu_name"))
' "$SWEEP_LABEL" 2>/dev/null)"
    [ -z "$ids" ] && return 0
    while read -r id gpu; do
        [ -z "$id" ] && continue
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
}
trap cleanup EXIT
trap 'warn "interrupted"; exit 130' INT TERM

# ---------------------------------------------------------------------------
# ssh helpers
# ---------------------------------------------------------------------------
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
SSH=""
SCP_HOST=""
SCP_PORT=""
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
    if [ -z "${SSH:-}" ]; then
        warn "rsh_t: no ssh target set -- REFUSING to run remotely-intended command locally: ${1:0:60}"
        return 97
    fi
    local q; q=$(printf '%q' "$1")
    # `< /dev/null` IS LOAD-BEARING.  ssh reads stdin, and these run inside
    # `while IFS=... read ... done <<< "$todo"` loops -- so without it the first
    # ssh SWALLOWS THE REST OF THE DRIVER LIST and the box quietly tests one
    # driver instead of six, reporting success for a sweep that never happened.
    # That is precisely the silent-undercoverage failure this script exists to
    # prevent, so it is spelled out rather than left as a habit.
    eval "timeout $tmo $SSH $q" < /dev/null
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
    python3 - "$REPO" "$arch" "$MAX_DPH" "$GPU_FILTER" "$TRIED_MACHINES" "$KNOWN_BAD_FILE" "$offers_json" <<'PY'
import json, sys, os, importlib.util
repo, want_arch, max_dph, gpu_filter, tried, kbfile, offers_path = sys.argv[1:8]
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
    if (o.get("dph_total") or 99) > float(max_dph):
        continue
    cands.append(o)

if not cands:
    sys.exit(1)
o = min(cands, key=lambda x: x.get("dph_total", 99))
print("\t".join(str(x) for x in [
    o.get("id"), o.get("machine_id"), o.get("gpu_name"),
    o.get("dph_total"), o.get("geolocation") or "?", o.get("inet_down") or "?",
    o.get("driver_version") or "?"]))
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
                        CUR_HOST="$host"; CUR_PORT="$port"
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
    tar --exclude-vcs-ignores --exclude=.git --exclude=sweep-runs \
        -czf /tmp/nvkvm-sweep-tree.tgz -C "$REPO" . 2>/dev/null \
        || { PROVISION_FAIL_DETAIL="could not tar the repo"; PROVISION_FAILED_STEP="ship"; return 1; }
    timeout 900 scp $SSH_OPTS -P "$SCP_PORT" -q /tmp/nvkvm-sweep-tree.tgz "root@$SCP_HOST:/root/" \
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
VR_STATUS=""; VR_DETAIL=""; VR_JSON=""; VR_ABI=""; VR_SUMMARY=""; VR_WARNINGS=""; VR_RC=""

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
    VR_STATUS=""; VR_DETAIL=""; VR_JSON=""; VR_ABI=""; VR_SUMMARY=""; VR_WARNINGS=""; VR_RC=""

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
    case "$VR_RC" in
        0) VR_STATUS="pass" ;;
        1) VR_STATUS="fail" ;;
        2) VR_STATUS="incomplete" ;;   # skips: NOT silently banked as a pass
        *) VR_STATUS="validate-rc-$VR_RC" ;;
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
    IFS=$'\t' read -r offer_id machine gpu dph geo inet advdrv <<<"$offer_line"
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
            instance "$iid" machine "$machine" dph "$CUR_DPH" \
            seconds "$dur" rationale "$why" role "driver-set" \
            warnings "${VR_WARNINGS:0:4000}" detail "${VR_DETAIL:0:3000}" \
            validate:json "${VR_JSON:-null}" \
            image "$KVM_IMAGE" tree "$TREE_STAMP" ts "$(date -u +%FT%TZ)")"

        say "    -> $VR_STATUS ${VR_SUMMARY:-} (${dur}s)"
    done <<<"$todo"

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
    info "reconcile-only: destroying anything labelled '$SWEEP_LABEL'. Spends nothing."
    REGISTRY="$(mktemp)"; CLEANED=1
    reap_strays
    say "still live:"
    live_instance_ids | sed 's/^/  /'
    exit 0
fi

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

# ---- plan -----------------------------------------------------------------
say ""
say "nvkvm sweep plan"
say "  tree          : $TREE_STAMP"
say "  image         : $KVM_IMAGE"
say "  architectures : $ARCHES"
say "  driver set    : --preset $PRESET${DRIVERS_REQ:+  (restricted to $DRIVERS_REQ)}"
say "  driver cache  : ${DRIVER_CACHE_DIR:-none (rentals download directly from NVIDIA)}"
say "  min drivers   : $MIN_DRIVERS per box (fewer verdicts than this FAILS the box)"
say "  caps          : \$$MAX_DPH/hr per box, \$$MAX_SPEND total, ${BUDGET_HOURS}h auto-destroy"
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
say "  $total_units driver run(s) across $nboxes box(es)"

if [ "$GO" != 1 ]; then
    # A dry run is not just a printout: it exercises the real offer search, the
    # real architecture mapping, the known-bad filter and the price cap against
    # live vast.ai data.  That is the multi-architecture layer, minus the money.
    say ""
    say "  offer lookup (live vast.ai data, read-only):"
    est=0
    for a in ${ARCHES//,/ }; do
        line="$(pick_offer "$a")" || { printf '    %-10s NO RENTABLE KVM OFFER under $%s/hr\n' "$a" "$MAX_DPH"; continue; }
        IFS=$'\t' read -r _ mid gname odph ogeo _ _ <<<"$line"
        n="$(drivers_for_arch "$a" | wc -l)"
        hours="$(python3 -c "print(round(1.2 + 0.25 * $n, 2))")"
        c="$(python3 -c "print(round($odph * $hours, 2))")"
        est="$(python3 -c "print(round($est + $c, 2))")"
        printf '    %-10s %-16s $%-7s machine=%-8s %-22s ~%sh -> ~$%s\n' \
               "$a" "$gname" "$odph" "$mid" "$ogeo" "$hours" "$c"
    done
    say ""
    say "  estimated total: ~\$$est   (cap is \$$MAX_SPEND)"
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
REGISTRY="$OUT_DIR/instances.registry"
touch "$RESULTS" "$REGISTRY"
rm -f "${REGISTRY}.done"
info "results: $RESULTS"

# ---- money safety, BEFORE the first cent ----------------------------------
arm_autodestroy

rm -f "$STOP_FILE"
say ""
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
