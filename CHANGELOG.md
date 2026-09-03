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
