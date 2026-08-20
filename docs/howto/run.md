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
(`scripts/setup_guest.sh:36-129`). Artefacts land in `/opt/nvkvm-guest/`.

The cloud-init `runcmd` is the entire first-boot bring-up
(`scripts/setup_guest.sh:104-110`):

```
mkdir -p /mnt/nvkvm
mount -t 9p -o trans=virtio,version=9p2000.L nvkvm_src /mnt/nvkvm
grep -q nvkvm_src /etc/fstab || echo 'nvkvm_src /mnt/nvkvm 9p trans=virtio,version=9p2000.L,nofail 0 0' >> /etc/fstab
cd /mnt/nvkvm/src/guest && make KDIR=/lib/modules/$(uname -r)/build
insmod /mnt/nvkvm/src/guest/nvkvm-guest.ko
```

The `fstab` line matters: `runcmd` runs **once per instance**, so without it any
later boot of the same image comes up with `/mnt/nvkvm` empty — no module
source, no staging script, no test suite (`scripts/setup_guest.sh:96-103`).

Packages installed: `build-essential git python3 linux-headers-virtual`.
Login is `ubuntu` / `ubuntu` with passwordless sudo.

Two cloud-init details in that file are there because they silently failed
before: `ssh_pwauth: true` must be **top-level** (nested under the `users:`
entry it is ignored and sshd keeps `PasswordAuthentication no`), and the
password must come from `chpasswd:` rather than a `users:` hash, because
cloud-init will not reset the password of a user that already exists in the
image (`scripts/setup_guest.sh:67-78`).

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
(`tests/perf/realapp_matrix.md`, "NVENC now hangs on the guest"):

```bash
sudo bash scripts/run_test_vm.sh > /tmp/qemu.log 2>&1 &
```

Set `NVKVM_DEBUG=1` in QEMU's environment for verbose per-operation tracing
(`src/qemu/virtio_nvgpu.c:1102`). Errors and security `DENY` lines are printed
unconditionally either way.

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
`--cap-add=SETUID --cap-add=SETGID --cap-add=SYS_CHROOT` if your runtime has
dropped them.

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

## Running the guest desktop in a window

By default the VM is headless (`-display none`, serial on stdio) and you reach
the guest over SSH. On a machine with a physical display you can instead watch
and drive the guest's desktop in a real window.

The window backends are not built by default — headless is the normal
deployment, where GTK/SDL would only add build dependencies for a window nobody
can see. Build them in once:

```bash
NVKVM_QEMU_UI=1 scripts/build_qemu.sh --force
```

Then start the VM with a display and a present mode:

```bash
VM_DISPLAY="gtk,gl=on" VM_SERIAL=none NVKVM_PRESENT_MODE=gl \
    scripts/run_test_vm.sh
```

`VM_DISPLAY` != `none` also adds `virtio-keyboard-pci` and `virtio-tablet-pci`,
without which the window would show the desktop and swallow every key and
click. The tablet reports absolute coordinates, so the host and guest pointers
track each other with no mouse grab.

Inside the guest, run a compositor on the **DRM backend** so it composites to
the virtual KMS head (the headless backend renders to an offscreen buffer that
never reaches the window):

```bash
sudo -E env XDG_RUNTIME_DIR=/run/user/1000 weston --backend=drm --renderer=gl
```

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
frames presented: ~637/s at 1920x1080, triple-buffered, 0 import errors
```

### Known gap: sync-fd passback

`SEMSURF_FENCE_CREATE` returns a sync fd that we cannot yet translate across
the VM boundary, so the guest reports `supports_sync_fd = 0` in `GET_DEV_INFO`
and NVIDIA's userspace takes a path that does not need one. Implementing
passback (a guest `dma_fence` signalled from a host watcher) would let the
sync-fd presentation path work and is the next step for present latency.
