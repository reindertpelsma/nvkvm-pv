# nvkvm display broker — design, threat model and verification log

Internal. This is the reasoning and the evidence behind the broker: why it
exists at all, what it is defending against, what has actually been run on
hardware and what has not. None of it is needed to *use* the broker — for
that see [`src/broker/README.md`](../../src/broker/README.md) — but all of it
is needed to change it safely.

Wire protocol lives in
[`../reference/broker-protocol.md`](../reference/broker-protocol.md).
Field findings from the display bring-up are in
[`display-broker-findings.md`](display-broker-findings.md).

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

Eight rules, all in `nvkvm_broker.c` (the eighth's pure half in `nb_cursor.c`),
all exercised by `selftest.sh` or by `test/test_cursor.{c,py}`:

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
   **And nothing that asks the fd's filesystem runs before the fd's type is
   proved in-kernel** (2026-10-03, review finding). `fstatfs` calls the
   superblock's `->statfs`, which on FUSE is a `FUSE_STATFS` request to the
   daemon behind the mount (`fs/fuse/inode.c` `fuse_statfs()`), waited for
   with this process's signals blocked for `signalfd` -- a stall of the thread
   holding the keyboard grab, chosen by whoever chose the fd. So:
   - a dma-buf is recognised first by the `exp_name:` line of
     `/proc/self/fdinfo/N`, which only `dma_buf_show_fdinfo()` prints
     (`fs/proc/fd.c` `seq_show()` prints the generic lines and the lock list,
     then the file's own `->show_fdinfo`, which a FUSE file does not have; the
     line arrived in Linux 5.3 with the `dmabuf` filesystem `DMA_BUF_MAGIC`
     names). Only then is it `fstatfs`ed. **Not** `readlink("/proc/self/fd/N")`:
     for anything but a pseudo-file that text is a path, and paths are chosen
     by whoever mounts things -- so it is not an identity the sender cannot
     shape. A broker without a readable `/proc/self/fdinfo` refuses every
     dma-buf frame and says why;
   - shmem (the shm tier, the cursor, the test backend's memfd) is recognised
     first by `fcntl(F_GET_SEALS)` succeeding, which `mm/memfd.c` answers from
     the file's mapping (`shmem_aops`) or `f_op` (hugetlbfs) without calling
     into any filesystem; `fstatfs` then rules out hugetlbfs. The one exception
     the kernel source shows is a Coda file mmapped from a container file on
     tmpfs, which shares that file's mapping -- and Coda can only be mounted
     by root, and its upcalls time out;
   - **closing** a client's fd is a filesystem call as well: `close(2)` runs
     `->flush` (`fs/open.c` `filp_flush()`), FUSE's is a `FUSE_FLUSH` sent
     with `force` set, and that makes the wait end in an UNINTERRUPTIBLE
     `wait_event()` -- whatever the caller's credentials, unlike `STATFS`. No
     check run first can avoid it: the fd is in the table from the moment
     `recvmsg` returns, and every way out of the table flushes. So an fd is
     closed on the main thread only when it is proved, as above, to be shmem,
     hugetlbfs or a dma-buf (none of whose `file_operations` has a `->flush`);
     any other -- only ever one the validators refuse -- is handed to a helper
     thread that closes it (`nb_fd_drop()`), started lazily on the first such
     fd, after `--drop-user`, with every signal blocked. A close stuck there
     costs one fd per queued fd, bounded by `RLIMIT_NOFILE`, past which
     `recvmsg` drops what it cannot install (`MSG_CTRUNC`, a violation); never
     the main loop. A broker that never meets such an fd stays single-threaded.
   `test/test_cursor.py` mounts a FUSE filesystem whose daemon answers nobody
   but the test (`test/stall_fuse.py`, no libfuse) and sends a file on it as a
   cursor, an `F_SHM` frame and a dma-buf frame: each must be refused while the
   broker keeps answering, with no request from the main thread reaching the
   daemon and the close arriving as a `FLUSH` from the helper thread.
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

8. **The guest cursor's fd is COPIED, never mapped and never handed on**
   (`CMD_CURSOR`, added 2026-10-03; `nb_cmd_cursor()` / `nb_cursor_read()` in
   `nvkvm_broker.c`, the pure rules in `nb_cursor.c`). It is the second command
   that carries an fd, and it gets the same treatment as `ATTACH` plus one
   stronger rule:
   - **the record is classified before the fd is touched** --
     `nb_cursor_check()`: framing errors (unknown `op`, non-zero
     `flags`/`reserved1`, an fd where none belongs or none where one does,
     image fields on `HIDE`/`SHOW`) disconnect; content errors (fourcc, size
     past 256, hot spot outside the image, stride outside
     `width*4 .. 1024`) refuse the cursor and keep the connection, because the
     image comes from the guest;
   - **the fd must be shmem**: `F_GET_SEALS` succeeding, **then** `fstatfs`
     == `TMPFS_MAGIC` -- in that order, see rule 4: the first version called
     `fstatfs` first, which on a FUSE file is the very stall the rule exists to
     prevent, reached one syscall early. Both halves are needed -- `/dev` is
     devtmpfs and reports `TMPFS_MAGIC`, so `/dev/tty` or a FUSE device node
     would pass the `fstatfs` test alone, and a read of either can block the
     thread that holds the keyboard grab. A device node also has `st_size` 0
     and fails the extent check below, and so does every other non-shmem fd
     the suite used to send -- so deleting the shmem proof left the whole suite
     green (review finding, 2026-10-03). `test/test_cursor.py` now also sends
     an fd to a 4 KiB regular file on a disk filesystem, which passes the
     extent check and reads fine: only the shmem proof refuses it, and the
     suite goes red without it (measured);
   - **the extent is measured, in 64 bits**: `offset + stride*(h-1) + w*4 <=
     st_size` (`fstat`, not `lseek`, so validation does not move the client's
     file offset);
   - **the rows are copied with `pread(2)` into a fixed 256 KiB static buffer
     and the fd is closed.** No mmap, so no seal is demanded: a client that
     truncates the memfd under the read gets a short read and a refused
     cursor, never a `SIGBUS` in the privileged process -- the hazard the shm
     tier needs `F_SEAL_SHRINK` for, removed by construction instead of by a
     precondition. The compositor never receives the client's fd: the backend
     re-uploads the core's copy, scaled, from broker-owned memory (Wayland: a
     2-slot 2 MiB shm pool, allocated once; X11: `RenderCreateCursor` from a
     depth-32 pixmap);
   - **the pixels are normalised on the way in** (every colour channel clamped
     to its alpha), so whatever the client sends, the compositor gets valid
     premultiplied ARGB;
   - **application is paced**, not per command: at most one cursor change per
     8 ms reaches the display server, latest wins, and the latest is always
     applied (`nb_sink_tick()`). The read itself is charged to the per-wakeup
     work budget like any frame copy. So a flood of `SET`s -- or an honest relay
     forwarding a guest that animates its pointer as fast as it can -- costs
     bounded CPU per wakeup and bounded display traffic per second, and never
     a disconnect of an honest VMM. **The core keeps two copies** to make that
     true: a `SET` lands in `nb_sink.cursor`, and backends are only ever handed
     `nb_sink.cursor_pub`, which `nb_sink_tick()` copies over when the interval
     allows. The first version handed backends the live copy, so the pacing
     bounded the core's `->cursor()` calls and nothing else: every backend also
     re-renders on its own schedule (Wayland on every frame commit, configure,
     buffer release and grab change; X11 on every commit), and each re-read the
     newest image straight past the interval (review finding, 2026-10-03). The
     test backend now re-renders on every `COMMIT` the same way, and
     `test/test_cursor.py` interleaves `SET`s with frame commits and requires
     no two images to reach the backend closer than half the interval (the bug
     showed them sub-millisecond apart);
   - **it is connection state**: forgotten on every attach and detach, so a
     `--persist` broker never shows one VM's pointer over the next VM's picture
     or over the placeholder.

   The image is shown only in **hover mode**. Under grab the broker hides it
   whatever the VMM sent: the pointer is locked and a host cursor at the lock
   position would point at the wrong place.

Two more properties that are policy rather than parsing:

- **A rejected frame is not a disconnect.** The descriptor originates in the
  guest, so a guest flipping nonsense must not be able to kill the display of a
  VMM that is behaving correctly. The frame is dropped, the reason is logged,
  the connection lives. Only *protocol* violations disconnect.
- **One client at a time, and the incumbent is never displaced.** A second
  allowed-uid process cannot steal the display out from under a running VM.

And two things the broker now TELLS the VMM, written down so they are
decisions rather than leaks (2026-10-03):

- **`EV_DEVICE` discloses the host compositor's DRM device number**
  (major:minor, resolved to the render node through sysfs where possible). It
  says which of the host's GPUs draws the desktop and nothing about what is on
  it, and it lets a relay pick a same-GPU or a cross-GPU present path up front
  instead of discovering a hybrid laptop by having every `ATTACH` refused. It
  is a hint the VMM may use to choose, never something the broker relies on:
  `ATTACH` is validated as if it had not been sent. It deliberately does NOT
  say which backend produced it, because the VMM must not be able to tell
  Wayland from X11.
- **An unsolicited `EV_FORMAT` x=0 now comes from X11 too** when DRI3 refuses an
  import the server advertised, matching what the Wayland probe already did,
  and on both backends `QUERY_FORMAT` then answers no for that pair. On X11 the
  x=0 is sent for **both alpha twins**: DRI3 imports XR24 and AR24 identically,
  so both are refused -- and the first version remembered both but announced
  only the one the frame used, so a relay holding x=1 for the other kept
  sending it into a refusal nothing on the wire explained (review finding,
  2026-10-03). The X11 memory now lives in the core (`nb_sink.fmt_refused`,
  filled by `nb_sink_format_refused()`, 8 slots -- four refused modifiers --
  oldest forgotten) and is checked by the one resolver `ATTACH` and
  `QUERY_FORMAT` share.
  **Both backends forget refusals when the client detaches.** ~~Recorded rather
  than fixed, because per-connection memory would make the two backends
  disagree~~ -- that reason held only while ONE backend would change; the
  same review asked for a consistent decision, and the decision is
  per-connection on both (`nb_client_state_reset()` clears the core's table;
  `wl_client_detach()` clears the Wayland `proven` table, keeping only the
  in-flight probe flag that attributes its answer). Why per-connection: a
  refusal is triggered by a buffer the GUEST chose, so on a `--persist` broker
  a memory that outlived the client let one VM make a pair read as unusable
  for the next VM, which then took a slower present path -- the same rule that
  makes the cursor connection state. The price is re-learning, bounded and
  cheap: on Wayland one dropped probe frame per refused pair per connection
  (a first buffer always goes through the asynchronous probe, so re-learning
  can never be the fatal `create_immed` error the table was added to avoid);
  on X11 one blocking round trip per refused pair per connection, inside the
  per-wakeup round-trip allowance. `test/test_cursor.py` drives the core's
  half through the test backend, which advertises a modifier for both twins
  and refuses it at import exactly as `x11_attach()` does.
- **A frame dropped at the format gate is never dropped in silence.** The core
  sends `EV_FORMAT` x=0 for the pair the `ATTACH` named, once per pair per
  connection (`nb_sink.fmt_told`). Found while fixing the X11 twins: the same
  hole existed on Wayland, where the probe reports the fourcc it IMPORTED --
  the opaque twin the resolver substituted -- so a relay that sent AR24 and was
  told x=0 for XR24 kept its yes for AR24 and every frame after was refused
  with nothing on the wire. Closing it in the core covers that, the X11 twins,
  and any future backend's version of the same mistake; the Wayland probe's
  verdict now goes through the core too (`nb_sink_format_refused()`, one
  fourcc -- unlike DRI3, a compositor imports per fourcc).

The broker also drops what it does not need: `PR_SET_DUMPABLE 0`,
`PR_SET_NO_NEW_PRIVS`, and `--drop-user` (it retains no capabilities — the
display connection and input are already open fds by then). It is normally not
started as root at all; it is a session program.

---


## Measured cost

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

### The guest cursor and `EV_DEVICE`, verified headlessly (2026-10-03)

No GPU on the machine this was written on, so what is claimed is exactly what
was run:

- **Offline, in CI** (`make check`, and again under `make check-sanitize`):
  `test/test_cursor.c` -- every boundary of every `CMD_CURSOR` rule from both
  sides, arithmetic chosen to wrap 32 bits if anything were computed in 32
  bits, and a 300 000-record deterministic sweep asserting what the backends
  rely on (an accepted record's extent is inside the protocol bounds, a scaled
  cursor fits a 512x512 slot, its hot spot is inside it, every pixel is
  premultiplied). `test/test_cursor.py` -- the same rules over the real socket
  against `--backend test`, with the image hash recomputed from the bytes sent
  (so it proves the RIGHT rows were copied from the RIGHT offset), 14 refusals
  that keep the connection, 9 violations that end it, 60 back-to-back `SET`s
  reaching the display as 2 uploads with the last one on screen, the grab
  hiding and restoring the image, detach forgetting it, and the six-packet
  handshake with `EV_DEVICE` appended after the unchanged five.
- **X11, real server**: `Xvfb` (no DRI3, so `--present-mode=shm`, 1:1). XFixes
  `GetCursorImage` over the content window read back the guest's 32x32 /
  40x30 / 60x40 / 120x80 / 180x120 test cursors with hot spot 1,1 and the
  exact premultiplied pixels sent (border `0xff000000`, centre `0x80808080`);
  `HIDE` and `CTRL+ALT+G` both turned it into the 1x1 blank, ungrab restored
  it, and the X server's default came back after the client detached. No X
  error in the log; ASAN+UBSAN with leak checking clean across three clients.
  The XRender-scaled path needs a DRI3 pixmap and is NOT run here.
- **Wayland, real compositor**: weston 14 (pixman) nested in that `Xvfb`, kiosk
  shell, shm frames 320x240 in a 1024x768 fullscreen window -- a 3.2x scale.
  `WAYLAND_DEBUG` shows the cursor buffer created at **128x96** and
  `set_cursor(..., 3, 3)` for a 40x30 image with hot spot 1,1; a screenshot of
  the X root shows the black border's bounding box at exactly 128x96 with its
  corner 3 px up-left of the pointer, and the 50 % centre composited over the
  guest's green as `0x80ff80`. `CTRL+ALT+G` issued `set_cursor(nil)` with the
  pointer lock, ungrab put the same surface back without a re-render. ASAN +
  UBSAN clean (leak checking off: the Wayland backend's pre-existing shutdown
  path never destroys its registry globals, unrelated to this change).
- **Not run anywhere**: the X11 DRI3-refusal verdict (needs a server with
  DRI3 that refuses an import), and `EV_DEVICE` with a real answer (needs a
  render node: weston's pixman renderer offers no dma-buf feedback and `Xvfb`
  no DRI3, so both reported `unknown`, as designed).

### The review fixes, verified headlessly (2026-10-03, later the same day)

A read-only review of the cursor/`EV_DEVICE` change returned ten findings;
each fix carries a test that was run red against a mutation of the fix (the
mutation, then the checks that failed):

| fix | mutation | went red |
|---|---|---|
| fd identity before any filesystem question | `nb_fd_shmem_seals()` calls `fstatfs` first | the FUSE cases: broker stopped answering; `STATFS` from the main thread |
| same, dma-buf path | `fstatfs` == `DMA_BUF_MAGIC` before the fdinfo proof | the FUSE dma-buf case: stalled, `STATFS` from the main thread |
| close a refused fd off the main thread | `nb_fd_drop()` closes everything itself | the FUSE cases: stalled in `FLUSH` from the main thread |
| a refusal case only the shmem proof catches | the proof deleted from `nb_cursor_read()` | the 4 KiB on-disk file is SHOWN as a cursor |
| backends see only the paced snapshot | `nb_sink_tick()` hands backends the live copy | images 1.17 ms apart across commits; 40 changes in 49 ms |
| x=0 for both alpha twins | the twin's verdict dropped | only AR24 told |
| refusals are connection state | the reset dropped | the next connection still told x=0 |
| a gate-dropped frame is answered x=0 | the core's `nb_format_tell_no()` call dropped | the unadvertised pair's frame got no `EV_FORMAT` |
| ...once per pair per connection | the `fmt_told` dedupe dropped | the already-told twin told again; the second frame answered again |
| hot spot on the guest's hot pixel | `floor` instead of `ceil` | 1.5x lands on 4,4; 65 310 sweep mismatches; 49 188 pixel mismatches |
| device-resolution geometry | output scale ignored | scale 2 gives 16x16 |
| 64-bit `gen` | `uint32_t` | wraps to 0 |
| F_SHM still demands the seal | the seal check dropped | unsealed memfd and `/dev/shm` file accepted |

- **The FUSE stall, for real**: `test/stall_fuse.py` mounts a filesystem (no
  libfuse; `mount(2)` as root, `fusermount3` otherwise) whose daemon answers
  only the test process. A 1 MiB file on it, sent as a cursor, an `F_SHM`
  frame and a dma-buf frame: each refused, the broker answering `QUERY_FORMAT`
  throughout, no request from its main thread, and the close arriving as a
  `FLUSH` from the helper thread -- which stays parked until the test closes
  `/dev/fuse` and the kernel aborts the connection.
- **A real dma-buf still passes** the new fdinfo proof (a udmabuf, under the
  broker's `PR_SET_DUMPABLE 0`) -- run locally; CI runners have no
  `/dev/udmabuf`, so CI runs the refusal side only and prints the skip.
- **HiDPI cursor, real compositor**: sway 1.11 nested in `Xvfb`
  (`WLR_BACKENDS=x11`, pixman, `output * scale 2`), 1024x768 shm frames in a
  512x384-logical window, a 32x32 guest cursor with hot spot 1,1. Before:
  `create_buffer(..., 16, 16, ...)`, `set_cursor(..., 0, 0)`, and the X cursor
  wlroots made from it (read back with XFixes) had lost the guest's right-hand
  border to the downscale. After: `get_viewport` on the cursor surface,
  `create_buffer(..., 32, 32, ...)`, `set_destination(16, 16)`,
  `set_cursor(..., 1, 1)`, and XFixes reads back a 32x32 cursor with the exact
  one-pixel border and hot spot 2,2 in device pixels.
- **Not run anywhere**: the X11 DRI3-refusal path itself (needs a DRI3
  server that refuses an import) -- the twin announcement and the reset are
  the core's (`nb_sink_format_refused()`, `nb_client_state_reset()`), driven
  through the test backend, and `x11_attach()` is one call into them; the
  Wayland stale-cursor fix (needs a compositor that holds both cursor buffers
  at once -- by reading only); and the Wayland `proven` reset on detach (needs
  a compositor that refuses an import it advertised).

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

