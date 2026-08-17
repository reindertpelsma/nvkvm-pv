/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_isolate_proto.h — QEMU ↔ isolate stub protocol
 *
 * Communication over a SOCK_SEQPACKET unix socket pair.
 * All messages are fixed-size structs preceded by a 4-byte type field.
 * Variable-length data (ioctl params) is sent as ancillary in-band data
 * in a follow-up write; the receiver knows how many bytes to expect from
 * the fixed header.
 *
 * File descriptor transfer uses SCM_RIGHTS on RECEIVE_FD messages.
 *
 * QEMU never trusts the isolate: every response value is range-checked.
 */

#ifndef NVKVM_ISOLATE_PROTO_H
#define NVKVM_ISOLATE_PROTO_H

#include <stdint.h>

/*
 * NVKMS wrapper ioctl on /dev/nvidia-modeset (NVKVM_DEV_MODESET).  Mirrors the
 * defines in nvkvm_proto.h, duplicated here because the freestanding stub and
 * QEMU include only this header.  _IOWR('m', 0, struct NvKmsIoctlParams) where
 * NvKmsIoctlParams = { u32 cmd; u32 size; u64 address }; the single embedded
 * user pointer `address` (offset 8) points at `size` bytes of inner params.
 */
#define NVKVM_NVKMS_IOCTL_CMD   0xC0106D00u
#define NVKVM_NVKMS_PARAMS_SIZE 16u
#define NVKVM_NVKMS_ADDR_OFF    8u

/* NVKMS REGISTER_SURFACE (sub-cmd 17): inner params embed up to 3 plane fds
 * (useFd=TRUE) that need handle_id→stub-fd translation.  Layout mirrors
 * nvkvm_proto.h — keep in sync. */
#define NVKVM_NVKMS_CMD_REGISTER_SURFACE 17u
#define NVKVM_NVKMS_REGSURF_USEFD_OFF    4u
#define NVKVM_NVKMS_REGSURF_PLANE0_OFF   16u
#define NVKVM_NVKMS_REGSURF_PLANE_STRIDE 32u
#define NVKVM_NVKMS_MAX_PLANES           3u

/* ── Command types (QEMU → isolate) ─────────────────────────────────────── */

#define ISOLATE_CMD_RECEIVE_FD   1   /* fd arrives via SCM_RIGHTS            */
#define ISOLATE_CMD_CLOSE_FD     2   /* close fd for handle_id               */
#define ISOLATE_CMD_IOCTL        3   /* fd + cmd + data                      */
#define ISOLATE_CMD_MMAP         4   /* fd + gva + len + prot + flags+offset */
#define ISOLATE_CMD_MUNMAP       5   /* gva + len                            */
#define ISOLATE_CMD_POLL         6   /* fd + events; start background poll   */
#define ISOLATE_CMD_UNPOLL       7   /* fd; stop background poll             */
#define ISOLATE_CMD_EXIT         8   /* clean shutdown                       */
#define ISOLATE_CMD_OPEN_DEVICE  9   /* stub opens /dev/nvidia*, replies w/ SCM_RIGHTS fd */
#define ISOLATE_CMD_REALIZE_UVM_FD 10 /* full UVM realize: init+register+intent+mmap */
#define ISOLATE_CMD_INTERRUPT    11   /* post SIGUSR1 to the worker on txn_id  */
#define ISOLATE_CMD_SETUP_RING   12   /* mint per-isolate SPSC ring pair; memfd via SCM_RIGHTS */
#define ISOLATE_CMD_ENTER_LOOP   13   /* drive the SPSC consumer loop until idle */
#define ISOLATE_CMD_PRESENT_EXPORT 14 /* PRIME_HANDLE_TO_FD a GEM; reply w/ dma-buf via SCM */
#define ISOLATE_CMD_XISO_IMPORT  15   /* #110 cross-isolate: PRIME_FD_TO_HANDLE a dma-buf
				       * (arrives via SCM_RIGHTS) → reply w/ GEM handle */

/* ── Response types (isolate → QEMU) ────────────────────────────────────── */

#define ISOLATE_RESP_OK          0x10  /* generic success                    */
#define ISOLATE_RESP_ERROR       0x11  /* generic error                      */
#define ISOLATE_RESP_IOCTL       0x12  /* ioctl result + optional data       */
#define ISOLATE_RESP_MMAP        0x13  /* mmap result                        */
#define ISOLATE_RESP_POLL_EVENT  0x14  /* async: fd became ready             */
#define ISOLATE_RESP_OPEN_DEVICE 0x15  /* open result + fd via SCM_RIGHTS    */
#define ISOLATE_RESP_REALIZE_UVM 0x16  /* realize result: host VA + rmStatus */
#define ISOLATE_RESP_RING_READY  0x17  /* SPSC ring mapped + self-test echo  */
#define ISOLATE_RESP_LOOP_EXITED 0x18  /* consumer loop drained + idled out  */
#define ISOLATE_RESP_PRESENT_EXPORT 0x19 /* present export result + dma-buf via SCM */
#define ISOLATE_RESP_XISO_IMPORT 0x1a    /* #110 cross-isolate import result + GEM handle */

/* ── RECEIVE_FD ──────────────────────────────────────────────────────────── */

struct isolate_cmd_receive_fd {
	uint32_t type;        /* ISOLATE_CMD_RECEIVE_FD */
	uint32_t handle_id;   /* key to store this fd under */
	uint32_t dev_id;      /* NVKVM_DEV_* — lets the stub swap in a
	                       * locally-opened fd for UVM, whose file
	                       * ownership must match the calling mm. */
	uint32_t reserved;    /* keep struct 8-byte aligned for future fields */
};

/* ── CLOSE_FD ────────────────────────────────────────────────────────────── */

struct isolate_cmd_close_fd {
	uint32_t type;        /* ISOLATE_CMD_CLOSE_FD */
	uint32_t handle_id;
};

/* ── IOCTL ───────────────────────────────────────────────────────────────── */

/*
 * IOCTL is the only command executed by the isolate's thread pool.
 * txn_id is assigned by QEMU and echoed in the response so concurrent
 * callers can match responses to pending requests.
 */
struct isolate_cmd_ioctl {
	uint32_t type;        /* ISOLATE_CMD_IOCTL */
	uint32_t handle_id;
	uint32_t cmd;
	uint32_t param_size;  /* bytes of param blob following this header */
	uint32_t aux_size;    /* bytes of aux blob following param blob    */
	uint32_t flags;       /* NVKVM_IOCTL_FL_* */
	uint32_t txn_id;      /* echoed in response for in-flight matching */
	uint32_t abi_profile; /* #81: nvkvm_abi_id of the host driver (stub uses
	                       * it for version-variant offsets: UVM rm_ctrl_fd,
	                       * NVOS46 status). 0 → stub falls back to 570/575. */
};

struct isolate_resp_ioctl {
	uint32_t type;        /* ISOLATE_RESP_IOCTL */
	uint32_t txn_id;      /* echoed from command */
	uint32_t param_size;  /* bytes of updated param blob following     */
	uint32_t aux_size;    /* bytes of updated aux blob following       */
	int32_t  retval;      /* ioctl return value (0 or -errno)          */
	uint32_t nvstatus;    /* NvStatus from params.status field         */
	uint64_t fault_addr;  /* GVA that triggered SIGSEGV (0 if none)   */
};

/* ── INTERRUPT ───────────────────────────────────────────────────────────────
 * Fire-and-forget (QEMU does not wait for a reply): the stub's reader thread
 * finds the worker currently executing target_txn and posts SIGUSR1 to it so
 * its in-flight host ioctl returns -EINTR.  The interrupted worker then sends
 * the normal ISOLATE_RESP_IOCTL with retval=-EINTR on the regular path.
 */
struct isolate_cmd_interrupt {
	uint32_t type;        /* ISOLATE_CMD_INTERRUPT */
	uint32_t target_txn;  /* txn_id of the ioctl to interrupt */
};

/* ── MMAP ────────────────────────────────────────────────────────────────── */

struct isolate_cmd_mmap {
	uint32_t type;        /* ISOLATE_CMD_MMAP */
	uint32_t handle_id;
	uint64_t gva;         /* MAP_FIXED target address in isolate       */
	uint64_t length;
	uint64_t offset;      /* fd offset                                 */
	uint32_t prot;
	uint32_t map_flags;   /* MAP_SHARED | MAP_FIXED (set by isolate)   */
};

struct isolate_resp_mmap {
	uint32_t type;        /* ISOLATE_RESP_MMAP */
	int32_t  retval;      /* 0 = success, -errno on failure            */
};

/* ── MUNMAP ──────────────────────────────────────────────────────────────── */

struct isolate_cmd_munmap {
	uint32_t type;        /* ISOLATE_CMD_MUNMAP */
	uint32_t reserved;
	uint64_t gva;
	uint64_t length;
};

/* ── POLL ────────────────────────────────────────────────────────────────── */

struct isolate_cmd_poll {
	uint32_t type;        /* ISOLATE_CMD_POLL */
	uint32_t handle_id;
	uint32_t events;      /* POLLIN | POLLOUT | POLLERR */
	uint32_t reserved;
};

struct isolate_cmd_unpoll {
	uint32_t type;        /* ISOLATE_CMD_UNPOLL */
	uint32_t handle_id;
};

/* Async event sent by isolate when fd becomes ready */
struct isolate_resp_poll_event {
	uint32_t type;        /* ISOLATE_RESP_POLL_EVENT */
	uint32_t handle_id;
	uint32_t revents;     /* which events fired */
	uint32_t reserved;
};

/* ── Generic OK/ERROR ────────────────────────────────────────────────────── */

struct isolate_resp_ok {
	uint32_t type;        /* ISOLATE_RESP_OK */
	uint32_t reserved;
};

struct isolate_resp_error {
	uint32_t type;        /* ISOLATE_RESP_ERROR */
	int32_t  err;         /* errno */
};

/* ── EXIT ────────────────────────────────────────────────────────────────── */

struct isolate_cmd_exit {
	uint32_t type;        /* ISOLATE_CMD_EXIT */
	uint32_t reserved;
};

/* ── OPEN_DEVICE ─────────────────────────────────────────────────────────── */

/*
 * QEMU asks the stub to open /dev/nvidia* (or eventfd) so the file's nvfp/mm
 * lineage matches the calling isolate process. The stub stores the fd under
 * handle_id and replies with a SCM_RIGHTS-attached copy of the fd in the
 * same sendmsg — QEMU then attaches that copy as the qemu_fd in its handle
 * table, satisfying invariant "QEMU always holds a copy of every stub-opened
 * fd via SCM_RIGHTS" (REFACTOR_PLAN §1, rule 5).
 *
 * dev_id values come from nvkvm_proto.h:
 *   NVKVM_DEV_CTL (0)      → /dev/nvidiactl
 *   NVKVM_DEV_GPU(n) (16+n)→ /dev/nvidia<n>
 *   NVKVM_DEV_EVENTFD (0xFF) → eventfd2(0, EFD_NONBLOCK | EFD_CLOEXEC)
 * /dev/nvidia-uvm and memfds are opened by QEMU, not via this command.
 */
struct isolate_cmd_open_device {
	uint32_t type;        /* ISOLATE_CMD_OPEN_DEVICE */
	uint32_t handle_id;   /* QEMU-assigned; stub stores fd under this id */
	uint32_t dev_id;      /* NVKVM_DEV_CTL / NVKVM_DEV_GPU(n) / EVENTFD */
	uint32_t flags;       /* O_RDWR etc; ignored for eventfd            */
	uint32_t txn_id;      /* echoed in response                         */
	uint32_t reserved;
};

struct isolate_resp_open_device {
	uint32_t type;        /* ISOLATE_RESP_OPEN_DEVICE */
	uint32_t txn_id;      /* echoed from command                        */
	int32_t  retval;      /* 0 on success; -errno on failure (no SCM)   */
	uint32_t reserved;
	/* On success: one fd attached via SCM_RIGHTS in the same sendmsg.  */
};

/* ── REALIZE_UVM_FD ──────────────────────────────────────────────────────────
 * QEMU sends this to spin up a fully-configured UVM fd in the stub:
 *   open /dev/nvidia-uvm
 *   UVM_INITIALIZE(flags = state.init_flags)
 *   UVM_REGISTER_GPU(uuid)         for each registered gpu
 *   UVM_REGISTER_GPU_VASPACE(...)  for each registered vas
 *   UVM_CREATE_RANGE_GROUP(id)     for each range group
 *   mode-specific intent ioctl (e.g. UVM_ALLOC_SEMAPHORE_POOL)
 *   mmap(2) at host_va_hint (or NULL for stub-chosen) with prot+flags
 *
 * The cmd carries the state snapshot + intent inline as two follow-up
 * SEQPACKET messages (sent by QEMU as: cmd header, then state bytes,
 * then intent bytes — sizes in the header).
 */
struct isolate_cmd_realize_uvm_fd {
	uint32_t type;        /* ISOLATE_CMD_REALIZE_UVM_FD */
	uint32_t txn_id;
	uint32_t mode;        /* NVKVM_UVM_REALIZE_MODE_* */
	uint32_t state_size;  /* bytes of snapshot to follow */
	uint32_t intent_size; /* bytes of intent to follow   */
	uint32_t prot;
	uint32_t map_flags;   /* MAP_FIXED added by stub if host_va_hint != 0 */
	uint32_t _pad;
	uint64_t length;
	uint64_t host_va_hint;/* 0 = let stub choose */
	uint64_t offset;      /* file offset for mmap */
};

struct isolate_resp_realize_uvm {
	uint32_t type;        /* ISOLATE_RESP_REALIZE_UVM */
	uint32_t txn_id;
	int32_t  retval;      /* 0 on success; -errno on failure            */
	uint32_t rm_status;   /* NV_STATUS from the realize ioctl(s)        */
	uint64_t host_va;     /* mmap result address                        */
	uint64_t length;      /* echo                                       */
	uint64_t realize_token;/* opaque (currently == host_va for now)     */
};

/* ── SETUP_RING ───────────────────────────────────────────────────────────
 * QEMU mints ONE memfd holding the request + response SPSC rings
 * (docs/design/command_buffer.md): it maps the memfd, initialises both ring
 * control blocks, then hands a copy of the memfd to the isolate via SCM_RIGHTS
 * in the same sendmsg.  The isolate maps it MAP_SHARED so QEMU, the isolate
 * (and, from Phase 4, the guest via a KVM memslot) all share the same pages.
 *
 * Before the ring is trusted a probe word round-trips through the shared data
 * region to prove BIDIRECTIONAL visibility: QEMU writes `probe` at the start
 * of the request data region; the isolate echoes it back in the response
 * (proves QEMU→isolate) and writes `probe ^ NVKVM_RING_PROBE_MASK` at the
 * start of the response data region (QEMU re-reads it → proves isolate→QEMU).
 *
 * The ring is a PURE OPTIMISATION: if setup fails the isolate keeps serving
 * every ioctl over the existing IOCTL/MMAP path, so SETUP_RING failure is
 * non-fatal to the isolate.
 *
 * Layout (see nvkvm_ring.h helpers): req ring control block at req_off (=0),
 * its data immediately after; resp ring control block at resp_off, its data
 * immediately after.  region_size is the whole memfd the isolate must mmap.
 */
struct isolate_cmd_setup_ring {
	uint32_t type;         /* ISOLATE_CMD_SETUP_RING */
	uint32_t region_size;  /* total memfd bytes the isolate must mmap */
	uint32_t req_off;      /* offset of request-ring control block (=0) */
	uint32_t resp_off;     /* offset of response-ring control block */
	uint32_t ring_bytes;   /* per-ring data bytes (power of two, >= 64) */
	uint32_t reserved;
	/* memfd attached via SCM_RIGHTS in the same sendmsg. */
};

struct isolate_resp_ring_ready {
	uint32_t type;         /* ISOLATE_RESP_RING_READY */
	int32_t  error;        /* 0 on success; -errno on failure */
	uint64_t probe_seen;   /* value the isolate read from the request data
	                        * region — QEMU checks it equals what it wrote */
};

/* Self-test mask (see SETUP_RING comment above). */
#define NVKVM_RING_PROBE_MASK 0x5a5a5a5a5a5a5a5aULL

/* ── ENTER_LOOP ───────────────────────────────────────────────────────────
 * QEMU forwards this (offloaded to its thread pool — it blocks for the whole
 * loop) to drive the isolate's reader thread into the SPSC consumer loop.  The
 * loop drains the request ring, executes flat RM_CONTROLs inline, writes
 * responses to the response ring, and — while looping — still polls the socket
 * at each drain edge so slow-path commands (IOCTL/INTERRUPT/EXIT) are serviced.
 * It exits after `idle_us` of no ring work AND no socket traffic, re-checking
 * nvkvm_ring_has_work() once more before committing to exit (the lost-wakeup-
 * free exit edge).  LOOP_EXITED carries the consumer's `head` (last_processed
 * free-running byte count) so the guest can compare it against last_published
 * and re-enter if the loop exited with work still queued.
 */
struct isolate_cmd_enter_loop {
	uint32_t type;        /* ISOLATE_CMD_ENTER_LOOP */
	uint32_t idle_us;     /* idle window before exit (0 → stub default) */
};

struct isolate_resp_loop_exited {
	uint32_t type;        /* ISOLATE_RESP_LOOP_EXITED */
	int32_t  error;       /* 0, or -errno if no ring is set up */
	uint64_t head;        /* request-ring head at exit (last_processed) */
};

/* ── PRESENT_EXPORT (#106) ─────────────────────────────────────────────────
 * QEMU asks the stub to export a render-node GEM object as a dma-buf so the
 * host display/codec can import it.  The stub looks up the render-node fd under
 * handle_id, runs DRM_IOCTL_PRIME_HANDLE_TO_FD on gem_handle, and replies with
 * the dma-buf fd attached via SCM_RIGHTS (same pattern as OPEN_DEVICE).  The
 * fd is a host buffer reference only — the stub never maps or reads it. */
struct isolate_cmd_present_export {
	uint32_t type;        /* ISOLATE_CMD_PRESENT_EXPORT */
	uint32_t handle_id;   /* render-node handle whose fd holds the GEM */
	uint32_t gem_handle;  /* GEM handle in the stub's render-node DRM file */
	uint32_t txn_id;      /* echoed in response */
};

struct isolate_resp_present_export {
	uint32_t type;        /* ISOLATE_RESP_PRESENT_EXPORT */
	uint32_t txn_id;      /* echoed from command */
	int32_t  retval;      /* 0 on success; -errno on failure (no SCM)   */
	uint32_t reserved;
	/* On success: one dma-buf fd attached via SCM_RIGHTS in the same sendmsg. */
};

/* ── XISO_IMPORT (#110 cross-isolate dma-buf) ──────────────────────────────
 * QEMU brokers a GPU buffer from one isolate (the owner, which allocated it)
 * into another (the importer, e.g. a compositor).  QEMU first PRESENT_EXPORTs
 * the bo from the owner stub (host dma-buf fd), then sends THIS command to the
 * importer stub with that dma-buf fd attached via SCM_RIGHTS.  The importer
 * runs DRM_IOCTL_PRIME_FD_TO_HANDLE on its own render-node fd, producing a real
 * local nvidia-drm GEM backed by the same physical memory — exactly what a
 * bare-metal cross-process import does.  Subsequent RM export/import (0x09 /
 * 0x3d06) then run entirely within the importer.  QEMU guarantees both isolates
 * belong to the same VM; entitlement (which process may import) is enforced
 * guest-side by the guest kernel gating who holds the guest dma-buf fd. */
struct isolate_cmd_xiso_import {
	uint32_t type;        /* ISOLATE_CMD_XISO_IMPORT */
	uint32_t handle_id;   /* importer render-node handle whose fd does the import */
	uint32_t txn_id;      /* echoed in response */
	uint32_t reserved;
	/* The host dma-buf fd to import arrives via SCM_RIGHTS in the same msg. */
};

struct isolate_resp_xiso_import {
	uint32_t type;        /* ISOLATE_RESP_XISO_IMPORT */
	uint32_t txn_id;      /* echoed from command */
	int32_t  retval;      /* 0 on success; -errno on failure */
	uint32_t gem_handle;  /* OUT: the importer-local GEM handle (valid if retval==0) */
};

/*
 * When the isolate is hardened with an empty mount namespace, /dev is no
 * longer reachable by path.  QEMU parks an O_PATH directory handle to the
 * host's /dev at this fixed fd in the stub child before exec; the stub opens
 * device nodes with openat(NVKVM_DEV_DIRFD, "nvidiaX", ...).  If the fd is not
 * a directory (un-hardened spawn), the stub falls back to "/dev/<name>".
 */
#define NVKVM_DEV_DIRFD 4

#endif /* NVKVM_ISOLATE_PROTO_H */
