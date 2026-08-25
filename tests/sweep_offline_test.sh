#!/usr/bin/env bash
#
# tests/sweep_offline_test.sh -- exercise scripts/sweep.sh's dangerous paths
#                                without renting anything.
#
# WHY THIS EXISTS
#   The parts of sweep.sh that matter most are the parts you only reach by
#   spending money: creating an instance, deciding whether a box is slow or
#   dead, destroying it and proving it is gone, and arming a timer that
#   survives the sweep being killed.  Testing those against real vast.ai costs
#   dollars per run and takes ~30 minutes per box, so in practice they would
#   never be tested at all -- which is exactly how an unattended spender
#   acquires a bug that only shows up as a bill.
#
#   So: sweep.sh is sourced in library mode (NVKVM_SWEEP_LIB=1) and driven
#   against a STUBBED `vastai` on PATH whose behaviour is scripted per test.
#   The code under test is the real code, byte for byte -- only the vast.ai
#   API is faked.
#
#   What this CANNOT cover, and is not claimed to: anything that happens on the
#   far side of ssh (building QEMU, installing a driver, booting the guest,
#   running validate.sh).  Those need real hardware.
#
# Usage:  bash tests/sweep_offline_test.sh
# Exit :  0 all passed, 1 otherwise.
#
set -u -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
ok()   { printf '  \033[32mPASS\033[0m %s\n' "$*"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want '$3', got '$2')"; fi; }

# ---------------------------------------------------------------------------
# the stub vastai
#
# State lives in $STATE: instances.json is the listing, logs/<id>.txt is what
# `vastai logs` returns, and destroy_deaf.txt names instances that IGNORE a
# destroy -- which is how the real CLI behaves when it hits its "[y/N]" prompt
# with no tty, prints "Aborted." and exits 0.
# ---------------------------------------------------------------------------
STATE="$TMP/state"
mkdir -p "$STATE/logs" "$TMP/bin"
echo '[]' >"$STATE/instances.json"
echo '[]' >"$STATE/offers.json"
: >"$STATE/destroy_deaf.txt"
: >"$STATE/create_output.txt"
: >"$STATE/destroy_calls.txt"

cat >"$TMP/bin/vastai" <<'STUB'
#!/usr/bin/env bash
# stub vastai -- prints the deprecation banner on STDERR exactly like the real
# one (v1.0.10), so any caller that forgets 2>/dev/null gets unparseable JSON
# and the test catches it.
echo "DEPRECATED: \`vastai show instances\` will be removed in a future release." >&2
S="$VASTAI_STUB_STATE"
cmd="${1:-}"; sub="${2:-}"
# `vastai logs <id> --tail N` -- the id is $2. Matched on $1 alone, before the
# two-word dispatch below, so the trailing flags cannot be mistaken for the id.
if [ "$cmd" = "logs" ]; then
    f="$S/logs/$sub.txt"
    [ -f "$f" ] && cat "$f"
    exit 0
fi
case "$cmd $sub" in
  "show instances")
      cat "$S/instances.json" ;;
  "show instance")
      python3 -c '
import json,sys
d=json.load(open(sys.argv[1]))
for i in d:
    if str(i.get("id"))==sys.argv[2]:
        print(json.dumps(i)); break
else:
    print("{}")
' "$S/instances.json" "$3" ;;
  "search offers")
      cat "$S/offers.json" ;;
  "create instance")
      cat "$S/create_output.txt" ;;
  "destroy instance")
      id="$3"
      echo "$id" >> "$S/destroy_calls.txt"
      if grep -qx "$id" "$S/destroy_deaf.txt"; then
          echo "Aborted."            # the real prompt-with-no-tty behaviour
          exit 0
      fi
      python3 -c '
import json,sys
p=sys.argv[1]
d=[i for i in json.load(open(p)) if str(i.get("id"))!=sys.argv[2]]
json.dump(d,open(p,"w"))
' "$S/instances.json" "$id"
      echo "destroyed $id" ;;
  *) : ;;
esac
exit 0
STUB
chmod +x "$TMP/bin/vastai"
export VASTAI_STUB_STATE="$STATE"
export PATH="$TMP/bin:$PATH"

# helpers to script the stub's state
set_instances() { printf '%s' "$1" >"$STATE/instances.json"; }
set_log()       { printf '%s' "$2" >"$STATE/logs/$1.txt"; }

# ---------------------------------------------------------------------------
# source the real sweep.sh, functions only
# ---------------------------------------------------------------------------
export NVKVM_SWEEP_LIB=1
export NVKVM_SWEEP_POLL=1
export NVKVM_SWEEP_DEADCHECK_SETTLE=1
export NVKVM_SWEEP_DESTROY_SETTLE=0      # assert the retry COUNT, not the wall clock
export NVKVM_SWEEP_DESTROY_BACKOFF=0
set --                       # sweep.sh parses "$@"; give it nothing
# shellcheck disable=SC1090
. "$REPO/scripts/sweep.sh"

# Point the mutable bits at the sandbox so the test never writes to the repo.
KNOWN_BAD_FILE="$TMP/known-bad.txt"
: >"$KNOWN_BAD_FILE"
REGISTRY="$TMP/registry"
: >"$REGISTRY"
RESULTS="$TMP/sweep.jsonl"
: >"$RESULTS"
STOP_FILE="$TMP/stop"
MAX_DPH=0.50
PROTECTED="99999999"

echo
echo "=== 1. the vastai stub really does print the banner on stderr ==="
banner="$(vastai show instances --raw 2>&1 >/dev/null | head -1)"
case "$banner" in DEPRECATED*) ok "stub reproduces the stderr deprecation banner" ;;
                  *) bad "stub banner missing" ;; esac
parsed="$(vj show instances | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))' 2>/dev/null)"
check "vj() strips stderr so the JSON parses" "$parsed" "0"

echo
echo "=== 2. parse_contract tolerates every shape the CLI emits ==="
check "python-repr with prose prefix" \
  "$(printf "%s" "Started. {'success': True, 'new_contract': 48512345}" | parse_contract)" "48512345"
check "clean json" \
  "$(printf '%s' '{"success": true, "new_contract": 48599999}' | parse_contract)" "48599999"
check "success:false STILL yields the id (a live contract exists!)" \
  "$(printf "%s" "{'success': False, 'new_contract': 48577777}" | parse_contract)" "48577777"
check "trailing 'launched successfully' line" \
  "$(printf '%s' 'thing launched successfully: 48566666' | parse_contract)" "48566666"
check "genuinely nothing -> empty" "$(printf '%s' 'error: no offer' | parse_contract)" ""

echo
echo "=== 3. expected ABI profile comes from nvkvm_abi.h itself ==="
build_abiq >/dev/null 2>&1
check "570.124.06" "$(abi_expected 570.124.06)" "570"
check "580.95.05"  "$(abi_expected 580.95.05)"  "580"
check "595.84 (top of the 580..595 claim)" "$(abi_expected 595.84)" "580"
check "610.57.04" "$(abi_expected 610.57.04)" "610"
check "535.54.03 intra-branch LOW"  "$(abi_expected 535.54.03)" "525"
check "535.86.05 intra-branch HIGH" "$(abi_expected 535.86.05)" "535"

echo
echo "=== 4. the driver set crosses the profile boundaries, >=5 per arch ==="
load_matrix
for a in turing ampere ada hopper blackwell; do
    n="$(drivers_for_arch "$a" | wc -l)"
    profs="$(drivers_for_arch "$a" | cut -f3 | sort -u | tr '\n' ' ')"
    if [ "$n" -ge 5 ]; then ok "$a: $n drivers spanning profiles: $profs"
    else bad "$a: only $n drivers (need >=5)"; fi
done
check "blackwell excludes the sub-570 row (floor)" \
  "$(drivers_for_arch blackwell | cut -f1 | grep -c '^565')" "0"
check "610.88 is NOT in the set (it does not exist for Linux)" \
  "$(drivers_for_arch ampere | cut -f1 | grep -c '610.88')" "0"
check "nothing in the empty 596-609 range" \
  "$(drivers_for_arch ampere | cut -f1 | awk -F. '$1>=596 && $1<=609' | wc -l)" "0"

echo
echo "=== 5. known-bad machine list ==="
printf '42636\t# seeded\n' >"$KNOWN_BAD_FILE"
if known_bad 42636; then ok "seeded machine is recognised"; else bad "seeded machine not recognised"; fi
if known_bad 12345; then bad "unknown machine wrongly blacklisted"; else ok "unknown machine is not blacklisted"; fi
blacklist_machine 44906 "test reason" 2>/dev/null
if known_bad 44906; then ok "blacklist_machine appends and is read back"; else bad "blacklist_machine did not persist"; fi
before="$(wc -l <"$KNOWN_BAD_FILE")"
blacklist_machine 44906 "again" 2>/dev/null
check "blacklisting twice does not duplicate" "$(wc -l <"$KNOWN_BAD_FILE")" "$before"

echo
echo "=== 6. offer selection: arch, price cap, known-bad, already-tried ==="
cat >"$STATE/offers.json" <<'OFF'
[{"id":1,"machine_id":42636,"gpu_name":"RTX 3090","dph_total":0.05,"geolocation":"X","inet_down":1,"driver_version":"575.51.03"},
 {"id":2,"machine_id":50000,"gpu_name":"RTX 3060","dph_total":0.10,"geolocation":"Y","inet_down":1,"driver_version":"575.51.03"},
 {"id":3,"machine_id":50001,"gpu_name":"RTX 4090","dph_total":0.20,"geolocation":"Z","inet_down":1,"driver_version":"575.51.03"},
 {"id":4,"machine_id":50002,"gpu_name":"Quadro P4000","dph_total":0.01,"geolocation":"W","inet_down":1,"driver_version":"575.51.03"},
 {"id":5,"machine_id":50003,"gpu_name":"RTX 5090","dph_total":9.99,"geolocation":"V","inet_down":1,"driver_version":"575.51.03"}]
OFF
TRIED_MACHINES=""
check "ampere skips the known-bad machine and takes the next" \
  "$(pick_offer ampere | cut -f1)" "2"
check "ada picks the Ada card" "$(pick_offer ada | cut -f1)" "3"
check "pre-Turing (P4000) is never chosen despite being cheapest" \
  "$(pick_offer turing | cut -f1)" ""
check "blackwell over the price cap -> no offer" "$(pick_offer blackwell | cut -f1)" ""
TRIED_MACHINES="50000"
check "a machine already tried this run is not re-picked" \
  "$(pick_offer ampere | cut -f1)" ""
TRIED_MACHINES=""

echo
echo "=== 7. dead box vs slow box -- the log is the ONLY signature ==="
DEAD_LOG="libvirt: QEMU Driver error : Domain not found: no domain with matching name 'C.777'"
set_log 777 "$DEAD_LOG"
DEADCHECK_HASH=""; DEADCHECK_WHEN=0
if box_is_dead 777 loading; then bad "'loading' must NEVER be called dead"; else ok "'loading' is never dead, whatever the log says"; fi
DEADCHECK_HASH=""; DEADCHECK_WHEN=0
if box_is_dead 777 created; then bad "condemned on the FIRST sighting (no settle window)"; else ok "first sighting is not enough -- needs the log to be frozen"; fi
sleep 2
if box_is_dead 777 created; then ok "created + frozen 'Domain not found' across the settle window -> DEAD"; else bad "did not detect the dead-box signature"; fi
set_log 888 "pulling image layer 3/9 ..."
DEADCHECK_HASH=""; DEADCHECK_WHEN=0
box_is_dead 888 created >/dev/null; sleep 2
if box_is_dead 888 created; then bad "a box still pulling its image was called dead"; else ok "an image-pulling box is never called dead"; fi
set_log 999 "$DEAD_LOG"
DEADCHECK_HASH=""; DEADCHECK_WHEN=0
box_is_dead 999 created >/dev/null
set_log 999 "$DEAD_LOG
now something new is happening"
sleep 2
if box_is_dead 999 created; then bad "a MOVING log was called frozen"; else ok "a log that is still moving is not frozen"; fi

echo
echo "=== 8. wait_for_box: blacklist-worthy death vs mere timeout ==="
set_instances '[{"id":777,"actual_status":"created","machine_id":42636}]'
set_log 777 "$DEAD_LOG"
WAIT_BUDGET=6; CUR_MACHINE=42636
wait_for_box 777 >/dev/null 2>&1; rc=$?
check "dead box -> rc=1 (blacklist and re-rent elsewhere)" "$rc" "1"
set_instances '[{"id":888,"actual_status":"loading","machine_id":50000}]'
set_log 888 "still pulling"
WAIT_BUDGET=3
wait_for_box 888 >/dev/null 2>&1; rc=$?
check "slow box -> rc=2 (timeout, machine NOT blacklisted)" "$rc" "2"
if known_bad 50000; then bad "a merely slow machine got blacklisted"; else ok "a merely slow machine is NOT blacklisted"; fi

echo
echo "=== 9. destroy_verified proves it, and never trusts the exit code ==="
set_instances '[{"id":111,"actual_status":"running","label":"nvkvm-sweep"}]'
: >"$STATE/destroy_deaf.txt"
if destroy_verified 111 >/dev/null 2>&1; then ok "destroy that really removes the instance -> success"; else bad "destroy of a removable instance failed"; fi
check "and the listing agrees it is gone" "$(live_instance_ids | grep -cx 111)" "0"

set_instances '[{"id":222,"actual_status":"running","label":"nvkvm-sweep"}]'
echo 222 >"$STATE/destroy_deaf.txt"
LEAK_SUSPECTED=0
if destroy_verified 222 >/dev/null 2>&1; then
    bad "a destroy that silently did NOTHING was reported as success"
else
    ok "a destroy that exits 0 but leaves the box alive is caught as a FAILURE"
fi
check "  ...and it raises the leak flag" "$LEAK_SUSPECTED" "1"
ndestroy="$(grep -cx 222 "$STATE/destroy_calls.txt")"
check "  ...after retrying (5 attempts), not giving up after one" \
  "$([ "$ndestroy" -ge 5 ] && echo yes || echo "no($ndestroy)")" "yes"
: >"$STATE/destroy_deaf.txt"

echo
echo "=== 10. protected instances are refused ==="
set_instances '[{"id":99999999,"actual_status":"running","label":"nvkvm-sweep"}]'
if destroy_verified 99999999 >/dev/null 2>&1; then bad "a PROTECTED instance was destroyed"; else ok "a protected instance is refused"; fi
check "  ...and is still alive" "$(live_instance_ids | grep -cx 99999999)" "1"

echo
echo "=== 11. stray reaping only ever touches our own label ==="
set_instances '[{"id":301,"actual_status":"running","label":"nvkvm-sweep","gpu_name":"RTX 3060"},
                {"id":302,"actual_status":"running","label":"uvm-fi","gpu_name":"RTX 5090"},
                {"id":303,"actual_status":"running","label":null,"gpu_name":"RTX 3090"}]'
reap_strays >/dev/null 2>&1
check "our labelled stray is destroyed"      "$(live_instance_ids | grep -cx 301)" "0"
check "another agent's labelled box survives" "$(live_instance_ids | grep -cx 302)" "1"
check "an unlabelled box survives"            "$(live_instance_ids | grep -cx 303)" "1"

echo
echo "=== 12. reconcile shouts about a leak instead of exiting quietly ==="
set_instances '[{"id":401,"actual_status":"running","label":"nvkvm-sweep"}]'
printf '401\n' >"$REGISTRY"
LEAK_SUSPECTED=0
reconcile >/dev/null 2>&1; rc=$?
check "a surviving registered instance -> reconcile fails" "$rc" "1"
check "  ...and raises the leak flag"                      "$LEAK_SUSPECTED" "1"
set_instances '[]'
LEAK_SUSPECTED=0
reconcile >/dev/null 2>&1
check "nothing alive -> reconcile passes" "$LEAK_SUSPECTED" "0"

echo
echo "=== 13. resumability: answers are kept, harness failures are retried ==="
: >"$RESULTS"
emit "$(jrec arch ampere driver 580.95.05 status pass)"
emit "$(jrec arch ampere driver 595.84   status fail)"
emit "$(jrec arch ampere driver 610.57.04 status driver-install-failed)"
emit "$(jrec arch turing driver 580.95.05 status box-never-provisioned)"
if unit_done ampere 580.95.05; then ok "a PASS is terminal -- not paid for twice"; else bad "pass not treated as done"; fi
if unit_done ampere 595.84;   then ok "a FAIL is terminal -- it is an answer"; else bad "fail not treated as done"; fi
if unit_done ampere 610.57.04; then bad "an install failure was treated as done"; else ok "an install failure is RETRIED, not banked"; fi
if unit_done turing 580.95.05; then bad "a dead box was treated as done"; else ok "a box that never provisioned is RETRIED"; fi
if unit_done ada 580.95.05; then bad "an unrelated arch matched"; else ok "the ledger is keyed on (arch, driver)"; fi

echo
echo "=== 14. jrec builds valid JSON and nests validate.sh's own output ==="
r="$(jrec arch ampere status pass validate:json '{"pass":28,"fail":0}' note "quote \" and 'tick'")"
check "nested json is a real object" \
  "$(printf '%s' "$r" | python3 -c 'import json,sys; print(json.load(sys.stdin)["validate"]["pass"])')" "28"
check "awkward characters survive" \
  "$(printf '%s' "$r" | python3 -c 'import json,sys; print("tick" in json.load(sys.stdin)["note"])')" "True"
r2="$(jrec a b validate:json 'not json at all')"
check "unparseable json is flagged, not silently dropped" \
  "$(printf '%s' "$r2" | python3 -c 'import json,sys; print("validate_unparsed" in json.load(sys.stdin))')" "True"

echo
echo "=== 15. the standalone auto-destroy timer actually destroys ==="
set_instances '[{"id":501,"actual_status":"running","label":"nvkvm-sweep"}]'
: >"$STATE/destroy_deaf.txt"
TREG="$TMP/timer-registry"; echo 501 >"$TREG"
TLOG="$TMP/timer.log"
NVKVM_SWEEP_PROTECT="" VASTAI_BIN="$TMP/bin/vastai" \
    setsid nohup bash "$REPO/scripts/sweep_autodestroy.sh" "$(( $(date +%s) + 3 ))" "$TREG" "$TLOG" >/dev/null 2>&1 &
tpid=$!
sleep 1
if ps -p "$tpid" -o args= 2>/dev/null | grep -q sweep_autodestroy.sh; then
    ok "timer is a real process, verifiable with ps by exact pid"
else
    bad "timer did not come up under its own pid"
fi
for _ in $(seq 1 30); do live_instance_ids | grep -qx 501 || break; sleep 1; done
check "timer destroyed the registered instance at its deadline" \
  "$(live_instance_ids | grep -cx 501)" "0"
wait "$tpid" 2>/dev/null

echo
echo "=== 16. the timer refuses to destroy a protected instance ==="
set_instances '[{"id":601,"actual_status":"running","label":"nvkvm-sweep"}]'
TREG2="$TMP/timer-registry2"; echo 601 >"$TREG2"
TLOG2="$TMP/timer2.log"
NVKVM_SWEEP_PROTECT="601" VASTAI_BIN="$TMP/bin/vastai" \
    bash "$REPO/scripts/sweep_autodestroy.sh" "$(( $(date +%s) + 2 ))" "$TREG2" "$TLOG2" >/dev/null 2>&1
check "protected instance survives its own registry entry" \
  "$(live_instance_ids | grep -cx 601)" "1"
if grep -q "REFUSING to destroy protected" "$TLOG2"; then ok "and the refusal is logged"; else bad "refusal not logged"; fi

echo
echo "=== 17. the timer stands down only when the LISTING agrees ==="
set_instances '[]'
TREG3="$TMP/timer-registry3"; echo 701 >"$TREG3"; : >"${TREG3}.done"
TLOG3="$TMP/timer3.log"
timeout 60 env VASTAI_BIN="$TMP/bin/vastai" NVKVM_SWEEP_PROTECT="" \
    bash "$REPO/scripts/sweep_autodestroy.sh" "$(( $(date +%s) + 900 ))" "$TREG3" "$TLOG3" >/dev/null 2>&1
rc=$?
check "done + nothing alive -> exits early instead of idling for 15m" "$rc" "0"
if grep -q "standing down" "$TLOG3"; then ok "and says why"; else bad "no stand-down line"; fi

set_instances '[{"id":702,"actual_status":"running","label":"nvkvm-sweep"}]'
TREG4="$TMP/timer-registry4"; echo 702 >"$TREG4"; : >"${TREG4}.done"
TLOG4="$TMP/timer4.log"
timeout 12 env VASTAI_BIN="$TMP/bin/vastai" NVKVM_SWEEP_PROTECT="" \
    bash "$REPO/scripts/sweep_autodestroy.sh" "$(( $(date +%s) + 900 ))" "$TREG4" "$TLOG4" >/dev/null 2>&1
if grep -q "STILL ALIVE" "$TLOG4"; then
    ok "'the sweep said it finished' alone does NOT disarm the net"
else
    bad "timer stood down while a registered instance was still alive"
fi

echo
echo "=== 18. spend cap is enforced before a create, not tallied after ==="
SPENT=0; MAX_SPEND=1.00
if spend_room 0.50; then ok "0.50 fits under a 1.00 cap"; else bad "cap rejected a fitting spend"; fi
spend_add 0.90
check "spend accumulates" "$SPENT" "0.9"
if spend_room 0.50; then bad "cap allowed a spend that would exceed it"; else ok "cap refuses the spend that would breach it"; fi

echo
echo "=== 19. a remote command must not eat the driver loop's stdin ==="
# REGRESSION.  rsh_t runs inside `while IFS= read ... done <<< \"\$todo\"`, and
# ssh reads stdin.  Without `< /dev/null` the first remote command swallows the
# rest of the driver list, so a box tests ONE driver and reports a clean sweep
# -- silent undercoverage, the exact thing this script exists to prevent.
cat >"$TMP/bin/ssh" <<'SSHSTUB'
#!/usr/bin/env bash
cat > /dev/null      # a real ssh consumes stdin; so does this
echo remote-ok
SSHSTUB
chmod +x "$TMP/bin/ssh"
SSH="ssh -p 22 root@example"
iterations=0
while IFS= read -r line; do
    iterations=$((iterations+1))
    rsh_t 5 "echo $line" >/dev/null 2>&1
done <<<"$(printf 'a\nb\nc\nd\ne\nf\n')"
check "all 6 driver rows are visited, not just the first" "$iterations" "6"
SSH=""
rsh_t 5 'rm -rf /definitely/not/local' >/dev/null 2>&1; rc=$?
check "an empty ssh target REFUSES to run the command locally" "$rc" "97"
rm -f "$TMP/bin/ssh"

echo
echo "=== 20. both NVIDIA kernel-module banners identify the running driver ==="
check "proprietary NVRM banner" \
  "$(printf '%s\n' 'NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.95.05  Release Build' | parse_nvrm_driver_version)" \
  "580.95.05"
check "open NVRM banner" \
  "$(printf '%s\n' 'NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  580.105.08  Release Build' | parse_nvrm_driver_version)" \
  "580.105.08"

echo
echo "======================================================================"
printf 'sweep offline suite: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
