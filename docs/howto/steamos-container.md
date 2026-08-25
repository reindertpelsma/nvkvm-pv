# SteamOS in the container, display broker on the host

This path keeps the display connection and keyboard grab in the logged-in host
session. The container receives no Wayland/X11 socket and no host DRM node for
display; its only desktop-facing interface is a unix socket to the broker.

The `steamos` Compose service is opt-in. On its first run it verifies its named
volumes, downloads a pinned Valve recovery image, records and checks its
SHA-256, boots a disposable Alpine VM, and lets Valve's own installer produce a
real dual-slot A/B qcow2. It then provisions nvkvm and boots that qcow2. Both the
installer VM and SteamOS use `/dev/kvm`.

## 1. Start the broker in the desktop session

Install its small host build dependencies once:

```bash
sudo apt install build-essential pkg-config libwayland-dev wayland-protocols
```

Create a host directory the logged-in user can manage, then run the persistent
broker as that user:

```bash
sudo install -d -m 0770 -o "$USER" -g "$(id -gn)" /run/nvkvm
scripts/run-display-broker.sh
```

The helper builds the broker if necessary and creates
`/run/nvkvm/steamos.sock` as mode `0660`. `--persist` keeps the window alive
across VM restarts. `CTRL+ALT+G` is the grab toggle and focus loss releases a
grab automatically; do not take a grab until the broker's startup line says it
can observe focus loss.

The broker is deliberately not a Compose service. Containerising the process
which must own the host display connection adds packaging, not a security
boundary. It must be launched with the logged-in user's `XDG_RUNTIME_DIR` and
`WAYLAND_DISPLAY` when started from SSH or a system service.

## 2. Start SteamOS

The container needs the numeric GID of `/dev/kvm` and of the group owning the
broker socket. Resolve them from the host rather than assuming distro-specific
group numbers:

```bash
export NVKVM_KVM_GID="$(stat -c %g /dev/kvm)"
export NVKVM_BROKER_GID="$(id -g)"
docker compose --profile steamos up --build steamos
```

Do not add a socket wait loop. QEMU reconnects with bounded backoff, re-sends
the guest geometry, and re-attaches its last frame. The broker may start before
or after the container, and it may restart without taking down SteamOS. The
Compose file bind-mounts `/run/nvkvm` as a directory—never the socket inode—so a
new socket created by a restarted broker is visible in the container.

The defaults are deliberately reproducible:

- recovery: `steamdeck-oobe-repair-20260707.10-3.8.14.img.bz2`;
- install target: sparse 64 GiB (the unused part is the games budget);
- setup stages: `repair,provision`;
- guest: 12 GiB RAM, 8 vCPUs;
- desktop session: Plasma/KWin; Steam launches gamescope for games rather than
  using gamescope as the SDDM login session;
- display socket: `/run/nvkvm/steamos.sock`;
- SSH: host loopback port 15022 to the guest's port 22.

Set `NVKVM_STEAMOS_LATEST=1` to opt into scraping Valve's recovery index. A
download is never silently replaced after its checksum was recorded; a mismatch
is a hard error. An existing qcow2 is also never rebuilt automatically.

For a broker-free diagnostic run, set `NVKVM_STEAMOS_QEMU_DISPLAY=none` before
creating the service. QEMU then opens no host display connection and no broker
socket; the guest GPU and loopback SSH forwarding remain available for probes.

Durable data is split across two volumes:

| volume | contents | visible to guest |
|---|---|---|
| `nvkvm-steamos` | recovery, qcow2, OVMF vars, install/serial logs, private SSH key | no |
| `nvkvm-steamos-data` | public SSH keys and ordinary shared files | yes, at `~/data` |

The guest boot script treats mount tag `data` as optional. A hand-written QEMU
command without it still reaches the desktop; it simply has no `~/data`, and
key-triggered sshd remains disabled.

## 3. Reach and inspect the guest

The entrypoint generates an Ed25519 key once. Only its public half is placed on
the 9p data share; the private half stays in the state volume. The image remains
generic and rotating `authorized_keys` does not require rebuilding it.

```bash
docker compose exec steamos nvkvm-steamos-ssh
```

The helper connects as `root` by default. The same container-managed public key
is also installed for SteamOS's interactive `deck` account, with a validated,
key-gated `NOPASSWD` sudo policy. Select that account explicitly when wanted:

```bash
docker compose exec -e NVKVM_STEAMOS_SSH_USER=deck steamos \
  nvkvm-steamos-ssh -- sudo -n id
```

If `/data/authorized_keys` was supplied externally, the container does not
invent or overwrite a matching private key. Use ordinary host SSH with that
identity instead:

```bash
ssh -i /path/to/key -p 15022 root@127.0.0.1
```

Treat the first `repair,provision` run as bring-up. A line from
`steamos_boot.sh` saying nvkvm validation passed establishes module, device and
userspace bring-up only; it runs before the display manager and cannot establish
that Plasma exists. Corroborate it inside the guest:

```bash
docker compose exec steamos nvkvm-steamos-ssh -- \
  sh -lc 'dmesg | grep -i nvkvm | tail -80; pgrep -a "kwin_wayland|plasmashell|gamescope"; nvidia-smi'
```

Then check the host broker log for an accepted frame and its presentation mode.
That host log plus a live compositor process is stronger evidence than either
one alone.

## Debugging the installer

The disposable VM has an interactive repair shell. It reuses an existing target
qcow2 and does not overwrite it:

```bash
docker compose --profile steamos run --rm steamos --setup-shell
```

The serial transcripts are in the `nvkvm-steamos` volume as `install.log` and
`serial.log`. The generated image is a sparse file but the Valve recovery and
its decompressed form consume real space.

The Compose capability set is intentional: everything is dropped, then
`SETUID`, `SETGID`, `SETPCAP`, and `SYS_CHROOT` are restored solely so isolates
can reach their uid+chroot sandbox rung. Removing those four makes isolation
weaker by silently falling back to seccomp-only. `/dev/kvm` is a device/group
grant, not a capability.
