# The OOBE gamescope session does not reach the display

**Status: OPEN.** Valve's download button serves the OOBE image, so this blocks
the out-of-box flow for anyone installing from the official image. Measured on
the physical PC (RTX 4070, driver 595.84) 2026-08-29 against nvkvm-pv d135fe9.

## What is NOT the problem (ruled out with evidence)

- **Modifiers.** The plane advertises `XR24, AR24` with modifiers
  `0x300000000606014, 0x30000000060e014, 0x0` — LINEAR included, read straight
  out of the IN_FORMATS blob in the guest. The `modifier 0x0 not supported for
  scan-out` failure is fixed.
- **Vulkan.** `vulkan: selecting physical device 'NVIDIA GeForce RTX 4070'` and
  `physical device supports DRM format modifiers`. The earlier
  `vkCreateDevice failed (VkResult: -3)` was measured on a host whose BAR1 was
  exhausted and is retracted.
- **The five FIFO/scheduling controls.** No longer denied; only `0x0080170f`
  (SET_CHANNEL_PROPERTIES) and `0x2080220b` (RC_ENABLE_WATCHDOG, recorded
  harmless) appear in the deny log. An earlier claim that those five caused the
  Vulkan modifier failure was **wrong** — correlation, not cause.
- **Seat assignment.** `loginctl seat-status seat0` lists
  `[MASTER] drm:card0` and `[MASTER] drm:card0-Virtual-1`; udev tags are
  `master-of-seat:uaccess:seat`. The DRM node is correctly on the seat.
- **`zero modifiers for DRM format 0x38344241 / 0x38344258`.** Those are AB48 and
  XB48, the 16-bit-float HDR formats, not the 8-bit ones. We advertise XR24/AR24
  only, so this is expected and appears benign. (Misread as AR24/XR24 earlier.)

## ELIMINATION, completed 2026-08-29/30 with a two-machine crossover

A second machine settled this. A laptop (RTX 3050 Laptop, Ampere) runs the
gamescope session successfully; the PC (RTX 4070, Ada) does not. Both machines
were then driven to identical software, one variable at a time.

| variable | how it was excluded |
|---|---|
| our 89 nvkvm-pv commits since `d28d603` | the laptop was upgraded to `1dc47d4` and still works. The new module was verified LIVE (`kms_present_wait` present) and backpressure ACTIVE (=2), not merely absent |
| nvkvm-steamos | the `dbd1d26` + `d28d603` pair also fails on the PC |
| the disk image | **crossover**: the PC's qcow2 boots the OOBE wizard fine on the laptop; the laptop's qcow2 fails on the PC |
| SteamOS build / variant / gamescope version | both images fail on the PC and work on the laptop (3.8.14/`oobe`/gamescope .2 and 3.8.16/`steamdeck`/.4) |
| NVIDIA driver version | PC swapped 595.84 -> 580.173.02, byte-identical build string to the laptop. Still fails |
| kernel module flavour | open kernel module -> proprietary. Still fails |
| BAR1 aperture size | fails at both 256 MiB and 8 GiB |
| host compositor | both hosts run Wayland |
| session entry method | an sddm autologin override AND the official `steamosctl switch-to-game-mode` fail identically |
| **present tier** | `auto`, `NVKVM_BROKER_LINEAR_ONLY=1` and `NVKVM_BROKER_PRESENT_MODE=shm` all fail |

**Control, in every one of those configurations: Plasma/KWin renders on the PC**
(connector `enabled`, kwin_wayland + plasmashell, nvkvm refs ~1500). The GPU path
works; this is specific to gamescope.

### The present-tier test deserves its own note

The laptop's NVIDIA card has **no display output at all** (no MUX; GNOME runs on
the Intel iGPU), so its guest frames MUST cross GPUs and it can never take the
top rung of the present ladder. Forcing the PC onto the bottom rung (`shm`) --
the tier the laptop is structurally confined to -- does NOT reproduce the
laptop's success. So the cross-GPU present path is not what makes it work there.

### What is left

**GPU architecture (Ada AD104 vs Ampere GA107), and the host kernel.** With
everything else pinned, an architecture-dependent difference in what we forward
is the leading explanation. NEXT TEST: rent an Ampere box and run this stage on
it. Ampere working against Ada failing would confirm it and give a reproducible
target; both failing would point at the host kernel or the desktop-vs-laptop
platform.

## The two real findings

### 1. `TimeoutStartSec=5` is too short — confirmed, and a fix exists

```
gamescope-session.service: start operation timed out. Terminating.
gamescope-session.service: State 'stop-sigterm' timed out. Killing.
```

The unit is `Type=notify` with a 5 s start deadline. gamescope gets as far as
`Running compositor on wayland display 'gamescope-0'`, starts both Xwaylands and
connects pipewire, then is SIGKILLed for not signalling readiness in time.
SDDM then retries, which is the crash loop — and the loop is what exhausts host
BAR1.

A drop-in raising it (`/etc/systemd/user/gamescope-session.service.d/`,
`TimeoutStartSec=120`) **works**: the session then lives ~60 s instead of 5 s.
That is a candidate product fix — nvkvm's init is slower than bare metal because
every RM ioctl is forwarded, so a 5 s deadline tuned for a Steam Deck is simply
wrong here.

### 2. gamescope runs HEADLESS, which is why nothing is ever displayed

```
wlserver: [backend/headless/backend.c:17] Starting headless backend
```

With no DRM backend there is no output to enable and no flip to make: the
connector stays `enabled=disabled` and the host-side flip count stays 0, no
matter how long the session is allowed to live. **Extending the timeout changes
the failure but does not fix the display.** This is the open question.

Not yet determined: whether wlroots is choosing headless because libseat failed
(`Could not connect to socket /run/seatd.sock` — seatd is inactive and disabled;
whether it then falls back to the logind backend was not established), because
`WLR_BACKENDS` is set somewhere in SteamOS's session scripts, or for another
reason. That is the next thing to find out, and it is the whole ballgame.

## How to work on this without damaging the host

Each crash cycle leaks host BAR1 that is not returned on process exit (see the
BAR1 note). Measured here: ~9 MiB for one manual start, ~28 MiB for two sddm
cycles, and the aperture fell from 131 MiB free to 64 MiB free over one
afternoon's testing. So:

- keep `sddm` stopped and start it for exactly one cycle at a time,
- check `nvidia-smi -q | grep -A3 "BAR1 Memory Usage"` before and after,
- recover when Free drops under ~50 MiB, and re-baseline after,
- never leave a retry loop running unattended.

**Recovery is a module reload, not a reboot** (~30 s, measured 68 -> 223 MiB
free):

```
systemctl stop gdm3          # the desktop pins the modules
rmmod nvidia_drm nvidia_modeset nvidia_uvm nvidia
modprobe nvidia_drm modeset=1
systemctl start gdm3
```

Do NOT use `nvidia-smi --query-compute-apps` to argue that nothing holds the
GPU -- it lists only COMPUTE contexts, so a desktop compositor never shows up
there. Use `fuser -v /dev/nvidia0`.

Where the leak lives, measured: with the stack down, the desktop stopped and
`fuser /dev/nvidia0` empty, BAR1 still read `used=188MiB`. All four `nvidia*`
modules then unloaded with plain `rmmod` -- nothing pinning them -- and a reload
returned it to `used=33MiB`. The host desktop accounts for ~1 MiB of that. Since
nvkvm has NO host kernel module (our host side is QEMU plus the isolate stub,
both userspace), the retained state is inside NVIDIA'"'"'s driver.

KWin/Plasma does reach the display on this same build (connector enabled, flips
counted), so the DRM path itself works — this is specific to the gamescope
session.

---

## CORRECTION (2026-08-30): "falls back to headless" was a misread log line

Everything above that describes gamescope *falling back* to the headless backend
is **wrong**, and the error it was chasing does not exist.

From gamescope's own source (`src/wlserver.cpp`, `wlserver_init`):

```c
wlserver.wlr.headless_backend = wlr_headless_backend_create( wlserver.event_loop );
```

That call is **unconditional**. gamescope always creates a headless *wlroots*
backend for the nested compositor games connect to, and drives the display
separately through its own DRM backend. The line

```
[Info] wlserver: [backend/headless/backend.c:17] Starting headless backend
```

is prefixed `wlserver:` and comes from wlroots — it appears in **every**
gamescope run, working ones included. It is not a fallback, not an error, and
not evidence of anything.

The two `[Error]` lines we chased are likewise non-fatal, confirmed from source
(`src/Backends/DRMBackend.cpp`):

- `Immediate flips are not supported by the KMS driver` — sets
  `g_bSupportsAsyncFlips = false`, logs, and continues. No `return false`.
  **Do not implement `DRM_CAP_ASYNC_PAGE_FLIP` to "fix" this**; it would mean
  claiming we honour `DRM_MODE_PAGE_FLIP_ASYNC` while our virtual head
  deliberately paces flips off a software vblank with backpressure (the
  mechanism that stops the guest lapping the host's scanout ring), i.e. trading
  a real safety property for nothing.
- `Syncobjs are not supported by the KMS driver` — the `else` branch only logs.

### What the evidence actually says

On the failing run, gamescope gets **further than anyone credited**:

```
drm: opening DRM node '/dev/dri/card0'
drm: Connectors:
drm:   Virtual-1 (connected)
drm: selecting connector Virtual-1
drm: selecting mode 1920x1080@60Hz
```

and **none of `init_drm`'s seven `return false` strings appear anywhere in the
log** — no ATOMIC cap failure, no dummy-syncobj failure, no `get_resources()`
failure, no missing primary plane, no "doesn't support any formats >= 8888", no
page-flip pipe failure. The primary plane advertises `XR24 AR24` (both ≥8888)
and `DRM_CAP_ADDFB2_MODIFIERS` is set.

So the DRM backend appears to initialise and select our connector and mode.
**The failure is somewhere after mode selection**, not in backend choice.

### Where to look next

The older evidence in this file — connector `enabled=disabled`, zero flips, zero
xwm clients — is still valid and is now the *only* live symptom. Two candidate
readings, neither yet tested:

1. gamescope commits and the atomic commit fails or never flips (look at the
   commit path and our KMS `atomic_check`/flush, and at host-side
   `nvkvm present: flip` counters).
2. gamescope is up and healthy, and nothing ever connects to it — "zero xwm
   clients" would then mean the OOBE/Steam client failed to launch inside the
   session, which is not an nvkvm problem at all.

Distinguish those before writing any more code. The one real gap this
investigation did close is `DRIVER_SYNCOBJ` (see the commit that added it):
that cap was genuinely missing, and gamescope now exercises its syncobj path
successfully — but it was never the reason for the symptom.
