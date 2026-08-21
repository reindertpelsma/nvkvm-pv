#!/usr/bin/env python3
"""
paths_probe.py — which fast paths does this side actually GET?

"Identical flags on both sides" is not the same as "identical code path".  vLLM
picks its all-reduce implementation at startup from capability probes, so a
capability that quietly fails in the guest changes the algorithm without
changing a single flag.  This prints the capabilities those probes read.

Run inside the venv, on both sides, and diff.
"""
import json, os, sys
import torch


def peer_matrix(n):
    m = {}
    for i in range(n):
        for j in range(n):
            if i == j:
                continue
            try:
                m["%d->%d" % (i, j)] = bool(torch.cuda.can_device_access_peer(i, j))
            except Exception as e:
                m["%d->%d" % (i, j)] = "ERR:%s" % e
    return m


def ipc_selftest():
    """Legacy CUDA IPC (cudaIpcGetMemHandle/OpenMemHandle) across two processes —
    this is what vLLM's custom all-reduce and NCCL's P2P transport both need."""
    import torch.multiprocessing as mp

    def child(q, qback):
        try:
            torch.cuda.set_device(1)
            h = q.get()
            t = torch.multiprocessing.reductions.rebuild_cuda_tensor(*h)
            qback.put(("OK", float(t.sum().item())))
        except Exception as e:
            qback.put(("ERR", repr(e)))

    ctx = mp.get_context("spawn")
    q, qback = ctx.Queue(), ctx.Queue()
    p = ctx.Process(target=child, args=(q, qback))
    p.start()
    torch.cuda.set_device(0)
    t = torch.ones(1024, device="cuda:0")
    q.put(torch.multiprocessing.reductions.reduce_tensor(t)[1])
    r = qback.get()
    p.join(30)
    return r


def main():
    n = torch.cuda.device_count()
    out = {
        "device_count": n,
        "torch": torch.__version__,
        "nccl_version": ".".join(str(x) for x in torch.cuda.nccl.version()),
        "peer_matrix": peer_matrix(min(n, 4)),
    }
    try:
        out["ipc_selftest"] = ipc_selftest()
    except Exception as e:
        out["ipc_selftest"] = ("ERR", repr(e))

    # vLLM's own P2P capability cache — the file its custom all-reduce reads.
    try:
        from vllm.distributed.device_communicators.custom_all_reduce_utils import (
            gpu_p2p_access_check)
        out["vllm_p2p_check"] = {("%d->%d" % (i, j)): bool(gpu_p2p_access_check(i, j))
                                 for i in range(min(n, 4)) for j in range(min(n, 4))
                                 if i != j}
    except Exception as e:
        out["vllm_p2p_check"] = "ERR:%r" % (e,)

    print("PATHS|" + json.dumps(out, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
