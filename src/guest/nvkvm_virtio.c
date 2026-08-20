// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_virtio.c — virtio transport frontend for the nvkvm guest module
 *
 * Manages:
 *  - Virtio device probe / VQ setup
 *  - Shared memory slot allocator
 *  - Synchronous request/response round-trips over VQ_TX / VQ_RX
 *  - Async event delivery from VQ_EVT to per-FD poll queues
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/scatterlist.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/bitmap.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/poll.h>          /* POLLIN for the #127 os-event poll arm */

#include "nvkvm.h"

/* ── Slot allocator ───────────────────────────────────────────────────────── */

int nvkvm_slot_alloc(struct nvkvm_state *state)
{
	unsigned long flags;
	int slot;

	spin_lock_irqsave(&state->slot_lock, flags);
	slot = find_next_zero_bit(state->slot_bitmap, NVKVM_SHM_NSLOTS, 1);
	if (slot >= NVKVM_SHM_NSLOTS)
		slot = -ENOMEM;
	else
		set_bit(slot, state->slot_bitmap);
	spin_unlock_irqrestore(&state->slot_lock, flags);
	return slot;
}

void nvkvm_slot_free(struct nvkvm_state *state, int slot)
{
	unsigned long flags;
	spin_lock_irqsave(&state->slot_lock, flags);
	clear_bit(slot, state->slot_bitmap);
	spin_unlock_irqrestore(&state->slot_lock, flags);
}

void *nvkvm_slot_addr(struct nvkvm_state *state, int slot)
{
	if (!state->shm_base || slot < 0 || slot >= NVKVM_SHM_NSLOTS)
		return NULL;
	return (void __force *)state->shm_base + (size_t)slot * state->slot_size;
}

/* ── txn_id allocator ──────────────────────────────────────────────────── */

/*
 * Allocate a fresh u32 txn_id whose corresponding bit in the in-flight
 * bitmap is currently clear. The next_txn_id seed advances monotonically;
 * the bitmap is the safety net that prevents an unlikely (but theoretically
 * possible) reuse-while-outstanding collision after wraparound.
 *
 * Returns the txn_id on success, or 0 if every slot is currently in use
 * (caller should error out — this means NVKVM_MAX_INFLIGHT is too small).
 */
static __u32 nvkvm_txn_id_alloc(struct nvkvm_state *state)
{
	unsigned long flags;
	__u32 seed = (__u32)atomic_inc_return(&state->next_txn_id);
	for (unsigned tries = 0; tries < NVKVM_MAX_INFLIGHT; tries++) {
		/* Map seed → bitmap slot. Avoid zero (sentinel).
		 * Use the low log2(NVKVM_MAX_INFLIGHT) bits as the slot. */
		unsigned slot = (seed + tries) % NVKVM_MAX_INFLIGHT;
		if (slot == 0) continue;
		spin_lock_irqsave(&state->inflight_lock, flags);
		if (!test_and_set_bit(slot, state->txn_inflight_bm)) {
			spin_unlock_irqrestore(&state->inflight_lock, flags);
			/* Combine slot with epoch bits from seed so consecutive
			 * uses of the same slot get distinct full txn_ids. */
			__u32 epoch = (seed / NVKVM_MAX_INFLIGHT) & 0xFFFFF;
			return (epoch << 12) | (slot & 0xFFF);
		}
		spin_unlock_irqrestore(&state->inflight_lock, flags);
	}
	return 0;
}

static void nvkvm_txn_id_free(struct nvkvm_state *state, __u32 txn_id)
{
	unsigned long flags;
	unsigned slot = txn_id & 0xFFF;
	if (slot == 0) return;
	spin_lock_irqsave(&state->inflight_lock, flags);
	clear_bit(slot, state->txn_inflight_bm);
	spin_unlock_irqrestore(&state->inflight_lock, flags);
}

/* ── In-flight request helpers ───────────────────────────────────────────── */

/* Allocate inflight + reserve a txn_id bitmap slot in one shot.
 * Returns NULL on memory failure OR on bitmap exhaustion.
 * Caller must release with inflight_free() (never plain kfree). */
static struct nvkvm_inflight *inflight_alloc(struct nvkvm_state *state)
{
	__u32 txn_id = nvkvm_txn_id_alloc(state);
	if (txn_id == 0)
		return NULL;
	struct nvkvm_inflight *inf = kzalloc(sizeof(*inf), GFP_KERNEL);
	if (!inf) {
		nvkvm_txn_id_free(state, txn_id);
		return NULL;
	}
	inf->txn_id = txn_id;
	init_completion(&inf->done);
	return inf;
}

/* Backward-compat shim still in use during the gradual migration. */
static struct nvkvm_inflight *inflight_alloc_legacy(__u32 txn_id)
{
	struct nvkvm_inflight *inf = kzalloc(sizeof(*inf), GFP_KERNEL);
	if (!inf)
		return NULL;
	inf->txn_id = txn_id;
	init_completion(&inf->done);
	return inf;
}

/* Free the inflight record AND release the txn_id slot. Always use this
 * instead of kfree() for any inflight obtained from inflight_alloc(). */
static void inflight_free(struct nvkvm_state *state, struct nvkvm_inflight *inf)
{
	if (!inf) return;
	nvkvm_txn_id_free(state, inf->txn_id);
	kfree(inf);
}

static void inflight_enqueue(struct nvkvm_state *state,
			     struct nvkvm_inflight *inf)
{
	unsigned long flags;
	spin_lock_irqsave(&state->inflight_lock, flags);
	list_add_tail(&inf->list, &state->inflight_list);
	spin_unlock_irqrestore(&state->inflight_lock, flags);
}


static void inflight_dequeue(struct nvkvm_state *state,
			     struct nvkvm_inflight *inf)
{
	unsigned long flags;
	spin_lock_irqsave(&state->inflight_lock, flags);
	list_del(&inf->list);
	spin_unlock_irqrestore(&state->inflight_lock, flags);
}

/*
 * Maximum size of a response message (nvkvm_hdr + largest resp struct).
 * nvkvm_resp_list_nvidia_devices is the largest: 8 + 32*16 = 520 bytes.
 * Round up to 512 to cover all current responses; LIST_NVIDIA_DEVICES uses
 * a dedicated larger buffer allocated at probe time.
 */
#define NVKVM_RESP_BUF_SIZE  512

/* ── VQ_TX completion callback — called from softirq context ─────────────── */
/*
 * QEMU writes the response into the IN sg of the TX element and pushes it
 * back.  The data pointer we passed to virtqueue_add_sgs() is the inflight
 * record, so virtqueue_get_buf() returns inf directly.
 */
static void nvkvm_tx_done_callback(struct virtqueue *vq)
{
	struct nvkvm_state *state = vq->vdev->priv;
	struct nvkvm_inflight *inf;
	unsigned int len;
	unsigned long flags;

	for (;;) {
		struct nvkvm_hdr *hdr;

		/* Serialize ring access against concurrent submitters. */
		spin_lock_irqsave(&state->vq_tx_lock, flags);
		inf = virtqueue_get_buf(vq, &len);
		spin_unlock_irqrestore(&state->vq_tx_lock, flags);
		if (!inf)
			break;
		hdr = inf->resp_buf;

		if (!hdr) {
			pr_warn("nvkvm: tx_done: null resp_buf\n");
			complete(&inf->done);
			continue;
		}

		/* txn_id sanity check: the response must mirror the request's
		 * txn_id. virtqueue ordering already gives us correct demux
		 * via the data pointer, so a mismatch here means QEMU (or a
		 * misbehaving wire) corrupted the round-trip. */
		{
			__u32 got = le32_to_cpu(hdr->txn_id);
			if (got != inf->txn_id)
				pr_warn_ratelimited(
				    "nvkvm: txn_id mismatch on reply: sent 0x%x got 0x%x type=0x%x\n",
				    inf->txn_id, got,
				    le32_to_cpu(hdr->type));
		}

		switch (le32_to_cpu(hdr->type)) {
		case NVKVM_REQ_OPEN_NVIDIA_HANDLE: {
			struct nvkvm_resp_open_nvidia_handle *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			inf->retval = le32_to_cpu(resp->handle_id);
			break;
		}
		case NVKVM_REQ_OPEN_MEMORY_HANDLE: {
			struct nvkvm_resp_open_memory_handle *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			inf->retval = le32_to_cpu(resp->handle_id);
			break;
		}
		case NVKVM_REQ_XISO_IMPORT: {
			struct nvkvm_resp_xiso_import *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			inf->retval = le32_to_cpu(resp->gem_handle);
			break;
		}
		case NVKVM_REQ_CLOSE_HANDLE: {
			struct nvkvm_resp_close_handle *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_CREATE_ISOLATE: {
			struct nvkvm_resp_create_isolate *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			inf->retval = le32_to_cpu(resp->isolate_id);
			break;
		}
		case NVKVM_REQ_KILL_ISOLATE: {
			struct nvkvm_resp_kill_isolate *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_SETUP_RING: {
			struct nvkvm_resp_setup_ring *resp = (void *)(hdr + 1);
			inf->status   = le32_to_cpu(resp->status);
			inf->retval   = le64_to_cpu(resp->ring_gpa);   /* GPA       */
			inf->nvstatus = le32_to_cpu(resp->ring_bytes); /* ring_bytes */
			break;
		}
		case NVKVM_REQ_ENTER_LOOP: {
			struct nvkvm_resp_enter_loop *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			inf->retval = le64_to_cpu(resp->head);   /* last_processed */
			break;
		}
		case NVKVM_REQ_PRESENT: {
			struct nvkvm_resp_present *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_COPY_HANDLE_TO_ISOLATE: {
			struct nvkvm_resp_copy_handle_to_isolate *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_CLOSE_HANDLE_ON_ISOLATE: {
			struct nvkvm_resp_close_handle_on_isolate *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_IOCTL_ON_ISOLATE: {
			struct nvkvm_resp_ioctl_on_isolate *resp = (void *)(hdr + 1);
			inf->retval     = le64_to_cpu(resp->retval);
			inf->status     = le32_to_cpu(resp->status);
			inf->nvstatus   = le32_to_cpu(resp->nvstatus);
			inf->fault_addr = le64_to_cpu(resp->fault_addr);
			break;
		}
		case NVKVM_REQ_MMAP_ON_ISOLATE: {
			struct nvkvm_resp_mmap_on_isolate *resp = (void *)(hdr + 1);
			inf->retval = le64_to_cpu(resp->gpa_base);
			inf->status = le32_to_cpu(resp->status);
			/* store mmap_token in nvstatus field (repurposed) */
			inf->nvstatus = le32_to_cpu(resp->mmap_token);
			break;
		}
		case NVKVM_REQ_MUNMAP_ON_ISOLATE: {
			struct nvkvm_resp_munmap_on_isolate *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_WRITE_MEMORY_HANDLE: {
			struct nvkvm_resp_write_memory_handle *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_READ_MEMORY_HANDLE: {
			struct nvkvm_resp_read_memory_handle *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_READ_HOST_FILE: {
			struct nvkvm_resp_read_host_file *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			inf->retval = le32_to_cpu(resp->nbytes);
			break;
		}
		case NVKVM_REQ_REALIZE_UVM_MAPPING: {
			struct nvkvm_resp_realize_uvm_mapping *resp = (void *)(hdr + 1);
			inf->retval     = le64_to_cpu(resp->gpa_base);
			inf->status     = le32_to_cpu(resp->status);
			inf->nvstatus   = le32_to_cpu(resp->rm_status);
			inf->fault_addr = le64_to_cpu(resp->realize_token);
			break;
		}
		default:
			pr_warn("nvkvm: tx_done: unknown type %u\n",
				le32_to_cpu(hdr->type));
			inf->status = EINVAL;
			break;
		}
		complete(&inf->done);
	}
}

/* ── VQ_RX callback — unused (responses come back on VQ_TX) ─────────────── */

static void nvkvm_rx_callback(struct virtqueue *vq)
{
	/* vq_rx has no pre-posted buffers; this callback is never triggered. */
	(void)vq;
}

/* ── VQ_EVT — async poll events from host (#101) ─────────────────────────────
 * QEMU fills a pre-posted nvkvm_evt_poll buffer with (isolate_id, handle_id,
 * events) whenever a forwarded NVIDIA OS-event fires on the host, then returns
 * it on VQ_EVT. We wake the matching guest fd's poll_wq and recycle the buffer.
 * Without this the host completion never reaches the guest poll() promptly and
 * libnvidia-* spins on a ~18 ms poll-timeout-then-recheck (NVENC throughput). */
#define NVKVM_EVT_NBUFS 16

static int nvkvm_evt_post_one(struct nvkvm_state *state,
			      struct nvkvm_evt_poll *evt)
{
	struct scatterlist sg;
	sg_init_one(&sg, evt, sizeof(*evt));
	return virtqueue_add_inbuf(state->vq_evt, &sg, 1, evt, GFP_ATOMIC);
}

/* Pre-post the IN buffers QEMU writes events into. Called once at init. */
static void nvkvm_evt_prime(struct nvkvm_state *state)
{
	int i;
	for (i = 0; i < NVKVM_EVT_NBUFS; i++) {
		struct nvkvm_evt_poll *evt = kzalloc(sizeof(*evt), GFP_KERNEL);
		if (!evt)
			break;
		if (nvkvm_evt_post_one(state, evt) < 0) {
			kfree(evt);
			break;
		}
	}
	virtqueue_kick(state->vq_evt);
}

static void nvkvm_evt_callback(struct virtqueue *vq)
{
	struct nvkvm_state *state = vq->vdev->priv;
	struct nvkvm_evt_poll *evt;
	unsigned int len;
	bool posted = false;

	while ((evt = virtqueue_get_buf(vq, &len)) != NULL) {
		if (len >= sizeof(*evt))
			nvkvm_evt_deliver(le32_to_cpu(evt->isolate_id),
					  le32_to_cpu(evt->handle_id),
					  le32_to_cpu(evt->events));
		/* recycle the buffer back onto VQ_EVT */
		if (nvkvm_evt_post_one(state, evt) < 0)
			kfree(evt);
		else
			posted = true;
	}
	if (posted)
		virtqueue_kick(vq);
}

/* ── Generic synchronous send ─────────────────────────────────────────────── */

/*
 * nvkvm_send_sync — place a request on VQ_TX and block for the response.
 *
 * @req_buf:  buffer holding nvkvm_hdr + request payload
 * @req_len:  total length
 * @inf:      pre-allocated inflight record (txn_id already set)
 *
 * Returns 0 on transport success (inf->status holds the operation result).
 */
int nvkvm_send_sync(struct nvkvm_state *state,
		    void *req_buf, size_t req_len,
		    struct nvkvm_inflight *inf)
{
	struct scatterlist out_sg, in_sg;
	struct scatterlist *sgs[2] = { &out_sg, &in_sg };
	int ret;

	/* Allocate the IN buffer that QEMU will write the response into. */
	inf->resp_buf = kzalloc(NVKVM_RESP_BUF_SIZE, GFP_KERNEL);
	if (!inf->resp_buf)
		return -ENOMEM;

	sg_init_one(&out_sg, req_buf, req_len);
	sg_init_one(&in_sg, inf->resp_buf, NVKVM_RESP_BUF_SIZE);

	inflight_enqueue(state, inf);

	/*
	 * Pass inf as the data cookie so virtqueue_get_buf() in the TX-done
	 * callback returns the inflight record directly.
	 *
	 * vq_tx_lock serializes the ring against concurrent submitters and the
	 * completion callback — virtqueues are not thread-safe.  GFP_ATOMIC
	 * because we hold a spinlock (the buffer is tiny; no indirect descs).
	 */
	{
		unsigned long flags;
		spin_lock_irqsave(&state->vq_tx_lock, flags);
		ret = virtqueue_add_sgs(state->vq_tx, sgs, 1, 1, inf, GFP_ATOMIC);
		if (!ret)
			virtqueue_kick(state->vq_tx);
		spin_unlock_irqrestore(&state->vq_tx_lock, flags);
	}
	if (ret) {
		inflight_dequeue(state, inf);
		kfree(inf->resp_buf);
		inf->resp_buf = NULL;
		return ret;
	}

	/*
	 * Interruptible wait — but ONLY for the IOCTL_ON_ISOLATE path
	 * (inf->isolate_id != 0), which can block arbitrarily long on a real
	 * GPU operation in the stub.  Control-plane requests leave isolate_id
	 * zero and wait uninterruptibly (they are fast and interrupting them
	 * mid-teardown would be wrong).
	 *
	 * On a pending signal we must NOT abandon the descriptor: req_buf and
	 * inf->resp_buf are still owned by the virtqueue and QEMU will write
	 * the response into resp_buf when the stub finishes.  Freeing now would
	 * be a use-after-free.  So we ask the isolate to interrupt the in-flight
	 * host ioctl (so a genuinely long GPU operation cannot pin this thread
	 * indefinitely), then wait uninterruptibly for the descriptor to come
	 * back.
	 *
	 * And then we DELIVER that response instead of reporting -ERESTARTSYS.
	 *
	 * Reporting -ERESTARTSYS here was wrong, and it is what made every X11
	 * client invisible.  The request has already been submitted to the host
	 * by the time we can be signalled, so the operation may ALREADY HAVE
	 * TAKEN EFFECT -- and several of the ones on this path are not
	 * idempotent.  NV_ESC_* 'F' nr 0xd4 parks an RM object on an export fd
	 * and DRM 'd' nr 0x49 (GEM_EXPORT_NVKMS_MEMORY) associates a bo with
	 * it; both are steps of eglExportDMABUFImageMESA.  Run them a second
	 * time on the same handle -- which is exactly what a restarted syscall
	 * does -- and the driver correctly refuses with EINVAL, because the
	 * export it is being asked to make already exists.
	 *
	 * NVIDIA's EGL reports that refusal as GL_OUT_OF_MEMORY ("Failed to
	 * acquire the EGL Image memory"), which is why this looked for a long
	 * time like an allocation/VRAM problem and was chased as one.  It is
	 * not: the card has 12 GB and the desktop uses a few hundred MB.
	 *
	 * Xwayland is the client that exposes this, and the reason is simply
	 * that it arms a periodic SIGALRM (its frame/scheduler timer).  That
	 * guarantees a signal lands inside these ioctls sooner or later, so its
	 * pixmap exports fail, the window has no content buffer, and the client
	 * is composited as an empty surface -- mapped, correctly sized, getting
	 * frame callbacks, and completely blank.  weston and native Wayland
	 * clients do not use SIGALRM, which is precisely why they were fine and
	 * only X11 broke.
	 *
	 * Measured on the physical RTX 4070 (595.84): every EINVAL in the guest
	 * was immediately preceded by an ERESTARTSYS on the same fd (51 of
	 * them), while the same weston + Xwayland + glxgears on the bare-metal
	 * host produced ZERO ERESTARTSYS and ZERO EINVAL across 21,389 traced
	 * ioctls.  With this fix: 0 GL_OUT_OF_MEMORY (was 43), 0 (EE) lines
	 * (was 982), and glxgears/glmark2/xterm/xclock/xeyes all render.
	 *
	 * Completing rather than restarting is the ordinary kernel rule for a
	 * syscall that has already made unrepeatable progress.  The signal is
	 * not lost: it stays pending and is delivered as soon as we return.
	 */
	if (inf->isolate_id) {
		if (wait_for_completion_interruptible(&inf->done) == -ERESTARTSYS) {
			nvkvm_virtio_interrupt_isolate(inf->isolate_id,
						       inf->txn_id);
			wait_for_completion(&inf->done);
			/*
			 * Fall through to the common path: the response is
			 * here, so hand it back.  Do NOT return -ERESTARTSYS.
			 */
		}
	} else {
		wait_for_completion(&inf->done);
	}
	inflight_dequeue(state, inf);

	kfree(inf->resp_buf);
	inf->resp_buf = NULL;
	return 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

/* Legacy nvkvm_virtio_open / _close / _ioctl deleted in Step 3d.
 * The isolate-aware path (OPEN_NVIDIA_HANDLE, CLOSE_HANDLE,
 * IOCTL_ON_ISOLATE) is the only path live now. */
#if 0
long nvkvm_virtio_ioctl(struct nvkvm_fd_ctx *ctx,
			unsigned int cmd,
			void *params_buf, size_t param_size,
			void *aux_buf, size_t aux_size)
{
	struct {
		struct nvkvm_hdr       hdr;
		struct nvkvm_req_ioctl req;
	} *msg;
	struct nvkvm_inflight *inf;
	__u32 txn_id = nvkvm_txn_id_alloc(&nvkvm);
	if (txn_id == 0) return -EBUSY;
	int shm_slot = -1;
	int shm_aux_slot = -1;
	long ret;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = inflight_alloc_legacy(txn_id);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}

	/* Copy parameter blob into shared memory slot */
	if (param_size > 0) {
		void *slot_ptr;
		shm_slot = nvkvm_slot_alloc(&nvkvm);
		if (shm_slot < 0) {
			ret = -ENOSPC;
			goto out;
		}
		slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
		if (!slot_ptr) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(slot_ptr, params_buf, param_size);
	}

	/* Copy auxiliary buffer (secondary params) into a separate slot */
	if (aux_size > 0 && aux_buf) {
		void *slot_ptr;
		shm_aux_slot = nvkvm_slot_alloc(&nvkvm);
		if (shm_aux_slot < 0) {
			ret = -ENOSPC;
			goto out_slot;
		}
		slot_ptr = nvkvm_slot_addr(&nvkvm, shm_aux_slot);
		if (!slot_ptr) {
			ret = -ENOMEM;
			goto out_slot;
		}
		memcpy(slot_ptr, aux_buf, aux_size);
	}

	wmb();   /* ensure all slot writes visible before kick */

	msg->hdr.type          = cpu_to_le32(NVKVM_REQ_IOCTL);
	msg->hdr.txn_id        = cpu_to_le32(txn_id);
	msg->req.fd_token      = cpu_to_le32(ctx->fd_token);
	msg->req.cmd           = cpu_to_le32(cmd);
	msg->req.param_size    = cpu_to_le32((__u32)param_size);
	msg->req.shm_slot      = cpu_to_le32((__u32)shm_slot);
	msg->req.aux_size      = cpu_to_le32((__u32)aux_size);
	msg->req.shm_aux_slot  = cpu_to_le32((__u32)(shm_aux_slot >= 0 ?
						      shm_aux_slot : 0));
	msg->req.session_id    = cpu_to_le32((__u32)ctx->session->id);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret < 0)
		goto out_aux_slot;

	/*
	 * Always copy back from shared memory, even when the driver returned an
	 * error.  The NVIDIA RM updates response fields in the params struct
	 * regardless of success/failure (e.g. NV_ESC_CHECK_VERSION_STR fills
	 * version_string and returns EINVAL; NV_ESC_RM_ALLOC writes h_object_new).
	 */
	if (param_size > 0) {
		void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
		rmb();   /* ensure host writes are visible */
		memcpy(params_buf, slot_ptr, param_size);
	}
	if (aux_size > 0 && aux_buf && shm_aux_slot >= 0) {
		void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_aux_slot);
		rmb();
		memcpy(aux_buf, slot_ptr, aux_size);
	}

	if (inf->status) {
		ret = -(long)inf->status;
		goto out_aux_slot;
	}
	ret = (long)inf->retval;

out_aux_slot:
	if (shm_aux_slot >= 0)
		nvkvm_slot_free(&nvkvm, shm_aux_slot);
out_slot:
	if (shm_slot >= 0)
		nvkvm_slot_free(&nvkvm, shm_slot);
out:
	inflight_free(&nvkvm, inf);
	kfree(msg);
	return ret;
}
#endif /* legacy ioctl helper — deleted in Step 3d */

/* ── Version negotiation ──────────────────────────────────────────────────── */

int nvkvm_negotiate_version(struct nvkvm_state *state)
{
	struct nvkvm_shm_ctrl __iomem *ctrl;
	__u32 host_version;

	if (!state->shm_base)
		return -ENODEV;

	ctrl = (struct nvkvm_shm_ctrl __iomem *)state->shm_base;
	host_version = ioread32(&ctrl->proto_version);

	if (host_version != NVKVM_PROTO_VERSION) {
		pr_err("nvkvm: protocol version mismatch: host=%u guest=%u\n",
		       host_version, NVKVM_PROTO_VERSION);
		return -EPROTO;
	}

	state->slot_size = ioread32(&ctrl->slot_size);
	if (state->slot_size < NVKVM_SHM_SLOT_MIN_SIZE ||
	    !is_power_of_2(state->slot_size)) {
		pr_err("nvkvm: invalid slot_size %zu from host\n",
		       state->slot_size);
		return -EINVAL;
	}

	memcpy_fromio(state->driver_version, ctrl->driver_version,
		      sizeof(state->driver_version));
	state->driver_version[sizeof(state->driver_version) - 1] = '\0';
	/* #81: pick the ABI profile for this host driver version. */
	state->abi = nvkvm_abi_for_version(state->driver_version);

	pr_info("nvkvm: host NVIDIA driver %s, slot_size=%zu\n",
		state->driver_version, state->slot_size);
	return 0;
}

/*
 * virtio_find_vqs() moved its parallel callbacks[]/names[] arrays into one
 * array of struct virtqueue_info.  Same three queues either way; only the
 * shape of the argument changed.  See nvkvm_compat.h for the house rule.
 */
static int nvkvm_find_vqs(struct virtio_device *vdev, unsigned int nvqs,
			  struct virtqueue *vqs[], vq_callback_t *cbs[],
			  const char * const names[])
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	return virtio_find_vqs(vdev, nvqs, vqs, cbs, names, NULL);
#else
	struct virtqueue_info info[NVKVM_NUM_VQS] = { };
	unsigned int i;

	if (nvqs > NVKVM_NUM_VQS)
		return -EINVAL;
	for (i = 0; i < nvqs; i++) {
		info[i].name     = names[i];
		info[i].callback = cbs[i];
	}
	return virtio_find_vqs(vdev, nvqs, vqs, info, NULL);
#endif
}

/* ── Virtio device init/fini ─────────────────────────────────────────────── */

int nvkvm_virtio_init(struct virtio_device *vdev, struct nvkvm_state *state)
{
	struct virtqueue *vqs[NVKVM_NUM_VQS];
	vq_callback_t *cbs[NVKVM_NUM_VQS] = {
		nvkvm_tx_done_callback, /* TX: called when QEMU posts response  */
		nvkvm_rx_callback,      /* RX: unused placeholder               */
		nvkvm_evt_callback,     /* EVT                                  */
	};
	static const char *names[NVKVM_NUM_VQS] = {
		"nvkvm-tx", "nvkvm-rx", "nvkvm-evt",
	};
	int ret;
	u64 shm_addr, shm_len;

	vdev->priv = state;
	state->vdev = vdev;

	spin_lock_init(&state->inflight_lock);
	spin_lock_init(&state->vq_tx_lock);
	INIT_LIST_HEAD(&state->inflight_list);
	atomic_set(&state->next_txn_id, 0);
	bitmap_zero(state->txn_inflight_bm, NVKVM_MAX_INFLIGHT);
	spin_lock_init(&state->slot_lock);
	bitmap_zero(state->slot_bitmap, NVKVM_SHM_NSLOTS);
	/* Mark slot 0 (ctrl) as permanently allocated */
	set_bit(NVKVM_SHM_CTRL_SLOT, state->slot_bitmap);

	ret = nvkvm_find_vqs(vdev, NVKVM_NUM_VQS, vqs, cbs, names);
	if (ret) {
		dev_err(&vdev->dev, "nvkvm: find_vqs failed: %d\n", ret);
		return ret;
	}
	state->vq_tx  = vqs[NVKVM_VQ_TX];
	state->vq_rx  = vqs[NVKVM_VQ_RX];
	state->vq_evt = vqs[NVKVM_VQ_EVT];

	/* Map shared memory BAR 0 */
	shm_addr = virtio_cread64(vdev, offsetof(struct nvkvm_virtio_config,
						 shm_base));
	shm_len  = virtio_cread64(vdev, offsetof(struct nvkvm_virtio_config,
						  shm_len));

	if (shm_len < NVKVM_SHM_SLOT_MIN_SIZE) {
		dev_err(&vdev->dev,
			"nvkvm: shared memory region too small: %llu\n",
			shm_len);
		vdev->config->del_vqs(vdev);
		return -EINVAL;
	}

	state->shm_base = ioremap(shm_addr, shm_len);
	if (!state->shm_base) {
		dev_err(&vdev->dev, "nvkvm: failed to map shared memory\n");
		vdev->config->del_vqs(vdev);
		return -ENOMEM;
	}
	state->shm_size  = shm_len;
	state->slot_size = NVKVM_SHM_SLOT_DEFAULT_SIZE; /* overridden by negotiate */

	state->mmap_window_gpa_base = virtio_cread64(vdev,
		offsetof(struct nvkvm_virtio_config, mmap_win_gpa));
	state->mmap_window_len = virtio_cread64(vdev,
		offsetof(struct nvkvm_virtio_config, mmap_win_len));

	/* QEMU dictates which capabilities this VM gets; graphics (DRM render
	 * node + NVKMS) is opt-out for compute-only VMs. */
	state->graphics_enabled = (virtio_cread64(vdev,
		offsetof(struct nvkvm_virtio_config, flags)) &
		NVKVM_CONFIG_F_GRAPHICS) != 0;

	virtio_device_ready(vdev);

	/* #101: pre-post the IN buffers QEMU fills with async OS-event
	 * notifications (NVENC/completion wakeups). Safe even if QEMU never
	 * sends any — they just sit on the queue. */
	nvkvm_evt_prime(state);
	return 0;
}

void nvkvm_virtio_fini(struct nvkvm_state *state)
{
	if (state->shm_base) {
		iounmap(state->shm_base);
		state->shm_base = NULL;
	}
	if (state->vdev) {
		state->vdev->config->reset(state->vdev);
		state->vdev->config->del_vqs(state->vdev);
		state->vdev = NULL;
	}
}

/* ── Isolate-aware API ────────────────────────────────────────────────────── */

/*
 * Simple helper: send a fixed-size request, wait for response.
 * Extracts inf->retval and inf->status for the caller.
 */
static int simple_req(__u32 req_type,
		      void *req_buf, size_t req_len,
		      __u64 *retval_out)
{
	struct nvkvm_inflight *inf;
	__u32 txn_id = nvkvm_txn_id_alloc(&nvkvm);
	if (txn_id == 0) return -EBUSY;
	void *buf;
	int ret;

	/*
	 * Callers may pass stack-allocated structs.  On kernels with
	 * CONFIG_VMAP_STACK, kernel stack pages are vmapped and virt_to_page()
	 * returns the wrong physical page, making them unusable for
	 * scatter-gather DMA.  Copy into a kmalloc'd buffer first.
	 */
	buf = kmalloc(req_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, req_buf, req_len);

	((struct nvkvm_hdr *)buf)->type   = cpu_to_le32(req_type);
	((struct nvkvm_hdr *)buf)->txn_id = cpu_to_le32(txn_id);

	inf = inflight_alloc_legacy(txn_id);
	if (!inf) {
		kfree(buf);
		return -ENOMEM;
	}

	ret = nvkvm_send_sync(&nvkvm, buf, req_len, inf);
	if (ret == 0) {
		if (inf->status)
			ret = -(int)inf->status;
		else if (retval_out)
			*retval_out = inf->retval;
	}
	inflight_free(&nvkvm, inf);
	kfree(buf);
	return ret;
}

int nvkvm_virtio_open_nvidia_handle(int dev_id, unsigned int flags,
				    unsigned int session_id,
				    __u32 *handle_id_out)
{
	struct {
		struct nvkvm_hdr                   hdr;
		struct nvkvm_req_open_nvidia_handle req;
	} msg = {};
	__u64 retval = 0;
	int ret;

	msg.req.dev_id     = cpu_to_le32(dev_id);
	msg.req.flags      = cpu_to_le32(flags);
	msg.req.session_id = cpu_to_le32(session_id);

	ret = simple_req(NVKVM_REQ_OPEN_NVIDIA_HANDLE, &msg, sizeof(msg), &retval);
	if (ret == 0 && handle_id_out)
		*handle_id_out = (__u32)retval;
	return ret;
}

int nvkvm_virtio_create_isolate(unsigned int session_id, __u32 *isolate_id_out)
{
	struct {
		struct nvkvm_hdr              hdr;
		struct nvkvm_req_create_isolate req;
	} msg = {};
	__u64 retval = 0;
	int ret;

	msg.req.session_id = cpu_to_le32(session_id);

	ret = simple_req(NVKVM_REQ_CREATE_ISOLATE, &msg, sizeof(msg), &retval);
	if (ret == 0 && isolate_id_out)
		*isolate_id_out = (__u32)retval;
	return ret;
}

/* #127: arm/disarm host-side polling of an os-event fd. The stub ppoll()s the
 * host fd and pushes a VQ_EVT (→ nvkvm_evt_deliver) when it fires, so a
 * blocking-sync waiter wakes promptly instead of eating the ~18ms poll-timeout
 * fallback. Control-plane (isolate_id stays 0 in the inflight → fast response). */
int nvkvm_virtio_poll_arm(__u32 isolate_id, __u32 handle_id, int arm)
{
	struct {
		struct nvkvm_hdr                 hdr;
		struct nvkvm_req_poll_on_isolate req;
	} msg = {};
	__u64 retval = 0;

	msg.req.isolate_id = cpu_to_le32(isolate_id);
	msg.req.handle_id  = cpu_to_le32(handle_id);
	msg.req.events     = cpu_to_le32(POLLIN);
	return simple_req(arm ? NVKVM_REQ_POLL_ON_ISOLATE
			      : NVKVM_REQ_UNPOLL_ON_ISOLATE,
			  &msg, sizeof(msg), &retval);
}

/*
 * Fetch this session's command-buffer ring placement.  Returns 0 and fills
 * *ring_gpa_out / *ring_bytes_out on success; -errno otherwise (incl. -ENODEV
 * when no ring is available → caller stays on the virtqueue path).  Carries two
 * result fields, so it can't use simple_req (which surfaces only retval).
 */
int nvkvm_virtio_setup_ring(unsigned int session_id, u64 *ring_gpa_out,
			    u32 *ring_bytes_out)
{
	struct {
		struct nvkvm_hdr           hdr;
		struct nvkvm_req_setup_ring req;
	} msg = {};
	struct nvkvm_inflight *inf;
	__u32 txn_id = nvkvm_txn_id_alloc(&nvkvm);
	void *buf;
	int ret;

	if (txn_id == 0)
		return -EBUSY;

	buf = kmalloc(sizeof(msg), GFP_KERNEL);
	if (!buf) {
		nvkvm_txn_id_free(&nvkvm, txn_id);
		return -ENOMEM;
	}
	msg.req.session_id = cpu_to_le32(session_id);
	memcpy(buf, &msg, sizeof(msg));
	((struct nvkvm_hdr *)buf)->type   = cpu_to_le32(NVKVM_REQ_SETUP_RING);
	((struct nvkvm_hdr *)buf)->txn_id = cpu_to_le32(txn_id);

	inf = inflight_alloc_legacy(txn_id);
	if (!inf) {
		kfree(buf);
		return -ENOMEM;
	}

	ret = nvkvm_send_sync(&nvkvm, buf, sizeof(msg), inf);
	if (ret == 0) {
		if (inf->status) {
			ret = -(int)inf->status;
		} else {
			if (ring_gpa_out)   *ring_gpa_out   = inf->retval;
			if (ring_bytes_out) *ring_bytes_out = inf->nvstatus;
		}
	}
	inflight_free(&nvkvm, inf);
	kfree(buf);
	return ret;
}

int nvkvm_virtio_enter_loop(unsigned int session_id, u32 idle_us, u64 *head_out)
{
	struct {
		struct nvkvm_hdr            hdr;
		struct nvkvm_req_enter_loop req;
	} msg = {};
	struct nvkvm_inflight *inf;
	__u32 txn_id = nvkvm_txn_id_alloc(&nvkvm);
	void *buf;
	int ret;

	if (txn_id == 0)
		return -EBUSY;

	buf = kmalloc(sizeof(msg), GFP_KERNEL);
	if (!buf) {
		nvkvm_txn_id_free(&nvkvm, txn_id);
		return -ENOMEM;
	}
	msg.req.session_id = cpu_to_le32(session_id);
	msg.req.idle_us    = cpu_to_le32(idle_us);
	memcpy(buf, &msg, sizeof(msg));
	((struct nvkvm_hdr *)buf)->type   = cpu_to_le32(NVKVM_REQ_ENTER_LOOP);
	((struct nvkvm_hdr *)buf)->txn_id = cpu_to_le32(txn_id);

	inf = inflight_alloc_legacy(txn_id);
	if (!inf) {
		kfree(buf);
		return -ENOMEM;
	}

	/* Blocks until the isolate's consumer loop idles out (QEMU offloads the
	 * forward so this is just a normal virtqueue wait, uninterruptible — the
	 * pump kthread owns it and exits via kthread_stop). */
	ret = nvkvm_send_sync(&nvkvm, buf, sizeof(msg), inf);
	if (ret == 0) {
		if (inf->status)
			ret = -(int)inf->status;
		else if (head_out)
			*head_out = inf->retval;
	}
	inflight_free(&nvkvm, inf);
	kfree(buf);
	return ret;
}

/*
 * nvkvm_virtio_present (#106 present path B) — tell QEMU the virtual head just
 * flipped a scanout bo backed by the stub-side GEM `stub_handle` (geometry as
 * given).  QEMU asks the owning isolate's stub to export the host dma-buf and
 * route it to the host display/codec.  Called from the KMS pipe update (atomic
 * commit tail, a sleepable kworker context), so a synchronous virtqueue wait is
 * safe; the round-trip is bounded (export is a single host ioctl).  No guest VA
 * crosses the boundary — only the opaque stub handle + scanout metadata.
 */
int nvkvm_virtio_present(struct nvkvm_fd_ctx *ctx, __u32 stub_handle,
			 __u32 width, __u32 height, __u32 pitch,
			 __u32 format, __u64 modifier)
{
	struct {
		struct nvkvm_hdr         hdr;
		struct nvkvm_req_present req;
	} msg = {};
	struct nvkvm_inflight *inf;
	__u32 txn_id;
	void *buf;
	int ret;

	if (!ctx || !ctx->session)
		return -EBADF;

	txn_id = nvkvm_txn_id_alloc(&nvkvm);
	if (txn_id == 0)
		return -EBUSY;

	buf = kmalloc(sizeof(msg), GFP_KERNEL);
	if (!buf) {
		nvkvm_txn_id_free(&nvkvm, txn_id);
		return -ENOMEM;
	}
	msg.req.isolate_id  = cpu_to_le32(ctx->session->isolate_id);
	msg.req.handle_id   = cpu_to_le32(ctx->handle_id);
	msg.req.stub_handle = cpu_to_le32(stub_handle);
	msg.req.width       = cpu_to_le32(width);
	msg.req.height      = cpu_to_le32(height);
	msg.req.pitch       = cpu_to_le32(pitch);
	msg.req.format      = cpu_to_le32(format);
	msg.req.session_id  = cpu_to_le32((__u32)ctx->session->id);
	msg.req.modifier    = cpu_to_le64(modifier);
	memcpy(buf, &msg, sizeof(msg));
	((struct nvkvm_hdr *)buf)->type   = cpu_to_le32(NVKVM_REQ_PRESENT);
	((struct nvkvm_hdr *)buf)->txn_id = cpu_to_le32(txn_id);

	inf = inflight_alloc_legacy(txn_id);
	if (!inf) {
		kfree(buf);
		return -ENOMEM;
	}

	ret = nvkvm_send_sync(&nvkvm, buf, sizeof(msg), inf);
	if (ret == 0 && inf->status)
		ret = -(int)inf->status;
	inflight_free(&nvkvm, inf);
	kfree(buf);
	return ret;
}

/*
 * #110 cross-isolate dma-buf import.  `ctx` is the importer (the drm_file the
 * GEM op arrived on); the bo is owned by a different isolate.  QEMU brokers it
 * (owner PRIME export → importer PRIME import) and returns a stub GEM handle
 * valid in the importer's render-node fd, which the caller then uses to forward
 * the RM export/import (0x09 / 0x3d06) entirely within the importer.
 */
int nvkvm_virtio_xiso_import(struct nvkvm_fd_ctx *ctx,
			     __u32 owner_isolate_id, __u32 owner_handle_id,
			     __u32 owner_stub_handle, __u32 *gem_out)
{
	struct {
		struct nvkvm_hdr             hdr;
		struct nvkvm_req_xiso_import req;
	} msg = {};
	__u64 retval = 0;
	int ret;

	if (!ctx || !ctx->session)
		return -EBADF;
	if (!owner_isolate_id || !owner_handle_id)
		return -EINVAL;

	msg.req.owner_isolate_id    = cpu_to_le32(owner_isolate_id);
	msg.req.owner_handle_id     = cpu_to_le32(owner_handle_id);
	msg.req.owner_stub_handle   = cpu_to_le32(owner_stub_handle);
	msg.req.importer_isolate_id = cpu_to_le32(ctx->session->isolate_id);
	msg.req.importer_handle_id  = cpu_to_le32(ctx->handle_id);

	ret = simple_req(NVKVM_REQ_XISO_IMPORT, &msg, sizeof(msg), &retval);
	if (ret == 0 && gem_out)
		*gem_out = (__u32)retval;
	return ret;
}

int nvkvm_virtio_copy_handle_to_isolate(__u32 handle_id, __u32 isolate_id)
{
	struct {
		struct nvkvm_hdr                      hdr;
		struct nvkvm_req_copy_handle_to_isolate req;
	} msg = {};

	msg.req.handle_id  = cpu_to_le32(handle_id);
	msg.req.isolate_id = cpu_to_le32(isolate_id);

	return simple_req(NVKVM_REQ_COPY_HANDLE_TO_ISOLATE,
			  &msg, sizeof(msg), NULL);
}

int nvkvm_virtio_close_handle_on_isolate(__u32 handle_id, __u32 isolate_id)
{
	struct {
		struct nvkvm_hdr                       hdr;
		struct nvkvm_req_close_handle_on_isolate req;
	} msg = {};

	msg.req.handle_id  = cpu_to_le32(handle_id);
	msg.req.isolate_id = cpu_to_le32(isolate_id);

	return simple_req(NVKVM_REQ_CLOSE_HANDLE_ON_ISOLATE,
			  &msg, sizeof(msg), NULL);
}

int nvkvm_virtio_close_handle(__u32 handle_id)
{
	struct {
		struct nvkvm_hdr          hdr;
		struct nvkvm_req_close_handle req;
	} msg = {};

	msg.req.handle_id = cpu_to_le32(handle_id);

	return simple_req(NVKVM_REQ_CLOSE_HANDLE, &msg, sizeof(msg), NULL);
}

int nvkvm_virtio_kill_isolate(__u32 isolate_id)
{
	struct {
		struct nvkvm_hdr            hdr;
		struct nvkvm_req_kill_isolate req;
	} msg = {};

	msg.req.isolate_id = cpu_to_le32(isolate_id);

	return simple_req(NVKVM_REQ_KILL_ISOLATE, &msg, sizeof(msg), NULL);
}

/*
 * nvkvm_virtio_interrupt_isolate — ask QEMU to interrupt an in-flight ioctl.
 *
 * Called from nvkvm_send_sync when a task blocked on a forwarded ioctl
 * receives a signal.  This is itself a control-plane request (isolate_id is
 * NOT set on its own inflight record) so it waits uninterruptibly for QEMU's
 * ack — QEMU only routes the interrupt to the stub and replies immediately;
 * it does not wait for the host ioctl to actually return.
 */
int nvkvm_virtio_interrupt_isolate(__u32 isolate_id, __u32 target_txn)
{
	struct {
		struct nvkvm_hdr           hdr;
		struct nvkvm_req_interrupt req;
	} msg = {};

	msg.req.isolate_id = cpu_to_le32(isolate_id);
	msg.req.target_txn = cpu_to_le32(target_txn);

	return simple_req(NVKVM_REQ_INTERRUPT, &msg, sizeof(msg), NULL);
}

/*
 * nvkvm_virtio_ioctl_on_isolate — forward ioctl through the isolate path.
 *
 * VMA whitelist (all VMAs in the current mm) is collected here and sent in a
 * third shm slot.  On -EFAULT, *fault_addr_out is set to the faulting GVA so
 * the caller can map the region and retry.
 */
long nvkvm_virtio_ioctl_on_isolate(struct nvkvm_fd_ctx *ctx,
				   unsigned int cmd,
				   void *params_buf, size_t param_size,
				   void *aux_buf, size_t aux_size,
				   __u32 flags,
				   __u64 *fault_addr_out)
{
	struct {
		struct nvkvm_hdr                 hdr;
		struct nvkvm_req_ioctl_on_isolate req;
	} *msg;
	struct nvkvm_inflight *inf;
	__u32 txn_id = nvkvm_txn_id_alloc(&nvkvm);
	if (txn_id == 0) return -EBUSY;
	int shm_slot = -1, shm_aux_slot = -1, shm_vma_slot = -1;
	struct nvkvm_vma_entry *vma_buf = NULL;
	size_t vma_count = 0;
	long ret;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = inflight_alloc_legacy(txn_id);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}
	/* Mark this wait interruptible: on a guest signal, send_sync will ask
	 * this isolate to interrupt the in-flight host ioctl. */
	inf->isolate_id = ctx->session->isolate_id;

	/*
	 * FF-2 backstop (security_audit_2026_06_01): the RM_CONTROL aux-extend
	 * paths (info-list / FIFO_GET_CHANNELLIST / GET_CLASSLIST / BUILD_VERSION)
	 * can grow aux_size well past a single SHM slot. Each slot is one tile of a
	 * VM-WIDE shared region, so an over-large memcpy here would stomp adjacent
	 * slots held by OTHER guest processes (class-1 LPE) or run off the region.
	 * Refuse before any slot memcpy can overflow. slot_size is host-negotiated,
	 * validated power-of-two >= NVKVM_SHM_SLOT_MIN_SIZE.
	 */
	if (param_size > nvkvm.slot_size || aux_size > nvkvm.slot_size) {
		pr_err_ratelimited("nvkvm: param/aux size %zu/%zu exceeds slot_size %zu — refusing\n",
				   param_size, aux_size, nvkvm.slot_size);
		ret = -EINVAL;
		goto out;
	}

	/* Param slot */
	if (param_size > 0) {
		void *slot_ptr;
		shm_slot = nvkvm_slot_alloc(&nvkvm);
		if (shm_slot < 0) { ret = -ENOSPC; goto out; }
		slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
		if (!slot_ptr) { ret = -ENOMEM; goto out; }
		memcpy(slot_ptr, params_buf, param_size);
	}

	/* Aux slot */
	if (aux_size > 0 && aux_buf) {
		void *slot_ptr;
		shm_aux_slot = nvkvm_slot_alloc(&nvkvm);
		if (shm_aux_slot < 0) { ret = -ENOSPC; goto out; }
		slot_ptr = nvkvm_slot_addr(&nvkvm, shm_aux_slot);
		if (!slot_ptr) { ret = -ENOMEM; goto out; }
		memcpy(slot_ptr, aux_buf, aux_size);
	}

	/*
	 * VMA whitelist slot — all VMAs in current mm.
	 *
	 * current->mm can be NULL here: a forwarded ioctl may run from a context
	 * with no user mm — most notably GEM_CLOSE forwarded out of
	 * nvkvm_gem_free() during drm_release() at PROCESS EXIT, where the kernel
	 * has already run exit_mm() (current->mm = NULL) before exit_files()
	 * closes the DRM fd. mmap_read_lock(NULL) then faults at &NULL->mmap_lock
	 * (offset 0xb0) — a hard oops that, for a compositor holding DRM master,
	 * leaves the master stuck and wedges all later modeset (SET_MASTER EBUSY).
	 *
	 * No mm means no guest VAs to whitelist, and the commands that reach this
	 * path without an mm (GEM_CLOSE) carry no embedded user pointers anyway, so
	 * skipping the whitelist (vma_count stays 0) is correct, not a workaround.
	 */
	vma_buf = current->mm ? kzalloc(sizeof(*vma_buf) * NVKVM_MAX_VMA_ENTRIES,
					GFP_KERNEL) : NULL;
	if (vma_buf) {
		struct mm_struct *mm = current->mm;
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, 0);

		mmap_read_lock(mm);
		for_each_vma(vmi, vma) {
			if (vma_count >= NVKVM_MAX_VMA_ENTRIES)
				break;
			vma_buf[vma_count].start    = cpu_to_le64(vma->vm_start);
			vma_buf[vma_count].end      = cpu_to_le64(vma->vm_end);
			vma_buf[vma_count].prot     = cpu_to_le32(
				(vma->vm_flags & VM_READ  ? PROT_READ  : 0) |
				(vma->vm_flags & VM_WRITE ? PROT_WRITE : 0) |
				(vma->vm_flags & VM_EXEC  ? PROT_EXEC  : 0));
			vma_buf[vma_count].reserved = 0;
			vma_count++;
		}
		mmap_read_unlock(mm);

		if (vma_count > 0) {
			size_t vma_bytes = vma_count * sizeof(*vma_buf);
			void *slot_ptr;
			shm_vma_slot = nvkvm_slot_alloc(&nvkvm);
			if (shm_vma_slot >= 0) {
				slot_ptr = nvkvm_slot_addr(&nvkvm, shm_vma_slot);
				if (slot_ptr)
					memcpy(slot_ptr, vma_buf, vma_bytes);
				else {
					nvkvm_slot_free(&nvkvm, shm_vma_slot);
					shm_vma_slot = -1;
					vma_count = 0;
				}
			} else {
				vma_count = 0;
			}
		}
		kfree(vma_buf);
	}

	wmb();

	msg->hdr.type   = cpu_to_le32(NVKVM_REQ_IOCTL_ON_ISOLATE);
	msg->hdr.txn_id = cpu_to_le32(txn_id);
	msg->req.isolate_id              = cpu_to_le32(ctx->session->isolate_id);
	msg->req.handle_id               = cpu_to_le32(ctx->handle_id);
	msg->req.cmd                     = cpu_to_le32(cmd);
	msg->req.param_size              = cpu_to_le32((__u32)param_size);
	msg->req.shm_slot                = cpu_to_le32((__u32)(shm_slot >= 0 ? shm_slot : 0));
	msg->req.aux_size                = cpu_to_le32((__u32)aux_size);
	msg->req.shm_aux_slot            = cpu_to_le32((__u32)(shm_aux_slot >= 0 ? shm_aux_slot : 0));
	msg->req.vma_whitelist_nentries  = cpu_to_le32((__u32)vma_count);
	msg->req.vma_whitelist_slot      = cpu_to_le32((__u32)(shm_vma_slot >= 0 ? shm_vma_slot : 0));
	msg->req.flags                   = cpu_to_le32(flags);
	msg->req.session_id              = cpu_to_le32((__u32)ctx->session->id);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret < 0)
		goto out;

	/* Copy back params and aux from shared memory */
	if (param_size > 0 && shm_slot >= 0) {
		void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
		rmb();
		if (slot_ptr)
			memcpy(params_buf, slot_ptr, param_size);
	}
	if (aux_size > 0 && aux_buf && shm_aux_slot >= 0) {
		void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_aux_slot);
		rmb();
		if (slot_ptr)
			memcpy(aux_buf, slot_ptr, aux_size);
	}

	if (fault_addr_out)
		*fault_addr_out = inf->fault_addr;

	if (inf->status == EFAULT && inf->fault_addr) {
		ret = -EFAULT;
		goto out;
	}
	if (inf->status) {
		ret = -(long)inf->status;
		goto out;
	}
	ret = (long)inf->retval;

out:
	if (shm_vma_slot >= 0) nvkvm_slot_free(&nvkvm, shm_vma_slot);
	if (shm_aux_slot >= 0) nvkvm_slot_free(&nvkvm, shm_aux_slot);
	if (shm_slot >= 0)     nvkvm_slot_free(&nvkvm, shm_slot);
	inflight_free(&nvkvm, inf);
	kfree(msg);
	return ret;
}

int nvkvm_virtio_munmap_on_isolate(__u32 isolate_id, __u32 mmap_token)
{
	struct {
		struct nvkvm_hdr                   hdr;
		struct nvkvm_req_munmap_on_isolate req;
	} msg = {};

	msg.req.isolate_id  = cpu_to_le32(isolate_id);
	msg.req.mmap_token  = cpu_to_le32(mmap_token);

	return simple_req(NVKVM_REQ_MUNMAP_ON_ISOLATE, &msg, sizeof(msg), NULL);
}

int nvkvm_virtio_open_memory_handle(unsigned int session_id, __u64 size,
				    __u32 *handle_id_out)
{
	struct {
		struct nvkvm_hdr                    hdr;
		struct nvkvm_req_open_memory_handle req;
	} msg = {};
	__u64 retval = 0;
	int ret;

	msg.req.size       = cpu_to_le64(size);
	msg.req.session_id = cpu_to_le32(session_id);

	ret = simple_req(NVKVM_REQ_OPEN_MEMORY_HANDLE, &msg, sizeof(msg), &retval);
	if (ret == 0 && handle_id_out)
		*handle_id_out = (__u32)retval;
	return ret;
}

int nvkvm_virtio_write_memory_handle(__u32 handle_id, __u64 offset,
				     int shm_slot, __u32 size)
{
	struct {
		struct nvkvm_hdr                      hdr;
		struct nvkvm_req_write_memory_handle  req;
	} *msg;
	__u32 txn_id = nvkvm_txn_id_alloc(&nvkvm);
	if (txn_id == 0) return -EBUSY;
	struct nvkvm_inflight *inf;
	int ret;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = inflight_alloc_legacy(txn_id);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}

	msg->hdr.type      = cpu_to_le32(NVKVM_REQ_WRITE_MEMORY_HANDLE);
	msg->hdr.txn_id    = cpu_to_le32(txn_id);
	msg->req.handle_id = cpu_to_le32(handle_id);
	msg->req.shm_slot  = cpu_to_le32((__u32)shm_slot);
	msg->req.offset    = cpu_to_le64(offset);
	msg->req.size      = cpu_to_le32(size);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret == 0 && inf->status)
		ret = -(int)inf->status;
	inflight_free(&nvkvm, inf);
	kfree(msg);
	return ret;
}

int nvkvm_virtio_read_memory_handle(__u32 handle_id, __u64 offset,
				    int shm_slot, __u32 size)
{
	struct {
		struct nvkvm_hdr                     hdr;
		struct nvkvm_req_read_memory_handle  req;
	} *msg;
	__u32 txn_id = nvkvm_txn_id_alloc(&nvkvm);
	if (txn_id == 0) return -EBUSY;
	struct nvkvm_inflight *inf;
	int ret;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = inflight_alloc_legacy(txn_id);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}

	msg->hdr.type      = cpu_to_le32(NVKVM_REQ_READ_MEMORY_HANDLE);
	msg->hdr.txn_id    = cpu_to_le32(txn_id);
	msg->req.handle_id = cpu_to_le32(handle_id);
	msg->req.shm_slot  = cpu_to_le32((__u32)shm_slot);
	msg->req.offset    = cpu_to_le64(offset);
	msg->req.size      = cpu_to_le32(size);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret == 0 && inf->status)
		ret = -(int)inf->status;
	inflight_free(&nvkvm, inf);
	kfree(msg);
	return ret;
}

int nvkvm_virtio_mmap_on_isolate(__u32 isolate_id, __u32 handle_id,
				 __u64 gva, __u64 offset, __u64 length,
				 __u32 prot, __u32 map_flags,
				 unsigned int session_id,
				 __u64 *gpa_base_out,
				 __u32 *mmap_token_out)
{
	struct {
		struct nvkvm_hdr                hdr;
		struct nvkvm_req_mmap_on_isolate req;
	} *msg;
	struct nvkvm_inflight *inf;
	__u32 txn_id = nvkvm_txn_id_alloc(&nvkvm);
	if (txn_id == 0) return -EBUSY;
	int ret;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = inflight_alloc_legacy(txn_id);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}

	msg->hdr.type       = cpu_to_le32(NVKVM_REQ_MMAP_ON_ISOLATE);
	msg->hdr.txn_id     = cpu_to_le32(txn_id);
	msg->req.isolate_id = cpu_to_le32(isolate_id);
	msg->req.handle_id  = cpu_to_le32(handle_id);
	msg->req.gva        = cpu_to_le64(gva);
	msg->req.offset     = cpu_to_le64(offset);
	msg->req.length     = cpu_to_le64(length);
	msg->req.prot       = cpu_to_le32(prot);
	msg->req.map_flags  = cpu_to_le32(map_flags);
	msg->req.session_id = cpu_to_le32(session_id);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret == 0) {
		if (inf->status) {
			ret = -(int)inf->status;
		} else {
			if (gpa_base_out)   *gpa_base_out   = inf->retval;
			if (mmap_token_out) *mmap_token_out = inf->nvstatus;
		}
	}
	inflight_free(&nvkvm, inf);
	kfree(msg);
	return ret;
}

/* ── REALIZE_UVM_MAPPING ─────────────────────────────────────────────────────
 *
 * State-machine model: instead of sending a raw mmap, the guest module
 * uploads the recorded UVM state + intent into two shm slots and asks
 * QEMU to realize the whole mapping atomically.  See
 * docs/STATE_MACHINE_PLAN.md.
 */
int nvkvm_virtio_realize_uvm_mapping(__u32 isolate_id, __u32 fd_handle_id,
				     __u32 mode, unsigned int session_id,
				     __u64 gva, __u64 length, __u64 offset_hint,
				     __u32 prot, __u32 map_flags,
				     __u32 state_shm_slot,
				     __u32 intent_shm_slot,
				     __u32 intent_size,
				     __u64 *gpa_base_out,
				     __u64 *realize_token_out,
				     __u32 *rm_status_out)
{
	struct {
		struct nvkvm_hdr                       hdr;
		struct nvkvm_req_realize_uvm_mapping   req;
	} *msg;
	struct nvkvm_inflight *inf;
	__u32 txn_id = nvkvm_txn_id_alloc(&nvkvm);
	int ret;

	if (txn_id == 0)
		return -EBUSY;
	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg) {
		nvkvm_txn_id_free(&nvkvm, txn_id);
		return -ENOMEM;
	}
	inf = inflight_alloc_legacy(txn_id);
	if (!inf) {
		kfree(msg);
		nvkvm_txn_id_free(&nvkvm, txn_id);
		return -ENOMEM;
	}

	msg->hdr.type            = cpu_to_le32(NVKVM_REQ_REALIZE_UVM_MAPPING);
	msg->hdr.txn_id          = cpu_to_le32(txn_id);
	msg->req.isolate_id      = cpu_to_le32(isolate_id);
	msg->req.fd_handle_id    = cpu_to_le32(fd_handle_id);
	msg->req.mode            = cpu_to_le32(mode);
	msg->req.session_id      = cpu_to_le32(session_id);
	msg->req.gva             = cpu_to_le64(gva);
	msg->req.length          = cpu_to_le64(length);
	msg->req.offset_hint     = cpu_to_le64(offset_hint);
	msg->req.prot            = cpu_to_le32(prot);
	msg->req.map_flags       = cpu_to_le32(map_flags);
	msg->req.state_shm_slot  = cpu_to_le32(state_shm_slot);
	msg->req.intent_shm_slot = cpu_to_le32(intent_shm_slot);
	msg->req.intent_size     = cpu_to_le32(intent_size);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret == 0) {
		if (inf->status) {
			ret = -(int)inf->status;
			if (rm_status_out) *rm_status_out = inf->nvstatus;
		} else {
			if (gpa_base_out)      *gpa_base_out      = inf->retval;
			if (realize_token_out) *realize_token_out = inf->fault_addr;
			if (rm_status_out)     *rm_status_out     = inf->nvstatus;
		}
	}
	inflight_free(&nvkvm, inf);
	kfree(msg);
	return ret;
}

/* ── READ_HOST_FILE ─────────────────────────────────────────────────────── */

int nvkvm_virtio_read_host_file(__u32 file_id, __u32 gpu_index, __u32 shm_slot,
				__u32 max_len, __u32 *nbytes_out)
{
	struct {
		struct nvkvm_hdr                   hdr;
		struct nvkvm_req_read_host_file    req;
	} *msg;
	struct nvkvm_inflight *inf;
	__u32 txn_id = nvkvm_txn_id_alloc(&nvkvm);
	int ret;

	if (txn_id == 0) return -EBUSY;
	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg) { nvkvm_txn_id_free(&nvkvm, txn_id); return -ENOMEM; }
	inf = inflight_alloc_legacy(txn_id);
	if (!inf) { kfree(msg); nvkvm_txn_id_free(&nvkvm, txn_id); return -ENOMEM; }

	msg->hdr.type    = cpu_to_le32(NVKVM_REQ_READ_HOST_FILE);
	msg->hdr.txn_id  = cpu_to_le32(txn_id);
	msg->req.file_id = cpu_to_le32(file_id);
	msg->req.shm_slot = cpu_to_le32(shm_slot);
	msg->req.max_len  = cpu_to_le32(max_len);
	/* Selects which host GPU a per-GPU file (INFORMATION / REGISTRY /
	 * NUMA_STATUS) is read from: an index into QEMU's sorted host-BDF list.
	 * The guest never names a path or a BDF, only this integer, so the
	 * host-side whitelist stays the whole security boundary.  Ignored by
	 * QEMU for the host-wide files (params, version, initstate). */
	msg->req.gpu_index = cpu_to_le32(gpu_index);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret == 0) {
		if (inf->status)
			ret = -(int)inf->status;
		else if (nbytes_out)
			*nbytes_out = (__u32)inf->retval;
	}
	inflight_free(&nvkvm, inf);
	kfree(msg);
	return ret;
}
