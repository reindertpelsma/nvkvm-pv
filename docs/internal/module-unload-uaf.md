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

## What it means

`nvkvm_exit` calls `unregister_virtio_driver`, which drives
`nvkvm_virtio_remove` -> `nvkvm_drm_fini` -> `drm_dev_put`. That triggers
`drm_managed_release`, which walks the drmm resource list and calls
`drm_mode_config_cleanup`. By then some of those objects are gone.

`nvkvm_kms_fini` is careful in isolation -- it does `cancel_work_sync` on both
work items, `hrtimer_cancel` on both timers, then `destroy_workqueue`, in a
documented order. The defect is not inside it. It is the ORDERING between that
teardown and drmm's own release: something drmm still owns is freed before
drmm releases it, so drmm walks poison.

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
