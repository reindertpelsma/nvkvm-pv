#!/usr/bin/env python3
"""Pin guest-side ordering that a compile-only kernel matrix cannot see."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def section(path: str, start: str, end: str) -> str:
    text = (ROOT / path).read_text(encoding="utf-8")
    begin = text.index(start)
    finish = text.index(end, begin)
    return text[begin:finish]


def ordered(text: str, *needles: str) -> None:
    pos = -1
    for needle in needles:
        pos = text.index(needle, pos + 1)


ioctl = section(
    "src/guest/nvkvm_main.c",
    "static long nvkvm_ioctl(",
    "/* ── mmap ",
)
ordered(
    ioctl,
    "mutex_lock(&ctx->uvm_state->ext_lock);",
    "nvkvm_uvm_shadow_prepare(ctx",
    "nvkvm_virtio_ioctl_on_isolate(ctx",
    "nvkvm_uvm_record_response(ctx",
    "mutex_unlock(&ctx->uvm_state->ext_lock);",
)

mmap = section(
    "src/guest/nvkvm_mmap.c",
    "int nvkvm_mmap_request(",
    "/*\n * UVM realize path",
)
ordered(
    mmap,
    "mutex_lock(&ctx->uvm_state->ext_lock);",
    "nvkvm_uvm_mmap_intent(ctx",
    "nvkvm_uvm_ext_mmap(ctx",
    "mutex_unlock(&ctx->uvm_state->ext_lock);",
)
ordered(
    mmap,
    "nvkvm_uvm_mmap_intent(ctx",
    "nvkvm_mmap_request_isolate(ctx",
    "mutex_unlock(&ctx->uvm_state->ext_lock);",
)

ext_mmap = section(
    "src/guest/nvkvm_uvm_ext.c",
    "int nvkvm_uvm_ext_mmap(",
    "bool nvkvm_uvm_ext_covers(",
)
assert "lockdep_assert_held(&st->ext_lock);" in ext_mmap
assert "mutex_lock(&st->ext_lock)" not in ext_mmap

# REGISTER_GPU's embedded rmCtrlFd is translated on every live transport and
# the corrected 40-byte ABI is shared with the dormant REALIZE replay.  A
# missing one of these silently drops the fallback's GPU list or forwards a
# guest fd number into host RM.
sanitize = section(
    "src/guest/nvkvm_ioctl.c",
    "int nvkvm_sanitize_ioctl_params(",
    "\t/*\n\t * The NR-based switch below",
)
assert "case UVM_REGISTER_GPU:" in sanitize
assert "p->rm_ctrl_fd = hid;" in sanitize

qemu_isolate = (ROOT / "src/qemu/nvkvm_isolate_handlers.c").read_text(
    encoding="utf-8"
)
assert "NVKVM_UVM_REGISTER_GPU_SIZE" in qemu_isolate
assert "NVKVM_UVM_REGISTER_GPU_FD_OFF" in qemu_isolate

stub = (ROOT / "src/stub/nvkvm_stub.c").read_text(encoding="utf-8")
assert "case NVKVM_STUB_UVM_REGISTER_GPU:" in stub
assert "UVM_REGISTER_GPU_PARAMS rmCtrlFd moved" in stub
assert "#define NVKVM_PROTO_VERSION     3" in proto
assert "REALIZE REGISTER_GPU replay size drifted" in stub

kms_init = section(
    "src/guest/nvkvm_kms.c",
    "int nvkvm_kms_init(",
    "/* Called immediately after drm_dev_register() succeeds. */",
)
assert "ddev->dev_private = kms;" in kms_init
assert "nvkvm_kms_head = kms;" not in kms_init

kms_activate = section(
    "src/guest/nvkvm_kms.c",
    "void nvkvm_kms_activate(",
    "/* Stop every path",
)
assert "struct nvkvm_kms *kms = ddev->dev_private;" in kms_activate
assert "nvkvm_kms_head = kms;" in kms_activate

setup_guest = (ROOT / "scripts/setup_guest.sh").read_text(encoding="utf-8")
module_unit = setup_guest[setup_guest.index("- path: /etc/systemd/system/nvkvm-guest.service"):
                          setup_guest.index("packages:", setup_guest.index(
                              "- path: /etc/systemd/system/nvkvm-guest.service"))]
ordered(
    module_unit,
    "mktemp -d /var/tmp/nvkvm-guest.XXXXXX",
    'mkdir -p \"$work/src\"',
    "cp -a /mnt/nvkvm/src/guest /mnt/nvkvm/src/common /mnt/nvkvm/src/abi",
    'make -C \"$work/src/guest\" KDIR=/lib/modules/$(uname -r)/build clean',
    'make -C \"$work/src/guest\"',
    'insmod \"$work/src/guest/nvkvm-guest.ko\"',
)
assert "cd /mnt/nvkvm/src/guest && make" not in module_unit

print("guest_wiring_test: PASS")
