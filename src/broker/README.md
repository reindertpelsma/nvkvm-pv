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
   is never briefly world-writable.

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
By default root and the invoking user may connect.

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
`/dev/dri/renderD*` for the display-only profile. `SO_PEERCRED` reports the
peer's uid **as seen in the broker's user namespace**, so if the container
maps uids (rootless podman with `--userns`), pass the *host-visible* uid to
`--allow-user`, or run the container without a uid map for this socket.

The capture/NVENC profile additionally needs `/dev/dri/renderD*` and the NVIDIA
userspace stack inside the container — but still no display server.

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

This matters more than anything else in this document. The machine this was
written on **has no GPU and no `/dev/dri` at all**, so nothing here has met
real hardware.

### Verified by running it

- The broker builds clean with both backends and no warnings
  (`-Wall -Wextra -Wformat=2 -Wshadow -Wvla`).
- `selftest.sh` — **42 checks, all passing** — against `--backend test`:
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

### NOT verified — needs the physical PC

- **No frame has ever been displayed.** Not one pixel, on either backend.
- The Wayland import path (`zwp_linux_buffer_params_v1` → `create_immed` →
  `wl_surface_attach`) has never met a real compositor with a real dma-buf.
- The X11 import path (`DRI3PixmapFromBuffers` → `PresentPixmap`) has never met
  a real X server with a real dma-buf. In particular: whether the NVIDIA DDX
  accepts NVIDIA's block-linear modifiers through `PixmapFromBuffers`, and
  whether `DRI3GetSupportedModifiers` reports them, is **unknown**.
- Whether a 32-bpp `ARGB8888` guest buffer imported as depth 24 renders
  correctly (it should — a scanout's alpha is not composited) is untested.
- Pacing under load, `EV_FRAME`/`EV_RELEASE` timing, and whether dropping on
  `EAGAIN` produces acceptable smoothness: untested.
- The grab: no real `XGrabKeyboard`, no real `keyboard-shortcuts-inhibit`, no
  real focus loss. The *policy* around them is tested; the *plumbing* is not.
- `relay_set_relative()` — switching the guest to a relative pointing device on
  grab — is **known not to work** in the equivalent GTK path (patch 0007), and
  nothing here changes that. Mouse-look in the guest is expected to still be
  broken, for reasons believed to be guest-side.
- Whether the compositor ever actually promotes the surface to direct scanout.
- The whole end-to-end path with a real guest, a real isolate and a real
  `nvkvm_req_present` relay.

### First things to try on the physical PC

1. `cd src/broker && make check` — should be 42/42.
2. Start the broker in your session; check the one-line grab announcement is
   the truth for your compositor.
3. `nvkvm-broker-testclient <sock> --present 640x480`. A memfd will be
   **rejected** on a real backend (`the fd is not a dma-buf`) — that is the
   validator working, not a failure. It still proves the handshake, the window
   and input.
4. Run each `--bad-*` option and confirm the rejection messages.
5. Only then start QEMU with `-display nvkvm-broker`. If the window stays
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
                      harness (--bad-*).
selftest.sh           42 behavioural checks, no GPU required.
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
