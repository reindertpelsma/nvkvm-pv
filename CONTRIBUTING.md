# Contributing

This is a solo research project. Issues, measurements and patches are all
welcome; none of it has a response SLA.

## The most useful thing you can send

**A platform report.** Coverage of GPUs and driver branches is a function of
what someone happened to rent, so hardware we do not have is worth more than
almost any patch. There is an
[issue template](.github/ISSUE_TEMPLATE/platform_report.yml) for it, and the
boring "it just worked" reports count — they are how the tested-platforms table
gets rows.

**A failure is worth more than a success.** If something breaks on your card or
your driver, that is the report we want most.

## Before you file a bug: check the host

Run the same thing outside the guest first, with the same binary. **Six**
apparent nvkvm bugs have turned out to reproduce identically on bare metal — a
missing NVML symlink, an EGL render-node query, and glamor's `EGL_NATIVE_PIXMAP_KHR`
pixmap import among them. Each cost real time before someone checked.

The reverse has happened twice, and it is why the check is worth doing rather
than a formality: bare metal **passing** is what identified `HOPPER_USERMODE_A`
and NCCL's shareable-handle import as genuinely ours. A driver that works on the
host and fails through nvkvm is an nvkvm bug, by definition.

If your workload fails on the host too, the bug is in NVIDIA's driver or in the
application, and we will tell you so. Checking first saves us both a day.

## Working on the code

```bash
bash scripts/build_qemu.sh --install-deps   # isolate stub, then QEMU 9.2
bash tests/unit/run_tests.sh                # the unit gate -- see the note below
```

Four things that have bitten people, including us:

- **`build_qemu.sh` skips the build if the binary already exists**
  (`scripts/build_qemu.sh:69-73`). After editing anything under `src/qemu/`,
  `src/common/` or `src/stub/`, re-run it with `--force`, or you will spend an
  afternoon testing your old code. This has happened more than once, and it does
  not fail as a build error — it surfaces as a confusing mismatch between new
  guest code and an old binary.
- **Run `tests/unit/run_tests.sh`, not `make run`.** `make run` exits non-zero
  by design: `test_isolate` fails 5 of its 7 cases at runtime on pre-existing API
  drift, documented at `tests/unit/Makefile:65-69`. Do not "fix" it by deleting
  the test. `run_tests.sh` names those five explicitly, so the suite is green
  when exactly those fail and red for anything else — including a suite that
  fails to build, which plain `make` hides by aborting on the first error.
- **Re-run the whole suite after a fix, not the part you changed.** A targeted
  change here once silently regressed 27 unrelated probes. The four gates below
  take about a minute between them.
- **A GPU is required for `tests/validate.sh`**, so CI cannot run it. Hardware
  coverage comes from `scripts/sweep_matrix.py` against rented boxes.

### The four gates

CI runs exactly these ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)),
and they are the same locally:

```bash
bash tests/unit/run_tests.sh
bash tests/qemu_syntax_check.sh && CC=gcc-14 bash tests/qemu_syntax_check.sh
cd tests/abi_parity && go test -count=1 ./...        # -count=1 is mandatory
docker run --rm -v "$PWD:/mnt" koalaman/shellcheck:v0.10.0 \
    -S warning scripts/*.sh tests/qemu_syntax_check.sh tests/unit/run_tests.sh
```

The shellcheck version is pinned on purpose: a newer one ships new checks and
would turn the gate red on a day nobody touched a script. `-count=1` is
mandatory because Go's test-result cache is keyed on the test binary, so a plain
`go test` after editing `src/common/nvkvm_abi.h` can replay a stale pass.
Details for each: [`docs/howto/build.md`](docs/howto/build.md#the-four-gates-that-need-no-gpu).

## What a good patch looks like

Match the surrounding code, including the comments. This codebase explains
*why* things are the way they are — which driver version changed a struct, which
measurement ruled a theory out, which bug a guard exists to prevent. That is
most of what makes it maintainable, and a patch that only says *what* it does
reads as a regression in the parts that are not code.

If you fix a bug, a line about how it presented is worth more than a line about
the fix. The fix is visible in the diff; the symptom is not.

## After merging a long-lived branch

Check the things that branch could not have known about. Twice now a **clean
auto-merge** — no conflicts, nothing to resolve — has silently reverted work
made on the other side after the branch point: a README trim, a `.gitignore`
`!go.mod` negation, a nested-heredoc escaping fix in
`scripts/setup_mint_guest.sh` (`2d0a214`), and later an `abi-parity` CI job and
a guest-module fix (`4fece85`). Git resolved in favour of the older side because
both sides had touched the file.

So after any such merge: re-run the four gates, `git ls-files | grep __pycache__`,
and diff against the merge that introduced whatever the branch was long enough
to have missed.

## If you are an LLM agent working in this repository

Read [`CLAUDE.md`](CLAUDE.md) first. It is the orientation and trap list —
component map, which document to read before touching what, and the build,
measurement and domain traps that have each cost this project real time.

## The dev VM harness is not a sandbox

`scripts/run_test_vm.sh` and `scripts/run_remote_test.sh` are a **development
harness only**. Point them at a guest you trust completely, and nothing else.

`run_test_vm.sh` exports the whole repository to the guest over 9p
**read-write**:

```
-virtfs local,path="$REPO_ROOT",mount_tag=nvkvm_src,security_model=mapped
```

The guest mounts that at `/mnt/nvkvm` and *builds its kernel module on it*,
which is why the export is writable and why making it read-only is not a
one-line change — the build would have to move to a guest-local copy or an
overlay first.

The consequence is a direct guest-root → host-root path, and it is short:

1. guest root edits any file under `/mnt/nvkvm` — say `scripts/run_test_vm.sh`;
2. `scripts/run_remote_test.sh restart` runs
   `bash $REMOTE_DIR/scripts/run_test_vm.sh` **on the host, as root**;
3. done.

`security_model=mapped` does not help here. It maps guest ownership and mode
bits into host xattrs; it does not make the export read-only, and the host-side
files stay owned by the (root) user running QEMU.

This is a property of the harness, not of nvkvm's boundary — the VMM's own
guest→host boundary is the thing the rest of this project is about, and it does
not depend on 9p. But do not benchmark, fuzz, or demo against an untrusted guest
image with this harness, and do not leave it running on a shared machine.

If you need a harness that survives an untrusted guest: drop the writable repo
export, ship the built artefacts in instead (a read-only export, a virtio-blk
image, or `scp` into the guest), and stop having any host-side script execute a
path the guest can write.

## Security

Do not file security issues in public. See [`SECURITY.md`](SECURITY.md).

## Licence

Apache-2.0, except `src/guest/` which is GPL-2.0 as required for kernel symbol
access. By contributing you agree your work ships under the licence of the file
you are changing.
