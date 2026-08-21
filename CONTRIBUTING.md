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

Run the same thing outside the guest first. Six apparent nvkvm bugs have turned
out to reproduce identically on bare metal — a missing NVML symlink, an EGL
render-node query, a Vulkan compute failure on Hopper, and glamor's pixmap
import among them. Each cost real time before someone checked.

If your workload fails on the host too, the bug is in NVIDIA's driver or in the
application, and we will tell you so. Checking first saves us both a day.

## Working on the code

```bash
bash scripts/build_qemu.sh --install-deps   # isolate stub, then QEMU 9.2
cd tests/unit && make && make run           # see the note below
```

Three things that have bitten people, including us:

- **`build_qemu.sh` skips the build if the binary already exists.** After
  editing anything under `src/qemu/`, re-run it with `--force`, or you will
  spend an afternoon testing your old code. This has happened more than once.
- **`make run` in `tests/unit` exits non-zero today.** `test_isolate` fails 5 of
  its 7 cases at runtime — pre-existing API drift, documented in the Makefile.
  The other six suites pass. Do not "fix" it by deleting the test.
- **A GPU is required for `tests/validate.sh`**, so CI cannot run it. Hardware
  coverage comes from `scripts/sweep_matrix.py` against rented boxes.

## What a good patch looks like

Match the surrounding code, including the comments. This codebase explains
*why* things are the way they are — which driver version changed a struct, which
measurement ruled a theory out, which bug a guard exists to prevent. That is
most of what makes it maintainable, and a patch that only says *what* it does
reads as a regression in the parts that are not code.

If you fix a bug, a line about how it presented is worth more than a line about
the fix. The fix is visible in the diff; the symptom is not.

## Security

Do not file security issues in public. See [`SECURITY.md`](SECURITY.md).

## Licence

Apache-2.0, except `src/guest/` which is GPL-2.0 as required for kernel symbol
access. By contributing you agree your work ships under the licence of the file
you are changing.
