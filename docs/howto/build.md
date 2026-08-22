# Build

Three artefacts, built in this order:

1. the **isolate stub** — a freestanding static binary, plus a generated header
   that embeds it as a byte array;
2. **QEMU 9.2.0** with the `virtio-nvgpu` device patched in;
3. the **guest kernel module** — built inside the guest, against the guest
   kernel.

Steps 1 and 2 are one script. Step 3 is done by cloud-init on first guest boot.

The QEMU delta is a **patch series** in [`patches/`](../../patches/) plus a file
copy, so `scripts/build_qemu.sh` is a convenience and not the only path —
[Building without the script](#building-without-the-script) is the same
sequence as commands you can type, and
[`patches/README.md`](../../patches/README.md) is the whole upstream surface in
one table: ten patches, 760 added lines and 29 removed, against `v9.2.0`.

## You may not need to build this

Two prebuilt paths exist and both are produced from a tag by
[`.github/workflows/release.yml`](../../.github/workflows/release.yml), on a
GitHub-hosted runner, from the same `scripts/build_qemu.sh` and `Dockerfile`
this page describes:

| | |
|---|---|
| `ghcr.io/reindertpelsma/nvkvm-pv` | the container image, built by the `Dockerfile` |
| `nvkvm-<version>-linux-x86_64.tar.gz` | the built QEMU tree plus the tag's source, on the [releases page](https://github.com/reindertpelsma/nvkvm-pv/releases) |

Build from source anyway if you want the GTK/SDL window (`NVKVM_QEMU_UI=1` —
the release binary is headless), a compute-only build, an older glibc than
Ubuntu 24.04's, or if you are changing anything under `src/`.

### Verify before you run either

An attestation nobody checks is decoration, so this is the whole point of
publishing them. [`gh`](https://cli.github.com/) checks the artifact in your
hand against a signed statement about which repository, commit and workflow run
produced it:

```bash
# the image, resolved from the registry
gh attestation verify oci://ghcr.io/reindertpelsma/nvkvm-pv:v0.0.1-rc2 \
    --repo reindertpelsma/nvkvm-pv \
    --signer-workflow reindertpelsma/nvkvm-pv/.github/workflows/release.yml

# the tarball, on disk
gh attestation verify nvkvm-<version>-linux-x86_64.tar.gz \
    --repo reindertpelsma/nvkvm-pv \
    --signer-workflow reindertpelsma/nvkvm-pv/.github/workflows/release.yml
```

`--repo` is not optional in spirit: without it `verify` accepts an attestation
from *any* repository, which downgrades a provenance check to a signature check.
`--signer-workflow` narrows it further to the one workflow file allowed to
produce these, so a different workflow in this same repository — added by
someone who got write access — would not satisfy it.

`gh attestation verify` reaches out to `api.github.com` for the attestation and
to Sigstore for the trust root. To verify on a machine with neither, fetch both
in advance and hand them over:

```bash
gh attestation download nvkvm-<version>-linux-x86_64.tar.gz --repo reindertpelsma/nvkvm-pv
gh attestation trusted-root > trusted_root.jsonl
# ... on the offline machine:
gh attestation verify nvkvm-<version>-linux-x86_64.tar.gz --repo reindertpelsma/nvkvm-pv \
    --bundle sha256:<digest>.jsonl --custom-trusted-root trusted_root.jsonl
```

### What is in the tarball, and how to check it against the tag

| | |
|---|---|
| `qemu-nvkvm/` | the build output of `scripts/build_qemu.sh` — `bin/qemu-system-x86_64` and QEMU's data files |
| `src/stub/nvkvm_stub` | the built isolate stub. It is *also* embedded in the QEMU binary; this copy is the `NVKVM_STUB_PATH` runtime fallback (step 1b below) |
| everything else | the repository at that tag, verbatim — the workflow produces it with `git archive` |

The source half is `git archive` output on purpose: it makes "does this tarball
match the tag?" answerable rather than a matter of trust.

```bash
V=<version>                                     # e.g. v0.1.0
tar xzf "nvkvm-$V-linux-x86_64.tar.gz"
rm -rf "nvkvm-$V/qemu-nvkvm" "nvkvm-$V/src/stub/nvkvm_stub" "nvkvm-$V/RELEASE.md"

git clone https://github.com/reindertpelsma/nvkvm-pv.git nvkvm
mkdir ref && git -C nvkvm archive --format=tar --prefix="nvkvm-$V/" "$V" | tar -x -C ref

diff -r "ref/nvkvm-$V" "nvkvm-$V"               # silence == identical to the tag
```

The three paths removed first are the only things the workflow adds: the two
build outputs and a generated `RELEASE.md`.

The guest kernel module is deliberately absent from both artifacts. It is an
out-of-tree module and has to be compiled against the running guest kernel, so
there is no such thing as a prebuilt one that is right for your guest —
`src/guest/` ships instead, and [the guest kernel module](#the-guest-kernel-module)
below is what happens to it.

### Installing the tarball

```bash
sudo cp -a qemu-nvkvm /opt/qemu-nvkvm
sudo install -Dm755 src/stub/nvkvm_stub /usr/lib/nvkvm/nvkvm_stub
```

`/opt/qemu-nvkvm/bin/qemu-system-x86_64` is where `scripts/run_test_vm.sh` looks
(`scripts/run_test_vm.sh:30`); `QEMU_BIN` overrides it if you would rather leave
the tree where you unpacked it. Runtime packages and the measured glibc floor
are in `RELEASE.md` inside the tarball — it is a real binary built on Ubuntu
24.04, so an older distro needs the source build.

## Host prerequisites

- Linux with KVM and an NVIDIA GPU with the driver installed. QEMU's device
  realize opens `/dev/nvidiactl` and fails hard if it cannot
  (`src/qemu/virtio_nvgpu.c:1152-1158`).
- Unprivileged user namespaces enabled — the isolate sandbox uses
  `CLONE_NEWUSER` plus `pivot_root` (`src/qemu/nvkvm_isolate.c:124-231`).
- **No root.** The build installs to `/opt` when that is writable and to
  `${XDG_DATA_HOME:-~/.local/share}/nvkvm` otherwise; override either path with
  `NVKVM_QEMU_PREFIX` / `NVKVM_QEMU_SRC`. Verified by building the whole thing
  as an unprivileged user with no sudo available to it.
- **Any distro.** The build needs the dependencies below by capability, not by
  package name; nothing in it is Debian-specific.

`scripts/build_qemu.sh` does **not** install anything unless you ask it to. It
probes, and names what is missing plus the package line for your distro
(Debian/Ubuntu, Fedora/RHEL, Arch and openSUSE are recognised). Pass
`--install-deps` to have it install them, which is the only step that wants
root.

What it probes for — these are the real requirements:

| kind | needs |
|---|---|
| on `PATH` | `git`, `ninja`, `meson`, `pkg-config`, `python3`, `xxd` |
| `pkg-config` modules | `glib-2.0`, `pixman-1`, `slirp`, `epoxy`, `gbm`, `egl`, `libdrm` |

The EGL/GBM/DRM three are for the host present path; `xxd` is how the stub gets
embedded. On Debian/Ubuntu that is:

```
ninja-build meson libglib2.0-dev libpixman-1-dev python3 python3-venv
python3-tomli git libslirp-dev pkg-config libattr1-dev
libepoxy-dev libgbm-dev libegl-dev libdrm-dev
xxd
```

### Building without the script

The script is a convenience, not the mechanism — everything below it is an
ordinary QEMU build, and the numbered steps in the rest of this document are
what it runs, in order. If you would rather not run it: install the
dependencies above, then follow those steps by hand. The only nvkvm-specific
parts are step 1b (build `src/stub`, which generates the embed header) and
steps 3–5 (copy `src/qemu` + `src/abi` + `src/common` into `hw/misc/` and fix
the include paths); the rest is `configure` and `ninja`.

## Build the stub and QEMU

```bash
bash scripts/build_qemu.sh                 # rootless if /opt is not writable
bash scripts/build_qemu.sh --install-deps  # + install missing deps (needs root)
```

Guarded, not idempotent-in-the-useful-sense: it exits 0 immediately if the
QEMU binary already exists under the chosen prefix
(`scripts/build_qemu.sh:37-41`). **A plain re-run after editing anything under
`src/qemu/` or `src/common/` is therefore a silent no-op** — you keep testing the
old binary against new guest code, which surfaces as a confusing mismatch rather
than as a build error (a new `NVKVM_HFILE_*` id, for instance, comes back
"Invalid argument" from the stale binary's whitelist).

Pass `--force` to rebuild over the existing install
(`scripts/build_qemu.sh:25-44`). The QEMU source tree and the ninja build dir
are reused, so a forced rebuild is incremental — minutes, not the full ~20:

```bash
sudo bash scripts/build_qemu.sh --force
```

What it does, in the order it does it:

**1b. Build and install the isolate stub** (`scripts/build_qemu.sh:90-94`).
`make -C src/stub` produces `nvkvm_stub` and `nvkvm_stub_bin.h`. The build flags
matter (`src/stub/Makefile:11-18`): `-nostdlib -static -fPIE -ffreestanding
-fno-stack-protector -fno-builtin`, linked `-nostdlib -static -pie
-Wl,-z,relro,-z,now` against `-lgcc` only. Then `strip --strip-all` and
`xxd -i` to produce the embed header. The binary is also installed to
`/usr/lib/nvkvm/nvkvm_stub` as the runtime fallback path — skipped with a note
when that directory is not writable, which is harmless because the stub is
embedded in the QEMU binary anyway (`NVKVM_STUB_PATH` overrides it).

This step is load-bearing and its absence is silent — the script's own comment
(`scripts/build_qemu.sh:77-89`):

> with neither the define nor the generated header the QEMU build SUCCEEDS with
> `stub_elf = NULL`, `stub_elf_len = 0`, and silently falls back to
> `/usr/lib/nvkvm/nvkvm_stub` at runtime. On a fresh box that path does not
> exist, so `fexecve` fails and every isolate device-open returns `-ENOENT`:
> `nvkvm-gpu[GA106] M5.1: open ctl/gpu FAILED r1=-2 r2=-2 — forwarding OFF`
> i.e. the device comes up with forwarding OFF and NOTHING says why.

**2. Fetch QEMU** (`scripts/build_qemu.sh:99-100`):

```bash
git clone --depth=1 --branch v9.2.0 \
    https://gitlab.com/qemu-project/qemu.git /opt/qemu-src
```

**3-4. Inject the nvkvm sources by copy** (`scripts/build_qemu.sh:107-125`). Not
a symlink, not a submodule:

```bash
cp src/qemu/*.c src/qemu/*.h              /opt/qemu-src/hw/misc/
mkdir -p                                   /opt/qemu-src/hw/misc/nvkvm_inc
cp src/abi/*.h src/common/*.h              /opt/qemu-src/hw/misc/nvkvm_inc/
cp src/qemu/nvkvm_linux_types.h            /opt/qemu-src/hw/misc/nvkvm_inc/linux_types_compat.h
cp src/stub/nvkvm_stub_bin.h               /opt/qemu-src/hw/misc/
```

All of `src/abi` and `src/common` is copied, not a hand-picked subset — an
incomplete copy fails the build (`scripts/build_qemu.sh:113-115`). The stub blob
lands in `hw/misc/` rather than `nvkvm_inc/` because `nvkvm_isolate.c` includes
it by bare name.

**Consequence for iteration:** editing `src/qemu/*.c` does nothing to an
existing `/opt/qemu-src` tree until the copy is repeated. See
[Rebuilding after a source change](#rebuilding-after-a-source-change).

**5. Rewrite includes** (`scripts/build_qemu.sh:136-143`). Two `sed` passes:
`"../../src/{common,abi}/X.h"` → `"nvkvm_inc/X.h"`, and
`<linux/types.h>` → `"linux_types_compat.h"` (QEMU's `osdep.h` conflicts with
the kernel UAPI types).

**6. Patch `hw/misc/meson.build`** (`scripts/build_qemu.sh:151-197`) —
idempotent, guarded by `grep -q virtio_nvgpu.c`. Adds
`nvkvm_inc = include_directories('nvkvm_inc')` and a `system_ss.add(when:
['CONFIG_VIRTIO'], ...)` block listing the eleven `.c` files.

**6b. Patch `hw/virtio/virtio.c`** (`scripts/build_qemu.sh:203-234`). Inserts
`[50] = "virtio-nvgpu",` into `virtio_device_names[]` right after the
`[VIRTIO_ID_GPIO]` entry — QEMU's table ends around ID 41 and
`virtio_id_to_name()` asserts `device_id < G_N_ELEMENTS(virtio_device_names)`.

**7. Configure** (`scripts/build_qemu.sh:239-256`):

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

**8-9. Build and install** (`scripts/build_qemu.sh:261-268`). QEMU 9.2
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

`build_qemu.sh` short-circuits once the binary exists, so a plain re-run changes
nothing. Use `--force`; it reuses the QEMU tree and the ninja build dir, so it is
incremental:

```bash
sudo bash scripts/build_qemu.sh --force
```

The manual loop people reach for instead is:

```bash
# QEMU-side change
cp src/qemu/*.c src/qemu/*.h /opt/qemu-src/hw/misc/
ninja -C /opt/qemu-src/build qemu-system-x86_64
cp /opt/qemu-src/build/qemu-system-x86_64 /opt/qemu-nvkvm/bin/qemu-system-x86_64

# stub change (also re-embeds it, so QEMU must be rebuilt after)
make -C src/stub
cp src/stub/nvkvm_stub /usr/lib/nvkvm/nvkvm_stub
```

**Both of those recipes are broken on their own, and so is
`scripts/run_remote_test.sh rebuild` (`:44-80`)** — that script is in any case
hardcoded to a private dev box and is not a reproducible entry point. Copying
`src/qemu/*.c` back over `hw/misc/` restores the `../../src/common/...` and
`../../src/abi/...` includes that step 5 rewrote, so the next `ninja` stops at
`fatal error: ../../src/common/nvkvm_proto.h: No such file or directory`. Either
replicate steps 4 and 5 by hand after every copy, or — simpler and what you
should actually do — re-run the script:

```bash
sudo bash scripts/build_qemu.sh --force
```

If you do drive ninja by hand, read its output: on failure the *old* binary
stays installed at `/opt/qemu-nvkvm/bin/`, and the experiment silently measures
nothing.

Guest-side, after remounting the 9p share:

```bash
sudo rmmod nvkvm_guest
cd /mnt/nvkvm/src/guest && sudo make -s
sudo insmod nvkvm-guest.ko
```

## Tests

### The four gates that need no GPU

These are what CI runs on every push
([`.github/workflows/ci.yml`](../../.github/workflows/ci.yml)), and the same
four commands run locally. Run all four before claiming a change is done — a
targeted fix here once silently regressed 27 unrelated probes.

| gate | command | what it catches |
|---|---|---|
| unit suite | `bash tests/unit/run_tests.sh` | a suite that fails, **and** a suite that silently stops running |
| QEMU compile check | `bash tests/qemu_syntax_check.sh` and `CC=gcc-14 bash tests/qemu_syntax_check.sh` | build breakage in `src/qemu/` without a 20-minute QEMU build |
| ABI parity | `cd tests/abi_parity && go test -count=1 ./...` | a profile row or selector boundary that disagrees with the C structs |
| shellcheck | `docker run --rm -v "$PWD:/mnt" koalaman/shellcheck:v0.10.0 -S warning scripts/*.sh tests/qemu_syntax_check.sh tests/unit/run_tests.sh` | a quoting bug that would fail at hour three of a paid sweep |

The GPU is deliberately absent: GitHub-hosted runners have no NVIDIA device, so
`tests/validate.sh` cannot run there. Hardware coverage comes from
`scripts/sweep_matrix.py` against rented boxes.

**Unit tests** — host, no GPU, no VM. They compile the real QEMU-side sources
against stub headers in `tests/unit/stubs/`:

```bash
bash tests/unit/run_tests.sh        # or: make -C tests/unit check
```

**Use `run_tests.sh`, not `make run`.** `tests/unit/Makefile:70-77` runs the
suites raw and exits non-zero *by design*: `test_isolate` fails 5 of its 7 cases
at runtime — pre-existing API drift between the test and the isolate table,
documented at `tests/unit/Makefile:65-69` — which makes `make run` useless as a
pass/fail signal. `run_tests.sh` names those five explicitly and is green when
exactly those fail.

It is also stricter than `make` in the way that matters. `make` aborts on the
first error, so on 2026-08-20 a `test_isolate` link failure meant `test_tables`
and `test_open_scm` were never built and **26 passing assertions silently
stopped running**, with nothing saying so. `run_tests.sh` builds with `make -k`,
then requires every expected binary to exist and every suite to report its
pinned assertion count (`tests/unit/run_tests.sh:1-33`).

Eight binaries, all of which build (`tests/unit/Makefile:39`):

| suite | pinned expectation |
|---|---|
| `test_dispatch` | 39/39 |
| `test_tables` | 17/17 |
| `test_handle` | 9/9 |
| `test_frontend` | 8/8 |
| `test_open_scm` | prints `ALL OPEN_SCM TESTS PASSED` |
| `test_ctrl_gate` | prints `test_ctrl_gate: PASS` |
| `test_isolate` | 7 cases run, exactly 5 known-failing |
| `mock_stub` | not a suite — the fake isolate `test_isolate` spawns |

If you add cases, bump the count in `run_tests.sh`; that one-line diff is the
point of pinning them. If one of the five known-failing isolate cases starts
passing, the suite also goes red — that means the drift got fixed and the file
needs updating, not that the test is wrong.

`test_dispatch` targets `src/qemu/nvkvm_dispatch.c`, which is dead code (see
[the pointer audit](../internal/audit-guest-pointers.md), section 2).

**ABI parity** — host, needs Go with cgo:

```bash
cd tests/abi_parity && go test -count=1 ./...
```

**`-count=1` is mandatory.** Go's build cache does track the cgo-included
header, but the *test-result* cache is keyed on the test binary, so a plain
`go test` after editing `src/common/nvkvm_abi.h` can replay a stale pass — which
would make the gate worthless in exactly the situation it exists for. Observed:
breaking the 610 bucket and re-running `go test` reported `ok`; `-count=1`
failed correctly.

It asserts compiled-in struct sizes against hardcoded expectations sourced from
gVisor `pkg/abi/nvgpu` and OGKM `nvos.h`. Nine tests across two files. Six in
`abi_parity_test.go`: frontend struct sizes (23 cases), alloc-param struct sizes
(5), UVM struct sizes (23), the guest↔QEMU protocol struct sizes, and two
ioctl-number range/distinctness checks. Three more in `abi_profile_test.go`
cover the ABI profile table itself — that each measured driver version selects
the expected profile, that the boundaries *inside* the 535 and 550 branches are
sharp, and that versions on the same side of a boundary stay put. It talks to no driver and no
GPU. Several expectations carry the bug they were added to catch — e.g. NVOS32
must be 184, not 88, because an 88 truncated the `ALLOC_SIZE` union and libGLX
saw `size=0`.

> `.gitignore` gotcha: `*.mod` matches `go.mod`, which is how
> `tests/abi_parity/go.mod` was once dropped during a repo extraction. The
> negations `!go.mod` / `!go.sum` at `.gitignore:12-13` restore it. If you fork
> or re-extract this tree, check that file survived.

**Integration tests** — inside the guest, module loaded. Two programs have
Makefile rules — `test_ioctl_fwd` and `fbo_formats_probe`, the offscreen-EGL FBO
completeness probe (`tests/integration/Makefile:4-12`); the other ~20 are
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
