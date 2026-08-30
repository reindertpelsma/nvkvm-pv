# NVKVM_REQ_RESET — verification

**Measured 2026-08-30**, RTX 3050 Laptop GPU, driver 580.173.02, guest Ubuntu
24.04 under nvkvm, tree `feat/handle-lifetime`.

## Why it exists

Handles outlive the session that created them by design: they are closed
explicitly, and `nvkvm_handle_close()` refuses with `-EBUSY` while an isolate
still holds one. That is correct while a guest kernel is alive to close them,
and it leaves nothing to reclaim across a module reload -- a force-unload, an
oops or a guest reboot strands isolates and memfds in the VMM that no later
guest request can name.

`nvkvm_handle_close_session()` used to be the implicit backstop. It is not a
security bound (a guest that wants to pin host memory simply never exits), but
it did make accidental leaks self-healing, and the explicit-close model removes
that. Reset is the replacement, at module-load granularity.

## Test: strand state, then reload

1. In the guest, hold a device fd open so the VMM has a live session+isolate:
   `bash -c 'exec 3</dev/nvidiactl; sleep 900' &`
2. Hard-reboot the guest so nothing unwinds:
   `echo b > /proc/sysrq-trigger`. QEMU survives (`run_test_vm.sh` passes no
   `-no-reboot`), so the VMM keeps its state.
3. The incoming module sends `NVKVM_REQ_RESET` from `nvkvm_init()`, after
   `register_virtio_driver()` (transport up) and before `register_devices()`
   (nothing openable yet, so no racing session is created and then reset).

### Result

    VMM:   nvkvm: RESET reclaimed 1 isolate(s), 1 session(s), 0 handle(s)
                  left by a previous guest module
    guest: unavailable_lines=0     (i.e. the request returned 0)

Both halves matter. An earlier run had the VMM reclaiming correctly while the
guest logged `reset unavailable (-22)` -- see below.

## A new request id has THREE registration points

`-Wswitch` on `nvkvm_tx_done_callback()` is deliberate and catches the second
one. It does not catch the third:

1. the `nvkvm_request_type` enum;
2. the completion switch in `nvkvm_tx_done_callback()` — enforced by
   `-Werror=switch`, which failed the build until the arm existed;
3. **`nvkvm_request_type_known()`'s range test** — a plain
   `type <= NVKVM_REQ_XISO_IMPORT` bound. A new id falls outside it, is
   rejected with `EINVAL` *before* the switch, and never reaches its arm.

The failure mode is misleading rather than loud: the VMM executes the request
and reclaims correctly, and the guest reports the feature unsupported. Keep the
bound on the last id.

## Not covered here

Unload safety (refusing while guest VMAs still point at the window) and the
handle/session decoupling itself. Reset is the reclaim backstop those depend
on, and is independently useful without them.
