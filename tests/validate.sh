#!/usr/bin/env bash
#
# tests/validate.sh -- nvkvm guest validation suite.
#
# Run this INSIDE the guest VM. It answers one question end to end:
#
#     "does nvkvm actually work on this host driver?"
#
# It walks the full ladder -- bring-up, compute, graphics -- and prints a
# per-check PASS / FAIL / SKIP table with the ACTUAL OBSERVED VALUES, then
# exits non-zero if anything failed or was unexpectedly skipped.
#
#   bash tests/validate.sh
#   bash tests/validate.sh --expect-gpu 'RTX 3060' --expect-driver 580.95.05
#   bash tests/validate.sh --allow-skip vk_compute_dispatch
#   bash tests/validate.sh --json /tmp/result.json
#
# Exit codes:
#   0  every check PASSed
#   1  at least one check FAILed
#   2  no failures, but at least one check was SKIPped without being
#      declared via --allow-skip
#   3  the harness could not TEST -- no /tmp, or a whole phase had neither a
#      compiler nor a prebuilt probe. See "UNTESTED" below.
#
# ---------------------------------------------------------------------------
# RUNNING THIS WHERE THERE IS NO COMPILER  (--build-probes / --probe-dir)
# ---------------------------------------------------------------------------
#
# 24 of the 30 checks are C probes compiled at run time, so on an image that
# ships no toolchain -- the produced SteamOS image removes gcc and make after
# building the guest module -- this suite could only ever check bring-up. It
# reported "30 total: 5 PASS, 1 FAIL, 24 SKIP" on the real shipped artifact,
# and that reads like a mostly-fine run. It was not a run at all: CUDA, Vulkan,
# EGL and GL were never exercised.
#
# So build the probes once, where a compiler exists, and ship the binaries:
#
#     bash tests/validate.sh --build-probes /usr/local/lib/nvkvm/probes
#     bash tests/validate.sh                     # finds them there by default
#
# The probes dlopen() everything (libcuda, libvulkan, libEGL) and link only
# -ldl and -lm, so a prebuilt binary needs nothing at run time but libc. Build
# them inside the image, against the image's own libc, at the point in the
# build where the toolchain is still present and before it is removed.
#
# And when they are NOT there and no compiler is either, those checks are
# recorded UNTESTED, not SKIP, and the run ends "VERDICT: CANNOT VALIDATE"
# with exit 3. --allow-skip cannot silence an UNTESTED check: a run that was
# structurally unable to test the thing must never resemble a run that tested
# it and passed. That is the floor, not the fix; the fix is --build-probes.
#
# ---------------------------------------------------------------------------
# DESIGN RULES (this suite exists because false greens cost this project days)
# ---------------------------------------------------------------------------
#
#  1. A check that COULD NOT RUN is SKIP. It is never PASS. "The binary did
#     not build" is not evidence that the GPU works.
#  2. Nothing is redirected to /dev/null on a failure path. stderr from every
#     probe is captured and printed on failure.
#  3. Every check that has a VALUE reports that value. "Vulkan OK" is not an
#     acceptable output; "Vulkan: NVIDIA GeForce RTX 3060" is. The table's
#     DETAIL column is the point of the table.
#  4. Correctness is decided by comparing a VALUE, not by reading an exit
#     code, wherever a value exists: the memcpy check is a byte-exact memcmp,
#     the kernel check verifies every output element, the GL check asserts an
#     exact pixel colour both inside AND outside the drawn triangle (so that
#     "I read someone else's framebuffer" cannot pass).
#  5. Renderer checks assert the NVIDIA device is what answered -- a software
#     rasteriser (llvmpipe / swrast / lavapipe) is an explicit FAIL, not a
#     pass, because a software fallback is exactly the failure mode a
#     paravirtual GPU regresses into.
#
# The CUDA / Vulkan / GL probes are C programs embedded below as heredocs and
# compiled at run time. They dlopen() libcuda / libvulkan / libEGL and
# hand-roll the handful of types they need, so the ONLY build dependency of
# this entire suite is a working `cc`. No CUDA toolkit, no Vulkan SDK, no
# -dev packages. That is deliberate: the suite has to run on a fresh guest.
#
# The embedded PTX is reused verbatim from tests/integration/vector_add_test.c
# and tests/integration/matmul_test.c.
#

set -u -o pipefail

# ---------------------------------------------------------------------------
# argument parsing
# ---------------------------------------------------------------------------

EXPECT_GPU=""
EXPECT_DRIVER=""
EXPECT_CUDA=""
EXPECT_ABI=""
ALLOW_SKIP=""
JSON_OUT=""
KEEP_WORKDIR=0
VERBOSE=0
BUILD_PROBES=""          # --build-probes DIR: compile the probes there and stop
PROBE_DIR="${NVKVM_PROBE_DIR:-}"   # --probe-dir DIR: use prebuilt probes from there
SELFTEST=0

# Where a prebuilt set is looked for when --probe-dir was not given. First hit
# wins. The /usr/local path is the one an image build should populate.
DEFAULT_PROBE_DIRS="/usr/local/lib/nvkvm/probes /usr/lib/nvkvm/probes"
DEFAULT_PROBE_DIRS="$DEFAULT_PROBE_DIRS $(cd "$(dirname "$0")" 2>/dev/null && pwd)/probes"

usage() {
    # The whole leading comment block, not a hardcoded line range -- the range
    # said 2,40 and would silently stop showing the options as the header grew.
    awk 'NR>1 && !/^#/{exit} NR>1{sub(/^# ?/,""); print}' "$0"
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --expect-gpu)     EXPECT_GPU="$2";    shift 2 ;;
        --expect-driver)  EXPECT_DRIVER="$2"; shift 2 ;;
        --expect-cuda)    EXPECT_CUDA="$2";   shift 2 ;;
        --expect-abi)     EXPECT_ABI="$2";    shift 2 ;;
        --allow-skip)     ALLOW_SKIP="$ALLOW_SKIP,$2"; shift 2 ;;
        --build-probes)   BUILD_PROBES="$2";  shift 2 ;;
        --probe-dir)      PROBE_DIR="$2";     shift 2 ;;
        --selftest)       SELFTEST=1;         shift ;;
        --json)           JSON_OUT="$2";      shift 2 ;;
        --keep)           KEEP_WORKDIR=1;     shift ;;
        -v|--verbose)     VERBOSE=1;          shift ;;
        -h|--help)        usage ;;
        *) echo "unknown argument: $1" >&2; exit 3 ;;
    esac
done

# ---------------------------------------------------------------------------
# result bookkeeping
# ---------------------------------------------------------------------------

RESULT_NAMES=()
RESULT_STATUS=()
RESULT_DETAIL=()

N_PASS=0
N_FAIL=0
N_SKIP=0
# UNTESTED is NOT a third flavour of SKIP, and the distinction is the whole
# point. SKIP means "this check decided not to run here" -- no Vulkan loader on
# a headless box, say -- and an operator can accept that with --allow-skip.
# UNTESTED means the suite was STRUCTURALLY UNABLE to test: no compiler and no
# prebuilt probe, so nothing about CUDA/Vulkan/GL was observed at all. Nobody
# may wave that away, because the resulting run carries no evidence whatsoever
# about the thing it is named after.
N_UNTESTED=0

C_RESET=""; C_PASS=""; C_FAIL=""; C_SKIP=""; C_BOLD=""; C_DIM=""; C_UNT=""
if [ -t 1 ]; then
    C_RESET=$'\033[0m'; C_PASS=$'\033[32m'; C_FAIL=$'\033[31m'
    C_SKIP=$'\033[33m'; C_BOLD=$'\033[1m';  C_DIM=$'\033[2m'
    C_UNT=$'\033[35m'
fi

# record <name> <PASS|FAIL|SKIP|UNTESTED> <detail...>
record() {
    local name="$1" status="$2"; shift 2
    local detail="$*"
    RESULT_NAMES+=("$name")
    RESULT_STATUS+=("$status")
    RESULT_DETAIL+=("$detail")
    case "$status" in
        PASS) N_PASS=$((N_PASS+1)); printf '  %s%-4s%s %-26s %s\n' "$C_PASS" PASS "$C_RESET" "$name" "$detail" ;;
        FAIL) N_FAIL=$((N_FAIL+1)); printf '  %s%-4s%s %-26s %s\n' "$C_FAIL" FAIL "$C_RESET" "$name" "$detail" ;;
        SKIP) N_SKIP=$((N_SKIP+1)); printf '  %s%-4s%s %-26s %s\n' "$C_SKIP" SKIP "$C_RESET" "$name" "$detail" ;;
        UNTESTED) N_UNTESTED=$((N_UNTESTED+1)); printf '  %s%-8s%s %-26s %s\n' "$C_UNT" UNTESTED "$C_RESET" "$name" "$detail" ;;
    esac
}

section() { checkpoint "section:$1"; printf '\n%s== %s ==%s\n' "$C_BOLD" "$1" "$C_RESET"; }

# ---------------------------------------------------------------------------
# the verdict
# ---------------------------------------------------------------------------
# Reads N_FAIL / N_UNTESTED / UNEXPECTED_SKIPS / N_PASS / TOTAL and returns the
# suite's exit code. A function so --selftest can drive it with fabricated
# counters -- the ordering below is the whole point of this suite and it is not
# reachable on a machine with no GPU any other way.
#
# The ordering is deliberate. A real FAIL is the loudest thing and stays first.
# But CANNOT VALIDATE outranks INCOMPLETE, and outranks PASS absolutely: a run
# that could not test the thing must never be reportable as a run that tested it
# and passed, whatever flags it was given. --allow-skip is deliberately not
# consulted here -- it only ever moves a SKIP out of UNEXPECTED_SKIPS, and it
# has no bearing at all on N_UNTESTED.
verdict() {
    if [ "$N_FAIL" -gt 0 ]; then
        printf '\n %sVERDICT: FAIL%s (%d check(s) failed)\n' "$C_FAIL" "$C_RESET" "$N_FAIL"
        return 1
    fi
    if [ "$N_UNTESTED" -gt 0 ]; then
        printf '\n %sVERDICT: CANNOT VALIDATE%s (%d of %d checks were never attempted; %d passed)\n' \
               "$C_UNT" "$C_RESET" "$N_UNTESTED" "$TOTAL" "$N_PASS"
        printf ' This is not a pass with caveats. The suite was structurally unable to test\n'
        printf ' %d of its %d checks, so it has no evidence about them either way.\n' "$N_UNTESTED" "$TOTAL"
        return 3
    fi
    if [ -n "$UNEXPECTED_SKIPS" ]; then
        printf '\n %sVERDICT: INCOMPLETE%s (no failures, but%s was skipped)\n' "$C_SKIP" "$C_RESET" "$UNEXPECTED_SKIPS"
        return 2
    fi
    printf '\n %sVERDICT: PASS%s (all %d checks passed)\n' "$C_PASS" "$C_RESET" "$N_PASS"
    return 0
}

# ---------------------------------------------------------------------------
# nvidia-smi's CUDA-version field
# ---------------------------------------------------------------------------
# There are FOUR distinct things the banner can be saying, and the old code
# collapsed three of them into one FAIL, "could not parse 'CUDA Version:'":
#
#   value    the field is there with a version         -> PASS (compare if asked)
#   na       the field is there and says N/A           -> depends on libcuda
#   absent   the banner has no CUDA field at all       -> FAIL, and quote it
#   (rc!=0)  nvidia-smi itself failed                  -> handled by the caller
#
# `N/A` is not a CUDA fault. nvidia-smi gets that number from NVML, whose
# cudaDriverVersion query resolves libcuda.so.1; with no libcuda installed it
# has nothing to report and prints N/A. The SteamOS `steamos` profile TRIMS
# libcuda deliberately (steamos-nvidia-installer's tested trim set, which
# nvkvm-steamos reuses verbatim) precisely because a gaming image does not need
# it -- so on the shipped image `N/A` is the CORRECT answer and failing the run
# for it reports a healthy guest as broken. That is the same defect class as the
# false CLASS 4 in nvkvm-steamos's boot/TESTING.md, in the same week.
#
# But `N/A` WITH libcuda installed is a real fault, and stays a FAIL.
#
# smi_cuda_field <banner-text>  -> echoes "value <v>" | "na" | "absent"
smi_cuda_field() {
    local banner="$1" v
    # Driver 610 renamed the banner fields: pre-610 prints
    #   "Driver Version: 580.95.05   CUDA Version: 13.0"
    # while 610+ prints
    #   "KMD Version: 610.43.02      CUDA UMD Version: 13.3"
    # Accept either spelling -- a hardcoded "CUDA Version:" silently fails to
    # parse on 610 and would turn a healthy guest into a FAIL.
    printf '%s\n' "$banner" | grep -qE 'CUDA (UMD )?Version:' || { printf 'absent\n'; return; }
    v="$(printf '%s\n' "$banner" | sed -n 's/.*CUDA \(UMD \)\{0,1\}Version: *\([0-9][0-9.]*\).*/\2/p' | head -1)"
    if [ -n "$v" ]; then printf 'value %s\n' "$v"; else printf 'na\n'; fi
}

# Is a CUDA driver library actually installed? If it is not, nvidia-smi having
# nothing to say about CUDA is expected, not a failure.
have_libcuda() {
    ldconfig -p 2>/dev/null | grep -q 'libcuda\.so' && return 0
    local d
    for d in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib; do
        ls "$d"/libcuda.so.* >/dev/null 2>&1 && return 0
    done
    return 1
}

# ---------------------------------------------------------------------------
# --selftest: exercise the pure parsing above against fixed banners.
# ---------------------------------------------------------------------------
# The rest of this suite needs a GPU. These functions do not, and they are the
# ones that decide whether a healthy guest is reported as broken -- so they are
# the ones worth pinning. Every case is a real banner shape, and the classifier
# under test is the same text the live path calls; there is no second copy.
if [ "$SELFTEST" = 1 ]; then
    st_run=0; st_pass=0
    st_chk() {   # <name> <expected> <banner>
        local name="$1" want="$2" banner="$3" got
        st_run=$((st_run+1))
        got="$(smi_cuda_field "$banner")"
        if [ "$got" = "$want" ]; then st_pass=$((st_pass+1)); printf '  PASS  %s\n' "$name"
        else printf '  FAIL  %s: got "%s", wanted "%s"\n' "$name" "$got" "$want"; fi
    }

    # Real capture from a guest, README.md's walkthrough.
    st_chk "pre-610 banner" "value 12.9" \
'+-----------------------------------------------------------------------------------------+
| NVIDIA-SMI 575.51.03              Driver Version: 575.51.03      CUDA Version: 12.9     |
|   0  NVIDIA GeForce RTX 3060        Off |   00000000:00:07.0                            |
+-----------------------------------------------------------------------------------------+'

    # 610 renamed both fields; tests/BOOT_MATRIX.md records 13.3 parsed here.
    st_chk "610 KMD/CUDA-UMD banner" "value 13.3" \
'| NVIDIA-SMI 610.43.02              KMD Version: 610.43.02      CUDA UMD Version: 13.3    |'

    # THE regression: the field is present and says N/A. Must not be confused
    # with "no field at all", and must not be parsed as a version.
    st_chk "N/A is not a version" "na" \
'| NVIDIA-SMI 570.133.20             Driver Version: 570.133.20    CUDA Version: N/A       |'

    st_chk "N/A on a 610 banner" "na" \
'| NVIDIA-SMI 610.43.02              KMD Version: 610.43.02      CUDA UMD Version: N/A     |'

    # No CUDA field anywhere: a genuinely unexpected banner shape, and the only
    # one that should still be a hard FAIL.
    st_chk "no CUDA field at all" "absent" \
'| NVIDIA-SMI 570.133.20             Driver Version: 570.133.20                            |
|   0  NVIDIA GeForce RTX 4090        Off |   00000000:00:07.0                            |'

    st_chk "empty banner" "absent" ""

    # A version with a trailing field must not swallow it.
    st_chk "trailing columns after the version" "value 12.4" \
'| NVIDIA-SMI 550.54.14   Driver Version: 550.54.14   CUDA Version: 12.4    |'

    st_verdict() {   # <name> <want-rc> <fail> <untested> <unexpected-skips>
        local name="$1" want="$2" rc
        N_FAIL="$3"; N_UNTESTED="$4"; UNEXPECTED_SKIPS="$5"
        N_PASS=6; TOTAL=30
        st_run=$((st_run+1))
        verdict >/dev/null; rc=$?
        if [ "$rc" = "$want" ]; then st_pass=$((st_pass+1)); printf '  PASS  %s\n' "$name"
        else printf '  FAIL  %s: verdict returned %s, wanted %s\n' "$name" "$rc" "$want"; fi
    }
    printf '\n'
    st_verdict "all clear -> 0"                       0 0  0 ""
    st_verdict "a failure -> 1"                       1 1  0 ""
    st_verdict "an undeclared skip -> 2"              2 0  0 " vk_compute_dispatch"
    # THE property. 24 checks never attempted, nothing failed, and every skip
    # declared acceptable: the run must still refuse to be a pass.
    st_verdict "untested with no failures -> 3"       3 0 24 ""
    st_verdict "untested outranks an undeclared skip" 3 0 24 " vk_compute_dispatch"
    st_verdict "a real failure still outranks it"     1 3 24 ""

    printf '\n%d/%d selftest cases passed\n' "$st_pass" "$st_run"
    [ "$st_pass" = "$st_run" ] || exit 1
    exit 0
fi

PROBE_RC=0
# --build-probes is a build step, not a validation run: it wants the heredocs
# and the compiler and nothing else. Silence the reporting so its output is the
# three lines that matter.
if [ -n "$BUILD_PROBES" ]; then
    record()  { :; }
    section() { :; }
fi

# has_result <name> -- true if the named check already recorded something
has_result() {
    local n
    for n in ${RESULT_NAMES[@]+"${RESULT_NAMES[@]}"}; do
        [ "$n" = "$1" ] && return 0
    done
    return 1
}

# skip_remaining <reason> <name...> -- record SKIP for any name not yet recorded
skip_remaining() {
    local reason="$1"; shift
    local n
    for n in "$@"; do
        has_result "$n" || record "$n" SKIP "$reason"
    done
}

# untested_remaining <reason> <name...> -- for checks the suite could not even
# attempt. Used ONLY when there is neither a compiler nor a prebuilt probe.
untested_remaining() {
    local reason="$1"; shift
    local n
    for n in "$@"; do
        has_result "$n" || record "$n" UNTESTED "$reason"
    done
}

# ---------------------------------------------------------------------------
# workdir + toolchain
# ---------------------------------------------------------------------------

WORK="$(mktemp -d /tmp/nvkvm-validate.XXXXXX)" || { echo "cannot mktemp" >&2; exit 3; }
cleanup() { [ "$KEEP_WORKDIR" = 1 ] || rm -rf "$WORK"; }
trap cleanup EXIT

CC=""
for c in cc gcc clang; do
    if command -v "$c" >/dev/null 2>&1; then CC="$c"; break; fi
done

printf '%s' "$C_BOLD"
cat <<BANNER
=============================================================================
 nvkvm guest validation suite
=============================================================================
BANNER
printf '%s' "$C_RESET"
printf ' host       : %s\n' "$(uname -srm)"
printf ' date       : %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
printf ' workdir    : %s\n' "$WORK"
printf ' compiler   : %s\n' "${CC:-<none>}"

# build <output> <source> <extra-ldflags...> ; on failure prints the compiler log
build() {
    local out="$1" src="$2"; shift 2
    if [ -z "$CC" ]; then return 127; fi
    if ! "$CC" -O2 -o "$WORK/$out" "$WORK/$src" "$@" > "$WORK/$out.buildlog" 2>&1; then
        echo "--- compiler output for $src ---" >&2
        cat "$WORK/$out.buildlog" >&2
        return 1
    fi
    return 0
}

# Resolve where prebuilt probes live: --probe-dir / $NVKVM_PROBE_DIR if given,
# else the first default directory that actually contains one.
if [ -z "$PROBE_DIR" ]; then
    for _d in $DEFAULT_PROBE_DIRS; do
        if [ -x "$_d/cuda_probe" ] || [ -x "$_d/vk_probe" ] || [ -x "$_d/gl_probe" ]; then
            PROBE_DIR="$_d"; break
        fi
    done
fi
PROBE_NOTE=""

# provide_probe <name> <source> <ldflags...>
#   0   $WORK/<name> is now a runnable probe
#   1   it could not be built (a compiler exists; the compile failed)  -> SKIP
#   127 there is no compiler AND no prebuilt binary                    -> UNTESTED
#
# A prebuilt binary is preferred over compiling: on an image that ships probes,
# what runs is exactly what was validated at build time, and the run does not
# depend on a toolchain being present at all.
provide_probe() {
    local out="$1" src="$2"; shift 2
    if [ -n "$PROBE_DIR" ] && [ -x "$PROBE_DIR/$out" ]; then
        if cp "$PROBE_DIR/$out" "$WORK/$out" && chmod +x "$WORK/$out"; then
            PROBE_NOTE="prebuilt from $PROBE_DIR"
            return 0
        fi
        echo "warning: $PROBE_DIR/$out is present but unusable; falling back to compiling" >&2
    fi
    if [ -z "$CC" ]; then return 127; fi
    PROBE_NOTE="compiled here"
    build "$out" "$src" "$@"
}

# The message every UNTESTED check carries. One place, so it cannot drift.
NO_PROBE_REASON="no C compiler on PATH and no prebuilt probe (see --build-probes)"

# --build-probes <dir>: compile the three probes into <dir> and stop. Called
# where the toolchain exists -- an image build, before it strips gcc.
BUILD_PROBES_DONE=0
emit_probe() {   # <name> <source> <ldflags...>
    local out="$1" src="$2"; shift 2
    if [ -z "$CC" ]; then
        echo "--build-probes needs a C compiler, and there is none on PATH." >&2
        exit 3
    fi
    build "$out" "$src" "$@" || { echo "--build-probes: $src failed to compile" >&2; exit 3; }
    mkdir -p "$BUILD_PROBES" || { echo "--build-probes: cannot create $BUILD_PROBES" >&2; exit 3; }
    cp "$WORK/$out" "$BUILD_PROBES/$out" || exit 3
    chmod 0755 "$BUILD_PROBES/$out"
    printf ' built %s -> %s/%s (%s bytes)\n' "$out" "$BUILD_PROBES" "$out" "$(wc -c <"$BUILD_PROBES/$out")"
    BUILD_PROBES_DONE=$((BUILD_PROBES_DONE+1))
}

# run_probe <binary> -- runs it, tees combined output, returns its exit code.
# Output is captured to $WORK/<binary>.out and NEVER discarded.
# A PROBE THAT HANGS IS NOT A PROBE THAT FAILED.
#
# This used to run the binary with no bound at all.  A probe that blocked --
# CUDA context setup waiting on something the host refused, say -- never
# returned, so validate.sh never reached its own JSON write and the caller got
# NOTHING: no verdict, no counts, no per-check detail.  From outside that is
# indistinguishable from a crash, and a sweep records it as "validate-unparsed",
# which reads like a harness defect rather than "the GPU path hung".
#
# MEASURED 2026-09-01 on a Quadro P4000: a run took 2260s against ~500s for a
# healthy one and produced no verdict, four times over, while the thing we were
# actually trying to learn -- whether Pascal gets past the alloc-class gate --
# was unanswerable because a hang and a failure looked the same.
#
# So: bound every probe, and when one exceeds the bound, ingest whatever it did
# emit (partial CHECK lines are still real results) and then record the probe
# itself UNTESTED.  UNTESTED is the honest status -- nothing was observed to
# fail, and nothing made the check structurally impossible; it simply did not
# finish.  It also cannot be silenced by --allow-skip and it moves the exit
# code, which is what stops a hang being read as a pass.
#
# `timeout --kill-after=N` was tried first and MEASURED (2026-09-01, same
# Quadro P4000) to not be enough: a sweep run sat for 2573s and produced
# neither PROBE_TIMEOUT's own UNTESTED nor a JSON verdict -- the wrapper never
# even reached `local rc=$?`.  HYPOTHESIS (not proven): `timeout` sends the
# signal but then still blocks in its own waitpid() for the child to actually
# exit; if the child is asleep in an uninterruptible ioctl into the guest
# module, it never returns from the kernel to take the SIGKILL, never exits,
# and `timeout` never returns either -- so the outer bound silently becomes no
# bound at all. So: never let this shell block waiting on the probe. Start it
# in the background and poll its liveness with `kill -0`, which only asks the
# kernel "does this pid still exist" and -- unlike wait()/waitpid() -- does
# NOT block even on a process stuck in D state. See _wait_bounded below.
PROBE_TIMEOUT="${NVKVM_PROBE_TIMEOUT:-300}"
PROBE_KILL_AFTER="${NVKVM_PROBE_KILL_AFTER:-10}"
PROBE_TIMED_OUT=""
PROBE_SURVIVED=""

# ---------------------------------------------------------------------------
# wedge diagnostics
# ---------------------------------------------------------------------------
# fd 9 is a private duplicate of this process's REAL stdout, taken once here
# before anything wraps a call in `$(...)`. run_probe/sh_bounded's callers
# capture combined output into a variable (e.g. `SMI_RAW="$(sh_bounded ...)"`,
# `out="$(timeout 120 ...)"`) -- writing wedge evidence to fd 1 or fd 2 there
# would land IN that variable instead of in the console/log a sweep pulls.
# Writing to fd 9 always reaches the same underlying stream as stdout, no
# matter what a call site does with 1 and 2 locally.
exec 9>&1

LAST_CHECKPOINT="(none yet)"
# checkpoint <label> -- record, BEFORE a step runs, what is about to happen.
# Written immediately (one write(2) per call, unbuffered) so that if the step
# wedges and nothing else ever gets printed, the last CHECKPOINT line in
# whatever log a sweep pulled names the exact step that was in flight.
checkpoint() {
    LAST_CHECKPOINT="$1"
    printf 'CHECKPOINT|%s|%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$1" >&9
}

# dump_wedge_evidence <label> <reaped|survived> <pid>
#   reaped   -- the kill (TERM, or KILL after the grace period) actually
#               terminated the process. A slow probe/command, not a wedge.
#   survived -- after SIGKILL plus the grace period the process is STILL
#               alive. This is the stronger claim: nothing but the kernel
#               finishing whatever uninterruptible syscall it is in can end
#               this process. Consistent with the D-state HYPOTHESIS above,
#               not proof of it.
dump_wedge_evidence() {
    local label="$1" kill_result="$2" pid="${3:-?}"
    {
        printf 'WEDGE-EVIDENCE|step=%s|pid=%s|kill_result=%s|last_checkpoint=%s\n' \
               "$label" "$pid" "$kill_result" "$LAST_CHECKPOINT"
        if [ "$kill_result" = survived ]; then
            printf '  ** process SURVIVED SIGKILL -- stronger than a plain timeout, consistent with an uninterruptible-sleep (D-state) wedge **\n'
        fi

        printf -- '--- ps -eo pid,stat,wchan:32,comm (D-state processes) ---\n'
        local dstate dpid
        dstate="$(ps -eo pid,stat,wchan:32,comm 2>/dev/null | awk 'NR==1{print; next} $2 ~ /^D/')"
        if [ -n "$dstate" ]; then
            printf '%s\n' "$dstate"
            for dpid in $(printf '%s\n' "$dstate" | awk 'NR>1{print $1}'); do
                if [ -r "/proc/$dpid/stack" ]; then
                    printf -- '--- /proc/%s/stack ---\n' "$dpid"
                    cat "/proc/$dpid/stack" 2>/dev/null || printf '  (read failed)\n'
                else
                    printf -- '--- /proc/%s/stack: unreadable (needs root, or process already gone) ---\n' "$dpid"
                fi
            done
        else
            printf '  (ps unavailable, or no D-state processes right now)\n'
        fi

        printf -- '--- dmesg tail ---\n'
        local dm
        if dm="$(dmesg 2>&1)" && [ -n "$dm" ]; then
            printf '%s\n' "$dm" | tail -n 40
        elif dm="$(sudo -n dmesg 2>&1)" && [ -n "$dm" ]; then
            printf '%s\n' "$dm" | tail -n 40
        else
            printf '  (dmesg unavailable: restricted, and no passwordless sudo)\n'
        fi
        printf 'WEDGE-EVIDENCE-END|step=%s\n' "$label"
    } >&9
}

# _wait_bounded <pid> <budget> <kill_after> <label> -- the shared poll loop
# behind both run_probe and sh_bounded. NEVER calls a blocking `wait <pid>` on
# a pid that might still be running -- only `kill -0`, which is non-blocking
# always, D-state or not. Sets:
#   _BOUNDED_RC         real exit status if the process ended on its own or
#                       was reaped after being killed; 124 if it never died
#   _BOUNDED_TIMED_OUT  1 if <budget> was exceeded at all
#   _BOUNDED_SURVIVED   1 if it was still alive after SIGKILL + grace
_wait_bounded() {
    local pid="$1" budget="$2" kill_after="$3" label="$4" waited=0 grace=0
    _BOUNDED_TIMED_OUT=0
    _BOUNDED_SURVIVED=0
    while kill -0 "$pid" 2>/dev/null; do
        [ "$waited" -ge "$budget" ] && break
        sleep 1
        waited=$((waited + 1))
    done
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid" 2>/dev/null
        _BOUNDED_RC=$?
        return 0
    fi

    _BOUNDED_TIMED_OUT=1
    checkpoint "${label}:budget(${budget}s)-exceeded,sending-TERM"
    kill -TERM "$pid" 2>/dev/null
    while [ "$grace" -lt "$kill_after" ] && kill -0 "$pid" 2>/dev/null; do
        sleep 1
        grace=$((grace + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        checkpoint "${label}:TERM-ignored,sending-KILL"
        kill -KILL "$pid" 2>/dev/null
        sleep 1
    fi

    if kill -0 "$pid" 2>/dev/null; then
        _BOUNDED_SURVIVED=1
        _BOUNDED_RC=124
        dump_wedge_evidence "$label" survived "$pid"
    else
        wait "$pid" 2>/dev/null
        _BOUNDED_RC=$?
        dump_wedge_evidence "$label" reaped "$pid"
    fi
    return 0
}

run_probe() {
    local bin="$1"; shift
    checkpoint "probe:$bin"
    "$WORK/$bin" "$@" > "$WORK/$bin.out" 2>&1 &
    local pid=$!
    _wait_bounded "$pid" "$PROBE_TIMEOUT" "$PROBE_KILL_AFTER" "probe:$bin"
    [ "$VERBOSE" = 1 ] && sed 's/^/    | /' "$WORK/$bin.out"
    if [ "$_BOUNDED_TIMED_OUT" = 1 ]; then
        PROBE_TIMED_OUT="$bin"
        if [ "$_BOUNDED_SURVIVED" = 1 ]; then PROBE_SURVIVED="$bin"; else PROBE_SURVIVED=""; fi
    else
        PROBE_TIMED_OUT=""
        PROBE_SURVIVED=""
    fi
    return "$_BOUNDED_RC"
}

# Call after ingest() when run_probe may have timed out, so the partial results
# are kept AND the truncation is visible. The wording is the machine-readable
# split this whole mechanism exists for: "bounded and reaped" (a slow probe)
# is a materially weaker claim than "bounded and the process SURVIVED the
# kill" (the D-state signature) -- never collapse the two into one message.
note_probe_timeout() {
    [ -n "${PROBE_TIMED_OUT:-}" ] || return 0
    if [ -n "${PROBE_SURVIVED:-}" ]; then
        record "${PROBE_TIMED_OUT}_completed" UNTESTED \
               "WEDGE: probe exceeded ${PROBE_TIMEOUT}s, was sent SIGKILL, and SURVIVED the kill -- bounded and the process SURVIVED the kill, not just bounded and reaped; see WEDGE-EVIDENCE above"
    else
        record "${PROBE_TIMED_OUT}_completed" UNTESTED \
               "probe exceeded ${PROBE_TIMEOUT}s and was killed (bounded and reaped) -- it HUNG rather than failed; any checks it had not yet reported are missing, not passing"
    fi
    PROBE_TIMED_OUT=""
    PROBE_SURVIVED=""
}

# sh_bounded <label> <budget-seconds> <cmd...> -- the SHELL_TIMEOUT layer for
# shell-level commands that talk to the driver (nvidia-smi, a repro binary)
# but are not one of the compiled probes above. Same non-blocking poll, same
# evidence dump. Runs <cmd...> with its normal stdout/stderr (so a caller can
# still do `SMI_RAW="$(sh_bounded nvidia_smi_banner "$SHELL_TIMEOUT" "$NVSMI")"`
# and get exactly what the command printed); on timeout, sets
# SH_BOUNDED_TIMED_OUT / SH_BOUNDED_SURVIVED for note_shell_timeout to read.
SHELL_TIMEOUT="${NVKVM_SHELL_TIMEOUT:-60}"
SHELL_KILL_AFTER="${NVKVM_SHELL_KILL_AFTER:-5}"
SH_BOUNDED_TIMED_OUT=""
SH_BOUNDED_SURVIVED=""

sh_bounded() {
    local label="$1" budget="$2"; shift 2
    checkpoint "shell:$label"
    "$@" &
    local pid=$!
    _wait_bounded "$pid" "$budget" "$SHELL_KILL_AFTER" "shell:$label"
    if [ "$_BOUNDED_TIMED_OUT" = 1 ]; then
        SH_BOUNDED_TIMED_OUT="$label"
        if [ "$_BOUNDED_SURVIVED" = 1 ]; then SH_BOUNDED_SURVIVED="$label"; else SH_BOUNDED_SURVIVED=""; fi
    else
        SH_BOUNDED_TIMED_OUT=""
        SH_BOUNDED_SURVIVED=""
    fi
    return "$_BOUNDED_RC"
}

# note_shell_timeout <name...> -- call right after sh_bounded with the check
# name(s) that command's result would otherwise feed. Returns 0 (and records
# each as UNTESTED, same split as note_probe_timeout) only if that sh_bounded
# call actually timed out, so callers can gate their normal parse path on it:
#     SMI_RAW="$(sh_bounded nvidia_smi "$SHELL_TIMEOUT" "$NVSMI")"
#     if note_shell_timeout nvidia_smi_gpu nvidia_smi_driver nvidia_smi_cuda; then :
#     elif ... # normal parsing, unchanged
note_shell_timeout() {
    [ -n "${SH_BOUNDED_TIMED_OUT:-}" ] || return 1
    local reason n
    if [ -n "${SH_BOUNDED_SURVIVED:-}" ]; then
        reason="WEDGE: ${SH_BOUNDED_TIMED_OUT} exceeded ${SHELL_TIMEOUT}s, was sent SIGKILL, and SURVIVED the kill -- bounded and the process SURVIVED the kill, not just bounded and reaped; see WEDGE-EVIDENCE above"
    else
        reason="${SH_BOUNDED_TIMED_OUT} exceeded ${SHELL_TIMEOUT}s and was killed (bounded and reaped)"
    fi
    for n in "$@"; do record "$n" UNTESTED "$reason"; done
    SH_BOUNDED_TIMED_OUT=""
    SH_BOUNDED_SURVIVED=""
    return 0
}

# ingest <outfile> -- parse CHECK|name|status|detail lines from a probe's output
ingest() {
    local f="$1" line name status detail
    while IFS= read -r line; do
        case "$line" in
            CHECK\|*)
                name="${line#CHECK|}"; status="${name#*|}"; name="${name%%|*}"
                detail="${status#*|}"; status="${status%%|*}"
                record "$name" "$status" "$detail"
                ;;
        esac
    done < "$f"
}

# LIBRARY MODE.  tests/wedge_diagnostics_test.sh sources this file to exercise
# checkpoint/_wait_bounded/run_probe/sh_bounded/note_probe_timeout/
# note_shell_timeout/dump_wedge_evidence against deliberately-wedged and
# deliberately-slow-but-killable fakes, which is the only way to test the
# unkillable-D-state path without a GPU. Stop here, before any phase that
# needs one. The `trap cleanup EXIT` installed above (with $WORK) is already
# in place and needs no change -- it fires normally when the sourcing
# harness's shell exits, removing this same $WORK.
if [ "${NVKVM_VALIDATE_LIB:-0}" = 1 ]; then
    return 0 2>/dev/null || exit 0
fi

# =============================================================================
# PHASE 1 -- guest bring-up (shell level)
# =============================================================================
section "1. guest bring-up"

# --- 1.1 guest kernel module -------------------------------------------------
MODINFO="$(lsmod 2>/dev/null | awk '$1=="nvkvm_guest"||$1=="nvkvm"{print $1" size="$2" refcnt="$3}')"
if [ -n "$MODINFO" ]; then
    record guest_module PASS "$MODINFO"
else
    LOADED_NV="$(lsmod 2>/dev/null | awk '/nvkvm/{print $1}' | tr '\n' ' ')"
    record guest_module FAIL "nvkvm_guest not in lsmod (nvkvm-ish modules: ${LOADED_NV:-none})"
fi

# --- 1.2 the five device nodes ----------------------------------------------
NODES="/dev/nvidia0 /dev/nvidiactl /dev/nvidia-uvm /dev/nvidia-uvm-tools /dev/nvidia-modeset"
MISSING=""; PRESENT=""
for n in $NODES; do
    if [ -e "$n" ]; then
        # report the actual major:minor -- proves it is a real char device from
        # our module and not a leftover regular file
        mm="$(stat -c '%t:%T' "$n" 2>&1)"
        typ="$(stat -c '%F' "$n" 2>&1)"
        if [ "$typ" = "character special file" ]; then
            PRESENT="$PRESENT ${n##*/}($((0x${mm%%:*})):$((0x${mm##*:})))"
        else
            MISSING="$MISSING ${n##*/}(not-a-chardev:$typ)"
        fi
    else
        MISSING="$MISSING ${n##*/}(absent)"
    fi
done
if [ -z "$MISSING" ]; then
    record dev_nodes PASS "5/5:${PRESENT}"
else
    record dev_nodes FAIL "missing/bad:${MISSING} present:${PRESENT:-none}"
fi

# --- 1.3 nvidia-smi ----------------------------------------------------------
#
# Do not trust PATH alone.  stage_guest_libs.sh installs nvidia-smi into
# /usr/local/bin, and this suite is normally run under sudo -- on a RHEL-family
# guest sudo's secure_path is /sbin:/bin:/usr/sbin:/usr/bin, which does NOT
# include /usr/local/bin (Debian's does).  So on CentOS Stream 9 all three
# checks recorded SKIP "not on PATH" while nvidia-smi sat right there and
# worked perfectly from a normal shell -- and an unexpected skip is scored as
# not-a-pass, so a working stack read 25/28 INCOMPLETE.
SMI_GPU=""; SMI_DRV=""; SMI_CUDA=""
NVSMI="$(command -v nvidia-smi 2>/dev/null || true)"
if [ -z "$NVSMI" ]; then
    for _c in /usr/local/bin/nvidia-smi /usr/bin/nvidia-smi /opt/bin/nvidia-smi; do
        [ -x "$_c" ] && { NVSMI="$_c"; break; }
    done
fi
if [ -z "$NVSMI" ]; then
    record nvidia_smi_gpu    SKIP "nvidia-smi not found on PATH or in the usual staging dirs"
    record nvidia_smi_driver SKIP "nvidia-smi not found on PATH or in the usual staging dirs"
    record nvidia_smi_cuda   SKIP "nvidia-smi not found on PATH or in the usual staging dirs"
else
    sh_bounded nvidia_smi_banner "$SHELL_TIMEOUT" "$NVSMI" > "$WORK/smi_banner.out" 2>&1; SMI_RC=$?
    SMI_RAW="$(cat "$WORK/smi_banner.out" 2>/dev/null || true)"
    if note_shell_timeout nvidia_smi_gpu nvidia_smi_driver nvidia_smi_cuda; then
        :
    elif [ $SMI_RC -ne 0 ]; then
        record nvidia_smi_gpu    FAIL "nvidia-smi exit=$SMI_RC: $(echo "$SMI_RAW" | head -3 | tr '\n' ' ')"
        record nvidia_smi_driver SKIP "nvidia-smi failed"
        record nvidia_smi_cuda   SKIP "nvidia-smi failed"
    else
        sh_bounded nvidia_smi_query_name "$SHELL_TIMEOUT" "$NVSMI" --query-gpu=name --format=csv,noheader > "$WORK/smi_name.out" 2>&1 || true
        SMI_GPU="$(head -1 "$WORK/smi_name.out" 2>/dev/null || true)"
        SMI_GPU_TIMED_OUT=0; note_shell_timeout nvidia_smi_gpu && SMI_GPU_TIMED_OUT=1
        sh_bounded nvidia_smi_query_driver "$SHELL_TIMEOUT" "$NVSMI" --query-gpu=driver_version --format=csv,noheader > "$WORK/smi_drv.out" 2>&1 || true
        SMI_DRV="$(head -1 "$WORK/smi_drv.out" 2>/dev/null || true)"
        SMI_DRV_TIMED_OUT=0; note_shell_timeout nvidia_smi_driver && SMI_DRV_TIMED_OUT=1
        SMI_CUDA_FIELD="$(smi_cuda_field "$SMI_RAW")"
        SMI_CUDA="${SMI_CUDA_FIELD#value }"
        [ "$SMI_CUDA_FIELD" = "$SMI_CUDA" ] && SMI_CUDA=""

        if [ "$SMI_GPU_TIMED_OUT" = 1 ]; then
            :
        elif [ -z "$SMI_GPU" ]; then
            record nvidia_smi_gpu FAIL "nvidia-smi reported no GPU name"
        elif [ -n "$EXPECT_GPU" ] && ! printf '%s' "$SMI_GPU" | grep -qiF -- "$EXPECT_GPU"; then
            record nvidia_smi_gpu FAIL "got '$SMI_GPU', expected to contain '$EXPECT_GPU'"
        else
            record nvidia_smi_gpu PASS "$SMI_GPU"
        fi

        if [ "$SMI_DRV_TIMED_OUT" = 1 ]; then
            :
        elif [ -z "$SMI_DRV" ]; then
            record nvidia_smi_driver FAIL "nvidia-smi reported no driver version"
        elif [ -n "$EXPECT_DRIVER" ] && [ "$SMI_DRV" != "$EXPECT_DRIVER" ]; then
            record nvidia_smi_driver FAIL "got '$SMI_DRV', expected '$EXPECT_DRIVER'"
        else
            record nvidia_smi_driver PASS "$SMI_DRV"
        fi

        case "$SMI_CUDA_FIELD" in
            absent)
                # Quote what the banner ACTUALLY said, so the next person can
                # see the shape instead of re-deriving it.
                record nvidia_smi_cuda FAIL \
                    "nvidia-smi's banner has no CUDA version field at all. Header line was: $(printf '%s\n' "$SMI_RAW" | sed -n '2p' | sed 's/  */ /g')" ;;
            na)
                if have_libcuda; then
                    record nvidia_smi_cuda FAIL \
                        "nvidia-smi reports 'CUDA Version: N/A' although libcuda is installed -- NVML cannot reach the CUDA driver"
                else
                    record nvidia_smi_cuda SKIP \
                        "nvidia-smi reports N/A and no libcuda is installed; nothing to report (a CUDA-trimmed image, e.g. the SteamOS 'steamos' profile)"
                fi ;;
            *)
                if [ -n "$EXPECT_CUDA" ] && [ "$SMI_CUDA" != "$EXPECT_CUDA" ]; then
                    record nvidia_smi_cuda FAIL "got '$SMI_CUDA', expected '$EXPECT_CUDA'"
                else
                    record nvidia_smi_cuda PASS "$SMI_CUDA"
                fi ;;
        esac
    fi
fi

# --- 1.4 ABI profile QEMU selected ------------------------------------------
# The guest module logs the host driver version it parsed. Cross-check it
# against the driver nvidia-smi reports: a mismatch here is the single most
# expensive bug class in this project (wrong ABI row -> silent truncated
# struct), so surface it even though it is only advisory.
# dmesg is root-only on a stock Ubuntu guest (kernel.dmesg_restrict=1), so an
# unprivileged `dmesg` returns nothing and this check would skip on a perfectly
# healthy guest. Try passwordless sudo before giving up.
DMESG=""
if DMESG="$(dmesg 2>&1)" && [ -n "$DMESG" ]; then
    :
elif DMESG="$(sudo -n dmesg 2>&1)" && [ -n "$DMESG" ]; then
    :
else
    DMESG=""
fi
ABI_LINE="$(printf '%s\n' "$DMESG" | grep -i 'nvkvm.*\(abi profile\|host NVIDIA driver\|host driver\)' | tail -3 | tr '\n' ';')"
if [ -n "$ABI_LINE" ]; then
    if [ -n "$EXPECT_ABI" ] && ! printf '%s' "$ABI_LINE" | grep -qF -- "$EXPECT_ABI"; then
        record abi_profile FAIL "expected '$EXPECT_ABI' in dmesg, got: $ABI_LINE"
    else
        record abi_profile PASS "$ABI_LINE"
    fi
else
    record abi_profile SKIP "no nvkvm abi/driver line in dmesg (dmesg restricted, or module quiet)"
fi

# =============================================================================
# PHASE 2 -- CUDA
# =============================================================================
section "2. CUDA (driver API, compute)"

CUDA_CHECKS="cuda_libcuda cuda_init cuda_driver_version cuda_device_count \
cuda_device_name cuda_compute_cap cuda_ctx_create cuda_htod_dtoh_8mib \
cuda_memset_d8 cuda_ptx_jit cuda_kernel_launch cuda_matmul \
cuda_managed_alloc cuda_managed_coherence"

cat > "$WORK/cuda_probe.c" <<'NVKVM_CUDA_EOF'
/*
 * nvkvm validation -- CUDA driver-API probe.
 *
 * dlopen()s libcuda.so.1 and hand-rolls the driver-API types, so this builds
 * with nothing but a C compiler -- no CUDA toolkit, no cuda.h. Same pattern as
 * tests/integration/*.c. The PTX below is reused verbatim from
 * tests/integration/vector_add_test.c and tests/integration/matmul_test.c.
 *
 * Emits one machine-readable line per check:
 *     CHECK|<name>|<PASS|FAIL|SKIP>|<detail>
 * Any check that is never reached is emitted as SKIP by finish() -- never as
 * PASS, and never silently omitted.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st  *CUcontext;
typedef struct CUmod_st  *CUmodule;
typedef struct CUfn_st   *CUfunction;
typedef struct CUstream_st *CUstream;

/* ---- ordered check list; anything unreported at exit becomes SKIP -------- */
static const char *ALL_CHECKS[] = {
    "cuda_libcuda", "cuda_init", "cuda_driver_version", "cuda_device_count",
    "cuda_device_name", "cuda_compute_cap", "cuda_ctx_create",
    "cuda_htod_dtoh_8mib", "cuda_memset_d8", "cuda_ptx_jit",
    "cuda_kernel_launch", "cuda_matmul",
    "cuda_managed_alloc", "cuda_managed_coherence", NULL
};
static int reported[64];

static int check_index(const char *n) {
    int i; for (i = 0; ALL_CHECKS[i]; i++) if (!strcmp(ALL_CHECKS[i], n)) return i;
    return -1;
}
static void emit(const char *name, const char *status, const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    int i = check_index(name); if (i >= 0) reported[i] = 1;
    printf("CHECK|%s|%s|%s\n", name, status, buf); fflush(stdout);
}
static void finish(const char *reason) {
    int i; for (i = 0; ALL_CHECKS[i]; i++)
        if (!reported[i]) printf("CHECK|%s|SKIP|%s\n", ALL_CHECKS[i], reason);
    fflush(stdout);
}

/* ---- CUDA entry points --------------------------------------------------- */
static CUresult (*p_cuInit)(unsigned);
static CUresult (*p_cuDriverGetVersion)(int *);
static CUresult (*p_cuDeviceGetCount)(int *);
static CUresult (*p_cuDeviceGet)(CUdevice *, int);
static CUresult (*p_cuDeviceGetName)(char *, int, CUdevice);
static CUresult (*p_cuDeviceGetAttribute)(int *, int, CUdevice);
static CUresult (*p_cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*p_cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*p_cuMemFree)(CUdeviceptr);
static CUresult (*p_cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
static CUresult (*p_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*p_cuMemsetD8)(CUdeviceptr, unsigned char, size_t);
static CUresult (*p_cuModuleLoadData)(CUmodule *, const void *);
static CUresult (*p_cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
static CUresult (*p_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                    unsigned, unsigned, unsigned, unsigned,
                                    CUstream, void **, void **);
static CUresult (*p_cuCtxSynchronize)(void);
static CUresult (*p_cuGetErrorName)(CUresult, const char **);
static CUresult (*p_cuMemAllocManaged)(CUdeviceptr *, size_t, unsigned int);

static void *H;
/* libcuda exports the _v2 ABI under a suffixed name; prefer it, fall back. */
static void *sym2(const char *base) {
    char b[128]; void *s;
    snprintf(b, sizeof b, "%s_v2", base);
    s = dlsym(H, b);
    if (!s) s = dlsym(H, base);
    return s;
}
static const char *errname(CUresult r) {
    const char *n = NULL;
    if (p_cuGetErrorName && p_cuGetErrorName(r, &n) == 0 && n) return n;
    return "?";
}

#define CU_MEM_ATTACH_GLOBAL 0x1u
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR 75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR 76

static const char vec_add_ptx[] =
"//\n"
".version 7.5\n"
".target sm_60\n"
".address_size 64\n"
"\n"
".visible .entry vec_add(\n"
"    .param .u64 vec_add_param_0,\n"
"    .param .u64 vec_add_param_1,\n"
"    .param .u64 vec_add_param_2,\n"
"    .param .u32 vec_add_param_3\n"
")\n"
"{\n"
"    .reg .pred  %p<2>;\n"
"    .reg .b32   %r<8>;\n"
"    .reg .b64   %rd<11>;\n"
"\n"
"    ld.param.u64    %rd1, [vec_add_param_0];\n"
"    ld.param.u64    %rd2, [vec_add_param_1];\n"
"    ld.param.u64    %rd3, [vec_add_param_2];\n"
"    ld.param.u32    %r2,  [vec_add_param_3];\n"
"    mov.u32         %r3, %ntid.x;\n"
"    mov.u32         %r4, %ctaid.x;\n"
"    mov.u32         %r5, %tid.x;\n"
"    mad.lo.s32      %r1, %r3, %r4, %r5;\n"
"    setp.ge.s32     %p1, %r1, %r2;\n"
"    @%p1 bra        $L__BB0_2;\n"
"\n"
"    cvta.to.global.u64  %rd4, %rd1;\n"
"    mul.wide.s32        %rd5, %r1, 4;\n"
"    add.s64             %rd6, %rd4, %rd5;\n"
"    cvta.to.global.u64  %rd7, %rd2;\n"
"    add.s64             %rd8, %rd7, %rd5;\n"
"    ld.global.u32       %r6, [%rd8];\n"
"    ld.global.u32       %r7, [%rd6];\n"
"    add.s32             %r6, %r7, %r6;\n"
"    cvta.to.global.u64  %rd9, %rd3;\n"
"    add.s64             %rd10, %rd9, %rd5;\n"
"    st.global.u32       [%rd10], %r6;\n"
"\n"
"$L__BB0_2:\n"
"    ret;\n"
"}\n";

static const char matmul_ptx[] =
"//\n"
".version 7.5\n"
".target sm_60\n"
".address_size 64\n"
"\n"
".visible .entry matmul(\n"
"    .param .u64 matmul_param_0,\n"
"    .param .u64 matmul_param_1,\n"
"    .param .u64 matmul_param_2,\n"
"    .param .u32 matmul_param_3\n"
")\n"
"{\n"
"    .reg .pred  %p<5>;\n"
"    .reg .b32   %r<16>;\n"
"    .reg .f32   %f<6>;\n"
"    .reg .b64   %rd<16>;\n"
"\n"
"    ld.param.u64    %rd1, [matmul_param_0];\n"
"    ld.param.u64    %rd2, [matmul_param_1];\n"
"    ld.param.u64    %rd3, [matmul_param_2];\n"
"    ld.param.u32    %r1,  [matmul_param_3];\n"
"    mov.u32         %r2, %ntid.y;\n"
"    mov.u32         %r3, %ctaid.y;\n"
"    mov.u32         %r4, %tid.y;\n"
"    mad.lo.s32      %r5, %r3, %r2, %r4;\n"
"    mov.u32         %r6, %ntid.x;\n"
"    mov.u32         %r7, %ctaid.x;\n"
"    mov.u32         %r8, %tid.x;\n"
"    mad.lo.s32      %r9, %r7, %r6, %r8;\n"
"    setp.ge.s32     %p1, %r5, %r1;\n"
"    setp.ge.s32     %p2, %r9, %r1;\n"
"    or.pred         %p3, %p1, %p2;\n"
"    @%p3 bra        $L__END;\n"
"\n"
"    mov.f32         %f1, 0f00000000;\n"
"    mov.u32         %r10, 0;\n"
"$L__LOOP:\n"
"    setp.ge.s32     %p4, %r10, %r1;\n"
"    @%p4 bra        $L__STORE;\n"
"    mad.lo.s32      %r11, %r5, %r1, %r10;\n"
"    mul.wide.s32    %rd4, %r11, 4;\n"
"    cvta.to.global.u64 %rd5, %rd1;\n"
"    add.s64         %rd6, %rd5, %rd4;\n"
"    ld.global.f32   %f2, [%rd6];\n"
"    mad.lo.s32      %r12, %r10, %r1, %r9;\n"
"    mul.wide.s32    %rd7, %r12, 4;\n"
"    cvta.to.global.u64 %rd8, %rd2;\n"
"    add.s64         %rd9, %rd8, %rd7;\n"
"    ld.global.f32   %f3, [%rd9];\n"
"    fma.rn.f32      %f1, %f2, %f3, %f1;\n"
"    add.s32         %r10, %r10, 1;\n"
"    bra.uni         $L__LOOP;\n"
"$L__STORE:\n"
"    mad.lo.s32      %r13, %r5, %r1, %r9;\n"
"    mul.wide.s32    %rd10, %r13, 4;\n"
"    cvta.to.global.u64 %rd11, %rd3;\n"
"    add.s64         %rd12, %rd11, %rd10;\n"
"    st.global.f32   [%rd12], %f1;\n"
"$L__END:\n"
"    ret;\n"
"}\n";

/* The NVIDIA userspace drivers are not always clean at process teardown when
 * live objects are still bound (an at-exit crash in the ICD would otherwise
 * turn a fully-reported PASS run into a signal death). Every probe therefore
 * runs its body and then _exit()s, skipping atexit handlers and library
 * destructors entirely. Results are already flushed line-by-line by emit(). */
static int probe_main(void) {
    CUresult r;
    Dl_info di;

    /* ---- 1. load libcuda ------------------------------------------------- */
    H = dlopen("libcuda.so.1", RTLD_NOW);
    if (!H) H = dlopen("libcuda.so", RTLD_NOW);
    if (!H) {
        emit("cuda_libcuda", "FAIL", "dlopen(libcuda.so.1): %s", dlerror());
        finish("libcuda.so.1 could not be loaded");
        return 1;
    }
    p_cuInit               = dlsym(H, "cuInit");
    p_cuDriverGetVersion   = dlsym(H, "cuDriverGetVersion");
    p_cuDeviceGetCount     = dlsym(H, "cuDeviceGetCount");
    p_cuDeviceGet          = dlsym(H, "cuDeviceGet");
    p_cuDeviceGetName      = dlsym(H, "cuDeviceGetName");
    p_cuDeviceGetAttribute = dlsym(H, "cuDeviceGetAttribute");
    p_cuGetErrorName       = dlsym(H, "cuGetErrorName");
    p_cuCtxCreate          = sym2("cuCtxCreate");
    p_cuMemAlloc           = sym2("cuMemAlloc");
    p_cuMemFree            = sym2("cuMemFree");
    p_cuMemcpyHtoD         = sym2("cuMemcpyHtoD");
    p_cuMemcpyDtoH         = sym2("cuMemcpyDtoH");
    p_cuMemsetD8           = sym2("cuMemsetD8");
    p_cuModuleLoadData     = dlsym(H, "cuModuleLoadData");
    p_cuModuleGetFunction  = dlsym(H, "cuModuleGetFunction");
    p_cuLaunchKernel       = dlsym(H, "cuLaunchKernel");
    p_cuCtxSynchronize     = dlsym(H, "cuCtxSynchronize");
    /* No _v2 for this one -- and sym2() would be actively wrong here, the way
     * it is for cuCtxSynchronize: libcuda 13 exports a _v2 with different
     * semantics for several entry points, so only the ones that really are
     * _v2-versioned above go through sym2(). */
    p_cuMemAllocManaged    = dlsym(H, "cuMemAllocManaged");

    if (!p_cuInit || !p_cuDeviceGetCount || !p_cuCtxCreate || !p_cuMemAlloc) {
        emit("cuda_libcuda", "FAIL", "libcuda loaded but core symbols missing");
        finish("libcuda missing core symbols");
        return 1;
    }
    /* report WHICH libcuda answered -- a stale/unstaged one is a real bug */
    if (dladdr((void *)p_cuInit, &di) && di.dli_fname)
        emit("cuda_libcuda", "PASS", "%s", di.dli_fname);
    else
        emit("cuda_libcuda", "PASS", "loaded (path unknown)");

    /* ---- 2. cuInit ------------------------------------------------------- */
    r = p_cuInit(0);
    if (r != 0) {
        emit("cuda_init", "FAIL", "cuInit rc=%d (%s)", r, errname(r));
        finish("cuInit failed");
        return 1;
    }
    emit("cuda_init", "PASS", "rc=0");

    /* ---- 3. driver version ------------------------------------------------ */
    if (p_cuDriverGetVersion) {
        int dv = 0;
        r = p_cuDriverGetVersion(&dv);
        if (r != 0) emit("cuda_driver_version", "FAIL", "rc=%d (%s)", r, errname(r));
        else        emit("cuda_driver_version", "PASS", "CUDA %d.%d (raw %d)",
                         dv / 1000, (dv % 1000) / 10, dv);
    } else {
        emit("cuda_driver_version", "SKIP", "cuDriverGetVersion not exported");
    }

    /* ---- 4. device count -------------------------------------------------- */
    int ndev = 0;
    r = p_cuDeviceGetCount(&ndev);
    if (r != 0) {
        emit("cuda_device_count", "FAIL", "rc=%d (%s)", r, errname(r));
        finish("cuDeviceGetCount failed");
        return 1;
    }
    if (ndev < 1) {
        emit("cuda_device_count", "FAIL", "rc=0 but count=%d", ndev);
        finish("no CUDA devices");
        return 1;
    }
    emit("cuda_device_count", "PASS", "%d", ndev);

    CUdevice dev = 0;
    r = p_cuDeviceGet(&dev, 0);
    if (r != 0) {
        emit("cuda_device_name", "FAIL", "cuDeviceGet rc=%d (%s)", r, errname(r));
        finish("cuDeviceGet failed");
        return 1;
    }

    /* ---- 5. device name --------------------------------------------------- */
    char name[256] = {0};
    r = p_cuDeviceGetName(name, sizeof name, dev);
    if (r != 0)            emit("cuda_device_name", "FAIL", "rc=%d (%s)", r, errname(r));
    else if (!name[0])     emit("cuda_device_name", "FAIL", "rc=0 but name is empty");
    else                   emit("cuda_device_name", "PASS", "%s", name);

    /* ---- 6. compute capability -------------------------------------------- */
    if (p_cuDeviceGetAttribute) {
        int maj = 0, min = 0;
        CUresult r1 = p_cuDeviceGetAttribute(&maj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
        CUresult r2 = p_cuDeviceGetAttribute(&min, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
        if (r1 || r2) emit("cuda_compute_cap", "FAIL", "rc=%d/%d", r1, r2);
        else if (maj == 0) emit("cuda_compute_cap", "FAIL", "reported sm_%d%d", maj, min);
        else emit("cuda_compute_cap", "PASS", "sm_%d%d", maj, min);
    } else {
        emit("cuda_compute_cap", "SKIP", "cuDeviceGetAttribute not exported");
    }

    /* ---- 7. context ------------------------------------------------------- */
    CUcontext ctx = NULL;
    r = p_cuCtxCreate(&ctx, 0, dev);
    if (r != 0) {
        emit("cuda_ctx_create", "FAIL", "rc=%d (%s)", r, errname(r));
        finish("cuCtxCreate failed -- no context, nothing downstream can run");
        return 1;
    }
    emit("cuda_ctx_create", "PASS", "rc=0 ctx=%p", (void *)ctx);

    /* ---- 8. 8 MiB HtoD/DtoH round trip, byte-exact ------------------------ */
    const size_t NBYTES = 8u * 1024u * 1024u;   /* >= 8 MiB as required */
    unsigned char *src = malloc(NBYTES), *dst = malloc(NBYTES);
    if (!src || !dst) {
        emit("cuda_htod_dtoh_8mib", "SKIP", "host malloc of 2x%zu failed", NBYTES);
    } else {
        /* deterministic, non-trivial pattern: an LCG, so a partially-copied
           buffer cannot accidentally compare equal */
        uint32_t s = 0x12345678u; size_t i;
        for (i = 0; i < NBYTES; i++) { s = s * 1103515245u + 12345u; src[i] = (unsigned char)(s >> 16); }
        memset(dst, 0xCD, NBYTES);

        CUdeviceptr d = 0;
        r = p_cuMemAlloc(&d, NBYTES);
        if (r != 0) {
            emit("cuda_htod_dtoh_8mib", "FAIL", "cuMemAlloc(%zu) rc=%d (%s)", NBYTES, r, errname(r));
        } else {
            CUresult rh = p_cuMemcpyHtoD(d, src, NBYTES);
            CUresult rd = p_cuMemcpyDtoH(dst, d, NBYTES);
            if (rh || rd) {
                emit("cuda_htod_dtoh_8mib", "FAIL", "HtoD rc=%d DtoH rc=%d", rh, rd);
            } else if (memcmp(src, dst, NBYTES) != 0) {
                size_t off = 0; while (off < NBYTES && src[off] == dst[off]) off++;
                size_t bad = 0; for (i = 0; i < NBYTES; i++) if (src[i] != dst[i]) bad++;
                emit("cuda_htod_dtoh_8mib", "FAIL",
                     "%zu MiB round trip: %zu bytes differ, first at offset %zu (src=0x%02x dst=0x%02x)",
                     NBYTES >> 20, bad, off, src[off], dst[off]);
            } else {
                emit("cuda_htod_dtoh_8mib", "PASS",
                     "%zu MiB round trip byte-exact (%zu/%zu bytes verified)",
                     NBYTES >> 20, NBYTES, NBYTES);
            }

            /* ---- 9. cuMemsetD8, verified by reading back ------------------ */
            if (!p_cuMemsetD8) {
                emit("cuda_memset_d8", "SKIP", "cuMemsetD8 not exported");
            } else {
                int ok = 1; char detail[256] = {0};
                unsigned char pats[2] = { 0xA5, 0x00 };
                int pi;
                for (pi = 0; pi < 2 && ok; pi++) {
                    CUresult rm = p_cuMemsetD8(d, pats[pi], NBYTES);
                    if (rm) { ok = 0; snprintf(detail, sizeof detail, "cuMemsetD8(0x%02x) rc=%d (%s)", pats[pi], rm, errname(rm)); break; }
                    memset(dst, (int)~pats[pi], NBYTES);
                    CUresult rr = p_cuMemcpyDtoH(dst, d, NBYTES);
                    if (rr) { ok = 0; snprintf(detail, sizeof detail, "readback DtoH rc=%d", rr); break; }
                    size_t bad = 0, first = (size_t)-1;
                    for (i = 0; i < NBYTES; i++)
                        if (dst[i] != pats[pi]) { bad++; if (first == (size_t)-1) first = i; }
                    if (bad) {
                        ok = 0;
                        snprintf(detail, sizeof detail,
                                 "pattern 0x%02x: %zu/%zu bytes wrong, first at %zu (got 0x%02x)",
                                 pats[pi], bad, NBYTES, first, dst[first]);
                    }
                }
                if (ok) emit("cuda_memset_d8", "PASS",
                             "0xA5 and 0x00 over %zu MiB, all %zu bytes verified by readback",
                             NBYTES >> 20, NBYTES);
                else    emit("cuda_memset_d8", "FAIL", "%s", detail);
            }
            p_cuMemFree(d);
        }
    }

    /* ---- 10. PTX JIT ------------------------------------------------------ */
    /* cuModuleLoadData on PTX *source* forces the JIT path. This is what
       catches a missing libnvidia-ptxjitcompiler / libnvidia-nvvm, which
       surfaces as CUDA_ERROR_JIT_COMPILER_NOT_FOUND (221). */
    CUmodule mod = NULL;
    if (!p_cuModuleLoadData) {
        emit("cuda_ptx_jit", "SKIP", "cuModuleLoadData not exported");
        finish("no cuModuleLoadData");
        return 1;
    }
    r = p_cuModuleLoadData(&mod, vec_add_ptx);
    if (r == 221) {
        emit("cuda_ptx_jit", "FAIL",
             "rc=221 CUDA_ERROR_JIT_COMPILER_NOT_FOUND -- libnvidia-ptxjitcompiler.so.1 "
             "or libnvidia-nvvm.so.4 is missing/unstaged in the guest");
        finish("PTX JIT compiler not available");
        return 1;
    }
    if (r != 0) {
        emit("cuda_ptx_jit", "FAIL", "cuModuleLoadData(PTX) rc=%d (%s)", r, errname(r));
        finish("PTX module load failed");
        return 1;
    }
    emit("cuda_ptx_jit", "PASS", "cuModuleLoadData(PTX .version 7.5/.target sm_60) rc=0");

    /* ---- 11. real kernel launch, output checked against expected ---------- */
    CUfunction fn = NULL;
    r = p_cuModuleGetFunction(&fn, mod, "vec_add");
    if (r != 0) {
        emit("cuda_kernel_launch", "FAIL", "cuModuleGetFunction(vec_add) rc=%d (%s)", r, errname(r));
    } else {
        const int N = 1 << 20;                      /* 1,048,576 elements */
        size_t bytes = (size_t)N * sizeof(int);
        int *ha = malloc(bytes), *hb = malloc(bytes), *hc = malloc(bytes);
        CUdeviceptr da = 0, db = 0, dc = 0;
        if (!ha || !hb || !hc) {
            emit("cuda_kernel_launch", "SKIP", "host malloc failed");
        } else {
            int i;
            for (i = 0; i < N; i++) { ha[i] = i; hb[i] = 2 * i; hc[i] = -1; }
            CUresult e = 0;
            e |= p_cuMemAlloc(&da, bytes); e |= p_cuMemAlloc(&db, bytes); e |= p_cuMemAlloc(&dc, bytes);
            e |= p_cuMemcpyHtoD(da, ha, bytes); e |= p_cuMemcpyHtoD(db, hb, bytes);
            e |= p_cuMemsetD8 ? p_cuMemsetD8(dc, 0, bytes) : 0;
            if (e) {
                emit("cuda_kernel_launch", "FAIL", "setup rc=%d", e);
            } else {
                int n = N;
                void *args[] = { &da, &db, &dc, &n };
                int threads = 256, blocks = (N + threads - 1) / threads;
                r = p_cuLaunchKernel(fn, blocks, 1, 1, threads, 1, 1, 0, NULL, args, NULL);
                CUresult rs = p_cuCtxSynchronize ? p_cuCtxSynchronize() : 0;
                if (r || rs) {
                    emit("cuda_kernel_launch", "FAIL",
                         "cuLaunchKernel rc=%d (%s) sync rc=%d", r, errname(r), rs);
                } else if ((r = p_cuMemcpyDtoH(hc, dc, bytes)) != 0) {
                    emit("cuda_kernel_launch", "FAIL", "result DtoH rc=%d (%s)", r, errname(r));
                } else {
                    /* verify EVERY element against the expected value */
                    long bad = 0; int firsti = -1, firstgot = 0;
                    for (i = 0; i < N; i++) {
                        if (hc[i] != 3 * i) {
                            bad++;
                            if (firsti < 0) { firsti = i; firstgot = hc[i]; }
                        }
                    }
                    if (bad) emit("cuda_kernel_launch", "FAIL",
                                  "vec_add: %ld/%d elements wrong, first i=%d expected %d got %d",
                                  bad, N, firsti, 3 * firsti, firstgot);
                    else     emit("cuda_kernel_launch", "PASS",
                                  "vec_add<<<%d,%d>>> all %d elements == 3*i (e.g. c[%d]=%d)",
                                  blocks, threads, N, N - 1, hc[N - 1]);
                }
            }
            if (da) p_cuMemFree(da); if (db) p_cuMemFree(db); if (dc) p_cuMemFree(dc);
            free(ha); free(hb); free(hc);
        }
    }

    /* ---- 12. matmul vs CPU reference, tolerance-checked ------------------- */
    CUmodule mm = NULL;
    r = p_cuModuleLoadData(&mm, matmul_ptx);
    if (r != 0) {
        emit("cuda_matmul", "FAIL", "cuModuleLoadData(matmul PTX) rc=%d (%s)", r, errname(r));
    } else {
        CUfunction f = NULL;
        r = p_cuModuleGetFunction(&f, mm, "matmul");
        if (r != 0) {
            emit("cuda_matmul", "FAIL", "cuModuleGetFunction(matmul) rc=%d (%s)", r, errname(r));
        } else {
            const int N = 256;
            size_t bytes = (size_t)N * N * sizeof(float);
            float *A = malloc(bytes), *B = malloc(bytes), *C = malloc(bytes), *R = malloc(bytes);
            CUdeviceptr da = 0, db = 0, dc = 0;
            if (!A || !B || !C || !R) {
                emit("cuda_matmul", "SKIP", "host malloc failed");
            } else {
                int i, j, k; uint32_t s = 0xBEEF1234u;
                for (i = 0; i < N * N; i++) { s = s * 1103515245u + 12345u; A[i] = (float)((s >> 20) % 17) - 8.0f; }
                for (i = 0; i < N * N; i++) { s = s * 1103515245u + 12345u; B[i] = (float)((s >> 20) % 13) - 6.0f; }
                CUresult e = 0;
                e |= p_cuMemAlloc(&da, bytes); e |= p_cuMemAlloc(&db, bytes); e |= p_cuMemAlloc(&dc, bytes);
                e |= p_cuMemcpyHtoD(da, A, bytes); e |= p_cuMemcpyHtoD(db, B, bytes);
                if (e) {
                    emit("cuda_matmul", "FAIL", "setup rc=%d", e);
                } else {
                    int n = N;
                    void *args[] = { &da, &db, &dc, &n };
                    unsigned tb = 16, gr = (unsigned)((N + 15) / 16);
                    r = p_cuLaunchKernel(f, gr, gr, 1, tb, tb, 1, 0, NULL, args, NULL);
                    CUresult rs = p_cuCtxSynchronize ? p_cuCtxSynchronize() : 0;
                    if (r || rs) {
                        emit("cuda_matmul", "FAIL", "launch rc=%d (%s) sync rc=%d", r, errname(r), rs);
                    } else if ((r = p_cuMemcpyDtoH(C, dc, bytes)) != 0) {
                        emit("cuda_matmul", "FAIL", "DtoH rc=%d (%s)", r, errname(r));
                    } else {
                        /* full CPU reference */
                        for (i = 0; i < N; i++)
                            for (j = 0; j < N; j++) {
                                float acc = 0.0f;
                                for (k = 0; k < N; k++) acc += A[i * N + k] * B[k * N + j];
                                R[i * N + j] = acc;
                            }
                        double maxabs = 0.0; long bad = 0; int bi = -1, bj = -1;
                        for (i = 0; i < N; i++)
                            for (j = 0; j < N; j++) {
                                double d = fabs((double)C[i * N + j] - (double)R[i * N + j]);
                                double tol = 1e-2 + 1e-3 * fabs((double)R[i * N + j]);
                                if (d > maxabs) { maxabs = d; }
                                if (d > tol) { bad++; if (bi < 0) { bi = i; bj = j; } }
                            }
                        if (bad) emit("cuda_matmul", "FAIL",
                                      "%dx%d fp32: %ld/%d elements outside tolerance, worst |d|=%.6g, "
                                      "first bad C[%d][%d]=%.6g ref=%.6g",
                                      N, N, bad, N * N, maxabs, bi, bj,
                                      (double)C[bi * N + bj], (double)R[bi * N + bj]);
                        else     emit("cuda_matmul", "PASS",
                                      "%dx%d fp32 vs CPU ref: all %d elements within tol, max|delta|=%.3g "
                                      "(C[0][0]=%.4f ref=%.4f)",
                                      N, N, N * N, maxabs, (double)C[0], (double)R[0]);
                    }
                }
                if (da) p_cuMemFree(da); if (db) p_cuMemFree(db); if (dc) p_cuMemFree(dc);
                free(A); free(B); free(C); free(R);
            }
        }
    }

    /* ---- 13/14. unified (managed) memory --------------------------------
     *
     * This is the check whose absence let "cuMemAllocManaged fails in the
     * guest" sit undetected on every platform this suite has ever passed on.
     * The old UVM coverage was `/dev/nvidia-uvm` appearing in the device-node
     * list -- a node existing says nothing about managed memory working, and
     * managed memory is what most ML frameworks allocate through.
     *
     * Managed memory is a different code path from every check above, not a
     * variation on one: the range is created by mmap() on /dev/nvidia-uvm
     * rather than by an ioctl, the pages are migrated on fault rather than
     * copied by cuMemcpy, and the CPU dereferences the device pointer
     * directly.  So it is checked the only way that proves it: write the
     * inputs from the CPU THROUGH the managed pointer (no cuMemcpy anywhere),
     * run a real kernel over them, and read every output element back through
     * the same pointer.  Both directions of coherence, verified by value.
     */
    if (!p_cuMemAllocManaged) {
        emit("cuda_managed_alloc", "SKIP", "cuMemAllocManaged not exported by libcuda");
    } else {
        /* 83 = CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY */
        int managed_cap = 1;
        if (p_cuDeviceGetAttribute &&
            p_cuDeviceGetAttribute(&managed_cap, 83, dev) != 0)
            managed_cap = 1;   /* attribute unreadable -- try anyway */

        const int N = 1 << 20;                        /* 1,048,576 elements */
        size_t bytes = (size_t)N * sizeof(int);
        CUdeviceptr ma = 0, mb = 0, mc = 0;
        CUresult ra = p_cuMemAllocManaged(&ma, bytes, CU_MEM_ATTACH_GLOBAL);
        CUresult rb = ra ? ra : p_cuMemAllocManaged(&mb, bytes, CU_MEM_ATTACH_GLOBAL);
        CUresult rc2 = rb ? rb : p_cuMemAllocManaged(&mc, bytes, CU_MEM_ATTACH_GLOBAL);

        if (ra || rb || rc2) {
            CUresult bad = ra ? ra : (rb ? rb : rc2);
            if (!managed_cap)
                emit("cuda_managed_alloc", "SKIP",
                     "device reports MANAGED_MEMORY=0 and cuMemAllocManaged rc=%d (%s)",
                     bad, errname(bad));
            else
                emit("cuda_managed_alloc", "FAIL",
                     "cuMemAllocManaged(%zu) rc=%d (%s) on a device that reports "
                     "MANAGED_MEMORY=1 -- unified memory is unavailable in this guest",
                     bytes, bad, errname(bad));
        } else if (!ma || !mb || !mc) {
            emit("cuda_managed_alloc", "FAIL",
                 "cuMemAllocManaged rc=0 but returned a null pointer (a=0x%llx b=0x%llx c=0x%llx)",
                 (unsigned long long)ma, (unsigned long long)mb,
                 (unsigned long long)mc);
        } else {
            emit("cuda_managed_alloc", "PASS",
                 "3 x %zu bytes, MANAGED_MEMORY=%d (a=0x%llx b=0x%llx c=0x%llx)",
                 bytes, managed_cap, (unsigned long long)ma,
                 (unsigned long long)mb, (unsigned long long)mc);

            /* ---- 14. host<->device coherence over the managed pages ------ */
            CUfunction mf = NULL;
            CUresult rf = p_cuModuleGetFunction(&mf, mod, "vec_add");
            if (rf != 0 || !mf) {
                emit("cuda_managed_coherence", "SKIP",
                     "cuModuleGetFunction(vec_add) rc=%d (%s)", rf, errname(rf));
            } else {
                /* CPU writes the inputs directly through the managed pointers. */
                int *pa = (int *)ma, *pb = (int *)mb, *pc = (int *)mc;
                int i;
                for (i = 0; i < N; i++) { pa[i] = i; pb[i] = 2 * i; pc[i] = -1; }

                int n = N;
                void *args[] = { &ma, &mb, &mc, &n };
                int threads = 256, blocks = (N + threads - 1) / threads;
                CUresult rl = p_cuLaunchKernel(mf, blocks, 1, 1, threads, 1, 1,
                                               0, NULL, args, NULL);
                CUresult rs = p_cuCtxSynchronize ? p_cuCtxSynchronize() : 0;
                if (rl || rs) {
                    emit("cuda_managed_coherence", "FAIL",
                         "vec_add over managed memory: launch rc=%d (%s) sync rc=%d (%s)",
                         rl, errname(rl), rs, errname(rs));
                } else {
                    /* CPU reads every output element back through the pointer. */
                    long bad = 0;
                    int firsti = -1, firstgot = 0, firstwant = 0, firstcycle = 0;
                    for (i = 0; i < N; i++) {
                        if (pc[i] != 3 * i) {
                            bad++;
                            if (firsti < 0) {
                                firsti = i; firstgot = pc[i];
                                firstwant = 3 * i; firstcycle = 0;
                            }
                        }
                    }
                    /*
                     * Second and third cycles: the pages have now been pulled
                     * to the CPU by the read above, so re-running the kernel
                     * migrates them back to the GPU and the next CPU read
                     * pulls them across again.  One cycle can pass on a system
                     * where migration is subtly broken -- e.g. where the first
                     * fault happens to be serviced but a later invalidation is
                     * not -- so thrash it and re-verify by value each time,
                     * with different inputs so a stale page cannot compare
                     * equal.  Same shape as tests/integration/cuda_micro.c
                     * case 6 (uvm_migrate), but checked rather than timed.
                     */
                    int cycle;
                    for (cycle = 1; cycle <= 2 && !bad; cycle++) {
                        for (i = 0; i < N; i++) {
                            pa[i] = i + cycle;      /* CPU writes -> pages to CPU */
                            pb[i] = 2 * i + cycle;
                            pc[i] = -1;
                        }
                        rl = p_cuLaunchKernel(mf, blocks, 1, 1, threads, 1, 1,
                                              0, NULL, args, NULL);
                        rs = p_cuCtxSynchronize ? p_cuCtxSynchronize() : 0;
                        if (rl || rs) {
                            emit("cuda_managed_coherence", "FAIL",
                                 "migration cycle %d: launch rc=%d (%s) sync rc=%d (%s)",
                                 cycle, rl, errname(rl), rs, errname(rs));
                            bad = -1;
                            break;
                        }
                        for (i = 0; i < N; i++) {   /* CPU reads -> pages back */
                            if (pc[i] != 3 * i + 2 * cycle) {
                                bad++;
                                if (firsti < 0) {
                                    firsti = i; firstgot = pc[i];
                                    firstwant = 3 * i + 2 * cycle;
                                    firstcycle = cycle;
                                }
                            }
                        }
                    }

                    if (bad < 0) {
                        /* already reported */
                    } else if (bad)
                        emit("cuda_managed_coherence", "FAIL",
                             "%ld/%d managed elements wrong on migration cycle %d, "
                             "first i=%d expected %d got %d",
                             bad, N, firstcycle, firsti, firstwant, firstgot);
                    else
                        emit("cuda_managed_coherence", "PASS",
                             "host wrote %d elements through the managed pointer, "
                             "vec_add<<<%d,%d>>> ran on them, host read all %d back; "
                             "3 CPU<->GPU migration cycles, every element verified "
                             "(e.g. c[%d]=%d)",
                             N, blocks, threads, N, N - 1, pc[N - 1]);
                }
            }
        }
        if (ma) p_cuMemFree(ma);
        if (mb) p_cuMemFree(mb);
        if (mc) p_cuMemFree(mc);
    }

    finish("not reached");
    return 0;
}

int main(void) { int rc = probe_main(); fflush(NULL); _exit(rc); }
NVKVM_CUDA_EOF

if [ -n "$BUILD_PROBES" ]; then
    emit_probe cuda_probe cuda_probe.c -ldl -lm
else
    provide_probe cuda_probe cuda_probe.c -ldl -lm; PROBE_RC=$?
fi
if [ -n "$BUILD_PROBES" ]; then
    :
elif [ "$PROBE_RC" = 127 ]; then
    untested_remaining "$NO_PROBE_REASON" $CUDA_CHECKS
elif [ "$PROBE_RC" != 0 ]; then
    skip_remaining "cuda_probe.c failed to compile (see compiler output above)" $CUDA_CHECKS
else
    printf ' %scuda_probe: %s%s\n' "$C_DIM" "$PROBE_NOTE" "$C_RESET"
    run_probe cuda_probe
    CUDA_RC=$?
    ingest "$WORK/cuda_probe.out"
    note_probe_timeout
    if [ $CUDA_RC -ge 128 ]; then
        skip_remaining "cuda_probe died on signal $((CUDA_RC-128)); tail: $(tail -2 "$WORK/cuda_probe.out" | tr '\n' ' ')" $CUDA_CHECKS
    else
        skip_remaining "cuda_probe exited $CUDA_RC without reporting this check" $CUDA_CHECKS
    fi
fi

# =============================================================================
# PHASE 3 -- Vulkan
# =============================================================================
section "3. Vulkan (enumeration + compute dispatch)"

VK_CHECKS="vk_loader vk_instance vk_physical_device vk_device_is_nvidia vk_compute_dispatch"

# SPIR-V for:
#   #version 450
#   layout(local_size_x = 64) in;
#   layout(std430, binding = 0) buffer Buf { uint data[]; };
#   void main() { uint i = gl_GlobalInvocationID.x; data[i] = data[i] * 3u + 7u; }
# Built with glslangValidator -V. Embedded base64 so the suite needs no
# shader compiler at run time. Verified by md5 below before use.
cat > "$WORK/comp.spv.b64" <<'NVKVM_SPV_EOF'
AwIjBwAAAQAKAAgAIwAAAAAAAAARAAIAAQAAAAsABgABAAAAR0xTTC5zdGQuNDUwAAAAAA4AAwAA
AAAAAQAAAA8ABgAFAAAABAAAAG1haW4AAAAACwAAABAABgAEAAAAEQAAAEAAAAABAAAAAQAAAAMA
AwACAAAAwgEAAAUABAAEAAAAbWFpbgAAAAAFAAMACAAAAGkAAAAFAAgACwAAAGdsX0dsb2JhbElu
dm9jYXRpb25JRAAAAAUAAwARAAAAQnVmAAYABQARAAAAAAAAAGRhdGEAAAAABQADABMAAAAAAAAA
RwAEAAsAAAALAAAAHAAAAEcABAAQAAAABgAAAAQAAABIAAUAEQAAAAAAAAAjAAAAAAAAAEcAAwAR
AAAAAwAAAEcABAATAAAAIgAAAAAAAABHAAQAEwAAACEAAAAAAAAARwAEACIAAAALAAAAGQAAABMA
AgACAAAAIQADAAMAAAACAAAAFQAEAAYAAAAgAAAAAAAAACAABAAHAAAABwAAAAYAAAAXAAQACQAA
AAYAAAADAAAAIAAEAAoAAAABAAAACQAAADsABAAKAAAACwAAAAEAAAArAAQABgAAAAwAAAAAAAAA
IAAEAA0AAAABAAAABgAAAB0AAwAQAAAABgAAAB4AAwARAAAAEAAAACAABAASAAAAAgAAABEAAAA7
AAQAEgAAABMAAAACAAAAFQAEABQAAAAgAAAAAQAAACsABAAUAAAAFQAAAAAAAAAgAAQAGAAAAAIA
AAAGAAAAKwAEAAYAAAAbAAAAAwAAACsABAAGAAAAHQAAAAcAAAArAAQABgAAACAAAABAAAAAKwAE
AAYAAAAhAAAAAQAAACwABgAJAAAAIgAAACAAAAAhAAAAIQAAADYABQACAAAABAAAAAAAAAADAAAA
+AACAAUAAAA7AAQABwAAAAgAAAAHAAAAQQAFAA0AAAAOAAAACwAAAAwAAAA9AAQABgAAAA8AAAAO
AAAAPgADAAgAAAAPAAAAPQAEAAYAAAAWAAAACAAAAD0ABAAGAAAAFwAAAAgAAABBAAYAGAAAABkA
AAATAAAAFQAAABcAAAA9AAQABgAAABoAAAAZAAAAhAAFAAYAAAAcAAAAGgAAABsAAACAAAUABgAA
AB4AAAAcAAAAHQAAAEEABgAYAAAAHwAAABMAAAAVAAAAFgAAAD4AAwAfAAAAHgAAAP0AAQA4AAEA
NVKVM_SPV_EOF

SPV_OK=1
if ! base64 -d < "$WORK/comp.spv.b64" > "$WORK/comp.spv" 2>"$WORK/spv.err"; then
    SPV_OK=0; SPV_ERR="base64 decode failed: $(cat "$WORK/spv.err")"
else
    # SPIR-V magic is 0x07230203, little-endian on disk: 03 02 23 07
    MAGIC="$(od -An -tx1 -N4 "$WORK/comp.spv" | tr -d ' \n')"
    SPVSZ="$(wc -c < "$WORK/comp.spv" | tr -d ' ')"
    if [ "$MAGIC" != "03022307" ]; then
        SPV_OK=0; SPV_ERR="embedded SPIR-V has bad magic $MAGIC (expected 03022307)"
    fi
fi

cat > "$WORK/vk_probe.c" <<'NVKVM_VK_EOF'
/*
 * nvkvm validation -- Vulkan probe.
 *
 * dlopen()s libvulkan.so.1 and hand-rolls the ~20 structs it needs, so it
 * builds with no Vulkan SDK and no vulkan.h. Argument 1 is a path to a
 * compute SPIR-V module.
 *
 * Checks:
 *   vk_loader           -- the loader is present and dispatchable
 *   vk_instance         -- vkCreateInstance succeeds
 *   vk_physical_device  -- at least one physical device, name reported
 *   vk_device_is_nvidia -- vendorID == 0x10DE and the name is NOT a software
 *                          rasteriser. llvmpipe/lavapipe/swrast is a FAIL:
 *                          silently falling back to software is exactly the
 *                          regression this suite exists to catch.
 *   vk_compute_dispatch -- runs data[i] = data[i]*3+7 over 4096 elements on
 *                          the GPU and verifies every element.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <dlfcn.h>

/* ---- handles: dispatchable are pointers, non-dispatchable are uint64 ----- */
typedef void *VkInstance, *VkPhysicalDevice, *VkDevice, *VkQueue, *VkCommandBuffer;
typedef uint64_t VkBuffer, VkDeviceMemory, VkDescriptorSetLayout, VkPipelineLayout,
                 VkShaderModule, VkPipeline, VkPipelineCache, VkDescriptorPool,
                 VkDescriptorSet, VkCommandPool, VkFence;
typedef int VkResult;
typedef uint64_t VkDeviceSize;

#define VK_SUCCESS 0
#define VK_WHOLE_SIZE (~0ULL)

typedef struct { uint32_t sType; const void *pNext; const char *pApplicationName; uint32_t applicationVersion;
                 const char *pEngineName; uint32_t engineVersion; uint32_t apiVersion; } VkApplicationInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; const VkApplicationInfo *pApplicationInfo;
                 uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
                 uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames; } VkInstanceCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; uint32_t queueFamilyIndex;
                 uint32_t queueCount; const float *pQueuePriorities; } VkDeviceQueueCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; uint32_t queueCreateInfoCount;
                 const VkDeviceQueueCreateInfo *pQueueCreateInfos; uint32_t enabledLayerCount;
                 const char *const *ppEnabledLayerNames; uint32_t enabledExtensionCount;
                 const char *const *ppEnabledExtensionNames; const void *pEnabledFeatures; } VkDeviceCreateInfo;
typedef struct { uint32_t queueFlags; uint32_t queueCount; uint32_t timestampValidBits;
                 uint32_t w, h, d; } VkQueueFamilyProperties;
typedef struct { uint32_t propertyFlags; uint32_t heapIndex; } VkMemoryType;
typedef struct { VkDeviceSize size; uint32_t flags; } VkMemoryHeap;
typedef struct { uint32_t memoryTypeCount; VkMemoryType memoryTypes[32];
                 uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16]; } VkPhysicalDeviceMemoryProperties;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; VkDeviceSize size; uint32_t usage;
                 uint32_t sharingMode; uint32_t queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices; } VkBufferCreateInfo;
typedef struct { VkDeviceSize size; VkDeviceSize alignment; uint32_t memoryTypeBits; } VkMemoryRequirements;
typedef struct { uint32_t sType; const void *pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex; } VkMemoryAllocateInfo;
typedef struct { uint32_t binding; uint32_t descriptorType; uint32_t descriptorCount; uint32_t stageFlags;
                 const void *pImmutableSamplers; } VkDescriptorSetLayoutBinding;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; uint32_t bindingCount;
                 const VkDescriptorSetLayoutBinding *pBindings; } VkDescriptorSetLayoutCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; uint32_t setLayoutCount;
                 const VkDescriptorSetLayout *pSetLayouts; uint32_t pushConstantRangeCount;
                 const void *pPushConstantRanges; } VkPipelineLayoutCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; size_t codeSize; const uint32_t *pCode; } VkShaderModuleCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; uint32_t stage; VkShaderModule module;
                 const char *pName; const void *pSpecializationInfo; } VkPipelineShaderStageCreateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; VkPipelineShaderStageCreateInfo stage;
                 VkPipelineLayout layout; VkPipeline basePipelineHandle; int32_t basePipelineIndex; } VkComputePipelineCreateInfo;
typedef struct { uint32_t type; uint32_t descriptorCount; } VkDescriptorPoolSize;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; uint32_t maxSets;
                 uint32_t poolSizeCount; const VkDescriptorPoolSize *pPoolSizes; } VkDescriptorPoolCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkDescriptorPool descriptorPool;
                 uint32_t descriptorSetCount; const VkDescriptorSetLayout *pSetLayouts; } VkDescriptorSetAllocateInfo;
typedef struct { VkBuffer buffer; VkDeviceSize offset; VkDeviceSize range; } VkDescriptorBufferInfo;
typedef struct { uint32_t sType; const void *pNext; VkDescriptorSet dstSet; uint32_t dstBinding;
                 uint32_t dstArrayElement; uint32_t descriptorCount; uint32_t descriptorType;
                 const void *pImageInfo; const VkDescriptorBufferInfo *pBufferInfo;
                 const void *pTexelBufferView; } VkWriteDescriptorSet;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; uint32_t queueFamilyIndex; } VkCommandPoolCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkCommandPool commandPool; uint32_t level;
                 uint32_t commandBufferCount; } VkCommandBufferAllocateInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; const void *pInheritanceInfo; } VkCommandBufferBeginInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t waitSemaphoreCount; const void *pWaitSemaphores;
                 const uint32_t *pWaitDstStageMask; uint32_t commandBufferCount;
                 const VkCommandBuffer *pCommandBuffers; uint32_t signalSemaphoreCount;
                 const void *pSignalSemaphores; } VkSubmitInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; } VkFenceCreateInfo;

#define ST_APP_INFO        0
#define ST_INSTANCE_CI     1
#define ST_DEVQUEUE_CI     2
#define ST_DEVICE_CI       3
#define ST_SUBMIT_INFO     4
#define ST_MEMALLOC_INFO   5
#define ST_FENCE_CI        8
#define ST_BUFFER_CI      12
#define ST_SHADERMOD_CI   16
#define ST_PSSCI          18
#define ST_COMPUTE_PIPE_CI 29
#define ST_PIPELAYOUT_CI  30
#define ST_DSL_CI         32
#define ST_DPOOL_CI       33
#define ST_DSET_ALLOC     34
#define ST_WRITE_DSET     35
#define ST_CMDPOOL_CI     39
#define ST_CMDBUF_ALLOC   40
#define ST_CMDBUF_BEGIN   42

#define VK_BUFFER_USAGE_STORAGE_BUFFER_BIT 0x20
#define VK_DESCRIPTOR_TYPE_STORAGE_BUFFER  7
#define VK_SHADER_STAGE_COMPUTE_BIT        0x20
#define VK_QUEUE_COMPUTE_BIT               0x02
#define VK_PIPELINE_BIND_POINT_COMPUTE     1
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  0x02
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x04

static const char *ALL_CHECKS[] = { "vk_loader", "vk_instance", "vk_physical_device",
                                    "vk_device_is_nvidia", "vk_compute_dispatch", NULL };
static int reported[16];
static void emit(const char *name, const char *status, const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    int i; for (i = 0; ALL_CHECKS[i]; i++) if (!strcmp(ALL_CHECKS[i], name)) reported[i] = 1;
    printf("CHECK|%s|%s|%s\n", name, status, buf); fflush(stdout);
}
static void finish(const char *reason) {
    int i; for (i = 0; ALL_CHECKS[i]; i++) if (!reported[i]) printf("CHECK|%s|SKIP|%s\n", ALL_CHECKS[i], reason);
    fflush(stdout);
}

static void *L;
#define VKSYM(v, n) do { *(void **)(&v) = dlsym(L, n); } while (0)

/* see the note in the CUDA probe: body + _exit(), never a normal return, so a
 * teardown crash inside the ICD cannot mask an otherwise complete result set */
static int probe_main(int argc, char **argv) {
    const char *spv_path = (argc > 1) ? argv[1] : "comp.spv";

    L = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!L) L = dlopen("libvulkan.so", RTLD_NOW);
    if (!L) {
        emit("vk_loader", "SKIP", "dlopen(libvulkan.so.1): %s -- install libvulkan1 in the guest", dlerror());
        finish("no Vulkan loader");
        return 0;
    }

    VkResult (*vkCreateInstance)(const VkInstanceCreateInfo *, const void *, VkInstance *);
    VkResult (*vkEnumeratePhysicalDevices)(VkInstance, uint32_t *, VkPhysicalDevice *);
    void (*vkGetPhysicalDeviceProperties)(VkPhysicalDevice, void *);
    void (*vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *);
    void (*vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
    VkResult (*vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo *, const void *, VkDevice *);
    void (*vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue *);
    VkResult (*vkCreateBuffer)(VkDevice, const VkBufferCreateInfo *, const void *, VkBuffer *);
    void (*vkGetBufferMemoryRequirements)(VkDevice, VkBuffer, VkMemoryRequirements *);
    VkResult (*vkAllocateMemory)(VkDevice, const VkMemoryAllocateInfo *, const void *, VkDeviceMemory *);
    VkResult (*vkBindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
    VkResult (*vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, uint32_t, void **);
    void (*vkUnmapMemory)(VkDevice, VkDeviceMemory);
    VkResult (*vkCreateDescriptorSetLayout)(VkDevice, const VkDescriptorSetLayoutCreateInfo *, const void *, VkDescriptorSetLayout *);
    VkResult (*vkCreatePipelineLayout)(VkDevice, const VkPipelineLayoutCreateInfo *, const void *, VkPipelineLayout *);
    VkResult (*vkCreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo *, const void *, VkShaderModule *);
    VkResult (*vkCreateComputePipelines)(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *, const void *, VkPipeline *);
    VkResult (*vkCreateDescriptorPool)(VkDevice, const VkDescriptorPoolCreateInfo *, const void *, VkDescriptorPool *);
    VkResult (*vkAllocateDescriptorSets)(VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *);
    void (*vkUpdateDescriptorSets)(VkDevice, uint32_t, const VkWriteDescriptorSet *, uint32_t, const void *);
    VkResult (*vkCreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo *, const void *, VkCommandPool *);
    VkResult (*vkAllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
    VkResult (*vkBeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo *);
    void (*vkCmdBindPipeline)(VkCommandBuffer, uint32_t, VkPipeline);
    void (*vkCmdBindDescriptorSets)(VkCommandBuffer, uint32_t, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet *, uint32_t, const uint32_t *);
    void (*vkCmdDispatch)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
    VkResult (*vkEndCommandBuffer)(VkCommandBuffer);
    VkResult (*vkCreateFence)(VkDevice, const VkFenceCreateInfo *, const void *, VkFence *);
    VkResult (*vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
    VkResult (*vkWaitForFences)(VkDevice, uint32_t, const VkFence *, uint32_t, uint64_t);

    VKSYM(vkCreateInstance, "vkCreateInstance");
    VKSYM(vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    VKSYM(vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    VKSYM(vkGetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    VKSYM(vkGetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    VKSYM(vkCreateDevice, "vkCreateDevice");
    VKSYM(vkGetDeviceQueue, "vkGetDeviceQueue");
    VKSYM(vkCreateBuffer, "vkCreateBuffer");
    VKSYM(vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    VKSYM(vkAllocateMemory, "vkAllocateMemory");
    VKSYM(vkBindBufferMemory, "vkBindBufferMemory");
    VKSYM(vkMapMemory, "vkMapMemory");
    VKSYM(vkUnmapMemory, "vkUnmapMemory");
    VKSYM(vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
    VKSYM(vkCreatePipelineLayout, "vkCreatePipelineLayout");
    VKSYM(vkCreateShaderModule, "vkCreateShaderModule");
    VKSYM(vkCreateComputePipelines, "vkCreateComputePipelines");
    VKSYM(vkCreateDescriptorPool, "vkCreateDescriptorPool");
    VKSYM(vkAllocateDescriptorSets, "vkAllocateDescriptorSets");
    VKSYM(vkUpdateDescriptorSets, "vkUpdateDescriptorSets");
    VKSYM(vkCreateCommandPool, "vkCreateCommandPool");
    VKSYM(vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    VKSYM(vkBeginCommandBuffer, "vkBeginCommandBuffer");
    VKSYM(vkCmdBindPipeline, "vkCmdBindPipeline");
    VKSYM(vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    VKSYM(vkCmdDispatch, "vkCmdDispatch");
    VKSYM(vkEndCommandBuffer, "vkEndCommandBuffer");
    VKSYM(vkCreateFence, "vkCreateFence");
    VKSYM(vkQueueSubmit, "vkQueueSubmit");
    VKSYM(vkWaitForFences, "vkWaitForFences");

    if (!vkCreateInstance || !vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties) {
        emit("vk_loader", "FAIL", "libvulkan.so.1 loaded but core symbols missing");
        finish("loader missing symbols");
        return 1;
    }
    {
        Dl_info di;
        if (dladdr((void *)vkCreateInstance, &di) && di.dli_fname)
            emit("vk_loader", "PASS", "%s", di.dli_fname);
        else
            emit("vk_loader", "PASS", "libvulkan.so.1 loaded");
    }

    VkApplicationInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = ST_APP_INFO; ai.pApplicationName = "nvkvm-validate";
    ai.pEngineName = "nvkvm-validate"; ai.apiVersion = (1u << 22) | (0u << 12) | 0u;

    VkInstanceCreateInfo ici; memset(&ici, 0, sizeof ici);
    ici.sType = ST_INSTANCE_CI; ici.pApplicationInfo = &ai;

    VkInstance inst = NULL;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) {
        emit("vk_instance", "FAIL", "vkCreateInstance rc=%d (no usable ICD? check /usr/share/vulkan/icd.d/)", r);
        finish("no Vulkan instance");
        return 1;
    }
    emit("vk_instance", "PASS", "vkCreateInstance rc=0");

    uint32_t ndev = 0;
    r = vkEnumeratePhysicalDevices(inst, &ndev, NULL);
    if (r != VK_SUCCESS || ndev == 0) {
        emit("vk_physical_device", "FAIL", "vkEnumeratePhysicalDevices rc=%d count=%u", r, ndev);
        finish("no Vulkan physical device");
        return 1;
    }
    VkPhysicalDevice *pds = calloc(ndev, sizeof *pds);
    vkEnumeratePhysicalDevices(inst, &ndev, pds);

    /* VkPhysicalDeviceProperties: apiVersion(0) driverVersion(4) vendorID(8)
       deviceID(12) deviceType(16) deviceName[256](20). Over-allocate so we
       never depend on the size of the trailing limits/sparse members. */
    unsigned char props[4096];
    int chosen = -1; char chosen_name[256] = {0}; uint32_t chosen_vendor = 0;
    char all_names[768] = {0};

    uint32_t i;
    for (i = 0; i < ndev; i++) {
        memset(props, 0, sizeof props);
        vkGetPhysicalDeviceProperties(pds[i], props);
        uint32_t vendor; memcpy(&vendor, props + 8, 4);
        const char *nm = (const char *)(props + 20);
        if (all_names[0]) strncat(all_names, ", ", sizeof all_names - strlen(all_names) - 1);
        strncat(all_names, nm, sizeof all_names - strlen(all_names) - 1);
        if (chosen < 0 || (vendor == 0x10DE && chosen_vendor != 0x10DE)) {
            chosen = (int)i; chosen_vendor = vendor;
            snprintf(chosen_name, sizeof chosen_name, "%s", nm);
        }
    }
    emit("vk_physical_device", "PASS", "%u device(s): %s", ndev, all_names);

    /* software rasteriser check -- a fallback must FAIL, never pass */
    char lower[256]; size_t k;
    for (k = 0; k < sizeof lower - 1 && chosen_name[k]; k++) lower[k] = (char)tolower((unsigned char)chosen_name[k]);
    lower[k] = 0;
    int is_sw = strstr(lower, "llvmpipe") || strstr(lower, "lavapipe") ||
                strstr(lower, "swiftshader") || strstr(lower, "software") || strstr(lower, "swrast");
    if (is_sw) {
        emit("vk_device_is_nvidia", "FAIL",
             "SOFTWARE RASTERISER: '%s' (vendorID 0x%04X) -- NVIDIA ICD not reachable",
             chosen_name, chosen_vendor);
        finish("no hardware Vulkan device");
        return 1;
    }
    if (chosen_vendor != 0x10DE) {
        emit("vk_device_is_nvidia", "FAIL", "vendorID 0x%04X != 0x10DE (NVIDIA); device '%s'",
             chosen_vendor, chosen_name);
        finish("chosen Vulkan device is not NVIDIA");
        return 1;
    }
    emit("vk_device_is_nvidia", "PASS", "%s (vendorID 0x%04X)", chosen_name, chosen_vendor);

    VkPhysicalDevice pd = pds[chosen];

    /* ---- compute dispatch ------------------------------------------------ */
    if (!vkCreateDevice || !vkCreateComputePipelines || !vkQueueSubmit) {
        emit("vk_compute_dispatch", "SKIP", "loader is missing compute entry points");
        finish("incomplete loader"); return 1;
    }

    FILE *f = fopen(spv_path, "rb");
    if (!f) { emit("vk_compute_dispatch", "SKIP", "cannot open SPIR-V '%s'", spv_path); finish("no spirv"); return 1; }
    fseek(f, 0, SEEK_END); long spvlen = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *spv = malloc((size_t)spvlen);
    if (fread(spv, 1, (size_t)spvlen, f) != (size_t)spvlen) {
        emit("vk_compute_dispatch", "SKIP", "short read on SPIR-V"); fclose(f); finish("bad spirv"); return 1;
    }
    fclose(f);
    if (spvlen < 4 || spv[0] != 0x07230203u) {
        emit("vk_compute_dispatch", "FAIL", "SPIR-V magic is 0x%08X, expected 0x07230203", spvlen >= 4 ? spv[0] : 0);
        finish("bad spirv"); return 1;
    }

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
    VkQueueFamilyProperties *qf = calloc(nqf, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qf);
    int qfi = -1;
    for (i = 0; i < nqf; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = (int)i; break; }
    if (qfi < 0) { emit("vk_compute_dispatch", "FAIL", "no compute-capable queue family among %u", nqf); finish("no compute queue"); return 1; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci; memset(&qci, 0, sizeof qci);
    qci.sType = ST_DEVQUEUE_CI; qci.queueFamilyIndex = (uint32_t)qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci; memset(&dci, 0, sizeof dci);
    dci.sType = ST_DEVICE_CI; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;

    VkDevice dev = NULL;
    r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkCreateDevice rc=%d", r); finish("no device"); return 1; }
    VkQueue q = NULL; vkGetDeviceQueue(dev, (uint32_t)qfi, 0, &q);

    const uint32_t N = 4096;
    VkDeviceSize bytes = (VkDeviceSize)N * 4;
    VkBufferCreateInfo bci; memset(&bci, 0, sizeof bci);
    bci.sType = ST_BUFFER_CI; bci.size = bytes; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buf = 0;
    r = vkCreateBuffer(dev, &bci, NULL, &buf);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkCreateBuffer rc=%d", r); finish("x"); return 1; }

    VkMemoryRequirements mr; memset(&mr, 0, sizeof mr);
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; memset(&mp, 0, sizeof mp);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    int mti = -1;
    for (i = 0; i < mp.memoryTypeCount; i++) {
        uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { mti = (int)i; break; }
    }
    if (mti < 0) { emit("vk_compute_dispatch", "FAIL", "no HOST_VISIBLE|HOST_COHERENT memory type (typeBits=0x%X, %u types)", mr.memoryTypeBits, mp.memoryTypeCount); finish("x"); return 1; }

    VkMemoryAllocateInfo mai; memset(&mai, 0, sizeof mai);
    mai.sType = ST_MEMALLOC_INFO; mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)mti;
    VkDeviceMemory mem = 0;
    r = vkAllocateMemory(dev, &mai, NULL, &mem);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkAllocateMemory rc=%d", r); finish("x"); return 1; }
    r = vkBindBufferMemory(dev, buf, mem, 0);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkBindBufferMemory rc=%d", r); finish("x"); return 1; }

    uint32_t *mapped = NULL;
    r = vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkMapMemory rc=%d", r); finish("x"); return 1; }
    for (i = 0; i < N; i++) mapped[i] = i;

    VkDescriptorSetLayoutBinding b; memset(&b, 0, sizeof b);
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslci; memset(&dslci, 0, sizeof dslci);
    dslci.sType = ST_DSL_CI; dslci.bindingCount = 1; dslci.pBindings = &b;
    VkDescriptorSetLayout dsl = 0;
    r = vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkCreateDescriptorSetLayout rc=%d", r); finish("x"); return 1; }

    VkPipelineLayoutCreateInfo plci; memset(&plci, 0, sizeof plci);
    plci.sType = ST_PIPELAYOUT_CI; plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
    VkPipelineLayout pl = 0;
    r = vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkCreatePipelineLayout rc=%d", r); finish("x"); return 1; }

    VkShaderModuleCreateInfo smci; memset(&smci, 0, sizeof smci);
    smci.sType = ST_SHADERMOD_CI; smci.codeSize = (size_t)spvlen; smci.pCode = spv;
    VkShaderModule sm = 0;
    r = vkCreateShaderModule(dev, &smci, NULL, &sm);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkCreateShaderModule rc=%d", r); finish("x"); return 1; }

    VkComputePipelineCreateInfo cpci; memset(&cpci, 0, sizeof cpci);
    cpci.sType = ST_COMPUTE_PIPE_CI;
    cpci.stage.sType = ST_PSSCI; cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = sm; cpci.stage.pName = "main";
    cpci.layout = pl;
    VkPipeline pipe = 0;
    r = vkCreateComputePipelines(dev, 0, 1, &cpci, NULL, &pipe);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkCreateComputePipelines rc=%d", r); finish("x"); return 1; }

    VkDescriptorPoolSize ps; ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps.descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpci; memset(&dpci, 0, sizeof dpci);
    dpci.sType = ST_DPOOL_CI; dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &ps;
    VkDescriptorPool dp = 0;
    r = vkCreateDescriptorPool(dev, &dpci, NULL, &dp);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkCreateDescriptorPool rc=%d", r); finish("x"); return 1; }

    VkDescriptorSetAllocateInfo dsai; memset(&dsai, 0, sizeof dsai);
    dsai.sType = ST_DSET_ALLOC; dsai.descriptorPool = dp; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet ds = 0;
    r = vkAllocateDescriptorSets(dev, &dsai, &ds);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkAllocateDescriptorSets rc=%d", r); finish("x"); return 1; }

    VkDescriptorBufferInfo dbi; dbi.buffer = buf; dbi.offset = 0; dbi.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet w; memset(&w, 0, sizeof w);
    w.sType = ST_WRITE_DSET; w.dstSet = ds; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    VkCommandPoolCreateInfo cpci2; memset(&cpci2, 0, sizeof cpci2);
    cpci2.sType = ST_CMDPOOL_CI; cpci2.queueFamilyIndex = (uint32_t)qfi;
    VkCommandPool cp = 0;
    r = vkCreateCommandPool(dev, &cpci2, NULL, &cp);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkCreateCommandPool rc=%d", r); finish("x"); return 1; }

    VkCommandBufferAllocateInfo cbai; memset(&cbai, 0, sizeof cbai);
    cbai.sType = ST_CMDBUF_ALLOC; cbai.commandPool = cp; cbai.level = 0; cbai.commandBufferCount = 1;
    VkCommandBuffer cb = NULL;
    r = vkAllocateCommandBuffers(dev, &cbai, &cb);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkAllocateCommandBuffers rc=%d", r); finish("x"); return 1; }

    VkCommandBufferBeginInfo cbbi; memset(&cbbi, 0, sizeof cbbi);
    cbbi.sType = ST_CMDBUF_BEGIN; cbbi.flags = 1;
    vkBeginCommandBuffer(cb, &cbbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, N / 64, 1, 1);
    vkEndCommandBuffer(cb);

    VkFenceCreateInfo fci; memset(&fci, 0, sizeof fci); fci.sType = ST_FENCE_CI;
    VkFence fence = 0;
    r = vkCreateFence(dev, &fci, NULL, &fence);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkCreateFence rc=%d", r); finish("x"); return 1; }

    VkSubmitInfo si; memset(&si, 0, sizeof si);
    si.sType = ST_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    r = vkQueueSubmit(q, 1, &si, fence);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkQueueSubmit rc=%d", r); finish("x"); return 1; }
    r = vkWaitForFences(dev, 1, &fence, 1, 5000000000ULL);
    if (r != VK_SUCCESS) { emit("vk_compute_dispatch", "FAIL", "vkWaitForFences rc=%d (timeout/lost device)", r); finish("x"); return 1; }

    /* verify EVERY element: data[i] must be i*3+7 */
    uint32_t bad = 0, firsti = 0, firstgot = 0;
    for (i = 0; i < N; i++) {
        uint32_t want = i * 3u + 7u;
        if (mapped[i] != want) { if (!bad) { firsti = i; firstgot = mapped[i]; } bad++; }
    }
    if (bad) emit("vk_compute_dispatch", "FAIL",
                  "%u/%u elements wrong; first i=%u expected %u got %u",
                  bad, N, firsti, firsti * 3u + 7u, firstgot);
    else     emit("vk_compute_dispatch", "PASS",
                  "%u elements, data[i]=i*3+7 verified on %s (e.g. data[%u]=%u)",
                  N, chosen_name, N - 1, mapped[N - 1]);

    vkUnmapMemory(dev, mem);
    finish("not reached");
    return 0;
}

int main(int argc, char **argv) { int rc = probe_main(argc, argv); fflush(NULL); _exit(rc); }
NVKVM_VK_EOF

if [ -n "$BUILD_PROBES" ]; then
    emit_probe vk_probe vk_probe.c -ldl
else
    provide_probe vk_probe vk_probe.c -ldl; PROBE_RC=$?
fi
if [ -n "$BUILD_PROBES" ]; then
    :
elif [ "$PROBE_RC" = 127 ]; then
    untested_remaining "$NO_PROBE_REASON" $VK_CHECKS
elif [ "$SPV_OK" != 1 ]; then
    skip_remaining "$SPV_ERR" $VK_CHECKS
elif [ "$PROBE_RC" != 0 ]; then
    skip_remaining "vk_probe.c failed to compile (see compiler output above)" $VK_CHECKS
else
    printf ' %svk_probe: %s%s\n' "$C_DIM" "$PROBE_NOTE" "$C_RESET"
    run_probe vk_probe "$WORK/comp.spv"
    VK_RC=$?
    ingest "$WORK/vk_probe.out"
    note_probe_timeout
    if [ $VK_RC -ge 128 ]; then
        skip_remaining "vk_probe died on signal $((VK_RC-128)); tail: $(tail -2 "$WORK/vk_probe.out" | tr '\n' ' ')" $VK_CHECKS
    else
        skip_remaining "vk_probe exited $VK_RC without reporting this check" $VK_CHECKS
    fi
fi

# =============================================================================
# PHASE 4 -- OpenGL via offscreen EGL
# =============================================================================
section "4. OpenGL (offscreen EGL, no X/Wayland)"

GL_CHECKS="egl_loader egl_context gl_renderer gl_renderer_is_nvidia gl_draw_pixel_check"

cat > "$WORK/gl_probe.c" <<'NVKVM_GL_EOF'
/*
 * nvkvm validation -- offscreen EGL / OpenGL ES probe.
 *
 * Uses EGL_EXT_platform_device so it needs no X server and no Wayland
 * compositor -- it binds the NVIDIA EGL device directly. dlopen()s
 * libEGL.so.1 and libGLESv2.so.2 and hand-rolls the types, so no EGL/GLES
 * headers are needed to build.
 *
 * NOTE ON THE PIXEL CHECK. It is not enough to render and observe "some
 * non-black pixels" -- this project has already been burned by a checker that
 * reported "100% non-black" while reading a compositor's own background. So
 * this probe renders into an FBO IT ALLOCATED, clears it to an exact colour,
 * draws a triangle covering only the lower-left half in a DIFFERENT exact
 * colour, and then asserts BOTH:
 *     a pixel inside the triangle  == the triangle colour
 *     a pixel outside the triangle == the clear colour
 * Reading anyone else's framebuffer cannot satisfy both.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <dlfcn.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext, *EGLDeviceEXT;
typedef unsigned int EGLenum, EGLBoolean;
typedef int EGLint;

#define EGL_NO_DISPLAY      ((EGLDisplay)0)
#define EGL_NO_CONTEXT      ((EGLContext)0)
#define EGL_NO_SURFACE      ((EGLSurface)0)
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#define EGL_OPENGL_ES_API   0x30A0
#define EGL_NONE            0x3038
#define EGL_SURFACE_TYPE    0x3033
#define EGL_PBUFFER_BIT     0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT  0x0004
#define EGL_RED_SIZE        0x3024
#define EGL_GREEN_SIZE      0x3023
#define EGL_BLUE_SIZE       0x3022
#define EGL_ALPHA_SIZE      0x3021
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WIDTH           0x3057
#define EGL_HEIGHT          0x3056

#define GL_RENDERER   0x1F01
#define GL_VENDOR     0x1F00
#define GL_VERSION    0x1F02
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_FRAMEBUFFER      0x8D40
#define GL_RENDERBUFFER     0x8D41
#define GL_RGBA8            0x8058
#define GL_TEXTURE_2D       0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST          0x2600
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA             0x1908
#define GL_UNSIGNED_BYTE    0x1401
#define GL_VERTEX_SHADER    0x8B31
#define GL_FRAGMENT_SHADER  0x8B30
#define GL_COMPILE_STATUS   0x8B81
#define GL_LINK_STATUS      0x8B82
#define GL_FLOAT            0x1406
#define GL_TRIANGLES        0x0004
#define GL_NO_ERROR         0
#define GL_FALSE            0

static const char *ALL_CHECKS[] = { "egl_loader", "egl_context", "gl_renderer",
                                    "gl_renderer_is_nvidia", "gl_draw_pixel_check", NULL };
static int reported[16];
static void emit(const char *name, const char *status, const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    int i; for (i = 0; ALL_CHECKS[i]; i++) if (!strcmp(ALL_CHECKS[i], name)) reported[i] = 1;
    printf("CHECK|%s|%s|%s\n", name, status, buf); fflush(stdout);
}
static void finish(const char *reason) {
    int i; for (i = 0; ALL_CHECKS[i]; i++) if (!reported[i]) printf("CHECK|%s|SKIP|%s\n", ALL_CHECKS[i], reason);
    fflush(stdout);
}

/* see the note in the CUDA probe: body + _exit(), never a normal return */
static int probe_main(void) {
    void *E = dlopen("libEGL.so.1", RTLD_NOW);
    if (!E) E = dlopen("libEGL.so", RTLD_NOW);
    if (!E) { emit("egl_loader", "SKIP", "dlopen(libEGL.so.1): %s -- install libegl1 in the guest", dlerror());
              finish("no EGL loader"); return 0; }
    void *G = dlopen("libGLESv2.so.2", RTLD_NOW);
    if (!G) G = dlopen("libGLESv2.so", RTLD_NOW);
    if (!G) { emit("egl_loader", "SKIP", "dlopen(libGLESv2.so.2): %s -- install libgles2 in the guest", dlerror());
              finish("no GLESv2"); return 0; }

    void *(*eglGetProcAddress)(const char *) = dlsym(E, "eglGetProcAddress");
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *) = dlsym(E, "eglInitialize");
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = dlsym(E, "eglChooseConfig");
    EGLBoolean (*eglBindAPI)(EGLenum) = dlsym(E, "eglBindAPI");
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = dlsym(E, "eglCreateContext");
    EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *) = dlsym(E, "eglCreatePbufferSurface");
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = dlsym(E, "eglMakeCurrent");
    EGLint (*eglGetError)(void) = dlsym(E, "eglGetError");
    EGLDisplay (*eglGetDisplay)(void *) = dlsym(E, "eglGetDisplay");

    if (!eglInitialize || !eglChooseConfig || !eglCreateContext || !eglMakeCurrent) {
        emit("egl_loader", "FAIL", "libEGL.so.1 loaded but core symbols missing");
        finish("incomplete EGL"); return 1;
    }
    { Dl_info di;
      if (dladdr((void *)eglInitialize, &di) && di.dli_fname) emit("egl_loader", "PASS", "%s", di.dli_fname);
      else emit("egl_loader", "PASS", "libEGL.so.1 loaded"); }

    /* Prefer EGL_EXT_platform_device: binds the NVIDIA GPU with no display
       server at all. Fall back to eglGetDisplay(NULL) only if unavailable. */
    EGLBoolean (*eglQueryDevicesEXT)(EGLint, EGLDeviceEXT *, EGLint *) =
        eglGetProcAddress ? eglGetProcAddress("eglQueryDevicesEXT") : NULL;
    EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *) =
        eglGetProcAddress ? eglGetProcAddress("eglGetPlatformDisplayEXT") : NULL;

    EGLDisplay dpy = EGL_NO_DISPLAY;
    char how[128] = "";
    if (eglQueryDevicesEXT && eglGetPlatformDisplayEXT) {
        EGLDeviceEXT devs[16]; EGLint nd = 0;
        if (eglQueryDevicesEXT(16, devs, &nd) && nd > 0) {
            EGLint i;
            for (i = 0; i < nd; i++) {
                EGLDisplay d = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL);
                if (d != EGL_NO_DISPLAY) {
                    EGLint maj = 0, min = 0;
                    if (eglInitialize(d, &maj, &min)) {
                        dpy = d; snprintf(how, sizeof how, "EGL_EXT_platform_device dev %d/%d, EGL %d.%d", i, nd, maj, min);
                        break;
                    }
                }
            }
        }
    }
    if (dpy == EGL_NO_DISPLAY && eglGetDisplay) {
        EGLDisplay d = eglGetDisplay(NULL);
        EGLint maj = 0, min = 0;
        if (d != EGL_NO_DISPLAY && eglInitialize(d, &maj, &min)) {
            dpy = d; snprintf(how, sizeof how, "eglGetDisplay(EGL_DEFAULT_DISPLAY), EGL %d.%d", maj, min);
        }
    }
    if (dpy == EGL_NO_DISPLAY) {
        emit("egl_context", "FAIL", "no EGL display could be initialised (eglGetError=0x%X)",
             eglGetError ? eglGetError() : 0);
        finish("no EGL display"); return 1;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfgattr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                         EGL_NONE };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfgattr, &cfg, 1, &ncfg) || ncfg < 1) {
        emit("egl_context", "FAIL", "eglChooseConfig returned %d configs (eglGetError=0x%X)",
             ncfg, eglGetError ? eglGetError() : 0);
        finish("no EGL config"); return 1;
    }
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
    if (ctx == EGL_NO_CONTEXT) {
        emit("egl_context", "FAIL", "eglCreateContext failed (eglGetError=0x%X)", eglGetError ? eglGetError() : 0);
        finish("no EGL context"); return 1;
    }
    EGLint pbattr[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface ? eglCreatePbufferSurface(dpy, cfg, pbattr) : EGL_NO_SURFACE;
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        /* surfaceless is fine too if the implementation supports it */
        if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            emit("egl_context", "FAIL", "eglMakeCurrent failed (eglGetError=0x%X)", eglGetError ? eglGetError() : 0);
            finish("no current context"); return 1;
        }
        snprintf(how + strlen(how), sizeof how - strlen(how), ", surfaceless");
    }
    emit("egl_context", "PASS", "%s", how);

    const unsigned char *(*glGetString)(unsigned) = dlsym(G, "glGetString");
    if (!glGetString) { emit("gl_renderer", "FAIL", "glGetString not found in libGLESv2"); finish("x"); return 1; }
    const char *rend = (const char *)glGetString(GL_RENDERER);
    const char *vend = (const char *)glGetString(GL_VENDOR);
    const char *ver  = (const char *)glGetString(GL_VERSION);
    if (!rend || !*rend) { emit("gl_renderer", "FAIL", "GL_RENDERER is empty"); finish("x"); return 1; }
    emit("gl_renderer", "PASS", "GL_RENDERER='%s' GL_VENDOR='%s' GL_VERSION='%s'",
         rend, vend ? vend : "?", ver ? ver : "?");

    char lower[512]; size_t k;
    for (k = 0; k < sizeof lower - 1 && rend[k]; k++) lower[k] = (char)tolower((unsigned char)rend[k]);
    lower[k] = 0;
    if (strstr(lower, "llvmpipe") || strstr(lower, "softpipe") || strstr(lower, "swrast") ||
        strstr(lower, "software rasterizer")) {
        emit("gl_renderer_is_nvidia", "FAIL", "SOFTWARE RASTERISER: GL_RENDERER='%s'", rend);
        finish("software GL"); return 1;
    }
    /*
     * GL_RENDERER is not a vendor field.  Consumer parts spell it
     * "NVIDIA GeForce RTX 4070/PCIe/SSE2", but DATACENTER parts drop the
     * prefix entirely -- a Tesla T4 reports "Tesla T4/PCIe/SSE2" while
     * GL_VENDOR still reads "NVIDIA Corporation" and GL_VERSION still reads
     * "OpenGL ES 3.2 NVIDIA 580.178.04".  Matching on the renderer alone
     * therefore failed every Tesla/A-series/H-series card as "non-NVIDIA",
     * and took gl_draw_pixel_check down with it as an unexpected SKIP.
     * Measured on 6x Tesla T4 / 580.178.04, 2026-08-22.
     *
     * GL_VENDOR is the field that actually names the vendor; the software
     * rasteriser test above stays on GL_RENDERER, which is where llvmpipe
     * and friends identify themselves.
     */
    char vlower[512]; size_t vk_;
    const char *vsrc = vend ? vend : "";
    for (vk_ = 0; vk_ < sizeof vlower - 1 && vsrc[vk_]; vk_++)
        vlower[vk_] = (char)tolower((unsigned char)vsrc[vk_]);
    vlower[vk_] = 0;
    if (!strstr(lower, "nvidia") && !strstr(vlower, "nvidia")) {
        emit("gl_renderer_is_nvidia", "FAIL",
             "neither GL_RENDERER='%s' nor GL_VENDOR='%s' names NVIDIA",
             rend, vend ? vend : "?");
        finish("non-NVIDIA GL"); return 1;
    }
    emit("gl_renderer_is_nvidia", "PASS", "%s", rend);

    /* ---- render + exact pixel check ------------------------------------- */
    void (*glGenFramebuffers)(int, unsigned *) = dlsym(G, "glGenFramebuffers");
    void (*glBindFramebuffer)(unsigned, unsigned) = dlsym(G, "glBindFramebuffer");
    void (*glGenTextures)(int, unsigned *) = dlsym(G, "glGenTextures");
    void (*glBindTexture)(unsigned, unsigned) = dlsym(G, "glBindTexture");
    void (*glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) = dlsym(G, "glTexImage2D");
    void (*glTexParameteri)(unsigned, unsigned, int) = dlsym(G, "glTexParameteri");
    void (*glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int) = dlsym(G, "glFramebufferTexture2D");
    unsigned (*glCheckFramebufferStatus)(unsigned) = dlsym(G, "glCheckFramebufferStatus");
    void (*glViewport)(int, int, int, int) = dlsym(G, "glViewport");
    void (*glClearColor)(float, float, float, float) = dlsym(G, "glClearColor");
    void (*glClear)(unsigned) = dlsym(G, "glClear");
    unsigned (*glCreateShader)(unsigned) = dlsym(G, "glCreateShader");
    void (*glShaderSource)(unsigned, int, const char *const *, const int *) = dlsym(G, "glShaderSource");
    void (*glCompileShader)(unsigned) = dlsym(G, "glCompileShader");
    void (*glGetShaderiv)(unsigned, unsigned, int *) = dlsym(G, "glGetShaderiv");
    void (*glGetShaderInfoLog)(unsigned, int, int *, char *) = dlsym(G, "glGetShaderInfoLog");
    unsigned (*glCreateProgram)(void) = dlsym(G, "glCreateProgram");
    void (*glAttachShader)(unsigned, unsigned) = dlsym(G, "glAttachShader");
    void (*glLinkProgram)(unsigned) = dlsym(G, "glLinkProgram");
    void (*glGetProgramiv)(unsigned, unsigned, int *) = dlsym(G, "glGetProgramiv");
    void (*glUseProgram)(unsigned) = dlsym(G, "glUseProgram");
    int  (*glGetAttribLocation)(unsigned, const char *) = dlsym(G, "glGetAttribLocation");
    void (*glEnableVertexAttribArray)(unsigned) = dlsym(G, "glEnableVertexAttribArray");
    void (*glVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *) = dlsym(G, "glVertexAttribPointer");
    void (*glDrawArrays)(unsigned, int, int) = dlsym(G, "glDrawArrays");
    void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void *) = dlsym(G, "glReadPixels");
    void (*glFinish)(void) = dlsym(G, "glFinish");
    unsigned (*glGetError)(void) = dlsym(G, "glGetError");

    if (!glGenFramebuffers || !glDrawArrays || !glReadPixels || !glCreateShader ||
        !glGenTextures || !glTexImage2D || !glFramebufferTexture2D) {
        emit("gl_draw_pixel_check", "SKIP", "libGLESv2 is missing FBO/shader entry points");
        finish("incomplete GLES2"); return 1;
    }

    /* Colour attachment is a TEXTURE, not a renderbuffer: GL_RGBA8
     * renderbuffer storage needs OES_rgb8_rgba8 and is NOT core GLES2, and it
     * really is absent on some stacks -- driver 610 returned
     * GL_FRAMEBUFFER_UNSUPPORTED (0x8CDD) for exactly that. An
     * RGBA/UNSIGNED_BYTE texture is core GLES2 and always colour-renderable. */
    const int W = 64, Hh = 64;
    unsigned fbo = 0, tex = 0;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, Hh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    unsigned fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbs != GL_FRAMEBUFFER_COMPLETE) {
        emit("gl_draw_pixel_check", "FAIL", "FBO incomplete: status=0x%X", fbs);
        finish("x"); return 1;
    }

    static const char *VS = "attribute vec2 pos;\nvoid main(){ gl_Position = vec4(pos,0.0,1.0); }\n";
    static const char *FS = "precision highp float;\nvoid main(){ gl_FragColor = vec4(1.0,0.5,0.0,1.0); }\n";
    unsigned vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    int ok = 0; char log[512];
    glShaderSource(vs, 1, &VS, NULL); glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(vs, sizeof log, NULL, log);
               emit("gl_draw_pixel_check", "FAIL", "vertex shader compile failed: %s", log); finish("x"); return 1; }
    glShaderSource(fs, 1, &FS, NULL); glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(fs, sizeof log, NULL, log);
               emit("gl_draw_pixel_check", "FAIL", "fragment shader compile failed: %s", log); finish("x"); return 1; }
    unsigned prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { emit("gl_draw_pixel_check", "FAIL", "program link failed"); finish("x"); return 1; }
    glUseProgram(prog);

    glViewport(0, 0, W, Hh);
    /* clear colour: exact 0,0,0,255 */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* triangle covering the LOWER-LEFT half only */
    static const float verts[] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f };
    int loc = glGetAttribLocation(prog, "pos");
    if (loc < 0) { emit("gl_draw_pixel_check", "FAIL", "attribute 'pos' not found"); finish("x"); return 1; }
    glEnableVertexAttribArray((unsigned)loc);
    glVertexAttribPointer((unsigned)loc, 2, GL_FLOAT, 0, 0, verts);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();

    unsigned gerr = glGetError();
    if (gerr != GL_NO_ERROR) {
        emit("gl_draw_pixel_check", "FAIL", "glGetError=0x%X after draw", gerr);
        finish("x"); return 1;
    }

    unsigned char *px = malloc((size_t)W * Hh * 4);
    glReadPixels(0, 0, W, Hh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    gerr = glGetError();
    if (gerr != GL_NO_ERROR) {
        emit("gl_draw_pixel_check", "FAIL", "glGetError=0x%X after glReadPixels", gerr);
        finish("x"); return 1;
    }

    /* inside the triangle (lower-left quadrant) and outside it (upper-right) */
    int ix = W / 4,      iy = Hh / 4;
    int ox = (3 * W) / 4, oy = (3 * Hh) / 4;
    unsigned char *pin  = px + ((size_t)iy * W + ix) * 4;
    unsigned char *pout = px + ((size_t)oy * W + ox) * 4;

    /* shader writes (1.0, 0.5, 0.0, 1.0) -> (255, 127|128, 0, 255) */
    int in_ok  = (pin[0] >= 253) && (pin[1] >= 125 && pin[1] <= 130) && (pin[2] <= 2) && (pin[3] >= 253);
    int out_ok = (pout[0] <= 2) && (pout[1] <= 2) && (pout[2] <= 2) && (pout[3] >= 253);

    if (in_ok && out_ok) {
        emit("gl_draw_pixel_check", "PASS",
             "64x64 FBO: inside(%d,%d)=RGBA(%u,%u,%u,%u)==triangle, outside(%d,%d)=RGBA(%u,%u,%u,%u)==clear",
             ix, iy, pin[0], pin[1], pin[2], pin[3], ox, oy, pout[0], pout[1], pout[2], pout[3]);
    } else {
        emit("gl_draw_pixel_check", "FAIL",
             "inside(%d,%d)=RGBA(%u,%u,%u,%u) expected ~(255,128,0,255) [%s]; "
             "outside(%d,%d)=RGBA(%u,%u,%u,%u) expected (0,0,0,255) [%s]",
             ix, iy, pin[0], pin[1], pin[2], pin[3], in_ok ? "ok" : "BAD",
             ox, oy, pout[0], pout[1], pout[2], pout[3], out_ok ? "ok" : "BAD");
    }

    finish("not reached");
    return 0;
}

int main(void) { int rc = probe_main(); fflush(NULL); _exit(rc); }
NVKVM_GL_EOF

if [ -n "$BUILD_PROBES" ]; then
    emit_probe gl_probe gl_probe.c -ldl
else
    provide_probe gl_probe gl_probe.c -ldl; PROBE_RC=$?
fi
if [ -n "$BUILD_PROBES" ]; then
    printf '\n%d probe(s) written to %s\n' "$BUILD_PROBES_DONE" "$BUILD_PROBES"
    printf 'Point a later run at them with --probe-dir %s, or install them at one of:\n  %s\n' \
           "$BUILD_PROBES" "$DEFAULT_PROBE_DIRS"
    exit 0
elif [ "$PROBE_RC" = 127 ]; then
    untested_remaining "$NO_PROBE_REASON" $GL_CHECKS
elif [ "$PROBE_RC" != 0 ]; then
    skip_remaining "gl_probe.c failed to compile (see compiler output above)" $GL_CHECKS
else
    printf ' %sgl_probe: %s%s\n' "$C_DIM" "$PROBE_NOTE" "$C_RESET"
    run_probe gl_probe
    GL_RC=$?
    ingest "$WORK/gl_probe.out"
    note_probe_timeout
    if [ $GL_RC -ge 128 ]; then
        skip_remaining "gl_probe died on signal $((GL_RC-128)); tail: $(tail -2 "$WORK/gl_probe.out" | tr '\n' ' ')" $GL_CHECKS
    else
        skip_remaining "gl_probe exited $GL_RC without reporting this check" $GL_CHECKS
    fi
fi

# =============================================================================
# SUMMARY
# =============================================================================

printf '\n%s' "$C_BOLD"
cat <<'EOT'
=============================================================================
 RESULTS
=============================================================================
EOT
printf '%s' "$C_RESET"
printf ' %-4s  %-26s  %s\n' "STAT" "CHECK" "OBSERVED VALUE"
printf ' %-4s  %-26s  %s\n' "----" "--------------------------" "--------------------------------------"

i=0
while [ $i -lt ${#RESULT_NAMES[@]} ]; do
    st="${RESULT_STATUS[$i]}"
    col="$C_SKIP"; [ "$st" = PASS ] && col="$C_PASS"; [ "$st" = FAIL ] && col="$C_FAIL"
    printf ' %s%-4s%s  %-26s  %s\n' "$col" "$st" "$C_RESET" "${RESULT_NAMES[$i]}" "${RESULT_DETAIL[$i]}"
    i=$((i+1))
done

# ---------------------------------------------------------------------------
# host-memory registration invariants
# ---------------------------------------------------------------------------
# Regressions the CUDA checks above structurally cannot see: every one of them
# registers cuMemHostAlloc memory, which is MAP_SHARED. The bugs live on the
# MAP_PRIVATE and genuinely-shared paths.
#
#   * MAP_PRIVATE (ordinary malloc'd heap) registered fine and then SIGSEGV'd
#     on the first write -- migrate_range cleared VM_MAYWRITE and
#     vm_get_page_prot() then yielded a read-only protection. Present on main,
#     invisible to 30/30 and to the app matrix, since GL/Vulkan/NVENC never
#     call cuMemHostRegister.
#   * A range something else already maps must be REFUSED, not relocated:
#     relocating rewrites one VMA and silently desynchronises every other view.
#   * register-then-fork must stay coherent (the worker-pool shape).
#   * COW heap inherited across fork must register, parent's copy intact.
#
# Missing source or no compiler is SKIP, not FAIL, so this stays runnable on a
# bare guest.
section "host-memory registration invariants"

REPRO_DIR="${NVKVM_REPRO_DIR:-$(cd "$(dirname "$0")/repro" 2>/dev/null && pwd)}"

reg_invariant() {
    # reg_invariant <check-name> <source.c> <detail-on-pass>
    local name="$1" src="$2" okdetail="$3"
    if ! command -v gcc >/dev/null 2>&1; then
        record "$name" SKIP "no compiler on this guest"; return
    fi
    if [ -z "$REPRO_DIR" ] || [ ! -f "$REPRO_DIR/$src" ]; then
        record "$name" SKIP "tests/repro/$src not present"; return
    fi
    if ! gcc -O1 -o "$WORK/${name}" "$REPRO_DIR/$src" -lcuda >"$WORK/${name}.cc" 2>&1; then
        record "$name" SKIP "will not build (no libcuda?)"
        return
    fi
    local out rc
    # These touch cuMemHostRegister -- the same class of call a Pascal wedge
    # was measured on -- so they get the same non-blocking bound as the
    # probes/nvidia-smi, not a plain `timeout` that can itself hang.
    # NOT `out="$(sh_bounded ...)"`. Command substitution reads until EOF,
    # and EOF needs EVERY write end of that pipe closed -- including one
    # inherited by a grandchild that outlives the process we bounded. These
    # repros FORK BY DESIGN, so that grandchild is the normal case, not an
    # edge case. Redirect to a file (what run_probe already does) so no
    # pipe exists for an orphan to hold open.
    sh_bounded "$name" 120 "$WORK/${name}" > "$WORK/${name}.out" 2>&1; rc=$?
    out="$(cat "$WORK/${name}.out" 2>/dev/null || true)"
    if note_shell_timeout "$name"; then
        :
    elif [ "$rc" -eq 0 ]; then
        record "$name" PASS "$okdetail"
    else
        record "$name" FAIL "rc=$rc: $(printf '%s' "$out" | grep -iE 'VERDICT|SEGV|REFUSED' | head -1 | cut -c1-72)"
    fi
}

reg_invariant host_register_private_rw private_register_write.c \
    "MAP_PRIVATE and MAP_SHARED both writable after cuMemHostRegister"
reg_invariant host_register_shared_coherent shared_view_desync.c \
    "two views of one shared buffer stay coherent (now by sharing, not refusal)"
reg_invariant host_register_then_fork register_then_fork.c \
    "child sees the parent's writes after register-then-fork"
reg_invariant host_register_cow_fork cow_after_fork.c \
    "child registered inherited COW heap; parent's copy intact"
reg_invariant host_register_two_process_share fork_both_register.c \
    "two processes each register the same MAP_SHARED buffer; both see both writes"

# --- classify skips ----------------------------------------------------------
UNEXPECTED_SKIPS=""
i=0
while [ $i -lt ${#RESULT_NAMES[@]} ]; do
    if [ "${RESULT_STATUS[$i]}" = SKIP ]; then
        n="${RESULT_NAMES[$i]}"
        case ",$ALLOW_SKIP," in
            *,"$n",*) : ;;                          # declared via --allow-skip
            *) UNEXPECTED_SKIPS="$UNEXPECTED_SKIPS $n" ;;
        esac
    fi
    i=$((i+1))
done

TOTAL=${#RESULT_NAMES[@]}
printf '\n %sTOTAL %d   PASS %d   FAIL %d   SKIP %d   UNTESTED %d%s\n' \
       "$C_BOLD" "$TOTAL" "$N_PASS" "$N_FAIL" "$N_SKIP" "$N_UNTESTED" "$C_RESET"

if [ -n "$UNEXPECTED_SKIPS" ]; then
    printf ' %sUNEXPECTED SKIPS:%s%s\n' "$C_SKIP" "$C_RESET" "$UNEXPECTED_SKIPS"
    printf '   (a skipped check is NOT a pass. Declare it with --allow-skip <name> only\n'
    printf '    if you have decided it is out of scope for this run.)\n'
fi
if [ "$N_UNTESTED" -gt 0 ]; then
    printf '\n %s%d of %d checks were never attempted.%s\n' "$C_UNT" "$N_UNTESTED" "$TOTAL" "$C_RESET"
    printf '   This run says NOTHING about them -- not that they work, not that they do\n'
    printf '   not. There was no compiler and no prebuilt probe, so no CUDA, Vulkan, EGL\n'
    printf '   or GL code ran at all. --allow-skip does not apply and cannot silence this.\n'
    printf '   Fix it by building the probes where a toolchain exists and shipping them:\n'
    printf '     bash %s --build-probes /usr/local/lib/nvkvm/probes\n' "$0"
fi

# --- optional JSON -----------------------------------------------------------
if [ -n "$JSON_OUT" ]; then
    {
        printf '{\n'
        printf '  "gpu": "%s",\n' "$SMI_GPU"
        printf '  "driver": "%s",\n' "$SMI_DRV"
        printf '  "cuda": "%s",\n' "$SMI_CUDA"
        printf '  "pass": %d, "fail": %d, "skip": %d, "untested": %d, "total": %d,\n' \
               "$N_PASS" "$N_FAIL" "$N_SKIP" "$N_UNTESTED" "$TOTAL"
        printf '  "checks": [\n'
        i=0
        while [ $i -lt ${#RESULT_NAMES[@]} ]; do
            d="$(printf '%s' "${RESULT_DETAIL[$i]}" | sed 's/\\/\\\\/g; s/"/\\"/g')"
            printf '    {"name": "%s", "status": "%s", "detail": "%s"}' \
                   "${RESULT_NAMES[$i]}" "${RESULT_STATUS[$i]}" "$d"
            i=$((i+1))
            [ $i -lt ${#RESULT_NAMES[@]} ] && printf ','
            printf '\n'
        done
        printf '  ]\n}\n'
    } > "$JSON_OUT"
    printf ' json     : %s\n' "$JSON_OUT"
fi

verdict
exit $?
