#!/usr/bin/env bash
#
# sweep_autodestroy.sh -- the money safety net for scripts/sweep.sh.
#
# WHY THIS IS A SEPARATE FILE, AND WHY IT IS ARMED FIRST
#
#   The worst outcome of an unattended sweep is not a wrong result, it is a
#   GPU box billing overnight because the thing that was supposed to destroy
#   it died.  Every in-process safety net (a bash trap, a python `finally`)
#   shares its fate with the process it protects: SIGKILL, an OOM kill, a
#   dropped ssh session or a closed terminal takes the net down with the
#   swimmer.  That has already happened on this project -- instance 48253468
#   was orphaned by exactly that pattern and billed until a human read
#   `vastai show instances` and found it.
#
#   So the net is a SEPARATE PROCESS, started from a SEPARATE FILE, before a
#   single cent is committed.  It shares nothing with the sweep but a registry
#   file.  sweep.sh verifies with ps(1) that it is actually running and
#   refuses to rent anything if it is not.
#
# CONTRACT
#
#   sweep_autodestroy.sh <deadline-epoch-seconds> <registry-file> <log-file>
#
#   The registry file is a plain text list, one vast.ai instance id per line,
#   appended by sweep.sh the instant a create returns (BEFORE anything is done
#   with the box, so a crash between create and use is still covered).  This
#   process re-reads it on every poll, so instances created after it started
#   are covered too.
#
#   At the deadline: destroy every id in the registry, unconditionally.  There
#   is deliberately NO disarm.  A destroy of an already-destroyed instance is a
#   harmless no-op, whereas a disarm that fires wrongly costs real money, so
#   the asymmetry decides the design.  vast.ai instance ids are never reused,
#   so a stale registry entry cannot collide with a later rental.
#
#   Before the deadline: exit early only when sweep.sh has signalled completion
#   (<registry>.done exists) AND every registered id is confirmed absent from
#   `vastai show instances`.  "The sweep said it was done" alone is not enough;
#   the listing is the only evidence that is worth anything here.
#
# TRAPS ENCODED HERE (each one cost real money or a real session)
#
#   * `vastai destroy instance <id>` PROMPTS "[y/N]".  With no tty it reads
#     EOF, prints "Aborted." and exits 0 -- a silent no-op that looks like a
#     success.  Hence `yes |` AND `-y`.
#   * The exit code of destroy only says the CLI ran.  The ONLY proof an
#     instance is gone is that it no longer appears in the listing.
#   * `vastai show instances` prints a DEPRECATION banner on stderr, which
#     lands in the middle of the JSON if stderr is merged.  Always 2>/dev/null.
#   * NEVER pkill -f a pattern that could match this script's own argv.  This
#     file does not kill anything but vast instances, by exact id.
#
set -u

DEADLINE="${1:?usage: sweep_autodestroy.sh <deadline-epoch> <registry-file> <log-file>}"
REGISTRY="${2:?usage: sweep_autodestroy.sh <deadline-epoch> <registry-file> <log-file>}"
LOGFILE="${3:-/tmp/nvkvm-sweep-autodestroy.log}"

VASTAI="${VASTAI_BIN:-$(command -v vastai || echo /usr/local/bin/vastai)}"

# Instances this timer must NEVER destroy, whatever a registry file says.
# These are long-lived boxes belonging to other people or other work; a destroy
# is irreversible and can throw away hours of state.  sweep.sh passes its own
# protected set through the environment, and the defaults below are the boxes
# known to be in use when this script was written.
PROTECTED="${NVKVM_SWEEP_PROTECT:-}"

log() { printf '%s autodestroy: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >>"$LOGFILE"; }

is_protected() {
    local id="$1" p
    for p in $PROTECTED; do [ "$p" = "$id" ] && return 0; done
    return 1
}

# Every id currently registered, de-duplicated, ignoring blanks and comments.
registered_ids() {
    [ -f "$REGISTRY" ] || return 0
    grep -oE '^[0-9]+' "$REGISTRY" 2>/dev/null | sort -u
}

# The ids vast.ai still shows as existing.  Printed one per line.
live_ids() {
    "$VASTAI" show instances --raw 2>/dev/null | python3 -c '
import json,sys
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(0)                      # unparseable: say nothing rather than lie
if isinstance(d, dict):
    d = d.get("instances", []) or []
for i in d:
    print(i.get("id"))
' 2>/dev/null
}

destroy_verified() {
    local id="$1" try
    if is_protected "$id"; then
        log "REFUSING to destroy protected instance $id"
        return 1
    fi
    for try in 1 2 3 4 5; do
        yes | "$VASTAI" destroy instance "$id" -y >>"$LOGFILE" 2>&1
        sleep 8
        if ! live_ids | grep -qx "$id"; then
            log "destroyed $id (verified absent from the listing, attempt $try)"
            return 0
        fi
        sleep 10
    done
    log "!! COULD NOT DESTROY $id AFTER 5 ATTEMPTS -- DESTROY IT BY HAND !!"
    return 1
}

log "armed: deadline=$(date -u -d "@$DEADLINE" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo "$DEADLINE") registry=$REGISTRY protected='$PROTECTED'"

while true; do
    now="$(date +%s)"
    if [ "$now" -ge "$DEADLINE" ]; then
        log "DEADLINE REACHED -- destroying every registered instance"
        break
    fi

    # Early exit: the sweep says it finished AND the listing agrees that
    # nothing it registered is still alive.  Both halves are required.
    if [ -f "${REGISTRY}.done" ]; then
        leaked=""
        live="$(live_ids)"
        for id in $(registered_ids); do
            if printf '%s\n' "$live" | grep -qx "$id"; then leaked="$leaked $id"; fi
        done
        if [ -z "$leaked" ]; then
            log "sweep signalled done and no registered instance is alive -- standing down"
            exit 0
        fi
        log "sweep signalled done but these are STILL ALIVE:$leaked -- staying armed"
    fi

    sleep 30
done

rc=0
for id in $(registered_ids); do
    if live_ids | grep -qx "$id"; then
        log "deadline destroy: $id is alive"
        destroy_verified "$id" || rc=1
    else
        log "deadline: $id already gone"
    fi
done
log "finished (rc=$rc)"
exit "$rc"
