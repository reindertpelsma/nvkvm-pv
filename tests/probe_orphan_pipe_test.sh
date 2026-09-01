#!/usr/bin/env bash
# Regression test for the wedge MEASURED on 2026-09-01 on a GTX 750 Ti, and
# almost certainly the same one that ate ~43 minutes on a Quadro P4000.
#
# THE BUG, in one sentence: `out="$(cmd)"` reads until EOF, EOF requires every
# write end of that pipe to be closed, and a grandchild that outlives `cmd`
# still holds one -- so the shell blocks forever even though the process it
# bounded is long dead.
#
# Why no timeout could ever have fixed it: the bound applies to the CHILD. The
# shell is not waiting on the child, it is waiting on the PIPE. The child was
# reaped on schedule and the read still never returned.
#
# Measured shape of the real thing:
#   validate.sh   pid 2340  Ss  pipe_read   fd3 -> pipe:[16608]   (read end)
#   orphan probe  pid 2483  S   pipe_read   fd1,fd2 -> pipe:[16608]  ppid=1
# tests/repro/shared_view_desync.c forks BY DESIGN -- it is a two-process
# coherence test -- so the orphan is the normal case here, not a freak event.
set -u
PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); printf '  PASS %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  FAIL %s -- %s\n' "$1" "$2"; }
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# A stand-in for the real repro: fork a child that outlives the parent and
# keeps stdout open. No GPU, no libcuda, no network.
cat > "$WORK/leaky" <<'LEAKY'
#!/usr/bin/env bash
( sleep 30 ) &          # inherits stdout; still alive when the parent exits
echo "parent-output"
exit 0
LEAKY
chmod +x "$WORK/leaky"

# The bound under test: background, poll, escalate -- never a blocking wait.
bounded() {
    local budget="$1"; shift
    "$@" & local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        [ "$waited" -ge "$budget" ] && { kill -9 "$pid" 2>/dev/null; break; }
        sleep 1; waited=$((waited+1))
    done
    wait "$pid" 2>/dev/null || true
}

# --- 1. the bug reproduces: capture via $( ) outlives its own bound ---------
start=$(date +%s)
( out="$(bounded 2 "$WORK/leaky" 2>&1)"; printf '%s' "$out" > "$WORK/viapipe" ) &
cap=$!
waited=0
while kill -0 "$cap" 2>/dev/null && [ "$waited" -lt 8 ]; do sleep 1; waited=$((waited+1)); done
if kill -0 "$cap" 2>/dev/null; then
    ok "capture-via-pipe still blocked after 8s despite a 2s bound (the bug)"
    kill -9 "$cap" 2>/dev/null; wait "$cap" 2>/dev/null || true
else
    bad "capture-via-pipe" "expected it to block on the orphan's write end, it returned in $(( $(date +%s) - start ))s"
fi

# --- 2. the fix does not: redirect to a file, then read the file ------------
start=$(date +%s)
( bounded 2 "$WORK/leaky" > "$WORK/out.txt" 2>&1; cat "$WORK/out.txt" > "$WORK/viafile" ) &
cap=$!
waited=0
while kill -0 "$cap" 2>/dev/null && [ "$waited" -lt 8 ]; do sleep 1; waited=$((waited+1)); done
if kill -0 "$cap" 2>/dev/null; then
    kill -9 "$cap" 2>/dev/null; wait "$cap" 2>/dev/null || true
    bad "capture-via-file" "still blocked after 8s -- the fix does not hold"
else
    ok "capture-via-file returned in $(( $(date +%s) - start ))s, orphan or not"
    grep -q parent-output "$WORK/viafile" 2>/dev/null \
        && ok "capture-via-file still captured the output" \
        || bad "capture-via-file" "output was lost: $(cat "$WORK/viafile" 2>/dev/null)"
fi

# --- 3. validate.sh must not reintroduce a capturing call site --------------
V="$(dirname "$0")/validate.sh"
if [ -f "$V" ]; then
    n=$(grep -c 'sh_bounded' "$V" | head -1)
    bad_sites=$(grep -n 'sh_bounded' "$V" | grep '\$(' | grep -v '^\s*#' | grep -vE ':\s*#' || true)
    if [ -z "$bad_sites" ]; then
        ok "validate.sh has no \$( ) capture around sh_bounded ($n references checked)"
    else
        bad "validate.sh capture sites" "$(printf '%s' "$bad_sites" | head -2 | tr '\n' ' ')"
    fi
fi

printf '\n%d/%d checks passed\n' "$PASS" "$((PASS+FAIL))"
[ "$FAIL" -eq 0 ]
