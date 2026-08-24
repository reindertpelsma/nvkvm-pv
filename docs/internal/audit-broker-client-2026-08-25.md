# Audit: the display broker against a hostile VMM

The broker is the privileged half of the display split. It holds the
compositor connection, it can grab the user's keyboard, and it runs as the
desktop user outside whatever sandbox the VMM is in. `src/broker/README.md` §3
asserts it treats every socket client as hostile. **That claim had never been
tested adversarially.** This is that test.

Method: read every client-reachable path, then attack a live broker on a
private socket (`/run/nvkvm/audit.sock`) with a purpose-built malicious client,
on the physical PC (RTX 4070, GNOME 50.1/Mutter Wayland). Where a conclusion
could be measured rather than argued, it was measured — including a
**counterfactual control build** for the one finding that mattered, because
"the fix works" and "the bug was never there" look identical otherwise.

Default-deny was the standard: a path is only clean if something explicitly
rejects the bad case, not if no test happened to reach it.

---

## The question that mattered most: can a client make the broker grab?

**No. There is no client-reachable path that turns a grab on** — but the more
useful answer is the one below it, because a client *could* stop an existing
grab being released, which is just as bad.

The grab is exercised through exactly one variable call, `nb_set_grab()`
(`nvkvm_broker.c:436`). Enumerating every call site of the session op:

| site | argument |
|---|---|
| `nvkvm_broker.c:353` | `false` (literal) |
| `nvkvm_broker.c:411` | `false` (literal) — detach |
| `nvkvm_broker.c:452` | inside `nb_set_grab()` |
| `nvkvm_broker.c:499` | `!s->grabbed` — **the only variable one** |
| `nvkvm_broker.c:583` | `false` (literal) — focus loss |
| `nvkvm_broker.c:638` | `false` (literal) |
| `nb_session_wl.c:2721`, `nb_session_x11.c:824` | `false` (literal) — teardown |

`nvkvm_broker.c:499` sits in `nb_sink_key()`, whose callers are only the three
backends (`nb_session_wl.c:1618`, `nb_session_x11.c:592`,
`nb_session_test.c:83`) — all of which deliver **real input from the display
server**. The client socket reaches `nb_handle_cmd()`, which dispatches only
`ATTACH`, `COMMIT` and `WINDOW`; none of the three touches grab state, and the
`default:` arm is a disconnect (`nvkvm_broker.c:926`).

The indirect routes named in the brief were checked too. `WINDOW` reaches
`ops->resize()` only. Fullscreen is `nvkvm_broker.c:499`'s sibling on the same
hotkey and is equally client-unreachable. Focus handling only ever calls
`set_grab(false)`. The new dialog only calls `nb_sink_force_ungrab()`.

Grab is also correctly dropped on detach (`nvkvm_broker.c:411`) — *"never leave
the host's keyboard grabbed because the VMM died"* — and refused outright on a
session that cannot observe focus loss (`nvkvm_broker.c:446`).

---

## B-1 — A client can starve the event loop, making a grab unreleasable

**Severity: high. Status: fixed and verified.**
`src/broker/nvkvm_broker.c`, `nb_sink_readable()`.

`nb_sink_readable()` looped until the socket drained. Drainage is the
*client's* choice: it can keep data available indefinitely.

**What the attacker gets.** The same `poll()` services the display server and
the client. While the loop never returns, no Wayland event is read, so no key
reaches `nb_sink_key()` — which means **`CTRL+ALT+G` cannot fire, and neither
can the focus-loss auto-ungrab at `nvkvm_broker.c:583`.** A client that begins
flooding while the grab is on leaves the user's keyboard captured by the guest
with no way out. The client never *obtains* a grab; it makes an existing one
permanent, which this project treats as the worst outcome in the design.

**Measured, with a control.** A client sending valid `COMMIT`s (draining the
reply stream, so it is not disconnected for backpressure) reached 1.8 million
commands/second. `strace -c` over ~6s of flood:

```
unbounded (control build)   470,790 recvmsg        0 poll     <- loop never runs
after the fix               620,415 recvmsg    9,694 poll     <- ~1,900 polls/s
```

620,415 / 9,694 ≈ 64, exactly `NB_RX_BUDGET`, so the bound is demonstrably what
returns control. The control build was compiled solely to prove the "before",
because the earlier `/proc/pid/syscall` probe showed `running` in both cases and
could not tell them apart — the first conclusion drawn from it was not safe.

**Fix.** Process at most `NB_RX_BUDGET` (64) commands per call, then return.
Returning with data pending loses nothing: `poll()` reports `POLLIN` again and
the next call resumes. Progress is preserved; monopoly is not.

## B-1b — A flood was free

**Severity: medium. Status: fixed and verified.** Same function.

B-1's fix keeps the loop alive but the broker still burned 100% of a core for
as long as the flood lasted (1,404 CPU ticks over a 15s attack).

**Fix.** `NB_RX_MAX_PER_SEC` = 20,000 sustained commands/second is a
disconnect. A VMM sends an `ATTACH`, a `COMMIT` and occasionally a `WINDOW` per
frame — about 500/s at 144 Hz — so the threshold is 40× above any honest client
and 90× below the observed attack. After the fix the same attack is cut off at
20,000 commands with **0 CPU ticks** consumed, and the live VM is unaffected.

## B-2 — Per-connection state survived the connection

**Severity: medium (functional; no privilege gain). Status: fixed.**
`nvkvm_broker.c` `nb_sink_attach()` / `nb_sink_detach()`, `nb_session_wl.c`.

`nb_sink_detach()` correctly resets the fds, the tx ring, `rxlen`, `rxfd` and
the grab. Three pieces of state were missed:

- **`window_established`** — gates "the guest may size the window once".
  Left set, the *next* VM's `WINDOW` is ignored and it inherits the dead one's
  window size permanently. Introduced by the guest-re-mode change.
- **`close_asked`** (`nb_session_wl.c`) — a fresh client's first close would be
  titled `STILL RUNNING - ASK AGAIN?`, reporting a history belonging to a VM
  that no longer exists.
- **The dialog itself** — could stay mapped, asking what to do about a VM that
  had gone.

This is the class the brief warned about: *a control keyed on the wrong
discriminator looks correct at every call site.* Each of these reads correctly
where it is used; they are wrong only in their lifetime.

**Fix.** `window_established` reset in `nb_sink_attach()`; `nb_sink_detach()`
now calls `ops->dismiss_dialog`, which also clears `close_asked`.

---

## Examined and clean

Each of these was attacked, not merely read. Broker verdicts are from its own
log; "link kept" means the frame was rejected without disconnecting, which is
deliberate — a guest flipping nonsense must not be able to kill the display of
a VMM that is behaving.

**Wire protocol** (`nb_handle_cmd`, `nvkvm_broker.c:824`)

| attack | result |
|---|---|
| unknown command type | violation → disconnect |
| non-zero `reserved0`/`reserved1` | violation → disconnect |
| `ATTACH` with no fd | violation → disconnect |
| `COMMIT` carrying an fd | violation → disconnect |
| `COMMIT` carrying descriptor fields | violation → disconnect |
| `WINDOW` carrying an fd or buffer fields | violation → disconnect |
| `WINDOW` 0×0, 99999², 8193², 0xffffffff² | rejected, link kept |
| 100 × `COMMIT` with nothing attached | ignored (`-ENOENT`), link kept |
| two `ATTACH`es before a `COMMIT` | later supersedes; LRU never evicts `current`/`pending` (`nb_session_wl.c`, `wl_attach`) |
| message split mid-command, then disconnect | clean detach, no crash |

Framing cannot be overrun: the read is `iov_len = CMD_SIZE - rxlen`
(`nvkvm_broker.c`, `nb_sink_readable`), so a 40-byte buffer bounds it by
construction, and there is no length field anywhere to lie about.

**`SCM_RIGHTS`** (`nb_sink_readable`)

Control buffer sized for 4 fds. Every fd seen is captured; all but the first are
closed immediately; >1 total, a second while one is banked from a partial read,
or `MSG_CTRUNC` are all violations. `nb_sink_detach()` closes a banked `rxfd`.
Attacked with 4 fds on one `ATTACH` → *"more than one fd on a single command"*
→ disconnect. **No fd leaks on any rejection path**: 500 connect/disconnect
cycles including violations and half-messages left the broker holding 6 fds.

**Geometry** (`nb_validate_desc`, `nvkvm_broker.c:687`) — still true after
everything that has landed. Order is default-deny: `fstatfs` proves the fd is a
dma-buf *first* (`DMA_BUF_MAGIC`; `/dev/null` → *"the fd is not a dma-buf"*,
link kept), then dimensions against `NVKVM_BROKER_MAX_DIM` (8192), then
fourcc+modifier against **what the compositor advertised** (`ops->format_ok`,
no hardcoded list), then `bpp != 0`, then `stride >= width*bpp`, then
`lseek(SEEK_END)` for the real size, then
`stride*height + offset <= size` computed in `uint64_t` — two `uint32_t`
operands cannot wrap it. Reject, never clamp.

**Resource exhaustion** — buffer cache fixed at 8 slots with LRU that skips
`current` and `pending`; outbound ring fixed at `NB_TXRING` with motion
coalescing and disconnect-on-full (`nb_emit`); one client at a time; fds bounded
as above; command rate bounded by B-1b. A client that connects and says nothing
costs one fd.

**Disconnect / reconnect** — 500 rapid connect/disconnect cycles: no crash, no
leak. Disconnect mid-command: clean. The QEMU-side reconnect added this session
was exercised across 5 broker restarts including `SIGKILL` mid-frame.

---

## Not covered

Stated so the next person knows where the edge is:

- **The X11 backend was not attacked.** It builds and its `WM_DELETE_WINDOW`
  path was added this session, but every measurement here is Wayland.
- **No malicious *real* dma-buf.** Every `ATTACH` test used a non-dma-buf fd or
  a valid descriptor. A genuine dma-buf with a hostile modifier — one the
  compositor advertised but that misbehaves on import — is untested, and it is
  the case where the consumer is the compositor rather than the broker.
- **The compositor as the downstream victim.** A client that attaches a real
  buffer and then commits at the rate limit still drives one
  `wl_surface_commit` per command into Mutter. B-1b bounds it at 20,000/s,
  which is a bound, not a measurement of what Mutter does with it.
- **`--backend test` accepts a memfd** (`accept_memfd`). Unreachable from
  `--backend auto` and it prints a banner, so it is not a hole in a real
  deployment — but it is a real relaxation of the dma-buf check and would be a
  hole if that gating ever regressed.
