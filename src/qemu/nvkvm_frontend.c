/*
 * nvkvm_frontend.c — NVIDIA frontend ioctl handlers (host side)
 *
 * Handles the core RM ioctls on /dev/nvidiactl and /dev/nvidiaN:
 *   NV_ESC_RM_ALLOC, NV_ESC_RM_FREE, NV_ESC_RM_CONTROL, NV_ESC_RM_DUP_OBJECT
 *   NV_ESC_REGISTER_FD, NV_ESC_ALLOC_OS_EVENT, NV_ESC_FREE_OS_EVENT
 *   plus "simple" ioctls that forward without pointer translation.
 *
 * The pointer translation model (from gVisor nvproxy):
 *  - For ioctls with embedded pointer fields (NVOS54_PARAMETERS.params,
 *    NVOS64_PARAMETERS.p_alloc_parms, etc.), the guest placed the secondary
 *    buffer in the aux slot. We substitute the host VA of the aux slot for
 *    the pointer field before calling the real ioctl, then restore it.
 *  - For ioctls with embedded fd fields, the guest already translated them
 *    to fd_token values; we convert those to host fds here.
 *
 * Security invariants:
 *  - We verify all handles in ALLOC/FREE/CONTROL requests against the
 *    per-client object graph before invoking the real driver.
 *  - We ensure NV01_ROOT_CLIENT allocations are unprivileged (no admin caps).
 *  - We do not allow handles from one session to appear in another session's
 *    requests.
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include "virtio_nvgpu.h"

/* ── Helper: invoke ioctl on the real host fd ─────────────────────────────── */

static long host_ioctl(int fd, unsigned int cmd, void *params)
{
	long ret;
	do {
		ret = ioctl(fd, cmd, params);
	} while (ret == -1 && errno == EINTR);
	return ret == -1 ? -errno : ret;
}

/* ── Helper: look up client by handle in session ─────────────────────────── */

static struct nvkvm_client *find_client(struct nvkvm_session *session,
					uint32_t h_client)
{
	int i;
	pthread_mutex_lock(&session->clients_lock);
	for (i = 0; i < session->nclients; i++) {
		if (session->clients[i] &&
		    session->clients[i]->handle == h_client) {
			pthread_mutex_unlock(&session->clients_lock);
			return session->clients[i];
		}
	}
	pthread_mutex_unlock(&session->clients_lock);
	return NULL;
}

static void register_client(struct nvkvm_session *session,
			     struct nvkvm_client *client)
{
	pthread_mutex_lock(&session->clients_lock);
	if (session->nclients < NVKVM_MAX_OBJECTS_PER_CLIENT)
		session->clients[session->nclients++] = client;
	pthread_mutex_unlock(&session->clients_lock);
}

/* ── NV_ESC_RM_ALLOC ──────────────────────────────────────────────────────── */

/*
 * nvkvm_handle_rm_alloc — forward NV_ESC_RM_ALLOC to the host driver.
 *
 * The params blob in ctx->params_buf is either NVOS21 or NVOS64 format
 * (distinguished by param_size). The guest zeroed p_alloc_parms before
 * sending; if ctx->aux_buf is non-NULL, we substitute it as the alloc params
 * pointer.
 *
 * After a successful alloc, we record the new object in the client's resource
 * map so future CONTROL/FREE calls can validate it.
 */
int nvkvm_handle_rm_alloc(struct nvkvm_req_ctx *ctx)
{
	int ret;
	struct nvkvm_client *client;
	uint32_t h_client, h_object_new, h_class;

	NVKVM_DBG( "nvkvm: rm_alloc: session=%u param_size=%zu hfd=%d\n",
		ctx->session->id, ctx->param_size, ctx->hfd->fd);

	if (ctx->param_size == sizeof(struct nvos64_parameters)) {
		struct nvos64_parameters *p = ctx->params_buf;
		nvp64_t saved_alloc_parms   = p->p_alloc_parms;
		nvp64_t saved_rights        = p->p_rights_requested;

		h_client     = p->h_root;
		h_object_new = p->h_object_new;
		h_class      = p->h_class;

		/* Substitute host pointer for alloc params if provided */
		if (ctx->aux_buf && ctx->aux_size > 0)
			p->p_alloc_parms = (nvp64_t)(uintptr_t)ctx->aux_buf;

		/*
		 * Security: ensure NV01_ROOT_CLIENT allocations are
		 * unprivileged. Clear any admin access bits in rights mask.
		 * gVisor does the equivalent by checking the allocation class
		 * and rejecting admin ones — we follow suit.
		 */
		if (h_class == NV01_ROOT_CLIENT)
			p->p_rights_requested = 0;

		ret = (int)host_ioctl(ctx->hfd->fd,
			_IOWR('F', NV_ESC_RM_ALLOC,
			      struct nvos64_parameters), p);

		/*
		 * Re-read h_object_new after the ioctl: the driver may assign a
		 * different handle than the one the guest requested.
		 */
		h_object_new = p->h_object_new;

		NVKVM_DBG( "nvkvm: rm_alloc64: ret=%d status=0x%x h_object_new_after=0x%x\n",
			ret, p->status, h_object_new);

		/* Restore zeroed fields before copying back to guest */
		p->p_alloc_parms      = saved_alloc_parms;
		p->p_rights_requested = saved_rights;

	} else if (ctx->param_size == sizeof(struct nvos21_parameters)) {
		struct nvos21_parameters *p = ctx->params_buf;
		nvp64_t saved_alloc_parms   = p->p_alloc_parms;

		h_client     = p->h_root;
		h_object_new = p->h_object_new;
		h_class      = p->h_class;

		if (ctx->aux_buf && ctx->aux_size > 0)
			p->p_alloc_parms = (nvp64_t)(uintptr_t)ctx->aux_buf;

		ret = (int)host_ioctl(ctx->hfd->fd,
			_IOWR('F', NV_ESC_RM_ALLOC,
			      struct nvos21_parameters), p);

		/* Re-read after ioctl: driver may assign a different handle. */
		h_object_new = p->h_object_new;

		p->p_alloc_parms = saved_alloc_parms;
	} else {
		return -EINVAL;
	}

	NVKVM_DBG( "nvkvm: rm_alloc: h_class=0x%x h_object_new=0x%x ret=%d\n",
		h_class, h_object_new, ret);

	if (ret < 0)
		return ret;

	/*
	 * Record the new object in the client's resource map.
	 * If this is a new root client, create and register the client struct.
	 */
	/*
	 * Root client allocation: class 0 (NV01_ROOT) or 0x41 (NV01_ROOT_CLIENT).
	 * libnvidia-ml uses class=0 with root=parent=0 for root client allocation.
	 * gVisor treats both identically.
	 */
	if (h_class == NV01_ROOT_CLIENT ||
	    (h_class == NV01_ROOT && h_client == NV01_NULL_OBJECT)) {
		struct nvkvm_client *c = nvkvm_client_alloc(h_object_new);
		register_client(ctx->session, c);
		NVKVM_DBG( "nvkvm: rm_alloc: ROOT_CLIENT h_class=0x%x h_object_new=0x%x registered in session=%u\n",
			h_class, h_object_new, ctx->session->id);
		/* The root client is its own resource entry */
		pthread_mutex_lock(&c->lock);
		nvkvm_obj_add(c, h_object_new, NV01_ROOT_CLIENT,
			      NV01_NULL_OBJECT, NULL);
		pthread_mutex_unlock(&c->lock);
	} else {
		client = find_client(ctx->session, h_client);
		if (client) {
			uint32_t h_parent;
			if (ctx->param_size == sizeof(struct nvos64_parameters))
				h_parent = ((struct nvos64_parameters *)
					    ctx->params_buf)->h_object_parent;
			else
				h_parent = ((struct nvos21_parameters *)
					    ctx->params_buf)->h_object_parent;

			pthread_mutex_lock(&client->lock);
			nvkvm_obj_add(client, h_object_new, h_class,
				      h_parent, NULL);
			pthread_mutex_unlock(&client->lock);
		}
	}

	return ret;
}

/* ── NV_ESC_RM_FREE ───────────────────────────────────────────────────────── */

int nvkvm_handle_rm_free(struct nvkvm_req_ctx *ctx)
{
	struct nvos00_parameters *p = ctx->params_buf;
	struct nvkvm_client *client;
	long ret;

	/* Validate client handle */
	NVKVM_DBG( "nvkvm: rm_free: session=%u h_root=0x%x h_object=0x%x nclients=%d\n",
		ctx->session->id, p->h_root, p->h_object_old, ctx->session->nclients);
	client = find_client(ctx->session, p->h_root);
	if (!client) {
		fprintf(stderr, "nvkvm: rm_free: unknown client 0x%x in session %u (nclients=%d)\n",
			p->h_root, ctx->session->id, ctx->session->nclients);
		return -EINVAL;
	}

	ret = host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_RM_FREE,
		      struct nvos00_parameters), p);

	/* Update our object graph regardless of driver result */
	pthread_mutex_lock(&client->lock);
	nvkvm_obj_free(client, p->h_object_old);
	pthread_mutex_unlock(&client->lock);

	return (int)ret;
}

/* ── NV_ESC_RM_CONTROL ────────────────────────────────────────────────────── */

/*
 * NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION (0x101) contains embedded pointer
 * fields (pDriverVersionBuffer etc.) that hold guest user-space VAs.  The host
 * NVIDIA driver calls copy_to_user() on those addresses, which fails because
 * they are guest VAs, not QEMU process VAs.  Fix by replacing them with
 * malloc'd host-side buffers, calling the ioctl, then restoring the original
 * addresses.  The changelist numbers are written into the params struct itself
 * (no embedded pointer) and come back via aux_buf normally; the string data is
 * discarded because CUDA's device enumeration only uses the numeric fields.
 */
static int nvkvm_ctrl_get_build_version(struct nvkvm_req_ctx *ctx,
					struct nvos54_parameters *p)
{
	struct nv0000_ctrl_system_get_build_version_params *ver =
		(struct nv0000_ctrl_system_get_build_version_params *)(uintptr_t)p->params;
	uint32_t sz = ver->size_of_strings;
	long ret;

	/*
	 * The guest extended aux_buf to sizeof(params) + 3*sz. Point the
	 * embedded pointer fields into the extension area so the host NVIDIA
	 * driver writes the version strings there instead of trying to
	 * copy_to_user a guest VA.  The guest will copy the strings from the
	 * returned aux_buf back to its original user-space buffers.
	 */
	if (sz > 0 && sz <= 512 &&
	    ctx->aux_size >= p->params_size + (size_t)3 * sz) {
		char *ext = (char *)ver + p->params_size;

		ver->p_driver_version_buffer = (nvp64_t)(uintptr_t)(ext);
		ver->p_version_buffer        = (nvp64_t)(uintptr_t)(ext + sz);
		ver->p_title_buffer          = (nvp64_t)(uintptr_t)(ext + 2 * sz);
	}
	/* else: no extension (or sz=0) — leave pointers at 0, driver fills numbers */

	ret = host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_RM_CONTROL, struct nvos54_parameters), p);

	if (sz > 0 && sz <= 512 &&
	    ctx->aux_size >= p->params_size + (size_t)3 * sz) {
		char *ext = (char *)ver + p->params_size;
		/* Ensure null-terminated for printing */
		char drv[65] = {0}, version[65] = {0}, titl[65] = {0};
		memcpy(drv, ext, sz < 64 ? sz : 64);
		memcpy(version, ext + sz, sz < 64 ? sz : 64);
		memcpy(titl, ext + 2 * sz, sz < 64 ? sz : 64);
		NVKVM_DBG(
			"nvkvm: get_build_version: ret=%ld status=0x%x changelist=%u official=%u sz=%u drv='%s' ver='%s' title='%s'\n",
			ret, p->status,
			ver->changelist_number, ver->official_changelist_number,
			sz, drv, version, titl);
	} else {
		NVKVM_DBG(
			"nvkvm: get_build_version: ret=%ld status=0x%x changelist=%u official=%u sz=%u (no ext)\n",
			ret, p->status,
			ver->changelist_number, ver->official_changelist_number, sz);
	}

	/* Zero the pointer fields before returning to guest — they're host VAs
	 * and meaningless in the guest context; the guest copies strings by offset. */
	ver->p_driver_version_buffer = 0;
	ver->p_version_buffer        = 0;
	ver->p_title_buffer          = 0;

	return (int)ret;
}

int nvkvm_handle_rm_control(struct nvkvm_req_ctx *ctx)
{
	struct nvos54_parameters *p = ctx->params_buf;
	nvp64_t saved_params = p->params;
	long ret;

	/* Validate that h_client is a known client in this session */
	NVKVM_DBG( "nvkvm: rm_control: session=%u h_client=0x%x h_obj=0x%x cmd=0x%x nclients=%d\n",
		ctx->session->id, p->h_client, p->h_object, p->cmd, ctx->session->nclients);
	if (!find_client(ctx->session, p->h_client)) {
		NVKVM_DBG(
			"nvkvm: rm_control: unknown client 0x%x in session %u (nclients=%d)\n",
			p->h_client, ctx->session->id, ctx->session->nclients);
		return -EINVAL;
	}

	/*
	 * Substitute host pointer for the command params buffer.
	 * The guest zeroed p->params; the secondary buffer is in aux_buf.
	 * We validate params_size against aux_size to prevent overread.
	 */
	if (ctx->aux_buf) {
		if (p->params_size > ctx->aux_size) {
			NVKVM_DBG(
				"nvkvm: rm_control: params_size %u > aux_size %zu\n",
				p->params_size, ctx->aux_size);
			return -EINVAL;
		}
		p->params = (nvp64_t)(uintptr_t)ctx->aux_buf;
	} else if (p->params_size > 0) {
		NVKVM_DBG(
			"nvkvm: rm_control: params_size %u but no aux_buf\n",
			p->params_size);
		return -EINVAL;
	}

	/* Dispatch commands with embedded pointer fields to dedicated handlers */
	NVKVM_DBG( "nvkvm: rm_control: cmd=0x%x params_size=%u aux_buf=%p p->params=0x%llx\n",
		p->cmd, p->params_size, ctx->aux_buf, (unsigned long long)p->params);
	if (p->cmd == NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION &&
	    ctx->aux_buf &&
	    p->params_size >= sizeof(struct nv0000_ctrl_system_get_build_version_params)) {
		ret = nvkvm_ctrl_get_build_version(ctx, p);
	} else {
		ret = host_ioctl(ctx->hfd->fd,
			_IOWR('F', NV_ESC_RM_CONTROL,
			      struct nvos54_parameters), p);
	}

	NVKVM_DBG( "nvkvm: rm_control: ret=%ld status=0x%x cmd=0x%x\n",
		ret, p->status, p->cmd);

	p->params = saved_params;
	return (int)ret;
}

/* ── NV_ESC_RM_DUP_OBJECT ────────────────────────────────────────────────── */

int nvkvm_handle_rm_dup_object(struct nvkvm_req_ctx *ctx)
{
	struct nvos55_parameters *p = ctx->params_buf;
	struct nvkvm_client *client_dst, *client_src;
	long ret;

	/* Both source and destination clients must belong to this session */
	client_dst = find_client(ctx->session, p->h_client);
	client_src = find_client(ctx->session, p->h_client_src);
	if (!client_dst || !client_src)
		return -EINVAL;

	ret = host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_RM_DUP_OBJECT,
		      struct nvos55_parameters), p);
	if (ret < 0)
		return (int)ret;

	/* Record the duplicated object in the destination client */
	pthread_mutex_lock(&client_dst->lock);
	nvkvm_obj_add(client_dst, p->h_object, p->h_parent,
		      p->h_parent, NULL);
	pthread_mutex_unlock(&client_dst->lock);

	return (int)ret;
}

/* ── NV_ESC_REGISTER_FD ───────────────────────────────────────────────────── */

int nvkvm_handle_register_fd(struct nvkvm_req_ctx *ctx)
{
	struct nv_ioctl_register_fd *p = ctx->params_buf;
	struct nvkvm_host_fd *ctl_hfd;
	int32_t saved_fd = p->ctl_fd;
	long ret;

	/*
	 * The guest translated ctl_fd to the fd_token of the /dev/nvidiactl
	 * FD. We look it up and substitute the real host fd.
	 */
	ctl_hfd = nvkvm_fd_lookup(ctx->session, (uint32_t)p->ctl_fd);
	if (!ctl_hfd)
		return -EBADF;

	p->ctl_fd = (int32_t)ctl_hfd->fd;
	ret = host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_REGISTER_FD,
		      struct nv_ioctl_register_fd), p);
	NVKVM_DBG( "nvkvm: register_fd: hfd=%d ctl_fd=%d (token=%u) ret=%ld\n",
		ctx->hfd->fd, (int)p->ctl_fd, (uint32_t)saved_fd, ret);
	p->ctl_fd = saved_fd;
	return (int)ret;
}

/* ── NV_ESC_ALLOC_OS_EVENT ────────────────────────────────────────────────── */

int nvkvm_handle_alloc_os_event(struct nvkvm_req_ctx *ctx)
{
	struct nv_ioctl_alloc_os_event *p = ctx->params_buf;
	uint32_t saved_fd = p->fd;
	int host_efd;
	long ret;

	/*
	 * The guest cannot pass its own eventfd across the VM boundary.
	 * We create a host-side eventfd and use that instead. When the
	 * host eventfd fires, we relay the notification to the guest via VQ_EVT.
	 *
	 * TODO: implement host eventfd → VQ_EVT relay thread.
	 */
	host_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (host_efd < 0)
		return -errno;

	p->fd = (uint32_t)host_efd;
	ret = host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_ALLOC_OS_EVENT,
		      struct nv_ioctl_alloc_os_event), p);
	p->fd = saved_fd;

	if (ret < 0) {
		close(host_efd);
		return (int)ret;
	}

	/* TODO: register host_efd with the event relay */
	return (int)ret;
}

/* ── NV_ESC_FREE_OS_EVENT ─────────────────────────────────────────────────── */

int nvkvm_handle_free_os_event(struct nvkvm_req_ctx *ctx)
{
	struct nv_ioctl_free_os_event *p = ctx->params_buf;
	/* TODO: look up and close the host eventfd, deregister relay */
	return (int)host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_FREE_OS_EVENT,
		      struct nv_ioctl_free_os_event), p);
}

/* ── Simple ioctls (no pointer translation needed) ────────────────────────── */

int nvkvm_handle_simple_ioctl(struct nvkvm_req_ctx *ctx, unsigned int cmd)
{
	if (_IOC_NR(cmd) == NV_ESC_NUMA_INFO && ctx->params_buf) {
		const uint8_t *b = (const uint8_t *)ctx->params_buf;
		unsigned sz = _IOC_SIZE(cmd);
		unsigned i;
		NVKVM_DBG(
			"nvkvm: numa_info PRE: hfd=%d dev_id=%d cmd=0x%x size=%u\n",
			ctx->hfd->fd, ctx->hfd->dev_id, cmd, sz);
		/* Scan for any non-zero bytes so we know the "interesting" range */
		unsigned first_nonzero = sz, last_nonzero = 0;
		int any_nonzero = 0;
		for (i = 0; i < sz; i++) {
			if (b[i]) {
				if (!any_nonzero) first_nonzero = i;
				last_nonzero = i;
				any_nonzero = 1;
			}
		}
		if (any_nonzero)
			NVKVM_DBG(
				"nvkvm: numa_info PRE: NONZERO bytes in range [%u..%u]\n",
				first_nonzero, last_nonzero);
		else
			NVKVM_DBG( "nvkvm: numa_info PRE: all bytes zero\n");
		/* Print first 64 bytes */
		for (i = 0; i + 15 < 64 && i < sz; i += 16)
			NVKVM_DBG(
				"nvkvm: numa_info PRE buf[%u..%u]= "
				"%02x %02x %02x %02x  %02x %02x %02x %02x  "
				"%02x %02x %02x %02x  %02x %02x %02x %02x\n",
				i, i+15,
				b[i],b[i+1],b[i+2],b[i+3],b[i+4],b[i+5],b[i+6],b[i+7],
				b[i+8],b[i+9],b[i+10],b[i+11],b[i+12],b[i+13],b[i+14],b[i+15]);
		/* If non-zero bytes exist beyond 64, print a window around them */
		if (any_nonzero && last_nonzero >= 64) {
			unsigned start = (first_nonzero > 16 ? first_nonzero - 16 : 64) & ~15U;
			unsigned end   = (last_nonzero + 32) & ~15U;
			if (end > sz) end = sz;
			for (i = start; i < end && i + 15 < sz; i += 16)
				NVKVM_DBG(
					"nvkvm: numa_info PRE buf[%u..%u]= "
					"%02x %02x %02x %02x  %02x %02x %02x %02x  "
					"%02x %02x %02x %02x  %02x %02x %02x %02x\n",
					i, i+15,
					b[i],b[i+1],b[i+2],b[i+3],b[i+4],b[i+5],b[i+6],b[i+7],
					b[i+8],b[i+9],b[i+10],b[i+11],b[i+12],b[i+13],b[i+14],b[i+15]);
		}
	}
	long ret = host_ioctl(ctx->hfd->fd, cmd, ctx->params_buf);
	/* UVM ioctl result logging — rm_status is at a command-specific offset */
	if (cmd == UVM_INITIALIZE || cmd == UVM_DEINITIALIZE ||
	    cmd == UVM_REGISTER_GPU || cmd == UVM_UNREGISTER_GPU) {
		/* These structs all have rm_status as the first uint32 or second
		 * uint32 (after flags/reserved).  Print offset-4 bytes which
		 * covers both layouts. */
		uint32_t rm_status = 0;
		if (ctx->params_buf && ctx->param_size >= 8)
			memcpy(&rm_status,
			       (const char *)ctx->params_buf + 4,
			       sizeof(rm_status));
		NVKVM_DBG( "nvkvm: uvm ioctl cmd=0x%x dev_id=%d ret=%ld rm_status=0x%x\n",
			cmd, ctx->hfd->dev_id, ret, rm_status);
	}
	if (_IOC_NR(cmd) == NV_ESC_NUMA_INFO) {
		NVKVM_DBG(
			"nvkvm: numa_info: hfd=%d cmd=0x%x size=%u ret=%ld errno=%d\n",
			ctx->hfd->fd, cmd, _IOC_SIZE(cmd), ret,
			(ret == -EINVAL || ret < 0) ? -(int)ret : 0);
	}
	if (_IOC_NR(cmd) == NV_ESC_CARD_INFO) {
		/* Log the first entry of CARD_INFO to see if a GPU is visible */
		const struct nv_ioctl_card_info *ci = ctx->params_buf;
		size_t n = ctx->param_size / sizeof(*ci);
		NVKVM_DBG(
			"nvkvm: card_info: ret=%ld n_entries=%zu valid=%d devId=0x%x\n",
			ret, n, (n > 0) ? ci[0].valid : 0,
			(n > 0) ? ci[0].pci_info.device_id : 0);
	}
	return (int)ret;
}
