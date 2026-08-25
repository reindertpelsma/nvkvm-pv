# nvkvm display broker

A privileged program that owns the window, the display-server connection and
the input grab, so that the nvkvm VMM (a patched QEMU) can run **sandboxed**
and still render **zero-copy**.

```
guest compositor                 QEMU (sandboxed)              broker (your session)
  |                                  |                              |
  | allocates its scanout bo         |                              |
  | through the forwarded            |                              |
  | render node                      |                              |
  v                                  |                              |
 isolate: the real RM object         |                              |
  |  PRIME_HANDLE_TO_FD              |                              |
  +---- host dma-buf fd ---SCM_RIGHTS--->  relays the SAME fd --SCM_RIGHTS-->
                                     |                              |
                                     |            zwp_linux_dmabuf_v1 -> wl_buffer
                                     |            or DRI3 -> X pixmap -> Present
                                     |                              |
                                     <---- input, pacing -----------+
```

One physical allocation, seen as the guest's scanout bo, the isolate's RM
object, a host dma-buf, and a `wl_buffer` (or an X pixmap). Nothing is copied
anywhere along that chain. On Wayland with direct surface attach it is **one
blit fewer than today**, because QEMU's GTK/SDL path imports the dma-buf as a
texture and blits it into a window framebuffer it owns, and this does not.

---

## 1. Why it exists

nvkvm's VMM has produced multiple real guest→host vulnerabilities, including an
arbitrary-address pin primitive reachable from a guest kernel. The obvious
response is to sandbox it. The obvious obstacle is that a VMM which draws on
your screen needs a display-server connection, and **an X11 socket inside a
sandbox is close to no sandbox at all** — X11 has no inter-client isolation, so
any client can read any other client's keystrokes and windows.

The broker removes that requirement. In broker mode QEMU holds **one unix
socket** and nothing else display-related:

| container profile | QEMU needs | QEMU does NOT need |
|---|---|---|
| **display only** | the virtio device, the isolate sockets, the broker socket | no EGL, no GL, no `libnvidia-eglcore`, no `/dev/dri/renderD*`, no X11 or Wayland socket |
| **display + capture/NVENC** | the above, **plus** EGL and a render node for `nvkvm_present_capture()` | still no display server, still no X11/Wayland socket |

That is not an aspiration: `-display nvkvm-broker` compiles and runs in a QEMU
built with `--disable-opengl` (see §7), because the relay lives **outside** the
`#if defined(CONFIG_OPENGL)` gate that `nvkvm_present_egl.c` sits under.

The justification is privilege separation. Copy count is a secondary, real, but
smaller win — see `docs/internal/display-broker-findings.md` finding 3 before
arguing about frames per second.

---

## 2. The direction of the buffer

**The buffer originates in the guest and travels up.** This is the fact three
earlier broker designs died on, and it is worth stating plainly because every
"the broker allocates and hands a buffer down" idea contradicts it:

- The guest compositor allocates its own scanout bo through the forwarded
  render node and negotiates its own modifier. There is no interface for
  "render into this buffer I am handing you" that does not mean patching every
  guest compositor.
- The present path (`nvkvm_req_present`) is a **lookup**: it discovers which
  host buffer the guest already chose, and asks the owning isolate's stub to
  PRIME-export it. QEMU is handed the resulting dma-buf fd over `SCM_RIGHTS`.
  **This already happens today, every frame.**
- Broker mode changes exactly one thing at exactly that point: instead of
  handing the fd to a UI backend, QEMU relays it onward.

The full reasoning, including the three rejected designs (DRM lease, buffer
substitution in the stub, copy-engine blit in the isolate) is in
`docs/internal/display-broker-findings.md`. Read it before proposing a fourth.

---

## 3. Threat model

**The broker treats the VMM, and every socket client, as hostile.**

The broker runs as your desktop user, outside the sandbox, holding your
keyboard. Whatever it accepts, it hands to a compositor or an X server that
also runs as you and is also outside the sandbox. So a lie the broker believes
is a bug in an unsandboxed process, which is the whole thing we were trying to
avoid.

Seven rules, all in `nvkvm_broker.c`, all exercised by `selftest.sh`:

1. **Declared geometry is validated against the REAL buffer size.**
   `lseek(fd, 0, SEEK_END)` measures the dma-buf; the frame is accepted only if
   `offset + stride * height <= size`, computed in 64-bit so two `uint32`
   operands cannot wrap it, and only if `stride >= width * bpp`.
   **Reject, never clamp.** This is finding **A-18** in
   `docs/internal/audit-boundaries-2026-08-20.md`, found once already one
   process further in; here the consumer that would read out of bounds is the
   *compositor*.
2. **Dimensions are clamped to `NVKVM_BROKER_MAX_DIM` (8192).** Same bound as
   `NVKVM_PRESENT_MAX_DIM` in `nvkvm_isolate_handlers.c`, which exists because
   finding **S-3** caught guest-controlled geometry landing in the VMM
   unexamined. Restated here because in this model the VMM is the attacker, so
   its enforcement is not evidence.
3. **fourcc and modifier are validated against what the GPU actually
   advertises** — `zwp_linux_dmabuf_v1`'s `format`/`modifier` events on
   Wayland, `DRI3GetSupportedModifiers` on X11. There is no hardcoded modifier
   list anywhere in the broker, because NVIDIA's block-linear modifiers are
   driver-version-specific (`src/guest/nvkvm_kms.c` carries two of them, read
   off real bos) and a hardcoded list would be wrong on the next driver.
4. **The fd is proved to be a dma-buf** before anything imports it:
   `fstatfs(fd).f_type == DMA_BUF_MAGIC`. A memfd, a pipe, a socket, a file on
   your disk — all rejected. (`--backend test` also accepts a memfd; that is
   why it is unreachable from `--backend auto` and prints a banner.)
5. **Fd intake is bounded.** Exactly one fd may accompany one command; a second
   is a protocol violation. Imported buffers live in a fixed 8-slot table and
   the least recently used is **destroyed and closed** on eviction, never the
   one the display is currently holding.
6. **Malformed or oversized ⇒ disconnect, never partial recovery.** Commands
   are a fixed 40 bytes; there is no length field anywhere and no resync path.
   Reserved fields must be zero. An unknown command type ends the connection.
7. **`SO_PEERCRED` is checked before a single byte is sent.** The kernel fills
   it at `connect()` time from the peer's credentials, so the peer cannot forge
   it — and unlike the socket's file mode it still holds if somebody widened
   the permissions. The socket itself is created `0600` under a `umask`, so it
   is never briefly world-writable. An adopted listener must actually be an
   `AF_UNIX` `SOCK_STREAM`; `--no-peercred` is refused for adopted descriptors,
   because configured mode bits were never applied to them and prove nothing.

Two more properties that are policy rather than parsing:

- **A rejected frame is not a disconnect.** The descriptor originates in the
  guest, so a guest flipping nonsense must not be able to kill the display of a
  VMM that is behaving correctly. The frame is dropped, the reason is logged,
  the connection lives. Only *protocol* violations disconnect.
- **One client at a time, and the incumbent is never displaced.** A second
  allowed-uid process cannot steal the display out from under a running VM.

The broker also drops what it does not need: `PR_SET_DUMPABLE 0`,
`PR_SET_NO_NEW_PRIVS`, and `--drop-user` (it retains no capabilities — the
display connection and input are already open fds by then). It is normally not
started as root at all; it is a session program.

---

## 4. Wire protocol

Defined once, in `src/common/nvkvm_broker_proto.h`, and included verbatim by
both sides. Little-endian, fixed-size records, no length fields, version 2.

**The wire format is identical on Wayland and X11.** The VMM cannot tell which
backend is running, and must not be able to: a VMM that could would eventually
contain a bug conditional on it.

### VMM → broker: `struct nvkvm_broker_cmd`, exactly 40 bytes

| offset | field | notes |
|---|---|---|
| 0 | `uint16 type` | `ATTACH` 1, `COMMIT` 2, `WINDOW` 3 |
| 2 | `uint16 reserved0` | must be 0 |
| 4 | `uint32 width` | |
| 8 | `uint32 height` | |
| 12 | `uint32 stride` | bytes per row of plane 0 |
| 16 | `uint32 offset` | byte offset of plane 0 in the dma-buf |
| 20 | `uint32 fourcc` | `DRM_FORMAT_*` |
| 24 | `uint64 modifier` | `DRM_FORMAT_MOD_*` |
| 32 | `uint32 seq` | advisory, logged only |
| 36 | `uint32 reserved1` | must be 0 |

- **`ATTACH`** carries exactly one fd as `SCM_RIGHTS`, which must be a dma-buf.
  The descriptor fields describe it. The broker validates (§3), imports, and
  closes its copy; the buffer stays alive through the `wl_buffer`/pixmap.
- **`COMMIT`** presents the most recently attached buffer. No fd, and every
  descriptor field must be zero. Split from `ATTACH` because a compositor
  distinguishes "the content changed" from "the frame is finished", and
  committing a half-drawn buffer is visible.
- **`WINDOW`** asks for a window of `width`×`height` on guest resolution
  change. It is a request: the window manager may ignore it, and the size that
  actually took effect comes back as `EV_SURFACE`.

Single-plane only, on purpose: the nvkvm guest head advertises XRGB8888 and
ARGB8888 (`src/guest/nvkvm_kms.c`), both single-plane, so multi-plane support
would be untested code on the privileged side. A multi-plane format is
rejected as an unadvertised fourcc.

### broker → VMM: `struct nvkvm_broker_pkt`, exactly 24 bytes

`{ uint16 type; uint16 flags; uint32 seq; int32 x; int32 y; uint32 w0; uint32 w1; }`

| type | meaning |
|---|---|
| `HELLO` 1 | `w0` = protocol version, `w1` = capability bits. Always first. |
| `SURFACE` 2 | `x`,`y` = the broker's window size. At attach and on every resize. |
| `FRAME` 3 | the display is ready for another frame (wl frame callback / `PresentCompleteNotify`) |
| `RELEASE` 4 | `w0`,`w1` = low,high 32 bits of the buffer id (its dma-buf inode) — no longer being read |
| `KEY` 5 | `x` = Linux evdev keycode, `y` = down |
| `BTN` 6 | `x` = Linux evdev `BTN_*`, `y` = down |
| `ABS` 7 | `x`,`y` = position, `w0`,`w1` = the range. Ungrabbed, pointer over the window, only. |
| `REL` 8 | `x`,`y` = delta. Grabbed only. |
| `WHEEL` 9 | `x` = vertical detents, `y` = horizontal |
| `GRAB` 10 | `x` = 1 on / 0 off |
| `FOCUS` 11 | `x` = 1 active / 0 inactive. While 0 no input at all is sent. |
| `POINTER` 12 | `x` = 1 pointer over the window |
| `BYE` 13 | `x` = reason (0 shutdown, 1 display lost, 2 protocol) |

`flags` mirrors grab and focus state on **every** packet, so the client can
never disagree with the broker about it whatever it did with the `GRAB` event.

Capability bits in `HELLO.w1`: `KEYBOARD`, `ABS_POINTER`, `REL_POINTER`,
`POINTER_LOCK`, `TOTAL_GRAB`, `FOCUS_EVENTS`, `FULLSCREEN`, `DMABUF`,
`MODIFIERS`, `RELEASE`.

### Backpressure, and the rule it enforces

**Input must never block on rendering, and rendering must never block on
input.** This project shipped that bug once — a laggy mouse whenever rendering
was slow — so both sides are non-blocking by construction:

- The broker's outbound queue is a fixed 512-packet ring. Under pressure it
  **coalesces motion**: absolute is latest-wins, relative deltas are summed, so
  a burst collapses to one packet and the pointer still ends up in the same
  place. **Key and button events are never dropped** — a press whose release
  was dropped leaves a stuck modifier in the guest. If the backlog is genuinely
  all keystrokes the client is disconnected instead of the events being lost.
- Reads from the client and writes to the display server are both
  `MSG_DONTWAIT` / non-blocking; a compositor socket that will not take a write
  gets `POLLOUT` on the next `poll()` rather than a blocking flush.
- On the QEMU side the relay sends with `MSG_DONTWAIT` from the virtio worker
  thread. If the socket is full the frame is **dropped and counted**: the guest
  produced faster than the display can consume and the next flip carries a
  newer buffer. A vCPU is never blocked on the display.

One honest exception, stated rather than hidden: the **X11 backend's import**
uses `xcb_request_check()`, which is a blocking round trip to the X server. It
runs once per *new* buffer — three or four times for the whole life of a VM,
because the 8-slot cache catches every repeat — not once per frame. It is there
because an unchecked DRI3 error arrives later as an event with nothing to
attribute it to, and the pixmap id silently refers to nothing: the choice is
between a sub-millisecond stall a handful of times and a black window with no
explanation. Wayland has no equivalent: `create_immed` reports failure without
a round trip, which is why it is used in preference to `create`.

---

## 5. Grab, focus and hotkeys

Owned by the broker, never by the VMM.

- **`CTRL+ALT+G`** toggles grab. Under grab all keyboard is captured and the
  pointer is locked; ungrabbed, absolute coordinates are sent only while the
  pointer is over the window.
- **`CTRL+ALT+F`** toggles fullscreen.
- Both chords, and their key-ups, are **consumed** — they never reach the guest.
- Input flows **only while the window is active**.

**Grab exits automatically on focus loss. This is a security property, not a
convenience:** a grab that survives focus loss is a keylogger — you alt-tab to
your password manager and the guest keeps receiving the keystrokes. Focus-out
is `FocusOut` on X11 and `wl_keyboard.leave` on Wayland. On the transition the
broker synthesises a release for every key the client believes is down, so no
modifier is left latched.

**If a session cannot observe focus loss, grab is not offered at all.**
`CTRL+ALT+G` refuses and says why.

**Capability is detected and announced at startup**, in one plain line, and
again in `HELLO`. On Wayland `keyboard-shortcuts-inhibit`, `pointer-constraints`
and `relative-pointer` are optional protocols a compositor may not advertise; a
partial grab is fine **if announced**, and a partial grab claimed as total is
not. The startup log says exactly one of:

```
GRAB IS TOTAL: under CTRL+ALT+G every key reaches the guest and the pointer is locked to the window.
GRAB IS PARTIAL: compositor is missing: keyboard-shortcuts-inhibit (Super and other compositor shortcuts WILL still fire under grab); ...
GRAB IS NOT OFFERED: this session cannot report focus loss, ...
```

Nothing any X client can do intercepts input the kernel handles below X (SysRq,
VT switching on some setups). That is in the caveat string rather than claimed
away.

---

## 6. Running it

### Build

```bash
cd src/broker && make          # prints which backends it kept
make check                     # build + selftest.sh
```

Dependencies — note what is **not** there: no libdrm, no EGL, no GL, no GBM.

```
Wayland backend    libwayland-dev wayland-protocols
X11 backend        libxcb1-dev libxcb-dri3-dev libxcb-present-dev
X11 rel. motion    libxcb-xinput-dev     (optional; without it, no relative
                                          motion under grab, and it says so)
```

### Run

```bash
# in your desktop session, as your normal user
nvkvm-display-broker --socket /run/nvkvm/display.sock --size 1920x1080

# then, in the sandbox
qemu-system-x86_64 ... -display nvkvm-broker,socket=/run/nvkvm/display.sock
```

Before involving QEMU, prove the broker alone works:

```bash
nvkvm-broker-testclient /run/nvkvm/display.sock --present 640x480
```

That prints the handshake and every input event, and sends one buffer. It
separates a broker problem from a QEMU problem, which is otherwise two
variables at once. Its `--bad-*` options are the ways a hostile VMM can lie;
each one must be rejected.

Options: `--backend auto|wayland|x11|test`, `--size WxH`, `--title`,
`--allow-user`, `--allow-group`, `--drop-user`, `--verbose`.
`--allow-group` compares the primary gid reported by `SO_PEERCRED`; Linux does
not expose the peer's supplementary groups through that interface.
By default root and the invoking user may connect.

Clipboard modes are `off` (default), `guest-to-host`, and `consent`. `consent`
adds host-to-guest transfer only after an explicit paste chord. Text is UTF-8
and capped at 7 KiB (7168 bytes) in either direction. Automatic `full` sync is
not implemented and is rejected rather than behaving exactly like `consent`.
The Wayland backend implements clipboard transfer; X11 rejects every non-off
mode. The explicit test backend has a simulated clipboard for regression tests.

### Putting the socket into a container

The broker socket is the **only** display-related thing the VMM needs. Bind-mount
the directory (not the socket file — a bind-mounted socket inode breaks if the
broker restarts and recreates it):

```bash
# podman / docker
--volume /run/nvkvm:/run/nvkvm

# systemd-nspawn
--bind=/run/nvkvm

# bubblewrap
--bind /run/nvkvm /run/nvkvm
```

and **do not** bind `/tmp/.X11-unix`, `$XDG_RUNTIME_DIR/wayland-*`, or
`/dev/dri/renderD*` for the display-only profile.

The capture/NVENC profile additionally needs `/dev/dri/renderD*` and the NVIDIA
userspace stack inside the container — but still no display server.

### What this actually costs, measured (RTX 3090 box, podman 3.4)

Both profiles were run. **Display only**, QEMU configured
`--disable-opengl --audio-drv-list=`:

```
DT_NEEDED: libgnutls libpixman-1 libpng16 libz libudev liblzo2 libfdt
           libgio-2.0 libgobject-2.0 libglib-2.0 libslirp libdw libaio
           libgmodule-2.0 libm libc
```

Not one X, GL, EGL, GBM, DRM, epoxy or Wayland library — and `-display help`
still lists `nvkvm-broker` (`egl-headless` is gone). Inside the container
`/dev/dri` does not exist, `/tmp/.X11-unix` does not exist, `DISPLAY` and
`WAYLAND_DISPLAY` are empty, and QEMU logs *"broker mode active: this QEMU
holds no display-server connection and imports nothing"*. **A frame reached
the screen from that container**, relayed by a helper linking libc alone.

Two details worth keeping:

- `--disable-opengl` **alone** still leaves `libX11` in the image, pulled in
  transitively by **`libpulse`** — QEMU itself references zero X symbols.
  Dropping the audio backend removes it. If a graphics-free container is the
  claim being made, drop audio too or say why libX11 is there.
- The BIOS blobs must come along (`-L`, or copy `share/qemu`), which is easy to
  forget when the container is this thin.

### `SO_PEERCRED` and rootless podman, precisely

`SO_PEERCRED` reports the peer's uid **as seen in the broker's user
namespace**. Measured, with the broker running as uid 1000:

| rootless invocation | broker sees | result |
|---|---|---|
| default (container root) | `uid 1000` | **works** — container root maps to the host user |
| `--user 1000:100` | *nothing* | **fails earlier than you expect** |
| `--userns=keep-id` | `uid 1000` | **works** |

The middle row is the trap, and it does not fail where §3 rule 7 would suggest:
container uid 1000 maps into the **subuid** range (100000+), and the socket is
`0600` owned by the broker's uid, so `connect()` returns **`EPERM` before
`SO_PEERCRED` is ever consulted**. Adding the mapped uid to `--allow-user` is
therefore *not enough* on its own — the socket's mode has to let that uid open
it too. **`--userns=keep-id` is the clean answer** and needs no `--allow-user`
at all.

### Fullscreen is where the frame reaches a hardware plane

Neither stack lets a client *request* a hardware plane; you can only qualify
for one, and the compositor decides:

- **Wayland**: a fullscreen, opaque, unoccluded surface whose format and
  modifier suit the plane is promoted to **direct scanout**. The broker sets an
  opaque region on every commit for exactly this reason.
- **X11**: a compositing WM **unredirects** a fullscreen window and the frame
  goes straight to the CRTC.

`CTRL+ALT+F` asks for the precondition. Whether it is granted is the
compositor's call, and it will not tell you.

---

## 7. What is verified, and what is not

This matters more than anything else in this document.

It was originally written on a machine with **no GPU and no `/dev/dri` at
all**. It has since been run on real hardware once: a rented KVM box with an
**RTX 3090**, NVIDIA **580.105.08**, X.Org 1.21.1.4 driving the real **NVIDIA
DDX** on a forced virtual head (`ConnectedMonitor "DFP-0"`) under KDE Plasma,
and **sway 1.7 / wlroots 0.15** headless on the same GPU. That run is the
source of everything marked *(hardware, RTX 3090)* below. **It has still never
met a physical monitor**, which is exactly why the scanout question below is
still open.

### First light — both backends, on real hardware

**A guest-shaped dma-buf reaches the screen on both backends.** The test buffer
is allocated through GBM on the NVIDIA render node and comes back with
modifier **`0x0300000000606014`** — one of the two block-linear modifiers
`src/guest/nvkvm_kms.c` records off real guest bos — so it is representative
of what a guest actually flips, not a linear stand-in.

- **X11**: `DRI3PixmapFromBuffers` + `PresentPixmap`, no GL in the broker.
  Frame correct, stride and offset correct.
- **Wayland**: `zwp_linux_buffer_params_v1` → `create_immed` →
  `wl_surface_attach`, bound at version 3. Frame correct.

Getting there took **three fixes, none of which any GPU-less test could have
found**; each is described where it lives:

1. **`nb_session_x11.c` — the DRI3 version gate was wrong on the one driver
   that matters.** The NVIDIA DDX reports **DRI3 1.0** and answers the 1.2
   `GetSupportedModifiers` request anyway, returning **twelve** block-linear
   modifiers. Gated on the version, the broker never asked, advertised only
   `DRM_FORMAT_MOD_INVALID`, and rejected every real frame. It now asks and
   lets the reply decide. Note also **0 window modifiers, 12 screen
   modifiers** — reading both lists, which the code already did, is what makes
   this work at all.
2. **`nb_common.c` — the 256-pair format table overflowed on a real
   compositor.** sway advertises every format the driver knows times thirteen
   modifiers; the table filled with `AB24`/`XB24`/`R8`/… in enumeration order
   and **`XR24`, the format the guest head actually flips, never got in**.
   Every frame rejected, black window. `nb_formats_add()` now drops any fourcc
   `nb_fourcc_bpp()` would reject anyway — 256 pairs became 56.
3. **`nb_session_x11.c` — the grab dropped itself instantly.**
   `XGrabKeyboard` generates its own `FocusOut` with `mode == NotifyGrab`, the
   focus-loss rule fired on it, and every grab lasted milliseconds
   (`GRAB 1; FOCUS 0; GRAB 0`). The two self-inflicted modes are now ignored;
   a real focus loss still drops the grab.

### Verified by running it

- The broker builds clean with both backends and no warnings
  (`-Wall -Wextra -Wformat=2 -Wshadow -Wvla`).
- `selftest.sh` — **43 checks, all passing** — against `--backend test`:
  socket mode 0600; the handshake and its order; `SO_PEERCRED` rejection of an
  unlisted uid (run as root with `setpriv`, and it proves the rejected uid
  receives nothing at all); one-client-at-a-time; **the whole ATTACH
  validator** (a buffer smaller than its claimed geometry is rejected and *not*
  clamped, an unadvertised fourcc is rejected, dimensions past 8192 are
  rejected, a pipe in place of a dma-buf is rejected, and a rejected frame does
  not kill the connection); protocol violations (two fds on one command, a
  non-zero reserved field, an fd on a `COMMIT`, a short message, an unknown
  command type) each disconnect; `WINDOW` → `SURFACE`; and the full input
  policy state machine (focus gating, `CTRL+ALT+G` consumed and toggling,
  ABS suppressed under grab, REL only under grab, focus loss releasing held
  keys and dropping the grab).
- QEMU builds clean with `-display nvkvm-broker` present:
  `qemu-system-x86_64 -display help` lists `nvkvm-broker`.
- **The no-OpenGL claim, compiled and demonstrated**: a QEMU configured with
  `--disable-opengl` (`OpenGL support (epoxy): NO`) still builds
  `nvkvm_display_relay.c` and still offers `-display nvkvm-broker`. In that
  build `egl-headless` is gone from `-display help` and `nvkvm-broker` is not.
  That is the "display only" container profile standing up at build time, not
  an argument that it should.
- **The real QEMU talks to the real broker.** Both binaries, end to end,
  against `--backend test`:
  - `-display nvkvm-broker,socket=...` parses; a missing socket fails with the
    intended one-line message naming the bind-mount as the likely cause;
  - the connection is accepted after `SO_PEERCRED`, `HELLO` is exchanged, QEMU
    logs `broker mode active: this QEMU holds no display-server connection and
    imports nothing`, and the capability word arrives intact;
  - input driven into the broker's stdin (focus, keys, buttons, absolute
    motion, wheel, `CTRL+ALT+G`, relative motion under grab, focus loss)
    reaches QEMU's input subsystem, is injected without a crash even with no
    graphic console at all, and QEMU's log mirrors the grab and focus
    transitions the broker made;
  - grab is dropped on focus loss on the broker side and QEMU sees it;
  - `BYE` is delivered once and both sides tear down cleanly.
- **X11 zero-copy without a GL context**: the findings document listed
  `DRI3PixmapFromBuffers` + `PresentPixmap` as *reasoning*, because the
  `xcb-dri3`/`xcb-present` headers were unavailable. They are now compiled —
  the X11 backend links `xcb-dri3` and `xcb-present` and contains no GL call.
  **Compiled is not run**; see below.
- The X11 backend's connect/screen/extension-probe path runs under `Xvfb` and
  fails at the intended gate with the intended message (`Xvfb` has no DRI3).
- The Wayland backend's connect/registry/roundtrip path runs against headless
  `weston` with the pixman renderer and fails at the intended gate with the
  intended message (no GPU ⇒ no `zwp_linux_dmabuf_v1`).

### Verified on hardware (RTX 3090, 580.105.08)

- **A frame on screen, both backends**, with a real block-linear dma-buf — see
  "First light" above. `make check` is still 42/42 on that machine.
- **The NVIDIA DDX accepts NVIDIA block-linear modifiers through
  `DRI3PixmapFromBuffers`**, and `DRI3GetSupportedModifiers` **does** report
  them — twelve of them, `0x03000000006060{10..15}` and
  `0x0300000000e080{10..15}` — despite the extension reporting version 1.0.
  This was the single biggest unknown and the answer is yes.
- **The X11 grab is real**: `XGrabKeyboard`/`XGrabPointer` hold, `CTRL+ALT+G`
  is consumed and toggles, keys reach the client under grab, true relative
  deltas arrive from XI2 raw motion, and **a real focus loss really does drop
  the grab** (the security property, on real hardware, not synthesised).
- **The Wayland grab announces `GRAB IS TOTAL` on sway**, which advertises
  `keyboard-shortcuts-inhibit`, `pointer-constraints` and `relative-pointer`.
- **The containerised VMM** — see §6. QEMU in a podman container with only
  `/run/nvkvm` bind-mounted, no render node, no display socket, and (built
  `--disable-opengl --audio-drv-list=`) **not one X, GL, EGL, GBM, DRM or
  Wayland library loaded** — connects, handshakes, and is driven by the broker.
  A frame reaches the screen from a container process that links libc alone.

### Known-bad on hardware, and what it costs

- **X11: the implicit (`DRM_FORMAT_MOD_INVALID`) path silently corrupts a
  buffer that is not really linear.** The same block-linear bo that renders
  perfectly through `PixmapFromBuffers` is accepted **without any X error** by
  DRI3 1.0's `PixmapFromBuffer` and drawn as shredded scanlines. There is no
  error to catch. The size bound still holds, so this is a correctness trap and
  not a memory-safety one; `x11_attach()` now logs a warning when a client
  takes that path on a server that does advertise explicit modifiers. A guest
  that reports its real modifier is unaffected.
- **X11: NVIDIA does not advertise `DRM_FORMAT_MOD_LINEAR`.** A genuinely
  linear buffer with modifier 0 is therefore *rejected* by the validator on
  this driver, though the same buffer works on Wayland (sway does advertise
  it). Correct behaviour by the rules in §3, but surprising, and worth knowing
  before reading a rejection log.
- **X11: XI2 raw motion is delivered twice under an active grab**, same
  `full_sequence`, same valuators — one copy via the root selection and one
  via the grab. Uncorrected this is exactly **2× mouse sensitivity** in the
  guest. De-duplicated in `nb_session_x11.c`; the clean fix is `XIGrabDevice`
  instead of core `XGrabPointer`, which is left as a deliberate change rather
  than smuggled in.

### On the physical PC: a real guest, a real monitor (2026-08-24)

RTX 4070, driver 595.84, **GNOME Shell 50.1 / Mutter on Wayland**, a real
3840×2160 monitor on DP-1, and a **Linux Mint 22.3 Cinnamon guest** behind the
broker. This is the first time any guest has run behind it — every earlier
frame came from a test tool handing over a dma-buf.

- **A Mint desktop reached the screen and was usable.** Keyboard, mouse, and
  the desktop interactive; the user's words were "screen is smooth. no lag".
  Guest scanout `1600×900 XR24 modifier 0x0300000000606014`, imported and
  presented with no copy in the broker and no GL anywhere in it.
- **QEMU held no display stack.** This build has neither GTK nor SDL
  (`-display help` lists only `none`, `egl-headless`, `dbus`, `nvkvm-broker`),
  and it was launched with `DISPLAY`, `WAYLAND_DISPLAY`, `GDK_BACKEND`,
  `XDG_RUNTIME_DIR` and `XAUTHORITY` all unset — see
  `/srv/launch-mint-broker.sh`. It logged *"broker mode active: this QEMU holds
  no display-server connection and imports nothing"* and put a desktop up.
- **GNOME advertises the whole grab set**: `keyboard=1 abs=1 rel=1 lock=1
  shortcuts-inhibited=1 focus-events=1 fullscreen=1`, and the broker announces
  `GRAB IS TOTAL`. `keyboard-shortcuts-inhibit` was previously only known from
  sway; Mutter has it. Focus-loss auto-ungrab observed working before any grab
  was taken.
- **`XR24` is in the advertised set** with both block-linear modifier families
  and linear, 56 pairs, no truncation. The format-table overflow that bit on
  sway does not happen here.

#### `Present: COPY`, and on this machine it can be nothing else

`wp_presentation` is the Wayland answer to the FLIP/COPY question — its
`presented` event carries `KIND_ZERO_COPY`, set exactly when the compositor
scanned the client's buffer out rather than compositing a copy. The broker now
binds it and logs `Present: FLIP` / `Present: COPY` on every change, the same
line the X11 backend prints.

**Windowed and fullscreen both report `COPY`**, and the host DRM state agrees:
`/sys/kernel/debug/dri/0000:01:00.0/state` shows the primary plane holding
`fb=150 allocated by = gnome-shell, imported=no` — the compositor's own
framebuffer, not the guest's buffer.

**The blocker is not the broker.** Mutter can only promote a surface that
covers the output. The output is 3840×2160; the guest renders 1600×900, and
**the nvkvm guest head's mode list stops at 1600×900**
(`/sys/class/drm/card0-Virtual-1/modes`). The guest buffer can therefore never
cover this CRTC, at any window size.

Two things have to change before FLIP is even reachable, and neither is in this
directory:

1. **The nvkvm virtual connector needs modes up to the host's native
   resolution.** Today it tops out at 1600×900.
2. **The window size has to reach the guest so it re-modes.** nvkvm's
   `QemuConsole` implements no `GraphicHwOps.ui_info`, so QEMU has no channel
   to tell the guest the window changed — which is also why enlarging the
   window only resamples 1600×900 pixels and looks soft. The user reached that
   conclusion from the picture alone: *"it does not become sharper if you
   resize, so it's only sharp at the initial window size or smaller"*. Correct,
   and now explained.

So the honest status is **not** "Wayland direct scanout does not work" — it is
"this guest cannot yet produce a buffer that qualifies". The measurement path
is in place and will answer the moment one can.

#### Grab, and the thing that made it look flaky

Grab engages, `CTRL+ALT+G` toggles it, and focus loss drops it. Two real
defects were found by using it:

- **With only `virtio-tablet` attached there is no relative device**, so the
  broker's REL events under grab land on nothing and the pointer freezes until
  ungrab. This is a guest-configuration gap, not a broker bug:
  `VM_RELATIVE_MOUSE=1` adds `virtio-mouse-pci` and mouse-look works.
  **Note this contradicts the standing expectation** that patch 0007's
  mechanism does not work on hardware — with a relative device present, the
  switch does work here: `grab ON → #5 QEMU Virtio Mouse (relative)`,
  `grab off → #4 QEMU Virtio Tablet (absolute)`.
- **The device selection was not deterministic**, which made the grab work
  roughly every other try. See the relay commit; it now prefers virtio and
  logs its choice.

#### Pacing

Qualitative only, and good: the guest desktop was described as smooth with no
lag, at `refresh 16.667 ms` (60 Hz) reported by `wp_presentation`. No frame
interval histogram was taken. Note for whoever does: the guest's flip log is
`pr_info_ratelimited`, so counting log lines reads ~2 fps on a guest doing 60 —
measure intervals, not counts.

### NOT verified — still needs the physical PC

- **Direct scanout / unredirect: not established, and this box could not.**
  Every `PresentCompleteNotify` reported `COPY`, fullscreen and windowed,
  composited and with KWin compositing suspended — but the head was a *forced
  virtual DFP with no monitor attached*, where a page flip may be impossible
  for reasons that have nothing to do with the broker. Note also that
  `PresentPixmap` targets the **content child window**, and the child is sized
  to the guest buffer, so it can only ever cover the CRTC when the guest
  resolution equals the host's. The X11 backend now logs the Present mode on
  every change (`Present: FLIP` / `Present: COPY`), so on a real monitor this
  is a one-glance answer rather than an investigation. Wayland offers no
  equivalent signal — the compositor still will not tell you.
- Whether a 32-bpp `ARGB8888` guest buffer imported as depth 24 renders
  correctly (it should — a scanout's alpha is not composited) is untested.
- Pacing under load, `EV_FRAME`/`EV_RELEASE` timing, and whether dropping on
  `EAGAIN` produces acceptable smoothness: untested. Every hardware test above
  drove frames from a test client, not from a guest flipping at its own rate.
- `keyboard-shortcuts-inhibit` was *advertised* by sway and announced as total;
  no compositor shortcut was actually fired at it to confirm it is inhibited.
- `relay_set_relative()` — switching the guest to a relative pointing device on
  grab — is **known not to work** in the equivalent GTK path (patch 0007), and
  nothing here changes that: it is still `qemu_mouse_set()` over
  `qmp_query_mice()`, unchanged. **Mouse-look in the guest is expected to still
  be broken, and this hardware run neither fixed nor disproved that** — the
  transport is now known good (GRAB packets reach QEMU from inside a
  container), so what remains is the device switch and the guest side.
- The whole end-to-end path with a real guest, a real isolate and a real
  `nvkvm_req_present` relay. Everything above used a dma-buf allocated by a
  test tool and handed over the same way the isolate hands one over; no guest
  was booted.

### First things to try on the physical PC

1. `cd src/broker && make check` — the shell portion should be 43/43, followed
   by the adopted-socket, clipboard-transaction and persistent-client tests.
   Run it from a path
   `nobody` can traverse (see §8).
2. Start the broker in your session; check the one-line grab announcement is
   the truth for your compositor, and that the advertised pair count is
   **not** followed by `(TRUNCATED …)`.
3. `nvkvm-broker-dmabuf-src --present <sock> --modifier default
   --fill-via-x11` — this is the one that puts a picture up. A red-bordered
   set of colour bars with diagonal stripes means the whole import path works
   with a genuine block-linear buffer. `--modifier linear` and
   `--modifier implicit` are the other two interesting cells.
4. `nvkvm-broker-testclient <sock> --present 640x480`. A memfd will be
   **rejected** on a real backend (`the fd is not a dma-buf`) — that is the
   validator working, not a failure. It still proves the handshake, the window
   and input.
5. Run each `--bad-*` option and confirm the rejection messages.
6. Grab it (`CTRL+ALT+G`), move a **physical** mouse, and check the guest turns
   at 1× — the XI2 double-delivery above was found with synthetic input and its
   de-duplication deserves a real mouse.
7. Fullscreen it (`CTRL+ALT+F`) on a real monitor and read the `Present:` line.
   `FLIP` is the answer nobody has yet seen.
8. Only then start QEMU with `-display nvkvm-broker`. If the window stays
   black, the broker's stderr says which validation rejected the frame — that
   log line is the whole debugging story.

---

## 8. Files

```
nvkvm_broker.h        the two abstractions: nb_session (a backend) and nb_sink
                      (the policy core).  Every security rule is in the sink,
                      once, so it is audited once.
nvkvm_broker.c        socket, SO_PEERCRED, privilege drop, the command
                      validator, hotkeys, grab/focus policy, the coalescing
                      output ring, the main loop.
nb_common.c           backend selection, the fourcc table, and the advertised
                      (fourcc, modifier) set the validator consults.
nb_session_wl.c       Wayland: xdg_toplevel + zwp_linux_dmabuf_v1.  No GL.
nb_session_x11.c      X11: xcb + DRI3 + Present, two windows (see the file
                      header for why).  No GL.
nb_session_test.c     no display at all; input from stdin, memfds accepted.
                      Unreachable from --backend auto.
testclient.c          the pre-QEMU smoke test, and the selftest's attack
                      harness (--bad-*).  Sends a MEMFD, so it proves the
                      validator and can never put a pixel on screen.
dmabuf_source.c       test tooling, NOT part of the broker: allocates a REAL
                      dma-buf through GBM on the render node and either
                      presents it (--present) or serves the fd over a socket
                      (--serve), standing in for the isolate.  The only thing
                      here that links libgbm.  --fill-via-x11 makes the X
                      server draw into a tiled buffer that gbm_bo_map refuses,
                      which is what turns "black window" into an answer.
dmabuf_relay.c        the sandboxed side: receives an fd, relays it to the
                      broker.  Links libc and nothing else — run ldd on it
                      inside the container; that link line IS the §1 claim.
selftest.sh           43 behavioural checks, no GPU required.  Note it needs
                      the tree to be reachable by `nobody` for the
                      SO_PEERCRED check — run it from /opt, not from /root,
                      or that one check fails for a permissions reason that
                      has nothing to do with the broker.
test/*.py             adopted-socket authentication, clipboard framing/cap,
                      and persistent-client generation/key-edge regressions.
```

QEMU side: `src/qemu/nvkvm_display_relay.{c,h}`, the hook in
`src/qemu/nvkvm_isolate_handlers.c` (`nvkvm_req_present`), and patches
`patches/0011` (QAPI `DisplayType`) and `patches/0012` (one meson line).

---

## 9. What it deliberately does not do

- **No DRM lease, no KMS, no mode-setting.** An earlier design leased a
  connector; findings 2 and 4 killed it. A lease hands the VM a *whole monitor*
  because neither X11 nor Wayland can yield a plane for a *window*.
- **No copy fallback.** If a compositor has no `zwp_linux_dmabuf_v1`, or an X
  server has no DRI3, the broker refuses to start and says so. A copy path
  would need a GL context in the privileged process, which is the thing being
  removed.
- **No allocation on behalf of a client**, and no size, count or index ever
  taken from the wire.
- **No cursor plane.** The guest head has none (`drm_simple_display_pipe_init`
  creates only the primary) and the supported guest composites its own cursor
  into the scanout buffer.
- **No reverse channel beyond the three commands.** Every field the privileged
  side reads is listed in §4, and that list is meant to stay short.
