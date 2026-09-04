#!/usr/bin/env bash
# module_cycle_test.sh -- sustained load/unload of the guest module.
#
# WHY THIS EXISTS
#
# On 2026-09-04 a fresh SteamOS guest rebuilt the module during boot and then
# hot-swapped it (rmmod + modprobe) on the live system.  ~400 ms later the
# guest panicked:
#
#   Oops: general protection fault, probably for non-canonical address
#   0x38d3e47b3d040a39 ... RIP: __kmalloc_cache_noprof+0x1c3/0x410
#   Kernel panic - not syncing: Fatal exception
#
# The faulting instruction was `mov r8,[rdi+rax]` with RDI a non-canonical
# value: a corrupted SLUB freelist pointer.  The reported call trace
# (unshare(CLONE_NEWNET) -> ipv4_sysctl_init_net -> kmalloc) was an innocent
# victim -- the next allocation out of a cache that something else had already
# poisoned.  The kernel named the suspect itself:
#
#   Unloaded tainted modules: nvkvm_guest(OE):1 [last unloaded: nvkvm_guest(OE)]
#
# rmmod REPORTED SUCCESS.  That is the defect.  A module must either unload
# cleanly or refuse to unload; succeeding while leaving something behind that
# writes into freed memory is the one outcome that is not allowed.  Both
# file_operations already carry .owner = THIS_MODULE, so an open fd pins the
# module and rmmod fails honestly with EWOULDBLOCK/EAGAIN -- that part is
# correct, and this test asserts it stays correct.  Whatever still dangles
# after a *clean* unload is what this test is built to catch.
#
# THE CONTRACT UNDER TEST
#   1. While userspace holds an fd, rmmod must REFUSE and the module must stay
#      loaded.  An honest EAGAIN beats a successful lie.
#   2. N load/unload cycles must produce no new BUG/Oops/WARNING/GPF/KASAN.
#   3. Slab usage must not grow without bound across those cycles.
#
# RUN IT UNDER POISONING.  Without slub_debug the corrupting write is only
# noticed later, by an unrelated victim, which is exactly what made the
# original panic so misleading.  Boot the guest with:
#
#     slub_debug=FZP page_poison=1
#
# and this test reports at the point of the bad access instead of 400 ms
# downstream.  KASAN is better still if the guest kernel has it.
#
# WHERE TO RUN IT.  Both stock guest flavours build the module in a mktemp
# work dir, insmod it, and delete the .ko on the way out (setup_guest.sh's
# nvkvm-guest.service traps EXIT).  So a running guest normally has the module
# LOADED and no module FILE, and nothing can reload it.  Build one that stays:
#
#   sudo apt-get install -y build-essential "linux-headers-$(uname -r)"
#   cp -a /mnt/nvkvm/src/{guest,common,abi} /root/src/
#   make -C /root/src/guest KDIR=/lib/modules/$(uname -r)/build
#   sudo NVKVM_KO=/root/src/guest/nvkvm-guest.ko bash module_cycle_test.sh 50
#
# Usage:  sudo bash module_cycle_test.sh [cycles]      (default 50)
set -u

MODULE="${NVKVM_MODULE:-nvkvm_guest}"
CYCLES="${1:-${NVKVM_CYCLES:-50}}"
CTL_DEV="${NVKVM_CTL_DEV:-/dev/nvidiactl}"
SLAB_TOLERANCE_KB="${NVKVM_SLAB_TOLERANCE_KB:-1024}"

N_PASS=0; N_FAIL=0
pass() { N_PASS=$((N_PASS+1)); printf '  PASS  %-34s %s\n' "$1" "${2:-}"; }
fail() { N_FAIL=$((N_FAIL+1)); printf '  FAIL  %-34s %s\n' "$1" "${2:-}"; }

[ "$(id -u)" -eq 0 ] || { echo "must run as root"; exit 2; }

loaded()   { grep -qw "^$MODULE" /proc/modules; }

# Resolve the .ko ONCE, up front.
#
# modprobe only works if the module was installed into /lib/modules and
# depmod has seen it.  The container image guest does not do that -- it
# insmod's the module straight from the staged share -- so a modprobe-only
# test reports "modprobe failed after a clean unload" on cycle 1 and blames
# nvkvm for what is really a missing modules.dep entry.  (That is exactly
# what the first run of this test did.)  Find the file, prefer modprobe when
# it can resolve the name, and fall back to insmod on the path.
KO=""
resolve_ko() {
    [ -n "${NVKVM_KO:-}" ] && { KO="$NVKVM_KO"; return 0; }
    KO="$(modinfo -n "$MODULE" 2>/dev/null)" && [ -n "$KO" ] && [ -f "$KO" ] && return 0
    # The FILE is nvkvm-guest.ko; the MODULE is nvkvm_guest. Search both
    # spellings -- looking only for ${MODULE}.ko finds nothing and the test
    # then calls a perfectly testable host UNTESTABLE.
    _alt="$(printf '%s' "$MODULE" | tr '_' '-')"
    for n in "$MODULE" "$_alt"; do
        for c in "/lib/modules/$(uname -r)/updates/${n}.ko" \
                 "/usr/lib/modules/$(uname -r)/updates/${n}.ko" \
                 "/mnt/nvkvm/src/guest/${n}.ko" \
                 "/opt/nvkvm/src/guest/${n}.ko" \
                 "/root/${n}.ko"; do
            [ -f "$c" ] && { KO="$c"; return 0; }
        done
    done
    KO=""
    return 1
}
load_module() {
    modprobe "$MODULE" 2>/dev/null && return 0
    [ -n "$KO" ] && insmod "$KO" 2>/dev/null && return 0
    return 1
}
slab_kb()  { awk '/^Slab:/ {print $2}' /proc/meminfo; }
dmesg_len(){ dmesg 2>/dev/null | wc -l; }

# Anything that means the kernel noticed corruption. Deliberately broad:
# a WARNING during teardown is a defect here even if nothing panics.
FAULT_RE='BUG:|Oops|general protection|kernel NULL pointer|KASAN|slab corruption|Object padding|Redzone|Freepointer corrupt|WARNING:|list_add corruption|refcount_t'

new_faults() {   # $1 = dmesg line count baseline
    dmesg 2>/dev/null | tail -n +"$(( $1 + 1 ))" | grep -E "$FAULT_RE" | head -20
}

echo "== nvkvm module cycle test =="
echo "   module=$MODULE cycles=$CYCLES ctl=$CTL_DEV"
grep -qE 'slub_debug|kasan' /proc/cmdline 2>/dev/null \
    && echo "   poisoning: $(tr ' ' '\n' < /proc/cmdline | grep -E 'slub_debug|kasan' | tr '\n' ' ')" \
    || echo "   poisoning: NONE -- reruns with slub_debug=FZP are strictly stronger"
echo

resolve_ko
if [ -n "$KO" ]; then echo "   module file: $KO"
else echo "   module file: NOT FOUND (modprobe-only; set NVKVM_KO=/path/to/${MODULE}.ko)"; fi
loaded || load_module
loaded || { echo "cannot load $MODULE -- nothing to test"; exit 2; }

# A reload we cannot perform is not a failure of the module. Prove up front
# that this host can put it back, and bail as UNTESTABLE if it cannot.
if ! modprobe -n "$MODULE" >/dev/null 2>&1 && [ -z "$KO" ]; then
    echo "cannot RELOAD $MODULE here: modprobe cannot resolve it and no .ko was found."
    echo "This host cannot run a load/unload cycle -- not a verdict on the module."
    exit 2
fi

# ── 1. Honest refusal while an fd is open ────────────────────────────────────
if [ -e "$CTL_DEV" ]; then
    exec 9<"$CTL_DEV" 2>/dev/null
    if [ $? -eq 0 ]; then
        rmmod_err="$(rmmod "$MODULE" 2>&1)"; rmmod_rc=$?
        if [ $rmmod_rc -eq 0 ] || ! loaded; then
            fail "refuses-unload-while-open" "rmmod SUCCEEDED with $CTL_DEV open -- module gone"
        else
            pass "refuses-unload-while-open" "rc=$rmmod_rc ${rmmod_err:-(module still resident)}"
        fi
        exec 9<&-
    else
        echo "  (could not open $CTL_DEV; skipping the refusal check)"
    fi
else
    echo "  (no $CTL_DEV; skipping the refusal check)"
fi

# ── 2 + 3. Sustained cycles, watching dmesg and slab ─────────────────────────
loaded || load_module
sync; sleep 1
base_dmesg="$(dmesg_len)"
base_slab="$(slab_kb)"
echo "  baseline: Slab=${base_slab} kB"

faulted=""; i=0
while [ "$i" -lt "$CYCLES" ]; do
    i=$((i+1))
    if ! rmmod "$MODULE" 2>/dev/null; then
        fail "cycle-$i-unload" "rmmod failed with nothing holding it"
        faulted="unload"; break
    fi
    if ! load_module; then
        fail "cycle-$i-load" "could not reload after a clean unload (modprobe and insmod $KO both failed)"
        faulted="load"; break
    fi
    f="$(new_faults "$base_dmesg")"
    if [ -n "$f" ]; then
        fail "cycle-$i-clean-dmesg" "kernel complained on cycle $i:"
        printf '%s\n' "$f" | sed 's/^/          /'
        faulted="dmesg"; break
    fi
    [ $((i % 10)) -eq 0 ] && echo "  ... $i/$CYCLES cycles, Slab=$(slab_kb) kB"
done

[ -z "$faulted" ] && pass "cycles-completed" "$CYCLES load/unload cycles"
[ -z "$faulted" ] && pass "no-new-kernel-faults" "nothing matching the fault set in dmesg"

sync; sleep 2
end_slab="$(slab_kb)"
delta=$(( end_slab - base_slab ))
if [ "$delta" -le "$SLAB_TOLERANCE_KB" ]; then
    pass "no-unbounded-slab-growth" "Slab ${base_slab} -> ${end_slab} kB (delta ${delta} kB over $i cycles)"
else
    fail "no-unbounded-slab-growth" "Slab grew ${delta} kB over $i cycles (tolerance ${SLAB_TOLERANCE_KB} kB)"
fi

# kmemleak is authoritative when present; absence is not evidence of no leak.
if [ -r /sys/kernel/debug/kmemleak ]; then
    echo scan > /sys/kernel/debug/kmemleak 2>/dev/null; sleep 5
    n="$(grep -c "unreferenced object" /sys/kernel/debug/kmemleak 2>/dev/null || echo 0)"
    if [ "$n" -eq 0 ]; then pass "kmemleak-clean" "no unreferenced objects"
    else fail "kmemleak-clean" "$n unreferenced object(s) after $i cycles"; fi
else
    echo "  (kmemleak unavailable -- leak coverage is Slab-delta only)"
fi

echo
echo "  TOTAL  PASS $N_PASS  FAIL $N_FAIL"
[ "$N_FAIL" -eq 0 ] && { echo "  VERDICT: PASS"; exit 0; }
echo "  VERDICT: FAIL"; exit 1
