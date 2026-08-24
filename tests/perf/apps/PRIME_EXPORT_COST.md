# What the per-frame PRESENT export actually costs

Measured 2026-08-24 on a rented **RTX 3090, driver 595.71.05**, Ubuntu 22.04
container with `/dev/dri/renderD135` exposed. Tool:
[`prime_export_cost.c`](prime_export_cost.c). Three runs, 3000–5000 iterations
each, against a real GBM `SCANOUT|RENDERING` 1920×1080 XRGB8888 bo — the same
kind of buffer a compositor flips.

| | p50 | p99 | mean | max |
|---|---|---|---|---|
| `PRIME_HANDLE_TO_FD` + close, in-process | **1.1 µs** | 1.1–1.2 µs | 1.1–1.2 µs | 13–30 µs |
| Full round-trip across a process boundary (SCM_RIGHTS) | **17.0–22.9 µs** | 21.1–32.6 µs | 19.5–27.0 µs | ~11.6 ms¹ |

¹ Appears exactly once per run at a consistent magnitude with p99 at ~30 µs, so
it is first-iteration warm-up (page faults, first SCM_RIGHTS), not a recurring
stall.

**The process boundary is the whole cost** — the export ioctl itself is ~1 µs.
Crossing to another process and blocking for the reply is ~16–22 µs.

## Verdict: the round-trip is noise

| | frame budget | export cost | share |
|---|---|---|---|
| 60 Hz | 16 667 µs | ~20 µs | **0.12 %** |
| 144 Hz | 6 944 µs | ~20 µs | **0.29 %** |

This is a **lower bound** — it excludes contention on the isolate's
`present_lock` and BQL interaction, both of which only add. But the margin is
three orders of magnitude. Even multiplying by four for the real stub's framed
protocol, worker-thread wakeup and condvar handoff, it stays under 1 % of a
frame.

**So the fd cache is not worth shipping.** It removes ~20 µs per frame at the
cost of a `GEM_CLOSE` hook in the DRM dispatch path and ~250 lines of cache in
a security-sensitive file — and, as the commit that added it records, the
round-trip it removes is *load-bearing for correctness*: the `ino` check that
catches recycled GEM handles works only because the fd is re-derived each frame.
Trading a real invariant for 0.12 % of a frame is a bad trade.

## What is worth keeping

The **export-path instrumentation**. `present_mean` is taken on the consumer
side and starts after the slot handoff, so it never covered this path at all —
anyone measuring the export with it measures the wrong thing. The counters added
alongside the cache (`lock_us`, `rt_us`, reported separately because their sum is
vCPU stall time) close that blind spot and cost nothing when
`NVKVM_EXPORT_TIMING` is unset.

## What this does not answer

Only the in-situ run can: the cache hit rate, whether `FDCACHE STALE` ever
fires, and whether a desktop looks correct with the cache on. Those need a
desktop guest flipping NVIDIA bos under nested KVM. They are moot if the cache
is dropped, which is the recommendation.
