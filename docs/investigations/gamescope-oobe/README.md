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
- reboot the host when Free drops under ~50 MiB, and re-baseline after,
- never leave a retry loop running unattended.

KWin/Plasma does reach the display on this same build (connector enabled, flips
counted), so the DRM path itself works — this is specific to the gamescope
session.
