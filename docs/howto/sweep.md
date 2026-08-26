# Running the unattended coverage sweep

`scripts/sweep.sh` rents vast.ai KVM boxes, builds nvkvm on each, boots the
guest, and runs `tests/validate.sh` against several **forced** NVIDIA driver
versions — then destroys the box and moves to the next GPU architecture. No
human and no agent per box.

The unit of coverage is the **architecture**, not the GPU model. The target is
one box per major architecture (turing, ampere, ada, hopper, blackwell), each
forced through at least five driver versions chosen to cross the ABI profile
boundaries.

> **Status:** the acquisition, safety, selection and reporting layers are
> covered by `tests/sweep_offline_test.sh` against a stubbed vast.ai API. The
> complete in-box path has also passed on Turing and Ampere with multiple forced
> drivers; an Ada control passed while its forced rows were correctly reported
> untested after NVIDIA's CDN returned HTTP 403 to that rental. See
> `tests/BOOT_MATRIX.md` and "What is and is not tested" below.

---

## Quick start

```bash
# 1. See the plan and what it would cost. Rents nothing, spends nothing.
scripts/sweep.sh --all-arches

# 2. Layer 1: one box, one driver. The smallest thing that can actually work.
#    --min-drivers 1 because a one-driver run is a bring-up, not coverage;
#    without it the box is correctly reported as a coverage shortfall.
scripts/sweep.sh --arch ampere --drivers 580.95.05 --min-drivers 1 --max-spend 1 --go

# 3. Layer 2: one box, the whole driver set.
scripts/sweep.sh --arch ampere --max-spend 3 --go

# 4. Layer 3: every architecture.
scripts/sweep.sh --all-arches --max-spend 12 --go

# Optional: relay official installers from a coordinator-local cache when a
# rental's NVIDIA CDN edge returns 403. Files use NVIDIA's canonical names.
scripts/sweep.sh --arch ada --driver-cache /srv/nvidia-drivers --go
```

Stop a running sweep gracefully:

```bash
touch /tmp/nvkvm-sweep.stop
```

It is checked between drivers and between boxes, and unwinds through the normal
destroy paths. Do **not** `kill -9` the sweep — that is what the standalone
auto-destroy timer exists to survive, but it is the safety net, not the plan.

---

## What it costs

A box is needed for roughly `1.2h + 0.25h per driver`, so a six-driver box is
about 2.7 hours. KVM-capable offers run $0.04–$0.40/hr, so a full
five-architecture sweep is on the order of **$1–3**. The dry run prints a live
estimate from the actual offer market before anything is rented.

Three independent cost controls, all on by default:

| control | flag | default | enforced |
|---|---|---|---|
| per-box hourly rate | `--max-dph` | `$0.50/hr` | at offer selection |
| total spend | `--max-spend` | `$10.00` | **before every create**, not tallied at the end |
| wall-clock kill switch | `--budget-hours` | `8h` | by a separate process (see below) |

---

## The auto-destroy timer

Before a single cent is committed, `sweep.sh` starts
`scripts/sweep_autodestroy.sh` as a **separate, detached process** and then
proves it is running with `ps` on its exact pid. If the timer does not come up,
the sweep refuses to rent anything.

This is deliberately not a bash trap or a Python `finally`. Those share their
fate with the process they protect — `kill -9`, an OOM kill or a dropped ssh
session takes the net down with the swimmer. That has already orphaned a box on
this project.

The timer reads `sweep-runs/<run>/instances.registry`, which the sweep appends
to the instant a create returns, *before* anything else touches the box. At its
deadline it destroys everything in that registry. There is intentionally **no
disarm**: destroying an already-destroyed instance is a harmless no-op, while a
disarm that fires wrongly costs real money. It exits early only when the sweep
has signalled completion **and** `vastai show instances` agrees that nothing
registered is still alive.

If something goes wrong and you just want everything of ours gone:

```bash
scripts/sweep.sh --reconcile      # destroys anything labelled nvkvm-sweep, spends nothing
```

---

## Output

Everything lands in `sweep-runs/<UTC timestamp>/` (gitignored):

```
sweep.jsonl              one JSON record per unit, appended immediately
summary.md               the rendered tables
autodestroy.log          what the timer did
instances.registry       every instance id this run created
logs/<arch>-<instance>/  build.log, guest.log, bundle.log, drv-*.log,
                         qemu-nvkvm-vm.log — pulled off the box before it dies
```

`sweep.jsonl` is written after every unit and `fsync`ed, so a crash loses at
most the unit in flight. Each record carries the architecture, the GPU, the
driver requested **and the driver `/proc/driver/nvidia/version` actually
reported**, the ABI profile nvkvm selected, validate.sh's own JSON nested
whole, the instance and machine ids, the tree commit, and the elapsed time.

`summary.md` leads with the question the sweep exists to answer:

```
## is this architecture covered?

| architecture | drivers with a verdict | passed | not tested | covered? |
|---|---|---|---|---|
| ampere | 6 | 6 | 0 | YES |
| ada    | 4 | 4 | 2 | **NO** |
```

"Covered" means at least `--min-drivers` versions produced a validate.sh
verdict, every one passed, and nothing was left untested.

---

## Resuming

Re-running is safe and does not repeat paid work:

```bash
scripts/sweep.sh --resume sweep-runs/2026-08-23T21-04-00Z --go
```

A `(architecture, driver)` unit is skipped when it already reached a **terminal**
status — `pass`, `fail`, `incomplete`, `driver-predates-gpu`. Those are answers;
re-running them buys nothing. Everything else (install failed, box never
provisioned, guest never booted) is the harness or the market failing, and is
**retried**.

---

## The driver set

Five-plus versions chosen to cross ABI profile boundaries, not five recent ones.
nvkvm's correctness against a driver is decided by one thing: does the ABI
profile it selects describe that driver's real struct layouts? So the set walks
the seams.

| version | profile | why |
|---|---|---|
| 565.57.01 | 550 | below the 570 seam — top of the 550 range |
| 570.124.06 | 570 | the 570 profile proper (== 575 layouts) |
| 580.95.05 | 580 | **bottom** of the 580 profile's claimed 580..595 range |
| 590.48.01 | 580 | middle of that range |
| 595.84 | 580 | **top** of the range — the seam where the struct table and the NVKMS enum table disagree about eras |
| 610.57.04 | 610 | newest published 610; V610 channel (376 B, `+hHandleVASpace`) |

Blackwell's architecture floor is 570, so `565.57.01` drops out and Blackwell
still gets five rows. Every other architecture gets six.

All six, and every same-profile alternate, were confirmed downloadable from
NVIDIA on 2026-08-23 (HTTP 200 at one of the two paths `sweep_matrix.py`
tries). Re-check with a `curl -I` before blaming a box if an install starts
404ing — NVIDIA does unpublish versions, which is why the alternates exist.

Some CDN edges reject rented IPs with HTTP 403 even while the same object is
reachable from the coordinator. `--driver-cache DIR` (or
`NVKVM_SWEEP_DRIVER_CACHE`) handles that without weakening a verdict. Put files
in `DIR` under their canonical names, for example
`NVIDIA-Linux-x86_64-610.43.02.run`. The coordinator hashes and transfers the
bytes but never executes them. The transfer is atomically promoted only after
its remote SHA-256 matches, and the disposable KVM box still runs the
installer's own `--check` before use. Missing entries fall back to the CDN;
corrupt entries are rejected and recorded as harness evidence.

The expected profile in that table is **not** hardcoded anywhere. `sweep.sh`
compiles `src/common/nvkvm_abi.h` and calls `nvkvm_abi_id_for_version()`, so the
expectation is by construction whatever the shipped selector says. The
comparison in the results is therefore "did the running QEMU agree with the
header it was built from" — which is the question worth asking, and it cannot
drift the way a copied table would.

Use `--drivers` to restrict the set, or `--preset matrix` for the full
twelve-row historical matrix from `sweep_matrix.py`.

**Versions that must not be added:**

- **610.88 does not exist for Linux.** NVIDIA's index lists only 610.43.02,
  610.43.03 and 610.57.04 for the 6xx branch.
- **596–609 is empty.** No OGKM branch was ever published in that range — see
  the comment saying exactly that in `nvkvm_abi_id_for_version()`.

Adding either produces an install failure that reads like a driver finding and
is nothing of the kind.

---

## Failure statuses, and why they are kept apart

A skipped driver reported as a pass is the exact failure mode this script exists
to eliminate, so "it did not work" is never one status.

| status | meaning | counts as tested? |
|---|---|---|
| `pass` / `fail` / `incomplete` | validate.sh's own verdict (`incomplete` = checks were SKIPped, which is **not** a pass) | yes |
| `driver-predates-gpu` | the driver is older than the silicon; installs fine, sees no GPU | excluded by design |
| `driver-install-failed` | the driver could not be installed — **fails the box** | **no** |
| `box-never-provisioned` | the rental never became usable | **no** |
| `not-a-vm` | we got a container, not a KVM VM | **no** |
| `build-failed` / `guest-failed` | QEMU or the guest image would not build | **no** |
| `guest-no-boot` / `guest-module-not-loaded` | the guest never came up | **no** |
| `bundle-failed` / `stage-failed` | host-libs staging broke — harness, never a GPU verdict | **no** |
| `coverage-shortfall` | fewer than `--min-drivers` verdicts on a box | **no** |
| `control-*` | the free run on whatever driver the image shipped | not counted |

Exit codes: `0` clean, `1` a real validate.sh failure, `2` something was **not
tested** (coverage incomplete — do not read the run as clean), `3` could not
start, `4` **an instance may still be billing**.

### The control run

Before any driver is purged, the sweep validates whatever driver the vast image
already ships and records it as `control-*`. It is free (no download, no module
rebuild) and it separates "this box's harness works" from "driver X is broken" —
without it, a bundle or guest-boot problem on a new image reads as five
consecutive driver failures and points the investigation at NVIDIA. It does not
count toward `--min-drivers`. Disable with `--no-control`.

---

## Operational traps encoded in the script

Each of these cost real time or money, and each is handled in code rather than
left to be rediscovered.

- **A VM box takes ~30 minutes to load.** `actual_status: loading` means the
  image is still pulling. The sweep waits up to 45 minutes and prints a
  heartbeat; it never destroys on a stopwatch. Three healthy instances were
  destroyed mid-pull on 2026-08-23 and paid for twice.
- **The only signature of a genuinely dead box is the log.** Not the status
  string, and never elapsed time. Dead is `actual_status: created` **plus** an
  instance log frozen at `Domain not found: no domain with matching name
  'C.<id>'`, unchanged across two polls ≥2 minutes apart. That is host-side and
  deterministic per machine, so the machine goes on the known-bad list in
  `scripts/sweep-known-bad-machines.txt` (seeded with 42636, 44906, 11908) and
  the sweep re-rents on a different `machine_id`.
- **You must actually get a VM, not a container.** Checked with
  `systemd-detect-virt` within seconds of the first ssh. `grep vmx
  /proc/cpuinfo` is **not** a valid check — a container inherits the host's CPU
  flags and looks perfectly healthy while the NVIDIA module belongs to the
  physical host. Requires vast's KVM image; `vms_enabled` describes the
  *machine*, not what you get.
- **`vastai destroy instance` prompts `[y/N]`** and with no tty prints
  "Aborted." and exits 0. The sweep uses `yes | ... -y` and then confirms the id
  is absent from `vastai show instances`. The return value is never trusted — a
  create can print `"success": false` and still leave a live contract, which is
  why `parse_contract` extracts the id even from a failed-looking create.
- **Working ssh is `public_ipaddr` + `ports['22/tcp'][0].HostPort`**; the
  `sshN.vast.ai` proxy is frequently refused and is only the fallback.
- **The advertised `driver_version` describes the physical host** and is
  meaningless for KVM rentals. Ignored for selection; the sweep reads
  `/proc/driver/nvidia/version` inside the instance.
- **Advertised bandwidth is unreliable** — one box pulled at ~10 KB/s while
  advertising 1.3 Gbps. Recorded for forensics, never used for ranking.
- **Never `pkill -f` a pattern that could match the caller's own command line.**
  It has killed the issuing shell five times on this project. The sweep matches
  on exact pids (`ps -p`), and the one remote pkill uses a bracketed pattern.
- **Blackwell and datacenter Hopper need the open kernel module** (`-m=kernel-open`).
- **`build_qemu.sh` exits 0 without rebuilding if the binary already exists.**
  The sweep is headless so `NVKVM_QEMU_UI` is deliberately unset — but if you
  ever add it, it needs `--force` too or the re-run is a silent no-op.
- **`vastai` prints a deprecation banner on stderr.** Merging stderr into stdout
  puts prose in front of the JSON and every parse fails. All CLI calls go
  through a wrapper that discards stderr.

---

## Relationship to `scripts/sweep_matrix.py`

`sweep_matrix.py` did the per-box work first, and its comments carry lessons
that cost real money: the gcc-11/gcc-12 kernel-module trap, the held-dpkg driver
purge, NVIDIA's CDN 403s, the stale `host-libs` bundle. `sweep.sh` does **not**
fork that knowledge — it calls it. Driver installation goes straight to
`sweep_matrix.install_driver()`, and the architecture map, floors and
open-module set are read out of the module at run time. There is one
implementation of the hard part.

What `sweep.sh` adds is everything *around* the box, which is where the
unattended failures actually live: money safety that survives the process being
killed, telling a slow box apart from a dead one, a growing known-bad machine
list, resumability, and an exit code that means something.

One difference worth knowing: substitutes for unpublished versions are **gated**
in `sweep.sh`. An alternate may only stand in for a primary when
`nvkvm_abi_id_for_version()` gives them the same profile, so a fallback can
never quietly change what the row measures.

---

## What is and is not tested

Run the offline suite any time — it rents nothing:

```bash
bash tests/sweep_offline_test.sh      # 80 checks, ~90s, rents nothing
```

It sources `sweep.sh` in library mode and drives the **real** functions against
a stubbed `vastai` whose behaviour is scripted per test: contract parsing
including `success: false`, offer selection with the price cap and known-bad
filter, the dead-box-vs-slow-box discriminator, destroy verification against a
CLI that exits 0 without doing anything, protected-instance refusal, stray
reaping by label, leak reconciliation, resume semantics, spend-cap enforcement,
the invocation-scoped warning selector, driver-cache relay integrity, and the
auto-destroy timer end to end.

**Covered offline:** everything above the ssh boundary.

**Covered on hardware:** tree shipping, QEMU and guest setup, host-driver
replacement, host bundle creation, guest boot/module load, userspace staging,
and all of `validate.sh` have run end to end on Turing and Ampere. The 2026-08-26
RR-09 matrix in `tests/BOOT_MATRIX.md` is the evidence. Each new architecture
and driver interval still needs its own verdict; a control pass or a transport
failure is deliberately not extrapolated into coverage.

`tests/validate.sh` is deliberately not modified by any of this; the sweep runs
whatever that suite becomes and reports its verdict and exit code faithfully.
