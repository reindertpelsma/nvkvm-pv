#!/usr/bin/env python3
"""Build an explicit, up-front sweep plan: which node tests which drivers.

WHY THIS EXISTS
---------------
The old selection handed every host the SAME preset list, which broke two
rules the sweep is supposed to honour:

  1. Never test two drivers from the SAME ABI boundary on one host. Within a
     boundary the layouts are byte-identical, so the second driver re-tests
     what the first already proved and spends a host slot doing it. The
     boundary preset put 580.95.05, 590.48.01 and 595.84 -- all ABI_580 -- on
     one host.

  2. Every host should test a DISTINCT set of exact versions. Renting a host is
     the expensive part; which versions it installs is free. So across hosts we
     should walk the whole published set rather than re-testing the same six
     versions on every box.

  3. Every ABI boundary must still be covered on every architecture. Rules 1
     and 2 spread versions out; this one stops that spreading from leaving a
     boundary untested.

  4. Vary the nvkvm GUEST kernel too. The guest kernel is a real axis -- our
     module is built against it -- and it has never been swept.

So the plan is computed ONCE, up front, and printed. A run then executes it.
That also makes requeueing tractable: when a fix lands mid-sweep, the nodes to
redo are exactly the failed ones plus the ones not yet started, and you can
point at them by name.
"""
import argparse, importlib.util, json, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# Rule 4: vary the GUEST kernel. Our module is compiled against it on every
# boot, and it has never been swept. The cheapest real lever is the Ubuntu
# cloud image series -- each ships a different kernel -- so these are release
# names, not kernel versions, and sweep.sh --guest-image takes them directly.
# VERIFIED 2026-08-30 that each of these resolves at cloud-images.ubuntu.com;
# oracular and plucky are 404 and are deliberately absent.
GUEST_IMAGES = ["noble", "jammy", "questing", "resolute"]


def load_matrix():
    spec = importlib.util.spec_from_file_location("m", os.path.join(HERE, "sweep_matrix.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def build_abiq():
    """Compile the ABI oracle out of the header under test.

    Deliberately the code's own answer rather than a copy of the table: if
    nvkvm_abi.h changes, the plan changes with it and cannot silently drift.
    """
    d = tempfile.mkdtemp(prefix="nvkvm-plan-abiq.")
    src, out = os.path.join(d, "a.c"), os.path.join(d, "abiq")
    with open(src, "w") as f:
        f.write('#include <stdio.h>\n#include <stdlib.h>\n#include "common/nvkvm_abi.h"\n'
                'int main(int c,char**v){unsigned a=0,b=~0u,p=~0u;if(c>1){char*s=v[1];'
                'a=strtoul(s,&s,10);if(*s==46){s++;b=strtoul(s,&s,10);}'
                'if(*s==46){s++;p=strtoul(s,&s,10);}}'
                'printf("%u\\n",nvkvm_abi_id_for_version(a,b,p));return 0;}\n')
    if subprocess.call(["cc", "-I", os.path.join(REPO, "src"), "-o", out, src],
                       stderr=subprocess.DEVNULL) != 0:
        sys.exit("cannot compile the ABI oracle from src/common/nvkvm_abi.h")
    return out


def profile_of(abiq, ver):
    try:
        return int(subprocess.check_output([abiq, ver], text=True).strip())
    except Exception:
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tags", default="/workspace/ogkm-supported-tags.txt")
    ap.add_argument("--arches", default="ampere,turing,ada,blackwell,hopper")
    ap.add_argument("--nodes-per-arch", type=int, default=1,
                    help="how many hosts to plan per architecture")
    ap.add_argument("--used", default="",
                    help="file of versions already tested in earlier runs; they "
                         "are skipped so successive sweeps walk the whole set")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    m = load_matrix()
    abiq = build_abiq()

    tags = [l.strip() for l in open(a.tags) if l.strip()]
    used = set()
    if a.used and os.path.exists(a.used):
        used = {l.strip() for l in open(a.used) if l.strip()}

    # version -> ABI profile, straight from the header under test
    by_profile = {}
    for t in tags:
        p = profile_of(abiq, t)
        by_profile.setdefault(p, []).append(t)
    for p in by_profile:
        by_profile[p].sort(key=lambda v: [int(x) for x in v.split(".")])

    plan, taken = [], set(used)
    node = 0
    for arch in a.arches.split(","):
        floor = m.ARCH_FLOOR.get(arch, 0)
        # Rule 3: every boundary this arch can reach must appear.
        profiles = sorted(p for p in by_profile
                          if any(int(v.split(".")[0]) >= floor for v in by_profile[p]))
        for n in range(a.nodes_per_arch):
            node += 1
            drivers = []
            for p in profiles:
                # Rule 1: exactly ONE version per boundary on this host.
                # Rule 2: prefer one nothing has tested yet.
                cands = [v for v in by_profile[p] if int(v.split(".")[0]) >= floor]
                fresh = [v for v in cands if v not in taken]
                pick = (fresh or cands)
                if not pick:
                    continue
                v = pick[n % len(pick)] if fresh else pick[-1]
                taken.add(v)
                drivers.append((v, p, "fresh" if v in fresh else "REUSED (set exhausted)"))
            plan.append({
                "node": node,
                "arch": arch,
                "guest_image": GUEST_IMAGES[(node - 1) % len(GUEST_IMAGES)],
                "drivers": [d[0] for d in drivers],
                "boundaries": [d[1] for d in drivers],
                "notes": [d[2] for d in drivers],
            })

    if a.json:
        print(json.dumps(plan, indent=2)); return

    print(f"sweep plan: {len(plan)} node(s), "
          f"{sum(len(p['drivers']) for p in plan)} driver run(s)")
    print(f"  {len(tags)} published tags; {len(taken - used)} newly claimed this plan; "
          f"{len(tags) - len(taken)} still untested after it\n")
    for p in plan:
        print(f"  node {p['node']}: {p['arch']:10s} guest-image={p['guest_image']}")
        for v, b, note in zip(p["drivers"], p["boundaries"], p["notes"]):
            flag = "" if note == "fresh" else f"   <- {note}"
            print(f"      {v:14s} ABI {b}{flag}")
        # Rule 1 is a property of the plan, so assert it here rather than hope.
        if len(set(p["boundaries"])) != len(p["boundaries"]):
            sys.exit(f"BUG: node {p['node']} has two drivers in one ABI boundary")
    print("\n  rule 1 (one driver per ABI boundary per node): asserted above")
    print("  rule 2 (distinct exact versions across nodes):  enforced by the claimed set")
    print("  rule 3 (every boundary covered per arch):       one entry per boundary per node")
    print("  rule 4 (guest kernel varied):                   via --guest-image, rotating")


if __name__ == "__main__":
    main()
