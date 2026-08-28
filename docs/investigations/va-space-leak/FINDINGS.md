# Host GPU VA-space exhaustion — investigation log

Branch `va-space-leak`. Test host: `pcr`, RTX 4070, driver **595.84 open kernel
module**, Ubuntu/GNOME (Wayland). Investigated 2026-08-28.

**Status: NOT ROOT-CAUSED. No fix committed.** This file records measurements,
eliminations and tooling so the work can resume on other hardware. Everything
below is measured on this host unless marked UNVERIFIED.

## The symptom

```
NVRM: dmaAllocMapping_GM107: can't alloc VA space for mapping.
NVRM: nvAssertOkFailedNoLog: Assertion failed: Out of memory [NV_ERR_NO_MEMORY]
      (0x00000051) returned from reusemappingdbMap(...)
```
Once it starts, `vkCreateDevice` fails for every Vulkan client on the host and
in the guest. `nvidia-smi` shows nothing wrong throughout — this is address
space, not memory, and nothing surfaces it.

## 1. Driver reload recovers it — a full reboot is NOT required

Measured on the wedged host before rebooting it (1058 `alloc VA space` events
in dmesg, `vulkaninfo` reporting 0 NVIDIA devices):

```
systemctl stop gdm
rmmod nvidia_drm nvidia_modeset nvidia_uvm nvidia   # all unloaded cleanly
modprobe nvidia && modprobe nvidia_modeset && modprobe nvidia_uvm && modprobe nvidia_drm
vulkaninfo --summary | grep -c "NVIDIA GeForce"     # -> 1  (was 0)
```
VRAM went 330 MiB -> 33 MiB. **The leaked state is entirely inside the NVIDIA
kernel modules; nothing is wrong with the GPU or its firmware.**

**CONFOUND, important:** `gdm` was stopped *before* the `rmmod`, so this does
not separate "driver reload fixed it" from "killing gnome-shell fixed it".
gnome-shell is the only long-lived host GPU client. The experiment that settles
it, on the next wedge, is the ladder in §6 — do that before any `rmmod`.

## 2. Measurable proxies (dmesg errors are lagging — these are not)

Three built for this, in `tools/`:

- **`va_probe.sh`** — per-process nvidia fd counts (QEMU, gnome-shell, stubs),
  nvidia VMA counts, dma_buf count/bytes from
  `/sys/kernel/debug/dma_buf/bufinfo`, VA error count, VRAM.
  Note stubs appear in `/proc/*/comm` as `memfd:nvkvm_stu`, so `pgrep
  nvkvm_stub` finds nothing — a trap that cost a measurement cycle.
- **`va_capacity.c`** — direct VA capacity probe (libcuda, no nvcc/headers).
  Binary-searches the largest `cuMemAlloc` and sums 128 MB chunks. Healthy
  host: `MAXCONTIG_MB` ~= `CUDA_FREE_MB` (allocation is VRAM-bound). When VA
  leaks, MAXCONTIG falls below CUDA_FREE while `nvidia-smi` stays flat. This is
  the discriminator between "out of memory" and "out of address space".
- **`mapva.py`** — reads QEMU's internal `g_mapva` table live out of
  `/proc/<pid>/mem` by symbol, with no rebuild and no restart. Reports
  occupancy (`used/8192`), the cumulative `g_mapva_seq`, and per-isolate
  counts. `g_mapva` is `src/qemu/nvkvm_isolate_handlers.c:1990`.

**The best leading indicator found is in dmesg itself, and it precedes
exhaustion by a long way:**
```
dmesg | grep -c "Failed to auto-unmap"
```
On the current boot this reached **909 while `can't alloc VA space` was still
0**. See §4.

## 3. Measured negatives — things that do NOT leak

Each ran against a live SteamOS guest (QEMU 9.2.0, `known-good-qemu92`) with
KWin up and the broker compositing on the host.

| Workload | Result |
|---|---|
| Idle guest, 15 min | `qemu_fd` 616, `stubs` 15, dma_buf 19-21, VA capacity flat. **No growth of any counter.** |
| 45 clean GPU processes (`vulkaninfo`) | `qemu_fd` rises 616 -> 693 during, returns to **exactly 616**. `mapva_used` returns to **exactly 412** while `mapva_seq_total` rose 2315 -> 3665. Orderly isolate teardown is clean. |
| 15 **SIGKILLed** Vulkan processes (`vkcube`) | `qemu_fd` 710 -> 617, `mapva_used` 412 -> 355. Both *down*. Abnormal isolate death also cleans up. |
| Continuous rendering (`vkcube`, several min, QEMU at 114% CPU, gnome-shell compositing) | Every counter flat. |
| 10 clean `vulkaninfo` | **0** new failed auto-unmaps. |
| 5 SIGKILLed short-lived `vkcube` | **0** new failed auto-unmaps. |

This empirically disproves, on this host, the three leading code hypotheses:

- **QEMU's SCM_RIGHTS handle duplicates** keeping RM clients immortal
  (`nvkvm_isolate_handlers.c:679` attaches the dup, `:694` refs it,
  `nvkvm_handle.c:368-371` refuses to close while refcounted). The mechanism is
  real and worth fixing on its own merits, but `qemu_fd` returns exactly to
  baseline after both clean and abnormal isolate churn, so it is not the leak.
- **`g_mapva` saturation** (`nvkvm_isolate_handlers.c:2011-2020` silently drops
  records when full). `nvkvm_mapva_forget_isolate` (`:802`) does reclaim on
  isolate death; occupancy did not ratchet across 45 isolates.
- **The present/dma-buf path.** Independently ruled out twice: a code audit
  found the host importers are fixed 8-entry inode-keyed LRU caches
  (`src/qemu/nvkvm_present_egl.c:336`, `src/broker/nvkvm_broker.h:45`) with
  destroys reached per frame; and the owner's leaking run produced
  **essentially zero presents** (the guest compositor never enabled its DRM
  output) while still draining 1203 VA events.

## 4. The one live signal found: RM fails to auto-unmap at client teardown

Present in the *wedged* boot's journal and reproduced live on the current boot:

```
NVRM: clientUnmapResourceRefMappings: Failed to auto-unmap (status=0x23) hClient c1d0025d: hResource: 3c
NVRM: clientUnmapResourceRefMappings: hContext: beef0004 at addr 3F5AC2000
NVRM: nvAssertFailedNoLog: Assertion failed: 0 @ rs_client.c:1194
```

- `status=0x23` is **`NV_ERR_INVALID_CLIENT`**.
- `clientUnmapResourceRefMappings` is RM's teardown sweep: when a client dies,
  RM auto-unmaps that client's remaining mappings. It is failing, so **the DMA
  mapping is not released** — which is exactly the shape of a VA leak that
  outlives every process and clears only on `rmmod`.
- `hContext: beef0004` is a caller-chosen handle that does **not** come from
  nvkvm: `grep -rni beef src/` finds nothing, and QEMU's own admin client uses
  `0xad000001 / 0xad000d00 / 0xad002080`
  (`src/qemu/nvkvm_isolate_handlers.c:1360-1366`). It is forwarded verbatim
  from the guest, or produced by another host component. **UNVERIFIED: who
  allocates `0xbeef0004`.**
- Distribution on the current boot: 909 failures, clustered in two bursts, at a
  very regular **59 failures per hClient** across many sequential client
  handles (`c1d00292`, `c1d00297`, `c1d0029c`, ... spaced by 5).

**UNVERIFIED and important:** whether these failed auto-unmaps *cause* the VA
exhaustion or merely accompany it. The current boot has 909 of them and **zero**
`alloc VA space` errors, so they are at minimum not sufficient on their own.

## 5. The reframing that matters most — this host regressed, the code did not

Per-boot history recovered from the persistent journal:

| boot | window | duration | `can't alloc VA space` |
|---|---|---|---|
| **-3** | Aug 27 17:36 -> Aug 28 02:50 | **~9 h** | **0** |
| -2 | Aug 28 16:46 -> 16:54 | 8 min | **19,321** |
| -1 | Aug 28 16:54 -> 18:23 | 89 min | **31,805** |

Boot -3 included a two-hour Shadow of the Tomb Raider session (sustained 80%
GPU, 168 W, 3760x2118), a full SteamOS install, and the QEMU 9.2.0 -> 11.1.1
rebuild, with **zero** leak. Two boots later: 19,321 events in eight minutes.

**The leak is therefore not inherent to running an nvkvm guest, and is not
proportional to GPU load or guest uptime.** It was introduced by a change to
this host. Candidates, all introduced the same day, none confirmed:
`nvkvm-kata` installed twice; Kata GPU containers run concurrently with the
SteamOS VM (two VMMs against one GPU); `nvidia-cdi-refresh.service` publishing
`/var/run/cdi/nvidia.yaml`; Docker bind-mounts creating
`/etc/vulkan/icd.d/nvidia_icd.json` and
`/etc/vulkan/implicit_layer.d/nvidia_layers.json` as **directories**; a second
QEMU at `/opt/qemu-nvkvm-kata`.

This also explains why ~50 minutes of varied guest workload on a freshly
rebooted host produced no leak at all: the current boot may simply not be in the
regressed state.

## 6. How to resume

**On the next wedge, run this ladder BEFORE any rmmod** — it identifies which
component owns the leaked mappings, which is the one thing the confounded
reload test in §1 could not answer:

1. Stop the guest container; confirm **no `qemu-system-x86_64` process remains**
   (this was never verified and matters — if QEMU lingers, §3's negative for the
   handle-dup mechanism is void). Check `vulkaninfo`.
2. Still wedged? `systemctl stop gdm` (kills gnome-shell, the only long-lived
   host GPU client) and re-check `vulkaninfo`. Recovery here means the host
   compositor held them.
3. Still wedged? `rmmod nvidia_drm nvidia_modeset` only, then re-check.
   Recovery here means `nvidia-drm`/`nvidia-modeset` kernel-side state.
4. Still wedged? Full `rmmod nvidia`. Recovery here means core RM client state.

**Cheapest decisive test, and the one to run first:** with the SteamOS stack
**not** running, start a Kata GPU container and sample
`journalctl -k | grep -c "alloc VA space"` over a few minutes. If it climbs with
no nvkvm guest at all, nvkvm is exonerated entirely.

**Reproducing on rented hardware:** likely NOT worth attempting first. The leak
has only ever been observed on this one host, a nine-hour heavy-load control on
the same code was clean, and the prime suspects are host-level installs
(Kata/CDI/Docker) rather than nvkvm. A rented box would reproduce the *nvkvm*
configuration, which is precisely the part the boot -3 control already
exonerates. Rent a box only after the Kata test above points back at nvkvm.
