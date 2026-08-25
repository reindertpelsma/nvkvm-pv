// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_uvm_ext.c — managed-memory fallback: the guest downgrades a UVM
 * *managed* range to an *external* range over an RM system-memory object.
 *
 * WHY THERE HAS TO BE A FALLBACK
 * ------------------------------
 * A UVM managed range's GPU VA *is* the CPU VA of the VMA that created it
 * (uvm_va_range.c:224 -> uvm_va_block.c:1308 -> :7738-7769), and the guest's
 * kernels dereference the GUEST's pointer, inside kernel launch parameters,
 * never inside an ioctl.  So there is no ioctl translation that can rescue a
 * forwarded `mmap(/dev/nvidia-uvm)`: whatever address QEMU maps at is the
 * address the GPU gets, and it is not the guest's.  The full measurement
 * record, including the three escape routes that are closed at source, is
 * docs/internal/uvm-va-decoupling.md.
 *
 * WHAT THIS DOES INSTEAD
 * ----------------------
 * An EXTERNAL range's address is an *ioctl argument*, so it can simply be the
 * guest's `G`.  On a UVM mmap that no range-creating ioctl asked for — which is
 * exactly "libcuda is creating a managed range", because managed memory is the
 * one UVM range kind with no creating ioctl (uvm.c:743-757) — this module:
 *
 *   1. allocates an ordinary RM sysmem object (NV01_MEMORY_SYSTEM, 0x3e) on a
 *      private client/device tree of its own;
 *   2. arms a CPU mapping of it with NV_ESC_RM_MAP_MEMORY and pulls those exact
 *      pages into the guest at `G` through the existing sparse-window path;
 *   3. publishes the SAME object to the GPU at `G` with
 *      UVM_CREATE_EXTERNAL_RANGE + UVM_MAP_EXTERNAL_ALLOCATION.
 *
 * One object, three views — guest CPU at G, host RAM, GPU at G — and no copy.
 *
 * WHY THIS IS A GUEST-ONLY CHANGE
 * -------------------------------
 * Every ioctl here already exists and is already gated, and QEMU is never told
 * that anything unusual is happening:
 *
 *   - class 0x3e is in the alloc allowlist (nvkvm_fe_alloc_allowlist.h) and RM
 *     allocates the pages, so no address is named by anyone — this is not the
 *     OS-descriptor path (0x71), which pins the *caller's* pages;
 *   - UVM 73 (CREATE_EXTERNAL_RANGE) and 33 (MAP_EXTERNAL_ALLOCATION) are
 *     already in the U-6 schema with va_mode CREATE and USE, so the ownership
 *     table records the range exactly as designed and needs no special case;
 *   - the CPU mapping is an ordinary /dev/nvidiactl mmap.  QEMU's UVM branch is
 *     degenerate (it hands out anonymous window pages and never touches the
 *     driver), so we do not use it: we ask for the mapping against an
 *     *nvidiactl* handle, which takes QEMU's working in_window branch and does
 *     a real zero-copy mmap of the device fd.  The one substitution is which
 *     handle_id the request names; nothing in QEMU ties a handle to the fd
 *     whose ->mmap() is running.
 *
 * WHAT IT IS NOT
 * --------------
 * This is pinned host memory mapped to the GPU — cudaHostAlloc +
 * cudaHostGetDevicePointer semantics.  It is NOT migrating or oversubscribing
 * managed memory: UVM_MIGRATE (51), SET_PREFERRED_LOCATION / SET_ACCESSED_BY
 * (42/46) and read-duplication (44/45) are all managed-only and return
 * NV_ERR_INVALID_ADDRESS on an external range.  See
 * docs/internal/known-limitations.md.
 */

#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include "nvkvm.h"
#include "../../src/abi/uvm.h"

/* Frontend ioctl command words.  The driver reads only _IOC_NR and _IOC_SIZE
 * out of these, and the isolate gates on _IOC_NR. */
#define FE_CMD(nr, type)  _IOWR('F', (nr), type)

/* RM object handles are chosen by RM when we pass 0 (serverAllocResource
 * generates one), which is how we avoid guessing a free handle in a namespace
 * we do not own.  Measured: tools/uvm_sysmem_probe.c. */
#define RM_HANDLE_LET_RM_CHOOSE 0u

/* 'nvkm' — the heap owner tag RM records against our allocations.  Any nonzero
 * tag works; zero is refused with NV_ERR_INVALID_OWNER (measured). */
#define NVKVM_RM_OWNER_TAG 0x6e766b6du

/*
 * UVM_MAP_EXTERNAL_ALLOCATION's tail is version-variant, so the fields after
 * the per-GPU attribute array are addressed by BYTE OFFSET, never through the
 * (V550-shaped) struct.  #81: the array is 256 entries on driver >= 550 and 32
 * before it, which moves rmCtrlFd from 1184 to 9248 — dereferencing the struct
 * on a 535 host writes 8 KiB past the real field.  The profile knows where
 * rmCtrlFd is; the rest of the tail is a fixed shape around it:
 *
 *     [fd_off - 8]  NvU64     gpuAttributesCount
 *     [fd_off + 0]  NvS32     rmCtrlFd
 *     [fd_off + 4]  NvHandle  hClient
 *     [fd_off + 8]  NvHandle  hMemory
 *     [fd_off + 12] NV_STATUS rmStatus
 */
#define UVM_MAP_EXT_PERGPU_OFF    24u   /* base, length, offset precede it */
#define UVM_MAP_EXT_PERGPU_STRIDE ((unsigned)sizeof(struct uvm_gpu_mapping_attributes))

static void put_u64(void *buf, unsigned off, __u64 v) { memcpy((char *)buf + off, &v, 8); }
static void put_u32(void *buf, unsigned off, __u32 v) { memcpy((char *)buf + off, &v, 4); }

/* ── small ioctl helpers ─────────────────────────────────────────────────── */

/* Where an ioctl is issued.  Scalars rather than a `struct nvkvm_fd_ctx *`
 * because teardown runs from vma_close(), which can fire after the owning fd
 * has gone; the region caches these so nothing there is dereferenced. */
struct ext_target {
	__u32 isolate_id;
	__u32 handle_id;
	__u32 session_id;
};

static struct ext_target ext_target_of(struct nvkvm_fd_ctx *c)
{
	struct ext_target t = {
		.isolate_id = c->session->isolate_id,
		.handle_id  = c->handle_id,
		.session_id = (__u32)c->session->id,
	};
	return t;
}

/*
 * NV_ESC_RM_ALLOC, NVOS64 form.
 *
 * `p_alloc_parms` is left 0 on the wire: the stub re-points it at its own copy
 * of the aux blob (nvkvm_stub.c, ptr_off 16 for nr 0x2b) precisely so a guest
 * pointer never reaches the driver.  Returns 0 and fills *nvstatus on a
 * completed call; a negative errno means the call did not reach RM.
 */
static int rm_alloc(struct ext_target t, __u32 h_root, __u32 h_parent,
		    __u32 *h_new, __u32 h_class,
		    void *ap, __u32 ap_size, __u32 *nvstatus)
{
	struct nvos64_parameters *p;
	long ret;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->h_root             = h_root;
	p->h_object_parent    = h_parent;
	p->h_object_new       = *h_new;
	p->h_class            = h_class;
	p->p_alloc_parms      = 0;
	p->p_rights_requested = 0;
	p->alloc_parms_size   = ap_size;

	ret = nvkvm_virtio_ioctl_on_isolate_nomm(
			t.isolate_id, t.handle_id, t.session_id,
			FE_CMD(NV_ESC_RM_ALLOC, struct nvos64_parameters),
			p, sizeof(*p), ap, ap_size);
	if (ret >= 0) {
		*h_new    = p->h_object_new;
		*nvstatus = p->status;
	}
	kfree(p);
	return ret < 0 ? (int)ret : 0;
}

static void rm_free_obj(struct ext_target t, __u32 h_root, __u32 h_parent,
			__u32 h_old)
{
	struct nvos00_parameters *p;
	long ret;

	if (!t.handle_id || !h_old)
		return;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return;
	p->h_root          = h_root;
	p->h_object_parent = h_parent;
	p->h_object_old    = h_old;
	ret = nvkvm_virtio_ioctl_on_isolate_nomm(
		t.isolate_id, t.handle_id, t.session_id,
		FE_CMD(NV_ESC_RM_FREE, struct nvos00_parameters),
		p, sizeof(*p), NULL, 0);
	/* Checked, not fired-and-forgotten: a sysmem object that RM refuses to
	 * free is PINNED HOST MEMORY that never comes back, and a loop of
	 * managed allocations would then take the host down rather than fail. */
	if (ret < 0 || p->status)
		pr_warn_ratelimited(
			"nvkvm: uvm fallback: RM_FREE h=0x%x parent=0x%x ret=%ld nvstatus=0x%x\n",
			h_old, h_parent, ret, p->status);
	kfree(p);
}

/* A UVM ioctl issued by US, on the guest's own UVM fd.  Returns 0 and fills
 * *nvstatus, or a negative errno if the call never completed. */
static int uvm_call(struct ext_target t, unsigned int cmd,
		    void *params, size_t size, __u32 *nvstatus,
		    unsigned rm_status_off)
{
	long ret = nvkvm_virtio_ioctl_on_isolate_nomm(
			t.isolate_id, t.handle_id, t.session_id,
			cmd, params, size, NULL, 0);
	if (ret < 0)
		return (int)ret;
	*nvstatus = *(__u32 *)((char *)params + rm_status_off);
	return 0;
}

/* ── the private RM object tree ──────────────────────────────────────────── */

/*
 * Build the client/device/subdevice tree this UVM fd allocates its backing
 * under.  Called once per UVM fd, under st->ext_lock.
 *
 * Two things here are load-bearing and were established by measurement
 * (tools/uvm_sysmem_probe.c on 575.51.03 / RTX 3060), not from the SDK:
 *
 *  - NV01_DEVICE_0 is refused with NV_ERR_INSUFFICIENT_PERMISSIONS (0x1b)
 *    unless a /dev/nvidiaN fd has been bound to the client's control fd with
 *    NV_ESC_REGISTER_FD first.  That is the only reason a device handle is
 *    opened at all; nothing else is ever issued on it.
 *  - deviceId 0 is used unconditionally.  The object is *system* memory, so it
 *    is not tied to the GPU whose device it is allocated under — UVM dups it
 *    into whichever GPU MAP_EXTERNAL_ALLOCATION names, and cross-device dup of
 *    a sysmem object is exactly what nvUvmInterfaceDupMemory does for
 *    cudaMalloc today.  The device is an allocation context, not a placement.
 */
static int ext_tree_build(struct nvkvm_uvm_fd_state *st)
{
	struct nv0080_alloc_parameters *dp = NULL;
	struct nv2080_alloc_parameters *sp = NULL;
	struct nv_ioctl_register_fd *rp = NULL;
	struct ext_target tctl, tdev;
	__u32 nvstatus = 0;
	__u32 h;
	int ret;

	if (st->ext_ready)
		return 0;

	st->ext_ctl = nvkvm_fd_ctx_open_dev(NVKVM_DEV_CTL, O_RDWR);
	if (IS_ERR(st->ext_ctl)) {
		ret = PTR_ERR(st->ext_ctl);
		st->ext_ctl = NULL;
		return ret;
	}
	st->ext_dev = nvkvm_fd_ctx_open_dev(NVKVM_DEV_GPU(0), O_RDWR);
	if (IS_ERR(st->ext_dev)) {
		ret = PTR_ERR(st->ext_dev);
		st->ext_dev = NULL;
		goto err;
	}
	tctl = ext_target_of(st->ext_ctl);
	tdev = ext_target_of(st->ext_dev);

	rp = kzalloc(sizeof(*rp), GFP_KERNEL);
	dp = kzalloc(sizeof(*dp), GFP_KERNEL);
	sp = kzalloc(sizeof(*sp), GFP_KERNEL);
	if (!rp || !dp || !sp) { ret = -ENOMEM; goto err; }

	/* Bind the device fd to the control fd.  The field carries a QEMU
	 * handle_id, not an fd — the stub resolves it (nvkvm_stub.c, nr 0xc9). */
	rp->ctl_fd = (__s32)st->ext_ctl->handle_id;
	ret = (int)nvkvm_virtio_ioctl_on_isolate_nomm(
			tdev.isolate_id, tdev.handle_id, tdev.session_id,
			FE_CMD(NV_ESC_REGISTER_FD, struct nv_ioctl_register_fd),
			rp, sizeof(*rp), NULL, 0);
	if (ret < 0) {
		pr_warn("nvkvm: uvm fallback: REGISTER_FD failed: %d\n", ret);
		goto err;
	}

	h = RM_HANDLE_LET_RM_CHOOSE;
	ret = rm_alloc(tctl, 0, 0, &h, NV01_ROOT, NULL, 0, &nvstatus);
	if (ret || nvstatus) {
		pr_warn("nvkvm: uvm fallback: NV01_ROOT ret=%d nvstatus=0x%x\n",
			ret, nvstatus);
		ret = ret ? ret : -EIO;
		goto err;
	}
	st->ext_h_client = h;

	dp->device_id      = 0;
	dp->h_client_share = st->ext_h_client;
	h = RM_HANDLE_LET_RM_CHOOSE;
	ret = rm_alloc(tctl, st->ext_h_client, st->ext_h_client, &h,
		       NV01_DEVICE_0, dp, (__u32)sizeof(*dp), &nvstatus);
	if (ret || nvstatus) {
		pr_warn("nvkvm: uvm fallback: NV01_DEVICE_0 ret=%d nvstatus=0x%x\n",
			ret, nvstatus);
		ret = ret ? ret : -EIO;
		goto err;
	}
	st->ext_h_device = h;

	/* Subdevice: not required by the memory allocation, allocated so the
	 * tree has the shape RM expects of a live client.  A failure here is not
	 * fatal — nothing below references it. */
	sp->sub_device_id = 0;
	h = RM_HANDLE_LET_RM_CHOOSE;
	if (!rm_alloc(tctl, st->ext_h_client, st->ext_h_device, &h,
		      NV20_SUBDEVICE_0, sp, (__u32)sizeof(*sp), &nvstatus) &&
	    !nvstatus)
		st->ext_h_subdev = h;

	st->ext_ready = true;
	ret = 0;
out:
	kfree(rp); kfree(dp); kfree(sp);
	return ret;

err:
	nvkvm_uvm_ext_fd_teardown(st);
	goto out;
}

void nvkvm_uvm_ext_fd_teardown(struct nvkvm_uvm_fd_state *st)
{
	if (!st)
		return;
	if (st->ext_ctl) {
		struct ext_target t = ext_target_of(st->ext_ctl);

		rm_free_obj(t, st->ext_h_client, st->ext_h_device,
			    st->ext_h_subdev);
		rm_free_obj(t, st->ext_h_client, st->ext_h_client,
			    st->ext_h_device);
		/* Freeing a root client names it in all three fields; hRoot 0
		 * has no client to resolve and RM answers
		 * NV_ERR_INVALID_OBJECT_HANDLE (measured). */
		rm_free_obj(t, st->ext_h_client, st->ext_h_client,
			    st->ext_h_client);
	}
	st->ext_h_subdev = 0;
	st->ext_h_device = 0;
	st->ext_h_client = 0;
	st->ext_ready    = false;
	if (st->ext_dev) { nvkvm_fd_ctx_put(st->ext_dev); st->ext_dev = NULL; }
	if (st->ext_ctl) { nvkvm_fd_ctx_put(st->ext_ctl); st->ext_ctl = NULL; }
}

/* ── teardown ────────────────────────────────────────────────────────────── */

/*
 * Undo, in the order teardown uses everywhere else: the GPU mapping goes first,
 * then the CPU mapping, then the object.  At no point is there a live GPU
 * mapping over memory that has been handed back.
 */
static void ext_unwind(struct nvkvm_mmap_region *r, bool have_range,
		       bool have_mmap)
{
	struct ext_target tuvm = {
		.isolate_id = r->ext_isolate_id,
		.handle_id  = r->ext_uvm_handle_id,
		.session_id = r->ext_session_id,
	};

	if (have_range && r->ext_gva) {
		struct uvm_free_params *fp = kzalloc(sizeof(*fp), GFP_KERNEL);
		__u32 nvstatus = 0;

		if (fp) {
			/*
			 * UVM_FREE needs an EXACT {base,length}: uvm_api_free()
			 * rejects length 0 and an external range has no
			 * look-up-by-base form (uvm_va_range.c:663-685).  Check
			 * the status — ignoring it leaks the range, and the VA
			 * is the object's identity host-side, so libcuda reusing
			 * it immediately then hits NV_ERR_UVM_ADDRESS_IN_USE.
			 */
			fp->base   = r->ext_gva;
			fp->length = (__u64)r->length;
			uvm_call(tuvm, UVM_FREE, fp, sizeof(*fp), &nvstatus,
				 offsetof(struct uvm_free_params, rm_status));
			if (nvstatus)
				pr_warn("nvkvm: uvm fallback: UVM_FREE 0x%llx+0x%lx nvstatus=0x%x\n",
					(unsigned long long)r->ext_gva,
					r->length, nvstatus);
			kfree(fp);
		}
	}
	if (have_mmap && r->mmap_token)
		nvkvm_virtio_munmap_on_isolate(r->ext_isolate_id, r->mmap_token);
	r->mmap_token = 0;

	/*
	 * The object is freed through the handle that OWNS the client, not
	 * through this mapping's own ctl handle: RM resolves a client handle
	 * against the file it was allocated on and answers anything else with
	 * NV_ERR_INVALID_CLIENT.  The owner handle is alive here in both callers
	 * -- nvkvm_mmap_release_fd() runs before nvkvm_uvm_ext_fd_teardown() --
	 * and in the one case where it is not (a vma_close after the fd is gone)
	 * the client itself has already been freed, taking the object with it.
	 */
	{
		struct ext_target town = {
			.isolate_id = r->ext_isolate_id,
			.handle_id  = r->ext_owner_handle_id,
			.session_id = r->ext_session_id,
		};
		rm_free_obj(town, r->ext_h_client, r->ext_h_device,
			    r->ext_h_memory);
	}
	if (r->ext_map_ctl) {
		nvkvm_fd_ctx_put(r->ext_map_ctl);
		r->ext_map_ctl = NULL;
	}
	r->ext_h_memory = 0;
}

/*
 * Release a fallback-backed range.
 *
 * Called with the region already claimed by exactly one of vma_close() /
 * mmap_release_fd(), and already off ctx->mmap_regions.  Frees the region.
 *
 * This must happen on MUNMAP, not be deferred to fd close.  For a forwarded
 * device mapping deferral is right — the NVIDIA driver expects those to outlive
 * individual VMAs.  A fallback range is the opposite: the VA IS the object's
 * identity host-side and libcuda reuses it immediately.  Measured on the earlier
 * QEMU-side implementation: with teardown deferred, a 4 MiB managed allocation
 * freed and followed by a 64 MiB one failed NV_ERR_UVM_ADDRESS_IN_USE, because
 * the new range overlapped the old one still live in the va_space.
 */
void nvkvm_uvm_ext_release(struct nvkvm_mmap_region *region)
{
	ext_unwind(region, true, true);
	kfree(region);
}

/* ── the synthesis ───────────────────────────────────────────────────────── */

/*
 * The GPUs this va_space has registered.  gpuAttributesCount == 0 does NOT mean
 * "all GPUs" — UVM refuses it outright (uvm_map_external.c:993-994) — so the
 * set has to come from what UVM_REGISTER_GPU told us.
 */
static __u32 ext_collect_uuids(struct nvkvm_uvm_fd_state *st, void *mp,
			       size_t mp_size)
{
	struct nvkvm_uvm_gpu_reg *g;
	__u32 n = 0;

	/* The writer uses st->lock.  We arrive with ext_lock held, establishing
	 * the documented ext_lock -> lock order and snapshotting a fully linked
	 * list rather than racing UVM_REGISTER_GPU on the same fd. */
	mutex_lock(&st->lock);
	list_for_each_entry(g, &st->registered_gpus, list) {
		unsigned off = UVM_MAP_EXT_PERGPU_OFF + n * UVM_MAP_EXT_PERGPU_STRIDE;

		if (n >= NVKVM_UVM_MAX_REG_GPUS)
			break;
		if (off + UVM_MAP_EXT_PERGPU_STRIDE > mp_size)
			break;
		memcpy((char *)mp + off, g->gpu_uuid, 16);
		n++;
	}
	mutex_unlock(&st->lock);
	return n;
}

int nvkvm_uvm_ext_mmap(struct nvkvm_fd_ctx *ctx, struct vm_area_struct *vma)
{
	struct nvkvm_uvm_fd_state *st = ctx->uvm_state;
	unsigned long vma_len = vma->vm_end - vma->vm_start;
	__u64 gva = vma->vm_start;
	struct nvkvm_mmap_region *region = NULL;
	struct nv_memory_allocation_params_v545 *ap = NULL;
	struct nv_ioctl_nvos33_parameters_with_fd *mm = NULL;
	struct uvm_create_external_range_params *cp = NULL;
	void *mp = NULL;                  /* UVM_MAP_EXTERNAL_ALLOCATION_PARAMS */
	size_t mp_size = nvkvm_prof()->uvm_map_ext_size;
	unsigned fd_off = nvkvm_prof()->uvm_map_ext_fd_off;
	__u32 ap_size = nvkvm_prof()->mem_alloc_size;
	struct ext_target tctl, tuvm;
	__u32 nvstatus = 0, h = 0, n_gpus;
	__u64 gpa_base = 0;
	__u32 mmap_token = 0;
	bool have_range = false, have_mmap = false;
	int ret;

	if (!st)
		return -EINVAL;

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	ap     = kzalloc(ap_size, GFP_KERNEL);
	mm     = kzalloc(sizeof(*mm), GFP_KERNEL);
	cp     = kzalloc(sizeof(*cp), GFP_KERNEL);
	mp     = kzalloc(mp_size, GFP_KERNEL);
	if (!region || !ap || !mm || !cp || !mp) { ret = -ENOMEM; goto out; }
	if (mp_size < fd_off + 16 || fd_off < UVM_MAP_EXT_PERGPU_OFF + 8 ||
	    ap_size < sizeof(*ap)) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&st->ext_lock);
	mutex_lock(&st->lock);
	if (!st->shadow_valid) {
		mutex_unlock(&st->lock);
		ret = -EIO;
		goto unlock;
	}
	mutex_unlock(&st->lock);

	ret = ext_tree_build(st);
	if (ret)
		goto unlock;

	tctl = ext_target_of(st->ext_ctl);
	tuvm = ext_target_of(ctx);

	region->ctx               = ctx;
	region->ext_backed        = true;
	refcount_set(&region->vma_refs, 1);
	region->ext_gva           = gva;
	region->length            = vma_len;
	region->handle_id         = ctx->handle_id;
	region->ext_uvm_handle_id = ctx->handle_id;
	region->ext_isolate_id    = tuvm.isolate_id;
	region->ext_session_id    = tuvm.session_id;
	region->ext_h_client      = st->ext_h_client;
	region->ext_h_device      = st->ext_h_device;
	region->ext_owner_handle_id = tctl.handle_id;

	/*
	 * A dedicated /dev/nvidiactl handle for THIS mapping.  RM arms the mmap
	 * context on the `struct file` named by NVOS33.fd and never clears it —
	 * nvidia_mmap_helper() consumes nothing, only nv_free_file_private()
	 * does — so a second NV_ESC_RM_MAP_MEMORY against a file that already
	 * carries one returns NV_ERR_STATE_IN_USE (0x63).  Measured both ways:
	 * tools/uvm_sysmem_probe.c "ctl->ctl x2" fails and "ctl->ctl2" works.
	 */
	region->ext_map_ctl = nvkvm_fd_ctx_open_dev(NVKVM_DEV_CTL, O_RDWR);
	if (IS_ERR(region->ext_map_ctl)) {
		ret = PTR_ERR(region->ext_map_ctl);
		region->ext_map_ctl = NULL;
		goto unlock;
	}
	region->ext_ctl_handle_id = region->ext_map_ctl->handle_id;

	/* 1. RM allocates the pages.  No address is named by anyone. */
	ap->owner = NVKVM_RM_OWNER_TAG;
	ap->type  = 0;                        /* NVOS32_TYPE_IMAGE */
	ap->attr  = NVOS32_ATTR_LOCATION_PCI; /* system memory */
	ap->size  = (__u64)vma_len;
	h = RM_HANDLE_LET_RM_CHOOSE;
	ret = rm_alloc(tctl, st->ext_h_client, st->ext_h_device, &h,
		       NV01_MEMORY_SYSTEM, ap, ap_size, &nvstatus);
	if (ret || nvstatus) {
		pr_warn_ratelimited(
			"nvkvm: uvm fallback: sysmem alloc 0x%lx bytes ret=%d nvstatus=0x%x\n",
			vma_len, ret, nvstatus);
		ret = ret ? ret : -ENOMEM;
		goto unlock;
	}
	region->ext_h_memory = h;

	/*
	 * 2. Arm the CPU mapping.  pLinearAddress is NOT an mmap offset: the
	 * ioctl stores the mmap context on the file named by `fd`, and
	 * nvidia_mmap_helper() refuses any mmap whose vm_pgoff is not zero
	 * (nv-mmap.c:503-513).  Hence offset 0 at step 3.  `fd` carries a
	 * handle_id; the stub resolves it to its own fd, which is the SAME
	 * struct file QEMU holds via SCM_RIGHTS — so the context QEMU's mmap
	 * consumes is the one this ioctl armed.
	 */
	mm->h_client = st->ext_h_client;
	mm->h_device = st->ext_h_device;
	mm->h_memory = region->ext_h_memory;
	mm->offset   = 0;
	mm->length   = (__u64)vma_len;
	mm->flags    = 0;
	mm->fd       = (__s32)region->ext_map_ctl->handle_id;
	ret = (int)nvkvm_virtio_ioctl_on_isolate_nomm(
			tctl.isolate_id, tctl.handle_id, tctl.session_id,
			FE_CMD(NV_ESC_RM_MAP_MEMORY,
			       struct nv_ioctl_nvos33_parameters_with_fd),
			mm, sizeof(*mm), NULL, 0);
	if (ret < 0 || mm->status) {
		pr_warn_ratelimited("nvkvm: uvm fallback: RM_MAP_MEMORY ret=%d nvstatus=0x%x\n",
				    ret, mm->status);
		ret = ret < 0 ? ret : -EIO;
		goto unlock;
	}

	/*
	 * 3. Pull those exact pages into the guest at G.  Named against the
	 * NVIDIACTL handle, not the UVM one: QEMU's UVM branch is the degenerate
	 * anonymous-window path that never touches the driver, while the
	 * nvidiactl branch does the real zero-copy mmap of the device fd.  The
	 * ctl-node mapping RM builds here is vm_insert_page()'d struct pages
	 * (nv-mmap.c, nvidia_mmap_sysmem), i.e. ordinary write-back RAM — which
	 * is what lets it sit under a KVM memslot at all.
	 */
	ret = nvkvm_virtio_mmap_on_isolate(tuvm.isolate_id,
					   region->ext_map_ctl->handle_id,
					   gva, 0, (__u64)vma_len,
					   PROT_READ | PROT_WRITE, MAP_SHARED,
					   (unsigned int)tuvm.session_id,
					   &gpa_base, &mmap_token);
	if (ret) {
		pr_warn_ratelimited("nvkvm: uvm fallback: MMAP_ON_ISOLATE: %d\n", ret);
		goto unlock;
	}
	region->mmap_token = mmap_token;
	region->gpa_base   = (unsigned long)gpa_base;
	have_mmap = true;

	if (!nvkvm_gpa_in_mmap_window((unsigned long)gpa_base, vma_len)) {
		pr_warn("nvkvm: uvm fallback: GPA 0x%llx outside window\n", gpa_base);
		ret = -EIO;
		goto unlock;
	}

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	ret = remap_pfn_range(vma, vma->vm_start,
			      (unsigned long)(gpa_base >> PAGE_SHIFT),
			      vma_len, vma->vm_page_prot);
	if (ret)
		goto unlock;
	/* The window is a prefetchable BAR to the guest, so remap_pfn_range()
	 * silently downgraded these write-back pages to UC-.  They are
	 * cache-coherent host RAM; put them back.  Same reasoning, and the same
	 * helper, as the forwarded path. */
	nvkvm_force_range_wb(vma->vm_mm, vma->vm_start, vma->vm_end);

	/* 4. Publish it to the GPU at G — the guest's own number, used as a GPU
	 * virtual address in the guest's own RM VA space and never as a host
	 * address.  73 is NVKVM_UVM_VA_CREATE in the U-6 schema, so this is what
	 * records the range in the ownership table. */
	cp->base   = gva;
	cp->length = (__u64)vma_len;
	ret = uvm_call(tuvm, UVM_CREATE_EXTERNAL_RANGE, cp, sizeof(*cp), &nvstatus,
		       offsetof(struct uvm_create_external_range_params, rm_status));
	if (ret || nvstatus) {
		pr_warn_ratelimited(
			"nvkvm: uvm fallback: CREATE_EXTERNAL_RANGE 0x%llx+0x%lx ret=%d nvstatus=0x%x\n",
			(unsigned long long)gva, vma_len, ret, nvstatus);
		ret = ret ? ret : -EIO;
		goto unlock;
	}
	have_range = true;

	n_gpus = ext_collect_uuids(st, mp, mp_size);
	if (!n_gpus) {
		pr_warn_ratelimited("nvkvm: uvm fallback: no GPU registered on this UVM fd\n");
		ret = -ENODEV;
		goto unlock;
	}
	put_u64(mp, 0,  gva);
	put_u64(mp, 8,  (__u64)vma_len);
	put_u64(mp, 16, 0);                      /* offset into the object */
	put_u64(mp, fd_off - 8, (__u64)n_gpus);  /* gpuAttributesCount */
	put_u32(mp, fd_off,     region->ext_map_ctl->handle_id);
	put_u32(mp, fd_off + 4, st->ext_h_client);
	put_u32(mp, fd_off + 8, region->ext_h_memory);
	ret = uvm_call(tuvm, UVM_MAP_EXTERNAL_ALLOCATION, mp, mp_size, &nvstatus,
		       fd_off + 12 /* rmStatus */);
	if (ret || nvstatus) {
		pr_warn_ratelimited(
			"nvkvm: uvm fallback: MAP_EXTERNAL_ALLOCATION 0x%llx+0x%lx ret=%d nvstatus=0x%x\n",
			(unsigned long long)gva, vma_len, ret, nvstatus);
		ret = ret ? ret : -EIO;
		goto unlock;
	}

	spin_lock(&ctx->mmap_lock);
	list_add_tail(&region->list, &ctx->mmap_regions);
	spin_unlock(&ctx->mmap_lock);

	vma->vm_ops          = &nvkvm_vm_ops;
	vma->vm_private_data = region;
	region->vma          = vma;
	region->offset       = (__u64)vma->vm_pgoff << PAGE_SHIFT;
	region = NULL;                    /* handed to the VMA */
	ret = 0;

unlock:
	if (ret && region)
		ext_unwind(region, have_range, have_mmap);
	mutex_unlock(&st->ext_lock);
out:
	kfree(region); kfree(ap); kfree(mm); kfree(cp); kfree(mp);
	return ret;
}

/*
 * Is [base, base+len) inside a range this fd backed itself?
 *
 * Deliberately narrow: exact containment in ONE region, and only regions this
 * very fd created.  It scopes the two UVM commands answered locally; a blanket
 * "yes" there would be a claim about ranges we know nothing about.
 */
bool nvkvm_uvm_ext_covers(struct nvkvm_fd_ctx *ctx, __u64 base, __u64 len)
{
	struct nvkvm_mmap_region *r;
	bool found = false;

	if (!len || base + len < base)
		return false;
	spin_lock(&ctx->mmap_lock);
	list_for_each_entry(r, &ctx->mmap_regions, list) {
		if (!r->ext_backed)
			continue;
		if (base >= r->ext_gva &&
		    base + len <= r->ext_gva + (__u64)r->length) {
			found = true;
			break;
		}
	}
	spin_unlock(&ctx->mmap_lock);
	return found;
}
