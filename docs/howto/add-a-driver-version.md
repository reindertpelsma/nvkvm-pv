# Add a driver version

`nvkvm` keys its version-variant struct layouts off the host driver's version,
in an eight-row table at `src/common/nvkvm_abi.h:113-235`. See
[ABI profiles](../reference/abi-profiles.md) for what the rows are and how they
reach all three components.

Adding support for a new driver branch is at most three things: measure a
profile row, check whether the branch issues control commands the allowlist has
never seen, and check whether any struct in `src/abi/` changed shape.

## 0. First, just try it

You may not need a new row at all. `nvkvm_abi_id_for_version()`
(`src/common/nvkvm_abi.h:311-381`) maps ranges, not points, so a 572 driver
already selects the 570 profile and a 585 driver already selects 580.

Note that it takes `major`, `minor` **and** `patch`, because two measured
boundaries fall inside a branch (535 at the Confidential Computing channel
fields, 550 at the V550 UVM array). If you are adding a point release inside an
existing branch, that is exactly the case to check rather than assume.

Start the VM and read QEMU's realize line:

```
nvkvm: host driver 575.51.03 → ABI profile 570
```

(`src/qemu/virtio_nvgpu.c:1177-1178`.) If `cuInit`, `cuCtxCreate`, an 8 MiB
round trip and a kernel launch all pass, the existing row fits. Report it —
boots on unexercised branches are the most useful contribution here.

If something fails, work through the diagnosis below before assuming the table
is wrong.

## 1. Measure, do not derive

```bash
tools/abi_derive.sh --tags "585.xx.yy"          # just the new tag
tools/abi_derive.sh                             # the whole matrix, re-derived
tools/abi_derive.sh --all-published-supported   # every official 515..610 tag
```

The script blobless-sparse-clones NVIDIA open-gpu-kernel-modules at each tag and
compiles one probe **per profile field**, printing `sizeof`/`offsetof` for each.
Paste what it prints. A cell it could not compile prints `MISSING` and keeps the
compiler error in the work dir — that is a gap to report, never a cell to fill
in from a neighbouring branch.

The output also measures the size of `UVM_REGISTER_GPU_PARAMS` and the offsets
of `rmCtrlFd` and `rmStatus`. Those are universal wire constants rather than
profile members today, but they still have to be checked over the complete
supported range. Use `--all-published-supported` for ABI work that touches
them: as of 610.57.04, all 216 official numeric tags from 515.43.04 onward
compile as `40 / 24 / 36`, so there is one interval and no version gate.

**Never hand-derive a row.** The script's header records why
(`tools/abi_derive.sh:5-12`):

> Those numbers were originally DERIVED by arithmetic ("V550 grew the array
> +9180 bytes, so 535 must be 9264-9180 = 84") rather than measured. Three of
> the five 535-row values were wrong -- `uvm_map_ext_size` is really 1200, not
> 84 -- and the error is SILENT: a wrong size does not fail to compile, it
> forwards a truncated struct and the kernel reads past the buffer.

Use the tag that matches the branch you are adding, not a nearby one. The
existing rows were measured across every OGKM branch from 515 to 610, with an
early and a late tag inside each — `tools/abi_derive.sh --reference-check`
re-derives the five long-cited rows so you can confirm the method reproduces
them before trusting anything new.

## 2. Decide whether it is a new row or an existing one

Compare the measured nine values against the existing rows. If they are
byte-identical to a row that already exists, only the *range mapping* needs
changing.

- **Identical to an existing row** → extend that row's range in
  `nvkvm_abi_id_for_version()` (`src/common/nvkvm_abi.h:311-381`), add the tag
  to that row's measured-tag comment, and update the range comment. Nothing else
  moves. This is what happened with 575 (shares 570's layouts) and with 590/595
  (both measured byte-identical to 580).
- **Different** → add a row.

## 3. Add the row

Three edits in `src/common/nvkvm_abi.h`:

1. A new `enum nvkvm_abi_id` value (`:75-83`), with a one-line comment naming
   *what changed* — the existing comments are the fastest way to understand the
   table ("V580 VASPACE + V580 NVOS46 (each +8 bytes)"). Keep the convention
   that the id is the major version at which the layout first appears; the id
   goes on the wire, so do not renumber an existing one.
2. A new entry in `nvkvm_abi_profiles[]` (`:113-235`) with the measured values,
   and a comment listing **the tags whose probes produced it**. Annotate any
   value that differs from its predecessor with the struct name that grew, as
   the 535, 570, 580 and 610 rows do.
3. The range in `nvkvm_abi_id_for_version()` (`:311-381`).

Check `nvkvm_abi_by_id()`'s default (`:279-303`) still says what you want. It
looks the fallback up by id rather than array index, specifically so adding a
row in the middle cannot silently change it — that bug has been shipped once
(`src/common/nvkvm_abi.h:281-291`).

## 4. Update the parity test

`tests/abi_parity` asserts the compiled-in table against measured values. Run
it:

```bash
cd tests/abi_parity && go test -count=1 -v ./...
```

Add your measured tag to the `measured` table in `abi_profile_test.go`, which
replays the whole matrix through `nvkvm_abi_for_version()` and checks all nine
fields. If your tag sits on a boundary, add it to `boundaries` too, so a future
selector that collapses the two rows fails loudly.

**Pass `-count=1`.** Go's build cache does track the cgo-included header, but the
*test-result* cache is keyed on the test binary, so a plain `go test` after
editing `nvkvm_abi.h` can replay a stale pass. This was observed: breaking the
610 bucket and re-running `go test` reported `ok`; `-count=1` failed correctly.

Note what this test does and does not do: it is a pure compile-time-constant
check against measured expectations. It does not talk to a driver, a GPU or a
VM. It catches a table that disagrees with the C structs and a selector whose
bucket boundaries are wrong; it cannot catch a table that disagrees with
reality. Only `tools/abi_derive.sh` plus a boot does that.

## 5. Boot it and watch for `DENY`

Capture QEMU's stdout — it is the only place allowlist diagnostics appear:

```bash
sudo bash scripts/run_test_vm.sh > /tmp/qemu.log 2>&1 &
grep -aE 'DENY|ABI profile' /tmp/qemu.log
```

Then run the ladder: `cuInit` → `cuCtxCreate` → an 8 MiB round trip verified
byte-exact → `cuMemsetD8` → a real kernel launch
(`tests/integration/arch_ladder_test.c`, `cuinit_test.c`,
`big_memcpy_test.c`, `vector_add_test.c`).

## 6. Expect the allowlist to need an entry

The profile table covers struct *layouts*. It does not cover which control
commands `libcuda` on that branch chooses to issue, and those differ.

This is exactly what happened bringing up 535. `libcuda` on 535.309.01 issues
`NVC36F_CTRL_GET_CLASS_ENGINEID` (`0xc36f0101`) during `cuCtxCreate`; `libcuda`
on 575.51.03 does not, so the command was absent from a table generated against
the 575 ABI. The symptom was one log line and a CUDA error code
(`src/qemu/nvkvm_ctrl_allowlist.h:218-239`):

```
nvkvm: DENY ctrl cmd 0xc36f0101 (not in allowlist / oversize)
```

with `cuCtxCreate` failing `CUDA_ERROR_OPERATING_SYSTEM` (304).

If you hit this, **do not just add the number**. The commit that added
`0xc36f0101` justified it against OGKM at the driver version in question, and
that is the standard to hold: check `ctrlXXXX.h` for what the command actually
is, check its params struct for embedded pointers, and check whether an
equivalent command is already allowed under another class id. In that case all
three answers were favourable — a read-only "which engine does this class run
on" query, 16 bytes of four `NvU32`s with no pointers, and the identical command
already allowed under its older class id `0x906f0101`.

A command whose params contain a pointer needs marshalling in the guest and
reconstruction in the stub before it can be allowed — see
[the forwarding model](../internal/forwarding-model.md). Adding an allowlist
entry with no handler is worse than leaving it out; `0x70`
(`NV_ESC_EXPORT_TO_DMABUF_FD`) was removed from the frontend list for exactly
that reason (`src/qemu/nvkvm_fe_alloc_allowlist.h:40-45`).

## 7. Check the alloc-class size table

`RM_ALLOC` parameter sizes are chosen per `hClass` in the guest
(`src/guest/nvkvm_main.c:1651-1744` for NVOS21, `:1777-…` for NVOS64). Four
entries take their size from the ABI profile; the rest are `sizeof`.

`libcuda` frequently passes `alloc_parms_size = 0` and relies on the driver to
size the buffer by class, so a missing entry means copying **zero bytes** — and
the stub then forwards `pAllocParms = NULL`. **This is the single most expensive
failure shape in this codebase**, because RM does not reject it: it builds the
object from all-defaults, returns success, and the damage surfaces several
layers away as something that looks like a driver bug. Every instance so far:

| class | tag | missing entry causes |
|---|---|---|
| `NV01_MEMORY_VIRTUAL` (`0x70`) | #84 | kernel sees `hVASpace=0` → `INVALID_ARGUMENT`; breaks libGLX's EGL device enum |
| `NV_SEMAPHORE_SURFACE` (`0xda`) | #84 | `INVALID_ARGUMENT`, then `libnvidia-eglcore` NULL-derefs the missing object |
| `NV01_CONTEXT_DMA` (`0x0002`) + `NVENC_SW_SESSION` (`0xa0bc`) | #99 | NVENC `InitializeEncoder` fails |
| `BLACKWELL_CHANNEL_GPFIFO_A/B`, `BLACKWELL_DMA_COPY_A/B` | #101 | `cuCtxCreate` → `CUDA_ERROR_INVALID_VALUE` (1) on an RTX 5090 |
| `HOPPER_USERMODE_A` (`0xc661`) | — | aperture built without `bBar1Mapping` → `GET_ADDR_SPACE_TYPE` answers REGMEM, Vulkan dies three calls later on an H100 |
| `NV50_THIRD_PARTY_P2P` (`0x503c`) | — | `cuInit` → `CUDA_ERROR_NOT_SUPPORTED` (801) |
| `GT200_DEBUGGER` (`0x83de`) | — | `INVALID_ARGUMENT`, then `CUDA_ERROR_CONTEXT_IS_DESTROYED` (709) on the next `cuMemAlloc` |

(`src/guest/nvkvm_main.c:1690-1795`; the class ids and their write-ups are in
`src/abi/nvgpu.h`.) The Hopper one is worth reading in full, because it is the
instance where the wrong conclusion got **published twice** before the right one:
[Vulkan compute on Hopper](../reference/correctness.md#vulkan-compute-on-hopper--root-caused-it-was-ours-and-it-was-never-a-driver-bug).

**Being on the alloc-class allowlist is not the same as being sized.** Every
class above was *reachable* — the allowlist let it through — and unsized. If you
add a class id to `src/qemu/nvkvm_fe_alloc_allowlist.h:59-149`, check the two
size-by-`hClass` switches in the guest at the same time. A new architecture
generation typically brings new channel/compute/graphics class ids and needs
both.

## 8. Update the documentation

- [`docs/reference/supported-drivers.md`](../reference/supported-drivers.md) —
  move the row from "measured" to "booted", and add the GPU to the tested table.
- [`docs/reference/abi-profiles.md`](../reference/abi-profiles.md) — the row
  table.
- The root `README.md` tested-platforms table.

Be precise about the distinction the whole page rests on: a row measured from
OGKM by `tools/abi_derive.sh` is *expected* to work. A row that has booted is
*known* to work. Do not blur them.
