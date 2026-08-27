/*
 * virtio_nvgpu.c — QEMU virtio-nvgpu device backend
 *
 * Implements the VirtIONvgpu QEMU device which processes requests from the
 * guest nvkvm-guest.ko kernel module and forwards them to the real NVIDIA
 * driver on the host.
 *
 * Request lifecycle:
 *   1. Guest writes request to VQ_TX and kicks the VQ.
 *   2. nvkvm_tx_handler() is called (QEMU IOThread or BH).
 *   3. Request is validated, dispatched, and the real ioctl is called.
 *   4. Response is posted to VQ_RX.
 *   5. Guest RX callback wakes the waiting task.
 *
 * Threading: all VQ callbacks run in QEMU's IOThread. Per-session and per-FD
 * structures are protected by their own mutexes for future multi-threaded
 * dispatch. Currently we hold the QEMU BQL across dispatch for simplicity
 * and will relax this later.
 *
 * Audit F1-1 (hang audit 2026-08) — read this before adding a new request
 * type that talks to an isolate.  "We hold the BQL across dispatch" means a
 * blocking stub round-trip issued from here stalls the main loop, QMP, timers
 * and every vCPU for its whole duration.  Only NVKVM_REQ_IOCTL_ON_ISOLATE and
 * NVKVM_REQ_ENTER_LOOP are offloaded to the thread pool; everything else —
 * MMAP/MUNMAP/PRESENT/XISO_IMPORT/CLOSE_HANDLE/POLL/UNPOLL/COPY_HANDLE/
 * SETUP_RING/REALIZE — runs inline right here.  Those round-trips are no
 * longer able to block forever (nvkvm_isolate.c gives every one of them a
 * deadline and declares an unresponsive isolate dead), but the RESIDUAL RISK
 * is unchanged in kind: a wedged isolate still freezes the VM for up to that
 * deadline.  Removing that needs the offload, not a shorter timeout.
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"          /* nvkvm-gpu PCI identity device */
#include "hw/pci/pci_device.h"
#include "hw/core/boards.h"   /* current_machine->ram_size (#55 GPA-overlap guard) */
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "block/thread-pool.h"
#include "qemu/aio.h"
#include "system/memory.h"
#include "system/address-spaces.h"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include "virtio_nvgpu.h"
#include "nvkvm_present_egl.h"   /* #102 present-to-window console */
#include <dirent.h>

/*
 * host_dev_path was used by the legacy handle_open() that opened
 * /dev/nvidia* in QEMU's process. Step 3 moved opens to the stub.
 * nvkvm_handle_open_nvidia() in nvkvm_handle.c has its own path table.
 * Kept under #if 0 with the rest of the legacy handlers as a tombstone.
 */
#if 0
static const char *host_dev_path(int dev_id)
{
	static char buf[32];
	if (dev_id == NVKVM_DEV_CTL)
		return "/dev/nvidiactl";
	if (dev_id == NVKVM_DEV_UVM)
		return "/dev/nvidia-uvm";
	snprintf(buf, sizeof(buf), "/dev/nvidia%d", dev_id - 16);
	return buf;
}
#endif

/* ── Session management ──────────────────────────────────────────────────── */

struct nvkvm_session *nvkvm_session_find(VirtIONvgpu *nv, uint32_t session_id)
{
	struct nvkvm_session *s;
	TAILQ_FOREACH(s, &nv->sessions, link) {
		if (s->id == session_id)
			return s;
	}
	return NULL;
}

struct nvkvm_session *nvkvm_session_create(VirtIONvgpu *nv,
					   uint32_t session_id)
{
	struct nvkvm_session *s;

	s = g_new0(struct nvkvm_session, 1);
	s->id = session_id;
	pthread_mutex_init(&s->lock, NULL);
	pthread_mutex_init(&s->clients_lock, NULL);
	TAILQ_INIT(&s->fds);
	TAILQ_INIT(&s->mmaps);
	s->next_fd_token   = 1;
	s->next_mmap_token = 1;

	pthread_mutex_lock(&nv->sessions_lock);
	TAILQ_INSERT_TAIL(&nv->sessions, s, link);
	pthread_mutex_unlock(&nv->sessions_lock);
	return s;
}

/*
 * #80 (audit H-2/H-3): destroy a session whose last isolate has been killed.
 * Unlinks it from the table first (so no later lookup races a free), then
 * reclaims everything the guest is no longer able to close itself:
 *   - all host /dev/nvidia* + memfd fds for the session (force, ignoring
 *     isolate_refcount — the isolates are gone), which releases the kernel RM
 *     objects + GPU memory;
 *   - the QEMU-side RM object graph (clients[]);
 *   - the legacy fd / mmap lists (empty in the handle-based model, drained
 *     defensively);
 *   - the session struct + its mutexes.
 *
 * Safe to free here: every control request runs serialised on the single TX
 * virtqueue thread, and we are only called once nisolates == 0.  A pooled
 * IOCTL worker (NVKVM_REQ_IOCTL_ON_ISOLATE) may still be unwinding after
 * nvkvm_isolate_kill (which joins the isolate's reader thread, not the pool
 * workers), but the worker only touches the static handle/isolate tables and
 * its own stack — it never dereferences a nvkvm_session — so freeing the
 * session struct here cannot UAF it.
 */
void nvkvm_session_destroy(VirtIONvgpu *nv, struct nvkvm_session *session)
{
	if (!session)
		return;

	pthread_mutex_lock(&nv->sessions_lock);
	TAILQ_REMOVE(&nv->sessions, session, link);
	pthread_mutex_unlock(&nv->sessions_lock);

	/* Force-close host fds → releases kernel RM objects + GPU memory. */
	nvkvm_handle_close_session(&nv->handles, session->id);

	/*
	 * Free the QEMU-side RM object graph.
	 *
	 * DEAD-1: nothing POPULATES this graph any more.  register_client() and
	 * every nvkvm_obj_add() call site lived in nvkvm_frontend.c, which is
	 * deleted, so session->nclients is now always 0 and this loop always
	 * runs zero times.  nvkvm_objects.c is kept — it still has this caller,
	 * and tests/unit/test_objects.c pins its cascade-free semantics — but it
	 * is vestigial, and removing it means also removing the clients[] /
	 * nclients / clients_lock fields from struct nvkvm_session.  That is the
	 * same "Step 3d.3" cleanup the #if 0 tombstone below is waiting on; done
	 * as its own commit so the diff says so, and because this file is one of
	 * the three that tests/qemu_syntax_check.sh cannot compile.
	 */
	pthread_mutex_lock(&session->clients_lock);
	for (int i = 0; i < session->nclients; i++) {
		if (session->clients[i]) {
			nvkvm_client_free(session, session->clients[i]);
			session->clients[i] = NULL;
		}
	}
	session->nclients = 0;
	pthread_mutex_unlock(&session->clients_lock);

	/* Drain the legacy fd list (dead in the handle model; defensive). */
	while (!TAILQ_EMPTY(&session->fds)) {
		struct nvkvm_host_fd *hfd = TAILQ_FIRST(&session->fds);
		TAILQ_REMOVE(&session->fds, hfd, link);
		if (hfd->fd >= 0)
			close(hfd->fd);
		g_free(hfd->clients);
		pthread_mutex_destroy(&hfd->clients_lock);
		g_free(hfd);
	}
	while (!TAILQ_EMPTY(&session->mmaps)) {
		struct nvkvm_mmap_region *mr = TAILQ_FIRST(&session->mmaps);
		TAILQ_REMOVE(&session->mmaps, mr, link);
		g_free(mr);
	}

	pthread_mutex_destroy(&session->lock);
	pthread_mutex_destroy(&session->clients_lock);
	g_free(session);
}

struct nvkvm_host_fd *nvkvm_fd_lookup(struct nvkvm_session *session,
				      uint32_t fd_token)
{
	struct nvkvm_host_fd *hfd;
	TAILQ_FOREACH(hfd, &session->fds, link) {
		if (hfd->token == fd_token)
			return hfd;
	}
	return NULL;
}

uint32_t nvkvm_fd_alloc_token(struct nvkvm_session *session,
			      struct nvkvm_host_fd *hfd)
{
	uint32_t token = session->next_fd_token++;
	hfd->token = token;
	TAILQ_INSERT_TAIL(&session->fds, hfd, link);
	return token;
}

void nvkvm_fd_remove(struct nvkvm_session *session, uint32_t fd_token)
{
	struct nvkvm_host_fd *hfd = nvkvm_fd_lookup(session, fd_token);
	if (hfd) {
		TAILQ_REMOVE(&session->fds, hfd, link);
		close(hfd->fd);
		pthread_mutex_destroy(&hfd->clients_lock);
		g_free(hfd->clients);
		g_free(hfd);
	}
}

/* ── Shared memory slot validation ──────────────────────────────────────── */

static bool slot_valid(VirtIONvgpu *nv, uint32_t slot)
{
	return slot >= 1 && slot < NVKVM_SHM_NSLOTS;
}

static void *slot_ptr(VirtIONvgpu *nv, uint32_t slot)
{
	return (char *)nv->shm_base + (size_t)slot * nv->slot_size;
}

/*
 * Audit C-1: a guest-controlled `size` (param_size/aux_size/req->size, up to the
 * stub's 256 KiB cap) must NEVER exceed the 64 KiB slot it indexes, or the
 * subsequent send/recv/pread on slot_ptr() over-reads/over-writes past the slot
 * and past the 16 MiB shm region — a guest-driven OOB R/W in the privileged VMM.
 * Every live handler must obtain its shm pointer through this bounded helper,
 * which returns NULL unless the slot is valid AND [slot, slot+size) fits inside
 * both the slot and the whole shm region.  (The legacy path had this check; the
 * thread-pool/memory/realize paths lost it.)
 */
static void *slot_blob(VirtIONvgpu *nv, uint32_t slot, uint64_t size)
{
	uint64_t base;
	if (!slot_valid(nv, slot))
		return NULL;
	if (size > nv->slot_size)
		return NULL;
	base = (uint64_t)slot * nv->slot_size;
	if (base + size > nv->shm_size)        /* defense-in-depth vs the last slot */
		return NULL;
	return slot_ptr(nv, slot);
}

/*
 * Legacy NVKVM_REQ_OPEN / _CLOSE / _IOCTL / _MMAP / _MUNMAP request handlers.
 * These are dead code as of Step 3d.1 (guest module no longer sends these
 * request types). Kept under #if 0 as a tombstone until Step 3d.3 deletes
 * them along with the underlying nvkvm_host_fd / session->fds machinery.
 */
#if 0

/* ── OPEN request handler ─────────────────────────────────────────────────── */

static void handle_open(VirtIONvgpu *nv, VirtQueue *vq,
			VirtQueueElement *elem,
			const struct nvkvm_hdr *hdr,
			const struct nvkvm_req_open *req)
{
	struct {
		struct nvkvm_hdr       hdr;
		struct nvkvm_resp_open resp;
	} resp_msg = {
		.hdr.type   = hdr->type,
		.hdr.txn_id = hdr->txn_id,
	};
	uint32_t dev_id    = le32_to_cpu(req->dev_id);
	uint32_t flags     = le32_to_cpu(req->flags);
	uint32_t session_id = le32_to_cpu(req->session_id);
	struct nvkvm_session *session;
	struct nvkvm_host_fd *hfd;
	int fd;
	const char *path;

	/* Sanitize dev_id */
	if (dev_id != NVKVM_DEV_CTL && dev_id != NVKVM_DEV_UVM &&
	    !(dev_id >= NVKVM_DEV_GPU(0) &&
	      dev_id <= NVKVM_DEV_GPU(NV_MINOR_DEVICE_NUMBER_REGULAR_MAX))) {
		resp_msg.resp.status = cpu_to_le32(EINVAL);
		goto send;
	}

	/* Sanitize flags: only allow O_RDONLY / O_RDWR / O_CLOEXEC */
	flags &= O_RDONLY | O_RDWR | O_CLOEXEC;

	path = host_dev_path(dev_id);
	fd = open(path, (int)flags | O_CLOEXEC);
	if (fd < 0) {
		resp_msg.resp.status = cpu_to_le32(errno);
		goto send;
	}

	/* Find or create the session for the requesting guest process. */
	pthread_mutex_lock(&nv->sessions_lock);
	session = nvkvm_session_find(nv, session_id);
	if (!session) {
		pthread_mutex_unlock(&nv->sessions_lock);
		session = nvkvm_session_create(nv, session_id);
	} else {
		pthread_mutex_unlock(&nv->sessions_lock);
	}

	hfd = g_new0(struct nvkvm_host_fd, 1);
	hfd->fd         = fd;
	hfd->dev_id     = dev_id;
	hfd->session_id = session_id;
	pthread_mutex_init(&hfd->clients_lock, NULL);

	pthread_mutex_lock(&session->lock);
	nvkvm_fd_alloc_token(session, hfd);
	pthread_mutex_unlock(&session->lock);

	resp_msg.resp.fd_token = cpu_to_le32(hfd->token);
	resp_msg.resp.status   = 0;
	NVKVM_DBG( "nvkvm: open: dev_id=%u session=%u token=%u host_fd=%d\n",
		dev_id, session->id, hfd->token, hfd->fd);

send:
	iov_from_buf(elem->in_sg, elem->in_num, 0,
		     &resp_msg, sizeof(resp_msg));
	virtqueue_push(vq, elem, sizeof(resp_msg));
	virtio_notify(VIRTIO_DEVICE(nv), vq);
}

/* ── CLOSE request handler ───────────────────────────────────────────────── */

static void handle_close(VirtIONvgpu *nv, VirtQueue *vq,
			 VirtQueueElement *elem,
			 const struct nvkvm_hdr *hdr,
			 const struct nvkvm_req_close *req)
{
	struct {
		struct nvkvm_hdr        hdr;
		struct nvkvm_resp_close resp;
	} resp_msg = {
		.hdr.type   = hdr->type,
		.hdr.txn_id = hdr->txn_id,
	};
	uint32_t fd_token = le32_to_cpu(req->fd_token);
	struct nvkvm_session *session = NULL;

	/* Find the session owning this fd_token */
	pthread_mutex_lock(&nv->sessions_lock);
	{
		struct nvkvm_session *s;
		TAILQ_FOREACH(s, &nv->sessions, link) {
			struct nvkvm_host_fd *hfd = nvkvm_fd_lookup(s, fd_token);
			if (hfd) {
				session = s;
				break;
			}
		}
	}
	pthread_mutex_unlock(&nv->sessions_lock);

	if (!session) {
		resp_msg.resp.status = cpu_to_le32(EBADF);
		goto send;
	}

	pthread_mutex_lock(&session->lock);
	nvkvm_fd_remove(session, fd_token);
	pthread_mutex_unlock(&session->lock);
	resp_msg.resp.status = 0;

send:
	iov_from_buf(elem->in_sg, elem->in_num, 0,
		     &resp_msg, sizeof(resp_msg));
	virtqueue_push(vq, elem, sizeof(resp_msg));
	virtio_notify(VIRTIO_DEVICE(nv), vq);
}

/* ── IOCTL request handler ───────────────────────────────────────────────────
 *
 * REMOVED (A-1).  handle_ioctl() lived here and was dead: static, and with no
 * callers.  It was the sole caller of nvkvm_dispatch_ioctl(), which contains a
 * `p->p_memory = 0` for NV_ESC_RM_ALLOC_MEMORY that reads exactly like the
 * sanitisation for the OS-descriptor class -- and is not, because nothing
 * reaches it.  A reviewer (and an auditor) can find that line, conclude the
 * pointer is handled, and be wrong; that is how A-1 survived.
 *
 * The live path is virtio_nvgpu.c's IOCTL_ON_ISOLATE case ->
 * nvkvm_ioctl_work_fn -> nvkvm_req_ioctl_on_isolate (nvkvm_isolate_handlers.c),
 * where the A-1 gate now lives.
 *
 * ── DEAD-1, 2026-08-24 ── that removal stopped one step short.
 *
 * nvkvm_dispatch.c and nvkvm_frontend.c are now DELETED, not merely documented.
 * The call graph, verified by grep over the whole tree: nvkvm_dispatch_ioctl()
 * had zero callers once handle_ioctl() went; nvkvm_ioctl_expected_param_size()
 * had zero callers outside its own unit test; and all eight exported functions
 * of nvkvm_frontend.c (562 lines) were called ONLY from nvkvm_dispatch.c.  So
 * the pair was unreachable end to end while still being compiled and linked,
 * which is exactly what made it look alive -- and its header comment read as a
 * specification of live controls ("Security invariants: we verify all handles
 * ... we ensure NV01_ROOT_CLIENT allocations are unprivileged ... we do not
 * allow handles from one session to appear in another session's requests").
 * None of the three executed.
 *
 * Deleting a control is only safe if the live path has its own.  Every security
 * fix that had landed in the pair, and where it lives now:
 *
 *   U-7, NVOS64.pRightsRequested zeroed for every class
 *        (was nvkvm_frontend.c:129)
 *        LIVE: zero_nvos64_rights(), src/stub/nvkvm_stub.c, called
 *        unconditionally on the worker path.  Unit-pinned by
 *        tests/unit/test_stub_ptr_sanitize (5 U-7 cases).  This is also the
 *        code behind the header's "NV01_ROOT_CLIENT allocations are
 *        unprivileged" invariant -- one fix, advertised twice.
 *        The audit's "fixed on both paths" was always "fixed on one".
 *
 *   G-2, NV_ESC_RM_IDLE_CHANNELS array pointers neutralised
 *        (was nvkvm_dispatch.c, the NVKVM_IDLE_MAX_CHANNELS block)
 *        LIVE: the memset of [12,40) in src/stub/nvkvm_stub.c's worker path.
 *        The dead copy already carried a "do not rely on it" banner (P2-1).
 *
 *   A-1, p_memory = 0 on NV_ESC_RM_ALLOC_MEMORY
 *        (was nvkvm_dispatch.c)
 *        LIVE: the iso_mmap_covers() gate in nvkvm_req_ioctl_on_isolate().
 *        Stronger than the dead one, which only zeroed the field.
 *
 *   R-1's shape: `if (_IOC_TYPE(cmd) != 'F') return -ENOTTY;` before the NR
 *        dispatch (was nvkvm_dispatch.c)
 *        LIVE: nvkvm_ioctl_type_matches_dev() plus the M-A default-deny, both
 *        in nvkvm_isolate_handlers.c.  Stronger: it binds the type to the
 *        handle's device rather than only excluding non-'F'.
 *
 *   U-5, RM_CONTROL params_size bounded by the aux blob
 *        (was nvkvm_frontend.c:344-348, as a rejection)
 *        LIVE: clamp_inner_params_size(), src/stub/nvkvm_stub.c, on both the
 *        worker and ring paths.  Unit-pinned (12 U-5 cases).  Note the live one
 *        CLAMPS where the dead one REJECTED; that is the established U-5 fix,
 *        not a regression introduced here.
 *
 *   per-session hClient validation via find_client()
 *        (was nvkvm_frontend.c, on ALLOC/FREE/CONTROL/DUP)
 *        LIVE, but at a different granularity ON PURPOSE: the H-3 hClient
 *        allowlist in nvkvm_req_ioctl_on_isolate() is per-VM, not per-session,
 *        and covers a SUPERSET of the NRs (ALLOC, ALLOC_MEMORY, CONTROL, FREE,
 *        DUP, SHARE, MAP, UNMAP, MAP_DMA, UNMAP_DMA, VID_HEAP).  The header's
 *        third invariant -- "no handles from one session in another session's
 *        requests" -- has NO live equivalent, and that is a recorded decision,
 *        not an oversight: see the long note at the top of
 *        nvkvm_req_ioctl_on_isolate() and audit GP-A-2.  Intra-VM access
 *        control is the guest module's job; QEMU's boundary is cross-VM.
 *        Known residuals on the live gate: it is inert while
 *        client_allow_n == 0 (R-6) and under-covers its own stated scope by
 *        five allowlisted NRs (R-5).  Both predate this deletion.
 *
 *   NVOS33.pLinearAddress zeroed before the reply reaches the guest
 *        (was nvkvm_dispatch.c, "guest must not use host VA")
 *        NO LIVE EQUIVALENT -- and this is already recorded, as U-11 in
 *        docs/internal/audit-guest-pointers.md, whose table names this exact
 *        line as dead code and rates the field UNENFORCED.  Deleting it changes
 *        nothing about the live behaviour: on a successful nr 0x4e the isolate
 *        VA the driver wrote at offset 32 is recorded host-side
 *        (nvkvm_mapva_record) and then copied back to the guest as-is.  What
 *        the deletion changes is that nobody can now mistake the dead line for
 *        the fix.  Left open deliberately rather than closed in passing, so it
 *        stays U-11's decision and not a drive-by one.
 *
 * The param-size table (nvkvm_ioctl_expected_param_size) was not a fix for any
 * named finding and had no live caller either.  The live bound on a guest's
 * declared sizes is slot_blob()'s C-1 check above, plus the per-gate minimums
 * that fail closed individually; the per-command size table that still runs is
 * the guest's own (src/guest/nvkvm_ioctl.c), which is guest code and therefore
 * not a control.  Its 19 unit assertions went with it -- see the header of
 * tests/unit/test_objects.c, which records exactly what was dropped.
 */


/* ── MMAP request handler ────────────────────────────────────────────────── */

static void handle_mmap(VirtIONvgpu *nv, VirtQueue *vq,
			VirtQueueElement *elem,
			const struct nvkvm_hdr *hdr,
			const struct nvkvm_req_mmap *req)
{
	struct {
		struct nvkvm_hdr       hdr;
		struct nvkvm_resp_mmap resp;
	} resp_msg = {
		.hdr.type   = hdr->type,
		.hdr.txn_id = hdr->txn_id,
	};
	uint32_t fd_token = le32_to_cpu(req->fd_token);
	uint64_t offset   = le64_to_cpu(req->offset);
	uint64_t length   = le64_to_cpu(req->length);
	int      prot     = (int)le32_to_cpu(req->prot);
	int      flags    = MAP_SHARED;
	struct nvkvm_session *session = NULL;
	struct nvkvm_host_fd *hfd;
	struct nvkvm_mmap_region *region = NULL;
	int ret;

	/* Validate length: must be non-zero, ≤1 GiB, page-aligned */
	if (!length || length > (1UL << 30) || (length & (4096UL - 1))) {
		resp_msg.resp.status = cpu_to_le32(EINVAL);
		goto send;
	}

	/* Sanitize prot: only read/write allowed */
	prot &= PROT_READ | PROT_WRITE;

	/* Find session+fd */
	pthread_mutex_lock(&nv->sessions_lock);
	{
		struct nvkvm_session *s;
		TAILQ_FOREACH(s, &nv->sessions, link) {
			hfd = nvkvm_fd_lookup(s, fd_token);
			if (hfd) { session = s; break; }
		}
	}
	pthread_mutex_unlock(&nv->sessions_lock);
	if (!session || !hfd) {
		resp_msg.resp.status = cpu_to_le32(EBADF);
		goto send;
	}

	ret = nvkvm_mmap_create(nv, hfd, offset, length, prot, flags, &region);
	if (ret) {
		resp_msg.resp.status = cpu_to_le32((uint32_t)-ret);
		goto send;
	}

	ret = nvkvm_mmap_map_to_guest(nv, region);
	if (ret) {
		nvkvm_mmap_destroy(nv, region);
		resp_msg.resp.status = cpu_to_le32((uint32_t)-ret);
		goto send;
	}

	pthread_mutex_lock(&session->lock);
	region->token = session->next_mmap_token++;
	TAILQ_INSERT_TAIL(&session->mmaps, region, link);
	pthread_mutex_unlock(&session->lock);

	resp_msg.resp.gpa_base    = cpu_to_le64(region->guest_pa);
	resp_msg.resp.length      = cpu_to_le64(region->length);
	resp_msg.resp.mmap_token  = cpu_to_le32(region->token);
	resp_msg.resp.status      = 0;

send:
	iov_from_buf(elem->in_sg, elem->in_num, 0,
		     &resp_msg, sizeof(resp_msg));
	virtqueue_push(vq, elem, sizeof(resp_msg));
	virtio_notify(VIRTIO_DEVICE(nv), vq);
}

/* ── MUNMAP request handler ──────────────────────────────────────────────── */

static void handle_munmap(VirtIONvgpu *nv, VirtQueue *vq,
			  VirtQueueElement *elem,
			  const struct nvkvm_hdr *hdr,
			  const struct nvkvm_req_munmap *req)
{
	struct {
		struct nvkvm_hdr         hdr;
		struct nvkvm_resp_munmap resp;
	} resp_msg = {
		.hdr.type   = hdr->type,
		.hdr.txn_id = hdr->txn_id,
	};
	uint32_t mmap_token = le32_to_cpu(req->mmap_token);
	struct nvkvm_mmap_region *region = NULL;

	pthread_mutex_lock(&nv->sessions_lock);
	{
		struct nvkvm_session *s;
		TAILQ_FOREACH(s, &nv->sessions, link) {
			struct nvkvm_mmap_region *r;
			TAILQ_FOREACH(r, &s->mmaps, link) {
				if (r->token == mmap_token) {
					region = r;
					TAILQ_REMOVE(&s->mmaps, r, link);
					break;
				}
			}
			if (region) break;
		}
	}
	pthread_mutex_unlock(&nv->sessions_lock);

	if (!region) {
		resp_msg.resp.status = cpu_to_le32(ENOENT);
		goto send;
	}

	nvkvm_mmap_unmap_from_guest(nv, region);
	nvkvm_mmap_destroy(nv, region);
	resp_msg.resp.status = 0;

send:
	iov_from_buf(elem->in_sg, elem->in_num, 0,
		     &resp_msg, sizeof(resp_msg));
	virtqueue_push(vq, elem, sizeof(resp_msg));
	virtio_notify(VIRTIO_DEVICE(nv), vq);
}
#endif /* legacy NVKVM_REQ_OPEN/CLOSE/IOCTL/MMAP/MUNMAP handlers */

/* ── VQ_TX callback ──────────────────────────────────────────────────────── */

/*
 * Asynchronous IOCTL_ON_ISOLATE dispatch.
 *
 * IOCTL_ON_ISOLATE is the hot path: a CUDA process issues thousands of them,
 * and each blocks until the per-isolate stub round-trip (or a UVM ioctl in
 * QEMU's own process) completes.  Running it inline in nvkvm_tx_handler would
 * block the single virtio TX thread, so a second concurrent guest process
 * whose request sits behind it in the ring is starved — and if one isolate's
 * stub wedges, every other guest hangs forever in wait_for_completion.
 *
 * Instead we offload each IOCTL_ON_ISOLATE to QEMU's thread pool: the worker
 * runs the (blocking) handler off the main loop, and the completion callback
 * — which runs back on the device AioContext, BQL held — pushes the response
 * onto the virtqueue.  The TX handler returns immediately to pop the next
 * request, so independent isolates run truly in parallel and a wedged isolate
 * no longer starves the others.  Responses may complete out of order; the
 * guest demuxes by txn_id (see nvkvm_tx_done_callback), so that is fine.
 *
 * Only IOCTL_ON_ISOLATE is offloaded.  MMAP/REALIZE stay synchronous: they
 * touch KVM memslots (KVM_SET_USER_MEMORY_REGION), which we keep on the main
 * loop, and they are infrequent (device bring-up, not the compute hot path).
 */
struct nvkvm_ioctl_work {
	VirtIONvgpu                       *nv;
	VirtQueue                         *vq;
	VirtQueueElement                  *elem;
	struct nvkvm_hdr                   hdr;
	struct nvkvm_req_ioctl_on_isolate  req;
	void                              *param_buf;   /* shm — stable */
	void                              *aux_buf;     /* shm — stable */
	struct nvkvm_resp_ioctl_on_isolate resp;
};

/* Worker thread: run the blocking ioctl handler off the main loop. */
static int nvkvm_ioctl_work_fn(void *opaque)
{
	struct nvkvm_ioctl_work *w = opaque;
	/*
	 * Audit P2-2: param_buf/aux_buf point into the guest-shared SHM slot,
	 * and this worker runs on the thread pool CONCURRENTLY with the guest
	 * vCPUs.  The handler's allowlist gates (NVKMS cmdType, alloc class,
	 * ctrl cmd, and crucially the cross-VM DUP_OBJECT src-client + hClient
	 * gates) read these bytes, then the very same bytes are shipped to the
	 * stub — a second vCPU can flip an allowed value to a denied one in the
	 * window between (a classic double-fetch that defeats the cross-tenant
	 * gates).  Snapshot the slot into a worker-private buffer ONCE up front
	 * so every gate checks, and the stub receives, the SAME bytes; copy the
	 * host writebacks back to the SHM slot afterwards (param/aux are in/out).
	 */
	void *shm_param = w->param_buf, *shm_aux = w->aux_buf;
	void *priv_param = NULL, *priv_aux = NULL;
	if (w->req.param_size > 0 && shm_param) {
		priv_param = g_malloc(w->req.param_size);
		memcpy(priv_param, shm_param, w->req.param_size);
	}
	if (w->req.aux_size > 0 && shm_aux) {
		priv_aux = g_malloc(w->req.aux_size);
		memcpy(priv_aux, shm_aux, w->req.aux_size);
	}
	nvkvm_req_ioctl_on_isolate(w->nv, &w->req, &w->resp,
				   priv_param ? priv_param : shm_param,
				   priv_aux   ? priv_aux   : shm_aux);
	if (priv_param) {
		memcpy(shm_param, priv_param, w->req.param_size);
		g_free(priv_param);
	}
	if (priv_aux) {
		memcpy(shm_aux, priv_aux, w->req.aux_size);
		g_free(priv_aux);
	}
	return 0;
}

/* Completion: runs on the device AioContext (BQL held) — ring ops are safe. */
static void nvkvm_ioctl_work_done(void *opaque, int ret)
{
	struct nvkvm_ioctl_work *w = opaque;
	struct { struct nvkvm_hdr h;
		 struct nvkvm_resp_ioctl_on_isolate r; } out;
	out.h = w->hdr;
	out.r = w->resp;
	iov_from_buf(w->elem->in_sg, w->elem->in_num, 0, &out, sizeof(out));
	virtqueue_push(w->vq, w->elem, sizeof(out));
	virtio_notify(VIRTIO_DEVICE(w->nv), w->vq);
	g_free(w->elem);
	g_free(w);
}

/*
 * ENTER_LOOP offload.  nvkvm_isolate_enter_loop blocks for the whole consumer-
 * loop lifetime (until the stub idles out), so it must NOT run on the main loop.
 * Mirror the IOCTL offload: a thread-pool worker blocks, the completion pushes
 * the response.  Holding one pool thread per active isolate's pump is fine at
 * our scale (the pool grows to 64) and the short idle window cycles it.
 */
struct nvkvm_enter_loop_work {
	VirtIONvgpu                  *nv;
	VirtQueue                    *vq;
	VirtQueueElement             *elem;
	struct nvkvm_hdr              hdr;
	struct nvkvm_req_enter_loop   req;
	struct nvkvm_resp_enter_loop  resp;
};

static int nvkvm_enter_loop_work_fn(void *opaque)
{
	struct nvkvm_enter_loop_work *w = opaque;
	nvkvm_req_enter_loop(w->nv, &w->req, &w->resp);
	return 0;
}

static void nvkvm_enter_loop_work_done(void *opaque, int ret)
{
	struct nvkvm_enter_loop_work *w = opaque;
	struct { struct nvkvm_hdr h;
		 struct nvkvm_resp_enter_loop r; } out;
	out.h = w->hdr;
	out.r = w->resp;
	iov_from_buf(w->elem->in_sg, w->elem->in_num, 0, &out, sizeof(out));
	virtqueue_push(w->vq, w->elem, sizeof(out));
	virtio_notify(VIRTIO_DEVICE(w->nv), w->vq);
	g_free(w->elem);
	g_free(w);
}

/* ── #127 async os-event delivery: host fd ready → VQ_EVT → guest poll_wq ─────
 * The stub background-polls registered host os-event fds and, on readiness,
 * sends ISOLATE_RESP_POLL_EVENT on its reader socket.  The isolate reader thread
 * calls nvkvm_virtio_push_evt(), which hops onto the device AioContext (BQL held)
 * via a one-shot BH so the vq_evt push is serialized with the rest of the device
 * — the reader thread must never touch a VirtQueue directly.  The guest's
 * vq_evt callback (nvkvm_evt_callback → nvkvm_evt_deliver) matches the buffer's
 * (isolate_id, handle_id) to the waiting fd's poll_wq and wakes it.  Without this
 * libnvidia falls back to an ~18 ms poll-timeout-recheck per blocking-sync wait. */
struct nvkvm_evt_push {
	VirtIONvgpu *nv;
	uint32_t     isolate_id;
	uint32_t     handle_id;
	uint32_t     revents;
};

static void nvkvm_evt_push_bh(void *opaque)
{
	struct nvkvm_evt_push *p = opaque;
	VirtQueueElement *elem = virtqueue_pop(p->nv->vq_evt, sizeof(*elem));
	if (elem) {
		struct nvkvm_evt_poll msg = {
			.isolate_id = cpu_to_le32(p->isolate_id),
			.handle_id  = cpu_to_le32(p->handle_id),
			.events     = cpu_to_le32(p->revents),
			.type       = cpu_to_le32(NVKVM_EVT_TYPE_POLL),
		};
		iov_from_buf(elem->in_sg, elem->in_num, 0, &msg, sizeof(msg));
		virtqueue_push(p->nv->vq_evt, elem, sizeof(msg));
		virtio_notify(VIRTIO_DEVICE(p->nv), p->nv->vq_evt);
		g_free(elem);
	} else {
		/* No pre-posted evt buffer free right now.  The guest re-arms its
		 * poll and the still-readable host fd re-fires, so this is
		 * recoverable (one missed wake, not a lost completion). */
		NVKVM_DBG("nvkvm: vq_evt full, dropped poll-event iso=%u h=%u\n",
			  p->isolate_id, p->handle_id);
	}
	g_free(p);
}

void nvkvm_virtio_push_evt(VirtIONvgpu *nv, uint32_t isolate_id,
			   uint32_t handle_id, uint32_t revents)
{
	struct nvkvm_evt_push *p = g_malloc(sizeof(*p));
	p->nv = nv;
	p->isolate_id = isolate_id;
	p->handle_id  = handle_id;
	p->revents    = revents;
	aio_bh_schedule_oneshot(qemu_get_aio_context(), nvkvm_evt_push_bh, p);
}

/* ── ui_info: the host window's size, on its way to the guest's KMS head ─────
 * Same VQ_EVT transport and the same BH hop as the poll events above, for the
 * same reason: this is called from the UI/main path, and only the device's
 * AioContext may touch a VirtQueue.
 *
 * Dropping it when the queue is full is correct here and NOT merely tolerable:
 * the payload is the CURRENT window size, so a lost event is superseded by the
 * next one rather than lost work.  Resizing is a gesture that produces a
 * stream, and the last one is the only one that matters. */
struct nvkvm_ui_info_push {
	VirtIONvgpu *nv;
	uint32_t     width;
	uint32_t     height;
	uint32_t     refresh_mhz;   /* 0 = unknown; the guest keeps its default */
};

static void nvkvm_ui_info_push_bh(void *opaque)
{
	struct nvkvm_ui_info_push *p = opaque;
	VirtQueueElement *elem = virtqueue_pop(p->nv->vq_evt, sizeof(*elem));

	if (elem) {
		struct nvkvm_evt_ui_info msg = {
			.width    = cpu_to_le32(p->width),
			.height   = cpu_to_le32(p->height),
			.refresh_mhz = cpu_to_le32(p->refresh_mhz),
			.type     = cpu_to_le32(NVKVM_EVT_TYPE_UI_INFO),
		};
		iov_from_buf(elem->in_sg, elem->in_num, 0, &msg, sizeof(msg));
		virtqueue_push(p->nv->vq_evt, elem, sizeof(msg));
		virtio_notify(VIRTIO_DEVICE(p->nv), p->nv->vq_evt);
		g_free(elem);
	} else {
		NVKVM_DBG("nvkvm: vq_evt full, dropped ui_info %ux%u\n",
			  p->width, p->height);
	}
	g_free(p);
}

void nvkvm_virtio_push_ui_info(VirtIONvgpu *nv, uint32_t width,
			       uint32_t height, uint32_t refresh_mhz)
{
	struct nvkvm_ui_info_push *p;

	if (!nv || !nv->vq_evt || !width || !height) {
		return;
	}
	p = g_malloc(sizeof(*p));
	p->nv     = nv;
	p->width       = width;
	p->height      = height;
	p->refresh_mhz = refresh_mhz;
	aio_bh_schedule_oneshot(qemu_get_aio_context(), nvkvm_ui_info_push_bh, p);
}

/* ── buffer release: the host display is done reading a guest scanout bo ─────
 * Same VQ_EVT transport and the same BH hop as the two above, for the same
 * reason -- this is called from the relay's main-loop socket handler.
 *
 * DROPPING WHEN THE QUEUE IS FULL IS SAFE, and that is a property of the
 * consumer rather than of this function: the guest treats a release as a hint
 * that one named buffer is free again, and falls back to timer-driven flip
 * completion when the hints stop (nvkvm_kms.c).  A lost release therefore costs
 * at most one deferred flip, never a stalled head.  It is NOT superseded by the
 * next event the way a ui_info is -- each one names a different buffer -- so it
 * is logged rather than silently dropped.
 */
struct nvkvm_release_push {
	VirtIONvgpu *nv;
	uint32_t     isolate_id;
	uint32_t     stub_handle;
};

static void nvkvm_release_push_bh(void *opaque)
{
	struct nvkvm_release_push *p = opaque;
	VirtQueueElement *elem = virtqueue_pop(p->nv->vq_evt, sizeof(*elem));

	if (elem) {
		struct nvkvm_evt_release msg = {
			.isolate_id  = cpu_to_le32(p->isolate_id),
			.stub_handle = cpu_to_le32(p->stub_handle),
			.reserved    = 0,
			.type        = cpu_to_le32(NVKVM_EVT_TYPE_RELEASE),
		};
		iov_from_buf(elem->in_sg, elem->in_num, 0, &msg, sizeof(msg));
		virtqueue_push(p->nv->vq_evt, elem, sizeof(msg));
		virtio_notify(VIRTIO_DEVICE(p->nv), p->nv->vq_evt);
		g_free(elem);
	} else {
		NVKVM_DBG("nvkvm: vq_evt full, dropped release iso=%u gem=0x%x\n",
			  p->isolate_id, p->stub_handle);
	}
	g_free(p);
}

void nvkvm_virtio_push_buf_release(VirtIONvgpu *nv, uint32_t isolate_id,
				   uint32_t stub_handle)
{
	struct nvkvm_release_push *p;

	if (!nv || !nv->vq_evt || !stub_handle) {
		return;
	}
	p = g_malloc(sizeof(*p));
	p->nv          = nv;
	p->isolate_id  = isolate_id;
	p->stub_handle = stub_handle;
	aio_bh_schedule_oneshot(qemu_get_aio_context(), nvkvm_release_push_bh, p);
}

static void nvkvm_tx_handler(VirtIODevice *vdev, VirtQueue *vq)
{
	VirtIONvgpu *nv = VIRTIO_NVGPU(vdev);
	VirtQueueElement *elem;

	while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement))) != NULL) {
		struct nvkvm_hdr hdr;
		size_t hdr_len;

		hdr_len = iov_to_buf(elem->out_sg, elem->out_num,
				     0, &hdr, sizeof(hdr));
		if (hdr_len < sizeof(hdr)) {
			error_report("nvkvm: short request header");
			virtqueue_push(vq, elem, 0);
			g_free(elem);
			continue;
		}
		switch (le32_to_cpu(hdr.type)) {

		/* ── Isolate/handle request types ────────────────────────────────── */

#define ISOLATE_REQ(TYPE, req_t, resp_t, handler) \
		case TYPE: { \
			struct req_t  req  = {0}; \
			struct resp_t resp = {0}; \
			struct { struct nvkvm_hdr h; struct resp_t r; } out; \
			iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), \
				   &req, sizeof(req)); \
			handler(nv, &req, &resp); \
			out.h = hdr; out.r = resp; \
			iov_from_buf(elem->in_sg, elem->in_num, 0, &out, sizeof(out)); \
			virtqueue_push(vq, elem, sizeof(out)); \
			virtio_notify(VIRTIO_DEVICE(nv), vq); \
			break; \
		}

		ISOLATE_REQ(NVKVM_REQ_LIST_NVIDIA_DEVICES,
			    nvkvm_req_list_nvidia_devices,
			    nvkvm_resp_list_nvidia_devices,
			    nvkvm_req_list_nvidia_devices)
		ISOLATE_REQ(NVKVM_REQ_OPEN_NVIDIA_HANDLE,
			    nvkvm_req_open_nvidia_handle,
			    nvkvm_resp_open_nvidia_handle,
			    nvkvm_req_open_nvidia_handle)
		/* #106 present: does a bounded stub round-trip (PRIME export)
		 * inline on the TX thread. TODO(perf): offload to the thread
		 * pool like NVKVM_REQ_IOCTL_ON_ISOLATE if per-frame TX stall
		 * matters for a high-fps desktop. */
		ISOLATE_REQ(NVKVM_REQ_PRESENT,
			    nvkvm_req_present,
			    nvkvm_resp_present,
			    nvkvm_req_present)
		/* #110 cross-isolate dma-buf import: two bounded stub round-trips
		 * (owner PRIME export + importer PRIME import) inline on the TX
		 * thread, like PRESENT. */
		ISOLATE_REQ(NVKVM_REQ_XISO_IMPORT,
			    nvkvm_req_xiso_import,
			    nvkvm_resp_xiso_import,
			    nvkvm_req_xiso_import)
		ISOLATE_REQ(NVKVM_REQ_OPEN_MEMORY_HANDLE,
			    nvkvm_req_open_memory_handle,
			    nvkvm_resp_open_memory_handle,
			    nvkvm_req_open_memory_handle)
		ISOLATE_REQ(NVKVM_REQ_CLOSE_HANDLE,
			    nvkvm_req_close_handle,
			    nvkvm_resp_close_handle,
			    nvkvm_req_close_handle)
		ISOLATE_REQ(NVKVM_REQ_CREATE_ISOLATE,
			    nvkvm_req_create_isolate,
			    nvkvm_resp_create_isolate,
			    nvkvm_req_create_isolate)
		ISOLATE_REQ(NVKVM_REQ_KILL_ISOLATE,
			    nvkvm_req_kill_isolate,
			    nvkvm_resp_kill_isolate,
			    nvkvm_req_kill_isolate)
		ISOLATE_REQ(NVKVM_REQ_INTERRUPT,
			    nvkvm_req_interrupt,
			    nvkvm_resp_interrupt,
			    nvkvm_req_interrupt)
		ISOLATE_REQ(NVKVM_REQ_SETUP_RING,
			    nvkvm_req_setup_ring,
			    nvkvm_resp_setup_ring,
			    nvkvm_req_setup_ring)

		case NVKVM_REQ_ENTER_LOOP: {
			/* Blocks for the whole consumer-loop lifetime → offload to
			 * the thread pool; completion pushes the response. Ownership
			 * of `elem` transfers to the work item (we `continue`). */
			struct nvkvm_enter_loop_work *w =
				g_new0(struct nvkvm_enter_loop_work, 1);
			w->nv   = nv;
			w->vq   = vq;
			w->elem = elem;
			w->hdr  = hdr;
			iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr),
				   &w->req, sizeof(w->req));
			thread_pool_submit_aio(nvkvm_enter_loop_work_fn, w,
					       nvkvm_enter_loop_work_done, w);
			continue;
		}
		ISOLATE_REQ(NVKVM_REQ_COPY_HANDLE_TO_ISOLATE,
			    nvkvm_req_copy_handle_to_isolate,
			    nvkvm_resp_copy_handle_to_isolate,
			    nvkvm_req_copy_handle_to_isolate)
		ISOLATE_REQ(NVKVM_REQ_CLOSE_HANDLE_ON_ISOLATE,
			    nvkvm_req_close_handle_on_isolate,
			    nvkvm_resp_close_handle_on_isolate,
			    nvkvm_req_close_handle_on_isolate)
		ISOLATE_REQ(NVKVM_REQ_POLL_ON_ISOLATE,
			    nvkvm_req_poll_on_isolate,
			    nvkvm_resp_poll_on_isolate,
			    nvkvm_req_poll_on_isolate)
		ISOLATE_REQ(NVKVM_REQ_UNPOLL_ON_ISOLATE,
			    nvkvm_req_unpoll_on_isolate,
			    nvkvm_resp_unpoll_on_isolate,
			    nvkvm_req_unpoll_on_isolate)

		case NVKVM_REQ_IOCTL_ON_ISOLATE: {
			/*
			 * Offload to the thread pool so a blocking stub
			 * round-trip does not stall the single TX thread and
			 * starve other guests.  The completion callback pushes
			 * the response.  Ownership of `elem` transfers to the
			 * work item — do NOT g_free it here (we `continue`).
			 */
			struct nvkvm_ioctl_work *w =
				g_new0(struct nvkvm_ioctl_work, 1);
			w->nv   = nv;
			w->vq   = vq;
			w->elem = elem;
			w->hdr  = hdr;
			iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr),
				   &w->req, sizeof(w->req));
			/* param/aux buffers live in shared memory; the guest
			 * keeps the slot reserved until it sees the response,
			 * so these pointers stay valid for the whole job.
			 * Audit C-1: bound size<=slot_size via slot_blob; on a
			 * malformed/oversize request clamp the size to 0 so the
			 * worker's send/recv touches nothing (no OOB, no NULL+len
			 * deref). */
			w->param_buf = NULL;
			w->aux_buf   = NULL;
			if (w->req.param_size > 0) {
				w->param_buf = slot_blob(nv, w->req.shm_slot,
							 w->req.param_size);
				if (!w->param_buf)
					w->req.param_size = 0;
			}
			if (w->req.aux_size > 0) {
				w->aux_buf = slot_blob(nv, w->req.shm_aux_slot,
						       w->req.aux_size);
				if (!w->aux_buf)
					w->req.aux_size = 0;
			}
			thread_pool_submit_aio(nvkvm_ioctl_work_fn, w,
					       nvkvm_ioctl_work_done, w);
			continue;  /* elem now owned by the work item */
		}

		case NVKVM_REQ_MMAP_ON_ISOLATE: {
			struct nvkvm_req_mmap_on_isolate  req  = {0};
			struct nvkvm_resp_mmap_on_isolate resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			nvkvm_req_mmap_on_isolate(nv, &req, &resp);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_mmap_on_isolate r; } out;
			out.h = hdr; out.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &out, sizeof(out));
			virtqueue_push(vq, elem, sizeof(out));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

		case NVKVM_REQ_MUNMAP_ON_ISOLATE: {
			struct nvkvm_req_munmap_on_isolate  req  = {0};
			struct nvkvm_resp_munmap_on_isolate resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			nvkvm_req_munmap_on_isolate(nv, &req, &resp);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_munmap_on_isolate r; } out;
			out.h = hdr; out.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &out, sizeof(out));
			virtqueue_push(vq, elem, sizeof(out));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

		case NVKVM_REQ_WRITE_MEMORY_HANDLE: {
			struct nvkvm_req_write_memory_handle  req  = {0};
			struct nvkvm_resp_write_memory_handle resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			void *db = (req.size > 0) ?
				   slot_blob(nv, req.shm_slot, req.size) : NULL; /* C-1 */
			nvkvm_req_write_memory_handle(nv, &req, &resp, db);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_write_memory_handle r; } wout;
			wout.h = hdr; wout.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &wout, sizeof(wout));
			virtqueue_push(vq, elem, sizeof(wout));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

		case NVKVM_REQ_READ_MEMORY_HANDLE: {
			struct nvkvm_req_read_memory_handle  req  = {0};
			struct nvkvm_resp_read_memory_handle resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			void *db = (req.size > 0) ?
				   slot_blob(nv, req.shm_slot, req.size) : NULL; /* C-1 */
			nvkvm_req_read_memory_handle(nv, &req, &resp, db);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_read_memory_handle r; } rout;
			rout.h = hdr; rout.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &rout, sizeof(rout));
			virtqueue_push(vq, elem, sizeof(rout));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

		case NVKVM_REQ_READ_HOST_FILE: {
			struct nvkvm_req_read_host_file  req  = {0};
			struct nvkvm_resp_read_host_file resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			void *db = slot_blob(nv, req.shm_slot, req.max_len); /* C-1 */
			nvkvm_req_read_host_file(nv, &req, &resp, db);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_read_host_file r; } hout;
			hout.h = hdr; hout.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &hout, sizeof(hout));
			virtqueue_push(vq, elem, sizeof(hout));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

		case NVKVM_REQ_REALIZE_UVM_MAPPING: {
			struct nvkvm_req_realize_uvm_mapping  req  = {0};
			struct nvkvm_resp_realize_uvm_mapping resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			/* C-1: bound state slot to the whole slot (handler validates
			 * the snapshot size internally) and intent to intent_size. */
			void *sb = slot_blob(nv, req.state_shm_slot, nv->slot_size);
			void *ib = (req.intent_size > 0) ?
				   slot_blob(nv, req.intent_shm_slot, req.intent_size) : NULL;
			nvkvm_req_realize_uvm_mapping(nv, &req, &resp, sb, ib);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_realize_uvm_mapping r; } rout;
			rout.h = hdr; rout.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &rout, sizeof(rout));
			virtqueue_push(vq, elem, sizeof(rout));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

#undef ISOLATE_REQ

		default:
			error_report("nvkvm: unknown request type %u",
				     le32_to_cpu(hdr.type));
			virtqueue_push(vq, elem, 0);
			break;
		}
		g_free(elem);
	}
}

/* ── Virtio config space ─────────────────────────────────────────────────── */

static void nvkvm_get_config(VirtIODevice *vdev, uint8_t *config)
{
	VirtIONvgpu *nv = VIRTIO_NVGPU(vdev);
	/*
	 * #55: resolve the sparse window to the firmware-assigned reservation-BAR
	 * GPA (and install the raw memslot there) the first time the guest reads
	 * config — which happens during the guest's nvkvm probe, after PCI
	 * enumeration has programmed the BAR.  Advertise that base/len as the
	 * window the guest validates returned GPAs against.  Falls back to the
	 * fixed base if there's no BAR (nvkvm_sparse_ensure handles both).
	 */
	uint64_t base = nvkvm_sparse_ensure(nv);
	if (base) {
		nv->config_space.mmap_win_gpa = cpu_to_le64(base);
		nv->config_space.mmap_win_len = cpu_to_le64((uint64_t)nv->sparse_size);
	}
	memcpy(config, &nv->config_space, sizeof(nv->config_space));
}

static uint64_t nvkvm_get_features(VirtIODevice *vdev, uint64_t features,
				   Error **errp)
{
	/*
	 * Disable VIRTIO_RING_F_EVENT_IDX.  With async out-of-order completions
	 * (IOCTL_ON_ISOLATE offloaded to the thread pool), EVENT_IDX interrupt
	 * suppression can strand the last used-ring entry: the guest's TX
	 * callback (nvkvm_tx_done_callback) drains with virtqueue_get_buf but
	 * does not use the disable_cb/enable_cb re-check pattern, so a buffer
	 * pushed in the suppression window never raises an IRQ and the guest
	 * hangs forever in wait_for_completion.  Without EVENT_IDX the device
	 * notifies on every push (unless the guest explicitly set NO_INTERRUPT,
	 * which it does not), so no completion can be lost.
	 */
	features &= ~(1ULL << VIRTIO_RING_F_EVENT_IDX);
	return features;
}

/* ── Device realize / unrealize ──────────────────────────────────────────── */

/*
 * The supervisor thread and install_mapping RPC handlers need a way to
 * reach the device. We only ever realize one virtio-nvgpu device per QEMU
 * process; record the pointer at realize time.
 */
static VirtIONvgpu *g_nvkvm_device;

VirtIONvgpu *nvkvm_get_global_device(void)
{
	return g_nvkvm_device;
}

/* Verbose per-operation tracing gate (nvkvm_log.h). Off unless NVKVM_DEBUG
 * is set in the environment; errors and security DENY logs are unconditional. */
int nvkvm_debug_enabled;

/*
 * Read-only QOM property backing "isolate-mode-active": the isolation mode
 * that is actually in force after `auto` has probed the ladder.  Readable with
 *     (qemu) qom-get /machine/peripheral/<id> isolate-mode-active
 * or over QMP, so a monitoring check can assert the boundary it expects
 * instead of trusting that the configured mode is the one that happened.
 */
static char *nvkvm_get_isolate_mode_active(Object *obj, Error **errp)
{
	VirtIONvgpu *nv = VIRTIO_NVGPU(obj);
	return g_strdup(nv->isolate_mode_active ? nv->isolate_mode_active
					        : "unknown");
}

static void virtio_nvgpu_device_realize(DeviceState *dev, Error **errp)
{
	VirtIODevice *vdev = VIRTIO_DEVICE(dev);
	VirtIONvgpu  *nv   = VIRTIO_NVGPU(dev);

	nvkvm_debug_enabled = (getenv("NVKVM_DEBUG") != NULL);

	g_nvkvm_device = nv;

#if !NVKVM_QEMU_GRAPHICS
	/* Compute-only QEMU build (NVKVM_QEMU_GRAPHICS=0): the graphics/display
	 * code (DRM render node, NVKMS modeset, present/EGL path) is compiled out,
	 * so force the runtime gate off regardless of the graphics= property. This
	 * is the QEMU-side twin of the guest module's NVKVM_GRAPHICS=0 build. */
	nv->graphics = false;
#endif

	/*
	 * Find QEMU's KVM VM fd by scanning our own /proc/self/fd for the
	 * "anon_inode:kvm-vm" entry, so the nvidia/UVM mmap path in
	 * nvkvm_isolate_handlers.c can call KVM_SET_USER_MEMORY_REGION
	 * on it.  (We can't include sysemu/kvm_int.h here — kvm_int.h is
	 * target-only.)
	 */
	{
		DIR *d = opendir("/proc/self/fd");
		if (d) {
			struct dirent *de;
			while ((de = readdir(d))) {
				if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
				char path[300], link[64];
				snprintf(path, sizeof(path), "/proc/self/fd/%s", de->d_name);
				ssize_t n = readlink(path, link, sizeof(link) - 1);
				if (n <= 0) continue;
				link[n] = 0;
				if (strcmp(link, "anon_inode:kvm-vm") == 0) {
					int fd = atoi(de->d_name);
					nvkvm_set_kvm_vm_fd(fd);
					NVKVM_DBG(
						"nvkvm: registered KVM vm fd %d\n", fd);
					break;
				}
			}
			closedir(d);
		}
	}
	int fd;

	virtio_init(vdev, VIRTIO_ID_NVGPU, sizeof(struct nvkvm_virtio_config));

	nv->vq_tx  = virtio_add_queue(vdev, 256, nvkvm_tx_handler);
	nv->vq_rx  = virtio_add_queue(vdev, 256, NULL);
	nv->vq_evt = virtio_add_queue(vdev, 64,  NULL);

	/* Probe host NVIDIA driver version */
	fd = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		error_setg(errp,
			   "nvkvm: cannot open /dev/nvidiactl on host: %s",
			   strerror(errno));
		return;
	}
	{
		/* '2' == NV_RM_API_VERSION_CMD_QUERY (driver returns its
		 * version without enforcing a string compare). cmd=0 is
		 * STRICT — open-source nvidia.ko enforces it. */
		struct nv_ioctl_rm_api_version ver = {.cmd = '2'};
		ioctl(fd, /* NV_ESC_CHECK_VERSION_STR */ _IOWR('F',
		      NV_ESC_CHECK_VERSION_STR, struct nv_ioctl_rm_api_version),
		      &ver);
		memcpy(nv->driver_version, ver.version_string,
		       sizeof(nv->driver_version));
	}
	close(fd);

	/* #81: select the per-version ABI profile from the host driver version.
	 * The guest independently selects the same profile from the version
	 * string we forward; QEMU also stamps the profile id into each
	 * ISOLATE_CMD_IOCTL so the stub uses matching offsets. */
	nv->abi = nvkvm_abi_for_version(nv->driver_version);
	fprintf(stderr, "nvkvm: host driver %s → ABI profile %u\n",
		nv->driver_version, nv->abi ? nv->abi->id : 0);

	/* Allocate shared memory region */
	nv->slot_size = NVKVM_SHM_SLOT_DEFAULT_SIZE;
	nv->shm_size  = (size_t)NVKVM_SHM_NSLOTS * nv->slot_size;
	nv->shm_base  = mmap(NULL, nv->shm_size,
			     PROT_READ | PROT_WRITE,
			     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (nv->shm_base == MAP_FAILED) {
		error_setg(errp, "nvkvm: failed to allocate shared memory");
		return;
	}

	/* Fill in the control block */
	{
		struct nvkvm_shm_ctrl *ctrl = nv->shm_base;
		ctrl->proto_version = cpu_to_le32(NVKVM_PROTO_VERSION);
		ctrl->slot_size     = cpu_to_le32((uint32_t)nv->slot_size);
		ctrl->nslots        = cpu_to_le32(NVKVM_SHM_NSLOTS);
		memcpy(ctrl->driver_version, nv->driver_version,
		       sizeof(ctrl->driver_version));
	}

	pthread_mutex_init(&nv->sessions_lock, NULL);
	pthread_mutex_init(&nv->mmap_win_lock, NULL);
	pthread_mutex_init(&nv->client_allow_lock, NULL);
	nv->client_allow_n = 0;
	TAILQ_INIT(&nv->sessions);

	/* Initialize isolate/handle managers */
	nvkvm_handle_table_init(&nv->handles);
	nvkvm_isolate_table_init(&nv->isolates, &nv->handles);
	/* #81: stamp every forwarded IOCTL with the host driver's ABI id so the
	 * stub uses matching version-variant offsets. */
	nv->isolates.abi_profile = nv->abi ? nv->abi->id : NVKVM_ABI_570;

	/*
	 * Validate the isolate sandbox configuration ONCE, here, before the guest
	 * exists.  UID-separation mode needs CAP_SETUID/CAP_SETGID; finding that
	 * out at the first setresuid() — in a forked child, behind the guest's
	 * first GPU ioctl — shows up as an opaque spawn failure with forwarding
	 * silently off.  Fail the realize instead, and never fall back to a
	 * different mode than the one that was asked for.
	 */
	{
		char cfg_err[256], cfg_desc[256];
		if (nvkvm_isolate_cfg_check(&nv->isolates, cfg_err,
					    sizeof(cfg_err)) != 0) {
			error_setg(errp, "nvkvm: %s", cfg_err);
			return;
		}
		nvkvm_isolate_cfg_describe(&nv->isolates, cfg_desc,
					   sizeof(cfg_desc));

		/*
		 * Anything weaker than namespace mode is reported at WARNING
		 * level, at every start — not as a debug line. Someone reading
		 * their logs must be able to discover they are on a weaker
		 * boundary without going looking for it, so the report names
		 * the selected rung, the stronger rungs that were attempted and
		 * why each was rejected.
		 */
		const char *rep = nvkvm_isolate_cfg_report(&nv->isolates);
		if (nvkvm_isolate_cfg_is_unconfined(&nv->isolates)) {
			/* Unmissable, at every start.  This mode required an
			 * explicit acknowledgement to reach, and it is still
			 * worth shouting about on the way past. */
			warn_report("nvkvm: ****************************************");
			warn_report("nvkvm: * ISOLATE CONFINEMENT IS COMPLETELY OFF *");
			warn_report("nvkvm: ****************************************");
			warn_report("nvkvm: isolate mode 'none': no namespaces, no "
				    "uid separation, and the stub's seccomp "
				    "filter is NOT installed. A compromised "
				    "isolate has the full privileges of this "
				    "QEMU process. Debugging only.");
		} else if (nvkvm_isolate_cfg_is_degraded(&nv->isolates)) {
			warn_report("nvkvm: %s", cfg_desc);
			if (rep && *rep)
				warn_report("nvkvm: %s", rep);
			warn_report("nvkvm: this is WEAKER than the default "
				    "'namespace' isolation — see "
				    "docs/internal/isolate-model.md");
		} else {
			info_report("nvkvm: %s", cfg_desc);
			if (rep && *rep)
				info_report("nvkvm: %s", rep);
		}

		/* Queryable after startup, not just logged. */
		g_free(nv->isolate_mode_active);
		nv->isolate_mode_active = g_strdup(cfg_desc);
		object_property_add_str(OBJECT(dev), "isolate-mode-active",
					nvkvm_get_isolate_mode_active, NULL);
	}

	/* #66 admin subdevice (lazy; for GET_PID_INFO per-process VRAM) */
	pthread_mutex_init(&nv->admin_lock, NULL);
	nv->admin_ctl_fd = -1;
	nv->admin_gpu_fd = -1;
	nv->admin_state  = 0;

	/*
	 * Resolve the GPA window layout for THIS VM before registering anything.
	 *
	 * These bases used to be compile-time constants at 1 TB / 1.5 TB / 2 TB,
	 * which need 41-42 physical address bits.  Consumer mobile Intel has 39
	 * (512 GiB), and KVM rejects the first memslot outright, killing QEMU
	 * before the guest boots.  nvkvm_gpa_layout_compute() derives the base
	 * from the narrower of the host's and the guest's MAXPHYADDR and places
	 * the block against the top of that space, above guest RAM.
	 */
	{
		char lerr[768];

		if (!nvkvm_gpa_layout_compute(&nv->gpa, lerr, sizeof(lerr))) {
			error_setg(errp, "%s", lerr);
			return;
		}

		/* Auditable: the decision, its inputs and its result, in one
		 * line each.  This is the record that says why the window
		 * landed where it did on this particular host. */
		info_report("nvkvm: GPA width: host MAXPHYADDR %u bits, guest "
			    "MAXPHYADDR %u bits -> using %u bits (limit 0x%llx, "
			    "%llu GiB)",
			    nv->gpa.host_bits, nv->gpa.guest_bits, nv->gpa.bits,
			    (unsigned long long)nv->gpa.limit,
			    (unsigned long long)(nv->gpa.limit >> 30));
		info_report("nvkvm: GPA windows: guest RAM top ~0x%llx (%llu GiB), "
			    "floor 0x%llx -> block base 0x%llx size %llu GiB "
			    "[shm 0x%llx, mmap 0x%llx +%llu GiB, sparse 0x%llx "
			    "+%llu GiB]%s",
			    (unsigned long long)nv->gpa.ram_top,
			    (unsigned long long)(nv->gpa.ram_top >> 30),
			    (unsigned long long)nv->gpa.floor,
			    (unsigned long long)nv->gpa.block_base,
			    (unsigned long long)(nv->gpa.block_size >> 30),
			    (unsigned long long)nv->gpa.shm_base,
			    (unsigned long long)nv->gpa.mmap_base,
			    (unsigned long long)(nv->gpa.mmap_size >> 30),
			    (unsigned long long)nv->gpa.sparse_base,
			    (unsigned long long)(nv->gpa.sparse_size >> 30),
			    nv->gpa.shrunk ? " (SPARSE WINDOW SHRUNK)" : "");
		if (nv->gpa.shrunk)
			warn_report("nvkvm: the sparse GPA window was reduced to "
				    "%llu GiB (from %llu GiB) to fit this VM's "
				    "%u-bit address space; a single cuCtxCreate "
				    "issues >1500 mmaps, so very large GPU "
				    "working sets may exhaust it",
				    (unsigned long long)(nv->gpa.sparse_size >> 30),
				    (unsigned long long)(NVKVM_SPARSE_GPA_SIZE >> 30),
				    nv->gpa.bits);
	}

	/* Register shared memory as a KVM memory region at gpa.shm_base.
	 * The guest reads shm_base/shm_len from the virtio config space and maps
	 * this region to access ioctl parameter slots with zero virtio copies. */
	memory_region_init_ram_ptr(&nv->shm_mr, OBJECT(dev), "virtio-nvgpu-shm",
				   nv->shm_size, nv->shm_base);
	memory_region_add_subregion(get_system_memory(),
				    nv->gpa.shm_base, &nv->shm_mr);
	nv->shm_mr_registered = true;
	nv->shm_gpa = nv->gpa.shm_base;

	/* Mmap window: 16 GB GPA range for GPU memory mappings */
	nv->mmap_win_gpa  = nv->gpa.mmap_base;
	nv->mmap_win_size = nv->gpa.mmap_size;
	nv->mmap_win_cur  = 0;

	/* Sparse GPA window — used by the memory-ioctl path for guest-VA
	 * regions that don't yet have backing.  Lazy via MAP_NORESERVE +
	 * a single big KVM region. */
	nv->sparse_kvm_slot = -1;
	/*
	 * Belt-and-braces overlap guard.  nvkvm_gpa_layout_compute() already
	 * refuses to place the block below its floor (guest RAM top + room for
	 * the reservation BAR), so this should be unreachable — assert it
	 * anyway rather than silently corrupting guest RAM if the floor
	 * calculation ever drifts from the machine's real layout.
	 */
	if (nv->gpa.block_base < nv->gpa.ram_top) {
		error_setg(errp,
			"nvkvm: GPA window block at 0x%llx would overlap guest "
			"RAM (top ~0x%llx, ram_size 0x%" PRIx64 "); reduce guest "
			"RAM or run on a host with more physical address bits",
			(unsigned long long)nv->gpa.block_base,
			(unsigned long long)nv->gpa.ram_top,
			(uint64_t)(current_machine ? current_machine->ram_size : 0));
		return;
	}
	if (nvkvm_sparse_init(nv) < 0)
		fprintf(stderr, "nvkvm: sparse window unavailable; "
			"memory-ioctl path will degrade\n");

	/* Populate virtio config space so the guest can locate both regions */
	nv->config_space.shm_base     = cpu_to_le64(nv->gpa.shm_base);
	nv->config_space.shm_len      = cpu_to_le64(nv->shm_size);
	/* The guest validates every host-returned GPA against this window.
	 * Two GPA windows are in play:
	 *   - sparse window (128 GiB @ 2 TB): single pre-installed memslot,
	 *     backs all the bulk BAR/sysmem mmaps as MAP_FIXED slices.
	 *   - mmap_win (16 GiB @ 1.5 TB): legacy per-mmap memslots, still used
	 *     by /dev/nvidia-uvm mappings (which cannot be MAP_FIXED into the
	 *     sparse window — the UVM kernel requires vm_start==pgoff<<SHIFT).
	 * Advertise one window spanning both [mmap_win_base, sparse_end).  The
	 * guest only ever validates GPAs QEMU actually returns (always inside
	 * one of the two sub-windows), so accepting the superset — including the
	 * unbacked gap between them — is safe. */
	nv->config_space.mmap_win_gpa = cpu_to_le64(nv->gpa.mmap_base);
	nv->config_space.mmap_win_len = cpu_to_le64(
		(nv->gpa.sparse_base + nv->gpa.sparse_size) - nv->gpa.mmap_base);

	/* Advertise the graphics capability to the guest (QEMU dictates). */
	nv->config_space.flags = cpu_to_le64(
		nv->graphics ? NVKVM_CONFIG_F_GRAPHICS : 0);
	if (!nv->graphics)
		info_report("nvkvm: graphics disabled (compute-only): DRM render "
			    "node + NVKMS modeset device + ioctls refused");

	/*
	 * #102: register a QemuConsole so the guest's GPU-composited scanout can
	 * be shown in a live QEMU display window.  Graphics-gated; no-op in the
	 * compute-only build.  The console is host-private and strictly a sink —
	 * frames flow guest→host only.
	 */
	if (nv->graphics)
		nvkvm_present_console_init(dev, nv);
}

static void virtio_nvgpu_device_unrealize(DeviceState *dev)
{
	VirtIONvgpu *nv = VIRTIO_NVGPU(dev);

	/* #102: close the present console before tearing down the device. */
	nvkvm_present_console_fini(nv);

	/* Tear down isolates and handles before shared memory */
	nvkvm_isolate_table_fini(&nv->isolates);
	nvkvm_handle_table_fini(&nv->handles);

	/*
	 * H-9: tear the sparse window down, which is also the ONLY thing that
	 * purges this device's entries from the process-wide GPA quarantine.
	 * Unrealize used to skip it entirely, so freed extents stayed queued
	 * holding a VirtIONvgpu * that is about to be freed — and the queue
	 * drains lazily, from whichever device frees next, straight into
	 * nvkvm_sparse_gpa_release() dereferencing nv->sparse_vmm_va.  The
	 * quarantine's contract is that the device outlives its entries; this
	 * is the call that makes that true.  Ordered after the isolate and
	 * handle teardown, since those are what can still be freeing extents.
	 */
	nvkvm_sparse_fini(nv);

	g_free(nv->isolate_mode_active);
	nv->isolate_mode_active = NULL;

	/* #66 admin subdevice: closing the fds frees its RM objects. */
	if (nv->admin_ctl_fd >= 0) close(nv->admin_ctl_fd);
	if (nv->admin_gpu_fd >= 0) close(nv->admin_gpu_fd);
	nv->admin_ctl_fd = nv->admin_gpu_fd = -1;
	nv->admin_state = -1;

	if (nv->shm_mr_registered) {
		memory_region_del_subregion(get_system_memory(), &nv->shm_mr);
		nv->shm_mr_registered = false;
	}

	if (nv->shm_base && nv->shm_base != MAP_FAILED)
		munmap(nv->shm_base, nv->shm_size);

	virtio_cleanup(VIRTIO_DEVICE(dev));
}

/* ── nvkvm-gpu: emulated NVIDIA PCI identity device ──────────────────────────
 *
 * The NVIDIA Vulkan/EGL userspace enumerates the GPU through the DRM render
 * node and, before opening it, walks renderD128 -> device -> parent PCI device
 * and reads its vendor/device/subsystem IDs, requiring vendor 0x10DE.  The
 * virtio-nvgpu transport must keep vendor 0x1AF4 (or the guest virtio-pci
 * driver won't bind), so we expose a SEPARATE, identity-only PCI device that
 * the guest's nvkvm-drm driver uses as the render node's sysfs parent.
 *
 * IDENTITY ONLY — no BARs, no MMIO, no DMA.  All real GPU I/O (compute and the
 * DRM render path alike) continues through the virtio device's GPA-window
 * mmap forwarding; this device never touches the data path.  IDs are read from
 * the host GPU's sysfs so they track the actual hardware across driver/GPU
 * changes (falls back to a generic NVIDIA id if the host can't be read). */

#define TYPE_NVKVM_GPU "nvkvm-gpu"

typedef struct NvkvmGpu NvkvmGpu;

struct NvkvmGpu {
	PCIDevice    parent_obj;

	/* No state at all, deliberately: this device IS its config-space identity
	 * and nothing more.  It registers no BAR, so the guest's PCI core records
	 * no regions for it and there is nothing for a guest driver to map.
	 *
	 * A `fake-bars` property once lived here which advertised the host GPU's
	 * BAR *geometry* (sizes/flags copied from the host's /sys/.../resource) to
	 * get NVIDIA's Xorg DDX past its device-validation check.  It was removed:
	 * it had no consumers -- CUDA, Vulkan, OpenGL and vLLM all work without it
	 * -- and the DDX path it unblocked is blocked further on regardless, at
	 * NVKMS, which would need a virtual NVKMS to answer for nvkvm's own head
	 * rather than the host's connectors.  Keeping a dormant switch whose only
	 * job is to make this device LOOK like it has BARs is a liability on a
	 * boundary the whole project sells on.  The measurements it produced, and
	 * the sha to revert if someone builds that virtual NVKMS, are in
	 * docs/internal/mint-guest-desktop.md. */
};
DECLARE_INSTANCE_CHECKER(NvkvmGpu, NVKVM_GPU, TYPE_NVKVM_GPU)

/* Read a hex value (e.g. "0x10de\n") from a host sysfs PCI attribute.
 * Returns def on any failure. */
static unsigned nvkvm_sysfs_hex(const char *bdf, const char *attr, unsigned def)
{
	char path[128];
	unsigned val;
	FILE *f;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", bdf, attr);
	f = fopen(path, "r");
	if (!f)
		return def;
	if (fscanf(f, "%x", &val) != 1)
		val = def;
	fclose(f);
	return val;
}

/* First host GPU BDF (scan /proc/driver/nvidia/gpus/); NULL if none. */
static const char *nvkvm_first_host_gpu_bdf(char *buf, size_t buflen)
{
	DIR *d = opendir("/proc/driver/nvidia/gpus");
	struct dirent *de;
	const char *out = NULL;

	if (!d)
		return NULL;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (strlen(de->d_name) < buflen) {
			memcpy(buf, de->d_name, strlen(de->d_name) + 1);
			out = buf;
		}
		break;
	}
	closedir(d);
	return out;
}

static void nvkvm_gpu_realize(PCIDevice *pdev, Error **errp)
{
	char bdf[32];
	const char *b = nvkvm_first_host_gpu_bdf(bdf, sizeof(bdf));
	uint16_t vendor = 0x10de, device = 0x2504, svid = 0x10de, sdid = 0x0000;
	uint8_t  revision = 0xa1;
	(void)errp;

	if (b) {
		vendor   = nvkvm_sysfs_hex(b, "vendor", 0x10de);
		device   = nvkvm_sysfs_hex(b, "device", 0x2504);
		svid     = nvkvm_sysfs_hex(b, "subsystem_vendor", vendor);
		sdid     = nvkvm_sysfs_hex(b, "subsystem_device", 0x0000);
		revision = nvkvm_sysfs_hex(b, "revision", 0xa1);
	}

	pci_config_set_vendor_id(pdev->config, vendor);
	pci_config_set_device_id(pdev->config, device);
	pci_config_set_revision(pdev->config, revision);
	/* Match the host GPU's PCI class exactly (VGA 0x0300 for a GeForce) — the
	 * ICD compares the device's class against what it expects for an NVIDIA
	 * GPU.  Secondary VGA with no ROM is harmless (SeaBIOS skips it). */
	pci_config_set_class(pdev->config, PCI_CLASS_DISPLAY_VGA);
	pci_set_word(pdev->config + PCI_SUBSYSTEM_VENDOR_ID, svid);
	pci_set_word(pdev->config + PCI_SUBSYSTEM_ID, sdid);

	/* No pci_register_bar() call here, and there must not be one: a region
	 * registered on this device is a region the guest can map. */
}

static void nvkvm_gpu_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass    *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k  = PCI_DEVICE_CLASS(klass);
	(void)data;

	k->realize   = nvkvm_gpu_realize;
	k->vendor_id = 0x10de;                 /* overridden from host in realize */
	k->device_id = 0x2504;
	k->revision  = 0xa1;
	k->class_id  = PCI_CLASS_DISPLAY_VGA;
	dc->desc     = "nvkvm emulated NVIDIA GPU PCI identity (no BARs, no MMIO, no DMA)";
	set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo nvkvm_gpu_info = {
	.name          = TYPE_NVKVM_GPU,
	.parent        = TYPE_PCI_DEVICE,
	.instance_size = sizeof(NvkvmGpu),
	.class_init    = nvkvm_gpu_class_init,
	.interfaces    = (InterfaceInfo[]) {
		{ INTERFACE_CONVENTIONAL_PCI_DEVICE },
		{ },
	},
};

/* ── QEMU type registration ──────────────────────────────────────────────── */

static const TypeInfo virtio_nvgpu_info = {
	.name          = TYPE_VIRTIO_NVGPU,
	.parent        = TYPE_VIRTIO_DEVICE,
	.instance_size = sizeof(VirtIONvgpu),
	.class_init    = NULL,     /* filled below */
};

/* const, and NOT NULL-terminated: upstream 5fcabe628b removed
 * DEFINE_PROP_END_OF_LIST() and device_class_set_props() now derives the count
 * with ARRAY_SIZE and build-asserts that the LAST entry's name is non-NULL, so
 * a trailing terminator is a compile error rather than a no-op. */
static const Property virtio_nvgpu_properties[] = {
	/* graphics=on|off (default on): expose+forward the DRM render node and
	 * NVKMS modeset device. off → compute-only VM, smaller attack surface. */
	DEFINE_PROP_BOOL("graphics", VirtIONvgpu, graphics, true),
};

static void virtio_nvgpu_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

	device_class_set_props(dc, virtio_nvgpu_properties);

	vdc->realize     = virtio_nvgpu_device_realize;
	vdc->unrealize   = virtio_nvgpu_device_unrealize;
	vdc->get_features = nvkvm_get_features;
	vdc->get_config   = nvkvm_get_config;
}

static void virtio_nvgpu_register_types(void)
{
	TypeInfo info = virtio_nvgpu_info;
	info.class_init = virtio_nvgpu_class_init;
	type_register_static(&info);
	type_register_static(&nvkvm_gpu_info);
}

type_init(virtio_nvgpu_register_types);
