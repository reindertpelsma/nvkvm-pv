# Release-blocker audit — v0.2.0, 2026-09-02

Independent audit of `main` at `e88cb3a`, done from a fresh worktree
(`audit/release-blockers-v0.2.0`), the day before the tag. Scope: does anything
in the public-facing surface (README, docs/howto, docs/reference, SECURITY.md,
docker-compose.yml) mislead a user, does the quickstart actually work as
written, are there dangling citations for anything security-relevant, and are
there more instances of the two bug shapes this project hit today (stale
artefact trusted as current; a check sampling at the wrong instant).

This builds on and verifies `docs/internal/release-readiness-2026-09-01.md` rather than
repeating it. Where that document's own TODOs are now resolved or still open,
this says so with evidence. Four read-only sub-investigations covered the four
areas in parallel; every finding below was independently spot-checked against
source by the author of this document, not taken on a sub-agent's word.

**Bottom line:** the compute/forwarding claims hold up well and the two
harness bugs from last night are genuinely fixed and merged. But the
**quickstart does not work as written** (two independent breaks in the
documented command sequence), the **README's headline performance claim
overclaims** exactly the thing the project's own house rule exists to prevent,
and a **second repo's false claim, flagged for a merge last night, is still
false on its main branch**. None of this is subtle — all of it is one `git
show` away from being caught. Three small, obviously-correct wording fixes are
already applied on this branch (see "Fixes applied" at the end).

---

## BLOCKERS — must fix before tagging

### B1. README's headline performance number was not bare metal for half its citations — now fixed on this branch

`README.md:45-52` (before this branch's fix) read: *"Geekbench 7 GPU (OpenCL)
runs at 98.0–99.9% of bare metal... Guest on one side, the same physical box on
the other,"* citing all four of RTX 4070, RTX 3050 Laptop, H100 PCIe, and A100
80GB.

This is false for two of the four. `docs/reference/parity.md:26-30`: *"The
'host' side is itself a VM on the datacenter cards. Both the A100 and the H100
were rented as 'dedicated' machines and both turned out to be hypervisor
guests... What it is not is a bare-metal number."* The project's own citation
rule, `docs/reference/quoting-numbers.md:24`, lists exactly this as **do not
quote**: *"A100 / H100 Geekbench numbers as 'bare metal' — both rented boxes
turned out to be hypervisor guests themselves... Fine to quote as nested-guest
evidence, never as a bare-metal claim."*

This is the first hard number a reader sees, in the most prominent spot in the
document, and it directly violates the project's own written rule about
itself. **Verdict: BLOCKER.** Fixed on this branch (`README.md:45-52`):
reworded to "of host," and added the RTX-bare-metal / datacenter-nested split
with a link to `parity.md`. No other content changed.

### B2. The documented quickstart fails on its own first command — `setup_guest.sh` needs root, README doesn't say so

`README.md:205` and `:237` (both the tarball and from-source paths) say:
```
bash scripts/setup_guest.sh
```
`scripts/setup_guest.sh:12` sets `GUEST_DIR="${NVKVM_GUEST_DIR:-/opt/nvkvm-guest}"`
with no rootless fallback (unlike `build_qemu.sh`, which has one), the script
runs under `set -euo pipefail` (line 10), and its very first action is
`mkdir -p "$GUEST_DIR"`. On a normal Linux box `/opt` is root-owned; a
non-root invocation dies immediately with `Permission denied`, before wget,
apt, or anything else in the script runs.

`docs/howto/run.md:9` has the correct command — `sudo bash scripts/setup_guest.sh`
— proving this isn't ambiguous, just inconsistent between two docs for the
identical command. **Verdict: BLOCKER** — a new user following README verbatim
hits a hard stop on literally the second command of either install path.
Fixed on this branch: both README occurrences now read
`sudo bash scripts/setup_guest.sh`.

### B3. The from-source build path dies on a genuinely fresh host — `--install-deps` isn't mentioned

`README.md:236` (before this branch's fix) told a new contributor to run
`bash scripts/build_qemu.sh` with no flags. `scripts/build_qemu.sh:179-232`:
`NVKVM_INSTALL_DEPS` defaults to 0, and without it, missing build packages
(ninja, meson, libglib2.0-dev, ...) produce `ERROR: missing build
dependencies:` and `exit 1` — no auto-install happens. `CONTRIBUTING.md:36`
already has the right invocation, `build_qemu.sh --install-deps`; README's
"From source" section just never picked it up. **Verdict: BLOCKER** — the
documented from-source quickstart does not complete on a fresh machine.
Fixed on this branch: README's from-source snippet now includes
`--install-deps`.

### B4. Manual Docker capability advice in `run.md` silently downgrades isolation — `SETPCAP` missing

`docs/howto/run.md:339-343`'s manual `docker run` capability list omits
`SETPCAP`. `docker-compose.yml:20-29` (the maintained, correct reference)
lists **four** required capabilities and explains why: *"without these four it
silently falls back from uid+chroot to seccomp-only, which is no uid
separation and no chroot"* — confirmed in `src/qemu/nvkvm_isolate_uid.h`,
which documents exactly this fallback ladder. A user who copies `run.md`'s
`docker run` line instead of using `docker-compose.yml` gets a materially
weaker sandbox with no error, no warning, and no way to notice — the isolate
mode silently downgrades. This is precisely the kind of gap `SECURITY.md`'s
"Weaker configurations you should know about" section exists to prevent, and
it isn't in there for this specific path. **Verdict: BLOCKER** (security-
relevant, silent, and the fix is fully specified by the project's own correct
copy fifteen lines away in a different file). Not fixed on this branch —
this is a content change beyond a wording tweak (rule: don't touch
mechanism, this needs the real cap-add flags added to a code block), flagged
for the maintainer.

### B5. `build.md`'s manual QEMU-patching walkthrough ships stale linker flags that disable ASLR in the isolate

`docs/howto/build.md:191-194` documents linking the isolate stub with
`-nostdlib -static -pie`. `src/stub/Makefile:38` actually uses `-static-pie`
(one token, not two flags) — and the Makefile's own comment at the exact lines
`build.md` cites as its source (`src/stub/Makefile:7-24`) explains why the
distinction matters: *`-static -pie` silently links as `ET_EXEC` at a fixed
`0x400000` — no ASLR in the isolate*, the process this project's own security
audits (`docs/internal/audit-guest-pointers.md`) treat as the containment
boundary for guest-pointer bugs. The doc's manual path also omits
`-DNVKVM_STUB_SELF_RELOC`, without which — per the same Makefile comment —
relocations are never applied and the binary corrupts its own memory at
startup. Nobody following `docs/howto/build.md`'s hand-build section instead
of running `build_qemu.sh` gets a working, correctly-hardened stub.
**Verdict: BLOCKER** for anyone who takes the doc's own framing ("if you'd
rather not run a script over your QEMU tree, do it by hand") literally, and
security-relevant because it's the isolation boundary. Not fixed on this
branch (requires editing the documented flags precisely, left to the
maintainer to avoid introducing a second error).

### B6. `nvkvm-steamos` main still ships the games claim flagged for correction last night — the fix branch was never merged

`docs/internal/release-readiness-2026-09-01.md` §3 records this as already corrected:
*"Row now claims only Tomb Raider and Portal 2... Branch `fix/games-claim`."*
Checked directly against `/workspace/nvkvm-steamos` (a sibling repo, not
touched, read-only): `fix/games-claim` (`a9cbb6f`, *"README: only claim the
games we can actually show"*) exists and is correct, but
`git merge-base --is-ancestor a9cbb6f main` returns **not an ancestor** — the
fix was never merged. `git show main:README.md` on that repo, right now,
still reads: *"Shadow of the Tomb Raider, Portal 2, **Stardew Valley** and
**Planetary Annihilation** launch and play under the broker"* — the exact
overclaim the corrective commit exists to fix (Stardew Valley appears nowhere
else in that repo; Planetary Annihilation is separately documented as crashing
after ~20 minutes). This repo (`nvkvm-pv`) is technically out of this audit's
scope, but v0.2.0 is being "tagged and announced," `nvkvm-steamos` is the
SteamOS half of that announcement, and TODO item 3 in last night's own
readiness doc was explicitly "merge `fix/games-claim`" before announcing.
**Verdict: BLOCKER for the announcement as a whole**, even though it is not
this repo's file to fix. Flagging prominently rather than silently fixing a
sibling repo I was not asked to touch.

---

## SHOULD-FIX

### S1. `nvkvm_ctrl_allowlist.h`'s cited provenance for the base allowlist doesn't exist, and even the branch that investigated this didn't fix the citation

Confirmed independently: `src/qemu/nvkvm_ctrl_allowlist.h:7,9` cite
`docs/audits/nvproxy_control_allowlist.md` and
`docs/audits/empirical_control_cmds.md` as the provenance for the 167-row
control-command allowlist. Neither exists in 985 commits on any branch.
`docs/README.md:107-115` ("Things that do not exist") discloses `docs/audits/*`
generically as a known gap folded into other pages — and
`docs/reference/allowlists.md` §8 does restate the same provenance claim, but
it is a restatement, not independent verification; it cannot substitute for
the missing source. **This is a known, disclosed-but-unresolved gap, not a
hidden one** — worth stating plainly rather than either overselling or
dismissing it.

More concretely: a branch already exists that investigated this exact problem
in depth today, prompted by the RDR2/Proton `vkCreateDevice` failure —
`audit/graphics-ctrl-commands` (854f263, 6fb90de), not merged. Its own doc
(`docs/audits/graphics_control_commands.md` on that branch) states: *"Neither
file exists anywhere in this repository, on any branch, in any commit...
This audit could not read them and did not invent their contents."* That
branch adds 4 legitimately-justified allowlist rows but explicitly does not
touch the RDR2 blocker itself (the NV9096 ZBC-family gap, 4 commands, closed
handler) — so **RDR2/Proton remains broken on `main` today**, consistent with
the task's framing. Worth noting: even the branch that diagnosed this
correctly did not fix the header's own dead citation at lines 7 and 9 — the
same broken pointer would still be there after that work merges.

Separately, and **not disclosed anywhere**: `nvkvm_ctrl_allowlist.h:240` cites
`tools/nvtrace.c` as the empirical-capture tool for a later addition. That file
never existed (`git log --all --full-history` empty). The real tool is
`tools/nv_ioctl_trace.c`; `docs/internal/mint-guest-desktop.md:774-775`
compiles it to a binary named `nvtrace.so` — the comment conflated the
artifact's conventional output name with a source path. Underlying method is
real and traceable; this is a citation typo, not fabricated provenance, but
it's undisclosed and in the same security-relevant file as B1's cousin.

### S2. `docs/reference/tested-platforms.md` still tells readers to distrust a Hopper fix that was restored 541 commits ago

`tested-platforms.md:290-292`: *"which is not `main` today; it was reverted by
`4fece85` and has not been restored... before quoting a current Hopper
number."* `docs/reference/correctness.md:198-206` — the page this exact
sentence links to — says the opposite, with its own correction dated
2026-08-31: *"The fix is present on `main`... restored by `c9e3875`... This
note previously read 'the fix is currently absent from this tree'... That was
true when written and stayed on the page for 541 commits after it stopped
being true."* Confirmed against source: `nvkvm_alloc_parms_probe_len` is
present in `src/guest/nvkvm_main.c:1145` today. `correctness.md` was fixed;
the sibling doc sitting right next to it in the same directory, making the
identical claim, was not. Undersells current Hopper support (false negative,
not an overclaim) but it's a live, disprovable error in an in-scope reference
page.

### S3. `docs/reference/guest-kernels.md` doesn't record the two biggest kernel-support events of the whole project, both from today

The doc's own framing (`kernel-matrix.yml`'s comment: *"Keep
docs/reference/guest-kernels.md as the record of what has actually been
run"*) makes this the canonical page. It was last edited 2026-08-20
(`3303812`). Since then, on `main`: the 6.16 `page_mapcount()` removal was
fixed and merged (`c0bdf4a`, today), and the `--guest-image` harness bug that
made the whole guest-kernel axis a no-op for the project's history was fixed
and merged (`e9fb8be`, today) — with a real verified run on
`guest_series=questing`, **kernel 6.17.0-40-generic, 35P/0F/0S**
(`docs/internal/release-readiness-2026-09-01.md` §2.1). Both of these are genuinely
good, hard-won news (confirmed: both commits are ancestors of `main`,
resolving TODO items 1 and 2 from last night's readiness doc). Neither shows
up in `guest-kernels.md`'s "run" table (still only Ubuntu 24.04/6.8 and
Debian 12/6.1) or in README's Requirements row ("run-tested on Ubuntu 24.04,
kernel 6.8"). The strongest new evidence the project has isn't in the one
place designated to hold it.

### S4. `CONTRIBUTING.md` tells contributors the wrong reason `make run` fails, and there's a second, real, undocumented reason

`CONTRIBUTING.md:48-50` (written 2026-08-21): *"`make run` exits non-zero by
design: `test_isolate` fails 5 of its 7 cases at runtime on pre-existing API
drift, documented at `tests/unit/Makefile:65-69`."* Current state:
`tests/unit/run_tests.sh:115` sets `ISOLATE_KNOWN_FAIL=""` — empty, i.e.
`test_isolate` passes all cases today. The cited `Makefile:65-69` is now
unrelated build rules. Separately, and for real: `tests/unit/Makefile`'s
`run:` recipe still invokes `./test_dispatch` and `./test_frontend`
(`Makefile`, `run:` target body), both deleted in the DEAD-1 cleanup
(`a0cb87a`, "delete the unreachable dispatch/frontend pair") with no
replacement rule — so `make run` today aborts on the second line with "No
such file or directory," never reaching `test_isolate` at all. Both the
documented reason and the actual reason `make run` is unusable are now wrong/
undocumented. Low stakes — CI uses `run_tests.sh`, not `make run` — but a
contributor who reads `CONTRIBUTING.md` and tries it anyway gets a confusing,
wrong signal.

### S5. `sweep.sh`'s `--resume` doesn't check that a "done" unit was measured on the current tree

`scripts/sweep.sh:2148-2162` (`unit_done()`): a unit is skipped on resume if a
prior JSONL row matches `(arch, driver)` and has a terminal status
(`pass`/`fail`/`incomplete`/`driver-predates-gpu`) — it never checks the row's
own `tree` field (`TREE_STAMP`, written from `git rev-parse --short HEAD`,
already present on every row per `sweep.sh:2533`) against the current tree.
Resuming a sweep after committing a mid-sweep fix will keep pre-fix terminal
rows as final and never re-run that unit, with nothing in the row or the run
flagging it as stale. Same failure family as the QEMU-binary-reuse bug fixed
last night, one layer up (the checkpoint file, not the binary). Not confirmed
to have produced a wrong number in a currently-published table (the retained
per-arch matrices in `docs/reference/tested-platforms.md` all cite consistent
per-row `tree` stamps and matching evidence directories, verified present on
disk), so this is a live risk rather than a proven-fired bug.

### S6. `build_qemu.sh`'s new content-address stamp — the actual fix from last night — doesn't hash two of the three source trees it builds from

`scripts/build_qemu.sh:88-93` (`nvkvm_build_stamp()`) hashes only
`src/qemu` and `patches`. But step 5 of the same script
(`build_qemu.sh:487-488`) copies headers from **`src/abi/`** and
**`src/common/`** into the QEMU tree and compiles them in — the copy step's
own comment says so explicitly (line 484: *"the nvkvm .c/.h include SEVERAL
headers from src/abi + src/common"*), and the guard section's own comment nine
lines above the function (`build_qemu.sh:14`, `:50`) tells the operator
`--force` is needed *"after editing anything under src/qemu/ or
src/common/"* — src/common is in the guidance, not in the hash. Editing a
shared ABI/protocol header today does not change the stamp, so a plain
re-run reports "is current for these sources — skipping build" and reuses a
binary that no longer matches those headers — the exact bug the stamp exists
to prevent, one directory over. `tests/build_stamp_test.sh` doesn't catch
this either; it re-inlines the same incomplete `find src/qemu patches`
expression rather than sourcing the real function.

### S7. `src/stub/Makefile` doesn't depend on 6 of the 7 headers it compiles, including the same ctrl-allowlist header the current HEAD commit just fixed elsewhere

`src/stub/Makefile:49`: `$(TARGET): nvkvm_stub.c stub_clone3.S
stub_freestanding.h` — but `nvkvm_stub.c` also includes
`nvkvm_isolate_proto.h`, `nvkvm_ring.h`, `nvkvm_ring_ioctl.h`,
`nvkvm_abi.h`, `nvkvm_nvkms_ops.h`, and `../qemu/nvkvm_ctrl_allowlist.h` —
none are Makefile prerequisites. This is the same header the commit at the
tip of this branch (`e88cb3a`, "test_ctrl_gate must depend on the allowlist
header") just added a dependency for in the *test* Makefile, with a commit
message naming this exact failure shape. `build_qemu.sh:259` invokes
`make -C src/stub` as a plain (non-`clean`) `make`, so on a reused build
directory, editing the allowlist header will not necessarily relink the stub
that gets embedded into the shipped QEMU binary. The outer content-stamp
(S6) does cover `src/qemu`, which includes this header, so an outer rebuild
usually re-triggers — but the inner `make -C src/stub` step has its own,
separately incomplete dependency graph, and a reused `src/stub/` build
directory is exactly the scenario the outer stamp doesn't protect against.

### S8. `setup_guest.sh`'s qcow2 conversion is existence-only, directly downstream of a rigorously checksum-verified download

`scripts/setup_guest.sh:251-256`: `if [ ! -f "$QCOW2_IMG" ]; ... else skip`.
Everything upstream (`setup_guest.sh:150-211`) is carefully content-verified —
sha256 sidecar bound to the source URL, atomic rename, retry-from-zero on a
mismatch. This step throws that discipline away: if the raw cloud image is
re-verified or re-fetched (upstream republishes "current" under the same
filename, or a checksum forces a refetch) but the qcow2 at its fixed,
series-derived path already exists, conversion is skipped and the VM boots
from stale bytes. The risk is explicitly named in the codebase's own prose —
`sweep.sh`'s `guest-image-mismatch` detail string (line ~1923) already says
*"check... that no stale qcow2 is being reused"* — just not guarded in code.
Same shape as the QEMU-binary reuse bug, one script over.

### S9. Systemic missing driver+date on public performance numbers

`docs/reference/quoting-numbers.md`'s own checklist requires hardware, driver,
and date "not dropped" next to a number. In practice, driver and/or date are
frequently missing from the prose immediately around the number in the most
publicly visible pages: README's Geekbench block (45-52, no driver/date for
any of 4 entries), the clpeak paragraph (`README.md:412-418`, doesn't even
name the GPU inline — a reader must follow the external link), the Blender
section (`README.md:420-427`, no driver/date), `docs/reference/blender-
opendata.md` (driver present, no date anywhere in the file — confirmed via
`git log`, only the commit date, 2026-08-21, exists, not in the reader-visible
text) and `docs/reference/openbenchmarking-clpeak.md` (GPU only in the title,
no driver, no date anywhere). By contrast `docs/reference/tested-
platforms.md` and `correctness.md` comply consistently. The gap is
concentrated exactly in the highest-traffic copy.

### S10. Massive stale `file:line` citations across `docs/howto/*` — real content, wrong pointers

`docs/howto/build.md`, `run.md`, `stage-guest-libraries.md`, and
`add-a-driver-version.md` cite exact source line numbers as their evidentiary
style, and the underlying files have since moved substantially — `build.md`
alone drifted 150-230 lines across ~11 citations after `build_qemu.sh` grew
from ~280 to 577 lines this week (adding the content-address stamp and a
QEMU-commit pin guard). `add-a-driver-version.md` and `docs/reference/abi-
profiles.md` share one systematic drift (+32 lines, from one inserted comment
block in `src/common/nvkvm_abi.h`) affecting essentially every citation in
both files. None of this breaks a documented *command* — the prose and the
commands both check out — but a reader who follows a citation to "see the
code" lands on unrelated code every time. Two specific consequences worth
calling out on their own:
- `build.md:172-178` still warns that a plain re-run after editing `src/qemu/`
  or `src/common/` is "a silent no-op" — false for `src/qemu/` since last
  night's stamp fix (still true for `src/common/`, see S6, so the warning
  needs narrowing, not deleting).
- `build.md:425-435` cites unit-suite counts ("twelve binaries," "831 cases")
  against a suite that is now 19 binaries and 924 cases
  (`tests/unit/run_tests.sh`).

Not itemizing the full citation list here (dozens of individual `file:line`
misses were catalogued during this audit) — the fix is mechanical
(regenerate the citations against current source) and the doc content itself
was independently verified accurate wherever checked.

### S11. `stage-guest-libraries.md` doesn't document the exit code that means "CUDA is broken"

`stage-guest-libraries.md:91-92`: *"It exits 2 with a list if anything was
missing."* `scripts/stage_guest_libs.sh:610-626` actually has three outcomes:
0 (clean), 2 (cosmetic — optional libs like Wayland/GBM EGL missing), and
**3** (a CUDA-critical library failed to stage). The doc only documents the
first two. This is the exact split the project fixed *last night*
specifically because the old single-exit-code version let `nvkvm-guest.service`
swallow the fatal case with `|| true` (`docs/internal/release-readiness-2026-09-01.md`
§4b) — the doc describing the fix wasn't updated to match it.

---

## NOTES

Confirmed real but low-stakes, narrow-audience, or already tracked as
open work — listed compactly rather than expanded, per the instruction not to
pad an honest audit.

| # | finding | evidence |
|---|---|---|
| N1 | `MODULE_VERSION("0.1.0")` in the guest module wasn't bumped with today's v0.2.0 pin sweep | `src/guest/nvkvm_main.c:59`; not touched, since fixing it is a `src/` change out of this branch's remit |
| N2 | README's pointer-audit summary ("14 unenforced paths, 5 since fixed") doesn't match the underlying doc, which itself has an internal inconsistency (table shows more than 9 "fixed"/"closed" rows against its own header claim of "nine") | `README.md:66-71` vs `docs/internal/audit-guest-pointers.md:4-5` and its 17-row table |
| N3 | README's boundary-audit counts don't reconcile against the table (19/15/4 claimed vs. 20 rows, 14 literal **fixed**, 1 open, 4 "partial" — a status the summary folds into "open") | `README.md:72-74` vs `docs/internal/audit-boundaries-2026-08-20.md` |
| N4 | README's Tested-Platforms driver range for Turing ("535, 575") omits driver 580, which is explicitly confirmed-tested (with the *stronger* 30-check suite) elsewhere in-tree | `README.md:588` vs `docs/reference/supported-drivers.md:106,116` |
| N5 | `sweep-known-bad-machines.txt` is still a tracked file (readiness doc TODO #7, "soon after," not "before announcing") — unresolved but correctly triaged as non-blocking already | `scripts/sweep-known-bad-machines.txt` in `git ls-files` |
| N6 | `box_is_dead()`'s `DEADCHECK_SETTLE` (still 120s default) — readiness doc TODO #8, explicitly still open and under test elsewhere, not re-investigated here | `scripts/sweep.sh:252` |
| N7 | `sweep.sh`'s remote tree sync (`tar -xzf ... -C /root/nvkvm`) doesn't clear the destination first, so a file deleted from git since a prior run can survive on a reused `--ssh` host and still get globbed into a build | `scripts/sweep.sh:1554`, feeds `build_qemu.sh`'s patch/header globs |
| N8 | Docker `stop`/`start` (not `down`/`up`) can leave a stale `host-libs-<old>` bundle if the host driver was upgraded in between; downstream staging fails loudly rather than mis-staging, so this is a confusing-error UX gap, not corruption | `scripts/container-entrypoint.sh:25` |
| N9 | `docs/investigations/cuinit-999-rented-boxes/README.md:47` cites `scripts/e2e/vecadd.c` as verification; no such tracked path (likely an ad-hoc file on the rented box) | confirmed via `git log --all --full-history` |
| N10 | `docs/investigations/kwin-atomic-modeset/systemsettings-wayland-hang.md` and others correctly cite deleted files (`nvkvm_dispatch.c`, `nvkvm_frontend.c`) with the deletion noted — not a finding, listed only to show it was checked |  |

---

## Explicitly checked and clean

- **"795 fps glmark2"** — appears only in documents that flag it as retracted
  (`known-limitations.md:780`, `quoting-numbers.md:25`, the readiness doc,
  `realapp_matrix.md:239`). Never live in README/SECURITY/docs/reference/
  docs/howto/docker-compose.yml. The unrelated "795 tok/s" vLLM figure
  (`README.md:550`) is a different, correctly-caveated measurement — checked
  so it isn't mistaken for the retracted one.
- **Proton/D3D12/DXVK/VKD3D/Windows-guest** — this repo (`nvkvm-pv`) makes no
  such claim anywhere in the tree; it explicitly says *"Not a Windows guest
  solution. Linux guests only"* (`README.md:86`, `docs/faq.md:50`). The RDR2/
  Proton failure the task describes belongs to the control-allowlist gap
  (S1/B6 area), not to any claim in this repo's own docs.
  `nvkvm-steamos` doesn't claim Proton support either — its games row is
  about native Linux/Vulkan titles only (once B6 is merged).
- **Pre-Turing (Pascal/Maxwell/Volta/Kepler)** — no doc in this repo implies
  support; every mention states the opposite ("Pascal enumerates but `cuInit`
  fails," "Pascal and older do not work"). §4 of the readiness doc's own
  root-cause diagnosis is research, explicitly marked not a launch item.
- **`docker-compose.yml` version pins** — `image:
  ghcr.io/reindertpelsma/nvkvm-pv:v0.2.0`, consistent with README and
  `docs/howto/build.md`. No stale `v0.0.1-rc2`/`v0.1.0`/`:latest`/`:main`
  anywhere in the tree except one clearly-illustrative semver-prerelease
  comment in `.github/workflows/release.yml:120`.
- **`kernel-matrix.yml`'s STRICT_SKIP** — the readiness doc's unresolved TODO
  #11 (does a network failure masquerade as "no kernel headers" on
  `ubuntu:26.04`?) is **resolved**: `tests/kernel_matrix.sh` was fixed today
  (`b5647f2`, "say WHY a row skipped") to distinguish `INSTALL FAILED (not an
  image property)` from a genuine headers gap, and `STRICT_SKIP: "1"` is
  confirmed set in `.github/workflows/kernel-matrix.yml:70`. Actual CI run
  history for `ubuntu:26.04` could not be checked from this offline
  environment — that part of the TODO is still genuinely open, just not for
  the reason originally suspected.
- **`docs/audit/README.md`'s nine security-critical findings from 2026-08-29**
  — spot-checked the two most severe against current `main`: the guest-
  userspace-reachable ring `_IOC_SIZE` stack-smash fix (audit finding #5,
  "remains critical") is present and correct in `src/stub/nvkvm_stub.c:2997-
  3007` (`ioc_sz > sizeof(param)` clamp, PUNT on oversize), landed same-day
  (`699db7b`) and confirmed merged to `main`. The `KILL_ISOLATE`
  authentication gap (#9) was re-rated low by the audit's own second pass
  (guest-kernel-only reachable, self-DoS, one VM) — consistent with
  `SECURITY.md`'s existing "not multi-tenant ready" framing, so not
  independently re-flagged here. The BAR1 VA leak ("the original release
  blocker... untouched") and the guest `ext_lock`/`mmap_lock` AB-BA lock
  ordering are recorded in that doc as genuinely still open, deliberately
  deferred design work, not something this pass re-investigated — they're
  already tracked where they belong.

---

## Fixes applied on this branch

Small, obviously-correct wording fixes only, per the audit's constraints — no
`src/` changes, no restructuring:

1. `README.md:45-52` — Geekbench headline: "of bare metal" → "of host," split
   the RTX-bare-metal / datacenter-nested claim, linked `parity.md` (B1).
2. `README.md:205,237` — added `sudo` to both `setup_guest.sh` quickstart
   invocations, matching `docs/howto/run.md`'s correct form (B2).
3. `README.md:236` — added `--install-deps` to the from-source
   `build_qemu.sh` invocation, matching `CONTRIBUTING.md` (B3).
4. `README.md:241-242` — patch count "nine... 631 lines" → "twelve...
   2271 lines" (actual current count; `ls patches/*.patch | wc -l` = 12,
   `wc -l` sum = 2271).
5. `README.md:246,685` — "three traps" → "the traps" (`CONTRIBUTING.md` now
   has four; a hardcoded count drifts again the next time it changes).

Everything else above is reported, not fixed — B4/B5 need real content
changes (capability flags, linker flags) that risk introducing a second
error if done without the maintainer's review; B6 is a different repo;
S1-S11 and the NOTEs are flagged for the maintainer's own triage.
