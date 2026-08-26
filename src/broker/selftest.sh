#!/bin/bash
# selftest.sh — exercise everything about the broker that is not the import.
#
# Uses --backend test, which drives no display and grabs no input: input comes
# from stdin and buffers are memfds.  That makes the socket layer, the command
# framing, the WHOLE ATTACH validator and the entire input policy state machine
# testable on a machine with no GPU, no monitor and no compositor — which is
# where most of this code's bugs would otherwise hide until hardware.
#
# What it CANNOT test: the actual dma-buf import and present on a real
# compositor or X server.  See README.md §7 for that honest split.
#
#   bash selftest.sh          # run it
#
# Exits non-zero if any expectation failed.
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
BROKER="$DIR/nvkvm-display-broker"
CLIENT="$DIR/nvkvm-broker-testclient"
TMP="$(mktemp -d)"
FAIL=0
N=0

cleanup() { kill %1 %2 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

ok()   { N=$((N+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { N=$((N+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=1; }
check(){ if grep -qE "$2" "$3"; then ok "$1"; else bad "$1"; fi; }
nocheck(){ if grep -qE "$2" "$3"; then bad "$1"; else ok "$1"; fi; }

[ -x "$BROKER" ] || { echo "build first: make" >&2; exit 2; }
[ -x "$CLIENT" ] || { echo "build first: make" >&2; exit 2; }

echo "== nvkvm display broker selftest =="
echo

# ─────────────────────────────────────────────────────────────────────────────
# Helper: one broker + one client run.  A fresh broker per case so no state can
# leak between them, which is the whole point of testing a state machine.
#
#   run_case <input-script> <client-args...>
# leaves the client's output in $CASE_OUT and the broker's log in $CASE_LOG.
# ─────────────────────────────────────────────────────────────────────────────
run_case() {
    local script="$1"; shift
    local s="$TMP/s.sock"
    CASE_OUT="$TMP/case.log"
    CASE_LOG="$TMP/caseb.log"
    rm -f "$s"
    # Events must arrive AFTER the client has attached: the broker has nobody
    # to send to before that, and by design it does not buffer for a client
    # that does not exist.
    ( sleep 0.9; printf '%s' "$script"; sleep 1.2 ) | \
        "$BROKER" --socket "$s" --backend test ${BROKER_EXTRA:-} \
            > "$CASE_LOG" 2>&1 &
    local bpid=$!
    sleep 0.4
    timeout 3 "$CLIENT" "$s" "$@" > "$CASE_OUT" 2>&1
    wait $bpid 2>/dev/null
}

# ── 1. socket, handshake, credentials ────────────────────────────────────────
echo "-- socket and handshake"

SOCK="$TMP/display.sock"
# --persist, DELIBERATELY.  The broker's default is now to exit when its client
# disconnects (a display outliving its VM shows nothing).  This section reuses
# ONE broker across several clients, so without --persist it exits after the
# first and every later check tests a socket that is not there -- which reads
# as "a second client was accepted" when in fact nothing was listening.
( sleep 4 ) | "$BROKER" --socket "$SOCK" --backend test --persist \
    > "$TMP/broker.log" 2>&1 &
sleep 0.7

MODE=$(stat -c %a "$SOCK" 2>/dev/null)
if [ "$MODE" = "600" ]; then ok "socket is mode 0600"
else bad "socket mode is ${MODE:-missing}, expected 600"; fi

timeout 2 "$CLIENT" "$SOCK" > "$TMP/c1.log" 2>&1
check "HELLO is the first packet"            '^seq=0[[:space:]]+HELLO'   "$TMP/c1.log"
check "HELLO announces protocol 2"           'proto v2'                  "$TMP/c1.log"
check "HELLO announces grab capability"      'kbd=1 abs=1 rel=1 lock=1'  "$TMP/c1.log"
check "HELLO announces dma-buf capability"   'dmabuf=1'                  "$TMP/c1.log"
check "SURFACE reports the window size"      'SURFACE .*x=1920 y=1080'   "$TMP/c1.log"
check "FRAME primes the client"              '[[:space:]]FRAME[[:space:]]' "$TMP/c1.log"

# one client at a time
timeout 2 "$CLIENT" "$SOCK" > "$TMP/hold.log" 2>&1 &
sleep 0.4
timeout 1 "$CLIENT" "$SOCK" > "$TMP/second.log" 2>&1
# Asserted on the SECOND CLIENT'S OWN OUTPUT, not on a broker log string: what
# matters is that it was not served, and a log message is the broker's wording
# rather than its behaviour.  The nocheck below is the real assertion; this one
# confirms the broker noticed and said so.
check   "second client is refused" 'refus|second client' "$TMP/broker.log"
nocheck "the refused client got no HELLO" 'HELLO' "$TMP/second.log"
wait %2 2>/dev/null

# SO_PEERCRED.  Needs root to change uid, so it is SKIPPED, never faked.
if [ "$(id -u)" = "0" ] && command -v setpriv >/dev/null 2>&1; then
    rm -f "$TMP/p.sock"
    ( sleep 3 ) | "$BROKER" --socket "$TMP/p.sock" --backend test \
        --allow-user root > "$TMP/peer.log" 2>&1 &
    sleep 0.5
    chmod 0777 "$TMP"          # so 'nobody' can reach the socket at all
    chmod 0666 "$TMP/p.sock"   # defeat the 0600 gate ON PURPOSE: this test is
                               # about SO_PEERCRED, not file permissions
    setpriv --reuid=65534 --regid=65534 --clear-groups \
        timeout 1 "$CLIENT" "$TMP/p.sock" > "$TMP/peer_client.log" 2>&1
    check   "an unlisted uid is rejected"   'rejected connection from uid 65534' "$TMP/peer.log"
    nocheck "the rejected uid got no HELLO" 'HELLO' "$TMP/peer_client.log"
    wait 2>/dev/null
else
    echo "  SKIP SO_PEERCRED rejection (needs root + setpriv)"
fi
wait %1 2>/dev/null

# ── 2. the ATTACH validator: the privileged side's parser ────────────────────
echo
echo "-- ATTACH validation (the privileged side parsing a hostile VMM)"

run_case '' --present 320x240
check   "a well-formed buffer is accepted" 'TEST attach: .*320x240'  "$CASE_LOG"
check   "COMMIT presents it"               'TEST commit: .*320x240'  "$CASE_LOG"
check   "the client is told the buffer was released" 'RELEASE'       "$CASE_OUT"
nocheck "no rejection on the happy path"   'REJECTED|not advertised' "$CASE_LOG"

# A-18: geometry that does not fit the buffer.  REJECT, never clamp.
run_case '' --present 320x240 --bad-size 4000
check   "a buffer smaller than its claimed geometry is REJECTED" \
        'REJECTED \(the compositor would read out of bounds\)' "$CASE_LOG"
nocheck "and is not clamped into an attach"    'TEST attach' "$CASE_LOG"
nocheck "a rejected frame does not kill the client" 'protocol violation' "$CASE_LOG"

run_case '' --present 320x240 --bad-fourcc
check   "an unadvertised fourcc is rejected" 'is not advertised by this display' "$CASE_LOG"
nocheck "and does not reach attach"          'TEST attach' "$CASE_LOG"

# The opaque-twin fallback: an alpha format whose twin IS advertised must be
# accepted and presented as the twin.  HARDENING 3 is not relaxed -- the pair
# actually handed to the display is still one the display named.
run_case '' --present 320x240 --alpha-fourcc
check   "AR24 falls back to its advertised opaque twin XR24" \
        'opaque twin' "$CASE_LOG"
check   "and the frame is attached, not refused" 'TEST attach' "$CASE_LOG"
nocheck "with no rejection"                      'not advertised by' "$CASE_LOG"

# CMD_QUERY_FORMAT: the VMM must be able to ASK before it commits to zero-copy,
# because a rejected ATTACH is not reported back to it.  The answer comes from
# the same resolver ATTACH uses, so a YES here is binding.
run_case '' --present 320x240 --query-format
check   "an advertised pair answers USABLE" \
        'fourcc=XR24 modifier=0x0000000000000000 -> USABLE' "$CASE_OUT"
check   "an alpha format its twin covers answers USABLE" \
        'fourcc=AR24 modifier=0x0000000000000000 -> USABLE' "$CASE_OUT"
check   "a foreign-vendor modifier answers NOT USABLE" \
        'modifier=0x0300000000606014 -> NOT USABLE' "$CASE_OUT"
check   "the broker logs the query and its verdict" \
        'QUERY_FORMAT .* -> NO' "$CASE_LOG"
nocheck "a query is not treated as a protocol violation" \
        'protocol violation' "$CASE_LOG"

# --linear-only: the forcing switch.  The test backend advertises XR24 with
# LINEAR *and* with MOD_INVALID; under the flag only LINEAR survives, so the
# advertised set shrinks and an implicit-modifier buffer stops being displayable.
# That is how the cross-vendor condition is reproduced on a single-GPU host.
BROKER_EXTRA=--linear-only
run_case '' --present 320x240 --query-format
check   "--linear-only narrows the advertised set to one pair" \
        'advertises 1 \(format, modifier\) pair' "$CASE_LOG"
check   "and LINEAR itself is still USABLE" \
        'fourcc=XR24 modifier=0x0000000000000000 -> USABLE' "$CASE_OUT"
check   "and a frame still reaches attach" 'TEST attach' "$CASE_LOG"

BROKER_EXTRA=
run_case '' --present 320x240 --query-format
check   "without it, both advertised pairs are kept" \
        'advertises 2 \(format, modifier\) pairs' "$CASE_LOG"

# TIER 3: a memfd must be accepted and presented through wl_shm.  This is the
# floor under a display that advertises a modifier it will not import, so it
# must work with NO modifier agreement at all -- even under --linear-only.
# --present-mode names one rung of the fallback ladder.  Each must narrow what
# is advertised, and `shm` must leave nothing at all -- that is what forces the
# VMM onto shared memory, the one buffer type a compositor cannot refuse.
BROKER_EXTRA=--present-mode=native
run_case '' --present 320x240
check   "--present-mode=native keeps every advertised pair" \
        'advertises 2 \(format, modifier\) pairs' "$CASE_LOG"
BROKER_EXTRA=--present-mode=linear
run_case '' --present 320x240
check   "--present-mode=linear narrows to LINEAR alone" \
        'advertises 1 \(format, modifier\) pair' "$CASE_LOG"
BROKER_EXTRA=--present-mode=shm
run_case '' --present 320x240
check   "--present-mode=shm advertises no dma-buf format at all" \
        'advertises 0 \(format, modifier\) pairs' "$CASE_LOG"
run_case '' --present 320x240 --shm
check   "and an F_SHM frame is still presented" 'TEST attach' "$CASE_LOG"
BROKER_EXTRA=

run_case '' --present 320x240 --shm --unadvertised-mod
check   "an F_SHM frame is accepted despite an unadvertised modifier" 'TEST attach' "$CASE_LOG"
nocheck "and is not rejected for that modifier" 'not advertised by' "$CASE_LOG"

# --resolution names what we SUGGEST to the guest, never what we require.  A
# bad value must be refused at startup rather than surfacing at frame time.
for bad in nonsense 0x0 99999x1; do
    "$BROKER" --backend test --resolution "$bad" --socket "$TMP/r.sock" \
        > "$TMP/res.log" 2>&1 </dev/null || true
    check "--resolution $bad is refused at startup" \
          'resolution must be' "$TMP/res.log"
done
for good in auto guest 1920x1080; do
    ( sleep 0.4 ) | "$BROKER" --backend test --resolution "$good" \
        --socket "$TMP/rg.sock" > "$TMP/resg.log" 2>&1 &
    sleep 0.8; wait 2>/dev/null
    check "--resolution $good is accepted" 'listening on' "$TMP/resg.log"
done

run_case '' --present 320x240 --bad-dim
check   "dimensions past the 8192 clamp are rejected" 'is out of range' "$CASE_LOG"
nocheck "and do not reach attach"                     'TEST attach'     "$CASE_LOG"

run_case '' --bad-fd
check   "an fd that is not a dma-buf is rejected" 'the fd is not a dma-buf' "$CASE_LOG"
nocheck "and does not reach attach"               'TEST attach'             "$CASE_LOG"

# ── 3. framing and fd intake: violations end the connection ──────────────────
echo
echo "-- protocol violations (disconnect, never partial recovery)"

run_case '' --bad-two-fds
check "two fds on one ATTACH is a violation" 'more than one fd on a single command' "$CASE_LOG"
check "and the client is disconnected"       'protocol violation'                   "$CASE_LOG"

run_case '' --bad-reserved
# Matches the FACT (a reserved field was non-zero and it was a violation), not
# the exact sentence.  This assertion silently stopped asserting when the
# message was reworded to name which reserved field -- a security check that
# passes only while a log string is stable is not a security check.
check "a non-zero reserved field is a violation" \
      'reserved[0-9]? is not zero|reserved fields are not zero' "$CASE_LOG"
check "and the reserved-field client is disconnected" \
      'protocol violation' "$CASE_LOG"

run_case '' --bad-commit-fd
check "an fd on a COMMIT is a violation" 'COMMIT carried an fd' "$CASE_LOG"

run_case '' --bad-frame
nocheck "a short message is never acted on" 'TEST commit' "$CASE_LOG"

# A client that sends raw junk instead of the protocol.
rm -f "$TMP/v.sock"
( sleep 3 ) | "$BROKER" --socket "$TMP/v.sock" --backend test \
    > "$TMP/violate.log" 2>&1 &
sleep 0.5
python3 - "$TMP/v.sock" <<'PY'
import socket, struct, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
time.sleep(0.3)
# A perfectly framed 40-byte command with a type nobody defines.  Reserved
# fields are zero, so this reaches the type switch rather than being caught
# by the cheaper reserved-field check first.
s.send(struct.pack('<HHIIIIIQII', 4242, 0, 0, 0, 0, 0, 0, 0, 0, 0))
time.sleep(0.5)
PY
sleep 0.3
check "an unknown command type is a violation" 'unknown command type' "$TMP/violate.log"
wait 2>/dev/null

# ── 4. WINDOW ────────────────────────────────────────────────────────────────
echo
echo "-- window resize"

run_case '' --window 800x600
check "a WINDOW request is answered with SURFACE" 'SURFACE .*x=800 y=600' "$CASE_OUT"

# ── 5. input policy state machine ────────────────────────────────────────────
echo
echo "-- input policy state machine"

run_case 'k 30 1
k 30 0
'
nocheck "no KEY while unfocused" '[[:space:]]KEY[[:space:]]' "$CASE_OUT"

run_case 'f 1
k 30 1
k 30 0
'
check "KEY flows once focused" '[[:space:]]KEY[[:space:]]+flags=-F x=30 y=1' "$CASE_OUT"

run_case 'f 1
a 10 20
'
nocheck "no ABS while the pointer is outside" '[[:space:]]ABS[[:space:]]' "$CASE_OUT"

run_case 'f 1
p 1
a 10 20
'
check "ABS flows when focused and inside" '[[:space:]]ABS[[:space:]]+flags=-F x=10 y=20' "$CASE_OUT"

# 29=LEFTCTRL 56=LEFTALT 34=G
run_case 'f 1
p 1
k 29 1
k 56 1
k 34 1
k 34 0
'
check   "CTRL+ALT+G turns grab on"        'GRAB[[:space:]]+flags=G' "$CASE_OUT"
nocheck "the G keypress is NOT forwarded" 'KEY[[:space:]]+.*x=34'   "$CASE_OUT"

run_case 'f 1
p 1
k 29 1
k 56 1
k 34 1
k 34 0
r 5 -3
a 10 20
'
check   "REL flows under grab"         'REL[[:space:]]+flags=G. x=5 y=-3' "$CASE_OUT"
nocheck "ABS is suppressed under grab" 'ABS[[:space:]]+flags=G'           "$CASE_OUT"

run_case 'f 1
p 1
k 29 1
k 56 1
k 34 1
k 34 0
k 30 1
f 0
'
check "focus loss releases the held key" 'KEY[[:space:]]+.*x=30 y=0'         "$CASE_OUT"
check "focus loss drops the grab"        'grab dropped: the window lost focus' "$CASE_LOG"
check "the client is told the grab ended" 'GRAB[[:space:]]+flags=--'         "$CASE_OUT"

echo
echo "$N checks run"
if [ "$FAIL" -eq 0 ]; then
    echo "all checks passed"
else
    echo "FAILURES above"
fi
exit "$FAIL"
