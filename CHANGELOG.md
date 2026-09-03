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

  *Workaround if you stay on v0.2.1:* run with `--network host` and drop the
  `-p` flag, so QEMU's loopback bind is the host's loopback.

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
