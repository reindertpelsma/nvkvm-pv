# Nested L2 pinned-sysmem zeros — what's still missing

Status 2026-08-30. Findings are written up in
`docs/investigations/nested-nvkvm-l2/ZEROS.md`; this file is only the
still-open measurement.

## Corrected: the oops is NOT the suspect

An earlier revision of this file named a kernel oops in the L2 guest as the
prime suspect and said the root cause should not be written up until that trace
existed. **The trace was captured, and it exonerates the oops.** It is:

    WARNING: CPU: 1 PID: 2442 at mm/memory.c:5514 handle_mm_fault+0x327/0x380
    Comm: nvz2   CR2: 00005dd31fb6e000
    nvz2[2442]: segfault at 5dd31fb6e000 ... error 7

`0x5dd31fb6e000` is the address probe step **P3** (`cuMemHostRegister` + kernel
dereference) printed for its own buffer, and **the same probe crashes
identically at L1** — `EXIT=139` at `[P3]` on the control run. So it is a
pre-existing `cuMemHostRegister` fault, present at both levels, that merely
happened to log next to the failure. It is not nesting-specific and it is not
the zeros. Adjacency, not causation.

(That `cuMemHostRegister` crash is real and worth its own ticket. It is not this
one.)

## The one measurement still open

Which of the two sysmem-aliasing mechanisms breaks, and confirmation of the
proposed mechanism for the OS_DESCRIPTOR path. The instrument is already
committed on `fix/nested-l2-htod-zeros`: `migrate_range` now prints
`comm/pid/gva/file/magic/vmflags`.

    # rebuild L1's guest module from this branch, boot L1 and L2, then at L1:
    sudo dmesg | grep 'DIAG: migrate_range'

Confirms the mechanism if, during an **L2** CUDA run, L1 logs migrations whose
`comm` is the isolate stub and whose VMA is `file=1 magic=0x1021994` (TMPFS) —
i.e. L1 is relocating a memfd that L1's own QEMU is simultaneously exposing to
L2 as guest RAM. In the flat case the same line should read `file=0` (plain
anonymous memory belonging to the CUDA process itself).

Note `dmesg` needs `sudo` in both guests, and both levels must be booted, which
takes several minutes.
