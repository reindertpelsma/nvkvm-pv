/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nvkvm_ring_ioctl.h — on-ring record format for the command-buffer fast path.
 *
 * The generic SPSC primitive lives in nvkvm_ring.h; this header defines the
 * PAYLOAD carried inside an NVKVM_RING_REC_DATA record for the ioctl fast path
 * (docs/design/command_buffer.md).  Shared verbatim by the guest kernel module
 * (producer of requests / consumer of responses) and the freestanding isolate
 * stub (consumer of requests / producer of responses).
 *
 * A request record payload is:   struct nvkvm_ring_ioctl_req  + param[] + aux[]
 * A response record payload is:  struct nvkvm_ring_ioctl_resp + param[] + aux[]
 *
 * Only "flat" RM_CONTROLs (nvos54, no embedded inner pointers/fds) ride the
 * ring — the hot decode-poll controls.  The stub treats every record as
 * HOSTILE: it copies the payload out, validates sizes against caps, and if the
 * control needs the per-control marshalling the slow path does (InfoList,
 * GET_BUILD_VERSION, EXPORT_OBJECT_TO_FD, …) it does NOT execute it — it sets
 * NVKVM_RING_RESP_PUNT so the guest re-issues on the virtqueue.  PUNT means
 * "not executed", so the fallback never double-runs a side-effecting control.
 *
 * The includer must provide the fixed-width integer types (stdint.h / the
 * stub's freestanding header / linux/types.h).
 */
#ifndef NVKVM_RING_IOCTL_H
#define NVKVM_RING_IOCTL_H

/* Size caps — a control that exceeds either stays on the virtqueue path.
 * nvos54 is 32 bytes; flat control inner params are small.  Bound the stub's
 * per-record stack scratch (param+aux must fit a record in a 64 KiB ring). */
#define NVKVM_RING_MAX_PARAM 256u
#define NVKVM_RING_MAX_AUX   4096u

/* response flags */
#define NVKVM_RING_RESP_PUNT 0x1u   /* stub did NOT execute; use the slow path */

/* Request payload (guest → isolate), follows nvkvm_ring_rec{len,type=DATA}. */
struct nvkvm_ring_ioctl_req {
	uint32_t txn_id;
	uint32_t handle_id;
	uint32_t cmd;          /* ioctl request code (NV_ESC_RM_CONTROL)        */
	uint32_t param_size;   /* bytes of param blob (nvos54, <= MAX_PARAM)    */
	uint32_t aux_size;     /* bytes of inner-params blob (<= MAX_AUX)       */
	uint32_t abi_profile;  /* nvkvm_abi_id of the host driver (offsets)     */
	/* uint8_t param[param_size]; then uint8_t aux[aux_size]; */
};

/* Response payload (isolate → guest), follows nvkvm_ring_rec{len,type=DATA}. */
struct nvkvm_ring_ioctl_resp {
	uint32_t txn_id;
	int32_t  retval;       /* 0 or -errno (ioctl return)                    */
	uint32_t nvstatus;     /* NvStatus from the params struct               */
	uint32_t param_size;   /* bytes of updated param blob                   */
	uint32_t aux_size;     /* bytes of updated aux blob                     */
	uint32_t flags;        /* NVKVM_RING_RESP_*                             */
	/* uint8_t param[param_size]; then uint8_t aux[aux_size]; */
};

#endif /* NVKVM_RING_IOCTL_H */
