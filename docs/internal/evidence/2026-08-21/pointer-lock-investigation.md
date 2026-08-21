# Pointer lock in the GTK window — what is ruled in and out, 2026-08-21

**Status: unresolved.** Written while the only machine that could test it was
being returned, so that whoever picks this up does not repeat the eliminations.

## The model that is wanted

Two modes, as specified by the user, and VirtualBox is the reference:

**Normal** — host tracks the mouse and hides its cursor over the window (the
guest draws its own); the pointer leaves the window whenever it likes and the
guest has no say; only coordinates are delivered, on hover; **a click is
forwarded to the guest like a click in a web app, and does NOT grab**; host OS
keyboard shortcuts are not intercepted; the guest sees an absolute pointer that
can teleport.

**Grab** — entered from a menu item or a shortcut, never as a side effect. The
guest tracks the mouse. The host stops tracking entirely: no cursor anywhere, no
coordinates. Real pointer lock. The guest sees relative motion only and does its
own tracking. Keyboard forwards fully except the escape combo. A guest
application taking its own pointer lock then works, because the host already
granted it.

## Confirmed defect, independent of the rest

`ui/gtk.c` ~line 995 implicitly grabs on the first left click while in relative
mode (`gd_grab_pointer(vc, "relative-mode-click")`). That contradicts the model
above and should be removed: grab is a setting or a shortcut. This does not
depend on the unresolved question below.

## What was measured, and what it eliminates

Tested live on the RTX 4070 box: `mouse_set 5` (make the relative virtio-mouse
current), then grab, then try to look around in Minecraft. **Motion did not
reach the game.** Host-side device switching is therefore NOT sufficient on its
own, which matters because that is exactly what a patch tying grab to
`qemu_mouse_set()` would do.

Ruled out as explanations:

- **Not `qemu_input_is_absolute()` returning the wrong thing.** It returns the
  first handler matching `REL|ABS` and tests its ABS bit (`ui/input.c:461`).
  With the relative mouse at the list head it correctly returns false.
- **Not the guest flipping the device back.** `info mice` after the failed test
  still showed `* Mouse #5: QEMU Virtio Mouse`. QEMU was in relative mode
  throughout.
- **Not a missing device in the guest.** `/proc/bus/input/devices` shows both
  `QEMU Virtio Tablet` (event5) and `QEMU Virtio Mouse` (event6).
- **Not missing Wayland protocol support.** libweston 13.0.0 exports
  `zwp_pointer_constraints_v1`, `zwp_relative_pointer_manager_v1` and
  `zwp_relative_pointer_v1`, and Xwayland has the relative pointer bound —
  `xinput list` on `:2` shows `xwayland-relative-pointer:16`.

So on the host side the deltas should have been queued: `gd_motion_event`
(`ui/gtk.c:893`) takes its relative branch at
`else if (s->last_set && s->ptr_owner == vc)` and calls `qemu_input_queue_rel`,
gated only on being grabbed.

## The part nobody has looked at

The delivery chain is longer than a normal VM's, and only its two ends were
checked:

    QEMU --relative--> virtio-mouse (event6) --> weston --> Wayland
    pointer-constraints / relative-pointer --> Xwayland (rootful, fullscreen)
    --> the X client

Xwayland here is a **weston client**, not an evdev reader — that is why
`xinput list` shows `xwayland-pointer:16` rather than the QEMU devices. Untested
links, in the order worth testing:

1. Does weston actually deliver relative motion from event6 while it also holds
   the tablet? A compositor with both an absolute and a relative pointer is the
   unusual part of this setup.
2. Does the X client's pointer grab become a Wayland pointer *constraint*, and
   does weston honour it?
3. Was the grab genuinely active during the test? "grab doesn't let me move
   mouse" is also consistent with the grab never being established.

A `wayland-info` in the guest, and weston's own log with pointer debug, would
settle (1) and (2) quickly on a machine with a display.

## Warning for whoever resumes this

Do not merge a patch that assumes host-side device switching is sufficient
without re-testing this end to end. The measurement above says it is not, and
the reason is not yet known.

---

# Second session: the answer, and it is the host

Ran after deploying `gpu-untested` (patches 0006 + 0007) to the RTX 4070 box.
This section supersedes the "unresolved" framing above for the *grab* half.

## Patch 0006 works

Confirmed by the user: a click no longer grabs. The click is forwarded, the
cursor stays visible, the pointer still leaves the window. That was the one
grab path a guest could arm, so this is a small security improvement as well as
the UX fix.

## Patch 0007 could never have worked on this host, and neither could any
## host-side patch of ours

Measured with `evtest` on `QEMU Virtio Mouse` (`event6`) inside the guest, while
the user grabbed and moved the mouse:

    real REL_X / REL_Y events captured: 0

And the user's description of "grab" is the diagnosis:

- host cursor **left the QEMU window freely while grabbed**, with the host still
  tracking it
- the guest's pointer was frozen

A grab that does not confine is not a grab. The cause:

    host session Type=wayland
    QEMU open fds referencing wayland-0: 0

**QEMU is an X11 client running under Xwayland.** Its GTK grab needs
`gdk_seat_grab()` to confine and `gdk_device_warp()` to re-centre, and an X11
client on Xwayland can do neither, because the Wayland compositor (mutter) owns
the real pointer. Once the pointer leaves the widget, GTK receives no motion, so
`gd_motion_event()` never reaches its relative branch and no deltas are ever
queued. Zero events is the expected result, not a surprise.

This is a QEMU-on-Wayland limitation. No change to `ui/gtk.c` fixes it, so 0007
should be judged on a host running an X11 session, where the grab primitives it
depends on actually work. It remains unproven either way.

## SDL is the right shape and does not currently work with nvkvm

`-display sdl,gl=on` runs QEMU as a **native Wayland client** -- 8 wayland
references against GTK's 0 -- which is the supported way to lock a pointer on
Wayland (`zwp_pointer_constraints_v1` + `zwp_relative_pointer_v1`), and SDL
implements relative mouse mode on top of them.

But the SDL window never showed the guest. With the default consoles it showed
the black VGA head; with `-vga none` it showed QEMU's placeholder,
"Guest has not initialized the display (yet)", so it was attached to a console
that never received a surface. nvkvm registers its own `QemuConsole` for the
scanout, and **patch 0005 -- which switches the window to that console when it
goes live -- is GTK-only**. SDL has no equivalent and no tabs to switch by hand.

So the display path and the pointer-lock path currently want different backends:
GTK renders and cannot grab; SDL could grab and does not render.

## What this means for the work

1. 0006 is good and independent. It could go to main on its own merit.
2. 0007 is untestable on a Wayland host. Test it on an X11 session before
   judging it.
3. The real fix for Wayland hosts is an SDL console story: teach the SDL backend
   the same "switch to the guest console when it goes live" behaviour patch 0005
   gives GTK. That is a new patch, not a modification of 0007, and nobody has
   written it.
4. Cosmetic, reported and unfixed: with GTK, the cursor is hidden over the whole
   drawing area including the padding outside the guest's render target, which
   makes the View menu awkward to hit.
5. Baseline measurement the previous session asked for: with BOTH devices
   attached, `info mice` at desktop time shows the tablet current
   (`* Mouse #4: QEMU Virtio Tablet (absolute)`). So normal mode is correct on
   boot even with the relative device present, which is the evidence that would
   justify making VM_RELATIVE_MOUSE default-on -- once there is a host where
   grab works at all.
