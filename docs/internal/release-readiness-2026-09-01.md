# Release readiness — audit of 2026-09-01

Written overnight before the planned announcement. Everything here is either
measured or explicitly marked unverified. Where a claim could not be checked,
that is stated rather than filled in with plausible reasoning.

**Bottom line:** the compute path is in good shape and got materially better
tonight. Two harness bugs were found that mean some past coverage was narrower
than the record implied. Nothing found tonight is a reason not to announce; two
things are reasons to fix wording.

---

## 1. Axis coverage, as actually verified

| axis | status | evidence |
|---|---|---|
| **GPU driver** | strong | 12/12 pass on the merged sharing fix (ampere + ada, ABI 550/570/580/610). Re-running now across four architectures. |
| **GPU architecture** | good, in progress | turing **7/7 clean** on this tree (control + 6 drivers). ampere/ada/blackwell running. |
| **Guest kernel** | **was never tested until tonight** | see §2. One real data point now: questing, **kernel 6.17.0-40-generic**, 35P/0F/0S. |
| **Corruption / sharing** | fixed and merged | `host_register_two_process_share` is inside all 35 checks, so every green row above also exercises it. |
| **Guest distro** | narrow | Ubuntu 24.04 and Debian 12 run-tested. Others compile-tested only. Unchanged. |
| **Datacenter GPU** | untested, but reachable | A100 is rentable with KVM today (~$1.09/hr, ~$3 for a full driver set). Hopper/H100/H200/B200: still zero KVM offers. |
| **Pre-Turing** | unsupported, now diagnosed | see §4. |

### The "0 of 69 datacenter offers" figure is stale

That came from a survey dated 2026-08-18. Measured again tonight against 627
single-GPU offers: **A100 SXM4, Q RTX 6000 and RTX 6000 Ada are all rentable
with `vms_enabled`**. Do not repeat the 0/69 line in launch copy. If a
datacenter claim is wanted, re-measure on the day and cite the date — the
market visibly fluctuates.

---

## 2. Harness bugs found tonight (these change what past results mean)

### 2.1 `--guest-image` was a no-op — the guest-kernel axis never ran

`sweep.sh` computed `GUEST_IMAGE_URL`, validated the series, printed
*"guest series: questing (the guest KERNEL axis -- sweep rule 4)"* in the plan
— and never put it in `ENVP`, so it never reached the box. `setup_guest.sh`
fell back to its hardcoded noble URL.

**Measured:** `--guest-image questing` booted Ubuntu 24.04 / 6.8.0-138-generic
and returned a clean 35P/0F/0S. A green row for a kernel that was never tested.

Consequence: **every sweep in this project's history booted Ubuntu 24.04 on
6.8**, whatever was requested. The guest-kernel axis was not skipped by choice;
it was unreachable while appearing exercised. That is why the 6.16
`page_mapcount` break reached `main`.

Fixed on `fix/guest-image-axis`, in three parts: export the URL; derive the
image filenames from it (noble keeps `ubuntu-24.04.qcow2`, because
`run_test_vm.sh`, `container-entrypoint.sh` and `docs/howto/run.md` all test
that exact path); and **assert what booted** — `uname -r` and
`VERSION_CODENAME` are now recorded on every row, and a mismatch is
`guest-image-mismatch`, counted as UNTESTED rather than FAIL.

Verified working: `guest_series=questing guest_kernel=6.17.0-40-generic`, two
rows, 35P/0F/0S.

### 2.2 The known-bad machine list is stored in the versioned tree

`sweep.sh` appends dead machines to `scripts/sweep-known-bad-machines.txt`, a
**tracked** file. Two consequences, both observed tonight:

* Writing it dirties the tree, and the sweep refuses to run against a dirty
  tree because it ships the working tree. One dead box therefore blocks every
  later sweep until somebody commits. This killed the `resolute` and `jammy`
  guest-kernel runs — both exited 3 without renting anything, reporting a
  working-tree error that says nothing about the cause.
* The list is per-branch and per-worktree. It diverged four ways tonight, and a
  retry from a different worktree **re-rented a machine already known bad**,
  waited out the full window, and blacklisted it again in a different file.

It is account-level operational state, not source. It belongs outside the tree.

### 2.3 `box_is_dead()` may be condemning healthy machines

Condemnation needs only status `created` + a log containing `Domain not found`
+ that log unchanged for `DEADCHECK_SETTLE` (**120s**). Instance 49444464 was
killed **2m42s** after entering `created`, 6m35s after creation — while the
same function's heartbeat printed *"44m of patience left … VM images take
~30m"*.

Three machines were condemned this way tonight. If a healthy-but-slow box is
quiet early on, it looks identical to a dead one and is **permanently
blacklisted**. Under test in a subagent by re-renting a condemned machine with
`NVKVM_SWEEP_DEADCHECK_SETTLE=3000`. If confirmed, the blacklist needs auditing
for false entries.

---

## 3. Overclaim audit

### Corrected tonight

* **`nvkvm-steamos` README, Games row.** Claimed four titles "launch and play".
  Stardew Valley appears **nowhere else in the repo** — no screenshot, no log.
  Planetary Annihilation is documented in `docs/status.md:219` as crashing after
  ~20 minutes (`GpuWatchdogThread::DeliberatelyTerminateToRecoverFromHang`).
  Row now claims only Tomb Raider and Portal 2, both of which have committed
  screenshots, and describes PA's actual behaviour. Branch `fix/games-claim`.

* **`docs/reference/correctness.md`.** Said the HOPPER_USERMODE_A fix was
  "currently absent from this tree" and told readers to restore it before
  quoting Hopper results. It has been present since `c9e3875`; the caveat was
  pinned to a commit **541 commits** behind `main`. Corrected on `main`.

### Checked and found honest — no change needed

* **README guest-kernel claim.** "kernel 5.15 – 7.0 (built on every LTS in
  range; run-tested on Ubuntu 24.04, kernel 6.8)" correctly separates *built*
  from *run-tested* and names the single run-tested config. The overclaim was in
  the evidence pipeline, not the prose.
  *One clause was false:* "built on every LTS in range" — untrue at the top of
  the range until the 6.16 fix, since the module could not compile there at all.
* **`nvkvm-kata` README.** Unusually candid: 8 caveat lines, explicit "Not
  tested at all: multi-GPU selection, Kubernetes/CRI, cgroup v1", and an honest
  note that the device cgroup is not enforced.
* **GPU requirement.** "Turing or newer — Pascal enumerates but `cuInit` fails,
  and the open kernel module will not probe it at all." Confirmed accurate
  tonight on real hardware.

### Still to check before publishing

* Every performance number intended for launch copy, against
  `docs/reference/quoting-numbers.md`'s own rule that figures carry hardware +
  driver + date. Not audited tonight.
* The retracted "795 fps glmark2" figure must not appear in any draft.

---

## 4. Pre-Turing: diagnosed, not supported

Not a launch item. Recorded because the diagnosis is now cheap to act on.

**Root cause identified.** On a Quadro P4000, with **both** the OGKM control and
the proprietary 580.95.05 install, the identical signature:

```
6  nvkvm: DENY alloc class 0x0000c06f      <- PASCAL_CHANNEL_GPFIFO_A
1  nvkvm: DENY ctrl cmd 0x20800513
1  nvkvm: DENY ctrl cmd 0x00730102
3  nvkvm: AUDIT unknown ioctl cmd=0x37
```

The guest reaches a channel allocation and the QEMU gate refuses it. The
blocker is the default-deny allowlist (which tracks gVisor nvproxy, and nvproxy
has no pre-Turing classes) — **not** the module flavour, and **not** a missing
USERMODE/submission path.

Constants looked up from NVIDIA's own OGKM headers rather than by trial:

| class | id | | class | id |
|---|---|---|---|---|
| `PASCAL_CHANNEL_GPFIFO_A` | 0xC06F | | `MAXWELL_CHANNEL_GPFIFO_A` | 0xB06F |
| `PASCAL_A` / `PASCAL_B` | 0xC097 / 0xC197 | | `MAXWELL_A` / `MAXWELL_B` | 0xB097 / 0xB197 |
| `PASCAL_DMA_COPY_A` | 0xC0B5 | | `MAXWELL_DMA_COPY_A` | 0xB0B5 |
| `PASCAL_COMPUTE_A` / `_B` | 0xC0C0 / 0xC1C0 | | `MAXWELL_COMPUTE_A` / `_B` | 0xB0C0 / 0xB1C0 |

No new alloc-param structs are needed: `nvkvm_main.c` already maps six
generations of `*_DMA_COPY_*` onto one `nvb0b5_allocation_parameters` (named
for *Kepler's* copy class, which is how long that layout has been stable), and
all compute/graphics onto `nv_gr_allocation_parameters`.

**How far back it could ever go:** Volta, Pascal, Maxwell — and no further.
Kepler's last driver branch is 470, below the 515 ABI floor, so no driver both
drives the silicon and has a derivable ABI. That is arithmetic, not effort.
All three are proprietary-only forever (OGKM needs GSP).

**Market reality:** of 627 offers, only **2 pre-Turing cards have KVM** (both
Quadro P4000). Maxwell (8 cards) and Volta (10, incl. Tesla V100) are present
but none expose the VM template. So Pascal is the only generation that can be
iterated on, and Maxwell/Volta support would be static-inspection only — which
must be labelled as such, exactly like the 515/525 ABI rows.

---

## 4b. Audit of the guest-library staging path

Prompted by an `RTX 4060 Ti` failing `cuda_ptx_jit` with
`CUDA_ERROR_JIT_COMPILER_NOT_FOUND` on four consecutive drivers (30P/1F/4S each,
with `cuda_kernel_launch`, `cuda_matmul`, `cuda_managed_alloc` and
`cuda_managed_coherence` cascading to SKIP). The guest had `libnvidia-nvvm` but
no `libnvidia-ptxjitcompiler`, and `nvkvm-guest.service` reported success.

**Expectation going in: the staging path was fragile.** Three failures of this
family are on record (stale driver via `ldconfig`; wrong bundle picked
alphabetically; this one). **Finding: it is in better shape than that history
suggests.** Each of those was fixed, and the fixes hold up:

* **Bundle selection is sound.** `stage_guest_libs.sh` prefers
  `host-libs-$HOSTV` matching the running host driver (read from `dmesg`), and
  when it must choose between several candidates it **exits 1 rather than
  guessing**. The alphabetical bug cannot recur.
* **Stale-library cleanup is comprehensive.** I expected a coverage gap and
  went looking for one. There isn't: three cleanup passes, and the second
  globs `"$SYS"/libnvidia-*.so.[0-9]*` plus explicit entries for `libcuda`,
  `libEGL_nvidia`, `libGLX_nvidia`, `libGLESv2_nvidia`, `libGLESv1_CM_nvidia`,
  which between them cover **every** library the script stages to `$SYS`.
  `$CUDADIR` is cleaned wholesale. I briefly believed `libnvidia-cfg`,
  `libnvidia-glvkspirv` and `libnvidia-opencl` were uncovered; they are matched
  by the `libnvidia-*` wildcard and my analysis had turned it into a literal.
* **`make_host_bundle.sh` fails loudly** when a REQUIRED library is absent
  (`libcuda`, `libnvidia-ml`, `libnvidia-ptxjitcompiler`, `libnvidia-nvvm`) —
  exit 1 with the names.

### Two real defects

**(a) A failed bundle is left on disk, indistinguishable from a good one.**
`make_host_bundle.sh` creates `host-libs-$V` and populates it with whatever it
can find, and only checks `missing_required` at the very end. So a run that
exits 1 still leaves a partial bundle behind, with no marker. Nothing downstream
can tell it from a complete one — `stage_guest_libs.sh` selects by directory
name. The sweep happens to be safe because it `rm -rf`s bundles before each
build; a user following the docs is not. **Fix:** build into a temporary
directory and rename into place only on success, or drop a `.complete` sentinel
that staging requires.

**(b) Critical and cosmetic staging failures shared one exit code.** FIXED
tonight on `fix/staging-critical-libs`. `stage_guest_libs.sh` exited 2 both for
an absent Wayland/GBM EGL library (which a headless host legitimately lacks) and
for an absent `libnvidia-ptxjitcompiler` (without which CUDA does not work).
One code, two meanings, so `nvkvm-guest.service` could only express its
tolerance as `|| true` — and that tolerance, written for the cosmetic case,
swallowed the fatal one. Critical libraries now exit 3, cosmetic stays 2, and
the unit fails on 3.

### ROOT CAUSE — found, and it was not staging at all

Measured in the live guest:

```
conf exists: yes          conf bytes: 0          ptxjit resolvable: 0
/usr/local/nvidia-guest/lib/libnvidia-ptxjitcompiler.so.1 -> ...610.57.04
```

`/etc/ld.so.conf.d/nvidia-guest.conf` existed but was **zero length**, so
`$CUDADIR` was never on the loader path. The libraries were staged correctly;
nothing could find them. The guard was `[ ! -f ... ]` — existence only — so an
empty file passed and was never repaired on any later boot.

Why exactly this pattern: `libnvidia-nvvm` also lands in `$SYS`, which is on the
default path, so it stayed resolvable, while `libnvidia-ptxjitcompiler` lives
only in `$CUDADIR` and vanished. `validate.sh` reports "so.1 **or** so.4 is
missing/unstaged", which is why it read as a staging fault. The control passed
35P/0F/0S because it runs before the guest has been through a driver swap.

Zero length with intact metadata is ext4 delayed allocation dropping a recent
write when the guest stops uncleanly — which a driver swap does. Same shape as
the version-only idempotence check that could not detect an OOM-truncated
NVIDIA install: **check the content, not the presence.**

Fixed on `fix/staging-critical-libs` (`a137d30`): the guard now greps for the
path, repairing missing, empty and wrong-content cases, with a `sync` after the
write. Verified: missing/empty/wrong all repair, correct is left alone.

Note this contradicts my own conclusion two sections up, which said the staging
path was sound and the defect lay elsewhere. The staging path **was** sound.
The bug was one layer past it, in loader configuration, and was only found by
inspecting a live guest rather than reading the scripts.

## 5. TODO, ordered

### Before announcing

1. **Merge `fix/page-mapcount-616`.** Without it, nobody on a ≥6.16 guest —
   SteamOS, Fedora 42+, current Arch — can build the guest module at all. Now
   sweep-verified on kernel 6.17 (35P/0F/0S), not just a local build.
2. **Merge `fix/guest-image-axis`**, or the guest-kernel axis silently stops
   being real again the moment someone trusts a `--guest-image` row.
3. **Merge `fix/games-claim`** in nvkvm-steamos.
4. **Wait for the four-architecture sweep to finish** and read the result. Do
   not announce on a partial run.
5. **Re-measure the datacenter-availability line** on the day, or drop it.
6. Scope the copy: consumer/prosumer, no multi-tenant security claim, every
   number carrying hardware + driver + date.

### Soon after

7. Move the known-bad machine list out of the versioned tree (§2.2).
8. Resolve the `box_is_dead()` false-positive question and audit the blacklist
   for wrongly-condemned machines (§2.3).
9. Add the guest-kernel axis to the routine sweep now that it works — jammy
   (5.15) and resolute at the ends. Neither has ever run.
10. `matrix.md`, auto-generated from `sweep.jsonl`, rendering UNTESTED as its
    own state. It is the artifact that makes "here is what we tested, here is
    what we did not" concrete.
11. **UNRESOLVED — do NOT act on my earlier claim here.** An earlier revision
    of this document asserted that CI's kernel matrix is "blind at the top of
    the range" because `tests/kernel_matrix.sh ubuntu:26.04` returned
    `SKIP -- no kernel headers in this image`. **That conclusion was wrong, or
    at least unsupported.** Containers on the machine I ran it from cannot reach
    package mirrors — `archlinux` failed with `Resolving timed out after 10002
    milliseconds`, and an `apt-get update` test exceeded 120s the same way. So
    every image skips here for want of network, whatever its headers situation.
    `kernel_matrix.sh` sends all installer output to /dev/null, which makes a
    network failure and a header-less image produce the identical result line —
    itself worth fixing.

    What is still true and worth checking: the matrix images top out around 6.14
    (fedora:42, ubuntu:25.04) and 6.12 (debian:13), so whether ≥6.16 is covered
    depends entirely on `ubuntu:26.04` behaving on GitHub's runners. **Check the
    actual CI run history** — that is the only place this can be settled — and
    if that row does skip there, `STRICT_SKIP: "1"` in the workflow should
    already be turning it into a failure.
12. Pre-Turing bring-up (§4).
13. `--ssh` end-to-end test against a real rented box — the code is committed
    but the full path has never been exercised.
