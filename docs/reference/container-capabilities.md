# What the NVIDIA container capabilities actually gate

Scope: the `NVIDIA_DRIVER_CAPABILITIES` list — `capabilities: [gpu, utility,
compute, graphics, display, video]` in [`docker-compose.yml`](../../docker-compose.yml),
or `--gpus 'all,capabilities=...'`.

Verified 2026-08-22 by reading [libnvidia-container](https://github.com/NVIDIA/libnvidia-container)
and [nvidia-container-toolkit](https://github.com/NVIDIA/nvidia-container-toolkit)
at `main`. Line numbers are from those trees.

## Summary

A capability name is parsed into `OPT_*` flags by one table
(`src/options.h:88-94`). Two entries do not mean what they are called:

```c
{"video",   OPT_VIDEO_LIBS|OPT_COMPUTE_LIBS},   /* video implies compute   */
{"display", OPT_DISPLAY|OPT_GRAPHICS_LIBS},     /* display implies graphics */
```

Those flags are then tested in exactly **14 places** across all 33 source files.
Every one falls into four groups, and only two of them are a security boundary:

| what the flag does | sites | boundary? |
|---|---|---|
| pick which host libraries/binaries get bind-mounted | `nvc_info.c:769-791`, `nvc_container.c:69` | **no** |
| write an EGL application profile inside the container | `nvc_mount.c:607,720` | **no** |
| pick which IPC sockets get bind-mounted | `nvc_mount.c:770,772` | yes |
| gate two device nodes | `nvc_mount.c:783,786` | yes |

## Libraries and userspace config files are not a security boundary

**State this plainly, because the capability list invites the opposite reading:**
bind-mounting a library into a container grants no access the container did not
already have.

A driver library is *data*. It works by issuing ioctls on device nodes. If the
node is not granted, the library fails; if the node is granted, the container can
issue precisely the same ioctls with or without the library — by hand, statically
linked, from any language. Removing `libnvidia-gl` does not remove any reachable
operation; it removes the convenience of not having to write the ioctl yourself.

The same holds for the EGL application profile that `graphics` triggers
(`update_app_profile`, `nvc_mount.c:305-345`). It writes:

```json
{"profiles":[{"name":"_container_","settings":["EGLVisibleDGPUDevices", 0x..]}],
 "rules":[{"pattern":[],"profile":"_container_"}]}
```

to `/etc/nvidia/nvidia-application-profiles-rc.d/10-container.conf` — resolved
under `cnt->cfg.rootfs`, i.e. **inside the container**, onto a tmpfs that
`mount_app_profile()` mounts there first (`mode=0555`,
`MS_NODEV|MS_NOSUID|MS_NOEXEC`). It is a bitmask hint telling the EGL loader
which dGPUs to enumerate. Nothing on the host is written, and nothing enforces
it — cooperative userspace reads it or ignores it.

So `graphics`, `video`, `ngx`, `compat32`, and the library half of
`utility`/`compute` are packaging conveniences. Adding them widens what *works*.
They do not widen what is *reachable*.

The boundary is the set of device nodes and sockets. That is the whole list.

## Device nodes

The only capability test on devices is two lines (`nvc_mount.c:783-788`):

```c
/* XXX Only compute libraries require specific devices (e.g. UVM). */
if (!(cnt->flags & OPT_COMPUTE_LIBS) && major(info->devs[i].id) != NV_DEVICE_MAJOR)
        continue;
/* XXX Only display capability requires the modeset device. */
if (!(cnt->flags & OPT_DISPLAY) && minor(info->devs[i].id) == NV_MODESET_DEVICE_MINOR)
        continue;
```

With `NV_DEVICE_MAJOR 195`, `NV_CTL_DEVICE_MINOR 255`,
`NV_MODESET_DEVICE_MINOR 254` (`nvc_internal.h:30-32`) that resolves to:

| node | major:minor | capability required |
|---|---|---|
| `/dev/nvidiactl` | 195:255 | **none — always granted** |
| `/dev/nvidia0` … `/dev/nvidiaN` | 195:0…N | **none — always granted** |
| `/dev/nvidia-modeset` | 195:254 | `display` |
| `/dev/nvidia-uvm`, `/dev/nvidia-uvm-tools` | dynamic major | `compute` |
| `/dev/dri/card*`, `/dev/dri/renderD*` | 226 | `graphics` or `display` — and see below, a different component does this |

### What is granted by default

`/dev/nvidiactl` and every `/dev/nvidiaN` are granted with **no capability at
all**. Two independent reasons:

1. Per-GPU nodes go through `device_mount_native()` (`nvc_mount.c:595-616`),
   which contains no capability test of any kind.
2. In the driver-level loop above, the first `continue` only skips devices whose
   major is *not* 195 — so everything at major 195 except minor 254 passes
   unconditionally.

**This is the single most important fact on this page.** The capability list
looks like it gates GPU access. It does not. The full RM ioctl surface —
`/dev/nvidiactl` plus every GPU node — is present in a container that requests
no capabilities whatsoever. What the capabilities gate is UVM, modeset, and DRM.

### How a node is actually gated

Two independent mechanisms, and a node needs **both**:

1. **bind-mount** — makes the node visible in the container's `/dev`
2. **device cgroup rule** — makes it openable

A visible node with no cgroup rule fails `open()` with `EPERM`. The rule is
semantically identical on both cgroup versions:

- **v1** (`cgroup_legacy.c:77-84`) appends `c <major>:<minor> rw` to
  `<container cgroup>/devices.allow`.
- **v2** (`cgroup.c:189-193`) builds
  `{.allow = true, .type = "c", .access = "rw", .major, .minor}`, ships it over
  RPC to the `nvcgo` helper, which compiles it to a `BPF_CGROUP_DEVICE` program,
  prepends it to the existing filters and attaches with `BPF_F_ALLOW_MULTI`.

The access string is `rw`, **never `m`**. No mknod. That matters for two
separate reasons: the container cannot create a node for a device it was not
granted, so the cgroup rule list is the complete set of reachable devices — not
merely the set of pre-created ones.

**Escape hatch.** Every cgroup call is wrapped in
`if (!(cnt->flags & OPT_NO_CGROUPS))`, and `options.h`'s
`default_container_opts` is `"standalone no-cgroups no-devbind utility"` — the
library's own default writes no rules at all. The runtime hook passes
`--no-cgroups` only when `nvidia-container-cli.no-cgroups` is set in
`/etc/nvidia-container-runtime/config.toml` (`main.go:121`). When it is set,
device access is whatever the OCI runtime put in the spec, and
libnvidia-container is a pure bind-mount tool. Common on rootless podman.

## `/dev/dri`

**libnvidia-container has zero references to DRM** — not `dri`, not `card`, not
`renderD`. The whole `/dev/dri` question lives in a different component.

Injection is done by nvidia-container-toolkit's OCI spec modifier / CDI:

- `internal/modifier/graphics.go:71` — required capability is `graphics` **or**
  `display`:
  ```go
  if !cudaImage.GetDriverCapabilities().Any(image.DriverCapabilityGraphics,
                                            image.DriverCapabilityDisplay) {
          return nil, "no required capabilities requested"
  }
  ```
- `internal/discover/graphics.go:397-420` — globs `/dev/dri/card*` and
  `/dev/dri/renderD*`, then applies `newDRMDeviceFilter` so only the DRM nodes
  belonging to the *requested* GPUs are injected. A non-NVIDIA iGPU's `card0` is
  not injected.

**Consequence worth knowing:** with the legacy prestart-hook path alone,
`/dev/dri` is never injected regardless of what capabilities you ask for,
because that path only runs libnvidia-container. If graphics breaks and the
capability list looks correct, check the runtime mode
(`nvidia-ctk runtime configure`) before touching capabilities.

## IPC sockets

Three candidates (`lookup_ipcs`, `nvc_info.c:577-601`). Missing ones are dropped
at discovery — `find_path()` logs `missing ipc path` and returns 0, then
`array_pack()` compacts the NULL out — so nothing is mounted for a daemon that
is not running.

| path | capability |
|---|---|
| `/var/run/nvidia-persistenced/socket` | `utility` |
| `/var/run/nvidia-fabricmanager/socket` | `utility` |
| `/tmp/nvidia-mps` | `compute` |

Bind-mounted `MS_NODEV|MS_NOSUID|MS_NOEXEC` but **not** `MS_RDONLY` —
`connect()` on a unix socket requires write permission (`mount_ipc`,
`nvc_mount.c:250-275`).

### nvidia-persistenced

A root daemon that holds a handle open on each GPU so driver state survives
last-close; without it the next CUDA init pays a multi-second re-init and loses
clock/ECC configuration. The modern replacement for `nvidia-smi -pm`.

The socket is an ONC/Sun RPC (XDR) endpoint, program **35006**
([source](https://github.com/NVIDIA/nvidia-persistenced), `nvpd_rpc.h:84-121`):

| version | procedure |
|---|---|
| 1 | `SetPersistenceMode`, `GetPersistenceMode` |
| 2 | `SetPersistenceModeOnly`, `SetNumaStatus` |

All three **setters** call `_nvpdIsClientRoot()` (`command_server.c:37-53`):

```c
getsockopt(req->rq_xprt->xp_sock, SOL_SOCKET, SO_PEERCRED, &ucred, &ucred_len);
if (ucred.uid != 0) return NVPD_ERR_PERMISSIONS;
```

`SO_PEERCRED` is kernel-supplied and translated into the *reader's* user
namespace, so:

- container running as root with no userns remap → host uid 0 → **passes**
- rootless, or Docker `userns-remap` → non-zero host uid → denied

The getter has no check. Impact if it passes is host-global per-GPU state:
persistence mode, and `SetNumaStatus` (onlines/offlines GPU memory NUMA nodes on
coherent-memory platforms). Nuisance and DoS, not code execution.

The sharper issue is that the rpcgen dispatcher calls `svc_getargs()` — the XDR
decode — *before* invoking the handler (`nvpd_rpc_server.c:54-59`), so the
decode path is reachable **pre-authorization** by any uid that can connect, in a
process running as root.

### nvidia-fabricmanager

Manages the NVSwitch fabric on NVLink multi-GPU systems (DGX/HGX): trains
NVLink connections, programs routing across switches, handles fabric
partitioning and error recovery. On consumer cards there is no NVSwitch, the
daemon is not installed, the socket does not exist, and nothing is mounted.

**fabricmanager is closed source.** We make no claim about its authorization
model — unlike persistenced, it cannot be checked.

## What /dev/nvidia-modeset exposes

Relevant because `display` is the capability that grants it.

The node is mode **0666** — `nvidia-modprobe`'s `NV_DEVICE_FILE_MODE` is
`S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH`
(`modprobe-utils/nvidia-modprobe-utils.h:81`), overridable via the
`DeviceFileMode` driver parameter. Any local user can open it.

The nvidia-modeset kernel interface contains **no `capable()`, no
`CAP_SYS_ADMIN`, and no uid check**
(`kernel-open/nvidia-modeset/nvidia-modeset-linux.c`).

Authorization inside NVKMS is **ownership, not privilege**
(`src/nvidia-modeset/src/nvkms.c:956-981`):

```c
static NvBool GrabModesetOwnership(struct NvKmsPerOpenDev *pOpenDev)
{
    if (pDevEvo->modesetOwner == pOpenDev) return TRUE;
    if (pDevEvo->modesetOwner != NULL)     return FALSE;
    ...
    pDevEvo->modesetOwner = pOpenDev;
    AssignFullNvKmsPermissions(pOpenDev);
    return TRUE;
}
```

First-come-first-served, with no capability test anywhere in the path.
`NVKMS_IOCTL_GRAB_OWNERSHIP` → `GrabOwnership()` → the above.

So the answer to "what can an unprivileged user do on `/dev/nvidia-modeset`"
does not depend on privilege at all — it depends on whether the modeset
ownership slot is already taken:

- **Ownership already held** (X with the NVIDIA driver, or `nvidia-drm
  modeset=1`, typical on any desktop): `GrabModesetOwnership` returns FALSE. An
  unprivileged client cannot take over the display.
- **Nothing holds ownership** (a headless host, `nvidia-drm modeset=0`): an
  unprivileged client that can open the node can grab ownership and then drive
  the display engine with full NVKMS permissions.

This is why nvkvm allowlists NVKMS by command number rather than passing the
node through — see [`allowlists.md`](allowlists.md) and
`src/qemu/nvkvm_nvkms_allowlist.h`.

## What this means for nvkvm

`compute` and `display` are load-bearing: we need UVM, and NVKMS is on the
ordinary Vulkan/EGL path (the allowed command set in
`src/qemu/nvkvm_nvkms_allowlist.h` was captured from a live
`vulkaninfo`/device-create session, not from a scanout path). `graphics`,
`video`, and `utility` select libraries.

`utility` cannot be dropped: `make_host_bundle.sh` runs inside the container and
needs `libnvidia-ml` and `nvidia-smi` to build the guest bundle, and those ride
the same flag as the persistenced and fabricmanager sockets. No capability
setting separates them.

**The persistenced socket therefore reaches our container whenever the host runs
the daemon, and our container runs as root in the host user namespace.** It
cannot be masked from `docker-compose.yml`: the prestart hook mounts the socket
*after* runc has set up the rootfs, so a `tmpfs` at that path is simply the
surface the hook creates the socket on, and a read-only mount makes
`file_create()` fail and the container refuse to start. `umount` from the
entrypoint is not available either — we drop `CAP_SYS_ADMIN`.

What does work, in order of preference:

1. **Do not run `nvidia-persistenced` on the host.** It is not enabled by
   default on most distributions, and nvkvm never needs it. If the socket does
   not exist, `find_path()` drops it and nothing is mounted.
2. **Run Docker with `userns-remap`.** The container's root maps to a non-zero
   host uid, `_nvpdIsClientRoot()` denies every setter, and the pre-auth XDR
   surface is all that remains.
3. Accept it. The reachable operations are persistence mode and NUMA status,
   both host-global nuisances rather than escapes.
