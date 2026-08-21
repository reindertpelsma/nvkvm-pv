# Evidence, 2026-08-21 — RTX 4070 desktop session

Captured from the physical test box (Ryzen 9 7900, RTX 4070 12 GB, driver
595.84) running a Linux Mint guest under nvkvm. The machine was not ours to keep,
so these are kept here rather than re-derivable on demand.

## `qemu-hang.bt` — the fd-0 bug, caught in the act

Full `thread apply all bt` of a QEMU that had frozen with the guest. It is the
production manifestation of **P-15** (`isolate: creating an isolate closed
QEMU's fd 0`), found the same afternoon by fixing four long-red unit tests.

Read it with the timeline: the binary was built **18:26:11**, the fix committed
**18:40:18**, this QEMU started **18:43:07** — i.e. 14 minutes too early to have
the fix.

What it shows:

- Thread 1, the main loop, blocked in `recvfrom` inside `slirp_pollfds_poll`
- that `recvfrom` is on **fd 0**, and `/proc/PID/fd/0` was a *socket* despite
  QEMU being launched with `< /dev/null`
- several threads parked on the `bql` futex behind it, which is why the whole VM
  froze rather than just the UI
- whole-process CPU delta over 5 s was **0 jiffies** — deadlocked, not busy

The chain: `nvkvm_isolate_table_init()` left `sync_open_fd` at 0, the first
`alloc_isolate_slot()` did `close(0)` on QEMU's own stdin, the descriptor number
was freed, and slirp's next socket landed on it. After the fix, `fd 0` stays
`/dev/null`.

Note the two wrong diagnoses this backtrace produced before the CPU measurement
settled it: "slirp saturated by the download" (there was no traffic) and "slirp
spinning" (there was no CPU). Being *in* slirp's stack is not evidence that
slirp is doing anything.

## `nvkvm-vram*.log` — GPU memory growth

Samplers across several QEMU generations. Format is
`timestamp vram_used,vram_free qemu_fds`; the later files add per-process
attribution, which is what made the growth legible at all — `nvidia-smi` lists
QEMU and each `nvkvm_stub` separately, and stubs map to guest processes by start
time.

- `nvkvm-vram.log`, `-vram2.log` — early total-VRAM sampling
- `-vram3.log` — readback present mode
- `-vram4.log`, `-vram5.log` — GL zero-copy (`NVKVM_PRESENT_MODE=gl`)

What they establish, and it is mostly a set of eliminations:

- **not wall-clock**: five minutes with a static window produced zero growth
- **not frame count**: ~18,000 presented frames produced zero attributable growth
- growth lands on **new surface/window creation**, and accumulates in the
  processes that import *other* processes' buffers (Xwayland, cinnamon), not in
  weston or QEMU

The original session that ran a 12 GB card out of memory grew ~1.2 GB → ~11.5 GB
in about 2.5 hours (~4 GB/h) under active desktop use. These logs never
reproduced that rate because the guest was mostly idle; the highest observed here
is ~750 MiB/h during window creation.

**The leak these point at is fixed** (`xiso: close the brokered GEM, because
nothing else ever did` — cross-isolate imported GEMs were never closed). That
the missing free existed is certain from source. That it was the *dominant* term
in the 4 GB/h is **inferred, not demonstrated** — nothing here measures memory
coming back after clients exit. The experiment that would settle it is in the
commit message.
