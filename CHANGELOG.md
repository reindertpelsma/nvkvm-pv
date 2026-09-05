# Changelog

Notable changes per release. The audience is someone who already has nvkvm
running and wants to know whether upgrading will break it — so anything that
can break a working setup is called out explicitly, and everything else is
summary.

Releases are tagged in this repo; the tag is the authoritative artifact.

## Compatibility

Two things can break a setup that currently works. Both are stated per release.

### 1. The guest ↔ QEMU wire protocol

The guest kernel module and QEMU must be built from the **same nvkvm
revision**. `NVKVM_PROTO_VERSION` (`src/common/nvkvm_proto.h`) is checked at
guest probe against the value QEMU publishes in shared memory, the match is
**exact**, and a mismatch fails closed:

```
nvkvm: protocol version mismatch: host=N guest=M
```

with `-EPROTO` from `nvkvm_virtio_probe()`. It never runs half-upgraded. If you
see that line, rebuild the guest module from the same tree as QEMU.

| release | `NVKVM_PROTO_VERSION` | rebuild both sides? |
|---|---|---|
| v0.2.5 | 3 | no for compatibility — but **rebuild both sides** or you do not get the Vulkan fix |
| v0.2.2 | 3 | no — unchanged |
| v0.2.1 | 3 | no — unchanged from v0.2.0 |
| v0.2.0 | 3 | **yes** — bumped 2 → 3 |
| v0.1.0 | 2 | — |

The 2 → 3 change expanded the dormant UVM state snapshot so `REGISTER_GPU`
replay carries its embedded RM control handle and both RM object handles. See
[`docs/reference/virtio-protocol.md`](docs/reference/virtio-protocol.md).

### 2. Container images

`nvkvm-steamos/docker-compose.yml` defaults every image to
`${NVKVM_IMAGE_TAG:-local}`, so the normal path builds locally and no registry
push can change what you run. If you have set `NVKVM_IMAGE_TAG=latest`, pin it
to a release tag instead — `latest` moves.

## [v0.2.5] — 2026-09-05

**Every Vulkan host-pointer import failed in the guest.** If you run any Vulkan
workload that stages through application memory, upgrade — and rebuild BOTH
sides (see below).

`NVKVM_PROTO_VERSION` is unchanged at 3, so a half-upgraded pair still *runs*.
It just does not get the fix: the guest change and the QEMU change are each
inert without the other, and the failure mode of a mismatched pair is exactly
the old behaviour, not a new one.

**Fixed**

- **`vkAllocateMemory` with `VK_EXT_external_memory_host` returned
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` — at every size, on every GPU and driver.**
  1 MiB failed just as reliably as 512 MiB, so it never looked like a memory
  problem, and the guest advertised the extension, the same memory heaps and the
  same `maxMemoryAllocationSize` as the host. Measured on RTX 4070 / 595.84 and
  RTX 3050 Laptop / 580.173.02, two independent trees, an idle machine:

  ```
  IMPORT   1 MiB: VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)
  IMPORT 256 MiB: VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)
  ```

  The cause was one denied function code. Vulkan's host-pointer import reaches
  RM as `NV_ESC_RM_VID_HEAP_CONTROL` with NVOS32 function 27
  (`ALLOC_OS_DESCRIPTOR`), and the U-3 gate allowed only function 2
  (`ALLOC_SIZE`).

  U-3 was right about the hazard and wrong only about the remedy. Function 27
  hands RM a caller-named address bound for `pin_user_pages()` in the isolate —
  the same primitive A-1 already faced for `NV_ESC_RM_ALLOC_MEMORY` with
  `hClass 0x71` — and A-1 did not answer it with denial. It answered with
  validated translation: the guest migrates the range onto memfds the stub
  `MAP_FIXED`s, QEMU proves coverage with `iso_mmap_translate()` and forwards
  the address **it** chose. Function 27 now takes that same path, so a range the
  host did not install cannot be described and the arbitrary-pin primitive never
  comes into existence. Everything else on that ioctl — 3 FREE, 5 INFO, 19
  HW_ALLOC, 20, and anything the driver adds later — stays denied.

  What it was worth, on an RTX 4070 against a 186,467 bare-metal average:

  | | Geekbench 7 GPU (Vulkan) |
  |---|---|
  | before | 56,157 — **30.1%**, Path Tracer scoring 0 of 11 workloads |
  | after | 175,398–181,298 — **94–97%**, all 11 workloads |

  OpenCL never touches this path, which is why the published OpenCL parity
  numbers in [`docs/reference/parity.md`](docs/reference/parity.md) were never
  affected and never wrong.

- **The "NVDEC has no baseline, it hangs on the host too" claim was false**, and
  it had deleted its own test. `testsrc` through `libx264` with default settings
  produces **yuv444p**; NVDEC accepts 4:2:0 only, so `h264_cuvid` refused the
  file with `rc=234` — failing fast, not hanging. On the strength of that
  reading, `tests/perf/graphics_remote.sh` dropped the decode row entirely, so
  there was no NVDEC coverage at all. Source is now generated `-pix_fmt
  yuv420p` and the row is back. Measured 2026-09-05, RTX 4070 / 595.84, one
  300-frame 1080p clip: host `h264_cuvid` 33.3x, guest 13.3x, both decoding all
  300 frames. Those are **not** a parity ratio — the two sides ran different
  ffmpeg builds, a confound this measurement did not control.

**Added**

- **`vk_import_host_ptr` in `tests/validate.sh`.** The bug above was invisible
  to every test in the tree and surfaced only as a benchmark composite reading
  badly — one workload of eleven scoring 0 while the other ten sat at ~93%. A
  composite is a poor regression detector, so this does the one call that
  failed: import 2 MiB of application memory and free it. It enables the
  extension only when the driver advertises it and SKIPs otherwise. Verified to
  discriminate: PASS on the fixed QEMU, FAIL on the unfixed one, same guest.
- **`nvos32_osdesc_desc_off` / `_limit_off` in `tools/abi_derive.sh`.** The
  descriptor and limit live at NVOS32 offsets 64 and 72; that was established by
  dumping the union in the guest and correlating against a pointer and length
  the caller controlled, then confirmed by `offsetof()` probes compiled against
  NVIDIA's headers at 535.183.01, 550.90.07, 570.86.16, 580.95.05 and 590.48.01
  — one layout across the range. The probes are permanent, so a driver that
  moves those fields shows up as a changed row rather than as a wrong pointer
  handed to `pin_user_pages()`.

**Not established**

- The fix is confirmed end to end on **one** GPU and driver (RTX 4070 /
  595.84): reproducer, `validate.sh` 37/37, and the Geekbench numbers above. The
  *bug* was independently reproduced on RTX 3050 Laptop / 580.173.02, but the
  *fix* has not run there. On an RTX 5090 (Blackwell GB202, 580.105.08) the
  fixed QEMU builds, the guest boots, the GPU forwards and the guest module
  loads — the Vulkan import itself was not reached, because the rented box ran
  out of disk before its NVIDIA userspace could be staged.

> **Note:** v0.2.3 and v0.2.4 were tagged without changelog entries. This file
> jumps v0.2.2 → v0.2.5 for that reason, not because those tags do not exist.

## [v0.2.2] — 2026-09-03

**The quickstart in v0.2.0 and v0.2.1 could not reach its own guest.** If you
are on either, upgrade — or see the workaround below.

**Fixed**

- **`ssh -p 2222 ubuntu@127.0.0.1` reset the connection**, on both previous
  releases. QEMU forwarded the guest's ssh to the *container's* loopback, while
  Docker's `-p 127.0.0.1:2222:2222` bridges the host's loopback to the
  container's **eth0** — so `docker-proxy` accepted the connection, found
  nothing listening inside, and dropped it. The guest was healthy the whole
  time and simply listening on an address Docker never bridges to. Measured:

  ```
  inside the container netns → 127.0.0.1:2222   →  SSH-2.0-OpenSSH_9.6p1
  container eth0 172.17.0.2:2222                →  Connection refused
  ```

  The bind is now `VM_SSH_BIND`. It still defaults to `127.0.0.1`, which is the
  remediation for the audit finding it came from — guest ssh on `0.0.0.0` with
  `ubuntu:ubuntu` and `NOPASSWD:ALL` published a root shell on rented
  public-IP boxes — and only `container-entrypoint.sh`, which owns a network
  namespace, widens it. In the container case exposure is set by the address in
  `-p`, so **keep the `127.0.0.1:` in it**.
  `tests/sweep_untrusted_endpoint_test.sh` now asserts both halves: the default
  is loopback, and the entrypoint is the only thing that widens it.

  *Workaround if you stay on v0.2.1:* reach the guest from inside the
  container's own network namespace, where the forward has always worked —
  `docker exec -it <container> ssh -p 2222 ubuntu@127.0.0.1` (the image ships
  an ssh client). Do **not** use `--network host` for this: it would hand the
  VMM container the host's entire network namespace to work around a
  one-address bug.

- **The guest password was documented nowhere.** cloud-init logs "no authorized
  SSH keys ... for user ubuntu", the guest offers `publickey,password`, and the
  credential is `ubuntu:ubuntu`. It is on the ssh line in the README now.
- **The README pinned `:v0.2.0`**, so every copy-paste ran the previous release
  — without, among other things, the `DRIVER_SYNCOBJ` fix gamescope needs to
  drive a connector.
- **`tests/validate.sh` aborted instead of reporting.** `REPRO_DIR` was read at
  the Vulkan ray-tracing-extension check and assigned ~460 lines later, so under
  `set -u` the run died with "REPRO_DIR: unbound variable" before printing
  TOTAL/VERDICT. That branch is only reached when `vk_compute_dispatch` FAILS,
  so the suite produced no verdict exactly when one was needed.
- **12 broken relative links**, 11 of them in `docs/howto/install.md`, which was
  written as though it sat at the repository root. Repo-wide there are now zero
  broken links and zero broken anchors.

**Added**

- `scripts/nvkvm-report.sh` — one pass over everything a bug report needs,
  host or guest, auto-detected. Reports MAXPHYADDR, GPU runtime power state,
  whether QEMU is actually a patched build, and whether `vfio-pci` has a GPU
  bound. Points at QEMU's stderr, which nothing else collects.
- `sweep.sh` verifies **CUDA works on the host** before crediting a row to
  nvkvm, with its own `host-cuda-broken` status. A rented box can pass
  `systemd-detect-virt`, `/dev/kvm` and `nvidia-smi` and still fail `cuInit`
  with rc=3 on the bare host; every CUDA check would then fail in the guest and
  be recorded as an nvkvm result. Measured on a rented RTX 3060, 2026-09-03.
- `CHANGELOG.md`, and an index for `docs/audit/` and `docs/investigations/` —
  18 documents that `docs/README.md` never listed.

**Verified on hardware**: RTX 3070 / driver 575.51.03, rented KVM box, the
README quickstart run verbatim from the host on a fresh volume —
`TOTAL 36  PASS 35  FAIL 0  SKIP 1  UNTESTED 0`.

**Compatibility**: none. Protocol unchanged at 3.

## [v0.2.1] — 2026-09-03

First public release. Fifteen commits on top of v0.2.0, mostly hardening and
documentation.

**Fixed**

- **stub: die with QEMU** (`PR_SET_PDEATHSIG`). A stub outliving its QEMU was an
  un-evadable DoS: socket-EOF cleanup is cooperative, `SIGKILL` is not. Set
  inside the stub (the kernel clears it across `execve` on a credential change)
  and before seccomp, since `prctl` is not in the allowlist.
- **guest/drm: declare `DRIVER_SYNCOBJ`**, which gamescope requires before it
  will drive a connector.
- **guest: `page_mapcount` shim guarded at the wrong kernel version** — it was
  gated at 6.16, but the symbol was removed in 6.12.
- **broker: give the display window a minimum size** — it could start collapsed.
- **ABI: allocation-parameter sizes** corrected, and the hardware verification
  the `0xcb33` row asked for is now recorded.
- **Packaging**: the test-harness ignores are mirrored into `.dockerignore`, so
  a build no longer depends on the state of `tests/harness/`.

**Documentation**

- `SECURITY.md` corrected where the code contradicted it, and all fifteen audit
  documents linked rather than three.
- The BAR1 VA investigation concluded: it is **not** a leak — deferred cleanup
  while live host referrers hold the mapping. `docs/investigations/va-space-leak/`.
- FAQ: `/dev/kvm` is not the Docker socket, why containerising the game fails,
  and the `/dev/udmabuf` exposure rated with its reasoning.

**Compatibility**: none. Protocol unchanged at 3; no CLI or config changes.

## [v0.2.0] — 2026-09-02

559 commits since v0.1.0, not itemised retroactively — this changelog starts
here, and releases from v0.2.1 on are itemised as they happen.

**Compatibility**: `NVKVM_PROTO_VERSION` **2 → 3**. Guest module and QEMU must
both be rebuilt from this revision or later; a mixed pair fails closed at probe.

## [v0.1.0] — 2026-08-22

Protocol version 2. Pre-release.
