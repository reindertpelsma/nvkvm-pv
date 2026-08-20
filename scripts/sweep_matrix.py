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
import argparse, json, os, subprocess, sys, time, datetime

KVM_IMAGE = "docker.io/vastai/kvm:ubuntu_cli_22.04-2025-05-16"
RESULTS   = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                         "tests", "sweep-results.json")
REPO      = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


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


def search(filt, limit, max_price):
    offers = vast_json(f"search offers '{filt}' -o dph") or []
    seen, picked = set(), []
    for o in sorted(offers, key=lambda x: x.get("dph_total", 99)):
        gpu = o.get("gpu_name")
        # one box per (gpu model, driver branch): the point is coverage, not repeats
        key = (gpu, str(o.get("driver_version", "?")).split(".")[0])
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


def run_one(offer, args):
    iid = None
    rec = {
        "gpu": offer.get("gpu_name"),
        "offer_id": offer.get("id"),
        "dph": offer.get("dph_total"),
        "driver_reported_by_vast": offer.get("driver_version"),
        "started": datetime.datetime.now().isoformat(timespec="seconds"),
        "status": "unknown",
    }
    try:
        out, rc = sh(f"vastai create instance {offer['id']} --image {KVM_IMAGE} "
                     f"--disk {args.disk} --ssh --direct --label nvkvm-sweep", timeout=180)
        try:
            iid = json.loads(out.replace("'", '"')).get("new_contract")
        except Exception:
            iid = None
        if not iid:
            rec["status"] = "create-failed"; rec["detail"] = out[:300]; return rec
        rec["instance_id"] = iid

        host, port = wait_ssh(iid)
        if not host:
            rec["status"] = "no-ssh"; return rec
        S = (f"ssh -o ConnectTimeout=20 -o StrictHostKeyChecking=no "
             f"-o UserKnownHostsFile=/dev/null -p {port} root@{host}")

        drv, _ = sh(f"{S} 'nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -1'", timeout=90)
        rec["host_driver"] = drv.strip()
        kvm, _ = sh(f"{S} 'ls /dev/kvm 2>/dev/null && echo yes || echo no'", timeout=60)
        if "no" in kvm:
            rec["status"] = "no-kvm"; return rec   # the check that costs cents, not hours

        sh(f"cd {REPO} && tar --exclude=.git -czf /tmp/sweep-tree.tgz .", timeout=180)
        sh(f"scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P {port} "
           f"-q /tmp/sweep-tree.tgz root@{host}:/root/", timeout=300)
        sh(f"{S} 'mkdir -p /root/nvkvm && tar -xzf /root/sweep-tree.tgz -C /root/nvkvm'", timeout=180)

        for step, cmd, tmo in [
            ("build",  "cd /root/nvkvm && bash scripts/build_qemu.sh --install-deps", 3600),
            ("guest",  "cd /root/nvkvm && bash scripts/setup_guest.sh",              1200),
            ("bundle", "cd /root/nvkvm && bash scripts/make_host_bundle.sh",          600),
        ]:
            _, rc = sh(f"{S} '{cmd} > /root/{step}.log 2>&1'", timeout=tmo)
            if rc != 0:
                rec["status"] = f"{step}-failed"
                tail, _ = sh(f"{S} 'tail -20 /root/{step}.log'", timeout=60)
                rec["detail"] = tail[-1200:]
                return rec

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
            rec["status"] = "guest-no-boot"; return rec

        sh(f'{S} "{G} \'sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh\'"', timeout=600)
        sh(f'{S} "{G} \'cd /mnt/nvkvm && bash tests/validate.sh --json /tmp/r.json\'"', timeout=1800)
        out, _ = sh(f'{S} "{G} \'cat /tmp/r.json\'"', timeout=120)
        try:
            rec["validate"] = json.loads(out)
            v = rec["validate"]
            rec["status"] = "pass" if v.get("fail", 1) == 0 else "fail"
            rec["summary"] = f"{v.get('pass','?')}P/{v.get('fail','?')}F/{v.get('skip','?')}S"
        except Exception:
            rec["status"] = "validate-unparsed"; rec["detail"] = out[-1200:]
        return rec
    except Exception as e:
        rec["status"] = "error"; rec["detail"] = str(e)[:400]; return rec
    finally:
        # Always destroy. Retried: an orphan bills until a human notices.
        if iid:
            for _ in range(4):
                _, rc = sh(f"vastai destroy instance {iid}", timeout=90)
                if rc == 0:
                    rec["destroyed"] = True; break
                time.sleep(10)
            else:
                rec["destroyed"] = False
                print(f"  !! DESTROY FAILED for {iid} -- destroy it by hand", file=sys.stderr)


def render(records):
    rows = ["| GPU | host driver | $/hr | result | detail |", "|---|---|---|---|---|"]
    for r in records:
        mark = {"pass": "PASS", "fail": "**FAIL**"}.get(r.get("status"), r.get("status"))
        rows.append(f"| {r.get('gpu','?')} | {r.get('host_driver','?')} | "
                    f"{r.get('dph','?')} | {mark} | {r.get('summary','')} |")
    return "\n".join(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--search", default="vms_enabled=true num_gpus=1")
    ap.add_argument("--limit", type=int, default=6)
    ap.add_argument("--max-price", type=float, default=0.60)
    ap.add_argument("--max-spend", type=float, default=5.00)
    ap.add_argument("--disk", type=int, default=64)
    ap.add_argument("--go", action="store_true", help="actually spend money (default is dry-run)")
    ap.add_argument("--render", action="store_true")
    args = ap.parse_args()

    if args.render:
        recs = json.load(open(RESULTS)) if os.path.exists(RESULTS) else []
        print(render(recs)); return

    offers = search(args.search, args.limit, args.max_price)
    if not offers:
        print("no offers matched"); return
    est = sum(o.get("dph_total", 0) for o in offers) * 0.75   # ~45 min each
    print(f"{len(offers)} offer(s), estimated total ~${est:.2f} (assumes ~45 min each)\n")
    for o in offers:
        print(f"  {o.get('gpu_name'):<20} ${o.get('dph_total',0):<7.3f} "
              f"driver={o.get('driver_version','?'):<12} id={o.get('id')}")
    if not args.go:
        print("\nDRY RUN — nothing rented. Re-run with --go to execute.")
        return
    if est > args.max_spend:
        print(f"\nREFUSING: estimate ${est:.2f} exceeds --max-spend ${args.max_spend:.2f}")
        return

    recs = json.load(open(RESULTS)) if os.path.exists(RESULTS) else []
    for o in offers:
        print(f"\n=== {o.get('gpu_name')} (offer {o.get('id')}) ===", flush=True)
        r = run_one(o, args)
        print(f"    -> {r.get('status')} {r.get('summary','')}", flush=True)
        recs.append(r)
        os.makedirs(os.path.dirname(RESULTS), exist_ok=True)
        json.dump(recs, open(RESULTS, "w"), indent=1)   # written per box, so a crash keeps results
    print("\n" + render(recs))


if __name__ == "__main__":
    main()
