#!/usr/bin/env python3
"""
sweep_matrix.py — rent GPU boxes, run tests/validate.sh on each, destroy, tabulate.

WHY THIS EXISTS
    The tested-platforms table is built by hand, one card at a time, which makes
    coverage a function of what someone happened to rent.  That was defensible
    while the working assumption held -- "the forwarded interface is
    per-architecture, not per-die", so one card per architecture is enough.

    On 2026-08-20 that assumption failed.  An A100 (GA100) failed at
    cuCtxCreate while five consumer Ampere dies passed 28/28.  Same
    architecture, different result.  Once same-arch cards can disagree,
    coverage has to be measured rather than inferred, and the only way that is
    affordable is to automate it: KVM-capable offers run $0.04-0.40/hr and a
    box is needed for well under an hour.

    Finding a failure here costs cents.  Finding it via a user's bug report
    costs weeks, and costs a young project the "experimental" label it is
    trying to shed.

WHAT IT DOES, per offer
    create -> wait for ssh -> ship this tree -> build QEMU -> build guest image
    -> boot the VM -> stage guest libs -> tests/validate.sh --json -> collect
    -> DESTROY.

    Results are appended to sweep-results.json and rendered as markdown.

SAFETY
    - Every instance is destroyed in a finally block, including on Ctrl-C, and
      destruction is retried.  An orphaned GPU box bills until someone notices.
    - --max-spend is enforced before each create, not just reported at the end.
    - Nothing but this git tree is copied to the box.  These are third-party
      machines: no credentials, no keys, no data.
    - --dry-run shows the plan and spends nothing.  It is the default.

USAGE
    ./scripts/sweep_matrix.py --search 'vms_enabled=true num_gpus=1' --limit 8
    ./scripts/sweep_matrix.py --search '...' --limit 8 --go --max-spend 5.00
    ./scripts/sweep_matrix.py --render        # re-render the table only
    ./scripts/sweep_matrix.py --check-tree    # does the tree match HEAD? (free)
"""
import argparse, json, os, re, shlex, subprocess, sys, time, datetime

KVM_IMAGE = "docker.io/vastai/kvm:ubuntu_cli_22.04-2025-05-16"
SWEEP_LABEL = "nvkvm-sweep"

# Graceful cancellation, observed BY THE SCRIPT.
#
# Ctrl-C cannot be relied on here.  A run started as a background job inherits
# SIGINT as ignored, so the signal is silently dropped and the run continues;
# escalating to SIGKILL then bypasses the finally: block that destroys the
# instance.  That is not hypothetical -- it orphaned instance 48253468, a box
# created seconds before the kill, which billed until it was found by reading
# `vastai show instances` rather than by trusting the kill.
#
# So: to stop a run, `touch /tmp/nvkvm-sweep.stop`.  It is checked between
# drivers and between boxes, unwinds through the normal paths, and every
# instance is destroyed on the way out.
STOP_FILE = "/tmp/nvkvm-sweep.stop"


def stop_requested():
    return os.path.exists(STOP_FILE)

# Instances this script must NEVER destroy, whatever else happens.
#
# reap_strays() already filters on SWEEP_LABEL, so in principle nothing else is
# reachable.  This is the second lock: label filtering is one typo (or one
# accidentally-labelled rental) away from destroying someone else's running
# work, and these are long-lived boxes other people are actively using.  A
# destroy is irreversible and a rented GPU box can hold hours of state.
#
#   48097794, 48210901  other nvkvm work
#   48251528            Blackwell 4x RTX 5060 multi-GPU / peer-access test
PROTECTED_INSTANCES = {"48097794", "48210901", "48251528"}
RESULTS   = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                         "tests", "sweep-results.json")
REPO      = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# ---------------------------------------------------------------------------
# THE DRIVER MATRIX
#
# Vast's advertised driver_version is accurate for CUDA-container rentals, but
# meaningless for the KVM template we need: the GPU is VFIO-passed-through into
# a guest image that vast ships with a *pre-installed* driver, identical on
# every host.  Ten boxes rented on 2026-08-20 all reported 575.51.03 for that
# reason, while the sweep believed it was spanning 570 and 580.  Selecting
# offers by driver cannot work.  Installing the driver ourselves can.
#
# WHICH VERSIONS.  Not arbitrary ones.  nvkvm's correctness against a driver is
# decided by ONE thing: does the ABI profile it selects describe that driver's
# real struct layouts?  So the matrix is derived from the profile table in
# src/common/nvkvm_abi.h and documented in docs/reference/abi-profiles.md --
# one representative per profile, plus BOTH SIDES of every boundary where a
# layout changes inside a branch.
#
# The intra-branch splits are the whole point.  A major-only lookup mis-keys
# exactly two versions, and both are shipped GA drivers people actually run:
#   535.54.03  channel alloc measures 304 B   |  535.86.05 measures 360 B  (CC fields)
#   550.40.07  UVM array measures 1200 B      |  550.40.53 measures 9264 B (V550 UVM)
# Testing 535 and 550 "generally" would miss both.  Testing these four pins them.
DRIVER_MATRIX = [
    # version,      expected ABI profile, why this exact version is in the list
    ("515.65.01",  "515", "profile floor: NV00DE/RM_USER_SHARED_DATA absent from the tree"),
    ("525.147.05", "525", "pre-CC channel (304 B), pre-V545 mem/nv00de"),
    ("535.54.03",  "525", "INTRA-BRANCH LOW: the 535 GA still measures 304 B"),
    ("535.86.05",  "535", "INTRA-BRANCH HIGH: CC channel fields, 360 B"),
    ("545.23.08",  "545", "V545 mem/nv00de, still pre-V550 UVM"),
    ("550.40.07",  "545", "INTRA-BRANCH LOW: UVM array still 1200 B"),
    ("550.40.53",  "550", "INTRA-BRANCH HIGH: V550 UVM array, 9264 B"),
    ("565.57.01",  "550", "top of the 550 profile range"),
    ("570.124.06", "570", "the H100 vk_compute_dispatch suspect"),
    ("575.51.03",  "570", "what the vast KVM image ships, so our baseline"),
    ("580.95.05",  "580", "V580 VASPACE + V580 NVOS46, each +8 bytes"),
    ("610.43.02",  "610", "V610 channel, 376 B, +hHandleVASpace"),
]

# NOT EVERY VERSION IN THE MATRIX IS STILL DOWNLOADABLE.
#
# Checked 2026-08-21 with HEAD+ranged-GET against both paths in driver_urls():
# three rows 404 at BOTH the tesla/ and XFree86/ paths and cannot be installed
# from NVIDIA at all any more:
#
#   545.23.08   gone   -> 545.23.06 (published, also major 545 -> NVKVM_ABI_545)
#   550.40.53   gone   -> 550.54.14 (published, minor!=40 -> NVKVM_ABI_550)
#   575.51.03   gone   -> already PREINSTALLED on the vast KVM image, so it is
#                         installed by doing nothing; see the fast path in
#                         install_driver().
#
# The substitutes are chosen to land on the SAME profile as the row they
# replace -- verified against nvkvm_abi_id_for_version() in
# src/common/nvkvm_abi.h, not assumed -- so the ABI expectation in
# DRIVER_MATRIX stays correct.
#
# CAVEAT, and it matters for how the 550 result is read: the 550 boundary is
# defined at exactly 550.40.53, and 550.54.14 is merely the nearest published
# version on the high side.  Substituting it still exercises both profiles and
# still proves the selector splits *inside* major 550, but it no longer pins
# the exact .53 cutoff.  Any run that used a substitute records it in
# `driver_actual` and is flagged in the rendered table.
DRIVER_ALTS = {
    "545.23.08": ["545.23.06", "545.29.06"],
    "550.40.53": ["550.54.14", "550.90.07"],
    "575.51.03": [],   # preinstalled; nothing to download
}

# A driver older than the silicon cannot be a finding.  These are the majors at
# which each architecture became supported at all; below the floor the install
# succeeds and `nvidia-smi` reports no devices, which is NOT an nvkvm result and
# is recorded as `driver-predates-gpu`.
ARCH_FLOOR = {
    "turing": 410, "ampere": 450, "ada": 520, "hopper": 525, "blackwell": 570,
}

ARCH_OF = [   # substring -> architecture, FIRST MATCH WINS: specific before generic
    ("RTX 6000 Ada", "ada"), ("RTX 5000 Ada", "ada"), ("RTX 4500 Ada", "ada"),
    ("RTX 4000 Ada", "ada"), ("RTX 2000 Ada", "ada"),
    ("A100", "ampere"), ("H100", "hopper"), ("H200", "hopper"),
    ("GH200", "hopper"), ("B200", "blackwell"),
    ("L40", "ada"), ("L4", "ada"),
    ("RTX PRO 6000", "blackwell"), ("RTX 50", "blackwell"),
    ("RTX 40", "ada"), ("RTX 30", "ampere"), ("RTX 20", "turing"),
    ("GTX 16", "turing"), ("Quadro RTX", "turing"), ("Tesla T4", "turing"),
    ("RTX A", "ampere"), ("A10", "ampere"), ("A40", "ampere"),
]

# Blackwell and the datacenter Hopper parts need the open kernel module; the
# proprietary one either refuses to build or refuses to bind.
OPEN_MODULE_ARCHES = {"blackwell", "hopper"}


def arch_of(gpu_name):
    # Whitespace-insensitive: vast writes "RTX 6000Ada" where NVIDIA writes
    # "RTX 6000 Ada", and a missed match silently sweeps the wrong driver
    # floor (an Ada card would be offered 515, which cannot see it).
    g = re.sub(r"\s+", "", (gpu_name or "").upper())
    for pat, arch in ARCH_OF:
        if re.sub(r"\s+", "", pat.upper()) in g:
            return arch
    return None


def drivers_for(gpu_name, requested):
    """The matrix rows applicable to this GPU, newest last.

    Filters out drivers that predate the silicon -- running them would produce
    a 'no devices' result that says nothing about nvkvm.
    """
    arch = arch_of(gpu_name)
    floor = ARCH_FLOOR.get(arch, 0)
    out = []
    for ver, prof, why in DRIVER_MATRIX:
        if requested and ver not in requested:
            continue
        if int(ver.split(".")[0]) < floor:
            continue
        out.append((ver, prof, why))
    return out


def driver_urls(ver):
    """NVIDIA publishes datacenter and consumer drivers under different paths,
    and neither is a superset.  Try both; the first that 404s is not an error.

    (Took a kernel_open argument that it never used -- the module flavour is an
    installer flag, not a URL component.)"""
    return [
        f"https://us.download.nvidia.com/tesla/{ver}/NVIDIA-Linux-x86_64-{ver}.run",
        f"https://us.download.nvidia.com/XFree86/Linux-x86_64/{ver}/NVIDIA-Linux-x86_64-{ver}.run",
    ]


_PURGED_HOSTS = set()


def rsh(S, cmd):
    """Quote a remote command ONCE, correctly.

    The f"{S} '...'" idiom used elsewhere in this file cannot express a command
    that itself contains single quotes -- an inner ' closes the outer one and
    the remainder is reinterpreted locally.  `apt-get remove '^nvidia-.*'` and
    `pkill -f '[q]emu...'` both do, and both silently mangled into something
    other than what was written.  shlex.quote handles the general case.
    """
    return f"{S} {shlex.quote(cmd)}"


def quiesce_gpu_clients(S):
    """Stop every workload which can retain an NVIDIA device fd.

    The KVM CLI image historically used by this sweep has no graphical
    session.  The desktop image does: it boots graphical.target, SDDM
    auto-logs UID 1000 into Plasma/Xorg, and Selkies keeps a user service alive
    which repeatedly runs nvidia-smi.  Merely stopping the display manager is
    therefore insufficient -- a freshly respawned nvidia-smi can race module
    removal after Xorg has gone away.

    Measured on Vast instance 48668379 (the requested desktop template): Xorg
    held nvidia_drm, and after isolating multi-user.target Selkies still held
    nvidia through its nvidia-smi sampler.  End the seat and its user manager,
    then kill any remaining holders by device fd.  This is a dedicated,
    disposable sweep VM; no unrelated GPU workload belongs on it.
    """
    cmd = r"""
systemctl isolate multi-user.target 2>/dev/null || true
systemctl stop display-manager.service sddm.service gdm.service gdm3.service lightdm.service 2>/dev/null || true
systemctl stop nvkvm-vm.service nvidia-persistenced.service 2>/dev/null || true

seat_uids="$(loginctl list-sessions --no-legend 2>/dev/null | awk '$4 == "seat0" { print $2 }' | sort -u)"
loginctl terminate-seat seat0 2>/dev/null || true
for uid in $seat_uids; do
    systemctl stop "user@${uid}.service" 2>/dev/null || true
    pkill -TERM -u "$uid" 2>/dev/null || true
done

# The bracket keeps pkill from matching this remote shell's own argv.
pkill -TERM -f '[q]emu-system-x86_64' 2>/dev/null || true

if command -v fuser >/dev/null 2>&1; then
    fuser -k -TERM /dev/nvidia* 2>/dev/null || true
    sleep 2
    fuser -k -KILL /dev/nvidia* 2>/dev/null || true
fi

if systemctl is-active --quiet display-manager.service 2>/dev/null; then
    echo '[HARNESS] display-manager.service is still active after quiescing'
    exit 70
fi

if command -v fuser >/dev/null 2>&1; then
    holders="$(fuser /dev/nvidia* 2>/dev/null || true)"
    if [ -n "$holders" ]; then
        echo "[HARNESS] NVIDIA device fds are still held by PIDs:$holders"
        fuser -v /dev/nvidia* 2>&1 || true
        exit 71
    fi
fi
"""
    out, rc = sh(rsh(S, cmd), timeout=240)
    if rc != 0:
        return False, out[-1600:] or f"[HARNESS] GPU quiesce failed (rc={rc})"
    return True, ""


def unload_nvidia_modules(S):
    """Remove the running NVIDIA stack or return an explicit harness error."""
    cmd = r"""
for attempt in 1 2 3; do
    for module in nvidia_peermem nvidia_uvm nvidia_drm nvidia_modeset nvidia; do
        rmmod "$module" 2>/dev/null || true
    done
    grep -q '^nvidia' /proc/modules || exit 0
    sleep 1
done

echo '[HARNESS] NVIDIA modules remain loaded after GPU clients were quiesced:'
grep '^nvidia' /proc/modules || true
if command -v fuser >/dev/null 2>&1; then
    fuser -v /dev/nvidia* 2>&1 || true
fi
exit 72
"""
    out, rc = sh(rsh(S, cmd), timeout=180)
    if rc != 0:
        return False, out[-1600:] or f"[HARNESS] NVIDIA module unload failed (rc={rc})"
    return True, ""


def purge_distro_driver(S):
    """Remove the DISTRO-PACKAGED NVIDIA driver before the first .run install.

    The vast KVM image does not ship a .run-installed driver: it ships Debian
    packages, and they are HELD (`dpkg -l` shows `hi`, and there is no
    /usr/bin/nvidia-uninstall).  That matters because the .run installer only
    replaces the files IT owns.  The dpkg-owned userspace in
    /usr/lib/x86_64-linux-gnu survives, so after installing, say, 535.86.05 the
    box has a 535 kernel module and a leftover 575 libcuda/libnvidia-ml.

    Every consumer of that pair then fails with "Driver/library version
    mismatch" / cuInit 803 -- and make_host_bundle.sh would faithfully collect
    the STALE 575 libraries (it globs $SYSLIB by version) and ship them to the
    guest.  That is a false failure that looks exactly like an nvkvm bug, which
    is the specific outcome this sweep must not produce.

    Held packages are skipped by apt unless unheld first, so unhold, then purge.
    """
    sh(rsh(S, "apt-mark unhold $(dpkg -l | awk '/^hi/ {print $2}') 2>/dev/null; true"),
       timeout=180)
    sh(rsh(S, "DEBIAN_FRONTEND=noninteractive apt-get remove --purge -y "
              "'^nvidia-.*' '^libnvidia-.*' '^cuda-drivers.*' "
              "'^xserver-xorg-video-nvidia.*' > /root/purge.log 2>&1; true"), timeout=900)
    # libnvidia-container* belongs to the docker runtime, not the driver; if the
    # purge above took it, nothing we do needs it back.
    sh(rsh(S, "DEBIAN_FRONTEND=noninteractive apt-get install -y -q "
              "build-essential dkms pkg-config libglvnd-dev linux-headers-$(uname -r) "
              "> /root/prereq.log 2>&1; true"), timeout=1200)

    # A previous .RUN install is not a dpkg package and survives the purge
    # above.  nvidia-installer then refuses outright:
    #
    #   ERROR: The installation was canceled due to the availability or
    #   presence of an alternate driver installation.
    #
    # MEASURED on instance 48277346 (RTX 3090, shipped 535.288.01): both
    # 580.95.05 and 610.43.02 downloaded and self-checked clean, then aborted
    # with exactly that line -- which lands in the table as
    # driver-install-failed and reads like "these drivers will not install on
    # Ampere".  They would; the box simply had a driver we had not removed.
    # The installer ships its own uninstaller for precisely this, so use it.
    sh(rsh(S, "test -x /usr/bin/nvidia-uninstall && "
              "/usr/bin/nvidia-uninstall --silent --no-questions "
              ">> /root/purge.log 2>&1; true"), timeout=900)
    left, _ = sh(rsh(S, "ls /usr/lib/x86_64-linux-gnu/libnvidia-ml.so.* "
                       "/usr/lib/x86_64-linux-gnu/libcuda.so.* 2>/dev/null | wc -l"),
                 timeout=120)
    if left.strip().isdigit() and int(left.strip()) > 0:
        print(f"    (warning: {left.strip()} nvidia userspace libs survived the purge)",
              flush=True)


def installed_driver_version(S):
    """The running kernel module's version, or '' if none is loaded."""
    got, _ = sh(f"{S} 'cat /proc/driver/nvidia/version 2>/dev/null | head -1'", timeout=60)
    import re as _re
    # BOTH banner forms.  Proprietary:
    #     NVRM version: NVIDIA UNIX x86_64 Kernel Module  575.51.03  Wed Apr 16 ...
    # Open (-m=kernel-open, mandatory on Blackwell and datacenter Hopper):
    #     NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  580.95.05  Release Build ...
    # The optional "for <arch>" is the whole difference, and without it this
    # returns "" for every open-module box -- which makes install_driver() report
    # a perfectly good install as a failure and FAILS the box, on exactly the
    # architectures that require the open module.  Measured on an RTX 5060,
    # 580.95.05 open, 2026-08-24.
    m = _re.search(r"Kernel Module(?:\s+for\s+\S+)?\s+([0-9]+\.[0-9]+(?:\.[0-9]+)?)", got)
    return m.group(1) if m else ""


def kernel_cc(S):
    """Path to the compiler the RUNNING KERNEL was built with, installing it if
    needed.  Returns "" if it cannot be determined.

    THIS IS NOT A DETAIL.  The vast image runs Ubuntu 22.04 with the 6.8 HWE
    kernel, which was built by gcc-12 and whose Kbuild therefore passes
    -ftrivial-auto-var-init=zero.  The image's DEFAULT cc is gcc-11, which does
    not know that flag, so EVERY .run kernel-module build dies with

        cc: error: unrecognized command-line option '-ftrivial-auto-var-init=zero'

    ...thousands of times, and the installer reports only "The nvidia kernel
    module was not created."  Measured on instance 48252610: 535.54.03 failed
    exactly this way for both -m=kernel and -m=kernel-open, and it looks
    precisely like "this old driver cannot build on this kernel" -- a GPU/driver
    conclusion -- when the real cause is that we handed it the wrong compiler.
    The preinstalled 575.51.03 module reports "GCC version: gcc version 12.3.0",
    i.e. the image itself was built with gcc-12; only our invocation was not.

    gcc-12 is already present on the image, so this is usually just a matter of
    naming it.
    """
    ver, _ = sh(f"{S} 'cat /proc/version'", timeout=60)
    m = re.search(r"gcc-(\d+)", ver) or re.search(r"gcc[^0-9]*(\d+)\.", ver)
    if not m:
        return ""
    maj = m.group(1)
    path = f"/usr/bin/gcc-{maj}"
    out, _ = sh(rsh(S, f"test -x {path} && echo HAVE || echo NEED"), timeout=60)
    if "NEED" in out:
        sh(rsh(S, f"DEBIAN_FRONTEND=noninteractive apt-get install -y -q gcc-{maj} "
                  f">> /root/prereq.log 2>&1; true"), timeout=900)
    out, _ = sh(rsh(S, f"test -x {path} && echo HAVE || echo NEED"), timeout=60)
    return path if "HAVE" in out else ""


def _fetch_installer(S, candidates, tried):
    """Download the first candidate that arrives INTACT.  Returns (ok, version).

    Appends one human-readable line per URL to `tried`, carrying the observed
    HTTP status and curl exit, so a failure says what actually happened
    instead of being guessed at afterwards.
    """
    for cand in candidates:
        for url in driver_urls(cand):
            out, _ = sh(rsh(S,
                f"curl -sS -L --max-time 1500 -w '\\nHTTP=%{{http_code}}' "
                f"-o /root/drv.run {url} 2>&1; echo \" CURLRC=$?\"; "
                f"df -P /root | tail -1"), timeout=1800)
            m = re.search(r"HTTP=(\d+)", out)
            code = m.group(1) if m else ""
            m2 = re.search(r"CURLRC=(\d+)", out)
            crc = m2.group(1) if m2 else "?"

            # NB: curl's own exit code, not the ssh command's.  The ssh rc is
            # the exit of the trailing `df`, which is always 0 -- checking it
            # (as this once did) accepted every failed transfer as a success.
            if code == "200" and crc == "0":
                # VERIFY THE ARCHIVE rather than trusting HTTP 200.  The
                # 610.43.02 installer once arrived with HTTP=200 and curl_rc=0
                # and was still corrupt: it printed "Verifying archive
                # integrity... Error in check sums 3400907251 3425542902" and
                # the row was filed driver-install-failed -- which reads as
                # "610 will not install", a driver conclusion, for what was a
                # bad download of a perfectly good installer.  The .run is a
                # makeself archive and can check itself; ask it.
                _, ok1 = sh(rsh(S, "chmod +x /root/drv.run && test -s /root/drv.run"),
                            timeout=120)
                _, ok2 = sh(rsh(S, "/root/drv.run --check >/root/drvcheck.log 2>&1"),
                            timeout=900)
                if ok1 == 0 and ok2 == 0:
                    return True, cand
                tried.append(f"{url}  -> HTTP=200 but CORRUPT "
                             f"(test -s rc={ok1}, --check rc={ok2})")
                continue
            tried.append(f"{url}  -> HTTP={code or '?'} curl_rc={crc}")
    return False, None


def install_driver(S, ver, arch, log):
    """Replace the host driver in the rented VM.  Returns (ok, detail, actual).

    This is only possible because the KVM template is a real VM with the GPU
    passed through -- the NVIDIA module is loaded inside the instance, so it is
    ours to replace.  (`systemd-detect-virt` returns `kvm`; on a CUDA-container
    rental the module belongs to the physical host and none of this works.
    VERIFIED on instance 48210901: systemd-detect-virt = kvm, driver 575.51.03
    loaded inside the guest, /dev/kvm present.)

    `actual` is the version that ended up installed, which is NOT always `ver`:
    three matrix rows are no longer published and fall back to DRIVER_ALTS.
    """
    kernel_open = arch in OPEN_MODULE_ARCHES

    # This must precede even the fast path.  A preinstalled driver does not
    # need replacing, but validating nvkvm while the desktop template's Xorg
    # and video streamer are concurrently consuming the passthrough GPU makes
    # both correctness and performance results ambiguous.
    quiet, detail = quiesce_gpu_clients(S)
    if not quiet:
        return False, detail, None

    # FAST PATH.  575.51.03 is preinstalled on the vast KVM image and is no
    # longer downloadable from either NVIDIA path, so the only way to test that
    # row is to recognise that it is already installed.  This also saves a
    # ~350 MB download plus a module rebuild whenever a swap is a no-op.
    cur = installed_driver_version(S)
    if cur == ver:
        return True, f"already installed: {ver} (no download; preinstalled or prior step)", ver

    candidates = [ver] + DRIVER_ALTS.get(ver, [])
    if ver in DRIVER_ALTS and not DRIVER_ALTS[ver] and cur != ver:
        return (False,
                f"{ver} is not published at either NVIDIA path and is not preinstalled "
                f"(box has {cur or 'no driver'})", None)

    # The distro packages only need clearing once per box, before the first
    # .run lands.  After that the .run owns the userspace.
    # Keyed by BOX, not global.  This was a function attribute, so after the
    # first box set it the second box never purged its distro driver at all --
    # a .run would have been layered over a live dpkg userspace, which is the
    # exact stale-library mismatch purge_distro_driver() exists to prevent.
    if S not in _PURGED_HOSTS:
        purge_distro_driver(S)
        _PURGED_HOSTS.add(S)

    # DO NOT collapse every curl failure into "not published".
    #
    # The first version of this said exactly that whenever curl returned
    # non-zero, and it was wrong in a way that matters: on 2026-08-21 the sweep
    # reported "no installer published at any path" for 570.124.06 and
    # 535.54.03 -- both of which return HTTP 200 to a HEAD request from
    # elsewhere, and one of which this very script had downloaded successfully
    # an hour earlier.  A transport failure was being written down as a fact
    # about what NVIDIA publishes.  That is a harness failure wearing the
    # costume of a finding, which is the one thing this sweep must not produce.
    #
    # So classify: an HTTP 404 means genuinely absent; anything else (disk
    # full, DNS, connection reset, rate-limiting, timeout) is transport and is
    # labelled [HARNESS].
    # NVIDIA's CDN returns intermittent HTTP 403 to some rented hosts.
    # MEASURED on the RTX 4070 box: 550.40.07 got 403 at both paths, then
    # 565.57.01 downloaded fine seconds later, then 570/580/610 got 403 again.
    # That is throttling / IP reputation, not a missing file -- so retry the
    # whole candidate list a few times with backoff before giving up on a row.
    fetched, use_ver, tried = False, None, []
    for _attempt in range(3):
        if _attempt:
            time.sleep(30 * _attempt)
        fetched, use_ver = _fetch_installer(S, candidates, tried)
        if fetched:
            break

    if not fetched:
        only404 = all("HTTP=404" in t for t in tried) and tried
        if only404:
            return False, ("not published by NVIDIA (HTTP 404 at every path); tried:\n  " +
                           "\n  ".join(tried[-4:])), None
        return False, ("[HARNESS] download failed for a TRANSPORT reason, not a missing "
                       "file -- do NOT record this as 'NVIDIA no longer publishes X'. "
                       "Check disk, DNS and CDN rate-limiting on the box; tried:\n  " +
                       "\n  ".join(tried[-4:])), None

    # Nothing may hold the device or the module cannot be unloaded.
    #
    # NOTE the bracket in '[q]emu-system-x86_64'.  Written plainly, pkill -f
    # matches the command line of the very `bash -c '...'` this runs inside --
    # that string is literally in its argv -- so pkill kills its own parent
    # shell and the rest of the command (the gpu-reset, the `true`) never runs.
    # This project has a standing rule against `pkill -f qemu-system-x86` for
    # exactly this reason; the bracket makes the pattern not match itself.
    # Downloads and package removal take long enough for an ill-behaved user
    # service to respawn.  Re-quiesce at the actual module boundary, and never
    # run the installer if the old stack cannot be proven absent.
    quiet, detail = quiesce_gpu_clients(S)
    if not quiet:
        return False, detail, None
    unloaded, detail = unload_nvidia_modules(S)
    if not unloaded:
        return False, detail, None

    mod   = "-m=kernel-open" if kernel_open else "-m=kernel"
    other = "-m=kernel" if kernel_open else "-m=kernel-open"
    base  = "--silent --no-questions --ui=none --disable-nouveau --install-libglvnd"

    cc = kernel_cc(S)
    ccenv = f"CC={cc} " if cc else ""

    # LESSON, recorded where the next person will be tempted to undo it:
    # --no-cc-version-check exists to get past a COSMETIC compiler mismatch
    # (kernel built by gcc 12.3, install running gcc 12.2 -- harmless).  It
    # also hides a REAL one.  Passing it unconditionally is what turned a
    # one-line "compiler version mismatch" abort into thousands of lines of
    # "unrecognized command-line option" followed by the uninformative "The
    # nvidia kernel module was not created" -- a message indistinguishable
    # from a genuine driver/kernel incompatibility, and one that would have
    # been written into a results table as a GPU finding.  If you re-add it to
    # the first attempt, you are re-arming exactly that trap.
    #
    # Attempt order is diagnostic, not just hopeful.
    #
    # --no-cc-version-check is DELIBERATELY absent from the first two attempts.
    # That check is the one thing that would have named the gcc-11/gcc-12
    # mismatch out loud instead of letting it become 4000 lines of
    # "unrecognized command-line option" and a bare "module was not created".
    # Suppressing it by default converted a clear toolchain error into
    # something that reads like a driver/kernel incompatibility.  So: let it
    # speak first, and only disable it as a last resort for the case where the
    # check is merely being pedantic about a minor version.
    #
    # Per-driver log.  This used to write every install to one hardcoded
    # /root/drvinstall.log, so the log for a failed install was overwritten by
    # the next driver before anyone could read it -- and `log`, the parameter
    # naming the file, was accepted and then ignored.
    attempts = [
        f"{ccenv}/root/drv.run {base} {mod}",
        f"{ccenv}/root/drv.run {base} {other}",
        f"{ccenv}/root/drv.run {base} --no-cc-version-check {mod}",
    ]
    rc = 1
    for i, cmd in enumerate(attempts):
        redir = ">" if i == 0 else ">>"
        _, rc = sh(rsh(S, f"{cmd} {redir} {log} 2>&1"), timeout=2400)
        if rc == 0:
            break
    if rc != 0:
        tail, _ = sh(rsh(S, f"tail -25 {log}"), timeout=60)
        hint = ""
        # Match the CLASS, not the symptom.  -ftrivial-auto-var-init=zero is
        # merely the flag today's kernel/compiler pair happens to disagree
        # about; any "cc: error:" or "unrecognized command-line option" in a
        # driver install log means the module was compiled by the wrong
        # toolchain, whatever the flag is called next year.
        gt, _ = sh(rsh(S, f"grep -ciE 'unrecognized command-line option|cc: error:|"
                          f"ftrivial-auto-var-init' {log} 2>/dev/null || echo 0"),
                   timeout=60)
        if gt.strip().isdigit() and int(gt.strip()) > 0:
            hint = (f"\n[HARNESS] kernel-module build failed with compiler errors "
                    f"(kernel_cc={cc or 'UNDETECTED'}); this is a TOOLCHAIN "
                    f"failure, NOT a driver/GPU result.  Compare /proc/version's "
                    f"compiler against the cc actually used.")
        return False, (tail[-1100:] + hint), None

    sh(f"{S} 'modprobe nvidia; modprobe nvidia_uvm; true'", timeout=180)
    got = installed_driver_version(S)
    if got != use_ver:
        raw, _ = sh(f"{S} 'cat /proc/driver/nvidia/version 2>/dev/null | head -1'", timeout=60)
        return False, f"installed {use_ver} but /proc reports: {raw.strip()[:200]}", None
    detail = f"{use_ver}" + ("" if use_ver == ver else f" (SUBSTITUTE for unpublished {ver})")
    return True, detail, use_ver


def sh(cmd, timeout=120, check=False):
    p = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
    if check and p.returncode != 0:
        raise RuntimeError(f"{cmd}\n{p.stdout}\n{p.stderr}")
    return p.stdout.strip(), p.returncode


def vast_json(args, timeout=120):
    out, rc = sh(f"vastai {args} --raw", timeout=timeout)
    if rc != 0:
        return None
    try:
        return json.loads(out)
    except Exception:
        return None


# Pre-Turing. The project states these do not work at all -- cuInit fails and
# the open kernel module will not probe them -- so renting one measures a
# documented negative. Worth doing ONCE, deliberately, with
# --include-unsupported; never worth a slot in a coverage sweep, which is what
# happened the first time this script ran (it sorted purely by price and picked
# a Quadro P4000 for $0.067 over cards whose behaviour is actually unknown).
UNSUPPORTED_MARKERS = (
    "P4000", "P5000", "P6000", "P100", "P40", "P4",          # Pascal
    "GTX 10", "TITAN X", "TITAN Xp",                          # Pascal consumer
    "M4000", "M6000", "M60", "M40", "GTX 9",                  # Maxwell
    "K80", "K40", "GTX 7",                                    # Kepler
)


def is_unsupported(gpu):
    g = (gpu or "").upper()
    return any(m.upper() in g for m in UNSUPPORTED_MARKERS)


def search(filt, limit, max_price, include_unsupported=False, prefer_untested=None,
           per_model=False):
    offers = vast_json(f"search offers '{filt}' -o dph") or []
    seen, picked = set(), []

    def rank(o):
        # Coverage value first, price second. Cheapness is a tiebreak, not the
        # goal: a $0.04 box that re-confirms a card already in the table buys
        # nothing, and a $0.30 box on an untested die buys a row.
        gpu = (o.get("gpu_name") or "")
        untested = 0 if (prefer_untested and any(t.upper() in gpu.upper()
                                                 for t in prefer_untested)) else 1
        return (untested, o.get("dph_total", 99))

    for o in sorted(offers, key=rank):
        gpu = o.get("gpu_name")
        if not include_unsupported and is_unsupported(gpu):
            continue
        # One box per ARCHITECTURE by default.  Two changes from the old key:
        #
        #   - it no longer includes the advertised driver branch, which is a
        #     field constant for KVM rentals (every box boots the same vast
        #     guest image with the same pre-installed driver).  That key could
        #     never yield driver diversity no matter how many offers it walked.
        #     The driver is now a controlled variable we install.
        #   - it keys on architecture, not model, because the claim being built
        #     is "every ABI profile on every architecture".  Keying on model
        #     spent three of four boxes on GTX 1660 / 1660 S / 1660 Ti -- all
        #     Turing, all the same answer -- while Ada and Blackwell went
        #     unrented.  --per-model restores the old behaviour when the
        #     question really is die-level (as it was for GA100 vs GA10x).
        key = (arch_of(gpu) or gpu) if not per_model else gpu
        if key in seen or o.get("dph_total", 99) > max_price:
            continue
        seen.add(key)
        picked.append(o)
        if len(picked) >= limit:
            break
    return picked


def wait_ssh(iid, budget=2400):
    """Poll until the instance reports running AND accepts ssh.

    Two changes forced by measurement on 2026-08-21:

    1. BUDGET.  900s was not enough.  An RTX 3060 (instance 48257135) never
       answered inside it and was recorded `no-ssh`, costing the entire Ampere
       row of that sweep -- and Ampere had zero coverage to begin with.  A box
       in run 2 took roughly ten minutes just to start answering, so 15 minutes
       total left almost no margin.  Provisioning here is genuinely slow and
       waiting is far cheaper than re-renting.

    2. PROXY FALLBACK.  We ask for --direct and connect to
       public_ipaddr:<mapped 22/tcp>, but that path is not always reachable --
       some hosts firewall the mapped port even though the rental is fine.
       Vast always also exposes a jump-host route (ssh_host/ssh_port, i.e.
       ssh N.vast.ai), so try direct first and fall back to it rather than
       declaring the box dead.  Returns whichever pair actually answered.
    """
    deadline = time.time() + budget
    while time.time() < deadline:
        d = vast_json(f"show instance {iid}") or {}
        if d.get("actual_status") == "running":
            cands = []
            direct_port = (d.get("ports") or {}).get("22/tcp")
            if d.get("public_ipaddr") and direct_port:
                cands.append((d["public_ipaddr"], direct_port[0]["HostPort"]))
            if d.get("ssh_host") and d.get("ssh_port"):
                cands.append((d["ssh_host"], d["ssh_port"]))
            for host, port in cands:
                out, rc = sh(f"ssh -o ConnectTimeout=12 -o StrictHostKeyChecking=no "
                             f"-o UserKnownHostsFile=/dev/null -p {port} root@{host} 'echo OK'",
                             timeout=40)
                if "OK" in out:
                    return host, port
        time.sleep(20)
    return None, None


def tree_stamp():
    """Identify the tree actually shipped -- commit AND dirty state.

    The sweep tars the WORKING TREE, not HEAD.  On 2026-08-20 a worktree
    sitting on uncommitted deletions shipped a tree with the UVM 65 fix
    reverted while HEAD was two commits past it, and the resulting
    cuCtxCreate 999 was nearly misread as a fresh regression.  A result
    without the tree it came from is not interpretable.
    """
    h, _ = sh("git -C %s rev-parse --short HEAD" % REPO, timeout=30)
    d, _ = sh("git -C %s status --porcelain -- . ':!tests/sweep-results.json'" % REPO, timeout=30)
    return {"commit": h.strip(), "dirty_files": sorted(
        l.split(maxsplit=1)[-1] for l in d.splitlines() if l.strip())}


def check_tree(allow_dirty=False):
    """Refuse to spend money on a tree that is not the tree you think it is.

    tree_stamp() RECORDS the dirty state, which is how the 2026-08-20 incident
    was eventually understood -- after the boxes were rented and the bill paid.
    Recording is not preventing.  This runs BEFORE any create, and stops a
    sweep whose result would be uninterpretable before it costs anything.

    CI cannot cover this: a CI runner checks out a clean tree by definition, so
    the failure mode -- a long-lived worktree that has drifted from HEAD -- can
    only be caught on the machine that is about to ship the tarball.  That is
    here.

    --allow-dirty is the deliberate escape hatch for the real case of sweeping
    an uncommitted fix.  It is loud on purpose: the dirty files go into every
    result record either way, so the tarball stays interpretable.
    """
    st = tree_stamp()
    print(f"tree: {st['commit']}"
          f"{' + %d uncommitted file(s)' % len(st['dirty_files']) if st['dirty_files'] else ' (clean)'}")
    if not st["dirty_files"]:
        return True
    for f in st["dirty_files"]:
        print(f"    dirty: {f}")
    if allow_dirty:
        print("--allow-dirty given: shipping the WORKING TREE, not "
              f"{st['commit']}.  The files above are what differs.")
        return True
    print("\nREFUSING: the working tree does not match HEAD, and the sweep tars the\n"
          "working tree.  Whatever these boxes measure will be attributed to\n"
          f"{st['commit']}, which is not what would run.  On 2026-08-20 that shipped a\n"
          "216-line silent revert to a paid sweep and nearly cost a week chasing a\n"
          "regression that had already been fixed.\n\n"
          "  commit the change, stash it, or pass --allow-dirty to ship it knowingly.")
    return False


def parse_contract(out):
    """Extract the new instance id from `vastai create instance` output.

    MUST be tolerant.  If a box is created and we lose its id, the finally
    block has nothing to destroy and it bills until a human notices.  The CLI
    prints "Started. {'success': True, 'new_contract': 123}" -- which is NOT
    json (bare True, prose prefix), so a plain json.loads here silently
    orphans every box.  Try json, then the id by regex, then the trailing
    "launched successfully" line.
    """
    try:
        d = json.loads(out)
        if isinstance(d, dict) and d.get("new_contract"):
            return d["new_contract"]
    except Exception:
        pass
    for pat in (r"new_contract['\"]?\s*[:=]\s*(\d+)",
                r"launched successfully:\s*(\d+)"):
        m = re.search(pat, out)
        if m:
            return int(m.group(1))
    return None


def destroy_verified(iid, tries=5):
    """Destroy an instance and CONFIRM it is gone from the listing.

    Two traps, both of which silently leave a box billing:
      1. `vastai destroy instance` PROMPTS "[y/N]".  Unattended there is no
         tty, it reads EOF, prints "Aborted." -- and exits 0.  Hence -y.
      2. Even then the exit code only says the CLI ran.  The only proof is
         the instance no longer appearing in `show instances`.
    Returns True only when the id is actually absent from the listing.
    """
    if str(iid) in PROTECTED_INSTANCES:
        print(f"  !! REFUSING to destroy protected instance {iid}", file=sys.stderr, flush=True)
        return False
    for _ in range(tries):
        sh(f"vastai destroy instance {iid} -y", timeout=90)
        time.sleep(8)
        data = vast_json("show instances") or []
        if isinstance(data, dict):
            data = data.get("instances", []) or []
        if not any(str(i.get("id")) == str(iid) for i in data):
            return True
        time.sleep(10)
    return False


def reap_strays():
    """Destroy anything still carrying our label.  Backstop, not primary path.

    Covers the cases run_one's finally cannot: create returned an unparseable
    id, or the create call timed out after the API had already committed the
    rental.  Only ever touches instances labelled SWEEP_LABEL.
    """
    data = vast_json("show instances") or []
    if isinstance(data, dict):
        data = data.get("instances", []) or []
    strays = [i for i in data if (i.get("label") or "") == SWEEP_LABEL
              and str(i.get("id")) not in PROTECTED_INSTANCES]
    for i in strays:
        iid = i.get("id")
        print(f"  !! STRAY {iid} ({i.get('gpu_name')}) alive -- destroying", file=sys.stderr, flush=True)
        if destroy_verified(iid):
            print(f"     destroyed {iid}", file=sys.stderr, flush=True)
        else:
            print(f"     !! COULD NOT DESTROY {iid} -- DESTROY BY HAND", file=sys.stderr, flush=True)
    return [i.get("id") for i in strays]


def boot_and_validate(S, args, drv):
    """Boot the nvkvm guest, stage libs against the CURRENT host driver, run
    validate.sh.  Called once per driver, so everything here must be repeatable.

    `drv` is the driver version actually installed -- needed because the bundle
    directory is named after it and the guest must be staged from THAT one.

    Returns a dict; never raises."""
    r = {}

    # The bundle is the host driver's userspace, so it MUST be rebuilt after
    # every swap.  Staging a 580 bundle against a 535 kernel module would fail
    # in ways that look like an nvkvm bug and are not.
    #
    # Delete the previous bundles first.  make_host_bundle.sh writes to
    # host-libs-<version>/, so sweeping N drivers on one box leaves N of them
    # on the 9p share, and stage_guest_libs.sh REFUSES to guess between
    # multiple candidates (it exits 1 having staged nothing).  Keeping exactly
    # one bundle makes that ambiguity impossible instead of relying on the
    # guest's dmesg parse to resolve it.
    sh(f"{S} 'rm -rf /root/nvkvm/host-libs-*'", timeout=120)
    _, rc = sh(f"{S} 'cd /root/nvkvm && bash scripts/make_host_bundle.sh > /root/bundle.log 2>&1'", timeout=600)
    if rc != 0:
        tail, _ = sh(f"{S} 'tail -30 /root/bundle.log'", timeout=60)
        r["status"] = "bundle-failed"; r["detail"] = tail[-1200:]; return r

    # VERIFY the bundle is the one we just built for THIS driver, rather than
    # trusting that it happened.  A bundle carrying a different version than
    # the loaded kernel module is the stale-bundle failure mode, and it
    # presents as a GPU failure downstream.
    bl, _ = sh(f"{S} 'ls -d /root/nvkvm/host-libs-* 2>/dev/null'", timeout=60)
    bundles = [b.strip() for b in bl.splitlines() if b.strip()]
    r["bundle"] = ",".join(os.path.basename(b) for b in bundles)
    if len(bundles) != 1 or not bundles[0].endswith(f"host-libs-{drv}"):
        r["status"] = "bundle-failed"
        r["detail"] = (f"expected exactly one bundle host-libs-{drv}, found: "
                       f"{r['bundle'] or '(none)'}")
        return r

    sh(f"{S} 'systemctl stop nvkvm-vm 2>/dev/null; true'", timeout=120)
    sh(f"{S} 'apt-get install -y -q sshpass >/dev/null 2>&1; "
       f"systemd-run --unit=nvkvm-vm --collect --setenv=VM_MEM=8G --setenv=VM_SMP=4 "
       f"--working-directory=/root/nvkvm bash scripts/run_test_vm.sh'", timeout=180)

    G = ("sshpass -p ubuntu ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=no "
         "-o UserKnownHostsFile=/dev/null -p 2222 ubuntu@localhost")
    booted = False
    for _ in range(30):
        out, _ = sh(f'{S} "{G} \'mountpoint -q /mnt/nvkvm && echo MOUNTED\'"', timeout=60)
        if "MOUNTED" in out:
            booted = True; break
        time.sleep(20)
    if not booted:
        r["status"] = "guest-no-boot"; return r

    # /mnt/nvkvm being mounted is NOT a readiness signal.  The mount is done by
    # cloud-init runcmd, while nvkvm-guest.service is a oneshot ordered only
    # After=local-fs.target -- so it can fire before its inputs exist and, with
    # RemainAfterExit, never retry.  Measured on instance 48241076: at mount
    # time the unit was inactive with an empty journal and no module loaded.
    # Racing validate.sh against that produced identical 7P/6F/15S runs on cards
    # that differ, which reads like a GPU verdict and is not one.
    sh(f'{S} "{G} \'sudo cloud-init status --wait\'"', timeout=1200)
    sh(f'{S} "{G} \'sudo systemctl restart nvkvm-guest.service\'"', timeout=1200)
    mod, _ = sh(f'{S} "{G} \'lsmod | grep -q nvkvm_guest && echo LOADED || echo NOMODULE\'"', timeout=90)
    if "LOADED" not in mod:
        r["status"] = "guest-module-not-loaded"
        jl, _ = sh(f'{S} "{G} \'sudo journalctl -u nvkvm-guest.service --no-pager 2>&1 | tail -25\'"', timeout=90)
        r["detail"] = jl[-1200:]
        return r

    # Which ABI profile did nvkvm actually pick for this driver?  This is the
    # single most valuable field in a driver sweep: the profile table is the
    # thing a new driver version breaks, and QEMU prints its choice at realize.
    prof, _ = sh(f"{S} \"journalctl -u nvkvm-vm --no-pager 2>/dev/null | "
                 f"grep -oE 'ABI profile [0-9]+' | tail -1\"", timeout=90)
    r["abi_profile"] = (prof.strip().split()[-1] if prof.strip() else "?")

    # stage_guest_libs.sh exits 2 when an OPTIONAL library is absent
    # (documented in setup_guest.sh), which is recorded and not fatal.  But it
    # also exits 1 when it stages NOTHING AT ALL -- no libcuda in the bundle,
    # or it refused to choose between bundles -- and the old code lumped those
    # together as "a bad rc is recorded, not fatal".  A run that staged nothing
    # then goes on to fail validate.sh for a reason that has nothing to do with
    # the GPU, which is precisely the kind of false verdict this sweep exists
    # to avoid.  Pass the bundle EXPLICITLY (removing the guess entirely) and
    # treat rc=1 as the harness failure it is.
    _stagecmd = "sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh " + f"/mnt/nvkvm/host-libs-{drv}"
    so, rc = sh(f'{S} "{G} \'{_stagecmd}\' 2>&1"', timeout=600)
    r["stage_rc"] = rc
    if rc == 1:
        r["status"] = "stage-failed"; r["detail"] = so[-1200:]; return r

    _, vrc = sh(f'{S} "{G} \'cd /mnt/nvkvm && bash tests/validate.sh --json /tmp/r.json\'"', timeout=1800)
    r["validate_rc"] = vrc
    out, _ = sh(f'{S} "{G} \'cat /tmp/r.json\'"', timeout=120)
    try:
        r["validate"] = json.loads(out)
        v = r["validate"]
        r["summary"] = f"{v.get('pass','?')}P/{v.get('fail','?')}F/{v.get('skip','?')}S"
        r["failed_checks"] = [c.get("name") for c in v.get("checks", [])
                              if c.get("status") not in ("pass", "PASS")][:8]
        # Take validate.sh's OWN verdict.  Recomputing it as fail==0 counted a
        # run with skipped checks as a success: 20P/0F/8S was reported "pass"
        # while validate.sh exits 2 for INCOMPLETE precisely so that skips are
        # not silently banked as passes.
        r["status"] = {0: "pass", 1: "fail", 2: "incomplete"}.get(vrc, f"validate-rc-{vrc}")
    except Exception:
        r["status"] = "validate-unparsed"; r["detail"] = out[-1200:]
    return r


def run_one(offer, args):
    """Rent one box, then sweep EVERY applicable driver on it.

    The expensive parts -- QEMU build, guest image -- are done once and reused
    across drivers.  That is the whole economic argument for installing drivers
    instead of shopping for them: one rental amortised over a dozen versions,
    rather than a dozen rentals hoping the market supplies variety it does not.
    """
    iid = None
    gpu = offer.get("gpu_name")
    base = {
        "gpu": gpu,
        "arch": arch_of(gpu),
        "offer_id": offer.get("id"),
        "dph": offer.get("dph_total"),
        # Kept only to document that it is NOT what ran.  See DRIVER_MATRIX.
        "driver_advertised_by_vast": offer.get("driver_version"),
        "started": datetime.datetime.now().isoformat(timespec="seconds"),
        "tree": tree_stamp(),
    }
    out_recs = []

    def bail(**kw):
        """Record a whole-box failure AND make it visible to the finally block.

        These used to `return [rec_for(...)]` directly, building a fresh list
        that out_recs never saw -- so the finally block, which stamps
        r["destroyed"] over out_recs, never touched them.  Every early failure
        (no-ssh, build-failed, not-a-vm...) therefore reported destroyed=None
        even when the instance had in fact been destroyed and verified, which
        makes the one field that must be trustworthy in a spend report look
        like an open question.
        """
        out_recs.append(rec_for("-", "-", "-", **kw))
        return out_recs

    def rec_for(ver, prof, why, **kw):
        d = dict(base); d.update({"driver": ver, "abi_profile_expected": prof,
                                  "driver_rationale": why}); d.update(kw)
        return d

    try:
        out, rc = sh(f"vastai create instance {offer['id']} --image {KVM_IMAGE} "
                     f"--disk {args.disk} --ssh --direct --label {SWEEP_LABEL}", timeout=180)
        iid = parse_contract(out)
        if not iid:
            return bail(status="create-failed", detail=out[:300])
        base["instance_id"] = iid

        host, port = wait_ssh(iid)
        if not host:
            return bail(status="no-ssh")
        S = (f"ssh -o ConnectTimeout=20 -o StrictHostKeyChecking=no "
             f"-o UserKnownHostsFile=/dev/null -p {port} root@{host}")

        kvm, _ = sh(f"{S} 'ls /dev/kvm 2>/dev/null && echo yes || echo no'", timeout=60)
        if "no" in kvm:
            return bail(status="no-kvm")   # costs cents, not hours

        # Driver swapping only works because this is a VM with the GPU passed
        # through.  Prove it rather than assume it: on a container rental the
        # module belongs to the physical host and every install below is a
        # no-op that would silently re-measure the same driver N times.
        virt, _ = sh(f"{S} 'systemd-detect-virt 2>/dev/null || echo unknown'", timeout=60)
        base["virt"] = virt.strip()
        if base["virt"] not in ("kvm", "qemu"):
            return bail(status="not-a-vm",
                            detail=f"systemd-detect-virt={base['virt']}; cannot replace the driver")

        preinstalled, _ = sh(f"{S} 'cat /proc/driver/nvidia/version 2>/dev/null | head -1'", timeout=90)
        base["driver_preinstalled"] = preinstalled.strip()[:160]

        sh(f"{S} 'systemctl stop unattended-upgrades 2>/dev/null; "
           f"systemctl mask unattended-upgrades 2>/dev/null; true'", timeout=120)
        # unattended-upgrades has twice wedged the apt resolver at 100% CPU
        # holding the dpkg lock, failing setup_guest.sh with rc=100.  That
        # reads as a `guest-failed` row about a GPU and is nothing of the kind.

        sh(f"cd {REPO} && tar --exclude-vcs-ignores --exclude=.git "
           f"--exclude=sweep-runs -czf /tmp/sweep-tree.tgz .", timeout=180)
        sh(f"scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P {port} "
           f"-q /tmp/sweep-tree.tgz root@{host}:/root/", timeout=300)
        sh(f"{S} 'mkdir -p /root/nvkvm && tar -xzf /root/sweep-tree.tgz -C /root/nvkvm'", timeout=180)

        # One-time: QEMU and the guest image do not depend on the host driver.
        # NEEDRESTART_MODE=a / DEBIAN_FRONTEND=noninteractive: on instance
        # 48254541 build_qemu.sh --install-deps died with needrestart's
        # interactive "Services to be restarted" prompt in the log tail and
        # nothing else -- recorded as build-failed, which is a harness result
        # wearing the shape of an environment one.  apt must never be able to
        # ask a question here.
        ENV = ("DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=a "
               "NEEDRESTART_SUSPEND=1 ")
        for step, cmd, tmo in [
            ("build",  f"cd /root/nvkvm && {ENV} bash scripts/build_qemu.sh --install-deps", 3600),
            ("guest",  f"cd /root/nvkvm && {ENV} bash scripts/setup_guest.sh",              2700),
        ]:
            _, rc = sh(f"{S} '{cmd} > /root/{step}.log 2>&1'", timeout=tmo)
            if rc != 0:
                errs, _ = sh(f"{S} \"grep -niE 'error|fatal|cannot|No space|Killed|undefined reference' "
                             f"/root/{step}.log | tail -25\"", timeout=60)
                tail, _ = sh(f"{S} 'tail -60 /root/{step}.log'", timeout=60)
                return bail(status=f"{step}-failed",
                                detail="ERRORS:\n" + errs[-1500:] + "\n\nTAIL:\n" + tail[-1500:])

        todo = drivers_for(gpu, args.drivers)

        # ORDER MATTERS, and getting it wrong silently destroys the baseline.
        #
        # The image's driver is DKMS-managed (nvidia-dkms-575,
        # nvidia-kernel-source-575), so purge_distro_driver() removes it for
        # good -- and 575.51.03 is NOT downloadable from NVIDIA any more.  In
        # plain DRIVER_MATRIX order 575.51.03 is tested LAST, i.e. after the
        # purge, so the one row that is a KNOWN-GOOD CONTROL would report
        # driver-install-failed on every box.  Without it there is no way to
        # separate "this old driver cannot build on this kernel" from "the
        # harness is broken", which is the whole point of having a baseline.
        #
        # So: whatever is already installed gets measured FIRST, for free,
        # before anything is purged.  Stable sort, so the rest of the matrix
        # keeps its documented order.
        cur0 = installed_driver_version(S)
        if cur0:
            todo.sort(key=lambda t: 0 if t[0] == cur0 else 1)
            if todo and todo[0][0] == cur0:
                print(f"    (testing preinstalled {cur0} first, before any purge)", flush=True)
        if not todo:
            return bail(status="no-applicable-drivers",
                            detail=f"arch={base['arch']} floor={ARCH_FLOOR.get(base['arch'])}")

        for ver, prof, why in todo:
            if stop_requested():
                print(f"    STOP requested ({STOP_FILE}) -- ending this box cleanly",
                      flush=True)
                break
            print(f"    driver {ver} (expect ABI {prof}) ...", flush=True)
            ok, detail, actual = install_driver(S, ver, base["arch"], f"/root/drv-{ver}.log")
            if not ok:
                out_recs.append(rec_for(ver, prof, why,
                                        status="driver-install-failed", detail=detail))
                continue

            # A driver older than the silicon installs fine and then finds no
            # GPU.  That is not an nvkvm result and must never be scored as one.
            smi, _ = sh(f"{S} 'nvidia-smi --query-gpu=name --format=csv,noheader 2>&1 | head -1'", timeout=120)
            if "NVIDIA" not in smi.upper() and "GEFORCE" not in smi.upper() and "RTX" not in smi.upper():
                out_recs.append(rec_for(ver, prof, why, status="driver-predates-gpu",
                                        detail=smi.strip()[:200]))
                continue

            r = boot_and_validate(S, args, actual)
            rec = rec_for(ver, prof, why, driver_installed=detail, **r)
            # Which version REALLY ran.  Differs from `driver` whenever the
            # requested row was unpublished and DRIVER_ALTS supplied a stand-in;
            # reporting the requested version alone would overstate the result.
            rec["driver_actual"] = actual
            rec["driver_substituted"] = (actual != ver)
            # Free correctness check on every single run: did nvkvm's selector
            # choose the profile the ABI table says it should for this version?
            got = rec.get("abi_profile", "?")
            rec["abi_profile_ok"] = (got == prof) if got != "?" else None
            if rec["abi_profile_ok"] is False:
                rec["abi_profile_mismatch"] = f"expected {prof}, nvkvm chose {got}"
            out_recs.append(rec)
            print(f"      -> {rec.get('status')} {rec.get('summary','')} "
                  f"ABI={got}{'' if rec['abi_profile_ok'] is not False else ' MISMATCH'}", flush=True)

        return out_recs
    except Exception as e:
        out_recs.append(rec_for("-", "-", "-", status="error", detail=str(e)[:400]))
        return out_recs
    finally:
        # Always destroy. Retried: an orphan bills until a human notices.
        if iid:
            d = destroy_verified(iid)
            for r in out_recs:
                r["destroyed"] = d
            if not d:
                print(f"  !! DESTROY FAILED for {iid} -- destroy it by hand", file=sys.stderr, flush=True)

def render(records):
    """Two tables.  The matrix is the deliverable; the detail list explains it.

    The matrix is architecture x driver rather than box x driver, because that
    is the claim the project actually wants to make: 'every ABI profile is
    exercised on every architecture'.  A cell holding a non-result (install
    failed, driver predates the silicon) is shown as such and never as a pass.
    """
    real = [r for r in records if r.get("driver") and r.get("driver") != "-"]
    if not real:
        return "(no driver runs recorded)"

    drivers = [v for v, _, _ in DRIVER_MATRIX if any(r["driver"] == v for r in real)]
    arches = []
    for r in real:
        a = r.get("arch") or "?"
        if a not in arches:
            arches.append(a)

    CELL = {"pass": "OK", "fail": "**FAIL**", "incomplete": "**INC**",
            "driver-install-failed": "inst-x", "driver-predates-gpu": "n/a",
            "guest-no-boot": "boot-x", "guest-module-not-loaded": "mod-x",
            "bundle-failed": "bundle-x", "stage-failed": "stage-x",
            "validate-unparsed": "parse-x"}

    rows = ["| architecture | " + " | ".join(drivers) + " |",
            "|---" * (len(drivers) + 1) + "|"]
    for a in arches:
        cells = []
        for d in drivers:
            hit = [r for r in real if (r.get("arch") or "?") == a and r["driver"] == d]
            if not hit:
                cells.append("–"); continue
            r = hit[-1]
            c = CELL.get(r.get("status"), r.get("status", "?"))
            if r.get("abi_profile_ok") is False:
                c += " ABI!"
            if r.get("driver_substituted"):
                c += "*"
            cells.append(c)
        rows.append(f"| {a} | " + " | ".join(cells) + " |")

    rows += ["", "Legend: OK = 28/28. **FAIL**/**INC** = validate.sh said so. "
                 "`n/a` = driver predates the silicon. `inst-x`/`boot-x`/`mod-x`/"
                 "`bundle-x` = harness or environment, NOT a GPU verdict. "
                 "`stage-x` = the guest staged NO libraries (harness). "
                 "`ABI!` = nvkvm selected a different ABI profile than "
                 "docs/reference/abi-profiles.md predicts -- always investigate. "
                 "`*` = the requested version is no longer published by NVIDIA and a "
                 "same-profile substitute ran instead (see `driver_actual`).", ""]

    rows += ["| GPU | driver | ABI (want/got) | result | detail |", "|---|---|---|---|---|"]
    for r in real:
        got = r.get("abi_profile", "?")
        want = r.get("abi_profile_expected", "?")
        abi = f"{want}/{got}" + ("" if r.get("abi_profile_ok") is not False else " **MISMATCH**")
        dv = r.get("driver")
        if r.get("driver_substituted"):
            dv = f"{dv} -> {r.get('driver_actual')}"
        rows.append(f"| {r.get('gpu','?')} | {dv} | {abi} | "
                    f"{CELL.get(r.get('status'), r.get('status'))} | "
                    f"{r.get('summary') or (r.get('detail') or '')[:80]} |")
    return "\n".join(rows)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--search", default="vms_enabled=true num_gpus=1")
    ap.add_argument("--limit", type=int, default=6)
    ap.add_argument("--max-price", type=float, default=0.60)
    ap.add_argument("--max-spend", type=float, default=5.00)
    ap.add_argument("--disk", type=int, default=64)
    ap.add_argument("--go", action="store_true", help="actually spend money (default is dry-run)")
    ap.add_argument("--include-unsupported", action="store_true",
                    help="also rent pre-Turing cards (Pascal and older). These are "
                         "documented as non-working; use this only to deliberately "
                         "re-measure that boundary, not in a coverage sweep.")
    ap.add_argument("--prefer", default="",
                    help="comma-separated substrings to rank FIRST regardless of price, "
                         "e.g. --prefer 'A100,L40,5080' -- use for dies not yet in the "
                         "tested-platforms table")
    ap.add_argument("--per-model", action="store_true",
                    help="one box per GPU model instead of per architecture. Use "
                         "when the question is die-level -- GA100 failed where "
                         "five GA10x dies passed, which is why this exists.")
    ap.add_argument("--drivers", default="",
                    help="comma-separated driver versions to install, e.g. "
                         "'570.124.06,575.51.03'.  Default: every row of "
                         "DRIVER_MATRIX applicable to the card.  Use "
                         "--list-drivers to see the matrix and why each row is "
                         "in it.")
    ap.add_argument("--list-drivers", action="store_true",
                    help="print the driver matrix and exit")
    ap.add_argument("--render", action="store_true")
    ap.add_argument("--check-tree", action="store_true",
                    help="print the tree that WOULD be shipped and exit non-zero "
                         "if it differs from HEAD. Spends nothing; safe to run "
                         "anywhere, including from another script.")
    ap.add_argument("--allow-dirty", action="store_true",
                    help="ship a working tree that differs from HEAD anyway. "
                         "For sweeping an uncommitted fix on purpose -- the "
                         "differing files are printed and recorded either way.")
    args = ap.parse_args()
    args.drivers = [x.strip() for x in args.drivers.split(",") if x.strip()]

    if args.list_drivers:
        print("driver      ABI  why this exact version is in the matrix")
        for v, p, why in DRIVER_MATRIX:
            print(f"{v:<11} {p:<4} {why}")
        print("\nArchitecture floors (below these a driver cannot see the card):")
        for a, f in ARCH_FLOOR.items():
            print(f"  {a:<10} {f}")
        return

    if args.check_tree:
        sys.exit(0 if check_tree(args.allow_dirty) else 1)

    if args.render:
        recs = json.load(open(RESULTS)) if os.path.exists(RESULTS) else []
        print(render(recs)); return

    prefer = [x.strip() for x in args.prefer.split(',') if x.strip()]
    offers = search(args.search, args.limit, args.max_price,
                    args.include_unsupported, prefer, args.per_model)
    if not offers:
        print("no offers matched"); return
    # Cost model: one build (~35 min) plus ~12 min per driver -- install,
    # re-bundle, boot, validate.  The per-driver term dominates, which is the
    # point: the build is paid once and amortised across the matrix.
    plan, est = [], 0.0
    for o in offers:
        n = len(drivers_for(o.get("gpu_name"), args.drivers))
        hours = 0.6 + 0.2 * n
        est += o.get("dph_total", 0) * hours
        plan.append((o, n, hours))
    print(f"{len(offers)} box(es), {sum(n for _, n, _ in plan)} driver runs, "
          f"estimated total ~${est:.2f}\n")
    for o, n, hours in plan:
        print(f"  {o.get('gpu_name'):<20} ${o.get('dph_total',0):<7.3f} "
              f"arch={str(arch_of(o.get('gpu_name'))):<10} {n:>2} drivers "
              f"~{hours:.1f}h  id={o.get('id')}")
        print(f"      vast advertises driver={o.get('driver_version','?')} "
              f"-- ignored; we install our own")
    if not args.go:
        print("\nDRY RUN — nothing rented. Re-run with --go to execute.")
        check_tree(allow_dirty=True)   # advisory here; nothing is being spent
        return
    # Last gate before money: is this tree the tree we think it is?
    if not check_tree(args.allow_dirty):
        sys.exit(1)
    if est > args.max_spend:
        print(f"\nREFUSING: estimate ${est:.2f} exceeds --max-spend ${args.max_spend:.2f}")
        return

    recs = json.load(open(RESULTS)) if os.path.exists(RESULTS) else []
    try:
        for o in offers:
            if stop_requested():
                print(f"\nSTOP requested ({STOP_FILE}) -- not renting any further boxes",
                      flush=True)
                break
            print(f"\n=== {o.get('gpu_name')} (offer {o.get('id')}) ===", flush=True)
            got = run_one(o, args)          # a LIST now: one record per driver
            recs.extend(got)
            os.makedirs(os.path.dirname(RESULTS), exist_ok=True)
            json.dump(recs, open(RESULTS, "w"), indent=1)   # written per box, so a crash keeps results
    finally:
        reap_strays()   # nothing labelled nvkvm-sweep may survive this function
    print("\n" + render(recs))


if __name__ == "__main__":
    main()
