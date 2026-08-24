/*
 * nvkvm_dispatch.c — ioctl dispatch table and param size table (host side)
 *
 * Maps ioctl command → handler and expected parameter size.
 * Mirrors gVisor's driverABI dispatch tables in version.go.
 *
 * All sizes are validated before any shared memory access. Unknown ioctls
 * return -ENOTTY. Ioctls with wrong sizes return -EINVAL.
 */

#include "qemu/osdep.h"
#include <errno.h>
#include <sys/ioctl.h>

#include "virtio_nvgpu.h"

#define NV_IOC_NR(cmd)  _IOC_NR(cmd)

/* Audit G-2: hard cap on NV_ESC_RM_IDLE_CHANNELS array length so a malicious
 * guest can neither overflow the aux-size check nor demand an unbounded read. */
#define NVKVM_IDLE_MAX_CHANNELS  4096u

/* ── Expected parameter sizes ─────────────────────────────────────────────── */

size_t nvkvm_ioctl_expected_param_size(unsigned int cmd,
                                       const struct nvkvm_abi_profile *prof)
{
	if (!prof)
		prof = nvkvm_abi_by_id(NVKVM_ABI_570);
	/* UVM full-word commands */
	switch (cmd) {
	case UVM_INITIALIZE:
		return sizeof(struct uvm_initialize_params);
	case UVM_DEINITIALIZE:
		return sizeof(struct uvm_deinitialize_params);
	case UVM_MM_INITIALIZE:
		return sizeof(struct uvm_mm_initialize_params);
	case UVM_REGISTER_GPU:
		return sizeof(struct uvm_register_gpu_params);
	case UVM_UNREGISTER_GPU:
		return sizeof(struct uvm_unregister_gpu_params);
	case UVM_REGISTER_GPU_VASPACE:
		return sizeof(struct uvm_register_gpu_vaspace_params);
	case UVM_UNREGISTER_GPU_VASPACE:
		return sizeof(struct uvm_unregister_gpu_vaspace_params);
	case UVM_REGISTER_CHANNEL:
		return sizeof(struct uvm_register_channel_params);
	case UVM_UNREGISTER_CHANNEL:
		return sizeof(struct uvm_unregister_channel_params);
	case UVM_CREATE_RANGE_GROUP:
		return sizeof(struct uvm_create_range_group_params);
	case UVM_DESTROY_RANGE_GROUP:
		return sizeof(struct uvm_destroy_range_group_params);
	case UVM_SET_RANGE_GROUP:
		return sizeof(struct uvm_set_range_group_params);
	case UVM_MAP_EXTERNAL_ALLOCATION:
		/* #81: version-variant (9264 V550+, 1200 pre-V550). The C
		 * struct hardcodes V550; take the size from the profile or a
		 * 535 guest's legitimate 1200-byte call is rejected EINVAL by
		 * the exact-size check in virtio_nvgpu.c. */
		return prof->uvm_map_ext_size;
	case UVM_FREE:
		return sizeof(struct uvm_free_params);
	case UVM_MIGRATE:
		return sizeof(struct uvm_migrate_params);
	case UVM_SET_PREFERRED_LOCATION:
		return sizeof(struct uvm_set_preferred_location_params);
	case UVM_UNSET_PREFERRED_LOCATION:
		return sizeof(struct uvm_unset_preferred_location_params);
	case UVM_SET_ACCESSED_BY:
		return sizeof(struct uvm_set_accessed_by_params);
	case UVM_UNSET_ACCESSED_BY:
		return sizeof(struct uvm_unset_accessed_by_params);
	case UVM_ENABLE_PEER_ACCESS:
		return sizeof(struct uvm_enable_peer_access_params);
	case UVM_DISABLE_PEER_ACCESS:
		return sizeof(struct uvm_disable_peer_access_params);
	case UVM_CREATE_EXTERNAL_RANGE:
		return sizeof(struct uvm_create_external_range_params);
	case UVM_VALIDATE_VA_RANGE:
		return sizeof(struct uvm_validate_va_range_params);
	case UVM_PAGEABLE_MEM_ACCESS:
		return sizeof(struct uvm_pageable_mem_access_params);
	case UVM_PAGEABLE_MEM_ACCESS_ON_GPU:
		return sizeof(struct uvm_pageable_mem_access_on_gpu_params);
	case UVM_ALLOC_SEMAPHORE_POOL:
		/* #81: version-variant (9248 V550+, 1184 pre-V550). */
		return prof->uvm_sem_pool_size;
	}

	/* Frontend ioctls — IOC_NR dispatch */
	switch (NV_IOC_NR(cmd)) {
	case NV_ESC_CARD_INFO: {
		size_t sz = _IOC_SIZE(cmd);
		if (!sz || sz > sizeof(struct nv_ioctl_card_info) *
		    NV_IOCTL_CARD_INFO_MAX_ENTRIES)
			return (size_t)-1;
		return sz;
	}
	case NV_ESC_REGISTER_FD:
		return sizeof(struct nv_ioctl_register_fd);
	case NV_ESC_ALLOC_OS_EVENT:
		return sizeof(struct nv_ioctl_alloc_os_event);
	case NV_ESC_FREE_OS_EVENT:
		return sizeof(struct nv_ioctl_free_os_event);
	case NV_ESC_CHECK_VERSION_STR:
		return sizeof(struct nv_ioctl_rm_api_version);
	case NV_ESC_SYS_PARAMS:
		return sizeof(struct nv_ioctl_sys_params);
	case NV_ESC_NUMA_INFO: {
		/* Struct grew in newer drivers — accept whatever size the ioctl encodes */
		size_t sz = _IOC_SIZE(cmd);
		return sz ? sz : (size_t)-1;
	}
	case NV_ESC_WAIT_OPEN_COMPLETE:
		return sizeof(struct nv_ioctl_wait_open_complete);
	case NV_ESC_RM_ALLOC_MEMORY:
		return sizeof(struct nv_ioctl_nvos02_parameters_with_fd);
	case NV_ESC_RM_FREE:
		return sizeof(struct nvos00_parameters);
	case NV_ESC_RM_CONTROL:
		return sizeof(struct nvos54_parameters);
	case NV_ESC_RM_ALLOC: {
		size_t sz = _IOC_SIZE(cmd);
		if (sz == sizeof(struct nvos21_parameters) ||
		    sz == sizeof(struct nvos64_parameters))
			return sz;
		return (size_t)-1;
	}
	case NV_ESC_RM_DUP_OBJECT:
		return sizeof(struct nvos55_parameters);
	case NV_ESC_RM_SHARE:
		return sizeof(struct nvos57_parameters);
	case NV_ESC_RM_VID_HEAP_CONTROL:
		return sizeof(struct nvos32_parameters);
	case NV_ESC_RM_MAP_MEMORY:
		return sizeof(struct nv_ioctl_nvos33_parameters_with_fd);
	case NV_ESC_RM_UNMAP_MEMORY:
		return sizeof(struct nv_ioctl_nvos34_parameters);
	case NV_ESC_RM_MAP_MEMORY_DMA:
		return sizeof(struct nvos46_parameters);
	case NV_ESC_RM_UNMAP_MEMORY_DMA:
		return sizeof(struct nvos47_parameters);
	case NV_ESC_RM_IDLE_CHANNELS:
		return sizeof(struct nv_ioctl_idle_channels);
	case NV_ESC_RM_ALLOC_CONTEXT_DMA2:
		return sizeof(struct nv_ioctl_alloc_context_dma2);
	case NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO:
		return sizeof(struct nvos56_parameters);
	case NV_ESC_EXPORT_TO_DMABUF_FD:
		return sizeof(struct nv_ioctl_export_to_dmabuf_fd);
	}

	return (size_t)-1;
}

/* ── Main dispatch ────────────────────────────────────────────────────────── */

int nvkvm_dispatch_ioctl(struct nvkvm_req_ctx *ctx, unsigned int cmd)
{
	/* UVM ioctls — simple passthrough (no embedded pointers in most) */
	switch (cmd) {
	case UVM_INITIALIZE:
	case UVM_DEINITIALIZE:
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	/*
	 * UVM_MM_INITIALIZE: links the secondary MM fd to the primary UVM fd.
	 * uvm_fd contains the fd_token of the primary UVM open (from the guest
	 * sanitizer); translate to the real host fd before forwarding.
	 */
	case UVM_MM_INITIALIZE: {
		/*
		 * The guest now puts the secondary UVM fd's handle_id in
		 * p->uvm_fd (translated by the guest sanitizer for the isolate
		 * path).  If we end up here on the legacy path (no isolate),
		 * resolve handle_id → host_fd via the global handle table.
		 */
		struct uvm_mm_initialize_params *p = ctx->params_buf;
		struct nvkvm_handle *uvm_h =
			nvkvm_handle_get(&ctx->nv->handles, (uint32_t)p->uvm_fd);
		if (!uvm_h || uvm_h->fd < 0) {
			NVKVM_DBG(
				"nvkvm: UVM_MM_INITIALIZE: handle_id %d not found\n",
				p->uvm_fd);
			return -EBADF;
		}
		int saved = p->uvm_fd;
		p->uvm_fd = (int32_t)uvm_h->fd;
		int ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		NVKVM_DBG(
			"nvkvm: UVM_MM_INITIALIZE (legacy): handle_id=%d host_fd=%d "
			"ret=%d rm_status=0x%x\n",
			saved, uvm_h->fd, ret, p->rm_status);
		p->uvm_fd = saved;
		return ret;
	}

	case UVM_CREATE_RANGE_GROUP:
	case UVM_DESTROY_RANGE_GROUP:
	case UVM_FREE:
	case UVM_SET_PREFERRED_LOCATION:
	case UVM_UNSET_PREFERRED_LOCATION:
	case UVM_ENABLE_READ_DUPLICATION:
	case UVM_DISABLE_READ_DUPLICATION:
	case UVM_SET_ACCESSED_BY:
	case UVM_UNSET_ACCESSED_BY:
	case UVM_ENABLE_PEER_ACCESS:
	case UVM_DISABLE_PEER_ACCESS:
	case UVM_CREATE_EXTERNAL_RANGE:
	case UVM_VALIDATE_VA_RANGE:
	case UVM_PAGEABLE_MEM_ACCESS:
	case UVM_PAGEABLE_MEM_ACCESS_ON_GPU:
	case UVM_MIGRATE:
	case UVM_MIGRATE_RANGE_GROUP:
	case UVM_ALLOC_SEMAPHORE_POOL:
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	/*
	 * UVM channel/vaspace registration: rm_ctrl_fd contains an fd_token
	 * (assigned by QEMU to the guest) that must be translated to the real
	 * host fd before the UVM driver sees it.  We zero the field on return
	 * so the guest cannot read back real host fd numbers from shared memory.
	 */
	/*
	 * Legacy fallback paths for UVM with embedded fd fields.  Guest now
	 * sends handle_ids (not fd_tokens) for these.  Used only when the
	 * isolate path isn't available; the isolate path translates in the
	 * stub instead.
	 */
	case UVM_REGISTER_GPU_VASPACE: {
		struct uvm_register_gpu_vaspace_params *p = ctx->params_buf;
		struct nvkvm_handle *ctrl_h =
			nvkvm_handle_get(&ctx->nv->handles, (uint32_t)p->rm_ctrl_fd);
		if (!ctrl_h || ctrl_h->fd < 0) return -EBADF;
		nvhandle_t saved = p->rm_ctrl_fd;
		p->rm_ctrl_fd = (nvhandle_t)ctrl_h->fd;
		int ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		p->rm_ctrl_fd = saved;
		return ret;
	}
	case UVM_UNREGISTER_GPU_VASPACE:
		/* no rm_ctrl_fd in this struct — plain passthrough */
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	case UVM_REGISTER_CHANNEL: {
		struct uvm_register_channel_params *p = ctx->params_buf;
		struct nvkvm_handle *ctrl_h =
			nvkvm_handle_get(&ctx->nv->handles, (uint32_t)p->rm_ctrl_fd);
		if (!ctrl_h || ctrl_h->fd < 0) return -EBADF;
		nvhandle_t saved = p->rm_ctrl_fd;
		p->rm_ctrl_fd = (nvhandle_t)ctrl_h->fd;
		int ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		p->rm_ctrl_fd = saved;
		return ret;
	}
	case UVM_UNREGISTER_CHANNEL:
		/* gVisor's UVM_UNREGISTER_CHANNEL_PARAMS has no rm_ctrl_fd
		 * — driver figures it out from hClient/hChannel.  No
		 * translation needed; pass through. */
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	case UVM_REGISTER_GPU:
	case UVM_UNREGISTER_GPU:
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	case UVM_MAP_EXTERNAL_ALLOCATION: {
		/* #81: rm_ctrl_fd's OFFSET is version-variant (1184 pre-V550,
		 * 9248 V550+); the C struct hardcodes V550, so p->rm_ctrl_fd
		 * reads 9248 on every driver and lands past the end of a 535
		 * guest's 1200-byte struct. Index by profile byte offset. */
		const struct nvkvm_abi_profile *prof =
			ctx->nv->abi ? ctx->nv->abi : nvkvm_abi_by_id(NVKVM_ABI_570);
		unsigned off = prof->uvm_map_ext_fd_off;
		nvhandle_t *fdp;
		struct nvkvm_handle *ctrl_h;
		nvhandle_t saved;
		int ret;

		if (off + sizeof(*fdp) > ctx->param_size)
			return -EINVAL;
		fdp = (nvhandle_t *)((char *)ctx->params_buf + off);
		ctrl_h = nvkvm_handle_get(&ctx->nv->handles, (uint32_t)*fdp);
		if (!ctrl_h || ctrl_h->fd < 0) return -EBADF;
		saved = *fdp;
		*fdp = (nvhandle_t)ctrl_h->fd;
		ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		*fdp = saved;
		return ret;
	}
	}

	/*
	 * Guard the NR-based dispatch below: UVM cmds use _IOC_TYPE == 0 and
	 * their NRs can collide with NV_ESC_* frontend NRs (e.g. UVM 0x27 ==
	 * NV_ESC_RM_ALLOC_MEMORY).  Any unrecognised UVM cmd should fall
	 * through to ENOTTY, not get reinterpreted as a frontend ioctl.
	 */
	if (_IOC_TYPE(cmd) != 'F')
		return -ENOTTY;

	/* Frontend ioctls — IOC_NR dispatch */
	switch (NV_IOC_NR(cmd)) {
	case NV_ESC_RM_ALLOC:
		return nvkvm_handle_rm_alloc(ctx);
	case NV_ESC_RM_FREE:
		return nvkvm_handle_rm_free(ctx);
	case NV_ESC_RM_CONTROL:
		return nvkvm_handle_rm_control(ctx);
	case NV_ESC_RM_DUP_OBJECT:
		return nvkvm_handle_rm_dup_object(ctx);
	case NV_ESC_REGISTER_FD:
		return nvkvm_handle_register_fd(ctx);
	case NV_ESC_ALLOC_OS_EVENT:
		return nvkvm_handle_alloc_os_event(ctx);
	case NV_ESC_FREE_OS_EVENT:
		return nvkvm_handle_free_os_event(ctx);

	/* Simple ioctls — no embedded pointers, pass directly through */
	case NV_ESC_CARD_INFO:
	case NV_ESC_CHECK_VERSION_STR:
	case NV_ESC_SYS_PARAMS:
	case NV_ESC_NUMA_INFO:
	case NV_ESC_WAIT_OPEN_COMPLETE:
	case NV_ESC_ATTACH_GPUS_TO_FD:
	case NV_ESC_RM_SHARE:
	case NV_ESC_RM_VID_HEAP_CONTROL:
	case NV_ESC_RM_MAP_MEMORY_DMA:
	case NV_ESC_RM_UNMAP_MEMORY_DMA:
	case NV_ESC_RM_ALLOC_CONTEXT_DMA2:
	case NV_ESC_EXPORT_TO_DMABUF_FD:
	case NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO:
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	/*
	 * RM_MAP_MEMORY / RM_UNMAP_MEMORY: contain pointer fields that the
	 * guest zeroed. The host driver returns a valid host VA in p_linear_address
	 * after the mmap; we zero it again before returning to the guest since
	 * the guest uses its own GPA-based mapping established via NVKVM_REQ_MMAP.
	 */
	case NV_ESC_RM_MAP_MEMORY: {
		struct nv_ioctl_nvos33_parameters_with_fd *p = ctx->params_buf;
		int ret;
		/* Translate handle_id → host fd */
		if (p->fd >= 0) {
			struct nvkvm_handle *h =
				nvkvm_handle_get(&ctx->nv->handles,
						 (uint32_t)p->fd);
			if (!h || h->fd < 0) return -EBADF;
			p->fd = (int32_t)h->fd;
		}
		ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		p->p_linear_address = 0;  /* guest must not use host VA */
		return ret;
	}
	case NV_ESC_RM_UNMAP_MEMORY:
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	case NV_ESC_RM_ALLOC_MEMORY: {
		struct nv_ioctl_nvos02_parameters_with_fd *p = ctx->params_buf;
		int ret;
		if (p->fd >= 0) {
			struct nvkvm_handle *h =
				nvkvm_handle_get(&ctx->nv->handles,
						 (uint32_t)p->fd);
			if (!h || h->fd < 0) return -EBADF;
			p->fd = (int32_t)h->fd;
		}
		ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		/*
		 * NOT A SECURITY CONTROL, and specifically not the one for
		 * NV01_MEMORY_SYSTEM_OS_DESCRIPTOR (hClass 0x71).  Nothing calls
		 * this function: its only caller, handle_ioctl(), was itself
		 * dead and has been removed (A-1).  This file survives only
		 * because tests/unit/test_dispatch.c is built against it.
		 *
		 * The live path never inspects p_memory at all.  The gate that
		 * does is in nvkvm_req_ioctl_on_isolate()
		 * (nvkvm_isolate_handlers.c, "A-1"), which requires the range to
		 * be one the host itself installed in that isolate.  Read that,
		 * not this.
		 */
		p->p_memory = 0;
		return ret;
	}

	case NV_ESC_RM_IDLE_CHANNELS: {
		/* NOTE (audit P2-1): this dispatch path is NOT wired into the
		 * live IOCTL_ON_ISOLATE flow (handle_ioctl is static/unused).
		 * The authoritative IDLE_CHANNELS pointer-sanitisation lives in
		 * the stub (nvkvm_stub.c, nr 0x41 block).  Kept here for the
		 * (currently dead) synchronous path; do not rely on it. */
		struct nv_ioctl_idle_channels *p = ctx->params_buf;
		uint32_t n  = p->num_channels;
		int ret;
		/*
		 * Audit G-2: the boundary (not the untrusted guest) must ensure
		 * no guest pointer is ever forwarded.  Always overwrite the
		 * p_* fields: when num_channels>0 they point into the aux slot
		 * (3 arrays of n handles), otherwise they are zeroed.  Use
		 * 64-bit math and a hard cap so a malicious guest cannot
		 * overflow the size check (3*n*sizeof in 32-bit wraps for
		 * n≈0x55555556, which would push p_devices/p_channels past the
		 * aux buffer → host OOB read).
		 */
		if (n > 0) {
			size_t needed = (size_t)3u * n * sizeof(nvhandle_t);
			if (n > NVKVM_IDLE_MAX_CHANNELS ||
			    !ctx->aux_buf || ctx->aux_size < needed)
				return -EINVAL;
			p->p_clients  = (nvp64_t)(uintptr_t)ctx->aux_buf;
			p->p_devices  = p->p_clients + (size_t)n * sizeof(nvhandle_t);
			p->p_channels = p->p_devices + (size_t)n * sizeof(nvhandle_t);
		} else {
			p->p_clients = p->p_devices = p->p_channels = 0;
		}
		ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		p->p_clients = p->p_devices = p->p_channels = 0;
		return ret;
	}
	}

	return -ENOTTY;
}
