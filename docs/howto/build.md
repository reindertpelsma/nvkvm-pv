# Build

Three artefacts, built in this order:

1. the **isolate stub** — a freestanding static binary, plus a generated header
   that embeds it as a byte array;
2. **QEMU 9.2.0** with the `virtio-nvgpu` device patched in;
3. the **guest kernel module** — built inside the guest, against the guest
   kernel.

Steps 1 and 2 are one script. Step 3 is done by cloud-init on first guest boot.

## Host prerequisites

- Linux with KVM and an NVIDIA GPU with the driver installed. QEMU's device
  realize opens `/dev/nvidiactl` and fails hard if it cannot
  (`src/qemu/virtio_nvgpu.c:1152-1158`).
- Unprivileged user namespaces enabled — the isolate sandbox uses
  `CLONE_NEWUSER` plus `pivot_root` (`src/qemu/nvkvm_isolate.c:124-231`).
- Root, for `apt-get` and for writing `/opt` and `/usr/lib/nvkvm`.

`scripts/build_qemu.sh` installs its own dependencies
(`scripts/build_qemu.sh:28-49`):

```
ninja-build meson libglib2.0-dev libpixman-1-dev python3 python3-venv
python3-tomli git libslirp-dev pkg-config libattr1-dev
libepoxy-dev libgbm-dev libegl-dev libdrm-dev
xxd
```

The EGL/GBM/DRM four are for the host present path; `xxd` is how the stub gets
embedded.

## Build the stub and QEMU

```bash
sudo bash scripts/build_qemu.sh
```

Idempotent: it exits 0 immediately if `/opt/qemu-nvkvm/bin/qemu-system-x86_64`
already exists (`scripts/build_qemu.sh:16-19`). To force a rebuild, remove that
binary or the whole `/opt/qemu-nvkvm`.

What it does, in the order it does it:

**1b. Build and install the isolate stub** (`scripts/build_qemu.sh:65-69`).
`make -C src/stub` produces `nvkvm_stub` and `nvkvm_stub_bin.h`. The build flags
matter (`src/stub/Makefile:11-18`): `-nostdlib -static -fPIE -ffreestanding
-fno-stack-protector -fno-builtin`, linked `-nostdlib -static -pie
-Wl,-z,relro,-z,now` against `-lgcc` only. Then `strip --strip-all` and
`xxd -i` to produce the embed header. The binary is also installed to
`/usr/lib/nvkvm/nvkvm_stub` as the runtime fallback path.

This step is load-bearing and its absence is silent — the script's own comment
(`scripts/build_qemu.sh:52-64`):

> with neither the define nor the generated header the QEMU build SUCCEEDS with
> `stub_elf = NULL`, `stub_elf_len = 0`, and silently falls back to
> `/usr/lib/nvkvm/nvkvm_stub` at runtime. On a fresh box that path does not
> exist, so `fexecve` fails and every isolate device-open returns `-ENOENT`:
> `nvkvm-gpu[GA106] M5.1: open ctl/gpu FAILED r1=-2 r2=-2 — forwarding OFF`
> i.e. the device comes up with forwarding OFF and NOTHING says why.

**2. Fetch QEMU** (`scripts/build_qemu.sh:74-75`):

```bash
git clone --depth=1 --branch v9.2.0 \
    https://gitlab.com/qemu-project/qemu.git /opt/qemu-src
```

**3-4. Inject the nvkvm sources by copy** (`scripts/build_qemu.sh:82-100`). Not
a symlink, not a submodule:

```bash
cp src/qemu/*.c src/qemu/*.h              /opt/qemu-src/hw/misc/
mkdir -p                                   /opt/qemu-src/hw/misc/nvkvm_inc
cp src/abi/*.h src/common/*.h              /opt/qemu-src/hw/misc/nvkvm_inc/
cp src/qemu/nvkvm_linux_types.h            /opt/qemu-src/hw/misc/nvkvm_inc/linux_types_compat.h
cp src/stub/nvkvm_stub_bin.h               /opt/qemu-src/hw/misc/
```

All of `src/abi` and `src/common` is copied, not a hand-picked subset — an
incomplete copy fails the build (`scripts/build_qemu.sh:88-90`). The stub blob
lands in `hw/misc/` rather than `nvkvm_inc/` because `nvkvm_isolate.c` includes
it by bare name.

**Consequence for iteration:** editing `src/qemu/*.c` does nothing to an
existing `/opt/qemu-src` tree until the copy is repeated. See
[Rebuilding after a source change](#rebuilding-after-a-source-change).

**5. Rewrite includes** (`scripts/build_qemu.sh:111-118`). Two `sed` passes:
`"../../src/{common,abi}/X.h"` → `"nvkvm_inc/X.h"`, and
`<linux/types.h>` → `"linux_types_compat.h"` (QEMU's `osdep.h` conflicts with
the kernel UAPI types).

**6. Patch `hw/misc/meson.build`** (`scripts/build_qemu.sh:126-172`) —
idempotent, guarded by `grep -q virtio_nvgpu.c`. Adds
`nvkvm_inc = include_directories('nvkvm_inc')` and a `system_ss.add(when:
['CONFIG_VIRTIO'], ...)` block listing the eleven `.c` files.

**6b. Patch `hw/virtio/virtio.c`** (`scripts/build_qemu.sh:178-209`). Inserts
`[50] = "virtio-nvgpu",` into `virtio_device_names[]` right after the
`[VIRTIO_ID_GPIO]` entry — QEMU's table ends around ID 41 and
`virtio_id_to_name()` asserts `device_id < G_N_ELEMENTS(virtio_device_names)`.

**7. Configure** (`scripts/build_qemu.sh:214-231`):

```bash
./configure \
    --target-list=x86_64-softmmu \
    --enable-kvm \
    --disable-werror --disable-sdl --disable-gtk --disable-vnc \
    --enable-opengl \
    --disable-virglrenderer \
    --extra-cflags=-DNVKVM_STUB_EMBEDDED \
    --prefix=/opt/qemu-nvkvm
```

`--enable-opengl` pulls in the egl-headless display and
`dpy_gl_scanout_dmabuf`, which the present path uses. virglrenderer stays off:
the present path scans out the guest render target's own dma-buf, it does not go
through virtio-gpu GL.

`-DNVKVM_STUB_EMBEDDED` is the define whose absence produces the silent
forwarding-off failure described above.

**8-9. Build and install** (`scripts/build_qemu.sh:236-243`). QEMU 9.2
configures out of tree, so ninja runs against `/opt/qemu-src/build`.

Output: `/opt/qemu-nvkvm/bin/qemu-system-x86_64`.

## Compute-only build

To build a QEMU with no display attack surface at all — no DRM render node
forwarding, no NVKMS, no host EGL present code — add
`-DNVKVM_QEMU_GRAPHICS=0` to `--extra-cflags`
(`src/qemu/virtio_nvgpu.h:33-50`). This forces the runtime `graphics` property
off regardless of what is passed on the command line, and compiles
`nvkvm_present_egl.c` out.

Pair it with the matching guest module build (`make NVKVM_GRAPHICS=0`, below).
The headers say so explicitly: "deploy the two consistently".

## The guest kernel module

Built inside the guest, against the guest kernel's build tree.

```bash
cd /mnt/nvkvm/src/guest
make KDIR=/lib/modules/$(uname -r)/build
sudo insmod nvkvm-guest.ko
```

`scripts/setup_guest.sh` arranges for cloud-init to do exactly this on first
boot (`scripts/setup_guest.sh:86-89`), including installing
`linux-headers-virtual` (`:80`).

Unload is `sudo rmmod nvkvm_guest` — note the underscore; the module file is
`nvkvm-guest.ko` but the module name is `nvkvm_guest`.

**Objects** (`src/guest/Kbuild:3-8`): `nvkvm_main.o nvkvm_ioctl.o nvkvm_virtio.o
nvkvm_mmap.o nvkvm_session.o nvkvm_hostfile.o`, plus `nvkvm_drm.o nvkvm_kms.o`
unless graphics is disabled.

**Compute-only variant** (`src/guest/Makefile:4-9`, `src/guest/Kbuild:16-21`):

```bash
make NVKVM_GRAPHICS=0 KDIR=/lib/modules/$(uname -r)/build
```

This drops `nvkvm_drm.o` and `nvkvm_kms.o` and the `-DNVKVM_GRAPHICS` define, so
the resulting `.ko` carries no DRM, KMS or modeset code at all.

**Module parameters**:

| parameter | mode | default | meaning |
|---|---|---|---|
| `num_gpus` | 0444 | 1 | how many `/dev/nvidiaN` nodes to expose (clamped to 16) |
| `ring_idle_us` | 0644 | 0 | stub consumer-loop idle window, in spin iterations despite the name |
| `ring_enable` | 0644 | false | route flat `RM_CONTROL`s over the SPSC fast ring |

(`src/guest/nvkvm_main.c:67-69`, `:296-303`, `:414-426`.) `ring_enable` is off
by default deliberately — see
[the data plane](../../ARCHITECTURE.md#problem-5-the-data-plane).

## Rebuilding after a source change

`build_qemu.sh` short-circuits once the binary exists, so the incremental loop
is manual:

```bash
# QEMU-side change
cp src/qemu/*.c src/qemu/*.h /opt/qemu-src/hw/misc/
ninja -C /opt/qemu-src/build qemu-system-x86_64
cp /opt/qemu-src/build/qemu-system-x86_64 /opt/qemu-nvkvm/bin/qemu-system-x86_64

# stub change (also re-embeds it, so QEMU must be rebuilt after)
make -C src/stub
cp src/stub/nvkvm_stub /usr/lib/nvkvm/nvkvm_stub
```

(This is what `scripts/run_remote_test.sh rebuild` does at `:40-74`; that script
itself is hardcoded to a private dev box and is not a reproducible entry point.)

Guest-side, after remounting the 9p share:

```bash
sudo rmmod nvkvm_guest
cd /mnt/nvkvm/src/guest && sudo make -s
sudo insmod nvkvm-guest.ko
```

## Tests

**Unit tests** — host, no GPU, no VM. They compile the real QEMU-side sources
against stub headers in `tests/unit/stubs/`:

```bash
cd tests/unit && make && make run
```

Seven binaries: `test_dispatch test_frontend test_handle mock_stub test_isolate
test_tables test_open_scm` (`tests/unit/Makefile:33`).

**ABI parity** — host, needs Go with cgo:

```bash
cd tests/abi_parity && go test -v ./...
```

It asserts compiled-in struct sizes against hardcoded expectations sourced from
gVisor `pkg/abi/nvgpu` and OGKM `nvos.h`. Five tests: frontend struct sizes (22),
alloc-param struct sizes, UVM struct sizes (23), the guest↔QEMU protocol struct
sizes, and ioctl-number range/distinctness checks. It talks to no driver and no
GPU. Several expectations carry the bug they were added to catch — e.g. NVOS32
must be 184, not 88, because an 88 truncated the `ALLOC_SIZE` union and libGLX
saw `size=0`.

> `.gitignore` gotcha: `*.mod` matches `go.mod`, which is how
> `tests/abi_parity/go.mod` was once dropped during a repo extraction. The
> negations `!go.mod` / `!go.sum` at `.gitignore:12-13` restore it. If you fork
> or re-extract this tree, check that file survived.

**Integration tests** — inside the guest, module loaded. Only `test_ioctl_fwd`
has a Makefile rule (`tests/integration/Makefile`); the other ~20 programs are
compiled ad hoc:

```bash
mkdir -p /tmp/build/abi && cp /mnt/nvkvm/src/abi/*.h /tmp/build/abi/
gcc -O0 -g -Wall -I/tmp/build -o /tmp/test_ioctl_fwd \
    /mnt/nvkvm/tests/integration/test_ioctl_fwd.c
gcc -O0 -g -o /tmp/cuinit_test \
    /mnt/nvkvm/tests/integration/cuinit_test.c -ldl
```

Add `-ldl` for the ones that `dlopen` `libcuda`.

**Perf matrices** — run from a third machine over SSH to both host and guest,
strictly serially on one GPU:

```bash
HOST_SSH=vh GUEST_SSH=vg tests/perf/run_matrix.sh    # CUDA + PyTorch + LLM
HOST_SSH=vh GUEST_SSH=vg tests/perf/run_graphics.sh  # Vulkan / EGL / NVENC
HOST_SSH=vh GUEST_SSH=vg tests/perf/run_parity.sh    # microbenchmarks
```

Default gate is 0.90 guest/host; exit 0 means every gate passed. The
methodology rules the harness enforces are at `tests/perf/README.md:19-27`, and
one of them is a hard requirement on the VM: guest RAM must exceed the model
size, or model load becomes disk-bound and fakes a large gap.

The ring unit tests (`tests/unit/ring_test.c`, `ring_loop_test.c`) are **not**
in the Makefile and do not currently compile — they predate the `cap` parameter
added to `nvkvm_ring_reserve`/`_peek`. `ring_setup_test.c` still compiles.
