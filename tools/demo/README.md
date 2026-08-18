# Demo recordings

Tooling to record an nvkvm boot as a terminal animation — the GPU showing up
inside a guest, from cold host to `nvidia-smi` in the VM.

Two pieces, deliberately split:

| | runs where | needs |
|---|---|---|
| `record_demo.sh` | on the GPU host | `script(1)` only |
| `termcast.py` | anywhere (your laptop, CI) | `pyte` + `pillow` for GIF; nothing for `.cast` |

The GPU host stays clean: nothing is installed on it to make a demo.

## Record

```bash
# on the host, with the test VM prerequisites already in place
tools/demo/record_demo.sh                 # -> /tmp/nvkvm-demo.{typescript,timing,geom}
```

The default scenario prints the host GPU, boots the test VM with
`scripts/run_test_vm.sh`, follows the serial console until the guest answers
SSH, then inside the guest shows `/dev/nvidia*`, `nvidia-smi`, and the Vulkan
device name — the same physical GPU, with the host still using it.

Record something else instead:

```bash
tools/demo/record_demo.sh -o /tmp/mine -- 'tests/validate.sh'
```

## Render

```bash
scp host:/tmp/nvkvm-demo.* .

# asciinema v2 cast (upload to asciinema.org, or play with asciinema play)
tools/demo/termcast.py cast nvkvm-demo -o boot.cast --max-idle 1.0 --speed 2

# animated GIF (what a README can embed)
tools/demo/termcast.py gif nvkvm-demo -o docs/img/boot.gif --max-idle 1.0 --speed 2
```

### Fast-forwarding

A VM boot is mostly waiting, so the timeline is compressed rather than cut:

- `--max-idle S` caps any single pause at `S` seconds. This is what turns a
  two-minute boot into a watchable clip, and it never drops output — only
  dead air shrinks.
- `--speed N` divides every remaining delay by `N`.
- `--trim-head S` drops the lead-in before the first byte.

Both subcommands take these, so the `.cast` and the GIF stay in sync.

### GIF size

Frames are only emitted when the screen actually changes, sampled at
`--fps` (default 10). If the result is too heavy: lower `--fps`, drop
`--font-size`, or narrow the recording (`NVKVM_DEMO_COLS=90`). `--max-frames`
is a hard stop and prints how much it truncated rather than silently cutting.

Requirements for rendering:

```bash
pip install pyte pillow
```
