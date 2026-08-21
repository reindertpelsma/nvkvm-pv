#!/usr/bin/env python3
"""
sync_micro.py — does *synchronised* multi-GPU work cost more in the guest than
*independent* multi-GPU work, and how does that scale with world size?

Runs N ranks (one per GPU).  Each iteration does a fixed amount of local GPU
work, then optionally a collective, then a device sync, and records the
per-iteration latency.  Percentiles are reported, not just means: a tail that a
synchronised group pays N times over is exactly the shape we are hunting.

Modes:
  independent — local work only, no collective (ranks never wait for each other)
  barrier     — local work + dist.barrier()          (pure synchronisation)
  allreduce   — local work + dist.all_reduce(buf)    (synchronisation + data)

Usage: sync_micro.py --world 4 --mode allreduce --bytes 65536 --iters 500
"""
import argparse, json, os, statistics, sys, time
import torch
import torch.distributed as dist
import torch.multiprocessing as mp


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round((len(xs) - 1) * q)))]


def worker(rank, args, q):
    os.environ["MASTER_ADDR"] = "127.0.0.1"
    os.environ["MASTER_PORT"] = str(args.port)
    os.environ["RANK"] = str(rank)
    os.environ["WORLD_SIZE"] = str(args.world)
    torch.cuda.set_device(rank)
    dev = torch.device("cuda", rank)

    if args.mode != "independent":
        dist.init_process_group("nccl", rank=rank, world_size=args.world)

    n = args.bytes // 2                       # bf16 elements
    buf = torch.ones(n, dtype=torch.bfloat16, device=dev)
    # Local work: a stack of small kernels, so the iteration has real GPU time
    # and is not pure launch overhead.
    a = torch.randn(args.work_dim, args.work_dim, dtype=torch.bfloat16, device=dev)
    b = torch.randn(args.work_dim, args.work_dim, dtype=torch.bfloat16, device=dev)

    def one_iter():
        for _ in range(args.kernels):
            a.addmm_(a, b, beta=1.0, alpha=0.0001)
        if args.mode == "allreduce":
            dist.all_reduce(buf)
        elif args.mode == "barrier":
            dist.barrier()
        torch.cuda.synchronize()

    for _ in range(args.warmup):
        one_iter()
    if args.mode != "independent":
        dist.barrier()
    torch.cuda.synchronize()

    lat = []
    t_start = time.perf_counter()
    for _ in range(args.iters):
        t0 = time.perf_counter()
        one_iter()
        lat.append((time.perf_counter() - t0) * 1e6)
    total = time.perf_counter() - t_start

    res = dict(rank=rank, mode=args.mode, world=args.world, bytes=args.bytes,
               kernels=args.kernels, iters=args.iters,
               mean_us=statistics.fmean(lat), p50=pct(lat, .5), p90=pct(lat, .9),
               p99=pct(lat, .99), maxv=max(lat), minv=min(lat),
               total_s=total)
    q.put(res)
    if args.mode != "independent":
        dist.barrier()
        dist.destroy_process_group()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--world", type=int, default=2)
    ap.add_argument("--mode", choices=["independent", "barrier", "allreduce"],
                    default="allreduce")
    ap.add_argument("--bytes", type=int, default=65536)
    ap.add_argument("--kernels", type=int, default=8)
    ap.add_argument("--work-dim", type=int, default=1024)
    ap.add_argument("--iters", type=int, default=500)
    ap.add_argument("--warmup", type=int, default=200)
    ap.add_argument("--port", type=int, default=29511)
    ap.add_argument("--label", default="run")
    a = ap.parse_args()

    ctx = mp.get_context("spawn")
    q = ctx.Queue()
    ps = [ctx.Process(target=worker, args=(r, a, q)) for r in range(a.world)]
    for p in ps:
        p.start()
    out = [q.get() for _ in range(a.world)]
    for p in ps:
        p.join()
    for r in sorted(out, key=lambda d: d["rank"]):
        r["label"] = a.label
        print("SYNC|" + json.dumps(r, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
