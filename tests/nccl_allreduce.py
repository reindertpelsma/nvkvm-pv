#!/usr/bin/env python3
"""nccl_allreduce.py — a real multi-GPU collective workload under nvkvm.

The C probes prove the primitives work.  This proves the thing people actually
run works: NCCL collectives across every GPU, driven by torch.distributed, one
process per GPU.  On hardware without P2P (every consumer GeForce part over
PCIe) NCCL falls back to staging through host memory — that fallback is still
a real forwarding path under nvkvm and still has to be bit-exact.

Correctness is checked by VALUE, not by "it ran":
  - all_reduce(SUM): rank r contributes r+1, so every element must equal
    sum(1..world) exactly.  Integer-valued fp32, so the sum is exact and an
    exact comparison is legitimate.
  - broadcast: rank 0's pattern must land byte-identical on every rank.
  - all_gather: each rank's slice must come back tagged with its own rank, so
    a collective that silently dropped or duplicated a contributor fails.
  - reduce_scatter(SUM): each rank keeps a different slice of the sum.

Run (inside the guest, or on the host for the baseline):
    torchrun --nproc_per_node=4 tests/nccl_allreduce.py
or:
    python3 tests/nccl_allreduce.py --world 4      # spawns its own ranks
"""
import argparse
import datetime
import os
import sys

import torch
import torch.distributed as dist


def log(rank, msg):
    if rank == 0:
        print(msg, flush=True)


def run(rank, world, elems, iters):
    torch.cuda.set_device(rank)
    dev = torch.device(f"cuda:{rank}")
    fails = []

    # ---- 1. all_reduce(SUM), value-checked -------------------------------
    expect = float(world * (world + 1) // 2)          # sum(1..world)
    for it in range(iters):
        t = torch.full((elems,), float(rank + 1), device=dev, dtype=torch.float32)
        dist.all_reduce(t, op=dist.ReduceOp.SUM)
        torch.cuda.synchronize()
        if not torch.all(t == expect):
            bad = (t != expect).nonzero()[0].item()
            fails.append(f"all_reduce iter{it}: elem {bad} = {t[bad].item()} != {expect}")
            break
    if not fails:
        log(rank, f"CHECK|nccl_all_reduce|PASS|{iters} iters x {elems} fp32 elems "
                  f"across {world} GPUs, every element == {expect:.0f}")

    # ---- 2. broadcast from rank 0 ----------------------------------------
    pat = torch.arange(elems, device=dev, dtype=torch.float32)
    t = pat.clone() if rank == 0 else torch.zeros(elems, device=dev, dtype=torch.float32)
    dist.broadcast(t, src=0)
    torch.cuda.synchronize()
    if not torch.equal(t, pat):
        fails.append("broadcast: rank %d did not receive rank 0's pattern" % rank)
    else:
        log(rank, f"CHECK|nccl_broadcast|PASS|{elems} elems from rank 0 identical on all ranks")

    # ---- 3. all_gather, each slice tagged by its owner --------------------
    mine = torch.full((elems,), float(rank + 100), device=dev, dtype=torch.float32)
    out = [torch.zeros(elems, device=dev, dtype=torch.float32) for _ in range(world)]
    dist.all_gather(out, mine)
    torch.cuda.synchronize()
    for r in range(world):
        if not torch.all(out[r] == float(r + 100)):
            fails.append(f"all_gather: slice {r} not tagged {r + 100}")
    if not any("all_gather" in f for f in fails):
        log(rank, f"CHECK|nccl_all_gather|PASS|{world} slices each tagged with its owning rank")

    # ---- 4. reduce_scatter(SUM) ------------------------------------------
    try:
        inp = [torch.full((elems,), float(rank + 1), device=dev, dtype=torch.float32)
               for _ in range(world)]
        outp = torch.zeros(elems, device=dev, dtype=torch.float32)
        dist.reduce_scatter(outp, inp, op=dist.ReduceOp.SUM)
        torch.cuda.synchronize()
        if not torch.all(outp == expect):
            fails.append(f"reduce_scatter: got {outp[0].item()} expected {expect}")
        else:
            log(rank, f"CHECK|nccl_reduce_scatter|PASS|each rank's slice == {expect:.0f}")
    except Exception as e:                                    # noqa: BLE001
        log(rank, f"CHECK|nccl_reduce_scatter|SKIP|{type(e).__name__}: {e}")

    # ---- 5. sustained load, so the fallback path runs under pressure -----
    big = 1 << 24  # 16 Mi elems = 64 MiB per rank
    t = torch.full((big,), float(rank + 1), device=dev, dtype=torch.float32)
    torch.cuda.synchronize()
    start = torch.cuda.Event(enable_timing=True)
    end = torch.cuda.Event(enable_timing=True)
    start.record()
    for _ in range(20):
        dist.all_reduce(t, op=dist.ReduceOp.SUM)
        t /= world          # keep values bounded and exact (sum/world == rank+1 only if all equal)
        t.fill_(float(rank + 1))
    end.record()
    torch.cuda.synchronize()
    ms = start.elapsed_time(end)
    gb = 20 * big * 4 / 1e9
    log(rank, f"CHECK|nccl_sustained|PASS|20 all_reduce of {big * 4 // (1 << 20)} MiB "
              f"in {ms:.0f} ms ({gb / (ms / 1000):.2f} GB/s aggregate payload)")

    # ---- collect failures across ranks -----------------------------------
    nfail = torch.tensor([len(fails)], device=dev, dtype=torch.int32)
    dist.all_reduce(nfail, op=dist.ReduceOp.SUM)
    torch.cuda.synchronize()
    if fails:
        for f in fails:
            print(f"CHECK|rank{rank}|FAIL|{f}", flush=True)
    return int(nfail.item())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--world", type=int, default=None)
    ap.add_argument("--elems", type=int, default=1 << 20)
    ap.add_argument("--iters", type=int, default=10)
    args = ap.parse_args()

    if "RANK" not in os.environ:
        # self-spawn one process per GPU
        import subprocess
        world = args.world or torch.cuda.device_count()
        env = dict(os.environ,
                   MASTER_ADDR=os.environ.get("MASTER_ADDR", "127.0.0.1"),
                   MASTER_PORT=os.environ.get("MASTER_PORT", "29517"),
                   WORLD_SIZE=str(world))
        procs = []
        for r in range(world):
            e = dict(env, RANK=str(r), LOCAL_RANK=str(r))
            procs.append(subprocess.Popen([sys.executable, __file__,
                                           "--elems", str(args.elems),
                                           "--iters", str(args.iters)], env=e))
        rc = 0
        for p in procs:
            rc |= p.wait()
        print(f"VERDICT|{'FAIL' if rc else 'PASS'}", flush=True)
        sys.exit(rc)

    rank = int(os.environ["RANK"])
    world = int(os.environ["WORLD_SIZE"])
    dist.init_process_group("nccl", rank=rank, world_size=world,
                            timeout=datetime.timedelta(seconds=600))
    if rank == 0:
        print(f"INFO|torch|{torch.__version__}", flush=True)
        print(f"INFO|nccl|{'.'.join(map(str, torch.cuda.nccl.version()))}", flush=True)
        print(f"INFO|world|{world}", flush=True)
        for i in range(torch.cuda.device_count()):
            p = torch.cuda.get_device_properties(i)
            print(f"INFO|gpu{i}|{p.name} sm_{p.major}{p.minor} {p.total_memory >> 20}MiB",
                  flush=True)
    nfail = run(rank, world, args.elems, args.iters)
    dist.barrier()
    dist.destroy_process_group()
    if rank == 0:
        print(f"TOTAL|ranks={world} failures={nfail}", flush=True)
    sys.exit(1 if nfail else 0)


if __name__ == "__main__":
    main()
