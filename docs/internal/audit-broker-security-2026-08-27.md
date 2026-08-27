# Audit: the broker's new tiers, the audio fifo, and the container boundary

The 2026-08-25 audit (`audit-broker-client-2026-08-25.md`) attacked a broker
that had one way to get a frame on screen: import a dma-buf. Since then the
broker has grown a **shared-memory present tier on both backends**, an **X11
clipboard**, **XRender scaling**, a **close dialog**, **placeholder painting**
and a **pacing watchdog** — and `nvkvm-steamos` has grown an **audio container**
with a fifo written by the untrusted VMM. None of that had been read
adversarially. This is that read.

Method: the intended invariants first (`src/broker/README.md` §3,
`docs/internal/broker-design.md` §3, `docs/reference/broker-protocol.md`), then
every client-reachable path checked **against** them, on the standing rule that
a violated documented invariant is a finding whether or not it is exploitable
today. Scope A is `src/broker/`; scope B and C are `nvkvm-steamos`'s audio path
and container boundary. Where a conclusion could be measured it was measured;
where it could not, it says so and sits under SUSPECTED.

Findings are numbered `S-n` (broker), `A-n` (audio), `C-n` (container). Nine
findings were fixed in this pass; the rest carry a specific fix and no code.

---

## The shape of it

Every serious finding in scope A comes from the same place: **the shm tier
introduced a second kind of fd, and the checks written for the first kind were
reused rather than re-derived.** The dma-buf rules held because a dma-buf is
identified by an unforgeable kernel fact and its extent cannot change. A memfd
is identified by the *same* `fstatfs` call answering about a filesystem the
attacker also has ordinary files on, and its extent is `ftruncate`-able at
will. The three worst findings (S-1, S-2, S-3) are each one instance of that
one substitution.

The second theme is smaller and older: **a control that reads the state it is
about to change.** S-5 is the detach-during-grab ordering; it is the same class
as B-2 in the previous audit and A-1/R-1 before that.

---

## S-1 — The dma-buf proof was globally switched off to make the shm tier work

**Severity: high. Status: FIXED (this branch).**
`src/broker/nvkvm_broker.c:1180` (old), `nb_session_wl.c:4071` (old),
`nvkvm_broker.h:457`.

Design §3 rule 4 is unambiguous: *"The fd is proved to be a dma-buf before
anything imports it: `fstatfs(fd).f_type == DMA_BUF_MAGIC`. A memfd, a pipe, a
socket, a file on your disk — all rejected. (`--backend test` also accepts a
memfd; that is why it is unreachable from `--backend auto`.)"* The previous
audit closed with that relaxation listed under *Not covered*, with the words
**"it would be a hole if that gating ever regressed."**

It regressed. `nb_session_wl.c` set `s->accept_memfd = true` **unconditionally**
— the production Wayland backend, reachable from `--backend auto` on every
desktop — because that is the flag the shared validator consulted and the shm
tier needed a memfd to get through. The gate and the tier were the same switch,
so turning the tier on turned the gate off:

```c
if (!nb_fd_is_dmabuf(fd, ss->accept_memfd)) {      /* true ⇒ TMPFS_MAGIC passes */
    nb_err("ATTACH: the fd is not a dma-buf");
    return -EINVAL;
}
d->is_shm = (c->flags & NVKVM_BROKER_CMD_F_SHM) != 0;
if (d->is_shm) { /* ...must really be a memfd... */ }
```

The `is_shm` branch enforces *declared shm ⇒ really a memfd*. **Nothing enforced
the converse.** A memfd sent **without** `F_SHM` therefore passed validation and
went down the dma-buf import path into
`zwp_linux_buffer_params_v1_add()`/`create_immed()`.

**What the attacker sends.** One `ATTACH` with `flags = 0`, valid geometry, and
`memfd_create()` in place of a dma-buf, naming a `(fourcc, modifier)` pair the
compositor has already proven (one honest frame first, or see S-6 for the
version that needs no honest frame at all).

**Consequence.** A non-dma-buf fd reaches the compositor's dma-buf import. The
import fails as a **Wayland protocol error**, which is fatal to the connection:
`wl_display_dispatch` returns −1, `nvkvm_broker.c:2958` logs *"session dispatch
failed"*, sends `BYE` and **breaks out of the main loop — the broker exits**.
`--persist` is below the break and does not help. The VMM chooses when the
privileged process dies. Note also that `TMPFS_MAGIC` is not "is a memfd": every
file on `/dev/shm` and on a tmpfs `/tmp` answers the same, so the relaxation was
wider than the tier that wanted it.

A second, quieter consequence: because Wayland accepted `F_SHM` unconditionally
and X11 accepted it only under `--present-mode=shm`, **the VMM could tell which
backend it was talking to** — against the protocol document's *"The wire format
is identical on Wayland and X11. The VMM cannot tell which backend is running,
and must not be able to."*

**Fix (applied).** One flag cannot answer two questions, so there are now two.
`accept_memfd` keeps its original meaning — *a memfd may stand in for a dma-buf
on the ordinary path* — and remains test-only. A new `accept_shm` means *this
backend can present an `F_SHM` buffer*. The validator branches on the
declaration and each branch proves its own fd type:

```c
d->is_shm = (c->flags & NVKVM_BROKER_CMD_F_SHM) != 0;
if (d->is_shm) {
    if (!ss->accept_shm) reject;
    if (fstatfs(...) != TMPFS_MAGIC) reject;
    if (!(fcntl(fd, F_GET_SEALS) & F_SEAL_SHRINK)) reject;   /* S-2 */
} else if (!nb_fd_is_dmabuf(fd, ss->accept_memfd)) {
    reject;
}
```

## S-2 — An shm buffer's size is measured once and the client can take it away

**Severity: high. Status: FIXED (this branch).**
`src/broker/nvkvm_broker.c:1195` (the missing check),
`nb_session_x11.c:350` (the `mmap`), `:628` and `:1317` (the reads),
`nb_session_wl.c:678` (the `wl_shm_pool`).

`nb_validate_desc()` measured the fd with `lseek(fd, 0, SEEK_END)` and checked
`offset + stride*height <= size`. Correct, and for a dma-buf permanent: a
dma-buf's extent is fixed at creation. **For a memfd it is a snapshot.** The fd
stays the client's; `ftruncate()` a microsecond later makes the measurement a
lie, and `grep -rn F_SEAL src/broker` returned nothing.

**What the attacker sends.**
1. `memfd_create()` with no seals (or `open("/dev/shm/x")`, also `TMPFS_MAGIC`),
   `ftruncate` to N bytes;
2. `ATTACH` with `F_SHM`, `XR24`, geometry inside N — accepted;
3. `ftruncate(fd, 0)`;
4. `COMMIT`.

**Consequence, and it differs by backend — the X11 one is the serious one.**

- **X11: SIGBUS in the broker.** `x11_attach` maps the fd itself
  (`nb_session_x11.c:350`, `PROT_READ|MAP_SHARED`) and `x11_commit` reads
  through the mapping — the packing `memcpy` at `:628` when
  `stride != width*4`, otherwise `xcb_put_image`'s copy at `:1317`. A
  `MAP_SHARED` tmpfs page past the new EOF raises **SIGBUS**, and the broker
  installs no handler (`nvkvm_broker.c:2755` blocks only INT/TERM/HUP and
  ignores SIGPIPE). Default action: the privileged process dies, at a moment
  the VMM picks, with one command. This is the only place in the broker where
  the broker itself dereferences client-supplied shared memory.
- **Wayland: the same hazard, in the user's compositor.** The memfd goes to
  `wl_shm_create_pool` and the compositor maps it. Mutter and wlroots route shm
  reads through libwayland-server's `wl_shm_buffer_begin_access()`, which
  installs a SIGBUS handler for exactly this, so on those the finding is a
  missing check rather than a proven crash; any compositor that maps the pool
  itself, or hands the mapping to a helper, takes the user's whole session down
  with it. That is *why* `F_SEAL_SHRINK` exists.

**Fix (applied).** Require `F_SEAL_SHRINK` on any fd declared `F_SHM`, in
`nb_validate_desc()` so both backends inherit it. This costs an honest client
nothing: the QEMU side already seals the very same memfd
(`src/qemu/nvkvm_udmabuf.c:83`) because **udmabuf demands it for the identical
reason** — *"it pins the pages, so the backing store must not be able to shrink
underneath the importing device."* `F_GET_SEALS` additionally fails outright on
a plain tmpfs file, which is how the `/dev/shm/...` variant is refused rather
than mapped. `testclient.c` now seals too, modelling a correct VMM rather than
being exempted, and `selftest.sh` gained three checks: an unsealed memfd is
rejected, is not presented, and **does not disconnect the client** — the same
policy every other bad frame gets.

## S-3 — A sparse 4 GiB memfd makes the wl_shm pool size negative

**Severity: high. Status: FIXED (this branch).**
`src/broker/nb_session_wl.c:678`; bound added at `nvkvm_broker.c:1300`.

```c
size_t need = (size_t)d->stride * d->height + d->offset;   /* exact, 64-bit */
pool = wl_shm_create_pool(w->shm, d->fd, (int32_t)need);   /* truncated */
```

The extent arithmetic is genuinely overflow-safe — that part of §3 rule 1 holds
— but nothing bounded `need` **from above**, and `stride` is a full `uint32_t`
bounded only from below by `stride >= width*bpp`.

**What the attacker sends.** `memfd_create` + `ftruncate(fd, 4 GiB)` — sparse,
so it costs no memory, and `lseek(SEEK_END)` dutifully reports 4 GiB — then
`ATTACH` with `F_SHM`, `width=8192`, `height=8192`, `offset=0`,
`stride=0x40000`. `need == 2^31`, and `(int32_t)need == INT32_MIN`.

**Consequence.** libwayland-server refuses a non-positive pool size with
`wl_resource_post_error()` — a protocol error, i.e. S-1's exit path: **the
broker dies.** The `need = 2^32 + N` variant instead presents a *small positive*
pool alongside large buffer geometry; a correct compositor catches that in
`shm_pool_create_buffer` (same fatal outcome), but "reject, never clamp" was
already broken by the cast, and a weaker shm implementation reads out of bounds.

**Fix (applied).** `need > INT32_MAX` is a rejection in `nb_validate_desc()`,
next to the other extent rules where both backends inherit it. It bounds
`stride` and `offset` with it: each is `<= need` whenever `height >= 1`.

## S-4 — The rejection log is unbounded, and every line is a blocking write

**Severity: medium. Status: FIXED (this branch).**
`src/broker/nvkvm_broker.c:87` (`nb_vlog`), rejection sites throughout
`nb_validate_desc()`, `nb_handle_cmd()`'s `QUERY_FORMAT` arm, plus
`nb_session_x11.c:465` and `:1204`.

Two design rules meet badly here. *"A rejected frame is not a disconnect"* —
correct, and load-bearing. And `nb_vlog` is `vfprintf` followed by **`fflush`
per line**. So the rejection path is the one thing a hostile client can drive
forever, and each turn of it is a synchronous `write(2)` on the thread that also
services the display server and the keyboard.

**What the attacker sends.** `ATTACH` with an unadvertised `(fourcc, modifier)`
at the B-1b ceiling — 20,000/s, using the same fd every time. The rejection
message for that case is ~400 bytes.

**Consequence.** Order **8 MB/s, sustained, indefinitely** into the session's
journal or the container's log driver. Two effects: disk/journal exhaustion —
a privileged-side resource the VMM should not be able to spend — and, on a log
pipe whose reader stops draining, `fflush` **blocks the main loop**. That is
B-1's consequence (no key dispatched, so `CTRL+ALT+G` and the focus-loss
auto-ungrab cannot fire, so an existing grab cannot be released) reached
through a door B-1's fix does not cover: B-1b bounds *commands*, not the log
amplification each rejected command causes. `CMD_QUERY_FORMAT` is the same
shape — one unconditional `nb_log` per command, for a message the protocol
header says is *"asked once per mode change, never per frame."*

**Fix (applied).** A reject-log budget of 10 lines/second per connection, reset
with the rest of the per-connection state, covering every per-frame rejection
in `nvkvm_broker.c` including `QUERY_FORMAT` and the `WINDOW` out-of-range arm.
When the window closes having swallowed some, it says how many: a silent
throttle would hide the flood it exists to bound. **Not fixed here:** the two
backend-side per-frame log sites (`nb_session_x11.c:465`, `:1204`) have no
`struct nb_sink *` in scope; they should take the same budget through the
session's `sink` pointer.

## S-5 — A detach inside the grab transition leaves the keyboard grabbed

**Severity: medium. Status: FIXED (this branch).**
`src/broker/nvkvm_broker.c`, `nb_set_grab()`.

`nb_sink_detach()` carries the rule *"never leave the host's keyboard grabbed
because the VMM died."* It reads `s->grabbed` to decide. `nb_set_grab()` took
the display-side grab, then called `nb_release_all()` — which **emits** — and
only then set `s->grabbed = true`:

```c
if (s->sess->ops->set_grab(s->sess, on) != 0) { ... }   /* grab is now HELD */
nb_release_all(s);                                       /* emits; may detach */
s->grabbed = on;                                         /* too late */
```

`nb_emit()` into a ring the client has stopped draining calls `nb_sink_detach()`
from underneath. That detach saw `s->grabbed == false` — the value from *before*
the transition — and so did not drop the grab. Of all the transitions the rule
protects, the one it missed was the grab being **taken**.

**What the attacker does.** Fill the outbound ring and stop reading. This needs
no user typing: `CMD_QUERY_FORMAT` answers are **not coalesced** by
`nb_tx_coalesce` (unlike motion and `FRAME`), so ~8,500 queries with the socket
unread — well under a second at the permitted rate — puts the ring at the brink
and holds it there. The user then presses `CTRL+ALT+G`.

**Consequence.** The broker comes out of `nb_set_grab()` holding the user's
keyboard with no client to send it to. Under `--persist` the window stays and
every keystroke is captured and discarded until the user presses `CTRL+ALT+G`
again. Recoverable in one chord, and no client receives the keys — which is why
this is medium and not high — but it is a documented invariant reachable at a
moment the attacker chooses.

**Fix (applied).** Re-check after the emits rather than reordering the
assignment (so the key-ups still go out under the flags they were generated
with): if the grab was being taken and the client is gone, drop it and say so.

## S-6 — Five format pairs and the broker exits; four and it goes blind

**Severity: high. Status: FIXED (this branch).**
`src/broker/nb_session_wl.c:534` (`wl_pair_slot`), `:721` (`wl_attach`),
`:566`/`:584` (the probe callbacks).

The `proven[]` table exists because *"a compositor can advertise a
(fourcc, modifier) its own driver then refuses to bind… which is a WAYLAND
PROTOCOL ERROR, so the connection dies and the broker exits. Under a restart
policy that is a spin: OBSERVED 15 restarts in a few minutes."* First contact
with a pair therefore goes through the **asynchronous** `create`, whose refusal
arrives as an event. Once proven, later frames use `create_immed`.

Two client-reachable defects put `create_immed` back on the unproven path.

**(a) Four slots, and running out was a fall-through.** `NB_PROVEN_SLOTS` is 4.
With all four occupied, `wl_pair_slot(…, true)` returns −1, and `wl_attach`'s
three guards were each written `if (ps >= 0 && …)` — so `ps == -1` fell past all
three, straight into `create_immed`. The client picks which pairs to spend the
table on, and the physical box advertises 56.

**(b) The probe's answer is filed against a single global pair.**
`w->probe_fourcc`/`w->probe_mod` are one slot — the comment says *"the pair the
outstanding probe is about"*, singular — and `probe_created`/`probe_failed`
re-derive the slot from them, because the listener's user data is `w`, not the
pair. But `nb_sink_readable` processes up to `NB_RX_BUDGET` = **64** commands
before any Wayland event is dispatched, so four `ATTACH`es naming four different
pairs start four probes with nothing in between. All four answers are attributed
to pair #4. Pairs #1–#3 stay `probing == true` **forever**: `wl_attach` returns
0 for them (frame silently dropped, black window, no log), and their slots are
never released — which is precisely how (a)'s precondition is reached.

**What the attacker sends.** Four `ATTACH`es naming four advertised pairs, then
a fifth naming a different one. Five commands. The fds need not be dma-bufs at
all — see S-1.

**Consequence.** (b) alone: the guest's format is permanently undisplayable and
nothing says why. (b) then (a): `create_immed` on a pair nothing vouched for,
refusal, protocol error, **broker exit** — and under the compose file's restart
policy, the documented restart spin.

**Fix (applied).** `ps < 0` is a frame rejection, which is the designed answer
for a pair the broker cannot vouch for. And one probe at a time: a
`probe_inflight` flag, cleared in both callbacks, so an answer can only ever be
filed against the pair it belongs to. A second concurrent pair is dropped for
one frame instead of being lost forever.

## S-7 — The XRender scaling path never paces the client

**Severity: medium (functional). Status: reported, not fixed.**
`src/broker/nb_session_x11.c:676-687`; default `scale_mode` is
`NB_SCALE_ASPECT` (`nvkvm_broker.c:2571`).

When the window is not exactly the guest's size — the common case, since scaling
is on by default — `x11_render_scaled()` succeeds and the function returns at
`:686` having called `nb_sink_release()` but **never `nb_sink_frame()`**. No
`PresentPixmap` was issued, so no `PresentCompleteNotify` is coming either. The
composite path has no pacing source at all.

**Consequence.** The only surviving `FRAME` token is the 100 ms watchdog in
`x11_tick`, so the guest is paced at **10 fps whenever scaling is active**. The
watchdog added in `7563189` is doing exactly its job and is thereby *hiding* a
missing token rather than exposing it — which is the thing a watchdog must not
be allowed to do quietly.

**Fix.** Call `nb_sink_frame(sink)` and set `x->last_frame_ms = x11_now_ms()`
in the XRender branch immediately after `nb_sink_release()`, for the same reason
the shm tier does at `:658`: the composite has copied the pixels and there is no
flip to wait for.

## S-8 — Neither commit path is paced, and on the shm tier each commit is a copy

**Severity: medium. Status: reported, not fixed.**
`nb_session_wl.c:913` (`wl_commit`), `:1024` (full-surface damage), `:1119`
(per-commit flush); `nb_session_x11.c:635` (`xcb_flush`).

`wl_commit` consults `frame_inflight` only to decide whether to re-arm the
callback, never to skip or coalesce. The sole bound is `NB_RX_MAX_PER_SEC`, i.e.
~10,000 `ATTACH`+`COMMIT` pairs per second (re-attaching a cached slot is a hash
hit and costs the client nothing). The previous audit listed this under *Not
covered*, correctly, as *"a bound, not a measurement of what Mutter does with
it."*

**The shm tier changes what one unit of that bound costs.** On the dma-buf tiers
the compositor imports; on shm it **copies `width*height*4`** — up to 256 MB at
the 8192² dimension bound — and the pages can be sparse, so the attacker
allocates nothing. Full-surface damage is declared every time, so it cannot be
optimised away. On X11 the same frames go through `x11_blit`'s `PutImage` bands
followed by `xcb_flush`, and **`xcb_flush` is not non-blocking** — it loops in
`_xcb_conn_wait` until the whole output queue is written. The protocol
document's backpressure section claims the opposite in as many words: *"writes
to the display server are… non-blocking; a compositor socket that will not take
a write gets `POLLOUT` on the next `poll()` rather than a blocking flush."* An X
server that cannot drain as fast as the client fills therefore stalls the main
loop inside one command, which `NB_RX_BUDGET` cannot bound.

**Fix.** Pace the present path on the frame callback: if a frame is already in
flight, replace the staged buffer and return without committing — latest-wins,
exactly the rule `nb_emit` already applies to motion. Independently, bound the
shm tier's accepted dimensions against what the window can actually show; 8192²
is a dma-buf-era number from when nobody had to copy it. And correct the X11
comment at `nb_session_x11.c:748`, which currently tells the next reader the
opposite of what libxcb does.

## S-9 — The X11 clipboard forwards whatever type the selection owner sends

**Severity: low. Status: reported, not fixed.** `nb_session_x11.c:2133-2156`.

`xcb_get_property` is issued with `XCB_GET_PROPERTY_TYPE_ANY`, and the reply's
`type` is tested only against `a_incr`. Anything else — `STRING` (Latin-1 by
definition), `image/png`, arbitrary binary from a hostile selection owner — goes
straight to `nb_sink_send_clipboard`. `pr->format` (8/16/32) is ignored, though
`xcb_get_property_value_length` is in bytes so no over-read follows.

`README.md` says *"Text is UTF-8 and capped at 7 KiB (7168 bytes) in either
direction."* The cap holds; the encoding does not, in this direction on this
backend. Note the asymmetry: guest→host **is** UTF-8-validated and separately
unit-tested. Direction is host→guest on an explicit user paste, so this is a
contract violation rather than a breach — but the other X clients on the user's
display are not a trusted input either.

**Fix.** In `x11_clip_receive`, reject a reply whose type is neither `a_utf8`
nor `XCB_ATOM_STRING` or whose `format != 8`, and run the same `nb_utf8_ok()`
the guest→host path uses before sending.

## S-10 — A clipboard fetch nobody answers kills paste for the rest of the connection

**Severity: low. Status: reported, not fixed.**
`nb_session_x11.c:2089-2112`, cleared only at `:2122`.

`x11_fetch_clipboard` sets `fetch_active` and waits for a `SelectionNotify` that
**any** X client owning `CLIPBOARD` can simply never send. There is no deadline
anywhere. Every later paste then returns `-EBUSY`; worse, the first paste
already latched `s->clip_held_key` and `nb_sink_clip_finish()` is never called,
so the chord was consumed and is never replayed. Paste is dead until the client
disconnects. ICCCM explicitly requires a requestor to time out.

**Fix.** Record `fetch_started_ms`, and in `x11_tick` — which already owns the
timer plumbing and returns the poll timeout — abandon a fetch older than ~2 s:
clear `fetch_active`, call `nb_sink_clip_finish(sink, generation, false)` (which
releases the held chord and logs the cancellation), and return the deadline as
the timeout while one is in flight.

## S-11 — Per-connection state that outlives the connection, again

**Severity: low (functional). Status: reported, not fixed.**
`nb_session_wl.c:2325` sets `client_attached`; nothing ever clears it.

After the first client ever connects, the idle placeholder reads
`VM ATTACHED - NO PICTURE YET` forever, including with no client and an idle
socket. This is B-2's class exactly — correct at its call site, wrong in its
lifetime — and it defeats the distinction the placeholder text was written to
draw. **Fix:** `w->client_attached = false;` in `wl_client_detach()`, beside the
existing `clip_notice_until` reset.

---

## A-1 — The audio wedge is closed for one case and open for every other

**Severity: high. Status: reported, not fixed.**
`nvkvm-steamos/docker/audio-entrypoint.sh:55-69`, `:76`, `:83-98`, `:119`.

`918b1e8` ("stop a host with no audio server from freezing the guest") fixes
exactly one case: **no audio socket exists at container start.** The player is
chosen **once, before the loop** (`:55-69`) and never re-probed, and the
unconditional drain lives **only** in the `none` branch (`:116`).

**What re-opens it — and it needs no attacker.** The host's PipeWire goes away
while its socket file remains: log out and back in, `systemctl --user restart
pipewire`, a GDM→Xorg switch. This is the same *"a socket outlives its server"*
trap the sibling entrypoint documents at `broker-entrypoint.sh:60-65`. `PLAYER`
is still `pw`, so each iteration is `pw-cat` failing instantly → `|| true` →
`sleep 1` **with nobody reading the fifo** — while `exec 3<>` (`:76`) holds it
open so the writer cannot even get `EPIPE`.

**Consequence, traced through the QEMU source.** `audio/wavaudio.c:119`
`fopen(wav_path, "wb")` and `:48` `fwrite` are both **blocking**, and
`wav_write_out` runs on QEMU's main loop; `:45` `audio_rate_get_bytes` pins
output to real time, so 48 kHz × 2ch × s16 = 192 kB/s fills the 64 KiB pipe
buffer in **~0.34 s**. QEMU's main loop then runs a few milliseconds out of
every second, permanently: timers, the display path, QMP and the broker link all
stall. That is the freeze the commit says it closed, reached by a different
route. `exec 3<>` is what makes it possible at all — it converts a safe `EPIPE`
(audio breaks, guest lives) into a blocking write (guest dies). Note the audio
container being *down* is safe; it being *up and not reading* is the wedge.

**Fix, in order of completeness.**
1. **Complete, VMM side:** open the fifo `O_WRONLY|O_NONBLOCK` and keep the fd
   non-blocking, so a full pipe drops samples instead of blocking the main loop.
   `wav` cannot do this today; it needs a small QEMU patch. Everything below
   only narrows the race.
2. Re-probe the sockets **inside** the loop, so a vanished audio server falls
   through to the drain branch.
3. Never leave the fifo unread: drop the bare `sleep 1`, and run
   `cat <&3 >/dev/null &` for the whole gap between player exit and restart.

## A-2 — The shared tmpfs bounds nothing the VMM cares about

**Severity: medium. Status: reported, not fixed.**
`docker-compose.yml:219-225` (`mode=1777`, `size=1m`), `:198`
(`broker-socket:/run/nvkvm` **rw** in vmm), `Dockerfile:47-67` (no `USER`, so
vmm is **uid 0**).

Two claims, both measured rather than argued:

- **The sticky bit protects nothing here.** `check_sticky()` permits unlink if
  you own the file **or the directory**; Docker creates the volume root
  `root:root` and the vmm container is uid 0, so it owns the directory.
  Measured: root with `CAP_FOWNER`, `CAP_DAC_OVERRIDE` and `CAP_DAC_READ_SEARCH`
  all dropped **successfully unlinked another uid's file in a 1777 directory it
  owned.** The VMM can delete or replace the broker's `steamos.sock` and the
  audio fifo at will. Impact is bounded — the broker holds its listener by fd
  and the VMM is its only client — but `SO_PEERCRED` is the only thing actually
  separating the services on that volume.
- **`size=1m` bounds data, not memory.** Empty files cost inodes and dentries,
  not blocks. Measured: **200,000 empty files on a `size=1m` tmpfs cost ~161 MB
  of kernel slab while `df` still reported 0% used**; the mount offered
  2,045,845 inodes, i.e. ~1.6 GB reachable. No service has `mem_limit`,
  `pids_limit`, `cpus` or `ulimits` (confirmed absent).

One obvious related theory was **disproved**: exhausting the tmpfs does not deny
the broker its socket — `bind()` and `mkfifo` both succeeded on a 100 %-full
tmpfs.

**Fix, one line.** Mount the shared volume read-only in the vmm:
`- broker-socket:/run/nvkvm:ro`. The vmm never writes there (only
`require_mount`, the socket path and the fifo path appear in
`steamos-container-entrypoint.sh`), and the semantics were verified: through a
read-only bind mount `connect()` to a unix socket **succeeded**,
`open(fifo, O_WRONLY)` + `write()` **succeeded**, and file creation was
**refused with `EROFS`** — sockets, fifos and device nodes are exempt from the
ro check in `sb_permission()`. That closes A-2, A-3 and the socket-replacement
path at zero functional cost. Add `nr_inodes=64` to `driver_opts.o`, and
`mem_limit`/`pids_limit` to the vmm service.

## A-3 — The VMM can pre-create the audio directory and crash-loop the player

**Severity: medium. Status: reported, not fixed.**
`docker/audio-entrypoint.sh:26`, `:36-42`.

The volume root is 1777, so the VMM can create `/run/nvkvm/audio` first.
`mkdir -p "$DIR" && chmod 0755 "$DIR"` then runs as uid 65534 against a
root-owned directory. Measured with the exact precondition: pre-created **0755**
survives (GNU `chmod` elides the syscall when the mode already matches);
pre-created **0700** gives `chmod: Operation not permitted`, `set -e` fires,
**exit 1**, and with `restart: unless-stopped` the audio container crash-loops
forever. Separately, `[ -p "$FIFO" ]` at `:36` **follows symlinks**, so the
"reuse the existing fifo" path can be aimed at an inode of the VMM's choosing.

**Fix.** The `:ro` mount above removes the write access entirely. Independently,
refuse to adopt a directory or fifo not owned by the current uid
(`stat -c %u`) rather than taking whatever is there.

---

## C-1 — 9p `passthrough` plus uid 0 plus a host bind is a setuid path to the host

**Severity: medium. Status: reported, not fixed.**
`scripts/steamos-container-entrypoint.sh:435`, `:168`; `docker-compose.yml:197`.

```
-virtfs "local,path=$DATA_DIR,mount_tag=data,security_model=passthrough"
```

`passthrough` writes the guest's uid/gid/mode straight through, QEMU runs as
uid 0, and `:168` does `chmod 0777 "$DATA_DIR"`. With the default named volume
this lands under `/var/lib/docker/volumes` (root-only, unlikely to be executed).
With the **documented** `NVKVM_STEAMOS_DATA=/absolute/host/path`
(`docker-compose.yml:195-197`) it is a user directory, and a guest that writes a
`-rwsr-xr-x root` binary there has a host escalation waiting for whoever runs
it. `CAP_MKNOD` is dropped so device nodes are out of reach; setuid bits are
not, since setting one on a file you own needs no capability.

**Fix.** `security_model=mapped-xattr` (guest ownership and mode live in xattrs;
nothing host-visible is really setuid). If passthrough must stay, document that
`NVKVM_STEAMOS_DATA` has to point at a `nosuid` mount.

## C-2 — The broker container gets the whole desktop session runtime directory

**Severity: medium. Status: reported, not fixed.**
`docker-compose.yml:79` (`${NVKVM_DESKTOP_RUNTIME_DIR:-/run/user/1000}:/run/host-runtime:rw`),
`:80` (`/tmp/.X11-unix:rw`).

`/run/user/1000` is not just `wayland-0`. It carries the session **D-Bus
socket**, `gnupg/S.gpg-agent`, `keyring/*`, `systemd/private` and the flatpak
documents portal. Anything reaching code execution in the broker before its
`--drop-user` transition owns the desktop session outright: arbitrary command
execution through `systemd --user StartTransientUnit` or the portals, plus the
user's gpg and ssh agents.

The compose file states the correct rule two stanzas below, for the audio
container (`:92-95`: *"Mounting it would have carried the Wayland socket in with
it"*) — and then does not apply it to the broker. The broker dropping to an
unowned uid once the display connection is open is a real mitigation, but it
covers only the post-drop window.

**Fix.** Bind the single socket, as the audio container already does:
`${NVKVM_DESKTOP_RUNTIME_DIR:-/run/user/1000}/${WAYLAND_DISPLAY:-wayland-0}:/run/host-runtime/${WAYLAND_DISPLAY:-wayland-0}`.
`/tmp/.X11-unix` can be `:ro` unconditionally; see SUSPECTED before making `:79`
read-only.

## C-3 — The policy test pins the audio container exactly and the untrusted one loosely

**Severity: medium. Status: reported, not fixed.**
`tests/compose_policy_test.sh:81-86`, and what is absent.

The audio service gets `set(am) == {...}` — exact (`:95`). The **vmm**, the one
service assumed hostile, gets presence/absence spot-checks only, so **adding
`/var/run/docker.sock` or `/:/host` to the vmm passes this test.** The test also
never asserts, for any service: `network_mode` on vmm (broker and audio are
checked at `:44`/`:71`) — `network_mode: host` would pass, and it hands the vmm
the host's **abstract-namespace X11 socket** `@/tmp/.X11-unix/X0`, which no
bind-mount policy can block; `privileged`; `pid:`/`ipc:`/`userns_mode` —
`pid: host` plus uid 0 is ptrace of host root processes; `user`; tmpfs options;
`nosuid`/`nodev` on the socket volume (only `noexec` is checked, `:102`).

A test that pins the trusted service and waves the untrusted one through has the
strictness exactly backwards.

**Fix.**
```python
assert set(vm) == {"/run/nvkvm", "/var/lib/nvkvm-steamos", "/data"}, vm
assert vmm.get("network_mode") in (None, "bridge")
for s in (broker, vmm, audio):
    assert not s.get("privileged")
    assert not s.get("pid") and not s.get("ipc") and not s.get("userns_mode")
assert vm["/run/nvkvm"][2:] == ("ro",)   # once A-2's fix lands
```

---

## SUSPECTED — could not be proved here

- **S-12: no scaler on X11 means a size-mismatched Present on every frame.**
  `nb_session_x11.c:666-696`. With `NB_HAVE_XCB_RENDER` undefined or
  `pict_fmt == 0`, the default `NB_SCALE_ASPECT` still resizes the content
  window to `dw,dh` and then presents a `sl->w × sl->h` pixmap. The file's own
  header (`:17-22`) states the invariant: *"PresentPixmap requires the pixmap
  and the target window to have the SAME dimensions."* If that holds, every
  frame is an async X error (one throttled log line each, S-4) and nothing is
  drawn. No X server was available to confirm what a given xserver does with a
  mismatch — but by the file's own rule it is wrong either way. **Fix:** when no
  scaler is available, force `dw = sl->w; dh = sl->h;` before
  `x11_size_content`, which is what the log line at `:1816` already promises.
- **S-13: per-commit `wl_display_flush` against a compositor that stops
  draining.** `nb_session_wl.c:1119`, at up to ~10,000 commits/s. libwayland's
  outbound buffer either errors (→ S-1's exit path) or grows under client
  control, depending on version; which applies to the libwayland linked here was
  not determined. S-8's pacing removes the pressure either way.
- **S-14: `wl_pollfds`/`wl_dispatch_session` disagree about `pfd[1]`.** `:3527`
  writes the second entry only when `max >= 2`; `:3555` reads
  `w->pfd[1].revents` guarded only on `fetch_fd >= 0`. The main loop always
  passes `max = 8`, so it cannot fire today; it is a latent uninitialised read.
- **C-4: `:ro` on the broker's runtime dir may break the shm tier.** libwayland
  uses `memfd_create` on modern systems but historically fell back to `mkstemp`
  in `$XDG_RUNTIME_DIR`. Test `:ro` against `--present-mode shm` before adopting
  it; the single-socket mount in C-2 is the safe primary fix regardless.
- **C-5: what the vmm reaches on the network.** No `network_mode`, so it sits on
  the default bridge with outbound internet (needed once, at
  `steamos-container-entrypoint.sh:134`) and a route to the host gateway. The
  guest already has LAN access through slirp by design, so the marginal exposure
  is host-gateway-bound services specifically. Not quantified.
- **A byte-dribbling client partly evades the command rate limit.**
  `nb_sink_readable` counts *completed* commands, but a client sending one byte
  at a time causes one `recvmsg` per byte — 40× the syscalls per counted
  command. The ceiling on commands still holds; the CPU cost per command does
  not. Not measured, and bounded enough that it is listed rather than fixed.

## Informational

- `nb_handle_cmd`'s generic flags check accepts `F_SHM` on **every** command
  type, including `CLIPBOARD`, whose overlay says *"flags: must be 0 on this
  layout."* Not exploitable; the bit is read only by `ATTACH`. It should be
  rejected in the arms that have no use for it, for the same reason `reserved1`
  moved into its arms after B-3.
- `wl_tick`/`x11_tick` guard on `sink != NULL`, which is assigned at first
  attach and never cleared, so after any client has connected the main loop
  wakes at 10 Hz forever with nothing to pace. `nb_sink_has_client()` is the
  right predicate. A wakeup, not a spin.
- `nb_placeholder.c`'s `put()` bounds `x < w` and `y < h` but indexes
  `y*stride_px + x`; correctness depends on `stride_px >= w`, which every caller
  satisfies and none states. No wire value reaches it.
- `NVKVM_AUDIO_RATE`/`_CHANNELS`/`_FORMAT` are dead knobs — compose passes them
  to neither service, so both sides use their own hardcoded defaults. They agree
  today by coincidence; an operator who sets one gets a pitch shift or silence.
- `docker/broker-entrypoint.sh:171` uses `--socket-mode 0666`, against the design
  doc's "0600 under a umask". It is documented and reasoned in
  `docs/container-compose.md:138-140`, but it does mean `SO_PEERCRED` plus
  `--allow-user root` is the only barrier and the file mode contributes nothing.
- `steamos-container-entrypoint.sh:101-116`: the recovery image's checksum is
  **trust-on-first-use** — it records the hash of whatever it first downloaded.
  HTTPS to Valve is the only integrity control on first install.
- `docker-compose.yml:159` gives a uid-0 container `/dev/udmabuf`. The rationale
  (it already holds `/dev/kvm`, same `root:kvm 0660`) is documented and
  reasonable; it is an accepted risk, not a neutral one.

---

## Examined and CLEAN

Listed so the edge is visible. "Clean" here means something explicitly rejects
the bad case, not that no test reached it.

### The wire protocol and the sink (`nvkvm_broker.c`)

- **Framing cannot be overrun.** `iov_len = CMD_SIZE - rxlen` into a buffer of
  exactly `CMD_SIZE`; no length field exists to lie about. A partial read banks
  its fd and resumes.
- **Fd intake.** Control buffer sized for 4 fds; every fd seen is captured, all
  but the first closed immediately, and `>1`, a second while one is banked, or
  `MSG_CTRUNC` are each violations. `nb_sink_detach` closes a banked `rxfd`.
  Every arm of `nb_handle_cmd` closes an unexpected fd **before** disconnecting.
- **Extent arithmetic.** `(uint64_t)stride * height + offset` from two
  `uint32_t`s cannot wrap; `lseek(SEEK_END)` measures rather than believing;
  `stride >= width*bpp`; `bpp == 0` rejected. Reject, never clamp. O_PATH fds die
  at the `lseek` (`EBADF`), pipes and sockets at `fstatfs`. Unchanged and still
  true — S-2 and S-3 are about what happens *after* the measurement and *above*
  the bound, not about this arithmetic.
- **Clipboard reassembly.** `n` is bounds-checked against the compile-time chunk
  size before anything else, structure before policy; the total is a chunk count
  the receiver keeps; `clip_in_len <= 7168` with a `+1` slack so the NUL always
  lands in range; `nb_utf8_ok` rejects overlong forms, surrogates, >U+10FFFF,
  truncation and embedded NULs (`i + need >= len` is the correct truncation
  test) and is separately unit-tested; chunk 0 resets the transaction and a
  non-monotonic chunk is a violation; policy rejections still advance framing so
  a refused transfer cannot poison the next.
- **`nb_sink_send_clipboard`** reserves 6 replay slots plus the chunk count
  before writing anything, so ring pressure can neither split the synthesised
  chord nor disconnect mid-paste; 478 + 6 < 512.
- **Outbound ring.** `nb_tx_coalesce` writes at most `used <= NB_TXRING-1`
  entries into its `NB_TXRING` scratch; a partially written tail packet is
  preserved; relative motion accumulates in `int64_t` and saturates rather than
  wrapping; motion runs are broken across `GRAB`/`FOCUS` so nothing merges
  across a state change; keys and buttons are never dropped.
- **Grab reachability.** Re-enumerated after this session's additions: still no
  client-reachable path turns a grab **on**. `ATTACH`/`COMMIT`/`WINDOW`/
  `CLIPBOARD`/`CAPS`/`QUERY_FORMAT` touch no grab state; the default arm
  disconnects. S-5 is about a grab being left on, not obtained.
- **`WINDOW` is honoured exactly once** and only within `1..8192`; later requests
  log and rescale. `nb_client_state_reset` clears `window_established`, so B-2's
  regression stays fixed.
- **`SO_PEERCRED`** is consulted before a byte goes out; `--no-peercred` is
  refused for adopted listeners and refused outright when combined with a
  world-accessible mode; `nb_adopt_fd` proves `S_ISSOCK`, `SOCK_STREAM`,
  `AF_UNIX` and `SO_ACCEPTCONN`; `nb_listen` refuses to unlink a non-socket and
  binds under a umask rather than chmod-ing afterwards.
- **One client at a time**, incumbent never displaced; a new client never
  inherits a grab; `nb_sink_attach`'s failure path leaves `client_fd = -1` so the
  caller's `close()` is not a double close.
- **`pfd[3 + NB_MAX_SESSION_FDS]`** matches the maximum the loop can fill;
  `pfd[2]` is the client slot only when a client existed at build time, and
  nothing attaches between build and use.
- **Both `tick()`s return a bounded 100 ms**, so the watchdog cannot spin the
  main loop.
- **Command rate limiting (B-1b) and the 64-command budget (B-1)** are intact
  and unchanged.

### Wayland backend

- Buffer LRU skips both `current` and `pending`, so the compositor is never left
  reading a destroyed buffer; the cache key is the broker-derived inode plus the
  full geometry. Fixed 8 slots; no wire value sizes anything.
- `wl_buffer`, `zwp_linux_buffer_params_v1`, `wl_region` and
  `wp_presentation_feedback` lifetimes traced: every creation site either stores
  into a just-destroyed slot or destroys immediately, and both terminal
  presentation events destroy the feedback object. No leak on any branch.
- Guest→host clipboard copies into a `len + 1` heap buffer with `len` already
  capped by the sink; the previous source and text are released before
  replacement; the unfocused case holds exactly one pending copy.
- `dsrc_send` forces `O_NONBLOCK` on the compositor-supplied fd and breaks on
  `EAGAIN`, so a host client that opens the selection and refuses to read cannot
  stall the loop; `close(fd)` on every exit including the wrong-mime branch.
- Host→guest fetch is bounded by `sizeof(fetch_buf)` in the `read()` length,
  one transfer at a time, polled rather than read inline, `fetch_fd` closed on
  every terminal path and on client detach, generation-gated against stale
  completion. The VMM cannot initiate a fetch — only a real host keystroke can.
- Every broker-owned shm allocation (title bar, dialog, idle, cursor,
  background, borders) derives its size from compositor-configured geometry or
  compile-time constants, never from the wire, and unwinds `memfd`/`mmap`/pool on
  every failure branch.
- Dialog is modal in the key handler, drops the grab before mapping, and
  `dismiss_dialog` is wired to `nb_sink_detach`.
- `wl_format_ok` answers only from the compositor-enumerated table and refuses
  any explicit modifier when the compositor never sent `modifier` events.

### X11 backend

- `x11_attach` hands xcb a `F_DUPFD_CLOEXEC` duplicate (xcb owns and closes what
  it is given) and the early-return happens before any dup exists: no fd leak on
  any path. A refused import has no pixmap to free.
- LRU skips `current`/`pending`; `victim < 0` returns `-ENOSPC` rather than
  indexing −1; `x11_buf_free` frees pixmap and mapping together then zeroes the
  slot, so repeated eviction cannot double free. Live shm mappings are bounded at
  three regardless of how many `ATTACH`es arrive without a `COMMIT`.
- The shm packing loop reads `width*4` from each row at `y*stride`, and
  `stride >= width*bpp` was enforced upstream, so it stays inside the measured
  extent (given S-2's seal).
- `x11_blit`'s band arithmetic: `xcb_get_maximum_request_length` is in 4-byte
  units and a row of `w` 32-bpp pixels is exactly `w` units, `rows` is clamped to
  ≥ 1, and the largest band is ~16.7 MB — inside `int`.
- `x11_dest_rect` computes in `long` with non-zero source dimensions and clamps
  both outputs to ≥ 1, so `x11_render_scaled` cannot divide by zero. Its `src`
  Picture is created and freed per call; `dst_pic` is created once.
- Guest→host clipboard is capped by the sink before `ChangeProperty`, so no INCR
  is needed outbound; inbound, `long_length` is derived from the 7 KiB cap in
  units, `delete=1` prevents accumulation, and INCR is **refused rather than
  half-implemented** — the right call at this cap.
- `x11_clip_serve` refuses with `property = XCB_ATOM_NONE` per ICCCM when there
  is nothing to serve, and `SelectionClear` drops the guest's text as soon as
  another client takes the selection.
- The dialog window is created once and thereafter only mapped/unmapped, so
  repeated `WM_DELETE_WINDOW` cannot leak windows; hit-testing bounds its index;
  the dialog is modal in the event loop.
- The grab's self-inflicted `NotifyGrab`/`NotifyUngrab` focus modes are ignored
  while a genuine focus loss still drops it; `x11_close` ungrabs.
- XI2 raw motion reads `axisvalues_raw` only after checking the length; the
  double-delivery de-duplication is intact.
- Async X errors are logged and survived rather than fatal — correct policy;
  only their volume was a problem (S-4).
- Session teardown frees both clipboard buffers, all eight slots and every
  server-side resource before `xcb_disconnect`.

### Audio and containers

- **The guest cannot outrun the fifo**: `wavaudio.c:45` rate-limits to real
  time, so no write pattern floods the pipe or the tmpfs faster than 192 kB/s.
- **The guest cannot change the wire format**: `-audiodev wav,out.*` pins it in
  QEMU's mixer, so the player's `--rate/--channels/--format` cannot be
  desynchronised from what is being written.
- **`--raw` is on both player branches**, so libsndfile never parses guest
  bytes — the stated property holds.
- **No shell injection in either entrypoint**: every expansion double-quoted, no
  `eval`, no `$()` over attacker-influenced data, and the audio container's
  environment comes from compose, never from the VMM.
- **The fifo's direction is enforced by the filesystem**, not by policy:
  `mkfifo -m 0622` plus `CAP_DAC_OVERRIDE` dropped means uid 0 in the vmm
  genuinely cannot read it back.
- **`fs.protected_fifos` reasoning is correct** — it applies only to fifos in
  world-writable *sticky* directories, and `$DIR` is 0755 non-sticky.
- **The drain `cat` never returns**, because `exec 3<>` keeps a writer open, so
  it is a permanent drain and not a one-second poll.
- **The VMM-side guard fails safe**: with no fifo present the guest is started
  with no audio device at all rather than blocking on `fopen`.
- **Audio container confinement**: `user: 65534:65534`, `cap_drop: ALL` with no
  `cap_add`, `read_only`, `network_mode: none`, and exactly three mounts with the
  two audio sockets bound individually rather than via `$XDG_RUNTIME_DIR` —
  asserted exactly by the policy test.
- **The vmm has no display path**: no `/tmp/.X11-unix`, no runtime dir, no
  `DISPLAY`/`WAYLAND_DISPLAY`/`XAUTHORITY`, no `/dev/dri`, `/dev/input`, dbus,
  `docker.sock`, host `/proc`, `/sys` or home directory anywhere in its set.
  Devices are pinned to exactly two by the test.
- `no-new-privileges: true` on all three services, default seccomp active,
  private pid and ipc namespaces, no `privileged` anywhere, port publishing
  loopback-bound.
- The broker starts as root only to `setpriv` to the desktop uid and keeps
  `CAP_SETUID/SETGID` inheritable solely for its own second drop; the test pins
  both the ambient-caps flag and `--drop-user`. `XAUTHORITY` is mounted `:ro`
  and unset when empty.
- Broker options are validated as explicit enums, never passed through as argv —
  correct for a process that owns a window and input focus.
- The 9p source share is `readonly=on`; only the public half of the SSH key
  crosses to `/data`; shutdown ordering is sound; the `.installing` promotion
  contract genuinely prevents a half-installed qcow2 being read as complete.

---

## What changed on this branch

`broker-audit-2026-08-27`. `src/broker/selftest.sh` **79/79** (76 before, plus
three for S-2) and `tests/unit/run_tests.sh` all 16 suites, both green; clean
build with `-Wall -Wextra -Wformat=2 -Wshadow -Wvla`.

| finding | change |
|---|---|
| S-1 | `accept_memfd` split into `accept_memfd` (test-only) and `accept_shm`; the validator branches on the `F_SHM` declaration and each branch proves its own fd type |
| S-2 | `F_SEAL_SHRINK` required on any `F_SHM` fd; `testclient.c` seals, as the QEMU sender already does; three selftest checks |
| S-3 | `need > INT32_MAX` rejected in `nb_validate_desc` |
| S-4 | 10 lines/second reject-log budget per connection, with a count of what it swallowed |
| S-5 | `nb_set_grab` drops the grab it just took if the client vanished during the release burst |
| S-6 | `ps < 0` is a rejection, not a fall-through to `create_immed`; one probe in flight at a time |

Nothing in scopes B and C was changed: every fix there is a compose or
entrypoint edit that wants to be verified against a running deployment, which
this environment does not have.

## Not covered

- **No live broker was attacked.** The 2026-08-25 audit ran a hostile client
  against a real broker on the physical PC; this one is source-level plus the
  selftest harness. S-2's SIGBUS, S-3's negative pool and S-6's five-`ATTACH`
  kill are traced through the code and the libraries' documented behaviour, not
  observed. They should each be reproduced against a live broker before the
  fixes are believed to be the reason they stop.
- **The compose findings were reasoned and partly measured in isolation**
  (sticky-bit unlink, tmpfs inode cost, `:ro` socket/fifo semantics) but not
  against the real three-container deployment.
- **No malicious *real* dma-buf**, unchanged from last time: every hostile
  `ATTACH` here used a memfd or a bad descriptor.
- **The X11 backend still has not been attacked live** — S-2's SIGBUS is on that
  backend, and it is the one path where the *broker* dereferences client memory.
