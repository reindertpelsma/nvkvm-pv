/*
 * nvkvm_isolate.h — QEMU-side isolate process manager
 *
 * One isolate per guest userspace mm. The isolate is a minimal static
 * process (nvkvm_stub) that mirrors the guest's GPU virtual address layout
 * so the NVIDIA kernel driver sees valid mappings in current->mm.
 *
 * Multi-inflight design
 * =====================
 * Each isolate has a dedicated reader thread that demultiplexes IOCTL
 * responses by txn_id onto per-caller condvars (stack-allocated by callers).
 * Non-IOCTL (sync) commands are gated one-at-a-time by sync_cmd_lock and
 * handed off via sync_lock + sync_cond (audit F3-1; ENTER_LOOP, PRESENT and
 * XISO_IMPORT each have their own slot instead).
 * All socket writes are serialized by write_lock.
 *
 * Lock order: sync_cmd_lock > sync_lock > write_lock > lock
 * (Never hold a later lock while trying to acquire an earlier one.)
 * present_lock > present_sync_lock and xiso_lock > xiso_sync_lock and
 * loop_lock > loop_sync_lock are independent chains that only ever nest
 * write_lock beneath them, never sync_lock.
 */

#ifndef NVKVM_ISOLATE_H
#define NVKVM_ISOLATE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>

#include "nvkvm_handle.h"
#include "nvkvm_isolate_uid.h"

#define NVKVM_ISOLATE_MAX  4096

/*
 * Per-isolate cap on relayed foreign handles (cross-isolate RM export/import).
 * NCCL opens one shareable handle per peer per channel, so a rank on a 6-GPU
 * node needs tens, not hundreds; 256 leaves headroom without making every
 * isolate struct large.  Overflow is handled by declining further relays for
 * that isolate, not by evicting a live one.
 */
#define NVKVM_XRM_MAX      256

/*
 * One recorded cross-isolate relay.  generation == 0 means "reserved, delivery
 * still in flight": the worker doing the blocking relay owns the eventual
 * rollback, and teardown must NOT drop a handle reference for it because none
 * has been taken yet.  A nonzero generation is the handle-table generation the
 * relayed fd was pinned against, so a slot that has since been closed and
 * reissued under the same handle id cannot be unreffed by mistake.
 */
struct nvkvm_xrm_handle {
	uint32_t handle_id;
	uint64_t generation;
};

/*
 * Forward declaration — fully defined in nvkvm_isolate.c.
 * Callers of nvkvm_isolate_ioctl never touch this directly; it lives on
 * the caller's stack and is registered/deregistered internally.
 */
struct nvkvm_pending_ioctl;

struct nvkvm_isolate {
	uint32_t    id;
	uint32_t    session_id;
	pid_t       pid;
	int         sock_fd;
	bool        alive;
	bool        in_use;
	void       *nv;       /* #127: owning VirtIONvgpu, so the reader thread can
	                       * push os-event wakeups (vq_evt). Copied from table->nv. */

	/*
	 * UID-separation mode only (NVKVM_ISO_MODE_UID): the unique host uid/gid
	 * this isolate's stub runs as.  0 when the mode is off.  Derived from the
	 * slot index, so it is unique among the VM's live isolates by construction
	 * and is only re-issued after nvkvm_isolate_kill() has waitpid()'d the
	 * previous holder (see nvkvm_isolate_uid.h).
	 */
	uid_t       run_uid;
	gid_t       run_gid;

	/*
	 * lock: protects alive, in_use, pending_head, next_txn_id.
	 * Held briefly; never during blocking I/O.
	 */
	pthread_mutex_t lock;

	/* Serializes all multi-part socket writes. */
	pthread_mutex_t write_lock;

	/* Reader thread — one per isolate, started at create time. */
	pthread_t   reader_tid;
	bool        reader_started;

	/* In-flight async IOCTL list (intrusive linked list on callers' stacks). */
	struct nvkvm_pending_ioctl *pending_head;
	uint32_t    next_txn_id;    /* monotonic counter, never 0 */

	/*
	 * Sync command slot — one non-IOCTL command at a time.
	 *
	 * Audit F3-1 (hang/correlation audit 2026-08): sync_lock does NOT
	 * actually deliver the "one at a time" this comment claims, because
	 * pthread_cond_wait drops it: a second sender could walk in while the
	 * first was parked, clear sync_done (erasing the parked waiter's
	 * wakeup) and then collect a reply that belonged to its predecessor.
	 * None of the shared response slots below (sync_open_fd,
	 * sync_mmap_retval, sync_realize_*, sync_ring_probe) carries a
	 * correlation tag, and the wire responses that use them (RESP_OK /
	 * RESP_ERROR / RESP_MMAP / RESP_RING_READY) have no txn_id field to
	 * add one to without a protocol change, so the fix is a real gate
	 * rather than a tag: sync_cmd_lock is held across the WHOLE round-trip
	 * (it is never released by the cond wait), so only one shared-slot
	 * command is ever live.  ENTER_LOOP — the one sync command that both
	 * runs off the TX thread and legitimately blocks for an unbounded time,
	 * i.e. the one that made this reachable — was moved to its own
	 * dedicated slot below so it cannot queue behind (or be starved by)
	 * the short commands.
	 */
	pthread_mutex_t sync_cmd_lock;
	pthread_mutex_t sync_lock;
	pthread_cond_t  sync_cond;
	bool        sync_done;
	int         sync_error;     /* -errno or 0 */
	int         sync_mmap_retval;
	int         sync_open_fd;   /* fd received via SCM_RIGHTS for OPEN_DEVICE;
	                             * -1 if none. Reader fills before signaling. */
	/* REALIZE_UVM_FD response slots — reader fills before signaling. */
	uint64_t    sync_realize_host_va;
	uint64_t    sync_realize_length;
	uint64_t    sync_realize_token;
	uint32_t    sync_realize_rm_status;
	/* SETUP_RING probe echo — reader fills before signaling. */
	uint64_t    sync_ring_probe;

	/*
	 * ENTER_LOOP slot (audit F3-1) — DEDICATED, for the same reason present
	 * and xiso are: it is issued from QEMU's thread pool and stays in flight
	 * for the whole consumer-loop lifetime, so sharing the sync_* slot with
	 * the short TX-thread commands is exactly the two-live-commands race.
	 * loop_lock serializes ENTER_LOOP callers (held across the round-trip);
	 * loop_sync_lock + loop_cond are the reader handoff.
	 */
	pthread_mutex_t loop_lock;
	pthread_mutex_t loop_sync_lock;
	pthread_cond_t  loop_cond;
	bool        loop_done;
	int         loop_error;
	uint64_t    loop_head;

	/*
	 * Present-export slot (#106) — DEDICATED, independent of the sync_* slot
	 * above, because present fires per frame and would otherwise race a
	 * concurrent setup-time OPEN_DEVICE/MMAP/REALIZE on the same isolate.
	 * present_lock serializes present-export callers (held across the whole
	 * round-trip, NOT released during the wait); present_sync_lock + present_cond
	 * are the reader handoff (released during cond_wait).
	 */
	pthread_mutex_t present_lock;
	pthread_mutex_t present_sync_lock;
	pthread_cond_t  present_cond;
	bool        present_done;
	int         present_err;
	int         present_fd;     /* dma-buf fd via SCM_RIGHTS; -1 if none */

	/*
	 * Cross-isolate import slot (#110) — DEDICATED like present, because a
	 * compositor isolate may import (this) concurrently with presenting its
	 * own scanout (present_*).  Targets THIS (importer) isolate; QEMU sends a
	 * dma-buf fd in and gets a GEM handle out.
	 */
	pthread_mutex_t xiso_lock;
	pthread_mutex_t xiso_sync_lock;
	pthread_cond_t  xiso_cond;
	bool        xiso_done;
	int         xiso_err;
	uint32_t    xiso_gem;       /* importer-local GEM handle on success */

	/*
	 * Cross-isolate RM export/import (CUDA VMM shareable handles / the NCCL
	 * SHM transport): the set of FOREIGN handle_ids QEMU has already relayed
	 * into this isolate with RECEIVE_FD.  The stub's handle_store()
	 * overwrites without closing, so pushing a second dup of the same handle
	 * would leak the first inside the stub; this is what makes the relay
	 * once-per-(isolate, handle) instead of once-per-import.  Guarded by its
	 * own lock because the broker runs on QEMU's pooled IOCTL workers.
	 */
	pthread_mutex_t xrm_lock;
	struct nvkvm_xrm_handle xrm_handles[NVKVM_XRM_MAX];
	unsigned    xrm_n;
	/*
	 * Cleared by nvkvm_isolate_kill() before it drains the list, so a
	 * pooled IOCTL worker cannot reserve a new relay against a slot that is
	 * already being torn down and have its reference outlive the drain.
	 */
	bool        xrm_accepting;

	/*
	 * Command-buffer SPSC ring pair (docs/design/command_buffer.md, Phase 2).
	 * QEMU mints one memfd holding both rings, keeps its own MAP_SHARED
	 * mapping (for init / the grow handshake / a future QEMU-side ring), and
	 * hands a copy to the isolate which maps the same memfd.  ring_ready is
	 * set once the bidirectional probe self-test passes.  ring_gpa /
	 * ring_kvm_slot are filled in Phase 4 when the region is installed into
	 * the guest's physical address space.  ring_memfd < 0 ⇒ no ring (the
	 * isolate keeps serving ioctls over the existing path).
	 */
	int         ring_memfd;
	void       *ring_qva;
	uint64_t    ring_region_size;
	uint32_t    ring_bytes;
	uint64_t    ring_gpa;
	int         ring_kvm_slot;
	bool        ring_ready;
};

struct nvkvm_isolate_table {
	pthread_mutex_t      lock;
	struct nvkvm_isolate isolates[NVKVM_ISOLATE_MAX];
	uint32_t             next_id;
	uint32_t             abi_profile;  /* #81: per-VM ABI id stamped into IOCTLs */
	/*
	 * Resolved isolation configuration for this VM (mode + uid window).
	 * Filled by nvkvm_isolate_table_init() from the environment; validated
	 * up front by nvkvm_isolate_cfg_check() at device realize.
	 */
	struct nvkvm_isolate_cfg cfg;
	char                     cfg_error[256];  /* non-empty => config unusable */
	char                     cfg_report[1024];/* what `auto` probed and chose */
	/*
	 * Owning VirtIONvgpu (opaque here to avoid a header cycle).  Set on the
	 * first isolate create; used by ring setup/teardown to place the ring
	 * memfd in the sparse GPA window (nvkvm_sparse_gpa_alloc/free) so the
	 * guest can map it.  QEMU only maps the ring — it never inspects contents.
	 */
	void                *nv;
	/*
	 * The VM's handle table.  nvkvm_isolate_kill() needs it to drop the
	 * isolate references held by relayed foreign handles; it is the same
	 * table every caller already passes explicitly, kept here so teardown
	 * does not have to be handed it by whoever happened to notice the death.
	 */
	struct nvkvm_handle_table *handles;
};

void nvkvm_isolate_table_init(struct nvkvm_isolate_table *t,
			      struct nvkvm_handle_table *handles);

/*
 * Validate the resolved isolation configuration against what this host can
 * actually do, ONCE, at device realize.  Returns 0, or -1 with a
 * human-readable reason written to `err`.
 *
 * This is deliberately up front: UID mode needs CAP_SETUID/CAP_SETGID, and
 * discovering that at the first setresuid() — inside a forked child, three
 * layers below the guest's first GPU ioctl — surfaces as an opaque isolate
 * spawn failure.  It never falls back to a different mode: silently
 * downgrading a security boundary is worse than refusing to start.
 */
int nvkvm_isolate_cfg_check(const struct nvkvm_isolate_table *t,
			    char *err, size_t errsz);

/*
 * True when the resolved mode is weaker than `namespace`.  The caller logs
 * nvkvm_isolate_cfg_report() at WARNING level in that case, at every start, so
 * an operator reading their logs finds out they are on a weaker boundary
 * without going looking for it.
 */
bool nvkvm_isolate_cfg_is_degraded(const struct nvkvm_isolate_table *t);

/* True for mode 'none' — every layer off, including the seccomp filter. */
bool nvkvm_isolate_cfg_is_unconfined(const struct nvkvm_isolate_table *t);

/* Multi-line account of what `auto` attempted and why each rung was rejected. */
const char *nvkvm_isolate_cfg_report(const struct nvkvm_isolate_table *t);

/* Human-readable one-liner describing the active mode (for startup logging). */
const char *nvkvm_isolate_cfg_describe(const struct nvkvm_isolate_table *t,
				       char *buf, size_t bufsz);
void nvkvm_isolate_table_fini(struct nvkvm_isolate_table *t);

/*
 * Spawn a new isolate process. The stub binary is loaded from
 * nvkvm_stub_elf[] (embedded at build time) via memfd_create + fexecve.
 * Returns 0 and fills *isolate_id_out on success.
 */
int nvkvm_isolate_create(struct nvkvm_isolate_table *t,
			 uint32_t session_id,
			 void *nv,
			 uint32_t *isolate_id_out);

/*
 * Send EXIT command, join the reader thread, wait for the isolate to exit,
 * and free the slot. All in-flight IOCTL callers receive -ECONNRESET.
 */
int nvkvm_isolate_kill(struct nvkvm_isolate_table *t, uint32_t isolate_id);

/* Host pid of a live isolate by id (0 if none) — for GET_PID_INFO pid mapping. */
pid_t nvkvm_isolate_host_pid(struct nvkvm_isolate_table *t, uint32_t isolate_id);

/*
 * Present export (#106): ask the isolate's stub to PRIME_HANDLE_TO_FD the
 * render-node GEM `gem_handle` (held under `handle_id`) and return the dma-buf
 * fd (received via SCM_RIGHTS) in *fd_out.  Caller owns *fd_out and must close
 * it.  Serialized per isolate.  Returns 0 on success, -errno otherwise.
 */
int nvkvm_isolate_present_export(struct nvkvm_isolate_table *t,
				 uint32_t isolate_id, uint32_t handle_id,
				 uint32_t gem_handle, int *fd_out);

/*
 * #110 cross-isolate import: hand `dmabuf_fd` to the importer isolate's stub,
 * which PRIME_FD_TO_HANDLEs it into a local GEM, returned in *gem_out.  Caller
 * retains ownership of dmabuf_fd.  Serialized per isolate.  0 / -errno.
 */
int nvkvm_isolate_xiso_import(struct nvkvm_isolate_table *t,
			      uint32_t isolate_id, uint32_t handle_id,
			      int dmabuf_fd, uint32_t *gem_out);

/*
 * Set up the per-isolate SPSC command-buffer ring (docs/design/command_buffer.md).
 * Mints a memfd holding the request+response rings, maps it in QEMU,
 * initialises both control blocks, hands a copy to the isolate via SCM_RIGHTS,
 * and runs a bidirectional shared-memory probe self-test.  On success the
 * isolate has the ring mapped and ready (Phase 3 spins a consumer on it).
 *
 * The ring is a pure optimisation: a non-zero return is logged and ignored by
 * the caller — the isolate keeps serving every ioctl over the existing path.
 */
int nvkvm_isolate_ring_setup(struct nvkvm_isolate_table *t, uint32_t isolate_id,
			     void *nv);

/*
 * Report an isolate's command-buffer ring placement so the guest can map it:
 * the guest-physical base + geometry.  Returns 0 and fills the out-params if
 * the ring is ready and guest-visible (ring_gpa != 0); -ENODEV otherwise (the
 * guest then stays on the virtqueue path).
 */
int nvkvm_isolate_ring_info(struct nvkvm_isolate_table *t, uint32_t isolate_id,
			    uint64_t *gpa, uint32_t *region_size,
			    uint32_t *resp_off, uint32_t *ring_bytes);

/*
 * Drive the isolate's SPSC consumer loop (docs/design/command_buffer.md).
 * Sends ISOLATE_CMD_ENTER_LOOP and BLOCKS until the loop idles out, then
 * returns 0 and fills *head_out with the request-ring head at exit
 * (last_processed).  The guest pump compares it to the published tail to decide
 * whether to re-enter.  Returns -errno on a dead isolate / missing ring.
 *
 * MUST be called off QEMU's main loop (it blocks for the whole loop lifetime) —
 * the virtio dispatch offloads it to the thread pool.
 */
int nvkvm_isolate_enter_loop(struct nvkvm_isolate_table *t, uint32_t isolate_id,
			     uint32_t idle_us, uint64_t *head_out);

/*
 * Fire-and-forget: ask the isolate to post SIGUSR1 to the worker currently
 * running target_txn so its in-flight host ioctl returns -EINTR.  Returns 0 if
 * the command was written to a live isolate, -ENOENT for an unknown/dead one.
 * Does NOT wait for the ioctl to actually return — the normal IOCTL response
 * path delivers the (now -EINTR) result.
 */
int nvkvm_isolate_interrupt(struct nvkvm_isolate_table *t,
			    uint32_t isolate_id, uint32_t target_txn);

/*
 * Send a handle's fd to the isolate via SCM_RIGHTS.
 * Also bumps the handle's isolate_refcount.
 */
int nvkvm_isolate_send_handle(struct nvkvm_isolate_table *t,
			      struct nvkvm_handle_table *ht,
			      uint32_t isolate_id, uint32_t handle_id);
/*
 * As above, but also reports the handle-table generation the reference was
 * taken against, so the caller can later release exactly that reference and
 * not one belonging to a recycled slot with the same handle id.
 */
int nvkvm_isolate_send_handle_generation(struct nvkvm_isolate_table *t,
					 struct nvkvm_handle_table *ht,
					 uint32_t isolate_id, uint32_t handle_id,
					 uint64_t *generation_out);

/*
 * Cross-isolate RM export/import bookkeeping (see xrm_handles above).
 * note(): atomic test-and-set — returns true if this isolate was ALREADY
 * recorded as holding handle_id (caller does nothing), false if it has just
 * been recorded (caller must do the relay, and call forget() if it fails).
 * A full table returns true, which degrades to "do not relay again" rather
 * than to a leak.
 */
bool nvkvm_isolate_note_foreign_handle(struct nvkvm_isolate_table *t,
				       uint32_t isolate_id, uint32_t handle_id);
/*
 * finalize(): publish the generation of a relay that has now been delivered.
 * Returns false if the reservation is gone (the isolate died mid-relay), in
 * which case the caller still owns the reference and must drop it itself.
 */
bool nvkvm_isolate_finalize_foreign_handle(struct nvkvm_isolate_table *t,
					   uint32_t isolate_id,
					   uint32_t handle_id,
					   uint64_t generation);
/*
 * forget(): remove the record.  Returns true if one was present, and writes
 * its generation (0 for a reservation that never completed delivery) to
 * *generation_out when non-NULL.
 */
bool nvkvm_isolate_forget_foreign_handle(struct nvkvm_isolate_table *t,
					 uint32_t isolate_id,
					 uint32_t handle_id,
					 uint64_t *generation_out);

/*
 * Ask the isolate to open /dev/nvidia* (or eventfd) on QEMU's behalf so the
 * file's nvfp/mm lineage is the stub process. On success the stub stores
 * the fd under handle_id and replies with a SCM_RIGHTS-attached copy of
 * the same fd; that copy is returned via *fd_out — caller owns it and
 * typically attaches it as the qemu_fd in the handle table.
 *
 * dev_id values match the NVKVM_DEV_* constants from nvkvm_proto.h. UVM
 * and memfd are NOT supported here; both stay opened in QEMU directly.
 */
int nvkvm_isolate_open_device(struct nvkvm_isolate_table *t,
			      uint32_t isolate_id, uint32_t handle_id,
			      uint32_t dev_id, uint32_t flags,
			      int *fd_out);

/*
 * Tell the isolate to close a handle's fd.
 * Also decrements the handle's isolate_refcount.
 */
int nvkvm_isolate_close_handle(struct nvkvm_isolate_table *t,
				struct nvkvm_handle_table *ht,
				uint32_t isolate_id, uint32_t handle_id);

/*
 * Forward an ioctl to the isolate asynchronously.
 * Multiple concurrent callers are supported; responses are matched by txn_id.
 * param_buf and aux_buf are updated in-place with the isolate's response data.
 * fault_addr_out receives the GVA that triggered SIGSEGV (0 if none).
 */
int nvkvm_isolate_ioctl(struct nvkvm_isolate_table *t,
			uint32_t isolate_id, uint32_t handle_id,
			unsigned int cmd,
			void *param_buf, size_t param_size,
			void *aux_buf, size_t aux_size,
			uint32_t flags,
			uint32_t *nvstatus_out,
			uint64_t *fault_addr_out);

/*
 * Tell the isolate to mmap handle_id's fd at gva (MAP_FIXED).
 */
int nvkvm_isolate_mmap(struct nvkvm_isolate_table *t,
		       uint32_t isolate_id, uint32_t handle_id,
		       uint64_t gva, uint64_t length, uint64_t offset,
		       int prot, int map_flags);

/*
 * Tell the isolate to munmap [gva, gva+length).
 */
int nvkvm_isolate_munmap(struct nvkvm_isolate_table *t,
			 uint32_t isolate_id, uint64_t gva, uint64_t length);

/*
 * Send REALIZE_UVM_FD: stub opens /dev/nvidia-uvm, replays the recorded
 * state, runs the mode-specific intent ioctl, and mmaps the result.
 *
 * The cmd header is sent first; then `state` (state_size bytes); then
 * `intent` (intent_size bytes), all on the same SEQPACKET socket.
 *
 * On success returns 0 and fills *host_va_out / *length_out / *token_out /
 * *rm_status_out.  rm_status != 0 means the kernel rejected the intent
 * but the transport succeeded.
 */
int nvkvm_isolate_realize_uvm_fd(struct nvkvm_isolate_table *t,
				 uint32_t isolate_id,
				 uint32_t mode,
				 const void *state, uint32_t state_size,
				 const void *intent, uint32_t intent_size,
				 uint32_t prot, uint32_t map_flags,
				 uint64_t length, uint64_t host_va_hint,
				 uint64_t offset,
				 uint64_t *host_va_out, uint64_t *length_out,
				 uint64_t *token_out, uint32_t *rm_status_out);

/*
 * Register/deregister poll on a handle in the isolate.
 */
int nvkvm_isolate_poll(struct nvkvm_isolate_table *t,
		       uint32_t isolate_id, uint32_t handle_id,
		       uint32_t events);
int nvkvm_isolate_unpoll(struct nvkvm_isolate_table *t,
			 uint32_t isolate_id, uint32_t handle_id);

/* Kill all isolates belonging to a session. */
void nvkvm_isolate_kill_session(struct nvkvm_isolate_table *t,
				uint32_t session_id);

#endif /* NVKVM_ISOLATE_H */
