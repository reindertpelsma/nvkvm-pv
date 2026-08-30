/*
 * nvkvm_handle.h — global handle table for nvidia and memory handles
 *
 * Handle IDs are 32-bit integers, globally unique, persistent across
 * isolate lifetime. The underlying fd lives in QEMU and is distributed
 * to isolates via SCM_RIGHTS on demand.
 */

#ifndef NVKVM_HANDLE_H
#define NVKVM_HANDLE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "nvkvm_log.h"

#define NVKVM_HANDLE_TYPE_NVIDIA  1   /* open /dev/nvidia* fd     */
#define NVKVM_HANDLE_TYPE_MEMORY  2   /* memfd                    */

#define NVKVM_HANDLE_MAX  65536

struct nvkvm_handle {
	uint32_t    id;
	uint64_t    generation;   /* distinguishes a slot after handle-id reuse */
	int         type;         /* NVKVM_HANDLE_TYPE_* */
	int         fd;           /* underlying fd in QEMU (-1 if closed) */
	uint32_t    session_id;   /* owning session */
	int         dev_id;       /* for TYPE_NVIDIA: NVKVM_DEV_*         */
	uint32_t    isolate_refcount;  /* # isolates that hold this handle */
	/*
	 * S-1 (oob-map): the object's size in bytes, recorded at creation.
	 * TYPE_MEMORY: the length the memfd was ftruncate()d to — the ONLY
	 * record of how big the object is, without which no (offset, length)
	 * a guest names can ever be bounds-checked against the thing it names
	 * (an out-of-range mmap of a memfd faults SIGBUS on first touch, and
	 * MMAP_ON_ISOLATE prefaults every page inside the VMM).  The guest
	 * chooses it, so it is only a bound because nvkvm_handle_open_memory()
	 * caps it (NVKVM_HANDLE_MEM_MAX); uncapped, "offset > h->size" is
	 * unsatisfiable and every check written against it says nothing.
	 * TYPE_NVIDIA: 0 — a device fd has no meaningful length and `offset`
	 * there is an RM mapping token, not a byte offset, so callers must
	 * skip the check for those rather than treat 0 as "empty".
	 */
	uint64_t    size;
	bool        poll_active;  /* handle is registered for poll        */
	bool        in_use;
};

struct nvkvm_handle_table {
	pthread_mutex_t  lock;
	struct nvkvm_handle handles[NVKVM_HANDLE_MAX];
	uint32_t         next_id;   /* monotonic counter, wraps with gap-fill */
	uint64_t         next_generation;
};

void nvkvm_handle_table_init(struct nvkvm_handle_table *t);
void nvkvm_handle_table_fini(struct nvkvm_handle_table *t);

/* Allocate a new nvidia handle (opens fd in QEMU). */
int nvkvm_handle_open_nvidia(struct nvkvm_handle_table *t,
			     uint32_t session_id, int dev_id, int flags,
			     uint32_t *handle_id_out);

/*
 * Allocate a handle slot WITHOUT opening anything (fd = -1, in_use = true).
 * Used by the stub-opens-the-fd path: QEMU reserves the slot, hands the
 * handle_id to the stub via ISOLATE_CMD_OPEN_DEVICE, and on success
 * attaches the SCM_RIGHTS-received fd via nvkvm_handle_attach_fd. If
 * anything fails between alloc and attach, nvkvm_handle_abort_open
 * releases the slot cleanly (no close — there's no fd yet).
 */
int  nvkvm_handle_alloc_pending(struct nvkvm_handle_table *t,
				 uint32_t session_id, int dev_id,
				 uint32_t *handle_id_out);
int  nvkvm_handle_attach_fd(struct nvkvm_handle_table *t,
			    uint32_t handle_id, int fd);
void nvkvm_handle_abort_open(struct nvkvm_handle_table *t,
			     uint32_t handle_id);

/* Allocate a new memory handle (creates memfd in QEMU). */
int nvkvm_handle_open_memory(struct nvkvm_handle_table *t,
			     uint32_t session_id, uint64_t size,
			     uint32_t *handle_id_out);

/*
 * Look up a handle.  The returned pointer is NOT protected: the table lock is
 * taken only to validate the slot and is DROPPED before returning, so every
 * field read through it — h->fd above all — is a plain unsynchronised read of
 * an array a concurrent CLOSE_HANDLE may already have reused.  The pointer
 * itself stays valid (the table is a fixed array, never freed) which is why
 * this looks safe and is not.
 *
 * Use it only for advisory reads made on the same thread that owns the
 * request, and NEVER to obtain an fd to operate on — take
 * nvkvm_handle_acquire_fd() for that, which dups under the lock.  The comment
 * this replaces claimed the pointer was returned "under table lock", which is
 * what made the remaining call sites look correct.
 */
struct nvkvm_handle *nvkvm_handle_get(struct nvkvm_handle_table *t,
				      uint32_t handle_id);

/* C-2: dup the handle's fd atomically under the table lock so a concurrent
 * close cannot recycle it mid-ioctl. Caller MUST close() the returned fd. */
int nvkvm_handle_acquire_fd(struct nvkvm_handle_table *t, uint32_t handle_id,
			    int *dev_id_out, uint64_t *generation_out);

/* As above, but atomically pins the handle for one isolate relay. */
int nvkvm_handle_acquire_fd_ref_isolate(struct nvkvm_handle_table *t,
					 uint32_t handle_id, int *dev_id_out,
					 uint64_t *generation_out);

/* Bump isolate refcount (called when sending fd to an isolate). */
int nvkvm_handle_ref_isolate(struct nvkvm_handle_table *t, uint32_t handle_id);

/* Decrement isolate refcount (called when isolate closes fd). */
int nvkvm_handle_unref_isolate(struct nvkvm_handle_table *t, uint32_t handle_id);

/* Generation-qualified variants used by delayed cross-isolate teardown. */
int nvkvm_handle_ref_isolate_generation(struct nvkvm_handle_table *t,
					 uint32_t handle_id, uint64_t generation);
int nvkvm_handle_unref_isolate_generation(struct nvkvm_handle_table *t,
					   uint32_t handle_id, uint64_t generation);

/* Close the underlying fd. Fails (returns -EBUSY) if isolate_refcount > 0. */
int nvkvm_handle_close(struct nvkvm_handle_table *t, uint32_t handle_id);

/* Close all handles belonging to a session (called on session teardown). */
/* Force every handle shut regardless of isolate_refcount.  RESET only:
 * kill all isolates first.  Returns how many were closed. */
uint32_t nvkvm_handle_close_all(struct nvkvm_handle_table *t);

void nvkvm_handle_close_session(struct nvkvm_handle_table *t, uint32_t session_id);

#endif /* NVKVM_HANDLE_H */
