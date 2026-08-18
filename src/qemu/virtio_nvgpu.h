/*
 * virtio_nvgpu.h — QEMU virtio-nvgpu device backend header
 *
 * The QEMU backend is the host-side counterpart of the guest nvkvm-guest.ko
 * module. It sits at the hypervisor/VM boundary and:
 *
 *  1. Validates every request from the guest before forwarding.
 *  2. Translates guest fd_tokens to real host fds.
 *  3. Translates guest physical addresses (GPAs) to host virtual addresses
 *     (HVAs) for mmap / OS descriptor operations.
 *  4. Calls the real NVIDIA ioctls and returns results.
 *  5. Manages the host-side RM object graph for isolation and lifecycle.
 *
 * Security model
 * ==============
 * The guest kernel module is not trusted. Anything it sends may be malformed,
 * out-of-range, or designed to exploit the host NVIDIA driver or QEMU.
 *
 *  - param_size is validated against the expected size for cmd before any
 *    pointer into shared memory is dereferenced.
 *  - shm_slot must be in [1, NVKVM_SHM_NSLOTS) and owned by this request.
 *  - fd_token must map to an open host fd in the per-session table.
 *  - session_id must match the fd_token's owning session.
 *  - RM handles in alloc/control/free are checked against the per-client
 *    object graph before the real ioctl is issued.
 *  - GPA ranges for mmap are allocated from a dedicated pool managed by QEMU;
 *    the guest cannot influence which HPA a GPA maps to.
 */

#ifndef VIRTIO_NVGPU_H
#define VIRTIO_NVGPU_H

/*
 * NVKVM_QEMU_GRAPHICS — compile-time graphics/display gate (default 1).
 *
 *   1 (default): full backend — DRM render node forwarding, NVKMS, and the
 *                host present/EGL path are built in; per-VM availability is
 *                still chosen at runtime via the `graphics=on|off` device prop.
 *   0          : compute-only build, like gVisor's nvproxy. The graphics= prop
 *                is forced off (all runtime graphics gates fire) AND the host
 *                EGL present code (nvkvm_present_egl.c) is compiled out, so the
 *                binary carries no display attack surface. This is the QEMU twin
 *                of the guest module's `make NVKVM_GRAPHICS=0` build; deploy the
 *                two consistently.
 *
 * Override at build time with -DNVKVM_QEMU_GRAPHICS=0.
 */
#ifndef NVKVM_QEMU_GRAPHICS
#define NVKVM_QEMU_GRAPHICS 1
#endif

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/queue.h>

/* Bring in QEMU's virtio helpers */
#include "hw/virtio/virtio.h"
#include "hw/pci/pci.h"
#include "qemu/iov.h"
/* exec/memory.h (for MemoryRegion) is transitively included via virtio.h */

#include "../../src/common/nvkvm_proto.h"
#include "../../src/common/nvkvm_isolate_proto.h"
#include "../../src/common/nvkvm_abi.h"
#include "nvkvm_log.h"
#include "../../src/abi/nvgpu.h"
#include "../../src/abi/uvm.h"
#include "nvkvm_handle.h"
#include "nvkvm_isolate.h"

/* ── Device constants ────────────────────────────────────────────────────── */

/* Virtio device type for NVIDIA GPU ioctl passthrough.
 * 50 is unassigned by the virtio spec; PCI device ID = 0x1040 + 50 = 0x1072.
 * Must be in range 0–63 so the PCI ID stays within 0x1040–0x107f (the valid
 * range the Linux virtio-pci driver recognizes).
 * Must match VIRTIO_ID_NVGPU in src/guest/nvkvm.h. */
#define VIRTIO_ID_NVGPU             50

/*
 * Guest-physical address layout for the nvkvm GPA windows.
 *
 * PORTABILITY — host MAXPHYADDR.  These three bases used to be compile-time
 * constants at 1 TB (shm), 1.5 TB (legacy mmap window) and 2 TB (sparse
 * window).  A GPA of 1 TB needs 41 physical address bits; the sparse window's
 * top (2 TB + 128 GiB) needs 42.  Every host this was developed on was a
 * server part (EPYC / Xeon, 46-52 bits), so it always fit.  Consumer mobile
 * Intel ships **39 bits** — an i7-11800H tops out at 512 GiB of GPA — and
 * there KVM rejects the very first window at device realize:
 *
 *   KVM_SET_USER_MEMORY_REGION failed, slot=6, start=0x10000000000,
 *                               size=0x1000000: Invalid argument
 *   kvm_set_phys_mem: error registering slot: Invalid argument
 *
 * and QEMU dies before the guest boots.  (Note the failing slot is the 16 MiB
 * *shm* region, not the sparse window: the whole fixed layout is affected, not
 * just the big one.)
 *
 * The bases are therefore computed at realize time from the *narrower* of the
 * host's and the guest's physical address width — see nvkvm_gpa_layout_compute
 * in nvkvm_mmap_host.c.  Only the sizes stay compile-time.
 */

/* Legacy per-mmap window (still used by /dev/nvidia-uvm mappings, which cannot
 * be MAP_FIXED into the sparse window). */
#define NVKVM_MMAP_WIN_SIZE         (16ULL << 30)     /* 16 GiB window */

/*
 * Sparse GPA window: a large VMM-backed GPA range used for memory-ioctl
 * GPA assignments where the guest doesn't yet have backing for the
 * userspace VA (cuMemAlloc, cuMallocManaged, sparse mmaps, etc.).
 *
 * VMM backing: one anon MAP_NORESERVE region of this size in QEMU's
 * mm.  KVM_SET_USER_MEMORY_REGION at startup maps the whole window
 * GPA -> VMM_VA in one slot.  Host kernel demand-faults pages on
 * first access, KVM never sees the fault, guest never sees the fault.
 *
 * Lives above the existing 16 GB mmap window.
 */
#define NVKVM_SPARSE_GPA_SIZE       (128ULL << 30)    /* 128 GiB sparse window */

/*
 * Floor for a deliberate shrink.  The sparse window is large because CUDA fires
 * >1500 individual mmaps during a single cuCtxCreate and the single-memslot
 * window is what keeps those off KVM's ~509-memslot budget.  What matters is
 * therefore the window's *total byte capacity*, not the mmap count: 16 GiB
 * still admits >1500 mappings averaging ~10 MiB, which covers a cuCtxCreate on
 * a consumer card.  We never go below this silently — see the shrink log line
 * and the "cannot fit" error in nvkvm_gpa_layout_compute.
 */
#define NVKVM_SPARSE_GPA_SIZE_MIN   (16ULL << 30)     /* 16 GiB floor */

/* A whole 1 GiB slot is reserved for the shm region (which is only
 * NVKVM_SHM_NSLOTS * slot_size, ~16 MiB) so every later region in the block
 * stays 1 GiB aligned regardless of the slot geometry. */
#define NVKVM_SHM_GPA_SLOT          (1ULL << 30)      /* 1 GiB */

/* Minimum alignment of the whole block (the old base was 1 TiB aligned; the
 * requirement is "at least 1 GiB"). */
#define NVKVM_GPA_ALIGN             (1ULL << 30)      /* 1 GiB */

/*
 * Resolved GPA layout, computed once at device realize.
 *
 * The three regions are placed as one contiguous block:
 *
 *   block_base + 0                     shm       (1 GiB slot, ~16 MiB used)
 *   block_base + 1 GiB                 mmap_win  (NVKVM_MMAP_WIN_SIZE)
 *   block_base + 1 GiB + mmap_size     sparse    (sparse_size)
 *
 * The block is placed top-down: as high as it can go while still ending at or
 * below 2^bits, which keeps it as far as possible from guest RAM and from the
 * bottom-up 64-bit PCI BAR allocator.
 */
struct nvkvm_gpa_layout {
	uint64_t shm_base;
	uint64_t mmap_base;
	uint64_t mmap_size;
	uint64_t sparse_base;
	uint64_t sparse_size;
	uint64_t block_base;
	uint64_t block_size;
	uint64_t limit;        /* 1 << bits — one past the last usable GPA   */
	uint64_t ram_top;      /* conservative top of guest RAM              */
	uint64_t floor;        /* lowest GPA the block is allowed to start at */
	uint32_t host_bits;    /* host MAXPHYADDR (CPUID 0x80000008)         */
	uint32_t guest_bits;   /* guest MAXPHYADDR (CPU "phys-bits" prop)    */
	uint32_t bits;         /* the binding minimum of the two            */
	bool     shrunk;       /* sparse window was reduced to make it fit   */
};

/* ── Object graph (mirrors gVisor nvproxy object.go) ────────────────────── */

struct nvkvm_object;

struct nvkvm_object_impl {
	void (*release)(struct nvkvm_object *obj);
};

struct nvkvm_object {
	uint32_t  handle;
	uint32_t  class_id;
	uint32_t  parent_handle;

	/* dependency graph */
	struct nvkvm_object **deps;   /* objects this depends on      */
	int                  ndeps;
	struct nvkvm_object **rdeps;  /* objects that depend on this  */
	int                 nrdeps;

	const struct nvkvm_object_impl *impl;

	/* for free-list traversal */
	struct nvkvm_object *free_next;
};

/* ── Root client (NV01_ROOT_CLIENT) ──────────────────────────────────────── */

#define NVKVM_MAX_OBJECTS_PER_CLIENT  4096

struct nvkvm_client {
	uint32_t           handle;

	pthread_mutex_t    lock;
	struct nvkvm_object *resources[NVKVM_MAX_OBJECTS_PER_CLIENT];
	int                 nresources;
	bool                released;

	/* captured params for checkpoint/restore (future) */
};

/* ── Per-FD state (one per host fd opened on behalf of guest) ────────────── */

struct nvkvm_host_fd {
	int       fd;           /* real host fd                          */
	int       dev_id;       /* NVKVM_DEV_*                           */
	uint32_t  token;        /* fd_token as seen by guest             */
	uint32_t  session_id;   /* owning session                        */

	/* clients owned by this fd (NV01_ROOT_CLIENT allocations) */
	struct nvkvm_client **clients;
	int                   nclients;
	pthread_mutex_t       clients_lock;

	TAILQ_ENTRY(nvkvm_host_fd) link;
};

/* ── Mmap region (one per active GPU memory mapping) ─────────────────────── */

struct nvkvm_mmap_region {
	uint32_t  token;
	void     *host_va;      /* address returned by host mmap()       */
	size_t    length;
	uint64_t  guest_pa;     /* GPA we assigned in the guest          */
	int       kvm_slot;     /* KVM memory slot (-1 if not mapped)    */

	TAILQ_ENTRY(nvkvm_mmap_region) link;
};

/* ── Per-session state ────────────────────────────────────────────────────── */

#define NVKVM_MAX_FDS_PER_SESSION  256

/* #80/H-1: capacity of the per-VM sparse-window free-list (recycled extents). */
#define NVKVM_GPA_FREE_MAX 16384

struct nvkvm_session {
	uint32_t  id;
	pid_t     guest_tgid;

	pthread_mutex_t lock;

	/* fd table: fd_token → nvkvm_host_fd (legacy path) */
	TAILQ_HEAD(, nvkvm_host_fd)    fds;
	uint32_t                        next_fd_token;

	/* mmap region table */
	TAILQ_HEAD(, nvkvm_mmap_region) mmaps;
	uint32_t                         next_mmap_token;

	/* global client map: handle → nvkvm_client (RM object graph) */
	struct nvkvm_client   *clients[NVKVM_MAX_OBJECTS_PER_CLIENT];
	int                    nclients;
	pthread_mutex_t        clients_lock;

	/* isolate IDs active in this session (one per guest mm) */
	uint32_t isolate_ids[256];
	int      nisolates;

	TAILQ_ENTRY(nvkvm_session) link;
};

/* ── Virtio device state ─────────────────────────────────────────────────── */

typedef struct VirtIONvgpu {
	VirtIODevice        parent_obj;

	/* Virtqueues */
	VirtQueue          *vq_tx;
	VirtQueue          *vq_rx;
	VirtQueue          *vq_evt;

	/* Shared memory region */
	void               *shm_base;
	size_t              shm_size;
	size_t              slot_size;
	uint64_t            shm_gpa;    /* GPA where guest sees shared mem  */
	MemoryRegion        shm_mr;     /* QEMU memory region for shm_base  */
	bool                shm_mr_registered;

	/* Virtio config space (little-endian, copied out by get_config) */
	struct nvkvm_virtio_config config_space;

	/* qdev property: enable the graphics stack (DRM render node + NVKMS).
	 * Default true; set graphics=off for compute-only VMs to drop the DRM/
	 * NVKMS device + ioctl attack surface. QEMU enforces it (rejects those
	 * device opens + ioctls) regardless of the untrusted guest. */
	bool                graphics;

	/*
	 * The isolation mode actually in force, resolved at realize (mode `auto`
	 * probes the ladder, so the configured value and the effective value can
	 * differ).  Exposed as the read-only QOM property "isolate-mode-active"
	 * so an operator or a monitoring check can read it back:
	 *     (qemu) qom-get /machine/peripheral/<id> isolate-mode-active
	 * The failure mode this defends against is an operator who believes they
	 * have namespace isolation and has no way to check.
	 */
	char               *isolate_mode_active;

	/* Mmap window: GPA range for GPU memory mappings */
	uint64_t            mmap_win_gpa;
	size_t              mmap_win_size;
	uint64_t            mmap_win_cur;   /* next available GPA offset    */
	pthread_mutex_t     mmap_win_lock;

	/*
	 * Sparse GPA window — see struct nvkvm_gpa_layout in the comment block
	 * above.  sparse_gpa_base / sparse_size are GPA-space; sparse_vmm_va
	 * is the MAP_NORESERVE anon region in QEMU's mm that backs the slot.
	 * sparse_cur is the next free GPA offset; sparse_kvm_slot is the
	 * KVM memory slot ID we installed at device realize.
	 */
	uint64_t            sparse_gpa_base;
	size_t              sparse_size;
	void               *sparse_vmm_va;
	uint64_t            sparse_cur;
	int                 sparse_kvm_slot;
	pthread_mutex_t     sparse_lock;

	/*
	 * #80 / audit H-1: free-list of returned GPA extents (offsets into the
	 * sparse window).  Without this, sparse_cur was a no-free bump pointer:
	 * a guest looping mmap/munmap (or cuMemAlloc/Free) leaked window space
	 * irrecoverably until all GPU mmaps failed (host-visible DoS, hits even
	 * a well-behaved long-lived guest).  munmap + isolate-kill now return
	 * extents here; alloc reuses them (first-fit) before advancing sparse_cur.
	 * Guarded by sparse_lock.
	 */
	struct nvkvm_gpa_extent { uint64_t off; uint64_t len; } *sparse_free;
	uint32_t            sparse_free_n;

	/*
	 * #55: the sparse window's GPA is the firmware-assigned base of the
	 * reservation BAR (so QEMU/PCI never place anything else there), not a
	 * hardcoded constant.  The PCI proxy sets window_base_get to a callback
	 * that returns the BAR's current GPA (0 until the guest programs it).
	 * The raw KVM memslot is installed lazily once the base is known
	 * (nvkvm_sparse_ensure); if no BAR/callback, we fall back to the fixed
	 * computed gpa.sparse_base so a transport without the BAR still works.
	 */
	uint64_t          (*window_base_get)(void *opaque);
	void               *window_base_opaque;

	/*
	 * Resolved GPA layout for this VM (host/guest MAXPHYADDR derived).
	 * Filled once in virtio_nvgpu_device_realize before any region is
	 * registered; every window base below comes from here rather than from
	 * a compile-time constant.  The PCI proxy also reads gpa.sparse_size to
	 * size its reservation BAR, so a shrunk window shrinks the BAR too.
	 */
	struct nvkvm_gpa_layout gpa;

	/* Session table */
	TAILQ_HEAD(, nvkvm_session) sessions;
	pthread_mutex_t             sessions_lock;
	uint32_t                    next_session_id;

	/* Handle and isolate managers (isolate architecture) */
	struct nvkvm_handle_table   handles;
	struct nvkvm_isolate_table  isolates;

	/*
	 * Phase 4 — per-VM RM client-handle allowlist.  nvidia hClient ids are
	 * a GLOBAL, access-gated namespace (not fd-scoped): an ioctl can name a
	 * client created on another fd/process if share rights allow.  Combined
	 * with the Path-alpha TYPE_ALL DUP grant, a guest could DUP another VM's
	 * object by naming its (h_client_src, h_src_object).  We record every
	 * hClient this VM's isolates successfully use and reject any forwarded
	 * ioctl that references a foreign hClient (e.g. DUP_OBJECT h_client_src).
	 * Grow-only is safe: a freed handle still belonged to this VM, and the
	 * kernel rejects a stale handle anyway.
	 */
#define NVKVM_CLIENT_ALLOWLIST_MAX 8192
	uint32_t            client_allow[NVKVM_CLIENT_ALLOWLIST_MAX];
	uint32_t            client_allow_n;
	pthread_mutex_t     client_allow_lock;

	/* Host NVIDIA driver version (read at init) */
	char                driver_version[64];
	/* #81: per-version ABI profile selected from driver_version at realize. */
	const struct nvkvm_abi_profile *abi;

	/*
	 * #66 — QEMU's own init-ns admin RM subdevice, used only to answer
	 * GET_PID_INFO (per-process VRAM for nvidia-smi).  The stub queries from
	 * inside CLONE_NEWPID/NEWUSER, where the driver attributes 0 bytes; QEMU
	 * is in the host init ns, where GET_PID_INFO returns the real value.
	 * Lazily allocated on first use; freed when the device's fd closes.
	 */
	pthread_mutex_t     admin_lock;
	int                 admin_ctl_fd;   /* /dev/nvidiactl (QEMU's process) */
	int                 admin_gpu_fd;   /* /dev/nvidia0                    */
	uint32_t            admin_hclient;
	uint32_t            admin_hsubdev;
	int                 admin_state;    /* 0 untried, 1 ready, -1 failed   */

	/*
	 * #102 present-to-window: opaque NvkvmPresent context (QemuConsole +
	 * pending-frame slot + dual GL/readback path).  Allocated at realize
	 * when graphics is on; owned by nvkvm_present_egl.c.  NULL in the
	 * compute-only build or when graphics=off.  void* so this header stays
	 * free of ui/console.h.
	 */
	void               *present_ctx;
} VirtIONvgpu;

#define TYPE_VIRTIO_NVGPU  "virtio-nvgpu-device"
#define VIRTIO_NVGPU(obj)  OBJECT_CHECK(VirtIONvgpu, (obj), TYPE_VIRTIO_NVGPU)

/* ── Handler context (per in-flight request) ─────────────────────────────── */

struct nvkvm_req_ctx {
	VirtIONvgpu       *nv;
	VirtQueue         *vq;
	VirtQueueElement  *elem;

	struct nvkvm_session  *session;
	struct nvkvm_host_fd  *hfd;

	/* Pointers into shared memory (already validated for bounds) */
	void              *params_buf;
	size_t             param_size;
	void              *aux_buf;
	size_t             aux_size;

	/* For mmap requests */
	struct nvkvm_mmap_region *mmap_region;
};

/* ── Function declarations ───────────────────────────────────────────────── */

/* virtio_nvgpu.c */
void virtio_nvgpu_init(VirtIONvgpu *nv);
void virtio_nvgpu_fini(VirtIONvgpu *nv);
/* #127: async os-event delivery — isolate reader thread → vq_evt → guest poll_wq.
 * Safe to call from any thread; hops onto the device AioContext internally. */
void nvkvm_virtio_push_evt(VirtIONvgpu *nv, uint32_t isolate_id,
			   uint32_t handle_id, uint32_t revents);

/* nvkvm_dispatch.c */
int  nvkvm_dispatch_ioctl(struct nvkvm_req_ctx *ctx,
			  unsigned int cmd);
/* #81: two UVM param sizes are driver-version-variant, so the expected size
 * depends on the active ABI profile — pass it in. */
size_t nvkvm_ioctl_expected_param_size(unsigned int cmd,
                                       const struct nvkvm_abi_profile *prof);

/* nvkvm_frontend.c */
int nvkvm_handle_rm_alloc(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_rm_free(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_rm_control(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_rm_dup_object(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_register_fd(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_alloc_os_event(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_free_os_event(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_simple_ioctl(struct nvkvm_req_ctx *ctx, unsigned int cmd);

/* nvkvm_isolate_handlers.c — new isolate/handle virtio request handlers */
int nvkvm_req_list_nvidia_devices(VirtIONvgpu *nv,
				   struct nvkvm_req_list_nvidia_devices *req,
				   struct nvkvm_resp_list_nvidia_devices *resp);
int nvkvm_req_open_nvidia_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_open_nvidia_handle *req,
				  struct nvkvm_resp_open_nvidia_handle *resp);
int nvkvm_req_present(VirtIONvgpu *nv,
		      struct nvkvm_req_present *req,
		      struct nvkvm_resp_present *resp);
int nvkvm_req_xiso_import(VirtIONvgpu *nv,
			  struct nvkvm_req_xiso_import *req,
			  struct nvkvm_resp_xiso_import *resp);
int nvkvm_req_open_memory_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_open_memory_handle *req,
				  struct nvkvm_resp_open_memory_handle *resp);
int nvkvm_req_close_handle(VirtIONvgpu *nv,
			    struct nvkvm_req_close_handle *req,
			    struct nvkvm_resp_close_handle *resp);
int nvkvm_req_create_isolate(VirtIONvgpu *nv,
			      struct nvkvm_req_create_isolate *req,
			      struct nvkvm_resp_create_isolate *resp);
int nvkvm_req_kill_isolate(VirtIONvgpu *nv,
			    struct nvkvm_req_kill_isolate *req,
			    struct nvkvm_resp_kill_isolate *resp);
int nvkvm_req_interrupt(VirtIONvgpu *nv,
			struct nvkvm_req_interrupt *req,
			struct nvkvm_resp_interrupt *resp);
int nvkvm_req_setup_ring(VirtIONvgpu *nv,
			 struct nvkvm_req_setup_ring *req,
			 struct nvkvm_resp_setup_ring *resp);
int nvkvm_req_enter_loop(VirtIONvgpu *nv,
			 struct nvkvm_req_enter_loop *req,
			 struct nvkvm_resp_enter_loop *resp);
int nvkvm_req_copy_handle_to_isolate(VirtIONvgpu *nv,
				      struct nvkvm_req_copy_handle_to_isolate *req,
				      struct nvkvm_resp_copy_handle_to_isolate *resp);
int nvkvm_req_close_handle_on_isolate(VirtIONvgpu *nv,
				       struct nvkvm_req_close_handle_on_isolate *req,
				       struct nvkvm_resp_close_handle_on_isolate *resp);
int nvkvm_req_ioctl_on_isolate(VirtIONvgpu *nv,
				struct nvkvm_req_ioctl_on_isolate *req,
				struct nvkvm_resp_ioctl_on_isolate *resp,
				void *param_buf, void *aux_buf);
int nvkvm_req_mmap_on_isolate(VirtIONvgpu *nv,
			       struct nvkvm_req_mmap_on_isolate *req,
			       struct nvkvm_resp_mmap_on_isolate *resp);
int nvkvm_req_munmap_on_isolate(VirtIONvgpu *nv,
				 struct nvkvm_req_munmap_on_isolate *req,
				 struct nvkvm_resp_munmap_on_isolate *resp);
int nvkvm_req_poll_on_isolate(VirtIONvgpu *nv,
			       struct nvkvm_req_poll_on_isolate *req,
			       struct nvkvm_resp_poll_on_isolate *resp);
int nvkvm_req_unpoll_on_isolate(VirtIONvgpu *nv,
				 struct nvkvm_req_unpoll_on_isolate *req,
				 struct nvkvm_resp_unpoll_on_isolate *resp);
int nvkvm_req_write_memory_handle(VirtIONvgpu *nv,
				   struct nvkvm_req_write_memory_handle *req,
				   struct nvkvm_resp_write_memory_handle *resp,
				   void *data_buf);
int nvkvm_req_read_memory_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_read_memory_handle *req,
				  struct nvkvm_resp_read_memory_handle *resp,
				  void *data_buf);
int nvkvm_req_realize_uvm_mapping(VirtIONvgpu *nv,
				   struct nvkvm_req_realize_uvm_mapping *req,
				   struct nvkvm_resp_realize_uvm_mapping *resp,
				   void *state_buf, void *intent_buf);
int nvkvm_req_read_host_file(VirtIONvgpu *nv,
			      struct nvkvm_req_read_host_file *req,
			      struct nvkvm_resp_read_host_file *resp,
			      void *shm_buf);

/* nvkvm_objects.c */
struct nvkvm_client *nvkvm_client_alloc(uint32_t handle);
void                 nvkvm_client_free(struct nvkvm_session *session,
				       struct nvkvm_client *client);
struct nvkvm_object *nvkvm_obj_lookup(struct nvkvm_client *client,
				      uint32_t handle);
int  nvkvm_obj_add(struct nvkvm_client *client, uint32_t handle,
		   uint32_t class_id, uint32_t parent_handle,
		   const struct nvkvm_object_impl *impl);
void nvkvm_obj_free(struct nvkvm_client *client, uint32_t handle);
void nvkvm_obj_add_dep(struct nvkvm_client *client,
		       uint32_t h1, uint32_t h2);

/* nvkvm_mmap_host.c */
void nvkvm_set_kvm_vm_fd(int fd);

/*
 * Physical-address-width probes and GPA window placement (nvkvm_mmap_host.c).
 *
 * nvkvm_host_phys_bits(): the host's MAXPHYADDR, from CPUID leaf 0x80000008
 * EAX[7:0], falling back to /proc/cpuinfo's "address sizes" line.  0 if
 * neither could be read.
 *
 * nvkvm_guest_phys_bits(): the guest's MAXPHYADDR, read generically off the
 * first CPU's "phys-bits" QOM property (x86 resolves this at CPU realize, and
 * -cpu host/max default it to the host's width).  0 if unavailable.
 *
 * nvkvm_gpa_layout_compute(): resolve the whole window block.  Returns true on
 * success; on false, errbuf holds a human-readable explanation naming the host
 * width, the guest RAM top, the window size and what would have fit.
 */
uint32_t nvkvm_host_phys_bits(void);
uint32_t nvkvm_guest_phys_bits(void);
bool     nvkvm_gpa_layout_compute(struct nvkvm_gpa_layout *out,
				  char *errbuf, size_t errlen);

/*
 * Sparse GPA window helpers.  See struct nvkvm_gpa_layout.
 *
 * nvkvm_sparse_init: called once at device realize.  mmaps the VMM-side
 * window as MAP_NORESERVE | MAP_ANONYMOUS and installs the KVM region.
 * Returns 0 on success, -errno on failure.  Subsequent allocations from
 * nvkvm_sparse_gpa_alloc only succeed after this returns 0.
 *
 * nvkvm_sparse_gpa_alloc(size): hand out a fresh GPA in the sparse
 * window.  size is rounded up to PAGE_SIZE.  Returns 0 if exhausted.
 * No backing is allocated — host kernel demand-faults on first access.
 *
 * nvkvm_gpa_to_vmm_va(gpa, size): translate a GPA in the sparse window
 * to a VMM-mm void*.  Returns NULL if gpa+size is outside the window.
 * The returned pointer is stable for the lifetime of the device.
 */
int   nvkvm_sparse_init(VirtIONvgpu *nv);
void  nvkvm_sparse_fini(VirtIONvgpu *nv);
uint64_t nvkvm_sparse_gpa_alloc(VirtIONvgpu *nv, size_t size);
/* #80/H-1: return a GPA extent to the window free-list (recycled by alloc). */
void nvkvm_sparse_gpa_free(VirtIONvgpu *nv, uint64_t gpa, size_t size);
void *nvkvm_gpa_to_vmm_va(VirtIONvgpu *nv, uint64_t gpa, size_t size);

/* #80/H-2: tear down a session — close its handles, free its RM object graph,
 * fd list, and the struct itself.  Called when the session's last isolate is
 * killed.  Caller must NOT hold nv->sessions_lock. */
void nvkvm_session_destroy(VirtIONvgpu *nv, struct nvkvm_session *session);
/* #55: resolve the window base (BAR-assigned, or fixed fallback) and lazily
 * install the raw KVM memslot there.  Idempotent; returns the base GPA (0 on
 * failure).  Safe to call from get_config and the alloc path. */
uint64_t nvkvm_sparse_ensure(VirtIONvgpu *nv);
VirtIONvgpu *nvkvm_get_global_device(void);
void nvkvm_mmap_win_alloc(VirtIONvgpu *nv, size_t length, uint64_t *gpa_out);

/*
 * KVM memory-slot pool — one freelist+watermark shared by every code path
 * that registers a region (mmap_create, mmap_on_isolate, realize-UVM).
 * alloc returns -1 on exhaustion; release is a no-op for invalid slots.
 * Defined in nvkvm_mmap_host.c.
 */
int  nvkvm_kvm_slot_alloc(void);
void nvkvm_kvm_slot_release(int slot);
void nvkvm_kvm_slot_stats(int *in_use, int *peak,
			  uint64_t *allocs, uint64_t *frees);
int  nvkvm_mmap_create(VirtIONvgpu *nv, struct nvkvm_host_fd *hfd,
		       uint64_t offset, size_t length,
		       int prot, int flags,
		       struct nvkvm_mmap_region **region_out);
void nvkvm_mmap_destroy(VirtIONvgpu *nv,
			struct nvkvm_mmap_region *region);
int  nvkvm_mmap_map_to_guest(VirtIONvgpu *nv,
			     struct nvkvm_mmap_region *region);
void nvkvm_mmap_unmap_from_guest(VirtIONvgpu *nv,
				 struct nvkvm_mmap_region *region);

/* nvkvm_ptr.c — pointer translation for ioctl secondary buffers */
int nvkvm_translate_ptr_to_host(struct nvkvm_req_ctx *ctx,
				uint64_t guest_p64, size_t size,
				void **host_ptr_out);

/* Session helpers */
struct nvkvm_session *nvkvm_session_find(VirtIONvgpu *nv, uint32_t session_id);
struct nvkvm_session *nvkvm_session_create(VirtIONvgpu *nv, uint32_t guest_tgid);
struct nvkvm_host_fd *nvkvm_fd_lookup(struct nvkvm_session *session,
				      uint32_t fd_token);
uint32_t             nvkvm_fd_alloc_token(struct nvkvm_session *session,
					  struct nvkvm_host_fd *hfd);
void                 nvkvm_fd_remove(struct nvkvm_session *session,
				     uint32_t fd_token);

#endif /* VIRTIO_NVGPU_H */
