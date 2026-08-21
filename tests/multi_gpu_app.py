#!/usr/bin/env python3
"""multi_gpu_app.py — a real multi-GPU workload that does not need NCCL.

tests/nccl_allreduce.py is the collective-library workload, but NCCL needs a
working bootstrap and a usable inter-GPU transport, and neither is a given on
a nested-virt box with consumer cards. This probe covers the same ground that
matters for nvkvm -- genuine compute on every GPU plus real cross-device data
movement -- using only torch's own copy paths, so it still produces a verdict
when NCCL cannot start.

Correctness is by value, not by "it ran":
  - each GPU multiplies constant matrices chosen so the exact fp32 product is
    known in closed form, and every element of the reduction is compared to it;
  - every ordered GPU pair round-trips a 4 MiB tensor tagged with its source
    ordinal, so a misrouted or dropped transfer cannot compare equal.

Run it identically on the bare-metal host and in the guest and diff.
"""
import sys
import time

import torch


def main():
    n = torch.cuda.device_count()
    print(f"INFO|devices|{n}", flush=True)
    for i in range(n):
        p = torch.cuda.get_device_properties(i)
        print(f"INFO|gpu{i}|{p.name} sm_{p.major}{p.minor} {p.total_memory >> 20}MiB",
              flush=True)
    if n < 2:
        print("CHECK|multi_gpu|SKIP|need >=2 GPUs", flush=True)
        return 2

    S = 2048
    # Constant matrices: A[i] is all (i+1), B[i] is all (2i+1), so every element
    # of A[i] @ B[i] is exactly S*(i+1)*(2i+1) -- integer-valued in fp32, so an
    # exact comparison is legitimate rather than a tolerance fudge.
    A = [torch.full((S, S), float(i + 1), device=f"cuda:{i}") for i in range(n)]
    B = [torch.full((S, S), float(2 * i + 1), device=f"cuda:{i}") for i in range(n)]

    for i in range(n):
        torch.cuda.synchronize(i)
    t0 = time.time()
    partials = [A[i] @ B[i] for i in range(n)]
    for i in range(n):
        torch.cuda.synchronize(i)
    t_mm = time.time() - t0

    # Cross-device movement: pull every partial onto GPU 0 and reduce.
    t0 = time.time()
    acc = torch.zeros(S, S, device="cuda:0")
    for i in range(n):
        acc += partials[i].to("cuda:0")
    torch.cuda.synchronize(0)
    t_mv = time.time() - t0

    expect = float(sum(S * (i + 1) * (2 * i + 1) for i in range(n)))
    ok = bool(torch.all(acc == expect).item())
    got = acc[0, 0].item()
    status = "PASS" if ok else "FAIL"
    print(f"CHECK|multi_gpu_matmul_reduce|{status}|"
          f"{n} GPUs, {S}x{S} fp32, every element == {expect:.0f} (got {got:.0f}), "
          f"matmul {t_mm * 1000:.0f} ms, cross-device reduce {t_mv * 1000:.0f} ms",
          flush=True)

    # Byte-exact round trip over every ordered pair, tagged by source ordinal.
    fails = 0
    for a in range(n):
        for b in range(n):
            if a == b:
                continue
            src = torch.arange(1 << 20, device=f"cuda:{a}", dtype=torch.int32) + (a << 24)
            back = src.to(f"cuda:{b}").to(f"cuda:{a}")
            if not torch.equal(src, back):
                print(f"CHECK|xfer_{a}_to_{b}|FAIL|round-trip mismatch", flush=True)
                fails += 1
    if not fails:
        print(f"CHECK|torch_cross_device_xfer|PASS|{n * (n - 1)} ordered pairs, "
              f"4 MiB each, byte-exact round trip", flush=True)

    bad = (0 if ok else 1) + fails
    print(f"VERDICT|{'PASS' if bad == 0 else 'FAIL'}", flush=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
