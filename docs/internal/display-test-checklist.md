# Display-path test checklist

For anyone sitting in front of a machine with an NVIDIA GPU, a monitor, and
20 minutes. The display path is the least-tested part of nvkvm and the hardest
to test: cloud and container hosts cannot exercise it at all, because NVKMS
plane handling needs a real KMS device with a display attached. Every bug found
on 2026-08-21 was in this path, and that is not a coincidence -- it is the only
part with no CI and no rentable hardware.

Run these in order. Each step says what correct looks like and what a failure
tells you, so a negative result is still worth recording.

## 0. Record the environment FIRST

This is not bookkeeping. **The host session type changes the result**, and a
report without it cannot be compared to any other report.

```
nvidia-smi --query-gpu=name,driver_version --format=csv,noheader
echo "$XDG_SESSION_TYPE"          # x11 or wayland -- LOAD BEARING
loginctl show-session $(loginctl list-sessions --no-legend | awk '{print $1}' | head -1) -p Type
ls /dev/kvm && uname -r
```

Also record the compositor (GNOME/mutter, KDE, etc.) and whether the display is
attached to the NVIDIA GPU or to an iGPU. On a hybrid laptop the window may be
rendered by Intel/AMD even though the render node is NVIDIA, which changes what
the present path can do.

## 1. Does the guest desktop render at all

```
NVKVM_QEMU_UI=1 bash scripts/build_qemu.sh --force
VM_DISPLAY="gtk,gl=on" bash scripts/run_test_vm.sh
```

Expect a Mint desktop in the QEMU window. In the QEMU log expect
`nvkvm: guest display is live -- switched window to console page 1`.

If the window shows the boot console or "Guest has not initialized the display
(yet)", the console auto-switch (patch 0005) did not fire -- record the backend.

## 2. Present mode A/B

Default is `readback`, which copies every frame GPU->CPU->GPU. On a host whose
desktop is rendered by the SAME NVIDIA GPU, the zero-copy path should work:

```
NVKVM_PRESENT_MODE=gl VM_DISPLAY="gtk,gl=on" bash scripts/run_test_vm.sh
```

Log should say `window mode=GL-zerocopy`. Compare responsiveness against
readback, and note any flicker in EITHER mode -- readback has an 8-entry import
cache whose eviction policy was pathological before 28c59ee, and tooltips and
menus are what exhaust it.

A blank window on `gl` means the window's own renderer cannot import an NVIDIA
dma-buf. That is the expected failure on a hybrid or non-NVIDIA-rendered
desktop, and it is why the default is readback.

## 3. Resolution

`xrandr` inside the guest session should report 1920x1080, matching the scanout.
If it reports 5120x2880, the mode pin lost a race with
cinnamon-settings-daemon; see `scripts/setup_mint_guest.sh`.

## 4. Input -- normal mode

With the default (tablet only, no `VM_RELATIVE_MOUSE`):

- hovering moves the guest pointer 1:1, and it can teleport
- the pointer leaves the window freely
- **a click is forwarded and does NOT grab** (this is patch 0006; before it, a
  click stole pointer and keyboard)
- host OS keyboard shortcuts still work

## 5. Input -- grab, and this is where the host session type decides

```
VM_RELATIVE_MOUSE=1 NVKVM_PRESENT_MODE=gl VM_DISPLAY="gtk,gl=on" bash scripts/run_test_vm.sh
```

In the guest, before grabbing, watch the relative device:

```
EV=$(grep -E '^N: Name=.QEMU Virtio Mouse' -A4 /proc/bus/input/devices | grep -oE 'event[0-9]+' | head -1)
sudo evtest /dev/input/$EV
```

Then Ctrl+Alt+G in the QEMU window and move the mouse.

- `REL_X`/`REL_Y` events ⇒ the whole host->guest chain works
- silence ⇒ nothing relative is being delivered

**On a Wayland host, expect silence, and it is not an nvkvm bug.** QEMU's GTK
backend never asks the compositor for a pointer lock -- it emulates one with
`gdk_seat_grab()` plus `gdk_device_warp()`, and an X11 client under Xwayland
cannot confine or warp a pointer the compositor owns. Measured 2026-08-21:
cursor walked out of the "grabbed" window, zero REL events.

**On an X11 host those primitives work**, so this is the environment that can
actually judge patch 0007 (`gpu-untested`). Nobody has run it there yet. If you
have one box to spend on this, spend it here.

## 6. GPU memory -- does it come back

Known-good numbers from an RTX 4070, driver 595.84, after the xiso fix:

```
# baseline, then 40 short-lived GL clients, then settle 30s, twice
/home/mint/exp/guest_batch.sh 40 1280x720+50+50
```

| round | delta |
|---|---|
| 1 | +15 MiB (warm-up) |
| 2 | -1 MiB |

Round 2 is the control: it separates one-off from linear. A per-round delta that
keeps growing is a leak; each client's buffer is ~3.5 MiB at 720p, so 40 leaked
buffers would be ~140 MiB per round and obvious.

Also worth doing if you have the time: leave a real desktop running for a couple
of hours and sample `nvidia-smi` per process. The original failure was ~1.2 GB
growing to ~11.5 GB over 2.5 hours until NVKMS allocations began failing, which
looks like glitching on every keypress and ends in a black screen.

## 7. Things only this environment can test

Worth attempting on any box that gets this far, because nothing else can:

- NVKMS surface registration and the vblank-semaphore paths. The allowlist
  numbering is unit-tested across 28 driver tags, but no test proves the ioctls
  behave.
- A driver branch we have not measured. The NVKMS command enum is renumbered by
  NVIDIA mid-branch -- 570.195 and 570.207 disagree -- so a new major, or a new
  minor within 570, is genuinely new coverage.
- Whatever the display does under sustained load: a game, a video, a
  compositor-heavy desktop.

## Reporting

Record the environment block from step 0 with every result, including negatives.
"Grab produced no events on GNOME/Wayland, driver 595.84, RTX 4070" is a useful
data point; "grab did not work" is not.
