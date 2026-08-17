// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_session.c — per-process session management
 *
 * A session groups all /dev/nvidia* file descriptors belonging to a single
 * guest process (identified by tgid). This mirrors gVisor's model where each
 * container has its own nvproxy instance with isolated client/object tables.
 *
 * Sessions are reference-counted. The last fd close drops the session, which
 * triggers host-side cleanup of all RM objects associated with the session's
 * clients.
 *
 * Isolation: each session gets a unique session_id sent with every ioctl
 * request. The QEMU backend keeps per-session fd and object tables, ensuring
 * that RM handles from one session cannot be used by another (the host
 * validates session_id against fd_token ownership on every request).
 */

#include <linux/slab.h>
#include <linux/io.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/sched/mm.h>
#include <linux/pid.h>

#include "nvkvm.h"

struct nvkvm_session *nvkvm_session_get_or_create(struct mm_struct *mm,
						  pid_t tgid)
{
	struct nvkvm_session *session = NULL;
	int id;

	mutex_lock(&nvkvm.sessions_lock);

	/*
	 * The security PRINCIPAL is the address space (mm), not the tgid.  This is
	 * deliberate and matches the only sane boundary: nvidia keys access on the
	 * tgid (RS_SHARE_TYPE_PID = current->tgid) and a thread group always has
	 * exactly one mm, so for every normal process mm and tgid are 1:1.  The
	 * only way they diverge is CLONE_VM without CLONE_THREAD (two tgids sharing
	 * one address space — vfork's transient window, or hand-rolled clone).
	 * Those tasks can already read/write each other's memory directly, so they
	 * are ONE security domain; folding them into a single mm-keyed session is
	 * correct, not a weakening.  We do NOT support sub-dividing a session by
	 * tgid.  (mm is also the robust key: tgids get recycled — audit H2.)
	 */
	idr_for_each_entry(&nvkvm.sessions_idr, session, id) {
		if (session->mm == mm) {
			if (session->tgid != tgid)
				pr_warn_once("nvkvm: tgid %d shares mm with session tgid %d (CLONE_VM w/o CLONE_THREAD); treating as one principal\n",
					     tgid, session->tgid);
			session->refcount++;
			mutex_unlock(&nvkvm.sessions_lock);
			return session;
		}
	}

	/* Allocate a new session */
	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session) {
		mutex_unlock(&nvkvm.sessions_lock);
		return ERR_PTR(-ENOMEM);
	}
	mmgrab(mm);                /* pin the mm — drop in nvkvm_session_put */
	session->mm         = mm;
	session->tgid       = tgid;
	/* Pin the tgid pid so GET_PIDS can later translate it into whatever
	 * pid namespace the querying process lives in (Docker-on-guest). */
	session->tgid_pid   = get_task_pid(current, PIDTYPE_TGID);
	session->refcount   = 1;
	session->isolate_id = 0;
	mutex_init(&session->isolate_lock);
	mutex_init(&session->ring_lock);
	init_waitqueue_head(&session->pump_wq);
	spin_lock_init(&session->vcache_lock);

	id = idr_alloc(&nvkvm.sessions_idr, session, 1, 0, GFP_KERNEL);
	if (id < 0) {
		mmdrop(mm);
		kfree(session);
		mutex_unlock(&nvkvm.sessions_lock);
		return ERR_PTR(id);
	}
	session->id = id;

	mutex_unlock(&nvkvm.sessions_lock);
	return session;
}

void nvkvm_session_vcache_clear(struct nvkvm_session *session)
{
	unsigned long fl;
	int k;
	spin_lock_irqsave(&session->vcache_lock, fl);
	for (k = 0; k < NVKVM_VCACHE_N; k++)
		session->vcache[k].valid = false;
	spin_unlock_irqrestore(&session->vcache_lock, fl);
}

void nvkvm_session_put(struct nvkvm_session *session)
{
	bool last;
	__u32 isolate_id = 0;
	struct mm_struct *mm = NULL;
	struct pid *tgid_pid = NULL;
	void *ring_base = NULL;

	mutex_lock(&nvkvm.sessions_lock);
	last = --session->refcount == 0;
	if (last) {
		idr_remove(&nvkvm.sessions_idr, session->id);
		isolate_id = session->isolate_id;
		session->isolate_id = 0;
		ring_base = session->ring_base;
		session->ring_base = NULL;
		session->req_ring = session->resp_ring = NULL;
		mm = session->mm;
		session->mm = NULL;
		tgid_pid = session->tgid_pid;
		session->tgid_pid = NULL;
	}
	mutex_unlock(&nvkvm.sessions_lock);

	if (last) {
		/* Stop the pump before unmapping/killing: it must reach its
		 * wait_event and exit while the isolate is still alive so any
		 * in-flight ENTER_LOOP completes (req_ring is already NULL above,
		 * so the pump sees no work and idles out). */
		nvkvm_session_stop_pump(session);
		/* Drop our ring mapping; QEMU frees the GPA + memfd on isolate kill. */
		if (ring_base)
			memunmap(ring_base);
		/* Kill the isolate process before freeing the session struct. */
		if (isolate_id)
			nvkvm_virtio_kill_isolate(isolate_id);
		if (mm)
			mmdrop(mm);
		if (tgid_pid)
			put_pid(tgid_pid);
		mutex_destroy(&session->isolate_lock);
		kfree(session);
	}
}
