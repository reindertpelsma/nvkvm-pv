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
11. **CI's kernel matrix is blind at the top of the range, not red.**
    Measured locally: `tests/kernel_matrix.sh ubuntu:26.04` returns
    `SKIP -- no kernel headers in this image`. It never compiles anything, so
    it could not have caught `page_mapcount`. The job is not failing to notice
    a break; the row that would have found it does not run.

    `kernel_matrix.sh:100` is `[ "$verdict" = SKIP ] && [ "$STRICT_SKIP" = 1 ]
    && rc=1`, and `.github/workflows/kernel-matrix.yml` sets `STRICT_SKIP: "1"`
    -- so on GitHub this should be a hard failure, unless headers resolve there
    and not here. **Unverified from this machine:** whether GitHub's runners can
    install `linux-headers` for `ubuntu:26.04`. Check the actual CI run history
    before concluding which of the two it is.

    The script's own header anticipated this exact failure: "a package gets
    renamed would quietly degrade to 'passing'". It degraded.

### Not blocking

12. Pre-Turing bring-up (§4).
13. `--ssh` end-to-end test against a real rented box — the code is committed
    but the full path has never been exercised.
