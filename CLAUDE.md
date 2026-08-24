# CLAUDE.md — orientation for LLM agents working in this repository

This file is written for machines with large context windows, not for humans in
a hurry. It is long on purpose. Read it fully before your first edit; the traps
below have each cost this project real hours, several of them more than once,
and most of them do not fail as errors — they fail as *plausible wrong answers*.

Everything here is grounded in something you can check: a file, a line, a commit
or a doc. Where a number appears, its source is named. If you find a claim here
that the tree contradicts, the tree wins — fix this file and say so in the
commit message.

---

## 0. The one-paragraph version

`nvkvm` gives a KVM guest driver-level access to an NVIDIA GPU without handing
over the card. A guest kernel module presents `/dev/nvidia*`, forwards the RM
ioctl surface over virtio to a QEMU device on the host, which validates it
against default-deny allowlists and replays it through a sandboxed per-process
isolate against the real NVIDIA driver. **Control calls cross the boundary; work
does not** — a kernel launch is a store to a mapped doorbell page, which is why
the parity numbers are ratios near 1.00 rather than a fraction of host speed.
Read [`ARCHITECTURE.md`](ARCHITECTURE.md) for the request path end to end.

---

## 1. Orientation: the components, and what to read before touching each

Five components, in the order a request meets them.

| # | component | lives in | read first |
|---|---|---|---|
| 1 | **guest kernel module** (`nvkvm-guest.ko`) — presents the device nodes, sanitises, forwards | `src/guest/` | [device nodes](docs/reference/device-nodes.md), [forwarding model](docs/internal/forwarding-model.md), [guest kernels](docs/reference/guest-kernels.md) |
| 2 | **virtio transport** — virtqueues, shared-memory slots, GPA windows | `src/guest/nvkvm_virtio.c`, `src/qemu/virtio_nvgpu.c`, `src/common/nvkvm_proto.h` | [virtio protocol](docs/reference/virtio-protocol.md) |
| 3 | **QEMU device** — validation, handle/fd translation, the allowlists | `src/qemu/` | [allowlists](docs/reference/allowlists.md), [forwarding model](docs/internal/forwarding-model.md) |
| 4 | **per-process isolate + stub** — a sandboxed host process with its own RM client | `src/qemu/nvkvm_isolate*.c`, `src/stub/` | [isolate model](docs/internal/isolate-model.md), [cross-isolate sharing](docs/internal/cross-isolate-sharing.md) |
| 5 | **host NVIDIA driver** — not ours, and the control in every experiment | — | [correctness](docs/reference/correctness.md) |

Shared contracts sit below all of them:

| | |
|---|---|
| `src/common/nvkvm_abi.h` | the **ABI profile table** — per-driver-version struct sizes/offsets. Eight rows, `:113-235`; selector `nvkvm_abi_id_for_version()` at `:311-381`. |
| `src/common/nvkvm_proto.h` | the guest↔QEMU wire protocol |
| `src/common/nvkvm_isolate_proto.h` | the QEMU↔stub protocol over `SOCK_SEQPACKET` |
| `src/common/nvkvm_ring.h` | header-only SPSC ring for the command-buffer fast path |
| `src/abi/nvgpu.h`, `src/abi/uvm.h` | NVIDIA ABI types, ported from gVisor nvproxy and open-gpu-kernel-modules |

**The three documentation tiers, and which one you are editing.** This matters
because the right edit is opposite in two of them:

- **User-facing** — `README.md`, `SECURITY.md`, `CONTRIBUTING.md`, `docs/faq.md`,
  `docs/howto/*`, and the first half of `docs/reference/*`. The test is *what
  does someone need to know to do this thing and know what to expect*. Prefer a
  table, a command or a link to a paragraph. Move mechanism out and link to it.
- **Internal** — `docs/internal/*`, and the deep reference pages
  (`abi-profiles.md`, `allowlists.md`, `virtio-protocol.md`,
  `device-nodes.md`, `supported-drivers.md`). **Depth is a feature here.**
  `file:line` citations, RM class numbers, ioctl command values, struct
  offsets, long root-cause narratives — all wanted. Do **not** trim these for
  readability or simplify the terminology out of them. What they should get is
  *navigability*: clear headings, a statement at the top of what the file
  covers, and cross-links.
- **Agent-facing** — this file. Same rule as internal: comprehensive beats
  concise.

**Nothing useful gets deleted anywhere.** If material is in the wrong tier,
relocate it and leave a link. If two documents overlap, merge into the deeper
one rather than cutting either. The only thing that should disappear is a claim
that is now *wrong* — and even then, a struck-through line is often better than
a silent removal, because a reader may be holding an old copy.
`docs/internal/mint-guest-desktop.md` does this well.

**The voice is load-bearing.** This project explains *why* things are the way
they are — which driver version changed a struct, which measurement ruled a
theory out, which bug a guard exists to prevent. That is most of what makes it
credible. Lines like *"28/28 is not proof that your workload computes
correctly"* are deliberate. Do not soften an honest limitation, do not add
badges or emoji, and do not flatten a comment that records a failure into a
comment that describes a function.

---

## 2. The rule that has mattered most

> **Before calling anything an nvkvm bug, check whether the bare-metal host does
> the same thing.**

The host is the control, and this project has a working control on almost every
box it rents. The check is usually one `LD_PRELOAD` shim
([`tools/nv_ioctl_trace.c`](tools/nv_ioctl_trace.c)) and five minutes.

**Six apparent bugs were retracted this way** — among them a missing NVML
symlink, an EGL render-node query, glamor's `EGL_NATIVE_PIXMAP_KHR` pixmap
import, and the first two write-ups of the Hopper Vulkan failure. See
`docs/internal/known-limitations.md` ("RETRACTED 2026-08-20") and
`tests/repro/README.md` ("the glamor retraction"), which ends:
*"nvkvm reproduces bare metal faithfully here."*

**Twice it went the other way and confirmed a real bug**, which is what makes
the check worth running rather than a formality:

- `HOPPER_USERMODE_A` — the same Vulkan probe passed on bare metal on *every*
  driver, including the ones the guest failed on. A driver that works on the
  host and fails through nvkvm is an nvkvm bug, by definition.
  ([the sweep](docs/reference/correctness.md#vulkan-compute-on-hopper--root-caused-it-was-ours-and-it-was-never-a-driver-bug))
- NCCL's shareable-handle import — *"Guest-only, host is clean, so this one is
  ours"* (`tests/BOOT_MATRIX.md`). Root cause: a missing fd translation for
  `NV0000_CTRL_CMD_OS_UNIX_GET_EXPORT_OBJECT_INFO` (`0x3d08`).

**The failure mode to avoid is elimination without a control.** The Hopper bug
was published as NVIDIA's twice. Both times the reasoning was *"the only
remaining variable is the host driver"* — which names the **trigger**, not the
culprit. A bug of ours that only some drivers expose fits that evidence
identically. The A/B that produced the wrong verdict was correctly executed; it
just ran every arm on the one driver that could not exhibit the bug. See
[What the earlier 580-only A/B did and did not show](docs/reference/correctness.md#what-the-earlier-580-only-ab-did-and-did-not-show).

---

## 3. Build traps

**`scripts/build_qemu.sh` skips the build if the binary already exists**
(`scripts/build_qemu.sh:69-73`). After editing anything under `src/qemu/`,
`src/common/` or `src/stub/`, re-run with `--force` (`:54`, `:69-76`) or you are
testing your old code. This does not fail as a build error — it surfaces as a
confusing mismatch between new guest code and a stale binary (a new
`NVKVM_HFILE_*` id, for instance, comes back "Invalid argument" from the old
whitelist). `--force` reuses the QEMU tree and the ninja dir, so it is
incremental: minutes, not the full ~20.

```bash
bash scripts/build_qemu.sh --force
```

**The manual copy-and-ninja loop is broken on its own.** Copying `src/qemu/*.c`
back over `hw/misc/` restores the `../../src/common/...` includes that the
script's step 5 rewrote (`build_qemu.sh:275-282`), so the next `ninja` stops at
`fatal error: ../../src/common/nvkvm_proto.h: No such file or directory`. Use
`--force`.

**If you do drive ninja by hand, read its output.** On failure the *old* binary
stays installed at `/opt/qemu-nvkvm/bin/`, and the experiment silently measures
nothing.

**The stub embed is load-bearing and its absence is silent.** Without
`-DNVKVM_STUB_EMBEDDED` and a fresh `nvkvm_stub_bin.h`, QEMU builds *fine* with
`stub_elf = NULL` and falls back to `/usr/lib/nvkvm/nvkvm_stub` at runtime. On a
fresh box that path does not exist, so every isolate device-open returns
`-ENOENT` and the device comes up with **forwarding OFF and nothing saying why**
(`scripts/build_qemu.sh:77-89`).

**Guest module and QEMU graphics settings must match.** `-DNVKVM_QEMU_GRAPHICS=0`
on the QEMU side pairs with `make NVKVM_GRAPHICS=0` on the guest module. The
headers say "deploy the two consistently".

Full walkthrough: [`docs/howto/build.md`](docs/howto/build.md).

---

## 4. Testing: the four gates, and how to not fool yourself with them

Run all four before claiming anything is done. They take about a minute between
them, and CI runs exactly these
([`.github/workflows/ci.yml`](.github/workflows/ci.yml)).

```bash
bash tests/unit/run_tests.sh
bash tests/qemu_syntax_check.sh && CC=gcc-14 bash tests/qemu_syntax_check.sh
cd tests/abi_parity && go test -count=1 ./...
docker run --rm -v "$PWD:/mnt" koalaman/shellcheck:v0.10.0 \
    -S warning scripts/*.sh tests/qemu_syntax_check.sh tests/unit/run_tests.sh
```

| gate | why it exists |
|---|---|
| `tests/unit/run_tests.sh` | on 2026-08-20 `test_isolate` stopped **linking**, `make` aborted on the first error, `test_tables` and `test_open_scm` were never built, and **26 passing assertions silently stopped running** (`ci.yml:8-11`) |
| `tests/qemu_syntax_check.sh`, twice | a forward declaration sat ~1000 lines below its only caller: older compilers warn, gcc-14 makes it a hard error. The only other thing that compiles that file is a 20-minute QEMU build (`ci.yml:13-18`) |
| `go test -count=1` | a wrong ABI row does not fail loudly — it forwards a mis-sized parameter block and the guest gets a plausible wrong answer (`5036b0f`) |
| shellcheck **v0.10.0**, pinned | `scripts/` is how everything gets built and shipped to rented boxes; a quoting bug there fails at hour three of a paid sweep (`ci.yml:20-22`) |

**Use `run_tests.sh`, never `make run`.** `make run` exits non-zero *by design*:
`test_isolate` fails 5 of its 7 cases at runtime on pre-existing API drift
between the test and the isolate table (`tests/unit/Makefile:65-69`). A suite
that is red by default is a suite nobody reads. `run_tests.sh` names those five
explicitly and goes **red** if a sixth fails, if one of the five starts passing
(the drift got fixed — update the file), if any suite regresses, if anything
fails to build, **or if a suite quietly loses assertions** (the counts are
pinned: `test_objects` 20, `test_tables` 17, `test_handle` 11,
`test_nvkms_allowlist` 618, `test_stub_ptr_sanitize` 17, `test_r1_type_dev` 33).
Do not "fix" the five by deleting the test.

Two of those names changed on 2026-08-24 and the counts with them: `test_frontend`
(8) is **gone** and `test_dispatch` (39) is now `test_objects` (20), because both
targeted `src/qemu/nvkvm_dispatch.c` / `nvkvm_frontend.c` — an unreachable pair
that DEAD-1 deleted. A shrinking pinned count is normally the exact failure this
mechanism exists to catch, so the 27 dropped assertions are named individually in
`tests/unit/test_objects.c` and in `src/qemu/virtio_nvgpu.c`'s DEAD-1 note. If you
find a count here that the suite disagrees with, the suite wins.

**`-count=1` on the Go tests is mandatory.** Go's build cache does track the
cgo-included header, but the *test-result* cache is keyed on the test binary, so
a plain `go test` after editing `src/common/nvkvm_abi.h` replays a stale pass.
Observed: breaking the 610 bucket and re-running `go test` reported `ok`.

**Pin the shellcheck version.** A newer shellcheck ships new checks and would
turn the gate red on a day nobody touched a script.

**Re-run the FULL `tests/validate.sh` after any fix — not the part you changed.**
A targeted change once silently regressed 27 unrelated probes. `validate.sh` is
28 checks (6 bring-up, 12 CUDA, 5 Vulkan, 5 GL) and needs a GPU, so CI cannot
run it; hardware coverage comes from `scripts/sweep_matrix.py` against rented
boxes. Exit codes: `0` all pass, `1` a failure, `2` no failures but an
undeclared skip, `3` the harness itself could not run. There is no flag to run a
subset — `--allow-skip <name>` whitelists a skip, it does not filter.

**28/28 is not proof of correctness, and this is not modesty.** A real
correctness bug passed the suite: the guest CPU and the GPU stopped seeing the
same memory, so kernels computed from stale input and *nothing* reported an
error, while `validate.sh` read 28/28 on both sides throughout. What surfaced it
was Geekbench 7 OpenCL failing validation on 11 workloads. See
[correctness.md](docs/reference/correctness.md).

---

## 5. Shell and process traps

**`pkill -f` matches your own command line.** `pkill -f qemu-system-x86` and
`pkill -f unattended-upgrade` appear in the argv of the very `bash -c '...'`
they run inside, so pkill kills its own parent shell and the rest of the command
never runs. This has killed the agent's own session repeatedly. There is a
standing rule against it (`scripts/sweep_matrix.py:446-450`). Use, in order of
preference:

```bash
systemctl stop nvkvm-vm          # best: a named unit
kill "$PID"                      # or an explicit pid you captured
pkill -f '[q]emu-system-x86_64'  # last resort: the bracket stops self-match
```

**`nohup ... &` over ssh dies with the session.** Use a transient unit, which is
what the sweep does (`scripts/sweep_matrix.py:807-809`):

```bash
systemd-run --unit=nvkvm-vm --collect --setenv=VM_MEM=8G --setenv=VM_SMP=4 \
    --working-directory=/root/nvkvm bash scripts/run_test_vm.sh
```

`scripts/run_remote_test.sh:43,66` still uses `nohup` — that is the older
mechanism, not the pattern to copy.

**Heredoc quoting does not nest.** A `<<'EOF'` *inside* an unquoted `<<EOF` is
still expanded by the outer shell. This is a real bug, not a lint nit: in
`scripts/setup_mint_guest.sh` a `<<'RS'` heredoc nested inside an unquoted
`<<EOF` had every `$c`, `$NVCARD` and `$(date)` expanded to empty, so the
generated script never found the NVIDIA card and the compositor fell back to
`bochs-drm` — i.e. llvmpipe, which *looks like it works*. Fixed in `f869d14`,
lost to a merge, restored in `2d0a214`.

**Single quotes do not survive a nested ssh hop either.** `$REMOTE_DIR` inside a
single-quoted remote command block expands on the **remote** shell, where it is
unset, and every path becomes `/scripts/...` (`scripts/run_remote_test.sh:20-22`;
shellcheck caught this one). `scripts/sweep_matrix.py` uses `shlex.quote` for
the same reason (`:217-225`).

**Commands that prompt will "succeed" unattended.** `vastai destroy instance`
prompts `[y/N]`, reads EOF with no tty, prints `Aborted.` — and **exits 0**.
Hence `-y`, and hence `destroy_verified()`, whose only proof is the instance no
longer appearing in `show instances` (`scripts/sweep_matrix.py:720-731`).

---

## 6. Working practice on rented boxes

**Rented boxes have no git credentials and their storage is not durable.** Keep
the authoritative worktree on the control machine, ship trees out, commit back.

**`scripts/sweep_matrix.py` ships the WORKING TREE, not HEAD.** It tars the
working directory (`:957-960`), so uncommitted changes — including uncommitted
*deletions* — are what actually runs on the box you paid for. On 2026-08-20 a
worktree sitting on uncommitted deletions shipped a tree with the UVM 65 fix
reverted while HEAD was two commits past it, and the resulting `cuCtxCreate 999`
was nearly misread as a fresh regression (`:644-657`). `check_tree()` now
refuses to spend money on a dirty tree unless `--allow-dirty` (`:659-691`):

```bash
python3 scripts/sweep_matrix.py --check-tree    # what would actually ship
```

CI cannot cover this: a runner checks out clean by definition, so a long-lived
worktree that has drifted from HEAD can only be caught on the machine about to
ship the tarball.

**Rescuing stranded work.** Use a bundle, and *verify* it — a bundle that does
not record a complete history will restore silently and wrongly:

```bash
git bundle create /tmp/rescue.bundle --all
git bundle verify /tmp/rescue.bundle    # must say "records a complete history"
```

(This one is process lore; unlike everything else in this file it has no
artifact in the tree. If you use it, consider committing the recipe.)

**After merging a long-lived branch, verify the things that branch could not
have known about.** A clean auto-merge is not evidence that nothing was lost:
when both sides touched a file, git resolves in favour of the older side without
conflicting. This has happened twice, and the second time was not caught:

| merge | silently reverted | restored in |
|---|---|---|
| mint-guest | the README trim | `b54b38f` |
| mint-guest | the `.gitignore` `!go.mod` negation, and the nested-heredoc fix | `2d0a214` |
| mint-guest | re-tracked `__pycache__` | `a6ef02b` |
| `4fece85` | the `abi-parity` CI job (`5036b0f`) | *(restored on `docs-polish`)* |
| `4fece85` | the `HOPPER_USERMODE_A` guest-module fix (`0492685`) | **still missing — see §9** |
| `4fece85` | the corrected Hopper write-up in `correctness.md` | *(restored on `docs-polish`)* |

The post-merge checklist, from `2d0a214`'s own commit message plus what
`4fece85` proved was still missing:

```bash
bash tests/unit/run_tests.sh && bash tests/qemu_syntax_check.sh
cd tests/abi_parity && go test -count=1 ./... && cd -
git ls-files | grep __pycache__          # must be empty
git diff <the-merge-that-added-it> HEAD -- src/ .github/ docs/
```

**`.gitignore` gotcha:** `*.mod` matches `go.mod`, which is how
`tests/abi_parity/go.mod` was once dropped during a repo extraction. The
negations `!go.mod` / `!go.sum` at `.gitignore:11-12` restore it. If you fork or
re-extract this tree, check that file survived.

---

## 7. Measurement discipline

This is where the project has most often fooled itself. Every rule below is
paid for; sources are
[`tests/perf/results/glmark2_2026-08-21/RESULTS.md`](tests/perf/results/glmark2_2026-08-21/RESULTS.md)
and `tests/perf/README.md` unless stated.

**Check the clocksource before comparing anything timed.** Under `kvm-clock` the
guest leaves the vDSO fast path and every clock read becomes a real syscall:

| | host | guest (kvm-clock) | guest (tsc) |
|---|---|---|---|
| `clock_gettime(MONOTONIC)` | 49.2 ns | **644.7 ns** | 31.9 ns |
| `gettimeofday` | 35.3 ns | 491.2 ns | — |

NVIDIA's GL driver spin-waits on a *timed* loop and glmark2 times every frame,
so short-frame scenes pay this directly. `clocksource=tsc` moved the full
glmark2 suite from 22685 to 27677 — **+22%**, i.e. 0.73x → 0.89x of host. Half
of an apparent "graphics gap" had nothing to do with the GPU.

**Never quote a single-scene benchmark.** A guest's first pass through a
workload in a process runs at **~0.37x** of steady state (16582 vs ~45000 on
repeat 1 vs later repeats) — roughly 2.7x slower — while every scene after it
runs 0.88–0.93x. A one-shot run measures the cold path and nothing else. An
earlier published "32% graphics gap" was exactly this artifact (`ee593c4`).

**`strace` cannot see vDSO calls, so count nothing — time it instead.** "The host
makes no clock syscalls" and "the host makes 8.5 per sync via the vDSO" look
*identical* under `strace`. (8.5 = 192031 `gettimeofday` + 64026 `clock_gettime`
over 30000 syncs.)

**Do not perturb what you measure.** A 1 Hz `nvidia-smi` poll took
`gl_finishrate` from 10.5 us to 174.8 us — **on the host**.

**Count frames with a counter, not with log lines.** A "~21/s" present rate was
an artifact of counting log messages only some paths emit; a "2.5 fps with
4.8-second freezes" reading was the kernel's printk ratelimiter (291 suppressed
callbacks per 5 s window is itself ~60/s). Both looked like real performance
bugs and neither was.

**Compare a number against the table it summarises before repeating a cause.** A
stated cause was published twice that the underlying numbers did not support.
If a paragraph and a table disagree, the table is the measurement.

**The harness rules, enforced in code** (`tests/perf/README.md`): capture the
host baseline in the *same* run, never against a remembered number; sample
steady state only (util sampled during model *load* faked a "14x gap"); guest
RAM ≥ model size or load goes disk-bound (a guest at `-m 4G` with a 4.4 GB model
faked a 17x gap); check byte-exactness alongside throughput; warm caches before
timing; run host-then-guest strictly serially on one physical GPU.

**Numbers that are superseded and must not be quoted** — the standing list is
`docs/internal/known-limitations.md#numbers-you-should-not-quote`. Currently
also superseded: the **0.12–0.37x multi-GPU serving figures**, which were
measured with `NCCL_SHM_DISABLE=1`. That workaround is no longer needed (fixed
2026-08-21) and the figure is now about **0.86x**
([why](docs/reference/parity.md#multi-gpu-tensor-parallel-serving)).

---

## 8. Domain traps

**Bare UVM ioctl numbers collide with the frontend space.** `65 == 0x41 ==
NV_ESC_RM_IDLE_CHANNELS` (`src/abi/nvgpu.h:891`, explained at
`src/abi/uvm.h:242-245`). `UVM_MAP_DYNAMIC_PARALLELISM_REGION` (65) fell through
to the frontend NR switch and was forwarded for a long time **as a completely
different ioctl** — silently misrouted, not rejected. The disambiguation is a
type gate: the size table refuses anything whose `_IOC_TYPE` is not `'F'`
(`src/guest/nvkvm_ioctl.c:118-131`), matching the sanitiser. If those two ever
disagree again you get the same silent misroute. Tracked as **A-5** in the
[boundary audit](docs/internal/audit-boundaries-2026-08-20.md).

**A class that is reachable but has no size entry gets forwarded as a NULL
parameter block — and the guest then sees a plausible wrong answer rather than
an error.** `libcuda` routinely passes `alloc_parms_size = 0` and lets RM size
the struct by class. If the guest's size-by-`hClass` switch has no case, it
copies zero bytes, the stub forwards `pAllocParms = NULL`, and **RM builds the
object from all-defaults and returns success**. Nothing is denied; no forwarded
ioctl returns non-zero; the damage surfaces layers away.

**This has cost five separate bugs** (`0492685`: *"the FIFTH
class-reachable-but-unsized bug here (#84 twice, #99, #101)"* — plus
`HOPPER_USERMODE_A`), and the full table is in
[Add a driver version §7](docs/howto/add-a-driver-version.md#7-check-the-alloc-class-size-table).
The sharpest instance: `HOPPER_USERMODE_A` (`0xc661`) was on the alloc-class
allowlist (`src/qemu/nvkvm_fe_alloc_allowlist.h:111`) and unsized, so its
`bBar1Mapping` never reached RM, `GET_ADDR_SPACE_TYPE` answered **REGMEM** where
bare metal answers **VIDMEM**, the userspace driver freed the channel it had
just built, and Vulkan died three calls later looking exactly like a driver bug.
It was published as one, twice.

> **So: allowlisted ≠ sized.** If you add a class id to
> `nvkvm_fe_alloc_allowlist.h:59-149`, check both size-by-`hClass` switches in
> `src/guest/nvkvm_main.c` in the same change.

**Do not widen the ctrl or NVKMS allowlist speculatively.** Those gates are what
guards the host — nine of them, six static default-deny tables plus three code
checks, listed in the order an ioctl meets them in
[allowlists.md](docs/reference/allowlists.md). Two agents have correctly refused
to widen, one after *measuring* that the widening did not fix the thing it was
aimed at (the `0x3d08` entry against the NCCL SHM bug, `7571718` — the real
cause was a missing fd translation). The NVIDIA DDX case is the other worked
example: allowing the denied `NVKMS_IOCTL_DECLARE_EVENT_INTEREST` only walks the
DDX one rung further down a ladder that ends at the *host's* physical monitors
(`docs/howto/run.md:341-343`).

A legitimate addition looks like the `0xc36f0101` commit: check what the command
is in OGKM at that driver version, check its params struct for embedded
pointers, and check whether an equivalent is already allowed under another class
id. **An entry with no handler is worse than no entry** — `0x70`
(`NV_ESC_EXPORT_TO_DMABUF_FD`) was removed from the frontend list for exactly
that reason (`src/qemu/nvkvm_fe_alloc_allowlist.h:40-45`).

**Never hand-derive an ABI profile row.** The original 535 row was derived by
arithmetic; three of its five values were wrong (`uvm_map_ext_size` is 1200, not
84), and the error is **silent** — a wrong size does not fail to compile, it
forwards a truncated struct and the kernel reads past the buffer
(`tools/abi_derive.sh:5-12`). Measure with `tools/abi_derive.sh --tags "<tag>"`;
a cell it cannot compile prints `MISSING`, which is a gap to report, never a
cell to fill in from a neighbouring branch.

**A virtual address is not a stable name for a buffer.** Both CPU-memory
migration paths once keyed dedup on a guest VA; free a pinned buffer and
allocate another and libcuda hands back the same address, so the dedup fired
against a mapping belonging to a dead buffer. Result: the GPU read the *previous*
buffer while the guest read its own fresh pages — silent wrong answers, 28/28
throughout. Fixed in `ca6d496` by asking the guest page tables whether the
mapping is still installed.

**Guest-side checks are advisory, host-side checks are the control.** The guest
sanitiser zeroes pointer-carrying fields, but it runs in the guest and a
malicious guest simply skips it. The host boundary overwrites those fields
itself. If you add a check, ask which side it is on.

---

## 9. Known state of this tree (as of `f9f187f`, 2026-08-21)

Two things a fresh agent will otherwise trip over:

**The `HOPPER_USERMODE_A` fix is NOT in the tree.** `4fece85` reverted
`0492685`'s hunk in `src/guest/nvkvm_main.c` — there is no
`nvkvm_alloc_parms_probe_len()` on `main` — while `docs/reference/tested-platforms.md`
still carries the 28/28 rows that were measured *with* it. The diagnosis is
correct and measured; the fix needs restoring before any current Hopper number
is quoted. Confirm with:

```bash
git diff 14e7511 HEAD -- src/guest/nvkvm_main.c
```

**`tests/integration/test_ioctl_fwd` is a compiled binary and is tracked.** That
is the exact class of thing the `ci.yml` "building must not modify tracked
files" gate exists for (`ci.yml:60-78`, added after `tests/unit/test_ctrl_gate`
did the same). It has not tripped the gate yet, but it should be untracked.

---

## 10. Quick reference

```bash
# build (host)
bash scripts/build_qemu.sh --install-deps     # first time
bash scripts/build_qemu.sh --force            # after ANY src/qemu, src/common, src/stub edit

# guest image + launch (host)
sudo bash scripts/setup_guest.sh              # Ubuntu 24.04 cloud image -> /opt/nvkvm-guest
sudo bash scripts/run_test_vm.sh              # VM_MEM=16G VM_SMP=4 QEMU_BIN=... to override
sudo bash scripts/run_test_vm.sh > /tmp/qemu.log 2>&1 &   # ALWAYS capture stdout: DENY lines live only there

# inside the guest (repo is 9p-mounted at /mnt/nvkvm)
sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh   # check the exit status; 2 == incomplete
bash /mnt/nvkvm/tests/validate.sh                  # 28 checks; 0 pass / 1 fail / 2 skip / 3 harness

# the four gates (host, no GPU)
bash tests/unit/run_tests.sh
bash tests/qemu_syntax_check.sh && CC=gcc-14 bash tests/qemu_syntax_check.sh
cd tests/abi_parity && go test -count=1 ./...
docker run --rm -v "$PWD:/mnt" koalaman/shellcheck:v0.10.0 \
    -S warning scripts/*.sh tests/qemu_syntax_check.sh tests/unit/run_tests.sh

# hardware sweep (spends money -- check the tree first)
python3 scripts/sweep_matrix.py --check-tree
```

**Environment variables worth knowing**

| var | where | effect |
|---|---|---|
| `NVKVM_DEBUG=1` | QEMU | verbose per-operation tracing. Errors and `DENY` print either way. |
| `NVKVM_ISOLATE_MODE` | QEMU | `auto` (default) / `namespace` / `uid+chroot` / `uid` / `seccomp` / `none` |
| `NVKVM_PRESENT_MODE` | QEMU | `gl` (zero-copy) / `readback`; unset ⇒ auto, currently always `readback` |
| `NVKVM_QEMU_UI=1` | build | build the GTK/SDL window backends (off by default; headless is the normal deployment) |
| `NVKVM_QEMU_PREFIX`, `NVKVM_QEMU_SRC` | build | relocate the install / source tree (rootless builds) |
| `NVKVM_STAGE_XORG=0` | guest staging | leave `/etc/X11/xorg.conf` alone |
| `VM_MEM`, `VM_SMP`, `QEMU_BIN` | `run_test_vm.sh` | default `16G`, `4`, `/opt/qemu-nvkvm/bin/qemu-system-x86_64` |

**Diagnostics you will see, and what they mean** — the full table is
[`docs/howto/run.md`](docs/howto/run.md#5-check-it-worked). The two most
misleading:

| symptom | actual cause |
|---|---|
| `open ctl/gpu FAILED r1=-2 r2=-2 — forwarding OFF` | the stub was not embedded — `build_qemu.sh --force` |
| everything works but `GL_RENDERER` says `llvmpipe` | a silent software fallback. **Always confirm the renderer string** before believing a graphics result. |

---

## 11. What a good commit looks like here

Match the surrounding code, including the comments. This codebase records *why*
— which driver version changed a struct, which measurement ruled a theory out,
which bug a guard exists to prevent. A patch that only says *what* it does reads
as a regression in the parts that are not code.

If you fix a bug, a line about **how it presented** is worth more than a line
about the fix. The fix is visible in the diff; the symptom is not.

If you retract a claim, say what the old claim was and why it was wrong, rather
than quietly replacing it. Two of the most useful passages in this repository
are retractions written that way (`0492685`, and the "RETRACTED 2026-08-20"
entries in `known-limitations.md`). Do not guess at a cause you have not
measured — *"guessing is what produced the two previous versions of this claim"*.

Do not commit or push unless asked. If you are on the default branch, branch
first.
