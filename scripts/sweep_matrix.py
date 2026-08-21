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
"""
import argparse, json, os, re, subprocess, sys, time, datetime

KVM_IMAGE = "docker.io/vastai/kvm:ubuntu_cli_22.04-2025-05-16"
SWEEP_LABEL = "nvkvm-sweep"
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


def driver_urls(ver, kernel_open):
    """NVIDIA publishes datacenter and consumer drivers under different paths,
    and neither is a superset.  Try both; the first that 404s is not an error."""
    return [
        f"https://us.download.nvidia.com/tesla/{ver}/NVIDIA-Linux-x86_64-{ver}.run",
        f"https://us.download.nvidia.com/XFree86/Linux-x86_64/{ver}/NVIDIA-Linux-x86_64-{ver}.run",
    ]


def install_driver(S, ver, arch, log):
    """Replace the host driver in the rented VM.  Returns (ok, detail).

    This is only possible because the KVM template is a real VM with the GPU
    passed through -- the NVIDIA module is loaded inside the instance, so it is
    ours to replace.  (`systemd-detect-virt` returns `kvm`; on a CUDA-container
    rental the module belongs to the physical host and none of this works.)
    """
    kernel_open = arch in OPEN_MODULE_ARCHES
    fetched = False
    for url in driver_urls(ver, kernel_open):
        _, rc = sh(f"{S} 'curl -fsSL -o /root/drv.run {url} && chmod +x /root/drv.run'", timeout=900)
        if rc == 0:
            fetched = True
            break
    if not fetched:
        return False, f"no installer published at either path for {ver}"

    # Nothing may hold the device or the module cannot be unloaded.
    sh(f"{S} 'systemctl stop nvkvm-vm 2>/dev/null; pkill -f qemu-system-x86_64 2>/dev/null; "
       f"nvidia-smi --gpu-reset 2>/dev/null; true'", timeout=180)
    sh(f"{S} 'rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia 2>/dev/null; true'", timeout=180)

    mod = "-m=kernel-open" if kernel_open else "-m=kernel"
    flags = (f"--silent --no-questions --ui=none --disable-nouveau "
             f"--no-cc-version-check --install-libglvnd {mod}")
    _, rc = sh(f"{S} '/root/drv.run {flags} > /root/drvinstall.log 2>&1'", timeout=2400)
    if rc != 0:
        # Retry with the other module flavour before believing the version is
        # unusable -- the open/proprietary split moved over these branches and
        # our arch guess is a heuristic, not an oracle.
        other = "-m=kernel" if kernel_open else "-m=kernel-open"
        _, rc = sh(f"{S} '/root/drv.run {flags.replace(mod, other)} "
                   f"> /root/drvinstall.log 2>&1'", timeout=2400)
    if rc != 0:
        tail, _ = sh(f"{S} 'tail -25 /root/drvinstall.log'", timeout=60)
        return False, tail[-1200:]

    sh(f"{S} 'modprobe nvidia; modprobe nvidia_uvm; true'", timeout=180)
    got, _ = sh(f"{S} 'cat /proc/driver/nvidia/version 2>/dev/null | head -1'", timeout=60)
    if ver not in got:
        return False, f"installed but /proc reports: {got.strip()[:200]}"
    return True, got.strip()



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


def wait_ssh(iid, budget=900):
    """Poll until the instance reports running AND accepts ssh."""
    deadline = time.time() + budget
    while time.time() < deadline:
        d = vast_json(f"show instance {iid}") or {}
        if d.get("actual_status") == "running":
            host = d.get("public_ipaddr")
            port = (d.get("ports") or {}).get("22/tcp")
            port = port[0]["HostPort"] if port else d.get("ssh_port")
            if host and port:
                out, rc = sh(f"ssh -o ConnectTimeout=12 -o StrictHostKeyChecking=no "
                             f"-o UserKnownHostsFile=/dev/null -p {port} root@{host} 'echo OK'", timeout=40)
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
    strays = [i for i in data if (i.get("label") or "") == SWEEP_LABEL]
    for i in strays:
        iid = i.get("id")
        print(f"  !! STRAY {iid} ({i.get('gpu_name')}) alive -- destroying", file=sys.stderr, flush=True)
        if destroy_verified(iid):
            print(f"     destroyed {iid}", file=sys.stderr, flush=True)
        else:
            print(f"     !! COULD NOT DESTROY {iid} -- DESTROY BY HAND", file=sys.stderr, flush=True)
    return [i.get("id") for i in strays]


def boot_and_validate(S, args):
    """Boot the nvkvm guest, stage libs against the CURRENT host driver, run
    validate.sh.  Called once per driver, so everything here must be repeatable.

    Returns a dict; never raises."""
    r = {}

    # The bundle is the host driver's userspace, so it MUST be rebuilt after
    # every swap.  Staging a 580 bundle against a 535 kernel module would fail
    # in ways that look like an nvkvm bug and are not.
    _, rc = sh(f"{S} 'cd /root/nvkvm && bash scripts/make_host_bundle.sh > /root/bundle.log 2>&1'", timeout=600)
    if rc != 0:
        tail, _ = sh(f"{S} 'tail -30 /root/bundle.log'", timeout=60)
        r["status"] = "bundle-failed"; r["detail"] = tail[-1200:]; return r

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

    # stage_guest_libs.sh exits non-zero when an OPTIONAL library is absent
    # (documented in setup_guest.sh), so a bad rc is recorded, not fatal.
    _, r["stage_rc"] = sh(f'{S} "{G} \'sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh\'"', timeout=600)

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

    def rec_for(ver, prof, why, **kw):
        d = dict(base); d.update({"driver": ver, "abi_profile_expected": prof,
                                  "driver_rationale": why}); d.update(kw)
        return d

    try:
        out, rc = sh(f"vastai create instance {offer['id']} --image {KVM_IMAGE} "
                     f"--disk {args.disk} --ssh --direct --label {SWEEP_LABEL}", timeout=180)
        iid = parse_contract(out)
        if not iid:
            return [rec_for("-", "-", "-", status="create-failed", detail=out[:300])]
        base["instance_id"] = iid

        host, port = wait_ssh(iid)
        if not host:
            return [rec_for("-", "-", "-", status="no-ssh")]
        S = (f"ssh -o ConnectTimeout=20 -o StrictHostKeyChecking=no "
             f"-o UserKnownHostsFile=/dev/null -p {port} root@{host}")

        kvm, _ = sh(f"{S} 'ls /dev/kvm 2>/dev/null && echo yes || echo no'", timeout=60)
        if "no" in kvm:
            return [rec_for("-", "-", "-", status="no-kvm")]   # costs cents, not hours

        # Driver swapping only works because this is a VM with the GPU passed
        # through.  Prove it rather than assume it: on a container rental the
        # module belongs to the physical host and every install below is a
        # no-op that would silently re-measure the same driver N times.
        virt, _ = sh(f"{S} 'systemd-detect-virt 2>/dev/null || echo unknown'", timeout=60)
        base["virt"] = virt.strip()
        if base["virt"] not in ("kvm", "qemu"):
            return [rec_for("-", "-", "-", status="not-a-vm",
                            detail=f"systemd-detect-virt={base['virt']}; cannot replace the driver")]

        preinstalled, _ = sh(f"{S} 'cat /proc/driver/nvidia/version 2>/dev/null | head -1'", timeout=90)
        base["driver_preinstalled"] = preinstalled.strip()[:160]

        sh(f"{S} 'systemctl stop unattended-upgrades 2>/dev/null; "
           f"systemctl mask unattended-upgrades 2>/dev/null; true'", timeout=120)
        # unattended-upgrades has twice wedged the apt resolver at 100% CPU
        # holding the dpkg lock, failing setup_guest.sh with rc=100.  That
        # reads as a `guest-failed` row about a GPU and is nothing of the kind.

        sh(f"cd {REPO} && tar --exclude=.git -czf /tmp/sweep-tree.tgz .", timeout=180)
        sh(f"scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P {port} "
           f"-q /tmp/sweep-tree.tgz root@{host}:/root/", timeout=300)
        sh(f"{S} 'mkdir -p /root/nvkvm && tar -xzf /root/sweep-tree.tgz -C /root/nvkvm'", timeout=180)

        # One-time: QEMU and the guest image do not depend on the host driver.
        for step, cmd, tmo in [
            ("build",  "cd /root/nvkvm && bash scripts/build_qemu.sh --install-deps", 3600),
            ("guest",  "cd /root/nvkvm && bash scripts/setup_guest.sh",              1200),
        ]:
            _, rc = sh(f"{S} '{cmd} > /root/{step}.log 2>&1'", timeout=tmo)
            if rc != 0:
                errs, _ = sh(f"{S} \"grep -niE 'error|fatal|cannot|No space|Killed|undefined reference' "
                             f"/root/{step}.log | tail -25\"", timeout=60)
                tail, _ = sh(f"{S} 'tail -60 /root/{step}.log'", timeout=60)
                return [rec_for("-", "-", "-", status=f"{step}-failed",
                                detail="ERRORS:\n" + errs[-1500:] + "\n\nTAIL:\n" + tail[-1500:])]

        todo = drivers_for(gpu, args.drivers)
        if not todo:
            return [rec_for("-", "-", "-", status="no-applicable-drivers",
                            detail=f"arch={base['arch']} floor={ARCH_FLOOR.get(base['arch'])}")]

        for ver, prof, why in todo:
            print(f"    driver {ver} (expect ABI {prof}) ...", flush=True)
            ok, detail = install_driver(S, ver, base["arch"], f"/root/drv-{ver}.log")
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

            r = boot_and_validate(S, args)
            rec = rec_for(ver, prof, why, driver_installed=detail, **r)
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
            "bundle-failed": "bundle-x"}

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
            cells.append(c)
        rows.append(f"| {a} | " + " | ".join(cells) + " |")

    rows += ["", "Legend: OK = 28/28. **FAIL**/**INC** = validate.sh said so. "
                 "`n/a` = driver predates the silicon. `inst-x`/`boot-x`/`mod-x`/"
                 "`bundle-x` = harness or environment, NOT a GPU verdict. "
                 "`ABI!` = nvkvm selected a different ABI profile than "
                 "docs/reference/abi-profiles.md predicts -- always investigate.", ""]

    rows += ["| GPU | driver | ABI (want/got) | result | detail |", "|---|---|---|---|---|"]
    for r in real:
        got = r.get("abi_profile", "?")
        want = r.get("abi_profile_expected", "?")
        abi = f"{want}/{got}" + ("" if r.get("abi_profile_ok") is not False else " **MISMATCH**")
        rows.append(f"| {r.get('gpu','?')} | {r.get('driver')} | {abi} | "
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
        return
    if est > args.max_spend:
        print(f"\nREFUSING: estimate ${est:.2f} exceeds --max-spend ${args.max_spend:.2f}")
        return

    recs = json.load(open(RESULTS)) if os.path.exists(RESULTS) else []
    try:
        for o in offers:
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
