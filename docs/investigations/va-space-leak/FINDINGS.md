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

---

# UPDATE — the leaked resource identified, with a causal chain

## 7. It is BAR1 aperture address space, and the chain is temporally ordered

From the leaking boot's 76,591-line capture
(`/workspace/nvkvm-steamos/evidence-pc-final-20260828/va-evidence.txt`):

| NVRM message | count |
|---|---|
| `dmaAllocMapping_GM107: can't alloc VA space for mapping.` | 31,805 |
| `NV_ERR_NO_MEMORY ... mapping_reuse.c:273` | 18,492 |
| `NV_ERR_NO_MEMORY ... reusemappingdbMap(&pBar1VaInfo->reuseDb, ...) @ kern_bus_gm107.c` | 9,246 |
| `nvAssertFailedNoLog ... @ rs_client.c:1194` | 5,089 |
| `clientUnmapResourceRefMappings: hContext: ... at addr ...` | 5,089 |
| `clientUnmapResourceRefMappings: Failed to auto-unmap (status=0x23)` | 5,089 (sum) |
| `NV_ERR_NO_MEMORY ... kbusMapFbAperture_HAL ... @ kern_bus.c` | 1,779 |

**Timeline in that boot:**
- first `Failed to auto-unmap`: **17:02:51**
- first `can't alloc VA space`: **17:09:39** — **6 min 48 s later**
- both continue to the end of the boot (18:20 / 18:22)

The failures precede the exhaustion by nearly seven minutes, in the right order
for cause and effect.

**The exhausted resource is named by the driver itself:**
`pBar1VaInfo->reuseDb` and `kbusMapFbAperture_HAL` — this is the **BAR1
aperture's VA allocator**, not VRAM.

## 8. Why nothing surfaces it

On this host **BAR1 Total is 256 MiB** (Resizable BAR is off; a 4070 with ReBAR
would report ~12 GiB). Decisively: in the *wedged* state `nvidia-smi` reported
**BAR1 Used = 59 MiB of 256 MiB**, i.e. 197 MiB "free". So the leaked mappings
consume BAR1 **address space** in RM's reuse database **without being accounted
as used**. `nvidia-smi` cannot see this leak by construction — neither its VRAM
figures nor its BAR1 figures move. That is the answer to "nothing surfaces it".

5,089 leaked mappings against a 256 MiB aperture is the right order of
magnitude to exhaust it.

## 9. The mechanism, as far as the evidence supports it

```
NVRM: clientUnmapResourceRefMappings: Failed to auto-unmap (status=0x23) hClient c1d0002e: hResource: beef00de
NVRM: clientUnmapResourceRefMappings: hContext: beef0004 at addr 11E57E000
```
- `status=0x23` = `NV_ERR_INVALID_CLIENT`.
- `clientUnmapResourceRefMappings` is RM's teardown sweep. It is failing, so the
  BAR1 mapping is never released.
- The whole handle family is `0xbeef*` (`hContext beef0004`, `hResource
  beef00de`/`beefc360`). **These are not nvkvm's:** `grep -rni beef src/` in
  nvkvm-pv finds nothing, and QEMU's admin client uses `0xad000001 /
  0xad000d00 / 0xad002080` (`src/qemu/nvkvm_isolate_handlers.c:1360-1366`).
  They are also **not in the open kernel modules** (`grep -rn 0xbeef` across
  `/workspace/ogkm-src` finds nothing), so they are allocated by NVIDIA's
  closed **userspace** driver in the guest and forwarded verbatim by nvkvm.

**Working hypothesis (UNVERIFIED):** a BAR1 mapping is created against a context
owned by one RM client but retired under another, so when the second client is
destroyed after the first, RM cannot resolve `hContext` and aborts the unmap
with `INVALID_CLIENT`. nvkvm does enable exactly this class of cross-client
visibility — it issues an extra host-side `NV_ESC_RM_SHARE` on **every**
successful `RM_ALLOC` (`src/qemu/nvkvm_isolate_handlers.c:3616`) and forwards
`NV_ESC_RM_DUP_OBJECT` verbatim. **The experiment that would settle it** is to
log the (hClient, hDevice, hMemory) triple at every forwarded
`RM_MAP_MEMORY`/`MAP_MEMORY_DMA` and correlate the leaked `hResource` values
against which client owned the context.

## 10. Live status on the rebooted host — the leak IS present, just slow

The rebooted host accumulates `Failed to auto-unmap` (1,465 -> 1,488 across a
guest start) while `can't alloc VA space` stays at **0**. That matches the
7-minute lead in §7: leaked mappings accumulate first, exhaustion follows only
once BAR1 fills. So this is a live, quantitative reproduction — the counter to
watch is:

```
journalctl -k -b 0 --no-pager | grep -c "Failed to auto-unmap"
```

**Use `journalctl -k`, not `dmesg`.** The dmesg ring buffer rotates under this
message volume and silently under-counts — observed going 909 -> 907 -> 903 ->
874 while the true journal count was 1,465. Two earlier "delta = 0" results in
§3 were measured with `dmesg` and are therefore weaker than they look; they
should be re-run against `journalctl` before being relied on.

## 11. Additional measured negatives (rebooted host)

| Test | Result |
|---|---|
| 10 Kata GPU containers, **no nvkvm guest at all** | 0 VA errors, Vulkan healthy. Kata alone does not reproduce it. |
| 8 Kata GPU containers **concurrently with the SteamOS VM** | 0 VA errors, **0** new failed auto-unmaps, Vulkan healthy. |
| `nvidia-cdi-refresh.service` | Ran once at boot, `NRestarts=0`. Not looping, not a churn source. |
| ~55 min of guest uptime incl. rendering and isolate churn | 0 VA errors. |

So the *fast* regression of boots -2/-1 (19,321 events in 8 minutes) is still
not reproduced, and neither Kata alone nor Kata+VM explains it.

## 12. Revised guidance for resuming

The §6 ladder still stands, but the priority has changed:

1. **Watch `Failed to auto-unmap` via `journalctl -k`, not the VA errors.** It
   leads by ~7 minutes and is present even on a "clean" boot.
2. **BAR1 size is likely why this host is the one that shows it.** 256 MiB
   (ReBAR off) is a small budget; a host with ReBAR enabled has ~12 GiB of BAR1
   and would take ~50x longer to exhaust — the same leak could exist everywhere
   and only ever be *noticed* here. **Check `nvidia-smi -q -d MEMORY | grep -A3
   BAR1` on any candidate repro box, and prefer one with ReBAR off.**
   This materially improves the odds of reproducing on rented hardware, and
   revises §6's pessimism: a rented box with a small BAR1 is now worth trying.
3. The `0xbeef*` handles are guest **userspace** handles. Correlating them
   against nvkvm's forwarded map/dup/share traffic is the next concrete step,
   and it needs no special hardware — a trace of forwarded RM ioctls on any
   working nvkvm host would do.

---

# UPDATE 2 — DETERMINISTIC REPRODUCTION

## 13. Every fully-initialized guest Vulkan client leaks exactly 59 BAR1 mappings on exit

Measured on the rebooted host, counting with `journalctl -k` (not `dmesg`):

```
baseline                                   = 1488
run vkcube in guest 60 s, then SIGKILL     = 1547   (delta +59)
run vkcube in guest 60 s, then clean SIGTERM = 1606 (delta +59)
```

**+59 both times.** Two conclusions, both important:

1. **Abnormal termination is irrelevant.** A clean `SIGTERM` leaks exactly as
   much as a `SIGKILL`. This kills the "isolate dies abnormally and strands
   handles" hypothesis outright (and with it the remaining relevance of the
   `isolate_refcount` / `-EBUSY` path in §3).
2. **The client must actually get going.** Earlier tests that leaked **0** were
   2-second `vkcube`s and plain `vulkaninfo` — neither creates the rendering
   resources. The leak is per-client-teardown, and its size is the number of
   BAR1 mappings that client accumulated.

The "59" matches the per-client grouping seen independently in the histogram of
the wedged boot (59 failures per `hClient`, across many sequential client
handles).

**Reproduction recipe (no special tooling needed):**
```bash
# host
journalctl -k -b 0 --no-pager | grep -c "Failed to auto-unmap"
# guest (SteamOS, via ./steamos-ssh)
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
setsid nohup vkcube >/dev/null 2>&1 </dev/null & sleep 60; pkill -x vkcube
# host again -- expect +59
```
At 59 mappings per client teardown against a 256 MiB BAR1, roughly **86 client
teardowns** reproduce the ~5,089 leaks seen in the wedged boot. A SteamOS
session that restarts gamescope/Steam/KWin repeatedly reaches that quickly,
which is consistent with the owner's 8-minute boot -2 collapse.

## 14. What the teardown looks like

`evidence/one-client-teardown-59-leaks.txt`. For a single dying client
(`c1d003f3`), all ~59 failures share **one** context:

```
Failed to auto-unmap (status=0x23) hClient c1d003f3: hResource: 40
hContext: beef0004 at addr 1543C9000
... hResource: 4b, 4c, 4d, 4e, 4f, 50, 51, 52, 54, 55, 5a, 5b, 5e, 5f, 60, 61, 62,
    and finally beef00de and beefc360
```

- `hResource` values are mostly small RM-assigned handles; two are guest
  userspace handles (`beef00de`, `beefc360`).
- **`hContext` is `beef0004` for every single one.** One context object, ~59
  mappings hanging off it, and that context is unresolvable
  (`NV_ERR_INVALID_CLIENT`) at the moment the client is torn down.

So the failure is not per-mapping bad luck: **the whole client's BAR1 mappings
are anchored on one context handle whose owning client RM can no longer
resolve.** Whatever splits that context away from the mappings' client is the
bug.

**Still UNVERIFIED — the specific nvkvm behaviour responsible.** The prime
suspect remains the host-initiated extra `NV_ESC_RM_SHARE` issued on **every**
successful `RM_ALLOC` (`src/qemu/nvkvm_isolate_handlers.c:~3590-3600`), which
deliberately widens cross-client visibility of freshly allocated objects; and
`NV_ESC_RM_DUP_OBJECT`, forwarded verbatim. **The decisive next experiment** is
to disable that post-alloc SHARE and re-run the +59 measurement above. If the
delta drops to 0, that line is the leak. That is a one-line change and a
five-minute test, and it is where whoever picks this up should start.

---

# UPDATE 3 — the leak is nvkvm-specific, and where it is narrowed to

## 15. Host-native control: the identical app leaks NOTHING on the host

Same host, same GPU, same driver, same binary (`/usr/bin/vkcube`), confirmed
running on the RTX 4070:

| client | teardown | new `Failed to auto-unmap` |
|---|---|---|
| `vkcube` **inside the nvkvm guest**, 60 s | SIGKILL | **+59** |
| `vkcube` **inside the nvkvm guest**, 60 s | clean SIGTERM | **+59** |
| `vkcube` **on the host**, 30 s | SIGKILL | **0** |
| host CUDA client (`va_capacity`, allocates/frees ~11 GiB) | clean exit | **0** |
| 10 Kata GPU containers (different VMM stack) | clean exit | **0** |

**This is the control the investigation needed.** RM's teardown auto-unmap does
not fail for ordinary host clients, so this is not a generic driver bug that any
GPU workload triggers. **Something nvkvm does to the mapping topology makes RM
unable to unmap on client teardown.**

## 16. What the forwarded RM traffic shows

With `NVKVM_DEBUG=1` on the vmm service (runtime env var,
`src/qemu/nvkvm_log.h` — no rebuild needed), over one guest session:

| trace | count |
|---|---|
| `RM_MAP_MEMORY` | 895 |
| `A100DBG UNMAP` | 376 |
| `A100DBG FREE` | 2071 |
| `post-alloc SHARE` (succeeded) | 1320 |
| `post-alloc SHARE` (**ret=-22**) | 230 |

- **895 maps vs 376 unmaps.** ~519 mappings are never explicitly unmapped; they
  rely on client teardown — which is the path that fails.
- Every map uses `h_device=0xbeef0004`, e.g.
  `h_client=0xc1d00424 h_device=0xbeef0004 h_memory=0x23 length=0x400000 fd=1343 -> pLinear=0x147e40000 status=0x0`.
- `0xbeef0004` is **`hClass=0x2080` = NV20_SUBDEVICE_0**, and each client
  allocates its *own* — so this is not a handle collision across clients.
- **`0xbeef0004` is never explicitly freed** (`FREE hObject=0xbeef0004` count =
  **0**). The map context is only ever released implicitly, with its client.
- 118 of the frees are explicit **client** frees (`hObject == hRoot == hClient`,
  `nvstatus=0x0`, i.e. they succeed).
- **The client that actually failed (`c1d004a1`) was never explicitly freed at
  all** — no `FREE hObject=0xc1d004a1` exists. Its teardown therefore ran via
  the implicit **fd-close** path.

## 17. Current best hypothesis (UNVERIFIED) and how to settle it

The mappings are anchored on a subdevice context that is only ever released
implicitly, and the failing teardowns run on the fd-close path rather than an
explicit client free. nvkvm is the only thing here that changes the *identity
and number of file descriptions* behind a guest client: the stub opens the real
device and **QEMU keeps an SCM_RIGHTS duplicate**
(`src/qemu/nvkvm_isolate_handlers.c:670-694`), QEMU can open nvidia nodes itself
(`src/qemu/nvkvm_handle.c:127`), and the stub opens a fresh `/dev/nvidia-uvm`
per REALIZE that it deliberately leaks
(`src/stub/nvkvm_stub.c:2626`, comment at `:2776-2777`). A guest process that
believes it holds one device file may be spread across more than one host open
— and therefore more than one RM client — so the client that owns the map
context is not the client RM tears down.

**Two experiments, in order, neither needing special hardware beyond a working
nvkvm host:**

1. **Log the fd->client identity.** `NVKVM_DEBUG=1` already prints the `fd` used
   by each `RM_MAP_MEMORY`. Add the owning handle/client for that fd and confirm
   whether `fd`'s RM client == `h_client`. If they differ, that is the bug, and
   the fix is to make the map's fd and its client the same open.
2. **Disable the host-initiated post-alloc `NV_ESC_RM_SHARE`**
   (`src/qemu/nvkvm_isolate_handlers.c:3528` `needs_share`) and re-run the +59
   measurement in §13. Note this SHARE **already fails 230 times with
   `ret=-22` (EINVAL)** in one session, so it is not behaving as intended
   regardless. If the delta drops to 0, that line is the leak.

**Do NOT treat this as fixed.** No code change is included in this branch,
because the mechanism is not yet proven and a speculative change to RM object
lifetime is exactly the kind of thing that produces a second, subtler leak.

## 18. Host left as found

`NVKVM_DEBUG` was added to the vmm service's `environment:` in
`/opt/nvkvm-steamos-two-container/docker-compose.yml` for the traces above and
has been **reverted** (backup at `/root/docker-compose.yml.bak-vaLeak`). To
re-enable, add one line under the `vmm:` service:
```yaml
      NVKVM_DEBUG: "1"
```
then `docker compose up -d vmm`. The vmm image was verified as QEMU **9.2.0**
before and after every restart, per the silent-rebuild warning.

Sampler units left running on the host (harmless, `--collect`):
`nvkvm-va-sampler`, `nvkvm-va-sampler2`, `nvkvm-cap-sampler`,
`nvkvm-leak-sampler`, writing `/root/*log*.txt`.

---

# UPDATE 4 — re-measured 2026-09-03 on a 16 GB BAR1 host

Host: PC "claude", RTX 4070, driver 595.84, **BAR1 resized to 16 GB (ReBAR on)**,
guest = SteamOS 3.8.16 in Game Mode under gamescope, nvkvm-pv main `b092a9a`.

## 19. A bigger aperture does NOT fix it — the rate is unchanged

Five consecutive `vkcube` cycles (start under `WAYLAND_DISPLAY=gamescope-0`,
render ~25 s, `SIGKILL`), counting the documented metric on the host:

| cycle | before | after | delta |
|---|---|---|---|
| 1 | 728 | 787 | **+59** |
| 2 | 787 | 846 | **+59** |
| 3 | 846 | 905 | **+59** |
| 4 | 905 | 964 | **+59** |
| 5 | 964 | 1023 | **+59** |

Exactly the +59 recorded in sec.13 on the 256 MiB host. **A 16 GB BAR1 buys ~64x
more teardowns before the host wedges; it does not reduce the leak per teardown.**

## 20. WARNING: nvidia-smi cannot measure this, and it looks reassuring

Throughout all five cycles `nvidia-smi` reported BAR1 Used flat at
**1014 / 16384 MiB**. Stopping the whole compose stack drops it 833 -> 351 MiB and
restarting returns it to ~872 MiB, cycle after cycle, with QEMU verifiably dead
(process count sampled per reading) during every "down" phase.

That whole picture reads as "teardown returns the memory, nothing accumulates" —
and it is **wrong**. It measures a counter sec.8 already said is blind to this
leak. Anyone re-deriving "there is no leak" from `nvidia-smi` has reproduced a
measurement error, not a result. Count `Failed to auto-unmap` instead.

Corollary for hosts like the vast RTX 3060 (fixed 256 MiB BAR1, **no Resizable
BAR capability at all**): with nvkvm not running *and zero QEMU processes*, that
box still shows 255/256 MiB used, because a desktop alone exceeds that aperture
(this host's desktop-only baseline is ~351 MiB). That is a separate, additive
problem — too small an aperture — and it is not this leak. Do not conflate them.

## 21. Structure of a single teardown (new)

Per cycle, exactly **one** RM client accounts for all 59 failures:

```
58 c1d00285      59 c1d00294      59 c1d0029e
58 c1d0028a      59 c1d00299      59 c1d002a3
59 c1d0028f
```

- **One failing client per app teardown**, never several.
- The failing client handle advances by **5** per cycle (0x285, 0x28a, 0x28f,
  0x294, 0x299, 0x29e, 0x2a3), i.e. roughly **five RM clients are allocated per
  guest Vulkan app** and exactly one of them fails teardown. That is consistent
  with sec.17's hypothesis that one guest client is spread across several host
  opens and the mapping owner is not the client RM tears down.
- The leaked `hResource` handles are mostly low RM-assigned numbers
  (7, 8, 9, b, c, d, e, f, 62, ...) plus exactly **2** nvkvm-synthesised
  `0xbeef....` handles per teardown.

## 22. `vulkaninfo` does NOT reproduce it

Three `vulkaninfo --summary` runs in the guest: delta **0**. It creates a device
and exits without a swapchain. The trigger therefore needs a
rendering/presenting client, which narrows the suspect mappings to the
presentation path rather than device creation.

## 23. Framing (owner's, and it is the right one)

Whoever triggers it, **an unprivileged process must not be able to permanently
consume a global kernel resource that survives its death** — that is an invariant
OGKM should hold regardless of how userspace arranges its file descriptors.
close(2) failing to reclaim is a driver defect with nvkvm as, at most, the
trigger. Practical consequence: this is worth an upstream reproducer, and nvkvm
should not be contorted around it until the mechanism is proven.

## 24. Next: `NVKVM_NO_POSTALLOC_SHARE`

sec.17 experiment 2 previously needed an ad-hoc patch. Branch
`diag/postalloc-share-gate` adds a runtime knob (`NVKVM_NO_POSTALLOC_SHARE=1`)
that skips the host-initiated post-alloc `NV_ESC_RM_SHARE`, so the +59
measurement can be A/B'd against it directly. CUDA/UVM is expected to break
under it -- that grant is what lets UVM dup objects during `cuCtxCreate` -- so it
is for measurement only.

## 25. Controls re-run 2026-09-03 — GPU containers do NOT leak

The worry worth settling: if plain CUDA containers leaked this way, every
multi-tenant GPU host (Vast.ai and friends) could be wedged by any tenant. They
do not.

| workload | teardowns | delta | notes |
|---|---|---|---|
| guest `vkcube` (presents) | 5 | **+59 each** | the leak |
| GPU container, `--gpus all`, CUDA client | 3 | **0** | probe verifiably ran |
| host-native CUDA client | 2 | **0** | matches sec.15 |

**Trap that produced a false negative first time:** the image used had its own
entrypoint, which refused to start and never executed the probe. The delta was
0 because *nothing ran*. Always assert the probe's own output (`MAXCONTIG_MB=`)
before believing a 0, and pass `--entrypoint`. `tools/reproduce_leak.sh` now
does both.

So the trigger is nvkvm-specific, and the "unprivileged process wedges a shared
GPU" concern does not generalise to ordinary containers.

## 26. On this host the leak has NOT yet cost measurable capacity

After 1113 failed auto-unmaps on the 16 GB BAR1 host:

```
MAXCONTIG_MB=10622  TOTAL_MB=10496  CUDA_FREE_MB=10677     (tools/va_capacity.c)
can't alloc VA space : 0
reusemappingdbMap NO_MEMORY : 0
```

`MAXCONTIG ~= CUDA_FREE` (0.5% apart) is the healthy signature -- allocation is
VRAM-bound, not VA-bound. So on this host the failed auto-unmaps are a **leading
indicator that has not yet become harm**. On the 256 MiB host the same +59 rate
exhausted the aperture and wedged host Vulkan (sec.7).

State both halves when reporting this. "RM fails to reclaim 59 mappings per
guest client teardown" is proven. "The host is currently degraded" is NOT true
here, and claiming it would be as wrong as the nvidia-smi reading that said
there was no leak at all.
