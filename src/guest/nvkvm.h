/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nvkvm.h — internal header for the nvkvm-guest kernel module
 */

#ifndef NVKVM_H
#define NVKVM_H

#include <linux/cdev.h>
#include "nvkvm_compat.h"
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/refcount.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/list.h>

#include "../../src/common/nvkvm_proto.h"
#include "../../src/common/nvkvm_abi.h"
#include "../../src/common/nvkvm_nvkms_ops.h"
#include "../../src/common/nvkvm_ring.h"
#include "../../src/common/nvkvm_ring_ioctl.h"
#include "../../src/abi/nvgpu.h"

/* virtio-nvgpu device ID.
 * Must be in the range 0–63 so the PCI device ID (0x1040 + type) falls in
 * the valid modern virtio PCI range 0x1040–0x107f.  50 is unassigned by the
 * virtio spec; PCI device ID = 0x1072.
 * Must match VIRTIO_ID_NVGPU in src/qemu/virtio_nvgpu.h. */
#define VIRTIO_ID_NVGPU   50

/* ── Per-session state (one per guest process that has opened a device) ───── */

/*
 * A session groups all FDs opened by a single guest process (tgid). When the
 * process exits, all host FDs belonging to its session are closed and all RM
 * objects belonging to its clients are freed.
 *
 * This mirrors gVisor's per-container nvproxy state, providing isolation:
 * each guest process sees only its own GPU contexts.
 */
struct nvkvm_session {
	/*
	 * Sessions are keyed by `mm` (address-space identity), not by tgid.
	 * Linux reuses tgids after a process exits; if a previous tgid-keyed
	 * session lingered (refcount race) the new process with the same tgid
	 * would inherit the stale session, isolate, and handle table — a
	 * cross-uid info-leak path inside one VM.  `mm_struct *` is a strong
	 * identity (refcounted, never reused while alive), so equal-mm means
	 * same process; threads share an mm so they share a session (correct);
	 * fork creates a new mm so the child gets a new session (correct).
	 * We hold a refcount on the mm via mmget() in get_or_create and drop
	 * it in put.  Audit H2.
	 */
	struct mm_struct *mm;
	pid_t   tgid;           /* for logging/diagnostics only           */
	/*
	 * Refcounted pid of the owning thread-group leader, captured at
	 * session creation.  Stored as `struct pid *` (not a raw number) so
	 * that process enumeration (NV2080_CTRL_CMD_GPU_GET_PIDS synthesis)
	 * can render it in the *caller's* pid namespace via pid_vnr() — this
	 * makes nvidia-smi work correctly inside guest containers (Docker on
	 * the guest VM): a caller sees only the GPU processes visible in its
	 * own pid ns, numbered as that ns sees them.  A raw tgid would be a
	 * root-ns number, meaningless (and a cross-ns leak) inside a container.
	 */
	struct pid *tgid_pid;
	int     id;             /* IDR key                                */
	int     refcount;       /* protected by nvkvm_state.sessions_lock */
	__u32   isolate_id;     /* QEMU isolate process ID (0 = not yet created) */
	struct mutex isolate_lock; /* protects isolate_id creation       */

	/*
	 * Command-buffer ring (docs/design/command_buffer.md).  QEMU placed the
	 * ring memfd in the sparse GPA window at isolate create; we memremap it
	 * here so the kmd can run the SPSC fast path.  ring_base == NULL means no
	 * ring (fall back to the virtqueue).  Set up once under isolate_lock.
	 */
	void   *ring_base;          /* memremap'd ring region (NULL = none)   */
	u64     ring_gpa;           /* guest-physical base                    */
	u32     ring_region_size;
	u32     ring_bytes;         /* per-ring data bytes                    */
	struct nvkvm_ring *req_ring;  /* guest→isolate (we produce)           */
	struct nvkvm_ring *resp_ring; /* isolate→guest (we consume)           */

	/*
	 * Fast-path producer/consumer state (docs/design/command_buffer.md).
	 * ring_lock serialises guest producers so the request ring has a single
	 * producer AND the response ring a single consumer (the lock holder
	 * reads back its own response) — preserving the SPSC contract regardless
	 * of how many guest threads issue fast controls.  The pump kthread keeps
	 * the isolate spinning via ENTER_LOOP while there is queued work; it is a
	 * pure observer of the rings (reads tail via has_work, never their data).
	 */
	struct mutex      ring_lock;
	u32               ring_txn_next;  /* producer-private txn id counter   */
	struct task_struct *pump_task;    /* per-session ENTER_LOOP pump       */
	wait_queue_head_t pump_wq;        /* woken when a producer publishes   */

	/*
	 * UVM_VALIDATE_VA_RANGE cache (#94).  libcuda re-validates the SAME
	 * (base,len) range thousands of times per pageable cuMemcpy (~191µs
	 * forwarded each → the DtoH bottleneck).  VALIDATE is an idempotent
	 * registration check, so we cache its rm_status and serve repeats
	 * locally.  Conservatively cleared on ANY UVM free/unmap/unregister so a
	 * stale "valid" can never outlive a teardown.
	 */
#define NVKVM_VCACHE_N 16
	struct { u64 base, len; u32 status; bool valid; } vcache[NVKVM_VCACHE_N];
	u32               vcache_next;
	spinlock_t        vcache_lock;
};

/* ── Per-FD context (one per open(/dev/nvidia*)) ──────────────────────────── */

struct nvkvm_mmap_region {
	struct list_head list;
	/*
	 * One reference per VMA that carries this object in vm_private_data.
	 * Linux calls vm_ops.open when a VMA is copied or split and close once
	 * for every resulting VMA.  Managed fallback backing may therefore be
	 * released only after this count reaches zero, not on the first close.
	 */
	refcount_t vma_refs;
	__u32    mmap_token;       /* opaque token from host                */
	__u32    handle_id;        /* QEMU-side handle (for isolate re-map) */
	unsigned long gpa_base;    /* guest physical address                */
	unsigned long length;
	__u64    offset;           /* fd offset at which we were mapped     */
	struct vm_area_struct *vma; /* NULL after munmap                   */

	/*
	 * Managed-memory fallback (nvkvm_uvm_ext.c).  Set when this region is
	 * NOT a forwarded device mapping but a UVM MANAGED range this module
	 * downgraded to an EXTERNAL range over an RM sysmem object.
	 *
	 * Everything teardown needs is cached HERE rather than reached through
	 * the fd context, because teardown runs from vma_close(), which can fire
	 * after close(2) removed the userspace descriptor.  The VMA's vm_file
	 * reference keeps the struct file, and therefore `ctx`, alive until the
	 * last VMA close.  `ctx` is used only for mmap_lock below.
	 */
	struct nvkvm_fd_ctx *ctx;   /* owning UVM fd, for the claim lock     */
	/*
	 * The /dev/nvidiactl handle this ONE mapping owns.  RM arms a mapping
	 * context per `struct file` and never clears it (nv-usermap.c:52-56 vs
	 * nv_free_file_private), so a file carries exactly one mapping for its
	 * whole lifetime and a second NV_ESC_RM_MAP_MEMORY against it returns
	 * NV_ERR_STATE_IN_USE.  One handle per allocation, released with it.
	 */
	struct nvkvm_fd_ctx *ext_map_ctl;
	bool     ext_backed;
	bool     ext_claimed;       /* whoever sets this under mmap_lock owns
				     * the teardown; the other side steps aside */
	__u64    ext_gva;           /* the GPU VA it was published at        */
	__u32    ext_isolate_id;
	__u32    ext_session_id;
	__u32    ext_uvm_handle_id; /* the UVM fd's QEMU handle              */
	__u32    ext_ctl_handle_id; /* this mapping's /dev/nvidiactl handle  */
	/*
	 * The fd-wide /dev/nvidiactl handle that OWNS the RM client.  RM resolves
	 * a client handle against the file it was allocated on, so NV_ESC_RM_FREE
	 * issued through any other fd returns NV_ERR_INVALID_CLIENT (0x23) and the
	 * sysmem object -- pinned host pages -- is never released.  Measured: a
	 * 300-iteration alloc/free loop logged 0x23 on every free.
	 */
	__u32    ext_owner_handle_id;
	__u32    ext_h_client;
	__u32    ext_h_device;
	__u32    ext_h_memory;
};

/*
 * CPU page migration tracking — one entry per page pinned and uploaded to a
 * memfd for the isolate's CPU-userptr access.
 */
struct nvkvm_cpu_page {
	struct page    *page;      /* pinned guest physical page                */
	unsigned long   gva;       /* page-aligned GVA mapped in the isolate    */
	__u64           gpa;       /* GPA the memfd page lives at in our window */
	unsigned long   length;    /* bytes this entry covers: PAGE_SIZE for a
				    * copy entry, one migration chunk for a
				    * range entry.  Audit H-9 needs the extent,
				    * not just its first page, to take the
				    * guest PTEs down before the GPA is freed. */
	__u32           handle_id; /* QEMU memory handle wrapping the memfd     */
	__u32           mmap_token;/* token for MUNMAP_ON_ISOLATE cleanup       */
	__u32           prot;      /* PROT_* flags (read/write)                 */
	struct mm_struct *mm;      /* mm the GVA belongs to.  Held by an
				    * mmgrab() taken when the entry was
				    * recorded (audit H-9), so the struct stays
				    * allocated and may be passed to
				    * mmget_not_zero(); the ADDRESS SPACE is
				    * only alive while that succeeds.  Compared,
				    * never walked, in cpu_page_entry_live.    */
	struct list_head list;
};

/*
 * Per-fd UVM state for the state-machine refactor (see
 * docs/STATE_MACHINE_PLAN.md).  Only allocated when dev_id ==
 * NVKVM_DEV_UVM.  Holds the configuration state libcuda accumulates
 * via UVM_INITIALIZE / REGISTER_GPU / CREATE_RANGE_GROUP /
 * ALLOC_SEMAPHORE_POOL / ... ioctls.  Calls are still forwarded; the live
 * mirror distinguishes pre-created semaphore mappings from MANAGED mmap, and
 * also retains the bounded state for the dormant REALIZE_UVM_MAPPING path.
 */
struct nvkvm_uvm_gpu_reg {
	struct list_head list;
	__u8             gpu_uuid[16];
	__u32            rm_ctrl_fd_handle_id;
	__u32            h_client;
	__u32            h_smc_part_ref;
};
struct nvkvm_uvm_vas_reg {
	struct list_head list;
	__u8             gpu_uuid[16];
	__u32            rm_ctrl_fd_handle_id;
	__u32            h_client;
	__u32            h_va_space;
};
struct nvkvm_uvm_range_group {
	struct list_head list;
	__u64            range_group_id;
};
struct nvkvm_uvm_mapping_intent {
	struct list_head list;
	__u32            mode;        /* NVKVM_UVM_REALIZE_MODE_*   */
	__u64            base;        /* guest VA the intent covers */
	__u64            length;
	void            *params;      /* mode-specific params blob  */
	size_t           params_size;
};

/* The dormant REALIZE snapshot has fixed arrays of 16 GPUs, VA spaces and
 * range groups.  Mapping intents are different: active mmap classification
 * consumes them and UVM permits arbitrary non-overlapping semaphore pools.
 * Account the two actual allocations separately instead of deriving a count
 * from those unrelated wire arrays: small classification nodes are required,
 * while the much larger dormant replay blobs are optional.  Admission happens
 * before the host ioctl, so exhausting the metadata budget cannot leave a
 * host-successful semaphore pool which a later mmap mistakes for MANAGED. */
#define NVKVM_UVM_MAX_INTENT_META_BYTES (256U * 1024U)
#define NVKVM_UVM_MAX_REALIZE_BYTES     (256U * 1024U)
struct nvkvm_uvm_realization {
	struct list_head list;
	__u64            realize_token;
	__u64            gva;
	__u64            gpa;
	__u64            length;
};
struct nvkvm_uvm_fd_state {
	struct mutex     lock;
	bool             initialized;
	/* A lost/capped bookkeeping update must not change the result of an
	 * ioctl the host already accepted.  Instead it makes the shadow unusable
	 * and managed mmap fails closed until a successful DEINITIALIZE resets
	 * both the host state and this bounded mirror. */
	bool             shadow_valid;
	/* VA-space and range-group records are consumed only by the dormant
	 * REALIZE snapshot.  Losing that bounded mirror must not disable the
	 * ordinary managed-memory fallback, whose inputs remain intact. */
	bool             realize_valid;
	__u64            init_flags;
	/*
	 * Managed-memory fallback: the private RM object tree this UVM fd owns
	 * (nvkvm_uvm_ext.c).  Built lazily on the first managed allocation and
	 * torn down with the fd; a CUDA process that never calls
	 * cudaMallocManaged pays nothing for it.
	 *
	 * ext_lock serialises the whole synthesis, and has to: RM's mmap context
	 * is ONE-SHOT PER `struct file` (nv-mmap.c:503-509), so the
	 * NV_ESC_RM_MAP_MEMORY that arms it and the mmap that consumes it must
	 * not interleave with another thread's pair on the same ctl handle.
	 * When both UVM locks are needed, the order is ext_lock -> lock.  The
	 * tracked ioctl transaction and managed mmap both follow that order.
	 * ext_lock may span a blocking host request; lock never does.
	 */
	struct mutex     ext_lock;
	struct nvkvm_fd_ctx *ext_ctl;   /* private /dev/nvidiactl handle     */
	struct nvkvm_fd_ctx *ext_dev;   /* private /dev/nvidia0, REGISTER_FD */
	__u32            ext_h_client;
	__u32            ext_h_device;
	__u32            ext_h_subdev;
	bool             ext_ready;
	struct list_head registered_gpus;     /* nvkvm_uvm_gpu_reg */
	struct list_head registered_va_spaces;/* nvkvm_uvm_vas_reg */
	struct list_head range_groups;        /* nvkvm_uvm_range_group */
	struct list_head intents;             /* nvkvm_uvm_mapping_intent */
	struct list_head realizations;        /* nvkvm_uvm_realization */
	__u32            n_registered_gpus;
	__u32            n_registered_va_spaces;
	__u32            n_range_groups;
	__u32            n_intents;
	size_t           intent_bytes;
	size_t           realize_bytes;
};

struct nvkvm_fd_ctx {
	__u32                  handle_id;   /* QEMU-side nvidia handle ID */
	int                    dev_id;      /* NVKVM_DEV_*                */
	struct nvkvm_session  *session;

	/*
	 * Lifetime refcount (G-6).  One reference for the open file; each proxy
	 * GEM minted on this ctx (nvkvm_drm.c) takes another, because a proxy
	 * can outlive its drm_file via a cross-file PRIME re-import yet still
	 * forwards GEM ops (GEM_CLOSE / GEM_EXPORT_NVKMS_MEMORY) on this exact
	 * ctx — the stub handle is only valid in the host fd this ctx owns.
	 * Teardown (handle close, session put, free) is deferred until the last
	 * ref drops, which also keeps the stub-side bo alive for the importer.
	 */
	refcount_t             refs;

	/* poll support */
	wait_queue_head_t      poll_wq;
	atomic_t               poll_events; /* cached POLL* bits from host         */
	atomic_t               poll_armed;  /* #127: 1 while the host is asked to
					     * relay this fd's readiness (one-shot;
					     * cleared on delivery so the next wait
					     * re-arms). Avoids the ~18ms blocking-
					     * sync poll-timeout fallback.            */
	struct list_head       evt_node;    /* #101: node in the async-event registry,
					     * keyed by (isolate_id, handle_id), so a
					     * VQ_EVT notification can find + wake us  */

	/* mmap regions owned by this FD */
	spinlock_t             mmap_lock;
	struct list_head       mmap_regions;

	/* CPU pages migrated to the isolate for userptr access */
	struct mutex           cpu_pages_lock;
	struct list_head       cpu_pages;

	/* State-machine state for UVM fds (NULL for non-UVM fds). */
	struct nvkvm_uvm_fd_state *uvm_state;
};

/* ── In-flight request tracking ───────────────────────────────────────────── */

struct nvkvm_inflight {
	struct list_head    list;
	__u32               txn_id;
	struct completion   done;
	/* response fields filled by virtio TX completion callback */
	__u64               retval;
	int                 status;   /* 0 = success, errno otherwise          */
	void               *resp_buf; /* IN-sg buffer holding the host response */
	/* extended fields for isolate-path responses */
	__u64               fault_addr; /* GVA of SIGSEGV in isolate (IOCTL_ON_ISOLATE) */
	__u32               nvstatus;   /* NvStatus from NVIDIA params              */
	/* When nonzero, nvkvm_send_sync waits interruptibly and, on a pending
	 * signal, asks this isolate to interrupt the in-flight ioctl (txn_id).
	 * Set only on the IOCTL_ON_ISOLATE path; control-plane reqs leave it 0
	 * and wait uninterruptibly. */
	__u32               isolate_id;
};

/* ── Global module state ──────────────────────────────────────────────────── */

#define NVKVM_MAX_GPUS  (NV_MINOR_DEVICE_NUMBER_REGULAR_MAX + 1)

/* "DDDD:BB:DD.F" — a PCI BDF string as it appears under
 * /proc/driver/nvidia/gpus/.  Matches NVKVM_BDF_LEN on the QEMU side. */
#define NVKVM_BDF_STRLEN  12

struct nvkvm_state {
	/* Device registration */
	struct class  *class;
	unsigned int   ctl_major;
	struct cdev    ctl_cdev;
	unsigned int   gpu_major;
	dev_t          gpu_devno_base;
	struct cdev    gpu_cdevs[NVKVM_MAX_GPUS];
	int            num_gpus;
	/* Host BDF of each GPU, as reported by the host's own
	 * /proc/driver/nvidia/gpus/ tree ("0000:00:07.0").  libcuda and NVML
	 * look per-GPU files up by the BDF that RM reports — which is the HOST
	 * BDF, not the guest slot — so the synthetic proc tree has to be named
	 * with these.  Filled by nvkvm_hostfile_discover_gpus(). */
	char           gpu_bdf[NVKVM_MAX_GPUS][NVKVM_BDF_STRLEN + 1];
	unsigned int   uvm_major;
	dev_t          uvm_devno;
	struct cdev    uvm_cdev;
	/* /dev/nvidia-modeset (NVKMS) — graphics config device (major 195 min 254) */
	struct cdev    modeset_cdev;
	bool           modeset_registered;
	/* /dev/dri/renderD128 (nvidia-drm) — graphics device (real DRM driver) */
	struct drm_device *drm_dev;
	/* Graphics enabled by QEMU (NVKVM_CONFIG_F_GRAPHICS); read at probe.
	 * When false the guest tears down modeset + skips the DRM render node. */
	bool           graphics_enabled;

	/* Session management */
	struct mutex   sessions_lock;
	struct idr     sessions_idr;

	/* Virtio transport */
	struct virtio_device   *vdev;
	struct virtqueue       *vq_tx;
	struct virtqueue       *vq_rx;
	struct virtqueue       *vq_evt;

	/* Shared memory region (BAR 0) */
	void __iomem           *shm_base;
	size_t                  shm_size;
	size_t                  slot_size;

	/* In-flight request tracking.
	 *
	 * txn_id is u32; the guest module assigns one per request and never
	 * reuses an outstanding one. The bitmap tracks the set of currently
	 * outstanding txn_ids so that even at the (impossibly high) rate
	 * required for the u32 counter to wrap we never collide.
	 *
	 * Bitmap size is bounded to NVKVM_MAX_INFLIGHT; at 4096 in-flight
	 * requests this is 512 bytes. Real workloads stay well under 256.
	 */
#define NVKVM_MAX_INFLIGHT 4096
	spinlock_t              inflight_lock;
	struct list_head        inflight_list;
	/*
	 * Serializes all access to vq_tx (virtqueue_add_sgs/kick on the submit
	 * side and virtqueue_get_buf on the completion side).  Linux virtqueues
	 * are NOT thread-safe; without this, two guest processes submitting on
	 * different vCPUs corrupt the split-ring (double-popped/skipped
	 * descriptors → a request is stranded → wait_for_completion hangs).
	 */
	spinlock_t              vq_tx_lock;
	atomic_t                next_txn_id;        /* monotonic seed */
	atomic64_t              interrupted_waits;  /* signal-interrupted ioctl waits */
	unsigned long           txn_inflight_bm[NVKVM_MAX_INFLIGHT / BITS_PER_LONG];

	/* Slot allocator for shared memory */
	spinlock_t              slot_lock;
	unsigned long           slot_bitmap[NVKVM_SHM_NSLOTS / BITS_PER_LONG];

	/* Host driver version (from shared memory ctrl block) */
	char                    driver_version[64];
	/* #81: per-version ABI profile, selected from driver_version at probe. */
	const struct nvkvm_abi_profile *abi;

	/* GPU mmap window — GPA range reserved for nvkvm_mmap_request() */
	unsigned long           mmap_window_gpa_base;
	unsigned long           mmap_window_len;
};

/* ── Global module state (defined in nvkvm_main.c) ────────────────────────── */

extern struct nvkvm_state nvkvm;

/* #81: null-safe ABI profile accessor (defaults to the 570/575 layout set
 * before probe has selected one from the host driver version). */
static inline const struct nvkvm_abi_profile *nvkvm_prof(void)
{
	return nvkvm.abi ? nvkvm.abi : nvkvm_abi_by_id(NVKVM_ABI_570);
}

/*
 * The cmdType that means NVKMS_IOCTL_REGISTER_SURFACE on THIS host.
 *
 * `enum NvKmsIoctlCommand` is unvalued, so position is the wire value, and
 * 570.207 inserted a member at 16 that shifted REGISTER_SURFACE from 16 to 17
 * (src/common/nvkvm_nvkms_ops.h).  This must be resolved from the host driver
 * version, not hardcoded: it gates the plane-fd translation in
 * nvkvm_sanitize_ioctl_params(), and getting it wrong forwards raw guest fds
 * to the host, which fails every REGISTER_SURFACE on the affected branches.
 *
 * Falls back to the shifted value on an unparseable version, matching the
 * numbering every branch from 570.207 to 610 uses.
 */
static inline __u32 nvkvm_nvkms_reg_surface(void)
{
	unsigned major = 0, minor = 0, patch = 0;
	struct nvkvm_nvkms_ops ops;

	nvkvm_abi_parse_version(nvkvm.driver_version, &major, &minor, &patch);
	ops = nvkvm_nvkms_ops_for_version(major, minor);
	return ops.known ? (__u32)ops.reg_surface
			 : (__u32)NVKVM_NVKMS_REG_SURFACE_SHIFTED;
}

/* ── Function declarations ─────────────────────────────────────────────────── */

/* nvkvm_main.c — shared fd-context lifecycle (also used by the DRM driver) */
struct nvkvm_fd_ctx *nvkvm_fd_ctx_open_dev(int dev_id, unsigned int flags);
void nvkvm_fd_ctx_close(struct nvkvm_fd_ctx *ctx);
/* G-6: take/drop a lifetime ref (proxy GEMs that outlive their drm_file). */
void nvkvm_fd_ctx_get(struct nvkvm_fd_ctx *ctx);
void nvkvm_fd_ctx_put(struct nvkvm_fd_ctx *ctx);

/* #101 async event delivery: registry of poll-capable fd contexts keyed by
 * (isolate_id, handle_id). register on open, unregister on close. deliver() is
 * called from the VQ_EVT virtqueue callback (softirq) to wake the matching fd. */
void nvkvm_evt_ctx_register(struct nvkvm_fd_ctx *ctx);
void nvkvm_evt_ctx_unregister(struct nvkvm_fd_ctx *ctx);
void nvkvm_evt_deliver(__u32 isolate_id, __u32 handle_id, __u32 events);

/* nvkvm_drm.c — nvidia-drm render-node emulation (graphics) */
int  nvkvm_drm_init(struct device *parent);
void nvkvm_drm_fini(void);

/* Present path (#102): if `fb` is backed by one of our proxy GEM objects (a
 * host/stub buffer forwarded via the render node — e.g. a compositor's scanout
 * buffer), report the stub-side handle and owning isolate ctx so the flip can be
 * presented to the host. Returns false for non-proxy fbs (e.g. shmem dumb). */
struct drm_framebuffer;
bool nvkvm_fb_stub_handle(struct drm_framebuffer *fb, __u32 *stub_handle,
			  struct nvkvm_fd_ctx **ctx);

/* nvkvm_kms.c — guest-emulated virtual KMS head (#102).  Init constructs the
 * mode objects before drm_dev_register(); activate publishes them to the
 * virtio event path only after registration succeeds. */
int  nvkvm_kms_init(struct drm_device *ddev);
void nvkvm_kms_activate(struct drm_device *ddev);
void nvkvm_kms_fini(struct drm_device *ddev);
/* The host's window changed size (ui_info, forwarded over VQ_EVT).  Offers it
 * as the head's preferred mode and hotplugs; the guest compositor decides. */
void nvkvm_kms_set_host_size(unsigned int w, unsigned int h);

/* nvkvm_virtio.c — transport layer */
int  nvkvm_virtio_init(struct virtio_device *vdev, struct nvkvm_state *state);
void nvkvm_virtio_fini(struct nvkvm_state *state);
int  nvkvm_negotiate_version(struct nvkvm_state *state);

/* Isolate-aware API */
int  nvkvm_virtio_open_nvidia_handle(int dev_id, unsigned int flags,
				     unsigned int session_id,
				     __u32 *handle_id_out);
int  nvkvm_virtio_create_isolate(unsigned int session_id,
				 __u32 *isolate_id_out);
/* #127: ask the host to relay an os-event fd's readiness (POLLIN) over VQ_EVT.
 * arm!=0 starts polling; arm==0 stops. Control-plane (does not block on GPU). */
int  nvkvm_virtio_poll_arm(__u32 isolate_id, __u32 handle_id, int arm);
int  nvkvm_virtio_setup_ring(unsigned int session_id, u64 *ring_gpa_out,
			     u32 *ring_bytes_out);
int  nvkvm_virtio_enter_loop(unsigned int session_id, u32 idle_us,
			     u64 *head_out);
int  nvkvm_virtio_present(struct nvkvm_fd_ctx *ctx, __u32 stub_handle,
			  __u32 width, __u32 height, __u32 pitch,
			  __u32 format, __u64 modifier);
int  nvkvm_virtio_xiso_import(struct nvkvm_fd_ctx *ctx,
			      __u32 owner_isolate_id, __u32 owner_handle_id,
			      __u32 owner_stub_handle, __u32 *gem_out);
bool nvkvm_gpa_in_mmap_window(unsigned long gpa_base, unsigned long len);
int  nvkvm_virtio_copy_handle_to_isolate(__u32 handle_id, __u32 isolate_id);
int  nvkvm_virtio_close_handle_on_isolate(__u32 handle_id, __u32 isolate_id);
int  nvkvm_virtio_close_handle(__u32 handle_id);
int  nvkvm_virtio_kill_isolate(__u32 isolate_id);
int  nvkvm_virtio_interrupt_isolate(__u32 isolate_id, __u32 target_txn);
long nvkvm_virtio_ioctl_on_isolate(struct nvkvm_fd_ctx *ctx,
				   unsigned int cmd,
				   void *params_buf, size_t param_size,
				   void *aux_buf, size_t aux_size,
				   __u32 flags,
				   __u64 *fault_addr_out);
/* Same forward with the VMA whitelist suppressed -- mandatory for any caller
 * that already holds mmap_write_lock (the managed-memory fallback). */
long nvkvm_virtio_ioctl_on_isolate_nomm(__u32 isolate_id, __u32 handle_id,
					__u32 session_id, unsigned int cmd,
					void *params_buf, size_t param_size,
					void *aux_buf, size_t aux_size);
int  nvkvm_virtio_mmap_on_isolate(__u32 isolate_id, __u32 handle_id,
				  __u64 gva, __u64 offset, __u64 length,
				  __u32 prot, __u32 map_flags,
				  unsigned int session_id,
				  __u64 *gpa_base_out,
				  __u32 *mmap_token_out);
int  nvkvm_virtio_munmap_on_isolate(__u32 isolate_id, __u32 mmap_token);
int  nvkvm_virtio_realize_uvm_mapping(__u32 isolate_id, __u32 fd_handle_id,
				      __u32 mode, unsigned int session_id,
				      __u64 gva, __u64 length, __u64 offset_hint,
				      __u32 prot, __u32 map_flags,
				      __u32 state_shm_slot,
				      __u32 intent_shm_slot,
				      __u32 intent_size,
				      __u64 *gpa_base_out,
				      __u64 *realize_token_out,
				      __u32 *rm_status_out);
int  nvkvm_virtio_read_host_file(__u32 file_id, __u32 gpu_index, __u32 shm_slot,
				 __u32 max_len, __u32 *nbytes_out);

/* nvkvm_hostfile.c — proc/sys entries fronting host files */
int  nvkvm_hostfile_init(void);
void nvkvm_hostfile_exit(void);
/* Probe QEMU for the number of host GPUs, recording each one's BDF into
 * nvkvm.gpu_bdf[].  Must run after the virtio probe (shm slots live).
 * Returns the count (>= 0), or a negative errno if the probe itself failed. */
int  nvkvm_hostfile_discover_gpus(void);
int  nvkvm_virtio_open_memory_handle(unsigned int session_id, __u64 size,
				     __u32 *handle_id_out);
int  nvkvm_virtio_write_memory_handle(__u32 handle_id, __u64 offset,
				      int shm_slot, __u32 size);
int  nvkvm_virtio_read_memory_handle(__u32 handle_id, __u64 offset,
				     int shm_slot, __u32 size);

/* nvkvm_ioctl.c */
size_t nvkvm_ioctl_param_size(unsigned int cmd);
int    nvkvm_sanitize_ioctl_params(struct nvkvm_fd_ctx *ctx,
				   unsigned int cmd,
				   void *params_buf, size_t param_size);
__s32  guest_fd_to_handle_id(int guest_fd);

/*
 * F-4 (security_audit_2026_06_01): a guest fd embedded in an ioctl field
 * (uvm_fd, rm_ctrl_fd, OS_EVENT.fd, NV0005.data, …) must be one of OUR device
 * files before we read `struct nvkvm_fd_ctx` out of its ->private_data — else a
 * caller pointing the field at any other fd (pipe/socket/eventfd/drm) causes a
 * type-confused read of a foreign subsystem's private_data. Mirrors the real
 * driver's `f->f_op == nv_frontend_fops` check in osUserHandleToKernelPtr.
 */
extern const struct file_operations nvkvm_fops;
#ifdef NVKVM_GRAPHICS
extern const struct file_operations nvkvm_drm_fops;
#endif
static inline bool nvkvm_file_is_ours(struct file *f)
{
	if (!f)
		return false;
	if (f->f_op == &nvkvm_fops)
		return true;
#ifdef NVKVM_GRAPHICS
	if (f->f_op == &nvkvm_drm_fops)
		return true;
#endif
	return false;
}

/* nvkvm_mmap.c */
extern const struct vm_operations_struct nvkvm_vm_ops;
int  nvkvm_mmap_request(struct nvkvm_fd_ctx *ctx, struct vm_area_struct *vma);
void nvkvm_mmap_release_fd(struct nvkvm_fd_ctx *ctx);
/* Rewrite the leaf PTEs of [start,end) back to write-back caching; see the
 * definition in nvkvm_mmap.c.  Callers must hold mmap lock. */
void nvkvm_force_range_wb(struct mm_struct *mm,
			  unsigned long start, unsigned long end);

/* nvkvm_uvm_ext.c -- managed-memory fallback (docs/internal/uvm-va-decoupling.md
 * section 2e).  A UVM mmap that no range-creating ioctl asked for IS libcuda
 * creating a MANAGED range; we do not forward it, we synthesise an EXTERNAL
 * range over an RM sysmem object instead.  Caller holds uvm_state->ext_lock
 * across classification and this whole synthesis. */
int  nvkvm_uvm_ext_mmap(struct nvkvm_fd_ctx *ctx, struct vm_area_struct *vma);
void nvkvm_uvm_ext_release(struct nvkvm_mmap_region *region);
void nvkvm_uvm_ext_fd_teardown(struct nvkvm_uvm_fd_state *st);
/* Is [base,base+len) inside a range THIS fd backed itself?  The scoping for the
 * two locally-answered UVM commands (45 and 31). */
bool nvkvm_uvm_ext_covers(struct nvkvm_fd_ctx *ctx, __u64 base, __u64 len);
int  nvkvm_efault_resolve(struct nvkvm_fd_ctx *ctx, __u64 fault_addr);
void nvkvm_cpu_pages_reap_stale(struct nvkvm_fd_ctx *ctx);
void nvkvm_cpu_pages_refresh(struct nvkvm_fd_ctx *ctx);
void nvkvm_cpu_pages_writeback(struct nvkvm_fd_ctx *ctx);
void nvkvm_cpu_pages_free(struct nvkvm_fd_ctx *ctx);
/* Eagerly migrate every guest page in [gva, gva+len) onto memfds shared with
 * the isolate.  Used by NV_ESC_RM_ALLOC_MEMORY hClass=NV01_MEMORY_SYSTEM_OS_-
 * DESCRIPTOR so the kernel's pin_user_pages on the stub's mm finds memfd
 * pages that alias libcuda's guest userspace pages. */
int  nvkvm_cpu_pages_migrate_range(struct nvkvm_fd_ctx *ctx,
				   __u64 gva, __u64 len, unsigned long prot);

/* Command-buffer fast path (nvkvm_main.c).  ring_try forwards a flat
 * RM_CONTROL over the SPSC ring; returns 0/-errno on success/failure, or
 * NVKVM_RING_TRY_PUNT if the control is not ring-eligible or the stub punted
 * (the caller then forwards on the virtqueue — the control was NOT executed). */
#define NVKVM_RING_TRY_PUNT 1
int  nvkvm_session_ring_try(struct nvkvm_fd_ctx *ctx, unsigned int cmd,
			    void *params_buf, size_t param_size,
			    void *aux_buf, size_t aux_size, u32 *nvstatus_out);
void nvkvm_session_stop_pump(struct nvkvm_session *session);
/* Invalidate the UVM_VALIDATE_VA_RANGE cache (#94): called on any UVM teardown
 * (unmap/unregister/free) and on a new OS_DESCRIPTOR migration, so a cached
 * "valid" can never outlive the range's registration. */
void nvkvm_session_vcache_clear(struct nvkvm_session *session);

/* nvkvm_session.c */
struct nvkvm_session *nvkvm_session_get_or_create(struct mm_struct *mm,
						  pid_t tgid);
void                  nvkvm_session_put(struct nvkvm_session *session);

/* Shared memory slot helpers (nvkvm_virtio.c) */
int   nvkvm_slot_alloc(struct nvkvm_state *state);
void  nvkvm_slot_free(struct nvkvm_state *state, int slot);
void *nvkvm_slot_addr(struct nvkvm_state *state, int slot);

/* Is the virtio transport usable? (nvkvm_virtio.c)
 *
 * FALSE means nvkvm_virtio_probe() never ran -- no nvkvm virtio device is
 * attached -- or the device has since been removed.  register_devices() and
 * nvkvm_hostfile_init() run unconditionally from module_init, so the character
 * devices, procfs and sysfs entries all exist in that state and are reachable
 * from userspace.  Every one of them must consult this before touching the
 * transport; see the comment on the definition.
 */
bool  nvkvm_transport_ready(const struct nvkvm_state *state);

/* Internal transport send (nvkvm_virtio.c) */
int   nvkvm_send_sync(struct nvkvm_state *state,
		      void *req_buf, size_t req_len,
		      struct nvkvm_inflight *inf);

#endif /* NVKVM_H */
