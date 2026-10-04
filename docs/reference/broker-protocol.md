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
| 0 | `uint16 type` | `ATTACH` 1, `COMMIT` 2, `WINDOW` 3, `CLIPBOARD` 4, `CAPS` 5, `QUERY_FORMAT` 6, `CURSOR` 7 |
| 2 | `uint16 flags` | was `reserved0`; `ATTACH` may set `F_SHM` (bit 0), every other bit and every other command must leave it 0 |
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
  closes its copy; the buffer stays alive through the `wl_buffer`/pixmap. The
  broker proves the fd is a dma-buf from `/proc/self/fdinfo` (the `exp_name:`
  line only a dma-buf's fdinfo carries) before it asks the fd's filesystem
  anything (`fstatfs` == `DMA_BUF_MAGIC`), so it needs a readable
  `/proc/self/fdinfo` to accept dma-buf frames; without one every dma-buf frame
  is refused and the reason logged. The geometry must fit the buffer's
  measured size as the **modifier** lays it out: `offset + stride*height` for
  `LINEAR` and the implicit layout; for NVIDIA block-linear the height rounded
  up to whole blocks (`GOB rows << h`) and the stride a multiple of 64. A
  modifier the broker cannot decode -- another vendor's, or a reserved NVIDIA
  field -- is never usable, whatever the display advertises
  ([broker-design: the block-linear extent](../internal/broker-design.md#the-block-linear-extent-2026-10-04)).
- **`COMMIT`** presents the most recently attached buffer. No fd, and every
  descriptor field must be zero. Split from `ATTACH` because a compositor
  distinguishes "the content changed" from "the frame is finished", and
  committing a half-drawn buffer is visible.
- **`WINDOW`** asks for a window of `width`×`height` on guest resolution
  change. It is a request: the window manager may ignore it, and the size that
  actually took effect comes back as `EV_SURFACE`.
- **`CLIPBOARD`**, **`CAPS`** and **`QUERY_FORMAT`** are described in full in
  `src/common/nvkvm_broker_proto.h`; `CLIPBOARD` overlays its own layout on the
  40 bytes (`struct nvkvm_broker_clip_cmd`).
- **`CURSOR`** (7) overlays `struct nvkvm_broker_cursor_cmd`; see
  [The guest cursor](#the-guest-cursor-cmd_cursor-7) below. Send it only when
  `HELLO` advertised `CAP_CURSOR`: a broker that did not is entitled to treat
  it as the unknown command it is to an older one, and hangs up.

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
| `CLOSE` 14 | the user closed the display; `x` = 0 powerdown, 1 force. Policy is the VMM's. |
| `CLIPBOARD` 15 | one chunk of host clipboard text (`struct nvkvm_broker_clip_pkt`) |
| `FORMAT` 16 | `x` = 1 displayable / 0 not, `y` = fourcc, `w0`,`w1` = modifier low/high. The answer to `QUERY_FORMAT` -- **and also sent unsolicited with `x` = 0** when the display refuses an import it had advertised, or when an `ATTACH` is dropped for its format; see below. |
| `DEVICE` 17 | `x` = `DEVICE_F_*`, `y` = 0, `w0`:`w1` = the display's DRM device major:minor. See below. |

`flags` mirrors grab and focus state on **every** packet, so the client can
never disagree with the broker about it whatever it did with the `GRAB` event.

Capability bits in `HELLO.w1`: `KEYBOARD` (bit 0), `ABS_POINTER` (1),
`REL_POINTER` (2), `POINTER_LOCK` (3), `TOTAL_GRAB` (4), `FOCUS_EVENTS` (5),
`FULLSCREEN` (6), `DMABUF` (7), `MODIFIERS` (8), `RELEASE` (9), `CURSOR` (10),
`DEVICE` (11).

### The handshake, in order

`HELLO`, `SURFACE`, `FOCUS`, `GRAB`, `FRAME` -- then, from a broker that sets
`CAP_DEVICE`, `DEVICE`. The first five are byte-for-byte what every earlier
version sent, so a client that reads exactly five packets and starts drawing
keeps working; `DEVICE` is appended, not inserted. All six are written in one
burst at attach, so a relay that wants the device before its first frame reads
what is already buffered before acting on the `FRAME`.

### The guest cursor (`CMD_CURSOR`, 7)

Advertised by `CAP_CURSOR`. The guest's pointer image, shown as the **host**
pointer while the pointer hovers over the guest's picture and the input is
**not** grabbed. Under grab the broker hides it whatever the VMM sent -- the
pointer is locked and the guest's pointer moves by relative motion, so the VMM
composes the cursor into the frame (it knows grab state from `EV_GRAB` and from
`F_GRABBED` on every packet). The broker keeps the image and puts it back the
moment the grab ends. An XOR cursor cannot be expressed in ARGB; the VMM
composes that one into the frame and sends `HIDE`.

`struct nvkvm_broker_cursor_cmd`, exactly 40 bytes, little-endian. The buffer
description sits at the same offsets as `ATTACH`'s (asserted in the header):

| offset | field | `SET` | `HIDE` / `SHOW` |
|---|---|---|---|
| 0 | `uint16 type` | 7 | 7 |
| 2 | `uint16 flags` | 0 | 0 |
| 4 | `uint32 width` | 1..256 | 0 |
| 8 | `uint32 height` | 1..256 | 0 |
| 12 | `uint32 stride` | `width*4` .. 1024, bytes | 0 |
| 16 | `uint32 offset` | byte offset of row 0 in the memfd | 0 |
| 20 | `uint32 fourcc` | `DRM_FORMAT_ARGB8888` (`'AR24'`, 0x34325241) only | 0 |
| 24 | `uint32 hot_x` | `< width` | 0 |
| 28 | `uint32 hot_y` | `< height` | 0 |
| 32 | `uint32 op` | `SET` = 1 | `HIDE` = 2, `SHOW` = 3 |
| 36 | `uint32 reserved1` | 0 | 0 |

- **`SET`** carries exactly one fd as `SCM_RIGHTS`: a **memfd** (any shmem
  file; it is proved with `F_GET_SEALS` succeeding and **then** `fstatfs` ==
  `TMPFS_MAGIC` -- in that order, because `F_GET_SEALS` is answered from the
  file's mapping without calling into any filesystem, while `fstatfs` on a FUSE
  file is a request to its daemon; and both, because `/dev` is devtmpfs and
  reports `TMPFS_MAGIC` too). An fd that is not shmem is refused without the
  broker asking its filesystem anything, and closed off the main thread. The
  broker requires `offset + stride*(height-1) + width*4 <= st_size`, computed
  in 64 bits -- the last row needs only its pixels, not a whole stride --
  **copies those rows out with `pread(2)`**, and closes its copy of the fd. It
  never maps the fd, so no seal is demanded: a memfd truncated under the read
  is a short read and a refused cursor, never a `SIGBUS`. The compositor is
  never handed the VMM's fd; it gets the broker's scaled copy. `SET` also
  makes the cursor visible.
- **`HIDE`**: no host pointer over the picture -- exactly the state before any
  `CURSOR` was sent. **`SHOW`**: the last `SET` image again; before any `SET` on
  this connection it changes nothing visible.
- **Pixels**: 32-bit little-endian words, A in bits 31..24, R 23..16, G 15..8,
  B 7..0 (so B,G,R,A in memory), **premultiplied** alpha -- DRM's default
  "Pre-multiplied" blend mode, which is what a KMS cursor plane buffer already
  holds. The broker clamps every colour channel to its alpha while copying, so
  a non-premultiplied image costs wrong colours, never a malformed buffer in
  the compositor.
- **Scaled with the frame**: the image is scaled by the factor the broker
  applies to the guest's frame in the window (size rounded to nearest, at least
  1 pixel, at most 512 per edge with the aspect kept), so the cursor is exactly
  as large relative to the guest's picture as the guest drew it. The **hot
  spot** is the scaled pixel that shows the guest's hot pixel
  (`ceil(hot * scaled / size)`, the first output pixel nearest-neighbour
  sampling maps onto it) -- exact at every scale that does not shrink the
  cursor. On a Wayland output with a fractional or integer scale above 1 the
  image is rendered at device resolution and brought to its logical size with
  a viewport (when the compositor offers `wp_viewporter`); the hot spot itself
  is a logical-pixel integer, which is all `wl_pointer.set_cursor` can carry.
- **Refused, not fatal**: a `SET` whose content is wrong -- fourcc, size, hot
  spot, stride, an extent past the fd's end, an fd that is not shmem -- is
  dropped, counted and logged (rate-limited), the previous cursor stays, and
  the connection lives: the geometry comes from the guest, and a guest flipping
  nonsense must not kill the display of a VMM that is behaving.
- **Violations**: unknown `op`, non-zero `flags` or `reserved1`, a `SET`
  without an fd, an fd on `HIDE`/`SHOW`, any non-zero image field on
  `HIDE`/`SHOW`, more than one fd -- `BYE` reason 2 and the connection is
  closed, as for every other framing error.
- **Paced**: a changed cursor reaches the display server at most once per
  8 ms, latest wins, and the latest is always applied. A burst of `SET`s is
  legal and costs the display a bounded number of uploads. The interval holds
  across everything that makes a backend re-render on its own (a frame
  `COMMIT`, a window resize, the grab ending): backends only ever see the image
  the interval has released.
- **Connection state**: forgotten on detach, so a `--persist` broker never
  shows one VM's pointer over the next VM's picture or over the placeholder.

### `EV_DEVICE` (17)

Advertised by `CAP_DEVICE`, which every backend of a broker that knows the
type sets -- so *"too old to say"* (no bit) and *"could not tell"* (`x` = 0)
are different answers.

| field | meaning |
|---|---|
| `x` | `DEVICE_F_KNOWN` (bit 0): `w0`:`w1` are meaningful. `DEVICE_F_RENDER` (bit 1): they name a **render node** (`renderD*`), as the display reported it or as the broker resolved the reported node through `/sys/dev/char/M:m/device/drm`. `KNOWN` without `RENDER` means it could not be resolved -- compare with care. |
| `y` | 0. Deliberately not "where it came from": that would name the backend, and the VMM must not be able to tell Wayland from X11. |
| `w0`, `w1` | major, minor of the DRM character device |

Where it comes from: Wayland -- `zwp_linux_dmabuf_feedback_v1.main_device` from
a second `zwp_linux_dmabuf_v1` object bound at version 4 (the one the validator
uses stays at 3, because version 4 stops the format/modifier events); X11 --
`fstat` of the fd `DRI3Open` hands back, closed at once. Re-sent, unsolicited,
whenever the display server reports a different device. A hint for choosing a
present path, never a gate: every `ATTACH` is validated as if it had not been
sent. It discloses the host compositor's DRM device number to the VMM -- a
deliberate, small disclosure, recorded in
[`../internal/broker-design.md`](../internal/broker-design.md) §3.

### An unsolicited `EV_FORMAT` `x` = 0 takes a yes back

Both backends send one when the display refuses to import a pair it had
advertised: Wayland when the asynchronous probe answers `failed`, X11 when
`DRI3PixmapFromBuffer(s)` answers with an X error. On X11 it is sent for **both
alpha twins** -- one `EV_FORMAT` for XR24 and one for AR24, same modifier --
since DRI3 imports the two identically and a refusal of one is a refusal of the
other. From then on `QUERY_FORMAT` for the refused pair(s) answers `x` = 0 as
well, and an `ATTACH` in them is rejected without another import attempt. A
relay must accept a **later** `x` = 0 for a pair it holds `x` = 1 for, and must
never treat an unsolicited `x` = 1 as an upgrade.

The broker also sends one **whenever an `ATTACH` is dropped at the format
gate** -- for the (fourcc, modifier) that `ATTACH` named, once per pair per
connection. So a dropped frame is never silent on the wire, whatever made the
pair unusable: never advertised, a layout the broker cannot bound, refused
earlier on this connection, or refused under its opaque twin (a Wayland probe
imports, and so reports, the twin the broker substituted -- XR24 for an AR24
frame -- and a relay that sent AR24 would otherwise have kept its yes). A
relay that never asked about the pair may simply record it.

**Connection state**, on both backends: the refusal is forgotten when the
client detaches, so a new connection is answered from what the display
advertises, and learns of a refusal again the same way (on Wayland at the cost
of one dropped probe frame). A refusal is caused by a buffer the guest chose;
on a `--persist` broker it must not decide the next VM's present path.

What a relay does next is its own choice; the broker offers no fallback of its
own. On X11, shared-memory (`F_SHM`) frames are presented only in the shm tier
(`--present-mode=shm`, or `auto` on a server without DRI3), so a relay whose
only advertised pair was refused there has nothing left to send.

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

