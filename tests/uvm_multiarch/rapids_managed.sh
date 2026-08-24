#!/usr/bin/env bash
#
# rapids_managed.sh — RAPIDS / cuDF on top of the managed-memory fallback.
#
# This is the most realistic managed-memory consumer that can be got running in
# the guest.  cuDF does NOT use managed memory by default — RMM's default
# device resource is plain cudaMalloc — so the interesting configuration has to
# be asked for explicitly:
#
#     rmm.reinitialize(managed_memory=True)
#
# which makes every DataFrame allocation a cudaMallocManaged, i.e. every one of
# them goes through this branch's fallback.  Running cuDF in its DEFAULT
# configuration would exercise nothing on this branch and is not worth the
# download, so the script runs BOTH and compares: default (cudaMalloc) as the
# control, managed as the subject, with the same query and a numerical
# comparison against pandas on the CPU.
#
# Run INSIDE the guest.  Needs outbound network (QEMU user-mode NAT).
#
set -u
OUT="${1:-/tmp/uvm-rapids}"
mkdir -p "$OUT"
kv() { printf '@@%s=%s\n' "$1" "$2"; }

echo "=== installing RAPIDS cuDF into a venv (this is a few GB over slirp) ==="
sudo apt-get update -qq >/dev/null 2>&1
sudo apt-get install -y -qq python3-venv python3-pip > "$OUT/apt.log" 2>&1
python3 -m venv /tmp/rapids-venv >> "$OUT/apt.log" 2>&1 || { kv RAPIDS venv_failed; exit 1; }
# shellcheck disable=SC1091
. /tmp/rapids-venv/bin/activate
pip install -q --upgrade pip >> "$OUT/pip.log" 2>&1
if ! pip install -q --extra-index-url=https://pypi.nvidia.com \
        "cudf-cu12>=24.10" pandas >> "$OUT/pip.log" 2>&1; then
    kv RAPIDS pip_failed
    tail -30 "$OUT/pip.log"
    exit 1
fi
kv RAPIDS_INSTALL ok
pip list 2>/dev/null | grep -iE 'cudf|rmm|cuda' | head -20

cat > /tmp/rapids_managed.py <<'PY'
"""cuDF over MANAGED memory vs cuDF over plain device memory, both checked
against pandas.  Under the fallback, `managed_memory=True` routes every RMM
allocation through cudaMallocManaged, so this is a real application's
allocator sitting directly on the branch under test."""
import sys, time, traceback

import numpy as np
import pandas as pd


def workload(xp, tag):
    """A join + groupby + sort — the things a dataframe library actually does,
    and enough separate buffers that a bad mapping cannot hide."""
    n = 2_000_000
    rng = np.random.default_rng(1234)
    keys = rng.integers(0, 5000, n)
    vals = rng.random(n)
    left = xp.DataFrame({"k": keys, "v": vals})
    right = xp.DataFrame({"k": np.arange(5000), "w": np.arange(5000) * 0.5})

    t0 = time.time()
    j = left.merge(right, on="k")
    g = j.groupby("k").agg({"v": "sum", "w": "max"}).reset_index()
    g = g.sort_values("k")
    dt = time.time() - t0

    out = g.to_pandas() if hasattr(g, "to_pandas") else g
    print(f"[{tag}] rows={len(out)} vsum={out['v'].sum():.6f} "
          f"wmax={out['w'].max():.6f} seconds={dt:.2f}")
    return out.reset_index(drop=True), dt


def main():
    ref, _ = workload(pd, "pandas-cpu")

    import rmm
    import cudf

    results = {}
    for tag, managed in (("cudf-device", False), ("cudf-managed", True)):
        try:
            rmm.reinitialize(managed_memory=managed, pool_allocator=False)
            print(f"@@RMM_RESOURCE_{tag}={type(rmm.mr.get_current_device_resource()).__name__}")
            got, dt = workload(cudf, tag)
            same = (len(got) == len(ref)
                    and np.allclose(np.sort(got["v"].to_numpy()),
                                    np.sort(ref["v"].to_numpy()), rtol=1e-9, atol=1e-9)
                    and np.allclose(np.sort(got["w"].to_numpy()),
                                    np.sort(ref["w"].to_numpy()), rtol=1e-9, atol=1e-9))
            results[tag] = ("CORRECT" if same else "WRONG_ANSWERS", dt)
            print(f"@@RAPIDS_{tag}={results[tag][0]} seconds={dt:.2f}")
        except Exception:
            traceback.print_exc()
            print(f"@@RAPIDS_{tag}=EXCEPTION")
            results[tag] = ("EXCEPTION", -1)

    ok = results.get("cudf-managed", ("", 0))[0] == "CORRECT"
    print("@@RAPIDS_RESULT=%s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


sys.exit(main())
PY

echo "=== running cuDF (device) and cuDF (managed) against a pandas reference ==="
python3 /tmp/rapids_managed.py 2>&1 | tee "$OUT/rapids.log"
kv RAPIDS_RC "${PIPESTATUS[0]}"
