# nvkvm display broker — wire protocol

Reference for anyone implementing or auditing either side of the broker
socket.  Both structures are fixed-size and byte-exact; a mismatch is a
protocol violation and ends the connection rather than being negotiated.

For what the broker is and how to run it, see
[`src/broker/README.md`](../../src/broker/README.md).  For why it is shaped
this way, see [`../internal/broker-design.md`](../internal/broker-design.md).

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
- On the QEMU side PRESENT currently runs inline in the BQL-held virtqueue
  callback.  The relay socket is main-loop/BQL-owned and every send uses
  `MSG_DONTWAIT`; if the socket is full the frame is **dropped and counted**.
  The newest dma-buf remains retained for reconnect, while older frames are
  replaced.  A future worker offload must marshal submission back to the main
  loop rather than creating a second socket owner.

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

