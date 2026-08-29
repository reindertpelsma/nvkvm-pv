# nvkvm display broker

A small privileged-side program that owns the window, the input and the
clipboard, so the VMM does not have to. QEMU hands it finished frames over a
unix socket and holds no connection to your display server at all.

## Do you need this?

**Use the broker when the VMM is sandboxed** — in a container, `systemd-nspawn`
or `bubblewrap` — and must not be able to reach your display server. That is
the only thing it buys you: the sandbox stays sealed, and the display
connection lives outside it. This is how `nvkvm-steamos` and the compose
deployments run.

**Do not use it for a plain QEMU on your own desktop.** If the VMM is not
sandboxed, render directly:

```bash
qemu-system-x86_64 ... -display sdl        # or -display gtk
```

The broker's security property is *"the VMM never touches the display
server"*. If the VMM is already running unconfined as your user, that property
is vacuous — it can reach your display server, your clipboard and your input
devices regardless of how frames get to the screen. All the broker adds in
that case is a socket hop and one more process to run. SDL is simpler, faster
to set up, and no less safe.

| your setup | use |
|---|---|
| QEMU in a container / nspawn / bwrap | the broker |
| QEMU as your normal user, unconfined | `-display sdl` |
| headless, no display at all | neither |

Design rationale, threat model and the hardware verification log:
[`docs/internal/broker-design.md`](../../docs/internal/broker-design.md).
Wire protocol: [`docs/reference/broker-protocol.md`](../../docs/reference/broker-protocol.md).

## Grab, focus and hotkeys

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


## Running it

### Build

```bash
cd src/broker && make          # prints which backends it kept
make check                     # build + selftest.sh
bash headless_present_test.sh  # real import + present, headless, no monitor
```

`selftest.sh` covers everything except the one thing that matters most — the
actual dma-buf import and present — because that needs a real compositor.
`headless_present_test.sh` covers it without one attached: weston on its
headless backend, and a bounded frame stream whose **last** frame must be
presented rather than left unflushed.

It also reaches a tier that is otherwise untestable on NVIDIA. Mutter
advertises XR24 + modifier `0x0` (LINEAR) and **NVIDIA then refuses to import
it**, so on an NVIDIA-only host every frame falls back to shm or to the
block-linear native tier and the linear path never runs. llvmpipe and AMD both
import it happily:

```bash
bash headless_present_test.sh                    # llvmpipe — needs no GPU, runs in CI
NVKVM_TEST_SOFTWARE=0 bash headless_present_test.sh   # real driver, still headless
NVKVM_TEST_NODE=/dev/dri/renderD129 ...          # pick the allocator explicitly
```

The allocator node defaults to a **non-NVIDIA** one when the box has several, since a
linear buffer allocated on NVIDIA has no consumer on an NVIDIA-only host. Skips
with status 77 when weston or a render node is missing.

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
Both display backends implement clipboard transfer in both directions: Wayland
through `wl_data_device`, X11 through `CLIPBOARD` selection ownership and a
single-property transfer (an `INCR` stream is refused rather than truncated,
since it only appears well above the 7 KiB cap), and `x11_open()` advertises
`NB_SESSION_CLIP_G2H | NB_SESSION_CLIP_H2G` for the same reason Wayland does —
the boundary the broker draws must not depend on which display server the host
happens to run. This paragraph used to say X11 rejected every non-off mode; it
had not been true since the X11 clipboard landed, and a doc that under-states
what a security boundary permits is worse than one that says nothing. The
explicit test backend has a simulated clipboard for regression tests.

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


## What it deliberately does not do

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
