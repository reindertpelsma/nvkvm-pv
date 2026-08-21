# Known issue: the NVKMS allowlist compares raw enum indices that shift between driver branches

**Status:** open. Not fixed for the current release — the fix is ABI-profile
plumbing, not an edit.
**Component:** `src/qemu/nvkvm_nvkms_allowlist.h`
**Found:** pre-release audit, 2026-08-21.
**Severity:** medium as shipped, high on an untested driver branch. The gate can
silently admit a command it was written to deny.

---

## The defect

`nvkvm_nvkms_cmd_allowed()` is a `switch` over bare integers:

```c
case 0:   /* NVKMS_IOCTL_ALLOC_DEVICE      */
case 1:   /* NVKMS_IOCTL_FREE_DEVICE       */
case 17:  /* NVKMS_IOCTL_REGISTER_SURFACE  */
case 18:  /* NVKMS_IOCTL_UNREGISTER_SURFACE*/
case 60:  /* per-surface, 595+ ICD                    */
case 61:  /* query-class (captured)        */
case 62:  /* query-class (captured)        */
```

Those integers are positions in `enum NvKmsIoctlCommand`
(`src/nvidia-modeset/interface/nvkms-api.h`), which is a plain unvalued enum —
position *is* the wire value. The header asserts the numbers are safe across
branches:

> Re-read the enum at the matching tag before trusting these on a new branch: it
> grows by APPENDING, so old values are stable […]

**That is not true.** The enum has been edited in the middle at least twice, and
each edit renumbers every command above the edit point. The gate has no idea
which driver it is talking to, so the same `case 18:` means different commands on
different hosts.

## Measured: the vendor-tag table

Enum members extracted from `enum NvKmsIoctlCommand` in each tag's
`src/nvidia-modeset/interface/nvkms-api.h`, compared positionally.

| transition | edit | effect |
|---|---|---|
| 515.105.01 → 575.51.03 | **insert** `CHECK_LUT_NOTIFIER` at index 13 | every command ≥ 13 shifts **+1** |
| 515.105.01 → 575.51.03 | append 7 members at 59–65 | (benign, this is the "appending" the header assumed) |
| 575.51.03 → 580.159.04 | *none* | identical enums |
| 580.159.04 → 610.43.02 | **delete** `EXPORT_VRR_SEMAPHORE_SURFACE` at 56 | every command > 56 shifts **−1** |
| 580.159.04 → 610.43.02 | **delete** `VRR_SIGNAL_SEMAPHORE` at 64 | every command > 64 shifts **−1** again |
| 580.159.04 → 610.43.02 | append `REGISTER_VBLANK_INTR_CALLBACK`, `UNREGISTER_VBLANK_INTR_CALLBACK` | — |

What nvkvm's seven allowed values actually mean, per tag:

| value | 515.105.01 | 575.51.03 | 580.159.04 | 610.43.02 |
|---|---|---|---|---|
| 0  | ALLOC_DEVICE | ALLOC_DEVICE | ALLOC_DEVICE | ALLOC_DEVICE |
| 1  | FREE_DEVICE | FREE_DEVICE | FREE_DEVICE | FREE_DEVICE |
| 17 | UNREGISTER_SURFACE | REGISTER_SURFACE | REGISTER_SURFACE | REGISTER_SURFACE |
| 18 | **GRANT_SURFACE** | UNREGISTER_SURFACE | UNREGISTER_SURFACE | UNREGISTER_SURFACE |
| 60 | *out of range* | SET_FLIPLOCK_GROUP | SET_FLIPLOCK_GROUP | **ENABLE_VBLANK_SEM_CONTROL** |
| 61 | *out of range* | ENABLE_VBLANK_SEM_CONTROL | ENABLE_VBLANK_SEM_CONTROL | **DISABLE_VBLANK_SEM_CONTROL** |
| 62 | *out of range* | DISABLE_VBLANK_SEM_CONTROL | DISABLE_VBLANK_SEM_CONTROL | **ACCEL_VBLANK_SEM_CONTROLS** |

Two consequences worth stating separately.

**1. On a 515/520-era host the gate admits `GRANT_SURFACE`.** The header's own
opening paragraph names the cross-client sharing verbs — "GRANT/ACQUIRE/RELEASE_
SURFACE" — as precisely what must not be reachable, and then the list admits one
of them, because value 18 predates the `CHECK_LUT_NOTIFIER` insertion. (The same
shift makes value 17 `UNREGISTER_SURFACE` and leaves `REGISTER_SURFACE`, at 16,
denied — so on that branch the guest cannot register a surface but can grant one.
That is an incoherent gate, not a working one.) nvkvm's own ABI-profile table
carries a `NVKVM_ABI_515` row, so 515/520 is a configuration this tree claims to
support.

**2. The header names the wrong command for the one cmdType it investigated.**
The 2026-08-17/2026-08-21 notes describe cmdType 60 measured *on 610.43.02* — an
instrumented build, the ICD issuing it once per offscreen context between
`REGISTER_SURFACE` and `UNREGISTER_SURFACE`, 32-byte params — and then name it
`SET_FLIPLOCK_GROUP`, read off the **575.51.03** tag. On 610 value 60 is
`ENABLE_VBLANK_SEM_CONTROL`. The correction also makes the observation *more*
coherent, not less: a vblank semaphore control bracketing a just-registered
surface is exactly the 0/17/60/18 sequence that was logged, and it explains why
"61 and 62 are NOT issued at all by the 610 ICD" — on 610 those two are the
*disable* and *accel* halves of the same family, not the pair 575 uses.

## Why this is not fixed now

The fix is not a renumbering; any fixed table is wrong on some branch. It is the
same plumbing the UVM gate already has: `struct nvkvm_abi_profile`
(`src/common/nvkvm_abi.h`) is selected from the host driver version at probe,
travels to the stub as `abi_profile` on the wire
(`nvkvm_isolate_proto.h`, `nvkvm_ring_ioctl.h`), and carries per-version sizes
and offsets so no code compares a raw constant against a version-dependent
value. NVKMS needs the equivalent: a per-profile *name → cmdType* mapping, with
the allowlist expressed in names.

That means, roughly:

1. Add a per-profile NVKMS command-number table to `struct nvkvm_abi_profile`
   (or a parallel table keyed by the same `enum nvkvm_abi_id`), covering at
   minimum the allowed set plus the deny-critical verbs
   (GRANT/ACQUIRE/RELEASE_SURFACE, GRANT/ACQUIRE/REVOKE_PERMISSIONS,
   the swap-group family) so the *denials* are also version-correct.
2. Derive it mechanically per tag, the way `tools/abi_derive.sh` derives the
   existing rows — the enum is machine-readable, so this should not be a hand
   transcription.
3. Rewrite `nvkvm_nvkms_cmd_allowed()` to take the profile and compare names.
4. Fail closed on an unknown driver version rather than falling through to a
   default row: for NVKMS a wrong row is an allowlist bypass, which is a
   stronger reason to fail closed than the UVM sizes had.

Until then the gate is only trustworthy on 575/580, which is where it was
captured and measured.

## Interim mitigations

- The allowlist is an interim measure regardless; the intended end state is to
  stop forwarding NVKMS at all and emulate a virtual head
  (`docs/design/virtual_modeset.md`).
- NVKMS forwarding is reached only on the graphics path (`nv->graphics`).
  Deployments that do not need in-guest display should leave it off.
- `NVKVM_NVKMS_EXTRA_ALLOW` widens the list for a run and is an investigation
  hatch only; it must not be set in a deployment facing an untrusted guest.

## Reproducing the table

Extract `enum NvKmsIoctlCommand` from
`src/nvidia-modeset/interface/nvkms-api.h` at each open-gpu-kernel-modules tag
and compare the member lists positionally. The enum is unvalued throughout every
tag checked (no `= N` on any member), so index equals wire value and a plain
positional diff is sufficient.
