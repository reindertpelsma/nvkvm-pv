# Packaging / scripts / kata audit — 2026-08-29

Scope: `nvkvm-steamos` (compose, Dockerfiles, scripts, boot), `nvkvm-kata`
(whole repo), `nvkvm-pv` packaging, CI workflows.

**Verdict: 54 findings — 3 critical, 21 serious, 30 minor.**

All three criticals are the same shape: **trusted input from an untrusted
source**, not logic bugs. Each is a small local fix.

## Critical

**1. vast.ai API fields are `eval`'d as root on the coordinator.**
`nvkvm-pv/scripts/sweep.sh:892` builds `SSH="ssh … root@$host"` from
`public_ipaddr`/`ssh_host` straight out of the API response; `rsh_t` then runs
`eval "timeout $tmo $SSH $q"` (`:705`). A marketplace host controlling its
advertised fields, or a compromised account, gets arbitrary root command
execution **on the machine running the sweep**. The comment at `:686` identifies
the eval as dangerous and guards only the *empty* case.
This is precisely the "never execute shell commands built from vast responses"
rule the sweep is supposed to obey, violated by the current sweep itself.

**2. The test guest's SSH is published on 0.0.0.0 with hardcoded credentials.**
`nvkvm-pv/scripts/run_test_vm.sh:308` uses `hostfwd=tcp::2222-:22` with no bind
address; `scripts/setup_guest.sh:247,265,270` sets `ubuntu:ubuntu`,
`ssh_pwauth: true`, `NOPASSWD:ALL`. `sweep.sh:1358` does this on every rented
public-IP box for the life of a sweep. The compose path binds `127.0.0.1`; the
bare-host and rented-box paths do not.

**3. The Kata guest's `/sbin/modprobe` is fetched over plain HTTP, unverified.**
`nvkvm-kata/scripts/prepare-guest-rootfs.sh:81-101` — both `Packages.gz` and the
`.deb` come over `http://`, and the binary runs as guest root at
`create_sandbox` before any container starts. The Packages stanza already carries
a SHA256 the parser ignores.

## Serious (21) — themes

- **A stale comment describing the safe behaviour while the code does something
  else**, four times: an undefined `warn()` that turns the documented fallback
  into a `set -e` abort and a restart loop (`steamos-container-entrypoint.sh:136`);
  firmware deletion justified "a VM does not need this" with no check that it is
  a VM, on the live root too (`steamos_boot.sh:1022`); `backup_once` going inert
  on the second install cycle so "exact revert" restores a stale config; a rootfs
  stamp claiming "idempotent by construction" that caches past a repo update.
  These bite hardest because the comment stops anyone re-reading the code.
- **No lock exists anywhere in any of the three repos** — no `flock`, no
  lockfile, no `RefuseManualStart=`. Root of five separate race findings
  (concurrent converge sharing `/home/.nvkvm-build.img`; two image-editing
  scripts sharing hardcoded `/mnt/...`; recovery `do_plant` with no trap).
- **Supply chain**: TOFU checksum on the SteamOS image (verified against a hash
  we recorded from our own first download); Alpine checksum verification that
  fails open twice and never re-verifies a cache hit; QEMU cloned by mutable tag
  and "verified" by re-reading that same tag; Kata tarballs extracted to `/` with
  no verification; release images built from moving `main` then attested.
- **Privilege at the edges of an otherwise good model**: `chmod 0777` on a
  host `$HOME` directory; 9p `security_model=passthrough` in nvkvm-pv where
  nvkvm-steamos correctly uses `mapped-xattr` for the same hazard; a guest-planted
  SSH key that survives a full reinstall.
- **CI**: every GitHub Action pinned by mutable tag in a job holding
  `packages: write` + `id-token: write` — while nvkvm-steamos pins every action
  by SHA, so this is an inconsistency rather than an oversight.

## What the auditor rated as genuinely good

The two container-privilege models (cap_drop ALL, exact-device assertions,
per-socket bind mounts instead of `$XDG_RUNTIME_DIR`, enforced by
`tests/compose_policy_test.sh`) are better than typical for this class of
project. Every privilege finding is at an *edge* of that model — the
`override-dri.yml` escape hatch, the 9p share, the `chmod 0777` — not in it.

Where the two repos disagree, **nvkvm-steamos is right and nvkvm-pv should adopt
its decision** (9p `mapped-xattr`, SHA-pinned actions, `.dockerignore` as a
superset of `.gitignore` enforced by a test).
