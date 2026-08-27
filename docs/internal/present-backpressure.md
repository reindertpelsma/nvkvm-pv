# Present backpressure: closing the guest's flip-completion loop

Branch `present-backpressure`. Status: **built and unit-gated, NOT hardware-tested.**
Nobody has yet run this against a real host compositor. See "What was not verified".

## The bug

`src/broker/nb_session_wl.c` logs

```
frame: REUSE-IN-FLIGHT seq=... buf=... slot=... (the compositor never released it; N so far)
```

when the guest commits a buffer that the HOST compositor is still reading. The
comment there states the semantics exactly: the guest has cycled its whole
scanout ring and come back to a buffer that is still on screen, so it has been
rendering into it underneath the compositor. That is a correctness bug, not a
cosmetic one, and the broker cannot fix it — it cannot stop the guest drawing.

It was an **open loop**, at two ends:

1. `src/qemu/nvkvm_display_relay.c` received `NVKVM_BROKER_EV_RELEASE` and
   dropped it, on the reasoning that "the guest recycles its own scanout bos".
   That is true of the guest's own accounting and misses the point: the host
   compositor is a **second consumer** of the same buffer, with a lifetime the
   guest cannot observe.
2. `src/guest/nvkvm_kms.c` completed page flips from a software vblank hrtimer
   slaved to the host refresh hint. Right average cadence, zero feedback from
   actual buffer lifetimes — so a transient host stall lets the guest keep
   flipping on schedule until it laps the ring.

### Measured, and its real severity

On the physical PC (Shadow of the Tomb Raider, fullscreen, 3760x2118,
host hint 59.996 Hz, GPU ~80% / 168 W):

* events at seq 21044, seq 125103, then **six consecutive** at seq 177876–177881;
* the log is rate-limited (`n_reuse_inflight <= 8 || % 256 == 0`), so the true
  count at "8" was somewhere in 8…255;
* **the counter then stopped moving** through an hour of further gameplay.

So the condition is **real but rare and bursty**. This is a robustness /
correctness fix for a rare condition. It is **not** the cause of the visible
~100 ms stalls the user reports, and must not be described as fixing them —
see "Recorded, not fixed" below.

Also relevant: the present mode is still `Present: COPY`. The host compositor is
compositing rather than promoting the surface to a hardware plane even in
fullscreen, and a compositing path holds each buffer until its own composite
completes. COPY is what makes the release late, and therefore what creates the
reuse condition in the first place.

## The cheap partial, evaluated: deeper buffering

Considered first, as it needs no protocol change. **Rejected**, on two grounds.

**Nothing in this repository chooses the ring depth.** The buffers being cycled
are allocated by *guest userspace* — the compositor, or a fullscreen client
whose surface the compositor scans out — through the NVIDIA render node.
`src/guest/nvkvm_kms.c` never allocates a scanout buffer; the head's only
`dumb_create` is `drm_gem_shmem_dumb_create`, which is not the path a real
compositor's bos take. `NB_MAX_BUFS = 8` in the broker is the broker's own slot
cache, not the guest's ring, and the relay retains exactly one frame. Confirmed
by grep: no in-tree code sizes, allocates, or requests a scanout ring depth.
"Make the guest use a deeper ring" is therefore a guest-userspace configuration
change (e.g. Mutter's dynamic triple buffering), outside this codebase, and a
fullscreen client doing its own swapchain may bypass it anyway.

**And the numbers say it would not have absorbed what was observed.** The
broker's log alternates `slot=0`/`slot=1`, `buf=861`/`buf=863` — a ring of
**two**. Reuse happens when the host holds ≥ D buffers, so at D=2 a single frame
of extra host latency is enough. Each extra buffer buys exactly one frame:

| ring depth D | host stall absorbed @ 59.996 Hz | scanout VRAM @ 3760x2118x4 |
|---|---|---|
| 2 (observed) | 0 ms  | 60.8 MiB |
| 3 (triple)   | 16.7 ms | 91.1 MiB |
| 8            | 100 ms  | 243 MiB |

The observed burst was **six consecutive** flips, i.e. a host stall of roughly
**100 ms** — the same order as the stalls the user sees. Absorbing that by depth
alone needs D≈8: four times the scanout memory, and ~100 ms of added display
latency in a game, to paper over a stall rather than react to it. Triple
buffering would have absorbed the two isolated events and none of the burst.

It remains a legitimate *complement* — if guest userspace triple-buffers, the
gate below simply engages less often — but it is not a fix, and it is not ours
to make.

## The fix

Close the loop, with the gate armed only by evidence and bounded by a deadline.

**Wire.** `NVKVM_EVT_TYPE_RELEASE` on the existing VQ_EVT channel
(`src/common/nvkvm_proto.h`), same 16 bytes as every other event, carrying
`(isolate_id, stub_handle)` — the pair the guest already names its bos by. No
new virtqueue, no new command, no broker change at all: the broker has been
sending `EV_RELEASE` on both the Wayland and X11 backends since before this.

**VMM.** The broker names a buffer by its host dma-buf **inode**, which the
guest has no name for. `nvkvm_display_relay_note_present()` records
inode → (isolate, GEM) for each zero-copy submit (a 16-entry
least-recently-noted table, `RELAY_BUFMAP_*`); the `EV_RELEASE` handler looks the
id up and forwards the translated pair with
`nvkvm_virtio_push_buf_release()`. A miss is normal, counted, and silent.

**Guest.** `nvkvm_pipe_update()` still queues the present and still arms the
flip event against the software vblank exactly as before. The one addition is
`nvkvm_bp_hold()`: if the buffer *this flip hands back to the compositor*
(`old_state->fb` — the one DRM's contract declares free the instant the event
goes out, and therefore the one that gets drawn into next) is one the host is
still holding, the driver takes ownership of the `drm_pending_vblank_event` and
delivers it later — when the release for that exact buffer arrives, or when a
deadline hrtimer fires.

Gating on the displaced buffer's identity, rather than on a count of buffers in
flight, is what keeps the common path untouched: when the host has already
released it — which is almost always, given the counter stopped moving — the
function returns false before taking a lock and nothing changes.

### Every no-release case, and what happens

The gate arms **only after a release for one of our own bos has actually
arrived**, and disarms after `NVKVM_KMS_BP_DISARM` = 4 consecutive deadline
expiries. That single rule covers most of the list by construction:

| case | what happens |
|---|---|
| **No broker attached** (`-display none`, headless CI, the Mint VM) | No release ever arrives, so `bp_armed` is never set. Not one flip is gated. Byte-for-byte the old behaviour. |
| **Older VMM / older QEMU** | Same: it never sends `NVKVM_EVT_TYPE_RELEASE`, the guest never arms. The event type was a previously-reserved value, so no version bump is needed. |
| **Readback tier** (cross-vendor host; also the shm rung) | The VMM submits its own LINEAR udmabuf, and deliberately does **not** call `note_present` there — the compositor never touches the guest's bo, so there is nothing to wait for. No mapped release ever arrives, the guest never arms, the tier is unaffected. |
| **Tier renegotiates zero-copy → readback mid-run** | Armed, then releases stop. At most 4 flips are gated, each for `kms_present_wait` vblank periods (default 2 ≈ 33 ms at 60 Hz), then the head disarms with one `pr_info` and returns to timer pacing. Bounded total ≈ 133 ms of added latency, once. |
| **Compositor exits / window unmapped / broker disconnects** | Identical to the row above: 4 gated flips, then disarm. The relay's own reconnect logic is untouched. |
| **Release event dropped** (VQ_EVT full) | One flip runs to its deadline and completes. Counted as a timeout; four in a row disarm. |
| **Release arrives for a bo we are not waiting on** | Untracked from the in-flight set, timeouts reset to 0, no flip is held. |
| **Deadline fires** | The flip completes, **and the entire in-flight set is cleared** — a host that missed one release is not trusted about the others, and stale entries would gate the next flip too. |
| **Pipe disabled / module unbound / head torn down** | `nvkvm_bp_flush()` delivers any held event before `drm_crtc_vblank_off()` and again in `nvkvm_kms_fini()` after the deadline timer is cancelled. A held event that is never delivered is a compositor that never draws again; that is the one outcome worse than the bug. |
| **Operator wants it gone** | `kms_present_wait` is `0644`. `echo 0 > /sys/module/nvkvm_guest/parameters/kms_present_wait` disables gating for all future flips at runtime; an already-held event is still bounded by its own deadline, which was computed when it was taken. |

Nothing else changes. The refresh-rate slaving that produced the 59.996 Hz hint
is untouched — the vblank hrtimer keeps running at the host rate, and the
vblank *clock* never stops (which is what broke weston's fade-in the last time
something interfered with it). Only the page-flip completion is held.

### Worst case, stated plainly

A flip is delayed by at most `kms_present_wait` (default 2, max 8) vblank
periods, **and never more than 250 ms whatever the period is**. The second bound
is not redundant: `kms_hz` accepts values down to 1, and the host's refresh hint
drives the period too, so "two vblank periods" on a 1 Hz head would be two
seconds — not backpressure, a hang, and closing on the 10-second `flip_done`
timeout the atomic helpers WARN at. At any refresh anyone actually runs the
period bound is the one that binds (~33 ms at 60 Hz default, ~133 ms at the
maximum) and the ceiling never engages. A guest that stutters is acceptable; a
guest that stops is not, and there is no path here where the flip event is not
delivered.

### Two hazards found in review and closed

Both were in the guest, both were the shape this project has been burned by
before, and both are worth knowing about if this code is ever moved:

1. **Use-after-free.** The deadline `hrtimer` was armed *after* dropping
   `pending_lock`, so a hold that had already passed the `!kms->stopping` check
   could re-arm it after `nvkvm_kms_fini()`'s last `hrtimer_cancel()` — leaving
   a live timer on a `drmm`-allocated head that `drm_dev_put()` then frees, with
   the handler taking `pending_lock` on freed memory. `nvkvm_drm_fini()` calls
   `nvkvm_kms_fini()` *before* `drm_dev_unregister()`, so a concurrent atomic
   commit is genuinely possible there. Arming now happens inside the same
   critical section as the check, which is what `nvkvm_pipe_enable_vblank()`
   already does with the vblank timer.

2. **A stale deadline completing the wrong flip.** When a release delivers
   flip N's event, N's deadline timer is still armed. Re-arming it for flip N+1
   is not enough: a callback already running and blocked on `pending_lock`
   cannot be recalled, and it would take N+1's event out early — completing a
   flip nobody waited for, wiping the in-flight set, and counting a timeout
   against a host that had just answered. The handler now compares `ktime_get()`
   against the stored absolute `bp_deadline` of the event it finds and re-arms
   instead of firing when it has woken for a generation that is already gone.

## What was not verified

**REUSE-IN-FLIGHT was not reproduced before the fix, and its absence after the
fix was not demonstrated.** Stated plainly rather than papered over:

* the physical PC was in use by its owner for the whole of this work and was
  not touched;
* the condition needs a real host compositor under real GPU load — on the PC
  itself it stopped firing after the first hour and has not fired since, so
  even with the hardware it is not a condition one can summon on demand;
* no vast.ai instance was rented. Reproducing this needs an L0 host with a GPU,
  an L1 KVM VM, an L2 nvkvm guest running a compositor, *and* a host compositor
  on the rented box promoting or compositing the guest's surface — the docs
  (`docs/internal/broker-design.md`, `known-limitations.md`) record that the
  host-compositor half has never been demonstrated headlessly on a rented box,
  and a negative result there would have said nothing.

What *was* verified is below. The behavioural claim that still needs hardware is
"a gated flip completes on the release rather than on the deadline" — check the
`present backpressure: N flips gated, M buffers released by the host, K deadline
expiries` line printed at unbind. `gated` with no `freed` means the host answered
once and stopped; `timeout` approaching `gated` means it is costing frames and
buying nothing, and `kms_present_wait=0` is the answer.

## Recorded, not fixed: the guest-resolution oscillation

Separate bug, **not touched by this branch**, recorded here because it is the
likely cause of the ~100 ms stalls and is much more likely to be what the user
actually feels. The broker logs bursts of guest resolution changes, three inside
~200 ms, then quiet for ~15 minutes:

```
20:12:03.576  1920x1080
20:12:03.657  2792x1572
20:12:03.792  1920x1080
```

The shape (1920 → high → 1920, settling in 2–3 iterations) is a negotiation
ring, and there is a closed loop that fits it exactly:

1. guest re-modes;
2. `nvkvm_display_relay.c` sees `width != r->last_w` and sends `CMD_WINDOW`;
3. the broker asks the compositor for a window that size;
4. the compositor configures something *near but not equal* to it — fractional
   output scale (`scale_120`), the broker's own titlebar, and the deliberate
   `pw &= ~7u` CVT-granularity round-down all bend it;
5. `wl_report_surface()` reports the configured size as `EV_SURFACE`;
6. the guest re-modes to *that* — back to (1).

`wl_report_surface()`'s comment says "reached only from host events … nothing a
guest does gets here". That is true of the immediate trigger and false of the
causal chain: the configure at (4) exists **because** of the guest's
`CMD_WINDOW` at (2). The `hint_w/hint_h` idempotence check terminates the ring
once the rounding reaches a fixed point, which is why it settles after 2–3
iterations instead of running forever. Each iteration is a real mode change with
buffer reallocation, so a burst is a visible hitch.

Worth checking before fixing: whether the guest should suppress `CMD_WINDOW` for
a mode change it made *in response to* an `EV_SURFACE`, which would break the
loop at (2) without any protocol change.

## Gates

Run on this branch, against the same gates run on `main` first as a control.

| gate | main (control) | this branch |
|---|---|---|
| `tests/unit/run_tests.sh` | UNIT SUITE OK, 16 suites | UNIT SUITE OK, **17** suites (`test_relay_bufmap`, 12/12) |
| `bash src/broker/selftest.sh` | 79/79 | **82/82** (+3, see below) |
| `bash tests/qemu_syntax_check.sh` | 8 files | 8 files |
| guest module, `make -C /lib/modules/$(uname -r)/build M=$PWD modules` | 5 warnings | **5 warnings, byte-identical set** |
| guest module, `NVKVM_GRAPHICS=0` (compute-only) | builds | builds, same 5 |

New coverage:

* `tests/unit/test_relay_bufmap.c` (12 cases) pins the inode → (isolate, GEM)
  translation the whole release path rests on, extracted from the production
  source between `NVKVM_RELAY_BUFMAP_BEGIN/END` markers so a copy cannot drift:
  re-noting an id replaces rather than shadows (inode reuse), a lookup does not
  consume the entry, zero ids are not stored, and eviction is
  least-recently-noted so the bos being cycled right now survive.
* three checks in `src/broker/selftest.sh` pin the property the VMM's table
  depends on and that nothing previously asserted: `EV_RELEASE`'s `w0`/`w1`
  carry the **same** id the `ATTACH` was given, and that id is never zero.
