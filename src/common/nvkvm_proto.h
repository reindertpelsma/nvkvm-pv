/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_proto.h — virtio-nvgpu wire protocol (isolate architecture)
 *
 * Shared between the guest kernel module (nvkvm-guest.ko) and the QEMU virtio
 * device backend. Both sides must be compiled with the same version of this
 * header.
 *
 * Transport: a single virtio device with three virtqueues:
 *   VQ 0 (TX): guest → host requests
 *   VQ 1 (RX): host → guest responses
 *   VQ 2 (EVT): host → guest async events (poll notifications)
 *
 * Security model
 * ==============
 * QEMU validates every field in every request. Unknown handle IDs or isolate IDs
 * cause QEMU to immediately panic the VM — the guest kernel should never send
 * invalid values; if it does, it is compromised.
 *
 * The guest kernel module validates every field from guest userspace before
 * forwarding over virtio. Neither side trusts the other beyond what is proven
 * by the protocol invariants below.
 *
 * Isolate architecture
 * ====================
 * Each guest userspace process (identified by mm_struct) has exactly one
 * "isolate" host process that mirrors its virtual address space. The isolate
 * has GPU device fds dup'd in via SCM_RIGHTS and replays every mmap at the
 * same GVA (MAP_FIXED), so the NVIDIA driver sees valid mappings in current->mm
 * when processing ioctls. QEMU routes all ioctls through the correct isolate.
 *
 * Handle IDs
 * ==========
 * Two types, both 32-bit opaque integers, globally unique across all sessions:
 *   nvidia handle: wraps an open /dev/nvidia* fd kept in QEMU
 *   memory handle: wraps a memfd kept in QEMU
 * Handles may be distributed to isolates via SCM_RIGHTS. QEMU tracks which
 * isolates hold each handle. close_handle requires no isolates to hold the handle.
 */

#ifndef NVKVM_PROTO_H
#define NVKVM_PROTO_H

#include <linux/types.h>

/* ── Virtio device configuration space ──────────────────────────────────── */

struct nvkvm_virtio_config {
	__le64 shm_base;        /* host-physical base of shared memory      */
	__le64 shm_len;         /* size of shared memory region in bytes    */
	__le64 mmap_win_gpa;    /* guest-physical base of mmap window       */
	__le64 mmap_win_len;    /* size of mmap window in bytes             */
	__le64 flags;           /* NVKVM_CONFIG_F_* — QEMU dictates features */
};

/*
 * Config feature flags. QEMU is authoritative: it both advertises these and
 * enforces them at the cross-VM boundary (a guest that ignores a cleared bit
 * still can't open/forward the corresponding devices). The guest uses them to
 * avoid exposing devices it can't use.
 *
 * F_GRAPHICS: this VM may use the graphics stack (/dev/dri/renderD128 +
 *   /dev/nvidia-modeset and the DRM/NVKMS ioctl surface). Cleared for
 *   compute-only VMs — shrinks the guest device + host attack surface.
 */
#define NVKVM_CONFIG_F_GRAPHICS  (1ULL << 0)

/* ── Protocol version ────────────────────────────────────────────────────── */

/* Version 3 expands nvkvm_uvm_state_snapshot.gpus[] with the rmCtrlFd handle
 * and RM object handles required to replay the real 40-byte
 * UVM_REGISTER_GPU_PARAMS.  Reject v2 peers rather than parse the following
 * va_spaces[] and range_group_ids[] at their old offsets. */
#define NVKVM_PROTO_VERSION     3

/* ── Virtqueue indices ───────────────────────────────────────────────────── */

#define NVKVM_VQ_TX     0  /* guest → host requests        */
#define NVKVM_VQ_RX     1  /* host → guest responses       */
#define NVKVM_VQ_EVT    2  /* host → guest async events    */
#define NVKVM_NUM_VQS   3

/* ── Shared memory layout ────────────────────────────────────────────────── */

#define NVKVM_SHM_SLOT_MIN_SIZE     (4096)
#define NVKVM_SHM_SLOT_DEFAULT_SIZE (64 * 1024)
#define NVKVM_SHM_NSLOTS            256
#define NVKVM_SHM_CTRL_SLOT         0

struct nvkvm_shm_ctrl {
	__le32 proto_version;
	__le32 slot_size;
	__le32 nslots;
	__le32 reserved;
	__u8   driver_version[64];
};

/* ── Device identifiers ──────────────────────────────────────────────────── */

#define NVKVM_DEV_CTL        0          /* /dev/nvidiactl                */
#define NVKVM_DEV_UVM        1          /* /dev/nvidia-uvm               */
#define NVKVM_DEV_GPU(n)     (16 + (n)) /* /dev/nvidia0 → /dev/nvidia15  */
#define NVKVM_DEV_DRM_RD(n)  (32 + (n)) /* /dev/dri/renderD128+n (nvidia-drm) */
#define NVKVM_DEV_MODESET    48         /* /dev/nvidia-modeset (NVKMS)    */

/*
 * NVKMS wrapper ioctl: the ONLY ioctl on /dev/nvidia-modeset.
 *   _IOWR(NVKMS_IOCTL_MAGIC 'm', NVKMS_IOCTL_CMD 0, struct NvKmsIoctlParams)
 *   = (3<<30) | (16<<16) | ('m'<<8) | 0 = 0xC0106D00.
 * NvKmsIoctlParams = { u32 cmd@0; u32 size@4; u64 address@8 } (16 bytes); the
 * kernel reads/writes `size` bytes at `address` (a single embedded user ptr).
 * See nvkms-ioctl.h.
 */
#define NVKVM_NVKMS_IOCTL_CMD   0xC0106D00u
#define NVKVM_NVKMS_PARAMS_SIZE 16u   /* sizeof(struct NvKmsIoctlParams) */
#define NVKVM_NVKMS_ADDR_OFF    8u    /* offset of the embedded address ptr */

/*
 * NVKMS sub-commands (NvKmsIoctlParams.cmd @ wrapper offset 0) whose inner
 * params embed file descriptors that must be translated guest-fd↔handle_id↔
 * stub-fd (same machinery as RM EXPORT_OBJECT_TO_FD).
 *
 * REGISTER_SURFACE: the Vulkan ICD exports the semaphore-surface memory to an
 * fd (RM EXPORT_OBJECT_TO_FD) then registers it here by fd (useFd=TRUE).  Its
 * cmdType is version-keyed -- 16 up to 570.195.03, 17 from 570.207 -- and is
 * resolved through src/common/nvkvm_nvkms_ops.h, never hardcoded.  The inner
 * struct layout below is NOT version-keyed: it is byte-identical from 570
 * through 610.  NvKmsRegisterSurfaceRequest layout (nvkms-api.h):
 *   deviceHandle@0, useFd@4, rmClient@8, planes[3]@16 (each 32B:
 *   {union{rmObject/fd}@0, offset@8, pitch@16, rmObjectSizeInBytes@24}).
 * So planes[i].u.fd live at 16 + i*32 = {16,48,80}, valid only when useFd!=0.
 */
#define NVKVM_NVKMS_REGSURF_USEFD_OFF    4u
#define NVKVM_NVKMS_REGSURF_PLANE0_OFF   16u
#define NVKVM_NVKMS_REGSURF_PLANE_STRIDE 32u
#define NVKVM_NVKMS_MAX_PLANES           3u
#define NVKVM_DEV_EVENTFD    0xFF       /* eventfd2() — for NV01_EVENT_OS_EVENT */

/* ── Request types ───────────────────────────────────────────────────────── */

/*
 * Keep these as an enum, not preprocessor constants.  The guest completion
 * switch is deliberately exhaustive over this type and compiled with
 * -Werror=switch: adding a request here without teaching the guest how to
 * consume its response must fail the module build, not become a third
 * "unknown type" runtime bug.
 */
enum nvkvm_request_type {
	/* Legacy (compat, will be removed) */
	NVKVM_REQ_OPEN                    = 1,
	NVKVM_REQ_CLOSE                   = 2,
	NVKVM_REQ_IOCTL                   = 3,
	NVKVM_REQ_MMAP                    = 4,
	NVKVM_REQ_MUNMAP                  = 5,

	/* Isolate/handle architecture */
	NVKVM_REQ_LIST_NVIDIA_DEVICES     = 10, /* enumerate host GPU devices     */
	NVKVM_REQ_OPEN_NVIDIA_HANDLE      = 11, /* open /dev/nvidia* in QEMU      */
	NVKVM_REQ_OPEN_MEMORY_HANDLE      = 12, /* memfd_create in QEMU           */
	NVKVM_REQ_CLOSE_HANDLE            = 13, /* close when no isolate holds it */
	NVKVM_REQ_CREATE_ISOLATE          = 14, /* spawn isolate process          */
	NVKVM_REQ_KILL_ISOLATE            = 15, /* exit isolate process           */
	NVKVM_REQ_COPY_HANDLE_TO_ISOLATE  = 16, /* SCM_RIGHTS send                */
	NVKVM_REQ_CLOSE_HANDLE_ON_ISOLATE = 17, /* CLOSE_FD cmd to isolate        */
	NVKVM_REQ_IOCTL_ON_ISOLATE        = 18, /* IOCTL cmd via isolate          */
	NVKVM_REQ_MMAP_ON_ISOLATE         = 19, /* MMAP cmd via isolate           */
	NVKVM_REQ_MUNMAP_ON_ISOLATE       = 20, /* MUNMAP cmd via isolate         */
	NVKVM_REQ_POLL_ON_ISOLATE         = 21, /* start polling fd in isolate    */
	NVKVM_REQ_UNPOLL_ON_ISOLATE       = 22, /* stop polling fd in isolate     */
	NVKVM_REQ_WRITE_MEMORY_HANDLE     = 23, /* shm_slot → memfd (page upload) */
	NVKVM_REQ_READ_MEMORY_HANDLE      = 24, /* memfd → shm_slot (writeback)   */
	NVKVM_REQ_REALIZE_UVM_MAPPING     = 25, /* state-machine mmap-realize     */
	NVKVM_REQ_READ_HOST_FILE          = 26, /* read live host proc/sys file   */
	NVKVM_REQ_INTERRUPT               = 27, /* interrupt an in-flight ioctl   */
	NVKVM_REQ_SETUP_RING              = 28, /* fetch this session's command-buffer ring GPA */
	NVKVM_REQ_ENTER_LOOP              = 29, /* drive the isolate's SPSC consumer loop */
	NVKVM_REQ_PRESENT                 = 30, /* #106 present path: virtual head flipped a
	                                           * scanout bo; QEMU exports its host dma-buf  */
	NVKVM_REQ_XISO_IMPORT             = 31, /* #110 cross-isolate dma-buf: broker a bo from
	                                           * the owner isolate into the caller's; returns
	                                           * the caller-local stub GEM handle            */
};

/* ── Generic header ──────────────────────────────────────────────────────── */

struct nvkvm_hdr {
	__le32 type;        /* NVKVM_REQ_*                            */
	__le32 txn_id;      /* guest-assigned, echoed in response     */
};

/* ── LIST_NVIDIA_DEVICES ─────────────────────────────────────────────────── */

struct nvkvm_req_list_nvidia_devices {
	__le32 reserved;
};

#define NVKVM_MAX_DEVICES  32

struct nvkvm_device_info {
	__le32 dev_id;       /* NVKVM_DEV_* */
	__le32 flags;        /* reserved    */
	__le64 reserved;
};

struct nvkvm_resp_list_nvidia_devices {
	__le32 ndevices;
	__le32 status;
	struct nvkvm_device_info devices[NVKVM_MAX_DEVICES];
};

/* ── OPEN_NVIDIA_HANDLE ──────────────────────────────────────────────────── */

struct nvkvm_req_open_nvidia_handle {
	__le32 dev_id;       /* NVKVM_DEV_*                          */
	__le32 flags;        /* O_RDWR etc.                          */
	__le32 session_id;   /* guest session                        */
	__le32 reserved;
};

struct nvkvm_resp_open_nvidia_handle {
	__le32 handle_id;    /* globally unique handle               */
	__le32 status;       /* 0 = success, errno otherwise         */
};

/* ── PRESENT (#106 present path B) ───────────────────────────────────────────
 * The guest's virtual KMS head just flipped a framebuffer backed by one of our
 * proxy GEMs (an NVIDIA scanout bo allocated through the render node).  The
 * guest sends this so QEMU can ask the owning isolate's stub to export the real
 * host buffer behind it as a dma-buf (PRIME_HANDLE_TO_FD on the stub-side GEM
 * handle) and route it to the host display/codec.  Geometry is carried so QEMU
 * can import the dma-buf as an EGLImage without re-deriving it.  No guest VA is
 * involved — only the opaque stub GEM handle + scanout metadata. */
struct nvkvm_req_present {
	__le32 isolate_id;   /* which isolate owns the render-node fd        */
	__le32 handle_id;    /* render-node handle whose stub fd holds the bo */
	__le32 stub_handle;  /* GEM handle in the stub's render-node DRM file */
	__le32 width;
	__le32 height;
	__le32 pitch;        /* bytes per row (plane 0)                      */
	__le32 format;       /* DRM fourcc                                   */
	__le32 session_id;
	__le64 modifier;     /* DRM format modifier (block-linear / linear)  */
};

struct nvkvm_resp_present {
	__le32 status;       /* 0 = QEMU exported/accepted the frame, errno else */
	__le32 reserved;
};

/* #110 cross-isolate dma-buf import. The caller (importer) holds a guest GEM
 * that proxies a bo owned by a different isolate; QEMU PRESENT_EXPORTs the bo
 * from the owner and PRIME_FD_TO_HANDLEs it into the importer's stub, returning
 * a stub GEM handle valid in the importer's render-node fd. */
struct nvkvm_req_xiso_import {
	__le32 owner_isolate_id;    /* isolate that allocated the bo            */
	__le32 owner_handle_id;     /* owner's render-node handle holding the bo */
	__le32 owner_stub_handle;   /* GEM handle in the owner's stub DRM file   */
	__le32 importer_isolate_id; /* caller's isolate                          */
	__le32 importer_handle_id;  /* caller's render-node handle (does the import) */
	__le32 reserved;
};

struct nvkvm_resp_xiso_import {
	__le32 status;       /* 0 on success, errno else */
	__le32 gem_handle;   /* importer-local stub GEM handle (valid if status==0) */
};

/* Kept for compat with existing open path */
struct nvkvm_req_open {
	__le32 dev_id;
	__le32 flags;
	__le32 session_id;
	__le32 reserved;
};

struct nvkvm_resp_open {
	__le32 fd_token;
	__le32 status;
};

/* ── OPEN_MEMORY_HANDLE ──────────────────────────────────────────────────── */

struct nvkvm_req_open_memory_handle {
	__le64 size;         /* initial size (may be 0 for grow-on-demand) */
	__le32 session_id;
	__le32 flags;        /* reserved                               */
};

struct nvkvm_resp_open_memory_handle {
	__le32 handle_id;
	__le32 status;
};

/* ── CLOSE_HANDLE ────────────────────────────────────────────────────────── */

struct nvkvm_req_close_handle {
	__le32 handle_id;
	__le32 reserved;
};

struct nvkvm_resp_close_handle {
	__le32 status;
	__le32 reserved;
};

/* ── CLOSE (legacy) ──────────────────────────────────────────────────────── */

struct nvkvm_req_close {
	__le32 fd_token;
	__le32 reserved;
};

struct nvkvm_resp_close {
	__le32 status;
	__le32 reserved;
};

/* ── CREATE_ISOLATE ──────────────────────────────────────────────────────── */

struct nvkvm_req_create_isolate {
	__le32 session_id;
	__le32 reserved;
};

struct nvkvm_resp_create_isolate {
	__le32 isolate_id;   /* opaque isolate identifier            */
	__le32 status;
};

/* ── KILL_ISOLATE ────────────────────────────────────────────────────────── */

struct nvkvm_req_kill_isolate {
	__le32 isolate_id;
	__le32 reserved;
};

struct nvkvm_resp_kill_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── INTERRUPT ───────────────────────────────────────────────────────────────
 * Sent by the guest when a task blocked in a forwarded ioctl receives a
 * signal.  QEMU routes it to the named isolate, which posts SIGUSR1 to the
 * worker thread running txn_id so its blocking host ioctl returns -EINTR.
 * Best-effort: the guest still waits (uninterruptibly) for the descriptor to
 * be reclaimed; the interrupt only shortens how long the host keeps working.
 */
struct nvkvm_req_interrupt {
	__le32 isolate_id;  /* QEMU-managed isolate holding the in-flight ioctl */
	__le32 target_txn;  /* txn_id of the ioctl to interrupt                 */
};

struct nvkvm_resp_interrupt {
	__le32 status;      /* 0 = routed; nonzero = unknown isolate/txn        */
	__le32 reserved;
};

/* ── SETUP_RING ──────────────────────────────────────────────────────────────
 * The guest asks for its session's command-buffer ring (docs/design/
 * command_buffer.md).  QEMU returns the guest-physical address of the ring
 * region (already placed in the sparse window at isolate create) plus its
 * geometry, so the guest kmd can memremap it and run the SPSC fast path.
 * status != 0 (or ring_gpa == 0) means no ring is available → the guest stays
 * on the virtqueue path.  QEMU only maps the ring; it never inspects contents.
 */
struct nvkvm_req_setup_ring {
	__le32 session_id;
	__le32 reserved;
};

struct nvkvm_resp_setup_ring {
	__le64 ring_gpa;      /* guest-physical base of the ring region (0 = none) */
	__le32 region_size;   /* total bytes to memremap                           */
	__le32 req_off;       /* offset of request-ring control block (=0)         */
	__le32 resp_off;      /* offset of response-ring control block             */
	__le32 ring_bytes;    /* per-ring data bytes                               */
	__le32 status;        /* 0 = ok; errno otherwise                           */
	__le32 reserved;
};

/* ── ENTER_LOOP ──────────────────────────────────────────────────────────────
 * The guest's per-session "pump" issues this to keep the isolate spinning on
 * the SPSC request ring (docs/design/command_buffer.md).  It is a BLOCKING
 * virtqueue request: QEMU offloads it to a worker that forwards ENTER_LOOP to
 * the stub and blocks until the loop idles out, then completes the request with
 * `head` (the consumer's last-processed free-running byte count).  The pump
 * re-enters when head < the request ring's published tail.
 */
struct nvkvm_req_enter_loop {
	__le32 session_id;
	__le32 idle_us;       /* idle window before the stub exits (0 = default) */
};

struct nvkvm_resp_enter_loop {
	__le64 head;          /* request-ring head at loop exit (last_processed) */
	__le32 status;        /* 0 = ok; errno otherwise (no ring / dead isolate) */
	__le32 reserved;
};

/* ── COPY_HANDLE_TO_ISOLATE ──────────────────────────────────────────────── */

struct nvkvm_req_copy_handle_to_isolate {
	__le32 handle_id;
	__le32 isolate_id;
};

struct nvkvm_resp_copy_handle_to_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── CLOSE_HANDLE_ON_ISOLATE ─────────────────────────────────────────────── */

struct nvkvm_req_close_handle_on_isolate {
	__le32 handle_id;
	__le32 isolate_id;
};

struct nvkvm_resp_close_handle_on_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── IOCTL_ON_ISOLATE ────────────────────────────────────────────────────── */

/*
 * The parameter blob and aux buffer are placed in shared memory slots.
 * vma_whitelist_slot holds an array of nvkvm_vma_entry structs (see below)
 * used for demand-fault resolution. Set vma_whitelist_nentries = 0 to skip.
 *
 * flags:
 *   NVKVM_IOCTL_FL_RETRY_EFAULT — this is a retry after demand-fault mapping
 */
#define NVKVM_IOCTL_FL_RETRY_EFAULT  (1 << 0)

struct nvkvm_req_ioctl_on_isolate {
	__le32 isolate_id;
	__le32 handle_id;            /* which nvidia handle (fd)              */
	__le32 cmd;                  /* full ioctl command                    */
	__le32 param_size;           /* size of param blob in shm_slot        */
	__le32 shm_slot;             /* slot index for param blob             */
	__le32 aux_size;             /* aux buffer size (0 if none)           */
	__le32 shm_aux_slot;         /* slot index for aux buffer             */
	__le32 vma_whitelist_nentries; /* # of VMA whitelist entries          */
	__le32 vma_whitelist_slot;   /* slot for nvkvm_vma_entry array        */
	__le32 flags;                /* NVKVM_IOCTL_FL_*                      */
	__le32 session_id;
	__le32 reserved;
};

struct nvkvm_resp_ioctl_on_isolate {
	__le64 retval;               /* raw ioctl return value                */
	__le32 status;               /* 0 = ok, errno on transport error      */
	__le32 nvstatus;             /* NvStatus from params.status field     */
	__le64 fault_addr;           /* GVA that caused SIGSEGV (0 if none)  */
};

/* VMA entry for demand-fault whitelist */
struct nvkvm_vma_entry {
	__le64 start;   /* inclusive */
	__le64 end;     /* exclusive */
	__le32 prot;    /* PROT_READ | PROT_WRITE | PROT_EXEC */
	__le32 reserved;
};

/* Maximum whitelist entries per ioctl */
#define NVKVM_MAX_VMA_ENTRIES  1024

/* ── MMAP_ON_ISOLATE ─────────────────────────────────────────────────────── */

/*
 * Asks QEMU to:
 *   1. mmap the handle fd at any host VA (QVA), register GPA→QVA in KVM slot.
 *   2. Send MMAP command to isolate: map same fd at gva (MAP_FIXED).
 * Returns the GPA allocated for the guest to use in remap_pfn_range.
 */
struct nvkvm_req_mmap_on_isolate {
	__le32 isolate_id;
	__le32 handle_id;
	__le64 gva;              /* exact guest VA to map in the isolate   */
	__le64 offset;           /* fd offset                              */
	__le64 length;           /* mapping length                         */
	__le32 prot;             /* PROT_READ | PROT_WRITE | ...           */
	__le32 map_flags;        /* MAP_SHARED etc. (MAP_FIXED added by QEMU) */
	__le32 session_id;
	__le32 reserved;
};

struct nvkvm_resp_mmap_on_isolate {
	__le64 gpa_base;         /* GPA for remap_pfn_range                */
	__le64 length;           /* actual mapped length (page-aligned)    */
	__le32 mmap_token;       /* opaque handle for munmap               */
	__le32 status;
};

/* ── MUNMAP_ON_ISOLATE ───────────────────────────────────────────────────── */

struct nvkvm_req_munmap_on_isolate {
	__le32 isolate_id;
	__le32 mmap_token;
};

struct nvkvm_resp_munmap_on_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── POLL_ON_ISOLATE ─────────────────────────────────────────────────────── */

struct nvkvm_req_poll_on_isolate {
	__le32 isolate_id;
	__le32 handle_id;
	__le32 events;           /* POLLIN | POLLOUT | ...                 */
	__le32 reserved;
};

struct nvkvm_resp_poll_on_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── UNPOLL_ON_ISOLATE ───────────────────────────────────────────────────── */

struct nvkvm_req_unpoll_on_isolate {
	__le32 isolate_id;
	__le32 handle_id;
};

struct nvkvm_resp_unpoll_on_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── Async events (VQ_EVT) ───────────────────────────────────────────────── */

/*
 * VQ_EVT carried exactly one kind of message and so had no type field.  It now
 * carries two, and the discriminator is the LAST word -- what used to be
 * `reserved`, which QEMU has always zero-filled.  So type 0 still means the
 * poll event it always meant, an older guest that ignores the word keeps
 * working against a newer QEMU, and a newer guest reading it against an older
 * QEMU sees 0 and is right.  No version bump is needed for that to hold; it
 * only holds because the field was genuinely always zero, so do not repurpose
 * any other `reserved` in this header on the strength of this precedent
 * without checking the same thing.
 *
 * Every event is the same 16 bytes, because the guest pre-posts fixed-size
 * buffers on VQ_EVT and recycles them (nvkvm_evt_post_one).
 */
#define NVKVM_EVT_TYPE_POLL     0u
#define NVKVM_EVT_TYPE_UI_INFO  1u

struct nvkvm_evt_poll {
	__le32 isolate_id;
	__le32 handle_id;
	__le32 events;
	__le32 type;            /* NVKVM_EVT_TYPE_POLL (0); was `reserved` */
};

/*
 * The host's window is now width x height -- QEMU's GraphicHwOps.ui_info,
 * which in broker mode is the broker's EV_SURFACE.  Advisory: the guest offers
 * it as the head's preferred mode and its own compositor decides.
 */
struct nvkvm_evt_ui_info {
	__le32 width;
	__le32 height;
	/*
	 * The host output's refresh in MILLIHERTZ, 0 when unknown.  Was
	 * `reserved` and always zero, so an older guest reads it as unknown --
	 * which is what it meant.
	 *
	 * Worth carrying: the virtual head is otherwise pinned by the kms_hz
	 * module parameter (default 60) whatever the host is doing, so a 144 Hz
	 * host drives a 60 Hz guest and neither side knows.
	 */
	__le32 refresh_mhz;
	__le32 type;            /* NVKVM_EVT_TYPE_UI_INFO (1) */
};

/* ── WRITE_MEMORY_HANDLE ─────────────────────────────────────────────────── */
/* Copy shm_slot[0..size) into memfd at offset (CPU page upload). */

struct nvkvm_req_write_memory_handle {
	__le32 handle_id;
	__le32 shm_slot;
	__le64 offset;
	__le32 size;
	__le32 reserved;
};

struct nvkvm_resp_write_memory_handle {
	__le32 status;
	__le32 reserved;
};

/* ── READ_MEMORY_HANDLE ──────────────────────────────────────────────────── */
/* Copy memfd at offset into shm_slot[0..size) (CPU page writeback). */

struct nvkvm_req_read_memory_handle {
	__le32 handle_id;
	__le32 shm_slot;
	__le64 offset;
	__le32 size;
	__le32 reserved;
};

struct nvkvm_resp_read_memory_handle {
	__le32 status;
	__le32 reserved;
};

/* Legacy ioctl request (retained for dispatch.c compat) */
struct nvkvm_req_ioctl {
	__le32 fd_token;
	__le32 cmd;
	__le32 param_size;
	__le32 shm_slot;
	__le32 aux_size;
	__le32 shm_aux_slot;
	__le32 session_id;
	__le32 reserved;
};

struct nvkvm_resp_ioctl {
	__le64 retval;
	__le32 status;
	__le32 reserved;
};

/* Legacy mmap request */
struct nvkvm_req_mmap {
	__le32 fd_token;
	__le32 prot;
	__le32 flags;
	__le32 reserved;
	__le64 offset;
	__le64 length;
};

struct nvkvm_resp_mmap {
	__le64 gpa_base;
	__le64 length;
	__le32 mmap_token;
	__le32 status;
};

struct nvkvm_req_munmap {
	__le32 mmap_token;
	__le32 reserved;
};

struct nvkvm_resp_munmap {
	__le32 status;
	__le32 reserved;
};

/* ── REALIZE_UVM_MAPPING — state-machine mmap-realize ────────────────────────
 *
 * Sent by the guest module when libcuda mmap's a UVM fd whose state has been
 * accumulated by PURE_CONFIG / STATE_REGISTRATION / MAPPING_INTENT recording
 * (see docs/STATE_MACHINE_PLAN.md).
 *
 * The guest module places the per-fd uvm_state snapshot in a shm slot and
 * the mode-specific intent params in another shm slot.  QEMU validates
 * STRICTLY (sizes exact, fields enumerated, flags allowlisted) before
 * issuing the batched ISOLATE_CMD_REALIZE_UVM_FD to the stub.
 */

/* Mode constants — must match nvkvm_uvm_mapping_intent.mode in the guest. */
#define NVKVM_UVM_REALIZE_MODE_SEM_POOL      1
#define NVKVM_UVM_REALIZE_MODE_EXTERNAL      2
#define NVKVM_UVM_REALIZE_MODE_CREATE_RANGE  3

#define NVKVM_UVM_MAX_REG_GPUS      16   /* QEMU enforces; below kernel UVM_MAX_GPUS */
#define NVKVM_UVM_MAX_VA_SPACES     16
#define NVKVM_UVM_MAX_RANGE_GROUPS  16

/* Carried via state_shm_slot — fixed layout, QEMU validates lengths
 * against the count fields. */
struct nvkvm_uvm_state_snapshot {
	__le64 init_flags;
	__le32 n_gpus;
	__le32 n_va_spaces;
	__le32 n_range_groups;
	__le32 _pad0;
	struct {
		__u8   gpu_uuid[16];
		__le32 rm_ctrl_fd_handle_id;
		__le32 h_client;
		__le32 h_smc_part_ref;
		__le32 _pad;
	} gpus[NVKVM_UVM_MAX_REG_GPUS];
	struct {
		__u8   gpu_uuid[16];
		__le32 rm_ctrl_fd_handle_id;
		__le32 h_client;
		__le32 h_va_space;
		__le32 _pad;
	} va_spaces[NVKVM_UVM_MAX_VA_SPACES];
	__le64 range_group_ids[NVKVM_UVM_MAX_RANGE_GROUPS];
};

/* Carried via intent_shm_slot — variable shape per mode.  For
 * MODE_SEM_POOL we ship the entire UVM_ALLOC_SEMAPHORE_POOL_PARAMS that
 * the guest recorded (9248 bytes; QEMU validates size exactly). */

struct nvkvm_req_realize_uvm_mapping {
	__le32 isolate_id;
	__le32 fd_handle_id;
	__le32 mode;                   /* NVKVM_UVM_REALIZE_MODE_*    */
	__le32 session_id;
	__le64 gva;                    /* requested guest VA          */
	__le64 length;
	__le64 offset_hint;            /* libcuda's mmap offset       */
	__le32 prot;
	__le32 map_flags;
	__le32 state_shm_slot;         /* nvkvm_uvm_state_snapshot    */
	__le32 intent_shm_slot;        /* mode-specific intent bytes  */
	__le32 intent_size;            /* size of intent blob         */
	__le32 _pad0;
};

struct nvkvm_resp_realize_uvm_mapping {
	__le64 gpa_base;               /* guest module remap_pfn_range here */
	__le64 length;                 /* echo                              */
	__le64 realize_token;          /* for teardown / side-effect routing */
	__le32 rm_status;              /* NV_STATUS from the kernel realize  */
	__le32 status;                 /* 0 on success, -errno otherwise     */
};

/* ── READ_HOST_FILE ────────────────────────────────────────────────────────
 *
 * Live read of a host-side proc/sys file the NVIDIA driver creates but the
 * guest doesn't have (no real nvidia.ko loaded in the VM).  libcuda probes
 * these paths during initialization and bails into CUDA_ERROR_UNKNOWN if
 * any are missing.
 *
 * The file is selected by an enum (explicit whitelist on QEMU); the guest
 * never names a path.  QEMU does a fresh open+read on each call so the
 * content is always current.
 */
enum nvkvm_host_file {
	NVKVM_HFILE_NVIDIA_PARAMS         = 1, /* /proc/driver/nvidia/params       */
	NVKVM_HFILE_NVIDIA_INITSTATE      = 2, /* /sys/module/nvidia/initstate     */
	NVKVM_HFILE_NVIDIA_UVM_INITSTATE  = 3, /* /sys/module/nvidia_uvm/initstate */
	NVKVM_HFILE_NVIDIA_NUMA_STATUS    = 4, /* /proc/driver/nvidia/gpus/<bdf>/numa_status */
	NVKVM_HFILE_NVIDIA_INFORMATION    = 5, /* /proc/driver/nvidia/gpus/<bdf>/information */
	NVKVM_HFILE_NVIDIA_REG_BASE       = 6, /* /proc/driver/nvidia/gpus/<bdf>/registry */
	NVKVM_HFILE_NVIDIA_VERSION        = 7, /* /proc/driver/nvidia/version      */
	NVKVM_HFILE_MAX                   = 8,
};

#define NVKVM_HFILE_MAX_SIZE   8192

struct nvkvm_req_read_host_file {
	__le32 file_id;        /* enum nvkvm_host_file */
	__le32 shm_slot;       /* dest slot; content written starting at offset 0 */
	__le32 max_len;        /* cap (must be <= NVKVM_HFILE_MAX_SIZE)            */
	__le32 gpu_index;      /* for per-GPU files (NUMA/INFORMATION/REGISTRY):
	                        * index into QEMU's discovered host-BDF list.  The
	                        * guest NEVER supplies a path or BDF string — only
	                        * this integer — so path traversal is impossible. */
};

struct nvkvm_resp_read_host_file {
	__le32 status;         /* 0 success, errno on failure  */
	__le32 nbytes;         /* bytes actually written to shm */
};

#define NVKVM_MAX_REQ_PAYLOAD  sizeof(struct nvkvm_req_ioctl_on_isolate)

#endif /* NVKVM_PROTO_H */
