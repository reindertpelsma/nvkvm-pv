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

# Is `close_session` load-bearing? Measured: no

Removing `nvkvm_handle_close_session()` in favour of explicit close is safe
only if the guest already reclaims its handles. `nvkvm_session_put()` does
*not* sweep them -- it stops the pump, unmaps the ring, kills the isolate, and
comments "QEMU frees the GPA + memfd on isolate kill" -- which reads as though
the guest depends on the VMM for reclaim. It does not.

Instrumented `close_session()` to report what it actually force-closes, and ran
a CUDA workload that terminates **abnormally** (the probe segfaults at P3,
`rc=139`), so the ungraceful path is the one covered:

    nvkvm DIAG: close_session(1) force-closed 0 memfd + 0 nvidia handle(s),
                0 still isolate-referenced          (x2)

It runs, and finds nothing, every time. The guest's paired
`close_handle_on_isolate` / `close_handle` calls have already reclaimed
everything by the time the session is destroyed, because fd release runs during
process teardown however the process died.

**So `close_session` is a backstop that never fires in practice**, not the
reclaim path. Dropping it costs nothing in the steady state, and `RESET` covers
the case it was really guarding: state stranded when the guest module itself
goes away without unwinding.

### Print unconditionally when the question is "did this run?"

The first version printed only non-zero counts, so zero lines were ambiguous
between "found nothing" and "never called" -- the exact distinction being
measured. Worth remembering: a diagnostic that is silent on the common case
cannot answer a reachability question.

### Not established

One workload shape, two events. A process killed while holding many handles
mid-registration was not tested, and neither was a guest kernel that fails
during release. The claim is "does not fire for a normally-structured workload
that dies abnormally", not "can never fire".

### Temporary

The `DIAG(temp)` commits on this branch instrument `close_session` and must be
removed before it merges anywhere.
