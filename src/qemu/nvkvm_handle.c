/*
 * nvkvm_handle.c — global handle table for nvidia and memory handles
 *
 * All operations are serialized under handle_table.lock.
 * The handle ID 0 is reserved (invalid).
 */

#include "qemu/osdep.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef SYS_memfd_create
/* x86-64 */
#define SYS_memfd_create 319
#endif

static inline int memfd_create_compat(const char *name, unsigned int flags)
{
	return (int)syscall(SYS_memfd_create, name, flags);
}
#undef memfd_create
#define memfd_create memfd_create_compat

#include "nvkvm_handle.h"
#include "../../src/common/nvkvm_proto.h"

/* Device path table indexed by NVKVM_DEV_* */
static const char *nvidia_dev_path(int dev_id)
{
	static char gpu_path[32];
	if (dev_id == NVKVM_DEV_CTL)
		return "/dev/nvidiactl";
	if (dev_id == NVKVM_DEV_UVM)
		return "/dev/nvidia-uvm";
	int n = dev_id - 16;
	if (n >= 0 && n < 16) {
		snprintf(gpu_path, sizeof(gpu_path), "/dev/nvidia%d", n);
		return gpu_path;
	}
	return NULL;
}

void nvkvm_handle_table_init(struct nvkvm_handle_table *t)
{
	memset(t, 0, sizeof(*t));
	pthread_mutex_init(&t->lock, NULL);
	t->next_id = 1;
	t->next_generation = 1;
	for (int i = 0; i < NVKVM_HANDLE_MAX; i++)
		t->handles[i].fd = -1;
}

void nvkvm_handle_table_fini(struct nvkvm_handle_table *t)
{
	pthread_mutex_lock(&t->lock);
	for (int i = 0; i < NVKVM_HANDLE_MAX; i++) {
		if (t->handles[i].in_use && t->handles[i].fd >= 0) {
			close(t->handles[i].fd);
			t->handles[i].fd = -1;
		}
	}
	pthread_mutex_unlock(&t->lock);
	pthread_mutex_destroy(&t->lock);
}

/* Allocate next available handle slot. Called with lock held. */
static struct nvkvm_handle *alloc_slot(struct nvkvm_handle_table *t,
				       uint32_t *id_out)
{
	/* linear scan from next_id (IDs are rare, table is small) */
	for (int attempt = 0; attempt < NVKVM_HANDLE_MAX; attempt++) {
		uint32_t id = t->next_id;
		t->next_id++;
		if (t->next_id >= NVKVM_HANDLE_MAX)
			t->next_id = 1;
		if (id == 0)
			continue;
		struct nvkvm_handle *h = &t->handles[id % NVKVM_HANDLE_MAX];
		if (!h->in_use) {
			memset(h, 0, sizeof(*h));
			h->id     = id;
			h->generation = t->next_generation++;
			if (t->next_generation == 0)
				t->next_generation = 1;
			h->fd     = -1;
			h->in_use = true;
			*id_out   = id;
			return h;
		}
	}
	return NULL;
}

int nvkvm_handle_open_nvidia(struct nvkvm_handle_table *t,
			     uint32_t session_id, int dev_id, int flags,
			     uint32_t *handle_id_out)
{
	int fd;

	if (dev_id == NVKVM_DEV_EVENTFD) {
		/* libcuda passes an eventfd to RM_ALLOC NV01_EVENT_OS_EVENT
		 * — that fd is only valid in the guest userspace process,
		 * so we materialise a real eventfd inside QEMU on the same
		 * path the nvidia handles use.  The same SCM_RIGHTS-to-
		 * isolate flow gives the stub a usable fd to hand the
		 * driver.  Subsequent event delivery back to the guest's
		 * eventfd is via VQ_EVT (TODO; not needed for cuCtxCreate
		 * + cuMemAlloc to make progress). */
		fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (fd < 0)
			return -errno;
	} else {
		const char *path = nvidia_dev_path(dev_id);
		if (!path)
			return -EINVAL;
		fd = open(path, flags | O_CLOEXEC);
		if (fd < 0)
			return -errno;
	}

	pthread_mutex_lock(&t->lock);
	uint32_t id;
	struct nvkvm_handle *h = alloc_slot(t, &id);
	if (!h) {
		pthread_mutex_unlock(&t->lock);
		close(fd);
		return -EMFILE;
	}
	h->type       = NVKVM_HANDLE_TYPE_NVIDIA;
	h->fd         = fd;
	h->session_id = session_id;
	h->dev_id     = dev_id;
	*handle_id_out = id;
	pthread_mutex_unlock(&t->lock);

	NVKVM_DBG( "nvkvm_handle: opened %s handle %u dev_id=%d fd=%d\n",
		dev_id == NVKVM_DEV_EVENTFD ? "eventfd" : "nvidia",
		id, dev_id, fd);
	return 0;
}

int nvkvm_handle_alloc_pending(struct nvkvm_handle_table *t,
				uint32_t session_id, int dev_id,
				uint32_t *handle_id_out)
{
	pthread_mutex_lock(&t->lock);
	uint32_t id;
	struct nvkvm_handle *h = alloc_slot(t, &id);
	if (!h) {
		pthread_mutex_unlock(&t->lock);
		return -EMFILE;
	}
	h->type       = NVKVM_HANDLE_TYPE_NVIDIA;
	h->fd         = -1;
	h->session_id = session_id;
	h->dev_id     = dev_id;
	*handle_id_out = id;
	pthread_mutex_unlock(&t->lock);
	return 0;
}

int nvkvm_handle_attach_fd(struct nvkvm_handle_table *t,
			   uint32_t handle_id, int fd)
{
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX || fd < 0)
		return -EINVAL;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (!h->in_use || h->id != handle_id) {
		pthread_mutex_unlock(&t->lock);
		return -EBADF;
	}
	if (h->fd >= 0) {
		pthread_mutex_unlock(&t->lock);
		return -EEXIST;   /* attach is one-shot */
	}
	h->fd = fd;
	pthread_mutex_unlock(&t->lock);
	return 0;
}

void nvkvm_handle_abort_open(struct nvkvm_handle_table *t, uint32_t handle_id)
{
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX)
		return;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (h->in_use && h->id == handle_id && h->fd < 0) {
		h->in_use = false;
		/* No close: fd was never attached. */
	}
	pthread_mutex_unlock(&t->lock);
}

int nvkvm_handle_open_memory(struct nvkvm_handle_table *t,
			     uint32_t session_id, uint64_t size,
			     uint32_t *handle_id_out)
{
	int fd = memfd_create("nvkvm_mem", MFD_CLOEXEC);
	if (fd < 0)
		return -errno;

	if (size > 0 && ftruncate(fd, (off_t)size) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}

	pthread_mutex_lock(&t->lock);
	uint32_t id;
	struct nvkvm_handle *h = alloc_slot(t, &id);
	if (!h) {
		pthread_mutex_unlock(&t->lock);
		close(fd);
		return -EMFILE;
	}
	h->type       = NVKVM_HANDLE_TYPE_MEMORY;
	h->fd         = fd;
	h->session_id = session_id;
	h->dev_id     = 0;
	/* S-1: remember what we just sized the memfd to.  Discarding it (as
	 * this did) is what left every later (offset, length) on this object
	 * unbounded — the ftruncate is the object's extent, so it has to
	 * survive the call that performed it. */
	h->size       = size;
	*handle_id_out = id;
	pthread_mutex_unlock(&t->lock);

	NVKVM_DBG( "nvkvm_handle: opened memory handle %u size=%llu fd=%d\n",
		id, (unsigned long long)size, fd);
	return 0;
}

struct nvkvm_handle *nvkvm_handle_get(struct nvkvm_handle_table *t,
				      uint32_t handle_id)
{
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX)
		return NULL;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (!h->in_use || h->id != handle_id) {
		pthread_mutex_unlock(&t->lock);
		return NULL;
	}
	pthread_mutex_unlock(&t->lock);
	return h;
}

/*
 * Audit C-2 fix: return a *dup* of the handle's fd, taken atomically under the
 * table lock.  IOCTL_ON_ISOLATE runs on QEMU's thread pool; without this a
 * concurrent nvkvm_handle_close() on the TX thread could close()+recycle the
 * host fd while a worker is mid-ioctl on it (use-after-close / wrong-object).
 * The dup is an independent fd referencing the SAME struct file, so the kernel
 * keeps that open file description alive for the whole ioctl regardless of what
 * happens to the original fd (the documented "blocking syscall holds a
 * reference and may complete" behaviour).  Caller MUST close() the returned fd.
 * Returns -1 if the handle is gone/closed.  *dev_id_out (optional) gets dev_id.
 */
int nvkvm_handle_acquire_fd(struct nvkvm_handle_table *t, uint32_t handle_id,
			    int *dev_id_out, uint64_t *generation_out)
{
	int dfd = -1;
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX)
		return -1;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (h->in_use && h->id == handle_id && h->fd >= 0) {
		dfd = fcntl(h->fd, F_DUPFD_CLOEXEC, 0);
		if (dev_id_out)
			*dev_id_out = h->dev_id;
		if (generation_out)
			*generation_out = h->generation;
	}
	pthread_mutex_unlock(&t->lock);
	return dfd;
}

int nvkvm_handle_acquire_fd_ref_isolate(struct nvkvm_handle_table *t,
					 uint32_t handle_id, int *dev_id_out,
					 uint64_t *generation_out)
{
	int dfd = -1;
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX)
		return -1;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (h->in_use && h->id == handle_id && h->fd >= 0) {
		dfd = fcntl(h->fd, F_DUPFD_CLOEXEC, 0);
		if (dfd >= 0) {
			h->isolate_refcount++;
			if (dev_id_out)
				*dev_id_out = h->dev_id;
			if (generation_out)
				*generation_out = h->generation;
		}
	}
	pthread_mutex_unlock(&t->lock);
	return dfd;
}

int nvkvm_handle_ref_isolate_generation(struct nvkvm_handle_table *t,
					 uint32_t handle_id, uint64_t generation)
{
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX)
		return -EBADF;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (!h->in_use || h->id != handle_id ||
	    (generation != 0 && h->generation != generation)) {
		pthread_mutex_unlock(&t->lock);
		return -EBADF;
	}
	h->isolate_refcount++;
	pthread_mutex_unlock(&t->lock);
	return 0;
}

int nvkvm_handle_ref_isolate(struct nvkvm_handle_table *t, uint32_t handle_id)
{
	return nvkvm_handle_ref_isolate_generation(t, handle_id, 0);
}

int nvkvm_handle_unref_isolate_generation(struct nvkvm_handle_table *t,
					   uint32_t handle_id, uint64_t generation)
{
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX)
		return -EBADF;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (!h->in_use || h->id != handle_id ||
	    (generation != 0 && h->generation != generation)) {
		pthread_mutex_unlock(&t->lock);
		return -EBADF;
	}
	if (h->isolate_refcount > 0)
		h->isolate_refcount--;
	pthread_mutex_unlock(&t->lock);
	return 0;
}

int nvkvm_handle_unref_isolate(struct nvkvm_handle_table *t, uint32_t handle_id)
{
	return nvkvm_handle_unref_isolate_generation(t, handle_id, 0);
}

int nvkvm_handle_close(struct nvkvm_handle_table *t, uint32_t handle_id)
{
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX)
		return -EBADF;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (!h->in_use || h->id != handle_id) {
		pthread_mutex_unlock(&t->lock);
		return -EBADF;
	}
	if (h->isolate_refcount > 0) {
		pthread_mutex_unlock(&t->lock);
		return -EBUSY;
	}
	if (h->fd >= 0) {
		close(h->fd);
		h->fd = -1;
	}
	h->in_use = false;
	pthread_mutex_unlock(&t->lock);
	return 0;
}

void nvkvm_handle_close_session(struct nvkvm_handle_table *t, uint32_t session_id)
{
	pthread_mutex_lock(&t->lock);
	for (int i = 1; i < NVKVM_HANDLE_MAX; i++) {
		struct nvkvm_handle *h = &t->handles[i];
		if (!h->in_use || h->session_id != session_id)
			continue;
		if (h->fd >= 0) {
			close(h->fd);
			h->fd = -1;
		}
		h->in_use = false;
	}
	pthread_mutex_unlock(&t->lock);
}
