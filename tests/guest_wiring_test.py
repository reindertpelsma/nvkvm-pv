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

print("guest_wiring_test: PASS")
