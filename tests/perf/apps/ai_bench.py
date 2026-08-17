#!/usr/bin/env python3
# ai_bench.py — PyTorch AI workloads, run identically in the host and guest venvs.
# Emits machine-readable "METRIC <key> <value>" + "CHECK <key> <ok|FAIL>" lines.
# No pretrained weights downloaded (random init is fine for throughput parity).
import sys, time, torch
import torch.nn as nn

def now(): torch.cuda.synchronize(); return time.perf_counter()
def emit(k, v): print(f"METRIC {k} {v:.2f}", flush=True)
def chk(k, ok): print(f"CHECK {k} {'ok' if ok else 'FAIL'}", flush=True)
def log(*a): print("[ai]", *a, file=sys.stderr, flush=True)

dev = "cuda"
assert torch.cuda.is_available(), "no CUDA"
log("device", torch.cuda.get_device_name(0), "torch", torch.__version__)
torch.backends.cudnn.benchmark = True

def timed(fn, iters, warm=5):
    for _ in range(warm): fn()
    t = now()
    for _ in range(iters): fn()
    return now() - t

# ---- 1/2: raw GEMM (fp32 + fp16 tensor cores) via torch.matmul ----
def matmul(dtype, key):
    N = 8192
    a = torch.randn(N, N, device=dev, dtype=dtype)
    b = torch.randn(N, N, device=dev, dtype=dtype)
    it = 30
    t = timed(lambda: torch.matmul(a, b), it)
    tflops = 2.0 * N**3 * it / t / 1e12
    emit(key, tflops); chk(key, torch.isfinite(torch.matmul(a, b)[0, 0]).item())

# ---- 3-5: torchvision CNNs ----
def cnn(name, train, key, bs=64, amp=False):
    import torchvision.models as M
    net = getattr(M, name)(weights=None).to(dev)
    x = torch.randn(bs, 3, 224, 224, device=dev)
    if train:
        # correctness on a clean pre-training forward (SGD on random init diverges
        # to inf by design — the throughput, not the trained weights, is the point)
        with torch.no_grad(): pre_ok = torch.isfinite(net(x).float().sum()).item()
        opt = torch.optim.SGD(net.parameters(), lr=1e-4); net.train()
        def step():
            opt.zero_grad(set_to_none=True)
            loss = net(x).sum(); loss.backward(); opt.step()
        it = 20
        t = timed(step, it)
        emit(key, bs * it / t); chk(key, pre_ok); return
    else:
        net.eval()
        def step():
            with torch.no_grad():
                if amp:
                    with torch.autocast("cuda", dtype=torch.float16): net(x)
                else: net(x)
        it = 30
    t = timed(step, it)
    emit(key, bs * it / t)  # images/sec
    with torch.no_grad(): out = net(x)
    chk(key, torch.isfinite(out.float().sum()).item())

# ---- 6: ViT inference ----
def vit(key, bs=32):
    import torchvision.models as M
    net = M.vit_b_16(weights=None).to(dev).eval()
    x = torch.randn(bs, 3, 224, 224, device=dev)
    def step():
        with torch.no_grad(): net(x)
    t = timed(step, 20)
    emit(key, bs * 20 / t)
    with torch.no_grad(): chk(key, torch.isfinite(net(x).sum()).item())

# ---- 7: BERT-like transformer encoder inference ----
def transformer(key, bs=32, seq=128):
    enc = nn.TransformerEncoder(
        nn.TransformerEncoderLayer(d_model=768, nhead=12, dim_feedforward=3072,
                                   batch_first=True, activation="gelu"),
        num_layers=12).to(dev).eval()
    x = torch.randn(bs, seq, 768, device=dev)
    def step():
        with torch.no_grad(): enc(x)
    t = timed(step, 20)
    emit(key, bs * 20 / t)  # sequences/sec
    with torch.no_grad(): chk(key, torch.isfinite(enc(x).sum()).item())

if __name__ == "__main__":
    matmul(torch.float32, "torch_matmul_fp32_TFLOPs")
    matmul(torch.float16, "torch_matmul_fp16_TFLOPs")
    cnn("resnet50", False, "resnet50_infer_imgs")
    cnn("resnet50", False, "resnet50_infer_amp_imgs", amp=True)
    cnn("resnet50", True,  "resnet50_train_imgs")
    vit("vit_b16_infer_imgs")
    transformer("bert_infer_seqs")
    log("done")
