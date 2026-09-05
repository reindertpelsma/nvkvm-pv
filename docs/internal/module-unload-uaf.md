# The guest module cannot be unloaded safely (drmm release ordering)

**Status: root-caused, not fixed.** Reproducer and full trace below.

## What happens

`rmmod nvkvm_guest` reports success while leaving freed, poisoned memory
behind. The corruption is discovered later by whatever allocates next, which
is why it looked like three unrelated bugs before it looked like one:

| where it surfaced | what was seen |
|---|---|
| SteamOS first boot | kernel panic in `ipv4_sysctl_init_net`, an innocent victim |
| cycle test, no poisoning | GPF at cycle 9-10 in `nvkvm_kms_init` via `drmm_kmalloc` |
| cycle test, `slub_debug=FZP` | the actual free, in our own teardown |

## The trace

Measured 2026-09-05 on bare metal (RTX 3090, driver 580.178.04), guest booted
with `slub_debug=FZP`, `tests/integration/module_cycle_test.sh`. It faults on
the FIRST unload once poisoning is on:

```
general protection fault, probably for non-canonical address 0x6b6b6b6b6b6b6b73
RIP: drm_mode_config_cleanup+0x58/0x300
RAX: 6b6b6b6b6b6b6b6b   RBX: 6b6b6b6b6b6b6b63   RCX: 6b6b6b6b6b6b6b6b

  drm_mode_config_cleanup
  drm_mode_config_init_release
  drm_managed_release
  drm_dev_put
  nvkvm_drm_fini            [nvkvm_guest]
  nvkvm_virtio_remove       [nvkvm_guest]
  virtio_dev_remove -> driver_detach -> unregister_virtio_driver
  nvkvm_exit                [nvkvm_guest]
  __x64_sys_delete_module
```

Three registers hold `0x6b6b6b6b6b6b6b6b` exactly -- SLUB's `POISON_FREE`
byte. This is a list walk over drmm-managed objects that have already been
freed and poisoned.

## Root cause: drmm releases LIFO, and init registers in the wrong order

`nvkvm_kms_init` (`src/guest/nvkvm_kms.c`) does this, in this order:

```c
ret = drmm_mode_config_init(ddev);            /* registers drm_mode_config_cleanup */
...
kms = drmm_kzalloc(ddev, sizeof(*kms), GFP_KERNEL);   /* registers the kms struct */
```

**drmm releases its actions LIFO.** So at `drm_dev_put` the order is reversed:
the `kms` allocation is freed and poisoned FIRST, and only then does
`drm_mode_config_cleanup` run.

And `struct nvkvm_kms` EMBEDS the mode objects:

```c
struct drm_connector            conn;
struct drm_simple_display_pipe  pipe;   /* contains crtc, plane, encoder */
```

`mode_config`'s connector/crtc/plane/encoder lists therefore point INTO the
struct that was just freed. `drm_mode_config_cleanup` walks those lists and
dereferences `0x6b6b6b6b6b6b6b6b`. That is the whole bug.

`nvkvm_kms_fini` is not at fault and reading it will not find this. It cancels
both work items, both timers and the workqueue in a correct, documented order.
The defect is in `nvkvm_kms_init`, hundreds of lines away from where it
detonates, and it is invisible until unload.

## The candidate fix (proposed, NOT applied)

Allocate `kms` BEFORE `drmm_mode_config_init`, so drmm frees it AFTER
`drm_mode_config_cleanup` has finished walking the lists.

Check before applying:
  - does anything between the two calls need `mode_config` already
    initialised? (`nvkvm_kms_clamp_mode()` and the `mode_config.*` assignments
    do -- they must stay after `drmm_mode_config_init`, only the ALLOCATION
    needs to move earlier)
  - `drm_vblank_init` sits between them; confirm it does not depend on `kms`
  - verify with `module_cycle_test.sh` under `slub_debug=FZP`: it faults on
    the FIRST unload today, so a fix is confirmed or refuted in ~40 seconds

## Why the reboot workaround is not the fix

`nvkvm-steamos` has a branch (`fix/no-module-hotswap`) that reboots instead of
hot-swapping. That removes the trigger from the first-boot path, and it is
worth having as a stopgap, but it hides a real double-free. The contract is
that a module unloads cleanly or refuses to unload -- an honest `EWOULDBLOCK`
beats a successful lie. Both `file_operations` already set
`.owner = THIS_MODULE`, so an open fd correctly pins the module; that half is
right. This is the other half.

## Reproducing

```sh
# in the guest, cloud images need grub.d -- /etc/default/grub is overridden
# by /etc/default/grub.d/50-cloudimg-settings.cfg and your edit is discarded
echo 'GRUB_CMDLINE_LINUX_DEFAULT="console=tty1 console=ttyS0 slub_debug=FZP"' \
  | sudo tee /etc/default/grub.d/99-slub.cfg
sudo update-grub && sudo poweroff        # COLD boot; an in-guest reboot hung
# then, with a .ko that persists (the boot service builds in a mktemp dir and
# deletes it, so build your own):
cp -a /mnt/nvkvm/src/{guest,common,abi} ~/src/
make -C ~/src/guest KDIR=/lib/modules/$(uname -r)/build
sudo NVKVM_KO=~/src/guest/nvkvm-guest.ko bash module_cycle_test.sh 40
```

Without poisoning it takes 9-10 cycles and blames an innocent allocation.
With poisoning it faults on the first unload, in the right place.
