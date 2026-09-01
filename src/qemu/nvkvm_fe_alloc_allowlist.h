/*
 * nvkvm_fe_alloc_allowlist.h — default-deny frontend-ioctl + RM_ALLOC class
 * allowlists (nvproxy parity, companion to nvkvm_ctrl_allowlist.h / #76).
 *
 * Frontend NRs (23): gVisor nvproxy 575-ABI frontendIoctl set (22) + the two it
 *   adds through v570. Covers our empirically-observed set.  NV_ESC_EXPORT_TO_
 *   DMABUF_FD (0x70) was an nvkvm-only extension here and has since been REMOVED
 *   (audit G-7, see the note in the table below) — do not read this line as
 *   saying it is present.
 * Alloc classes (89): nvproxy 575-ABI RM_ALLOC class set (87) — already a
 *   superset of our empirically-observed CUDA classes — plus two nvkvm
 *   additions, each annotated at its row: NV01_EVENT (0x5) and AMPERE_B
 *   (0xc797).  nvproxy deliberately omits privileged memory (0x3f),
 *   OS_DESCRIPTOR (0x71) and bare NV01_EVENT (0x5); we keep the first two
 *   omissions but NOT the third — 0x5 is allowed here for graphics/compute
 *   completion events (#84).
 *
 * Both gates live in QEMU (the guest module is untrusted). Unknown NR / class is
 * DENIED. Provenance: docs/audits/nvproxy_frontend_alloc.md.
 */
#ifndef NVKVM_FE_ALLOC_ALLOWLIST_H
#define NVKVM_FE_ALLOC_ALLOWLIST_H
#include <stdint.h>

static const uint8_t nvkvm_fe_nr_allowlist[] = {
	0x27,
	0x29,
	0x2a,
	0x2b,
	0x34,
	0x35,
	0x41,
	0x4a,
	0x4e,
	0x4f,
	0x54,
	0x57,
	0x58,
	0x5e,
	/* 0x70 NV_ESC_EXPORT_TO_DMABUF_FD removed (audit G-7): it is allowlisted
	 * but unhandled — the stub would create a real dma-buf fd that nothing
	 * closes and nothing passes back, leaking a stub fd per call
	 * (self-isolate fd-exhaustion) and returning a meaningless fd to the
	 * guest.  Re-add WITH fd passback + teardown tracking at the dma-buf
	 * present milestone (docs/design/virtual_modeset.md). */
	0xc8,
	0xc9,
	0xce,
	0xcf,
	0xd2,
	0xd4,
	0xd6,
	0xd7,
	0xda,
};
#define NVKVM_FE_NR_ALLOWLIST_N \
	(sizeof(nvkvm_fe_nr_allowlist) / sizeof(nvkvm_fe_nr_allowlist[0]))

static const uint32_t nvkvm_alloc_class_allowlist[] = {
	0x00000000u,
	0x00000001u,
	0x00000002u,
	0x00000005u,  /* NV01_EVENT (graphics/compute completion events) — #84 */
	0x0000003eu,
	0x00000040u,
	0x00000041u,
	0x00000070u,
	0x00000073u,
	0x00000079u,
	0x00000080u,
	0x000000dau,
	0x000000deu,
	0x000000e0u,
	0x000000f1u,
	0x000000f8u,
	0x000000fbu,
	0x000000fdu,
	0x000000feu,
	0x00002080u,
	0x00002081u,
	0x0000208fu,
	0x0000503bu,
	0x0000503cu,
	0x000050a0u,
	0x000083deu,
	0x0000902du,
	0x00009067u,
	0x00009072u,
	0x00009096u,
	0x000090ccu,
	0x000090e6u,
	0x000090e7u,
	0x000090f1u,
	0x0000a06cu,
	0x0000a0bcu,
	0x0000a140u,
	0x0000b2ccu,
	0x0000b8b0u,
	0x0000b8d1u,
	0x0000b8fau,
	0x0000c06fu,  /* PASCAL_CHANNEL_GPFIFO_A -- pre-Turing, not in nvproxy */
	0x0000c097u,  /* PASCAL_A */
	0x0000c0b5u,  /* PASCAL_DMA_COPY_A */
	0x0000c0c0u,  /* PASCAL_COMPUTE_A */
	0x0000c1b5u,  /* PASCAL_DMA_COPY_B -- MEASURED 12x DENY once 0xc06f was fixed */
	0x0000c197u,  /* PASCAL_B */
	0x0000c1c0u,  /* PASCAL_COMPUTE_B */
	0x0000c361u,
	0x0000c461u,
	0x0000c46fu,
	0x0000c4b0u,
	0x0000c4b7u,
	0x0000c4d1u,
	0x0000c56fu,
	0x0000c597u,
	0x0000c5b5u,
	0x0000c5c0u,
	0x0000c661u,
	0x0000c697u,
	0x0000c6b0u,
	0x0000c6b5u,
	0x0000c6c0u,
	0x0000c6fau,
	0x0000c761u,
	0x0000c797u,  /* AMPERE_B (GA10x 3D/graphics class, e.g. RTX 3060) — #84 */
	0x0000c7b0u,
	0x0000c7b5u,
	0x0000c7b7u,
	0x0000c7c0u,
	0x0000c7fau,
	0x0000c86fu,
	0x0000c8b5u,
	0x0000c96fu,
	0x0000c997u,
	0x0000c9b0u,
	0x0000c9b5u,
	0x0000c9b7u,
	0x0000c9c0u,
	0x0000c9d1u,
	0x0000c9fau,
	0x0000ca6fu,
	0x0000cab5u,
	0x0000cb33u,
	0x0000cb97u,
	0x0000cba2u,
	0x0000cbc0u,
	0x0000cd40u,
	0x0000cd97u,
	0x0000cdb0u,
	0x0000cdc0u,
	0x0000cdd1u,
	0x0000cdfau,
	0x0000ce97u,
	0x0000cec0u,
	0x0000cfb7u,
};
#define NVKVM_ALLOC_CLASS_ALLOWLIST_N \
	(sizeof(nvkvm_alloc_class_allowlist) / sizeof(nvkvm_alloc_class_allowlist[0]))

#endif /* NVKVM_FE_ALLOC_ALLOWLIST_H */
