/*
 * nvkvm_tables.h — Four-table model for the QEMU-side of the nvkvm forwarder.
 *
 * Single source of truth. All IDs visible across the (guest kernel → QEMU →
 * isolate stub) boundary are managed here. Stub never invents IDs; QEMU
 * allocates a handle_id and sends it down to the stub as part of an open
 * command. Stub keeps no table — it operates on stub-local fd numbers that
 * QEMU dictates.
 *
 *   ┌────────────────────────────────────────────────────────────────────┐
 *   │  Handle table  (handle_id → qemu_fd, type, ready, refcount)        │
 *   │  Isolate table (isolate_id → pid, comm_fd)                         │
 *   │  Isolate↔Handle map (M:N: isolate_id × handle_id → fd_on_isolate)  │
 *   │  MMAP table    (mmap_id → handle_id, offset, vmm_va, gpa, size)    │
 *   │  GPA windows   (one big sparse memfd per window; suballocate)      │
 *   └────────────────────────────────────────────────────────────────────┘
 *
 * Invariants (enforced by the impl, asserted by the tests):
 *
 *   I1  qemu_fd != -1 outside the open transaction window. On open failure
 *       the handle slot is never made visible to the guest.
 *
 *   I2  A handle entry is freed only after every dependent entry — mmaps,
 *       iso↔handle links, and poll registrations — has been removed AND the
 *       in-flight refcount has drained to zero. Strict dependency order.
 *
 *   I3  `handle_acquire` increments the refcount under the table lock; the
 *       caller releases with `handle_release`. Close marks `ready=false`
 *       (under lock), then waits for refcount to hit zero on a condvar
 *       before cleaning. Concurrent close + in-flight op is race-free.
 *
 *   I4  Handle IDs are u32, non-zero (zero is the sentinel for "no handle").
 *       Generation bits are baked into the high bits of the id so that
 *       after a slot is reused, a stale id from a previous owner fails
 *       lookup. This protects against "fd-number reuse"-style bugs.
 *
 *   I5  GPA windows back many mmaps via offsets into one memfd, one KVM
 *       memslot per window. Sparse: `fallocate(FALLOC_FL_PUNCH_HOLE)` on
 *       free, with ±1 adjacency to coalesce. Double-punch is a no-op.
 *       At most NVKVM_TABLES_MAX_WINDOWS windows.
 *
 *   I6  All public functions are thread-safe. Internal helpers may require
 *       the table lock held; documented per-function.
 */

#ifndef NVKVM_TABLES_H
#define NVKVM_TABLES_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* ── Limits ─────────────────────────────────────────────────────────────── */

#define NVKVM_TABLES_MAX_HANDLES       4096
#define NVKVM_TABLES_MAX_ISOLATES      256
#define NVKVM_TABLES_MAX_MMAPS         8192
#define NVKVM_TABLES_MAX_ISO_HND_LINKS 16384
#define NVKVM_TABLES_MAX_WINDOWS       8

/* Default window size: 128 GiB. Upfront one window, lazily add more. */
#define NVKVM_TABLES_DEFAULT_WINDOW_SIZE (128ULL * 1024 * 1024 * 1024)
/* Window must be page-aligned; allocations rounded up to this. */
#define NVKVM_TABLES_PAGE_SIZE 4096ULL

/* ── Types ──────────────────────────────────────────────────────────────── */

enum nvkvm_handle_type {
	NVKVM_HND_INVALID = 0,
	NVKVM_HND_NVIDIACTL,   /* /dev/nvidiactl       — opened in stub */
	NVKVM_HND_NVIDIA_GPU,  /* /dev/nvidiaN         — opened in stub */
	NVKVM_HND_NVIDIA_UVM,  /* /dev/nvidia-uvm      — opened in QEMU */
	NVKVM_HND_MEMFD,       /* memfd_create         — opened in QEMU */
	NVKVM_HND_EVENTFD,     /* eventfd              — either side    */
};

/* Internal handle slot. Treat as opaque from callers. */
struct nvkvm_handle_entry {
	bool                 in_use;
	uint32_t             handle_id;     /* full id incl. generation */
	enum nvkvm_handle_type type;
	int                  qemu_fd;       /* -1 only during open window */
	bool                 ready;         /* false during open/close */
	bool                 poll_enabled;
	uint32_t             refcount;      /* in-flight ops referencing this */
	pthread_cond_t       refcount_zero_cv;
};

struct nvkvm_handle_table {
	pthread_mutex_t       lock;
	struct nvkvm_handle_entry slots[NVKVM_TABLES_MAX_HANDLES];
	uint16_t              generation[NVKVM_TABLES_MAX_HANDLES];
	uint32_t              next_slot;
};

/* Isolate */
struct nvkvm_isolate_entry {
	bool     in_use;
	uint32_t isolate_id;     /* full id incl. generation */
	pid_t    pid;
	int      comm_fd;
	bool     alive;
};

struct nvkvm_isolate_table {
	pthread_mutex_t            lock;
	struct nvkvm_isolate_entry slots[NVKVM_TABLES_MAX_ISOLATES];
	uint16_t                   generation[NVKVM_TABLES_MAX_ISOLATES];
	uint32_t                   next_slot;
};

/* Isolate ↔ Handle link (M:N) */
struct nvkvm_iso_hnd_entry {
	bool     in_use;
	uint32_t isolate_id;
	uint32_t handle_id;
	int      fd_on_isolate;
};

struct nvkvm_iso_hnd_map {
	pthread_mutex_t            lock;
	struct nvkvm_iso_hnd_entry slots[NVKVM_TABLES_MAX_ISO_HND_LINKS];
};

/* MMAP entry */
struct nvkvm_mmap_entry {
	bool     in_use;
	uint32_t mmap_id;
	uint32_t handle_id;
	uint64_t handle_offset;
	void    *vmm_va;
	uint64_t gpa;
	uint64_t size;
	int      prot;
	uint32_t window_id;
	uint64_t window_offset;
};

struct nvkvm_mmap_table {
	pthread_mutex_t         lock;
	struct nvkvm_mmap_entry slots[NVKVM_TABLES_MAX_MMAPS];
	uint16_t                generation[NVKVM_TABLES_MAX_MMAPS];
	uint32_t                next_slot;
};

/* GPA window: one big sparse memfd, sub-allocated via offsets. */
struct nvkvm_gpa_window {
	bool     in_use;
	uint32_t window_id;
	uint64_t gpa_base;
	uint64_t size;
	int      memfd;
	void    *qva;        /* QEMU's MAP_FIXED view of the memfd */
	int      kvm_slot;   /* slot # passed to KVM_SET_USER_MEMORY_REGION;
				    -1 if KVM unavailable (unit tests) */
	/* Sub-allocator: free regions, sorted by offset, coalescing. */
	struct nvkvm_window_free_region *free_head;
};

struct nvkvm_window_free_region {
	uint64_t offset;
	uint64_t size;
	struct nvkvm_window_free_region *next;
};

struct nvkvm_gpa_window_table {
	pthread_mutex_t         lock;
	struct nvkvm_gpa_window windows[NVKVM_TABLES_MAX_WINDOWS];
	uint16_t                generation[NVKVM_TABLES_MAX_WINDOWS];
	uint64_t                next_gpa_base;
};

/* The aggregate. One per VirtIONvgpu device. */
struct nvkvm_tables {
	struct nvkvm_handle_table     handles;
	struct nvkvm_isolate_table    isolates;
	struct nvkvm_iso_hnd_map      iso_hnd;
	struct nvkvm_mmap_table       mmaps;
	struct nvkvm_gpa_window_table windows;
};

/* ── API ────────────────────────────────────────────────────────────────── */

void nvkvm_tables_init(struct nvkvm_tables *t);
void nvkvm_tables_fini(struct nvkvm_tables *t);

/* Handle: allocate a slot but leave it !ready (qemu_fd = -1) so an open
 * transaction can run. Returns 0 + handle_id on success. */
int nvkvm_tables_handle_alloc(struct nvkvm_tables *t,
			       enum nvkvm_handle_type type,
			       uint32_t *out_handle_id);

/* Set the qemu_fd on a slot that hasn't been made ready yet. Sets ready=true.
 * Idempotent for the same fd; rejects if slot is already ready. */
int nvkvm_tables_handle_attach_qemu_fd(struct nvkvm_tables *t,
					uint32_t handle_id, int qemu_fd);

/* Roll back an alloc that never completed open. Closes qemu_fd if set. */
int nvkvm_tables_handle_abort_open(struct nvkvm_tables *t, uint32_t handle_id);

/* Acquire: lookup handle by id, increment refcount, return pointer.
 * Returns -ENOENT on stale/invalid id, -EBUSY if ready=false (closing). */
int nvkvm_tables_handle_acquire(struct nvkvm_tables *t, uint32_t handle_id,
				 struct nvkvm_handle_entry **out_entry);
void nvkvm_tables_handle_release(struct nvkvm_tables *t, uint32_t handle_id);

/* Close: mark !ready, drain refcount, then free the slot.
 * Returns -EBUSY if there are still iso_hnd links or mmaps for this handle
 * (caller must clean those first to enforce dependency order). */
int nvkvm_tables_handle_close(struct nvkvm_tables *t, uint32_t handle_id);

/* Isolate */
int nvkvm_tables_isolate_alloc(struct nvkvm_tables *t, pid_t pid, int comm_fd,
				uint32_t *out_isolate_id);
int nvkvm_tables_isolate_mark_dead(struct nvkvm_tables *t,
				    uint32_t isolate_id);
int nvkvm_tables_isolate_close(struct nvkvm_tables *t, uint32_t isolate_id);

/* Iso ↔ Hnd: link the stub-local fd of a handle in a specific isolate. */
int nvkvm_tables_iso_hnd_link(struct nvkvm_tables *t,
			       uint32_t isolate_id, uint32_t handle_id,
			       int fd_on_isolate);
int nvkvm_tables_iso_hnd_unlink(struct nvkvm_tables *t,
				 uint32_t isolate_id, uint32_t handle_id);
int nvkvm_tables_iso_hnd_lookup_fd(struct nvkvm_tables *t,
				    uint32_t isolate_id, uint32_t handle_id,
				    int *out_fd);
/* Returns count of remaining isolate links for this handle. */
int nvkvm_tables_iso_hnd_count_for_handle(struct nvkvm_tables *t,
					   uint32_t handle_id);

/* GPA windows */
int nvkvm_tables_window_create(struct nvkvm_tables *t, uint64_t size,
				int kvm_vm_fd, uint32_t *out_window_id);
int nvkvm_tables_window_destroy(struct nvkvm_tables *t,
				 uint32_t window_id, int kvm_vm_fd);

/* MMAP: sub-allocate `size` bytes from any window, return gpa + window. */
int nvkvm_tables_mmap_alloc(struct nvkvm_tables *t,
			     uint32_t handle_id, uint64_t handle_offset,
			     uint64_t size, int prot,
			     uint64_t *out_gpa, uint32_t *out_mmap_id);
/* Free an mmap, punch hole in backing memfd with ±1 adjacency coalesce. */
int nvkvm_tables_mmap_free(struct nvkvm_tables *t, uint32_t mmap_id);

/* Counts for diagnostics + tests */
int nvkvm_tables_mmap_count_for_handle(struct nvkvm_tables *t,
					uint32_t handle_id);

#endif /* NVKVM_TABLES_H */
