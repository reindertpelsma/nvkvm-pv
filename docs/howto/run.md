# Run a guest

Assumes [Build](build.md) is done: `/opt/qemu-nvkvm/bin/qemu-system-x86_64`
exists and `/usr/lib/nvkvm/nvkvm_stub` is installed.

## 1. Prepare the guest image

```bash
sudo bash scripts/setup_guest.sh
```

Runs on the **host** despite the name — it builds the image; the in-guest work
is done by cloud-init on first boot. Idempotent.

It fetches the Ubuntu 24.04 (Noble) cloud image, converts it to qcow2 and grows
it by 20 GB, writes cloud-init `user-data`/`meta-data`, and generates a seed ISO
(`scripts/setup_guest.sh:219-450`). Artefacts land in `/opt/nvkvm-guest/`.

The download is resumable, but a `.part` file is renamed into the cache only
after validation. The default Ubuntu image is checked against the published
`SHA256SUMS`; the verified digest is retained beside it so an already prepared
host does not depend on the rolling `current/` manifest staying unchanged.
For a custom `NVKVM_GUEST_IMAGE_URL`, set `NVKVM_GUEST_IMAGE_SHA256` (preferred)
or `NVKVM_GUEST_IMAGE_SHA256_URL` (a sha256sum-format manifest). Without either,
the URL is operator-trusted and `qemu-img check` still rejects a truncated or
invalid qcow2 cache on every run. `NVKVM_GUEST_DIR` relocates the artefacts from
the default `/opt/nvkvm-guest/`.

The cloud-init `runcmd` performs the first-boot mounts and enables the persistent
guest service (`scripts/setup_guest.sh:349-431`):

```
mkdir -p /mnt/nvkvm
mount -t 9p -o trans=virtio,version=9p2000.L nvkvm_src /mnt/nvkvm
grep -q nvkvm_src /etc/fstab || echo 'nvkvm_src /mnt/nvkvm 9p trans=virtio,version=9p2000.L,nofail 0 0' >> /etc/fstab
systemctl enable --now nvkvm-guest.service
```

The `fstab` line matters: `runcmd` runs **once per instance**, so without it any
later boot of the same image comes up with `/mnt/nvkvm` empty — no module
source, no staging script, no test suite (`scripts/setup_guest.sh:360-377`).

The service copies the module sources to a private temporary directory before
building, then loads the module and stages an available NVIDIA userspace bundle
(`scripts/setup_guest.sh:277-316`). The repository export therefore remains
read-only; `NVKVM_DEV_HARNESS_INSECURE_RW=1` is unnecessary for normal setup.

Packages installed: `build-essential git python3 linux-headers-virtual`.
Login is `ubuntu` / `ubuntu` with passwordless sudo.

Two cloud-init details in that file are there because they silently failed
before: `ssh_pwauth: true` must be **top-level** (nested under the `users:`
entry it is ignored and sshd keeps `PasswordAuthentication no`), and the
password must come from `chpasswd:` rather than a `users:` hash, because
cloud-init will not reset the password of a user that already exists in the
image (`scripts/setup_guest.sh:262-273`).

`setup_guest.sh` does **not** stage the NVIDIA userspace. That is a separate
manual step — see [step 4](#4-stage-the-nvidia-userspace).

## 2. Collect the host NVIDIA userspace bundle

Run on the **host**, before starting the VM:

```bash
bash scripts/make_host_bundle.sh
```

It reads the driver version from `nvidia-smi`, collects the libraries from
`/usr/lib/x86_64-linux-gnu` into `host-libs-<version>/` at the repository root,
and copies the EGL/Vulkan loader JSON configs alongside them
(`scripts/make_host_bundle.sh:20-96`). Because the repo root is the 9p share,
the bundle is visible in the guest at `/mnt/nvkvm/host-libs-<version>/`.

The libraries are not redistributable, which is why they cannot live in the
repository and have to be taken from whatever driver you have installed
(`scripts/make_host_bundle.sh:5-11`). Details, including which ones are
required and which merely degrade a capability, are in
[Stage the guest NVIDIA userspace](stage-guest-libraries.md).

## 3. Launch

```bash
sudo bash scripts/run_test_vm.sh
```

The repo 9p export is **read-only** by default, and a normal first boot does
not need that changed: `nvkvm-guest.service` copies the module sources into a
private temporary directory and builds there (`scripts/setup_guest.sh`), so
nothing writes to the export. This section previously said a first boot
required the flag, which contradicted §1 and the code.

You only need it when you want in-guest edits to land back in the repository —
development, not setup:

```bash
sudo NVKVM_DEV_HARNESS_INSECURE_RW=1 bash scripts/run_test_vm.sh
```

which prints a banner explaining what it re-arms: a writable export is a
guest-root → host-root path (`scripts/run_test_vm.sh`, top-of-file banner, and
[CONTRIBUTING.md](../../CONTRIBUTING.md#the-dev-vm-harness-is-not-a-sandbox)).
Everything else — booting, benchmarking, driver bring-up, demos — works
read-only, so set the flag for the build boot and drop it again afterwards.

Prefers `/opt/qemu-nvkvm/bin/qemu-system-x86_64`, falls back to system QEMU with
a warning, or honours `$QEMU_BIN` (`scripts/run_test_vm.sh:29-40`). Extra
arguments are appended to the QEMU command line.

RAM and vCPU count come from `$VM_MEM` and `$VM_SMP`, defaulting to `16G` and
`4` (`scripts/run_test_vm.sh:24-25`). Larger models need them raised — the LLM
parity runs use `VM_MEM=64G VM_SMP=16`, and a guest whose RAM does not exceed
the model size turns model load disk-bound and fakes a large gap.

The command line (`scripts/run_test_vm.sh:67-90`), with the two variables
expanded to their defaults:

```
qemu-system-x86_64 \
    -enable-kvm \
    -m 16G \           # $VM_MEM
    -smp 4 \            # $VM_SMP
    -cpu host \
    -drive file=/opt/nvkvm-guest/ubuntu-24.04.qcow2,format=qcow2,if=virtio \
    -drive file=/opt/nvkvm-guest/seed.iso,format=raw,if=virtio,readonly=on \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -device virtio-nvgpu-pci-non-transitional \
    -device nvkvm-gpu,addr=7 \
    -virtfs local,path=<repo root>,mount_tag=nvkvm_src,security_model=mapped \
    -serial stdio \
    -display none
```

Notes on the pieces that matter:

**`-device virtio-nvgpu-pci-non-transitional`** is the forwarder. Aliases:
`virtio-nvgpu-pci` (generic) and `virtio-nvgpu-pci-transitional`
(`src/qemu/virtio_nvgpu_pci.c:146-154`). Virtio type 50, PCI id `0x1072`.

**`-device nvkvm-gpu,addr=7`** is the identity-only NVIDIA PCI device. It has no
BARs, no MMIO and no DMA; it exists so the guest's DRM render node has an
NVIDIA-vendor sysfs parent, which the Vulkan ICD requires before it will bind
(`src/qemu/virtio_nvgpu.c:1323-1336`). Its vendor/device/subsystem/revision ids
are read from the host GPU's sysfs at realize (`:1382-1407`).

**`addr=7` is not arbitrary.** It places the device at `0000:00:07.0`, which is
the BDF the guest module hardcodes when synthesising
`/proc/driver/nvidia/gpus/<bdf>/` (`src/guest/nvkvm_hostfile.c:28-31`). Change
one and you must change the other.

**There is no memory-backend object, no hugepage setup, no `-numa`, no
`-machine` flag.** Plain `-m 16G` on the default machine type. `-m 16G` is also
a perf-methodology requirement: the harness notes that 4 GB made model load
disk-bound and produced a fake 17x gap (`tests/perf/README.md:23-25`).

**The mmap window is not a command-line property.** The GPA windows are
compile-time constants in `src/qemu/virtio_nvgpu.h`: shared memory at 1 TB
(`:86`), a 16 GiB legacy mmap window at 1.5 TB (`:87-88`), and the 128 GiB
sparse window at 2 TB (`:102-103`). The sparse window's actual base is whatever
firmware assigns to the reservation BAR; the constant is only a fallback.

**Hard limit:** guest RAM ≥ 1 TB overlaps the shared-memory window. QEMU fails
realize loudly rather than corrupting silently
(`src/qemu/virtio_nvgpu.c:1239-1254`).

**Compute-only:** add `graphics=off` to the device:

```
-device virtio-nvgpu-pci-non-transitional,graphics=off
```

(`src/qemu/virtio_nvgpu.c:1444-1449`, default on.) QEMU then refuses to open the
DRM render node or `/dev/nvidia-modeset` at all, and refuses every DRM and NVKMS
ioctl. Pair it with a guest module built `NVKVM_GRAPHICS=0`.

**Always capture QEMU's stdout.** It is the only place `DENY`/allowlist
diagnostics appear, and the absence of a capture is precisely why the current
NVENC failure has no root cause
([`tests/perf/realapp_matrix.md`](../../tests/perf/realapp_matrix.md), the NVENC
row — and see [Known limitations](../internal/known-limitations.md#nvenc--the-5755103-hang-did-not-reproduce-2026-08-20)
for why that row is now "does not reproduce here" rather than "fixed"):

```bash
sudo bash scripts/run_test_vm.sh > /tmp/qemu.log 2>&1 &
```

Set `NVKVM_DEBUG=1` in QEMU's environment for verbose per-operation tracing
(`src/qemu/virtio_nvgpu.c:1102`). Errors and security `DENY` lines are printed
unconditionally either way.

## UEFI guests: OVMF needs a bigger 64-bit PCI window

`run_test_vm.sh` boots SeaBIOS, where this does not arise. If you boot a guest
under **OVMF/UEFI** — which some distro images require, SteamOS among them — add:

```
-fw_cfg opt/ovmf/X-PciMmio64Mb,string=262144
```

Without it the guest **hangs in firmware before the bootloader, with no error
message on any console**. It is not a crash and not a boot-device problem: OVMF
simply stops.

Why: nvkvm reserves a large 64-bit GPA window for guest-physical forwarding, and
says so at startup —

```
nvkvm: GPA windows: guest RAM top ~0x400000000 (16 GiB), floor 0x4000000000
       -> block base 0xdfdbc0000000 size 145 GiB
```

OVMF's default 64-bit PCI MMIO aperture is far smaller than that window, and it
wedges rather than failing loudly. `X-PciMmio64Mb` is OVMF's own knob for the
aperture size; 262144 (256 GiB) comfortably covers the window above.

Measured on an RTX 3090 host, driver 580.105.08, booting SteamOS
(`steamdeck-oobe-repair-20260707.10`) with OVMF 4M. Guest RIP read from the QEMU
monitor isolates it — firmware-range RIP means wedged, kernel-range means booted:

| devices attached | guest RIP | result |
|---|---|---|
| `nvkvm-gpu` only | `ffffffffa58d7aef` | boots |
| `virtio-nvgpu` only | `000000007f46a561` | **wedged in firmware** |
| neither (control) | `ffffffff928d7aef` | boots |
| both, `+X-PciMmio64Mb=262144` | `ffffffffa92d7aef` | boots |

So it is `virtio-nvgpu`'s window that OVMF cannot accommodate; the identity
`nvkvm-gpu` device is not involved.

### Debugging note: `screendump` may not capture the console you mean

QEMU numbers consoles in **device registration order**, so which one is console
0 depends on your command line, not on which device is "the display". With
`-device VGA` listed before `virtio-nvgpu` — the ordering `run_test_vm.sh` uses —
**the emulated VGA is console 0** and nvkvm's scanout is console 1. Reverse the
device order and the numbering reverses with it.

That matters because a bare `screendump file.ppm` always grabs console 0. Point
it at the wrong one and you get either nvkvm's placeholder —

```
Guest has not initialized the display (yet).
```

— which is what nvkvm shows until the guest compositor renders through it, or a
blank VGA framebuffer. **Either way a guest that is booting perfectly well looks
dead**, and that is a costly wrong turn. `screendump <file> <device-id>` does not
reliably select a specific console either, and `screendump -d <id>` is rejected
outright ("unsupported option -d").

Reliable ways to tell what you are actually looking at, in decreasing order of
certainty:

- **`-vga none`.** Remove the emulated VGA entirely. Then nothing but nvkvm can
  be producing the picture, and `info pci` should list `10de:xxxx` as the only
  VGA-class device. This is how the "SteamOS renders through nvkvm" claim was
  established rather than inferred.
- **Resolution.** The two heads usually differ — a 1920x1080 capture is not the
  1280x800 bochs framebuffer.
- **In the guest**, `/sys/class/drm/card*/device/driver` by **name**, not by
  number. Node numbering is not guaranteed; `card0` is whichever registered
  first.
- **Guest RIP** from the monitor is the reliable *liveness* check when the
  picture is ambiguous: a kernel-space RIP means the guest is running regardless
  of what any console shows.

**Use guest RIP as the liveness check instead:**

```
(qemu) info registers
RIP=ffffffff.......   # kernel space -> the guest is running
RIP=000000007f......  # firmware range -> still in OVMF
```

## 4. Stage the NVIDIA userspace

```bash
ssh ubuntu@localhost -p 2222        # password: ubuntu
sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh
```

**Check the exit status.** The script exits 2 with an `INCOMPLETE — missing from
bundle:` list if anything was absent (`scripts/stage_guest_libs.sh:335-346`).
Treat a nonzero exit as "do not trust a subsequent parity run". Full detail in
[Stage the guest NVIDIA userspace](stage-guest-libraries.md).

It also blacklists `nouveau`, which takes effect only after a guest reboot
(`scripts/stage_guest_libs.sh:328-333`).

## 5. Check it worked

```bash
nvidia-smi
```

should report your host's driver version and the real GPU model. That is the
guest enumerating a card the host has not given up.

Then the ladder, in order of what each step proves:

```bash
gcc -O0 -g -o /tmp/cuinit_test /mnt/nvkvm/tests/integration/cuinit_test.c -ldl
/tmp/cuinit_test                      # cuInit + cuCtxCreate
```

`tests/integration/` also has `vector_add_test.c` (a real kernel launch),
`big_memcpy_test.c` (an 8 MiB round trip verified byte-exact),
`cumemalloc_test.c`, `matmul_test.c` and `arch_ladder_test.c`.

Signs of specific failures:

| symptom | look for |
|---|---|
| `open ctl/gpu FAILED r1=-2 r2=-2 — forwarding OFF` | the stub was not embedded — rebuild QEMU with `-DNVKVM_STUB_EMBEDDED` and a fresh `nvkvm_stub_bin.h`. Use `bash scripts/build_qemu.sh --force`; a plain re-run exits 0 without rebuilding once the binary exists ([Build](build.md)) |
| `cuInit` → `CUDA_ERROR_UNKNOWN` (999) | the `/proc` and `/sys` shims — check `dmesg` for module load errors |
| `cuInit` → 803, or "Driver/library version mismatch" | guest userspace not version-matched to the host driver |
| `cuCtxCreate` → 304 | often a denied control command; grep the QEMU log for `DENY` |
| `cuModuleLoadData` → 221 (`JIT_COMPILER_NOT_FOUND`) | `libnvidia-nvvm` missing from `/usr/local/nvidia-guest/lib` |
| `nvkvm: DENY ctrl cmd 0x...` | a control command outside the allowlist; see [Allowlists](../reference/allowlists.md) |
| `nvkvm: host driver <v> → ABI profile <n>` | informational, printed at realize — confirm the profile matches your driver |

## Isolate sandbox mode

The isolate sandbox has four rungs. The default is `auto`, which probes and
picks the strongest one this host can actually run.

```bash
# default — probe: namespace -> uid(+chroot) -> seccomp.  Never picks `none`.
NVKVM_ISOLATE_MODE=auto  bash scripts/run_test_vm.sh

# pin a rung (fails loudly at startup if it cannot run here)
NVKVM_ISOLATE_MODE=namespace   bash scripts/run_test_vm.sh   # strongest
NVKVM_ISOLATE_MODE=uid+chroot  bash scripts/run_test_vm.sh   # containers
NVKVM_ISOLATE_MODE=uid         bash scripts/run_test_vm.sh
NVKVM_ISOLATE_MODE=seccomp     bash scripts/run_test_vm.sh   # lowest real rung

# one uid window per concurrently-running VM, 4096 apart
NVKVM_ISOLATE_UID_BASE=504096 NVKVM_ISOLATE_MODE=uid bash scripts/run_test_vm.sh
```

**If you are running nvkvm inside a container, read this.** `namespace` mode
cannot run under Docker's default profile — `docker run` with no security flags
blocks `CLONE_NEWUSER` through its default seccomp profile and its
`docker-default` AppArmor policy. The kernel sysctls will *not* tell you this:
on a stock container `kernel.unprivileged_userns_clone` reads `1` and
`user.max_user_namespaces` reads `55416` while `unshare -U` still fails. Use
`auto` (which attempts the clone and finds out) or pin `uid+chroot`, and add
`--cap-add=SETUID --cap-add=SETGID --cap-add=SETPCAP --cap-add=SYS_CHROOT`
if your runtime has dropped them.

**All four, including `SETPCAP`.** This list previously omitted it and did not
match [`docker-compose.yml`](../../docker-compose.yml), which has had all four
all along. `SETPCAP` is what lets the isolate drop its capability **bounding
set** before the uid drop; without it that drop silently does not happen and
you get a weaker boundary than the one documented, with no error to tell you.
[`SECURITY.md`](../../SECURITY.md) lists the same four.

`uid+chroot` is a **materially weaker** boundary than `namespace`. Read
[The isolate model → Isolation modes](../internal/isolate-model.md#isolation-modes)
before relying on it.

### Which mode am I actually on?

Two ways, and `auto` makes sure you do not have to go looking:

1. QEMU logs the resolved mode at startup, at **warning** level whenever it is
   weaker than `namespace`, together with which stronger rungs were attempted
   and why each failed.
2. Query it:

   ```bash
   # add -qmp unix:/tmp/mon.sock,server,nowait to the QEMU command line
   { "execute": "qom-get", "arguments": {
       "path": "/machine/peripheral-anon/device[1]/virtio-backend",
       "property": "isolate-mode-active" } }
   { "return": "isolate sandbox: uid+chroot (uid window 500000..504095, 4096 slots)" }
   ```

   The property is on the virtio **backend**, not the PCI proxy — with an
   explicit `id=` the path is `/machine/peripheral/<id>/virtio-backend`.

`NVKVM_ISOLATE_MODE=none` removes every boundary, including the stub's seccomp
filter, and refuses to start without
`NVKVM_ISOLATE_UNSAFE_ACK=i-understand-this-removes-all-isolation`.

## Compute-only checklist

Both halves, or neither:

- QEMU: `-device virtio-nvgpu-pci-non-transitional,graphics=off`, or a build
  with `-DNVKVM_QEMU_GRAPHICS=0`
- guest module: `make NVKVM_GRAPHICS=0`

With graphics off, `/dev/nvidia-modeset` and `/dev/dri/renderD128` never appear
in the guest, and the corresponding host devices are never opened.

## The guest's own Xorg session (a stock distro desktop)

**Nothing to do — `scripts/stage_guest_libs.sh` installs this.**  Along with the
NVIDIA libraries it writes `/etc/X11/xorg.conf` from
`data/xorg/nvkvm-xorg.conf`, so a guest that has been staged already has it.
The rest of this section is what that file is for, and what to do in the two
cases where you have to think about it: you already have an `xorg.conf`, or you
want X left alone.

**Why the file is part of installation.**  A stock distro left alone picks one
of the two X paths that cannot work on the nvkvm head — NVIDIA's DDX (which
wants the *host's* display engine; see below) or `modesetting` with glamor
(whose scanout-pixmap import NVIDIA's EGL rejects, on bare metal too).  The file
names the third path — `modesetting` with `Option "AccelMethod" "none"` — which
works.  A guest already needs a kernel module built against its own kernel and
an NVIDIA userspace matched to the host driver; this is the third and smallest
item on that list, and the same script installs all of it.

**`BusID` is rewritten for your guest.**  The shipped file says `PCI:0:7:0`
because `run_test_vm.sh` puts the device at `addr=7`; the staging script reads
the real address out of the guest's own PCI tree instead (a BDF is hex and
`BusID` is decimal, so `0000:00:1f.0` is `PCI:0:31:0` — a straight copy would be
wrong).  It prints the value it used.

**It will not overwrite an `xorg.conf` you wrote.**  The script recognises its
own file by the `nvkvm-xorg.conf` marker on line 1.  Anything else is yours: it
leaves the file exactly as it is and prints what to merge and where from.
Overwriting someone's X configuration unasked is not an acceptable thing for a
staging script to do, and a desktop that stops coming up is a much worse outcome
than a manual merge.

**To skip it entirely**, for a guest whose X server is not ours to touch:

```bash
NVKVM_STAGE_XORG=0 sudo -E bash /mnt/nvkvm/scripts/stage_guest_libs.sh
```

and then, if you do want it after all, it is one copy:

```bash
# inside the guest, as root
cp /mnt/nvkvm/data/xorg/nvkvm-xorg.conf /etc/X11/xorg.conf
# check BusID matches: lspci -nn | grep NVIDIA   (slot 7 by default)
```

Start X the distro's own way (display manager, `startx`, whatever it ships).
The X screen composites on the CPU; run GL applications on the GPU the way a
PRIME laptop does:

```bash
__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia glxgears -info
# GL_RENDERER = NVIDIA GeForce RTX 3070/PCIe/SSE2      ~2460 FPS
# (without the two variables: llvmpipe, ~490 FPS — check this, not the
#  renderer string, to be sure you are not on a silent software fallback)
```

Set the two variables in the session's environment (`/etc/environment`, or the
`.desktop` files of the applications you care about) to make it the default.

It has to be `/etc/X11/xorg.conf`, and **not** an `xorg.conf.d` drop-in — this
is the tidy-up that looks obviously right and is not.  With a drop-in the NVIDIA
driver package's `OutputClass` still matches the same device, Xorg tries its
driver **first**, that fails the screen, and the server exits rather than
falling through to the second one.  An explicit `Device` section in
`/etc/X11/xorg.conf` outranks `OutputClass` driver selection, which is also why
this file needs nothing the distro ships removed.

**What is genuinely unavailable is NVIDIA's own X driver, the DDX.**  It reaches
the GPU fine and then asks NVKMS to select a display subsystem — and the NVKMS
behind this device is the *host's*, owning the host's connectors, not nvkvm's
virtual head.  nvkvm denies that command (`NVKMS_IOCTL_DECLARE_EVENT_INTEREST`),
and widening the allowlist only walks the DDX one rung further down a ladder
that ends at the host's physical monitors.  Satisfying it needs a virtual NVKMS
that answers for nvkvm's own head; that is real work, not a missing forward.  So
`nvidia-settings` and anything else that requires that specific driver will not
run in the guest.  Ordinary desktops, GL, Vulkan and CUDA are unaffected.
Background and measurements:
[`docs/internal/mint-guest-desktop.md`](../internal/mint-guest-desktop.md).

## Running the guest desktop in a window

By default the VM is headless (`-display none`, serial on stdio) and you reach
the guest over SSH. On a machine with a physical display you can instead watch
and drive the guest's desktop in a real window.

There are two ways to do that, and they differ in what QEMU ends up holding:

| | window owned by | QEMU links | QEMU needs |
|---|---|---|---|
| **GTK/SDL** (below) | QEMU | GTK or SDL, EGL, GL | your X11 or Wayland socket |
| **display broker** | a separate privileged process | nothing graphical | only a unix socket |

Use the broker if you intend to confine the VMM at all — it is the difference
between handing QEMU your display server and handing it a socket. QEMU relays
the guest's dma-buf over that socket and imports nothing, so a broker-mode build
needs no EGL, no GL, no `libnvidia-eglcore` and no `/dev/dri/renderD*` for
display. Verified running at uid 1000 with an empty capability set. Setup, wire
protocol and threat model: [`src/broker/README.md`](../../src/broker/README.md).

The broker is also where fullscreen gets you **direct scanout** — on
GNOME/Wayland the compositor scans the guest's own buffer out unmodified rather
than compositing a copy of it.

The GTK/SDL backends below are the simpler option when you are not sandboxing
anything. They are not built by default — headless is the normal deployment,
where GTK/SDL would only add build dependencies for a window nobody
can see. Build them in once:

```bash
NVKVM_QEMU_UI=1 scripts/build_qemu.sh --force
```

Then start the VM with a display and a present mode:

```bash
VM_DISPLAY="gtk,gl=on" VM_SERIAL=none NVKVM_PRESENT_MODE=gl \
    scripts/run_test_vm.sh
```

The window **switches to the guest's GPU head by itself** the moment that head
first presents. It opens on QEMU's emulated VGA (console 0) so GRUB and the
early kernel are visible, then moves to nvkvm's console once the guest
compositor comes up, logging one line when it does:

```
nvkvm: guest display is live -- switched window to console page 1
```

It switches exactly once. If you then pick a different tab yourself it will not
drag you back, and with `-vga none` there is only one console so nothing
happens at all.

Add `show-tabs=on` to see the tab bar and switch by hand — worth doing if you
are debugging the head, because without it a window sitting on the boot console
looks exactly like a hung guest:

```bash
VM_DISPLAY="gtk,gl=on,show-tabs=on" ... scripts/run_test_vm.sh
```

(The same toggle lives in the window's **View → Show Tabs** menu.)

`VM_DISPLAY` != `none` also adds `virtio-keyboard-pci` and `virtio-tablet-pci`,
without which the window would show the desktop and swallow every key and
click. The tablet reports absolute coordinates, so the host and guest pointers
track each other with no mouse grab.

Inside the guest, run a compositor on the **DRM backend** so it composites to
the virtual KMS head (the headless backend renders to an offscreen buffer that
never reaches the window). Run it as the unprivileged `ubuntu` user, **not**
as root:

```bash
# as ubuntu, not root -- see below
env XDG_RUNTIME_DIR=/run/user/1000 LIBSEAT_BACKEND=seatd \
    weston --backend=drm --renderer=gl --socket=wayland-0 --idle-time=0 \
            --xwayland
```

Six details here are load-bearing, and each one fails in a way that does not
look like its cause:

- **Pick the DRM node by DRIVER, never by index.** This is the one that fails
  *silently and looks like success*, so take it first. If the VM boots with an
  emulated VGA present -- which it does by default, so that GRUB and the early
  kernel have somewhere to draw -- the guest has **two** DRM devices, and the
  emulated one usually enumerates first:

  ```
  card0 -> bochs-drm      <- NOT the GPU
  card1 -> nvidia         <- nvkvm's head
  ```

  A compositor that takes `card0` (weston's default is the first it finds) will
  come up, composite, animate, and screenshot perfectly -- **on llvmpipe**. The
  only tell is a line most people never read:

  ```
  EGL vendor: Mesa Project
  GL renderer: llvmpipe (LLVM 20.1.2, 256 bits)
  ```

  Resolve it by driver instead, and pass it explicitly:

  ```bash
  for c in /sys/class/drm/card[0-9]*; do
      [ "$(basename "$(readlink -f "$c/device/driver")")" = nvidia ] &&
          NVCARD=$(basename "$c") && break
  done
  weston --backend=drm --drm-device="$NVCARD" ...
  ```

  **Always confirm `GL renderer` says NVIDIA before believing any graphics
  result.** Card indices are not stable across configurations: with no emulated
  VGA the same head is `card0`, so a hardcoded index is right until the day it
  quietly is not.

- **`--socket=wayland-0`, not an arbitrary name.** snapd's AppArmor profile
  permits only `/run/user/[0-9]*/wayland-[0-9]*`. With any other socket name,
  snap applications -- which on Ubuntu includes Firefox and Chromium -- fail
  with a bare `Permission denied`, and the guest log shows
  `apparmor DENIED name="/run/user/1000/wl-f"`. No amount of group or
  permission fixing helps; the name itself is the block.

- **Run as `ubuntu`, not root.** Snaps refuse to run as root, and snapd's
  Wayland proxy dies on `mkdir /run/user/0: Permission denied` long before it
  reaches the GPU. Running the compositor as root therefore breaks exactly the
  applications most worth demoing, and it fails as `we don't have any display`
  rather than as a permissions error. `scripts/setup_guest.sh` provisions the
  seat, the runtime dir and the groups this needs.

- **`LIBSEAT_BACKEND=seatd`.** weston's built-in seatd needs root to open the
  DRM node, so a non-root compositor needs the real daemon. Note the group
  owning `/run/seatd.sock` is **`video`** on Ubuntu, not the `_seatd` or `seat`
  that upstream documentation suggests -- read it off the socket rather than
  assuming.

- **`--idle-time=0`** for anything unattended. weston's default is 300s, after
  which it blanks the output; a benchmark left running longer than that reports
  a frozen frame counter that looks exactly like a pipeline stall.

- **`--xwayland`** for X-only clients, with the `xwayland` package installed.
  Without it they do not fail politely: the Minecraft launcher (GTK) takes a
  `SIGSEGV` inside `libX11` because `XOpenDisplay()` returns NULL and the
  caller dereferences it unchecked, while its stderr prints `OK` and the real
  error goes to `~/.minecraft/bootstrap_log.txt`. Xwayland spawns lazily on the
  first X client, so `pgrep Xwayland` finding nothing beforehand is not a
  failure.

`weston-screenshooter` additionally needs weston started with `--debug`, or it
returns `Output capture error: unauthorized`.

### Present modes

`NVKVM_PRESENT_MODE` picks how the guest's composited frame reaches the window:

| value      | path                                                     |
|------------|----------------------------------------------------------|
| `gl`       | zero-copy: the guest frame's dma-buf goes straight to `dpy_gl_scanout_dmabuf` |
| `readback` | import to a texture on a private EGL context, `glReadPixels` to a CPU surface |
| unset      | auto — currently always `readback`                        |

Auto is deliberately conservative: zero-copy only works when the *window's own*
GL renderer can import an NVIDIA dma-buf, i.e. the host desktop is rendered on
the same NVIDIA GPU, and QEMU's console API does not let us check that. On a
host desktop you know is NVIDIA-rendered, set `gl` — it is both faster and, on
at least one measured box, the only one of the two that works: the readback
import failed there with `glEGLImageTargetTexture2DOES glGetError=0x0502`
(`GL_INVALID_OPERATION`) and the window stayed blank.

Measured on an RTX 4070 (driver 595.84, Ubuntu 26.04 host, GNOME/Wayland),
guest weston on the DRM backend with glmark2 running:

```
nvkvm present: window mode=GL-zerocopy (console_has_gl=1)
```

| stage                        | rate    |
|------------------------------|---------|
| guest KMS flips              | 59.9 Hz |
| host submits                 | 60.0/s  |
| host window swaps            | 60.0/s  |
| dropped frames               | 0       |

Measured with per-frame counters compiled into the present path
(`NVKVM_PRESENT_TIMING=1`), over 60 consecutive one-second samples of which
every single one was exactly 60 frames; it holds at 60.0/s with 8 concurrent
EGL clients, and `glmark2` scores 58.0/s windowed and 60.0/s fullscreen. The
per-present PRIME export costs 0.07 ms, so it is nowhere near being the limit.

**Count frames with a counter, not with log lines.** An earlier figure of
~21/s here was an artifact of counting log messages that only some paths emit;
a "2.5 fps with 4.8-second freezes" reading was likewise the kernel's printk
ratelimiter (291 suppressed callbacks per 5 s window is itself ~60/s). Both
looked like real performance bugs and neither was.

### Known gap: sync-fd passback

`SEMSURF_FENCE_CREATE` returns a sync fd that we cannot yet translate across
the VM boundary, so the guest reports `supports_sync_fd = 0` in `GET_DEV_INFO`
and NVIDIA's userspace takes a path that does not need one. Implementing
passback (a guest `dma_fence` signalled from a host watcher) would let the
sync-fd presentation path work and is the next step for present latency.
