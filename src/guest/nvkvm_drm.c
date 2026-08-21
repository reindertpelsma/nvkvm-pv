// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_drm.c — minimal nvidia-drm render-node emulation for the guest.
 *
 * The NVIDIA Vulkan/EGL userspace enumerates the GPU through the DRM render
 * node /dev/dri/renderD128, NOT through /dev/nvidia* (which carry compute).
 * The ICD stats the node, derives its rdev major, and requires
 * /sys/dev/char/<major>:128/device/drm to exist before opening it.  Both the
 * canonical DRM major (226) and that sysfs tree are owned by the kernel DRM
 * core and can only be obtained by registering a real DRM device — a raw cdev
 * cannot claim major 226.  So we register a render-only drm_driver here.
 *
 * Division of labour (mirrors the access-model split — guest kernel owns
 * intra-VM semantics, host owns the hardware):
 *   - VERSION              → answered by the DRM core from driver->name/date/...
 *                            (driver-constant: "nvidia-drm").
 *   - nvidia private ioctls (GET_DEV_INFO, DMABUF_SUPPORTED, …) → forwarded to
 *     the host /dev/dri/renderD128 through the SAME per-mm isolate the
 *     process's /dev/nvidia0 uses, so the returned gpu_id correlates with the
 *     RM device.  QEMU's DRM allowlist (default-deny) gates what reaches the
 *     host; the stub opens the host render node and runs the ioctl.
 *
 * Render-path GEM ioctls (IMPORT_USERSPACE_MEMORY / MAP_OFFSET / EXPORT_DMABUF)
 * are added in a later milestone with proper guest-VA marshalling (no raw guest
 * pointer ever reaches the VMM/stub).
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/pci.h>
#include <drm/drm_drv.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>
#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>   /* #102: dumb buffers for KMS scanout */
#include <drm/drm_framebuffer.h>        /* #102: fb->obj[] for the present path */
#include <drm/drm_prime.h>              /* #110: get_sg_table for dma-buf import */
#include <linux/scatterlist.h>

#include "nvkvm.h"

#define NVKVM_PCI_VENDOR_NVIDIA 0x10de

#define NVKVM_DRM_COMMAND_BASE 0x40

/* ── GEM handle bridging ─────────────────────────────────────────────────────
 *
 * nvidia-private ioctls that mint GEM objects (e.g. SEMSURF_FENCE_CTX_CREATE)
 * run in the STUB's render-node DRM file, so the handle they return is valid in
 * the stub's GEM table — not the guest DRM core's.  But the Vulkan ICD then
 * uses that handle with DRM *core* ioctls (GEM_CLOSE, GEM_MAP_OFFSET) that the
 * guest's built-in DRM core resolves against the guest file's GEM table, where
 * it doesn't exist -> -ENOENT, and the ICD bails.
 *
 * Bridge the two namespaces: for every handle a forwarded ioctl returns, create
 * a lightweight proxy drm_gem_object in the guest file (no backing pages — it
 * never holds real memory; the hardware object lives in the stub).  The guest
 * IDR assigns its own handle, so we keep the stub handle in the proxy and
 * rewrite the returned handle to the guest one; calls that feed a handle back
 * to a forwarded ioctl translate guest->stub first.  GEM_CLOSE on the guest
 * frees the proxy and forwards a GEM_CLOSE(stub_handle) to release the real
 * object.  All translation is intra-VM (guest kernel owns GEM semantics); the
 * stub only ever sees its own handles. */
/* drm_nvidia_gem_object_type (host nvidia-drm-ioctl.h): what GEM_IDENTIFY_OBJECT
 * (0x0e) reports.  Our proxies all back ALLOC_NVKMS_MEMORY bos → NVKMS. */
#define NVKVM_GEM_OBJECT_NVKMS       0
#define NVKVM_GEM_OBJECT_DMABUF      1
#define NVKVM_GEM_OBJECT_USERMEMORY  2
#define NVKVM_GEM_OBJECT_UNKNOWN     0x7fffffff

struct nvkvm_gem_object {
	struct drm_gem_object base;
	struct nvkvm_fd_ctx  *ctx;         /* isolate to forward GEM_CLOSE to */
	__u32                 stub_handle; /* handle in the stub's DRM file   */
	__u32                 obj_type;    /* NVKVM_GEM_OBJECT_* for IDENTIFY  */
	/* #110 dma-buf import backing: lazily-allocated pages so NVIDIA EGL's
	 * dma_buf_map_attachment(get_sg_table) succeeds. Only populated when the
	 * buffer is actually PRIME-imported (most bos are GPU-only and never are). */
	struct page         **import_pages;
	unsigned long         import_npages;
	/* #110 cross-isolate import cache: when a DIFFERENT isolate (e.g. a
	 * compositor) PRIME-imports this proxy, QEMU brokers the bo into that
	 * isolate's stub and returns a stub-local GEM handle.  Cache the last
	 * importer so repeated GEM ops (per-frame capture) reuse it instead of
	 * re-brokering.  Single entry: the common case is one compositor. */
	__u32                 xiso_importer_iso;  /* importer's isolate_id, 0=none */
	__u32                 xiso_gem;           /* stub GEM handle in that isolate */
	/* The importer's ctx, holding a reference for exactly the reason ->ctx
	 * above does: xiso_gem only means anything in the host render fd THIS ctx
	 * owns, and nvkvm_gem_free has to forward a GEM_CLOSE there even if the
	 * importer's drm_file -- or the whole compositor -- went away first.
	 * Caching the bare (isolate_id, handle_id) instead would be a fail-open
	 * bug: both ids are table-allocated and reusable, so a close issued after
	 * the importer died could land on an unrelated isolate's GEM. */
	struct nvkvm_fd_ctx  *xiso_ctx;
	/* Serialises broker-and-install against a second importer, so a replace
	 * cannot orphan or double-close the entry it evicts. */
	struct mutex          xiso_lock;
};

#define to_nvkvm_gem(o) container_of(o, struct nvkvm_gem_object, base)

/* Drop one cached cross-isolate import: close the brokered GEM in the importer's
 * stub, then drop the ctx ref that kept that stub fd addressable.  Takes
 * ownership of the (ctx, gem) pair the caller has already detached from the
 * cache, and must be called WITHOUT xiso_lock held -- it forwards a blocking
 * virtio round-trip. */
static void nvkvm_gem_xiso_release(struct nvkvm_fd_ctx *ctx, __u32 gem)
{
	struct drm_gem_close close = { .handle = gem };
	__u64 fault = 0;

	if (!ctx)
		return;
	if (gem)
		nvkvm_virtio_ioctl_on_isolate(ctx, DRM_IOCTL_GEM_CLOSE,
					      &close, sizeof(close),
					      NULL, 0, 0, &fault);
	nvkvm_fd_ctx_put(ctx);
}

static void nvkvm_gem_free(struct drm_gem_object *obj)
{
	struct nvkvm_gem_object *ng = to_nvkvm_gem(obj);

	/* Release the cross-isolate import first, if a compositor ever brokered
	 * this bo into its own stub (nvkvm_gem_resolve_fwd).  Nothing else ever
	 * closed it: there is no XISO_RELEASE in the protocol and QEMU keeps no
	 * per-importer record, so without this the importer's stub kept every
	 * client buffer it had ever touched alive for its whole life, and the
	 * owner's GEM_CLOSE could not reclaim that memory because the importer's
	 * PRIME reference still held it.  Freed before the owner's handle so the
	 * import goes away before the thing it references.  No lock: we are the
	 * last reference to the object, so no lookup can reach the cache. */
	nvkvm_gem_xiso_release(ng->xiso_ctx, ng->xiso_gem);
	ng->xiso_ctx          = NULL;
	ng->xiso_gem          = 0;
	ng->xiso_importer_iso = 0;
	mutex_destroy(&ng->xiso_lock);

	/* Release the real object in the stub on the ctx that owns the handle,
	 * then drop the ctx reference taken in nvkvm_gem_proxy_create.  Holding
	 * that ref guarantees ng->ctx (and its stub fd) is still live here even
	 * if the creating drm_file closed first (cross-file PRIME re-import). */
	if (ng->ctx) {
		struct drm_gem_close close = { .handle = ng->stub_handle };
		__u64 fault = 0;

		nvkvm_virtio_ioctl_on_isolate(ng->ctx, DRM_IOCTL_GEM_CLOSE,
					      &close, sizeof(close),
					      NULL, 0, 0, &fault);
		nvkvm_fd_ctx_put(ng->ctx);
	}
	if (ng->import_pages) {
		unsigned long i;
		for (i = 0; i < ng->import_npages; i++)
			if (ng->import_pages[i])
				__free_page(ng->import_pages[i]);
		kvfree(ng->import_pages);
	}
	drm_gem_object_release(obj);
	kfree(ng);
}

/*
 * get_sg_table (#110): NVIDIA's EGL imports a scanout/capture bo's dma-buf via
 * dma_buf_map_attachment, which calls here for the backing sg_table.  Our proxy
 * GEM has no real pages (the bo lives in the stub), so without this the import
 * fails BAD_ALLOC.  Lazily allocate guest pages on first import (most bos are
 * GPU-only and never imported, so we never pay this).  ITERATION 1: plain pages
 * to unblock the map and observe what NVIDIA EGL does next; stub page-sharing
 * (so the host GPU reaches the same memory) is wired onto the registration
 * ioctl the next strace reveals.
 */
static struct sg_table *nvkvm_gem_get_sg_table(struct drm_gem_object *obj)
{
	struct nvkvm_gem_object *ng = to_nvkvm_gem(obj);
	unsigned long n = obj->size >> PAGE_SHIFT, i;

	if (!ng->import_pages) {
		ng->import_pages = kvmalloc_array(n, sizeof(struct page *),
						  GFP_KERNEL | __GFP_ZERO);
		if (!ng->import_pages)
			return ERR_PTR(-ENOMEM);
		for (i = 0; i < n; i++) {
			ng->import_pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
			if (!ng->import_pages[i]) {
				while (i--)
					__free_page(ng->import_pages[i]);
				kvfree(ng->import_pages);
				ng->import_pages = NULL;
				return ERR_PTR(-ENOMEM);
			}
		}
		ng->import_npages = n;
	}
	return drm_prime_pages_to_sg(obj->dev, ng->import_pages, n);
}

static const struct drm_gem_object_funcs nvkvm_gem_funcs = {
	.free         = nvkvm_gem_free,
	.get_sg_table = nvkvm_gem_get_sg_table,
};

/* Create a guest proxy GEM for a stub-side handle; returns the guest handle.
 * `size` is the proxy object's reported GEM size: for buffers that become
 * scanout framebuffers (GEM_ALLOC_NVKMS_MEMORY) it MUST be >= the real buffer
 * size, because drm_gem_fb_create validates pitch*height <= gem->size when a
 * compositor calls AddFB2.  For non-framebuffer proxies (fence contexts) any
 * page-sized stub is fine.  Always page-aligned (a GEM size below one page is
 * rejected by the core). */
static int nvkvm_gem_proxy_create(struct drm_file *file,
				  struct nvkvm_fd_ctx *ctx,
				  __u32 stub_handle, size_t size,
				  __u32 *guest_handle)
{
	struct nvkvm_gem_object *ng;
	int ret;

	size = PAGE_ALIGN(size);
	if (!size)
		size = PAGE_SIZE;
	ng = kzalloc(sizeof(*ng), GFP_KERNEL);
	if (!ng)
		return -ENOMEM;
	drm_gem_private_object_init(file->minor->dev, &ng->base, size);
	ng->base.funcs = &nvkvm_gem_funcs;
	mutex_init(&ng->xiso_lock);
	/*
	 * Audit G-6 (fixed #110): take a ctx reference.  A proxy can outlive its
	 * creating drm_file via a cross-file PRIME re-import (NVIDIA EGL opens
	 * renderD128 several times); nvkvm_gem_free then forwards GEM_CLOSE — and
	 * the dma-buf import path forwards GEM_EXPORT_NVKMS_MEMORY — on exactly
	 * this ctx, since the stub handle is only valid in the host fd it owns.
	 * Without the ref a later deref would be a guest-kernel UAF.  Dropped in
	 * nvkvm_gem_free.
	 */
	nvkvm_fd_ctx_get(ctx);
	ng->ctx        = ctx;
	ng->stub_handle = stub_handle;
	ng->obj_type    = NVKVM_GEM_OBJECT_NVKMS;
	ret = drm_gem_handle_create(file, &ng->base, guest_handle);
	/* The handle (or the proxy on failure) now owns the only ref. */
	drm_gem_object_put(&ng->base);
	return ret;
}

/* Resolve a guest GEM handle to the stub handle it proxies, or 0 if unknown. */
static __u32 nvkvm_gem_to_stub(struct drm_file *file, __u32 guest_handle)
{
	struct drm_gem_object *obj = drm_gem_object_lookup(file, guest_handle);
	__u32 sh = 0;

	if (obj) {
		if (obj->funcs == &nvkvm_gem_funcs)
			sh = to_nvkvm_gem(obj)->stub_handle;
		drm_gem_object_put(obj);
	}
	return sh;
}

/*
 * Resolve a guest GEM handle to the (stub_handle, forwarding-ctx) pair a GEM op
 * should target — handling the cross-isolate case (#110).
 *
 * A stub GEM handle lives in exactly one stub render fd: the ctx that ran
 * GEM_ALLOC_NVKMS_MEMORY (the "owner").  NVIDIA's EGL opens renderD128 several
 * times, and a guest app can pass a bo's dma-buf to a *different* process (a
 * compositor) entirely — both reach the SAME guest proxy object via PRIME, but
 * its stub_handle is meaningless in any isolate other than the owner's.
 *
 *  - Same isolate (incl. the multi-open single-process case): forward on the
 *    owner's ctx with the owner's stub_handle.
 *  - Different isolate (cross-process compositor import): QEMU brokers the bo
 *    into the caller's stub (owner PRIME-export → caller PRIME-import) and hands
 *    back a caller-local GEM handle; forward on the ctx it was brokered into
 *    with that.  Cached on the proxy so per-frame re-imports don't re-broker,
 *    and the cached handle is closed when the proxy is freed or the entry is
 *    evicted -- it is the only thing that ever releases it.
 *
 * The cache is one entry deep, so two importers alternating on one bo evict
 * each other, and an eviction can close a handle a third thread has already
 * been handed and is about to forward on.  That loses a frame (the stub fails
 * the op closed on an unresolvable handle) rather than corrupting anything
 * outside the evicted importer's own fd; a deeper or per-use-refcounted cache
 * is the real answer if a two-compositor workload ever matters.
 *
 * Returns 0 and fills *stub_h_out / *fwd_ctx_out on success; -errno on a failed
 * cross-isolate broker; -ENOENT if the handle is not one of our proxies. */
static int nvkvm_gem_resolve_fwd(struct drm_file *file, __u32 guest_handle,
				 __u32 *stub_h_out, struct nvkvm_fd_ctx **fwd_ctx_out)
{
	struct drm_gem_object *obj = drm_gem_object_lookup(file, guest_handle);
	struct nvkvm_gem_object *ng;
	struct nvkvm_fd_ctx *owner_ctx, *caller_ctx = file->driver_priv;
	__u32 owner_iso, caller_iso;
	int ret = 0;

	if (!obj)
		return -ENOENT;
	if (obj->funcs != &nvkvm_gem_funcs) {
		drm_gem_object_put(obj);
		return -ENOENT;
	}
	ng = to_nvkvm_gem(obj);
	owner_ctx  = ng->ctx;
	owner_iso  = (owner_ctx && owner_ctx->session) ? owner_ctx->session->isolate_id : 0;
	caller_iso = (caller_ctx && caller_ctx->session) ? caller_ctx->session->isolate_id : 0;

	if (!caller_ctx) {
		drm_gem_object_put(obj);
		return -EBADF;
	}

	if (owner_iso == 0 || owner_iso == caller_iso) {
		/* Same isolate — forward on the owner's ctx (where the handle
		 * is valid), exactly as the multi-open single-process path. */
		*stub_h_out  = ng->stub_handle;
		*fwd_ctx_out = owner_ctx ? owner_ctx : caller_ctx;
		drm_gem_object_put(obj);
		return 0;
	}

	/* Cross-isolate: reuse the cached broker result, else broker now.  The
	 * lock spans the broker so two importers racing on the same bo cannot
	 * both install (which would leak whichever lost) -- the second one waits
	 * and then evicts the first. */
	mutex_lock(&ng->xiso_lock);
	if (ng->xiso_importer_iso == caller_iso && ng->xiso_gem) {
		/* Forward on the ctx the handle was brokered INTO, not merely on
		 * some ctx in the same isolate: a stub GEM handle is scoped to
		 * one host render fd, and an isolate may hold several (NVIDIA's
		 * EGL opens renderD128 more than once).  Same rule the owner
		 * branch above follows, and the same ctx nvkvm_gem_free closes
		 * the handle on. */
		*stub_h_out  = ng->xiso_gem;
		*fwd_ctx_out = ng->xiso_ctx ? ng->xiso_ctx : caller_ctx;
		mutex_unlock(&ng->xiso_lock);
		drm_gem_object_put(obj);
		return 0;
	}

	{
		struct nvkvm_fd_ctx *old_ctx = NULL;
		__u32 old_gem = 0;
		__u32 gem = 0;

		ret = nvkvm_virtio_xiso_import(caller_ctx, owner_iso,
					       owner_ctx->handle_id,
					       ng->stub_handle, &gem);
		if (ret == 0 && gem) {
			/* Detach whatever the single-entry cache held so it is
			 * closed rather than orphaned, and pin the new importer's
			 * ctx until this proxy is freed (or evicted below). */
			old_ctx = ng->xiso_ctx;
			old_gem = ng->xiso_gem;
			nvkvm_fd_ctx_get(caller_ctx);
			ng->xiso_ctx          = caller_ctx;
			ng->xiso_importer_iso = caller_iso;
			ng->xiso_gem          = gem;
			*stub_h_out  = gem;
			*fwd_ctx_out = caller_ctx;
		} else if (ret == 0) {
			ret = -EIO;
		}
		mutex_unlock(&ng->xiso_lock);
		/* Outside the lock: this forwards a blocking GEM_CLOSE. */
		nvkvm_gem_xiso_release(old_ctx, old_gem);
	}
	drm_gem_object_put(obj);
	return ret;
}

/* Present path (#102): map a scanout framebuffer back to the host/stub buffer
 * behind it. A compositor's scanout buffer is an NVIDIA bo allocated via the
 * render node, so its guest GEM is one of our proxy objects carrying the stub
 * handle + owning isolate. (A plain shmem dumb fb is NOT a proxy → false.) */
bool nvkvm_fb_stub_handle(struct drm_framebuffer *fb, __u32 *stub_handle,
			  struct nvkvm_fd_ctx **ctx)
{
	struct drm_gem_object *obj;

	if (!fb)
		return false;
	obj = fb->obj[0];
	if (!obj || obj->funcs != &nvkvm_gem_funcs)
		return false;
	if (stub_handle)
		*stub_handle = to_nvkvm_gem(obj)->stub_handle;
	if (ctx)
		*ctx = to_nvkvm_gem(obj)->ctx;
	return true;
}

/* Param structs — sizes must match the host nvidia-drm-ioctl.h exactly so the
 * DRM core copies the right number of bytes in/out. */
struct drm_nvidia_get_dev_info_params {       /* 36 bytes, all scalars */
	__u32 gpu_id, mig_device, primary_index, supports_alloc;
	__u32 generic_page_kind, page_kind_generation, sector_layout;
	__u32 supports_sync_fd, supports_semsurf;
};

/* Semaphore-surface fence ioctls (render-path sync, #84). Sizes/layout MUST
 * match host nvidia-drm-ioctl.h so the DRM core copies the right byte count. */
struct drm_nvidia_semsurf_fence_ctx_create_params {  /* 32 bytes */
	__u64 index;             /* IN  */
	__u64 nvkms_params_ptr;  /* IN  user ptr to NVKMS import params */
	__u64 nvkms_params_size; /* IN  */
	__u32 handle;            /* OUT GEM handle to fence context */
	__u32 __pad;
};
struct drm_nvidia_semsurf_fence_create_params {      /* 24 bytes */
	__u32 fence_context_handle; /* IN  */
	__u32 timeout_value_ms;     /* IN  */
	__u64 wait_value;           /* IN  */
	__s32 fd;                   /* OUT sync fd */
	__u32 __pad;
};

/* Scanout-buffer allocation path (#109 present path). The NVIDIA gbm backend
 * allocates a display-capable bo on the render node via ALLOC_NVKMS_MEMORY, and
 * keys its per-DRM-file allocator state on GET_DRM_FILE_UNIQUE_ID — both are
 * DRM_RENDER_ALLOW (no DRM-master / no display privilege), so the unprivileged
 * stub forwards them on its host renderD128.  Both are flat scalars (no embedded
 * pointers).  Sizes/layout MUST match host nvidia-drm-ioctl.h byte-for-byte. */
struct drm_nvidia_get_drm_file_unique_id_params {    /* 8 bytes */
	__u64 id;                   /* OUT unique id of the host DRM file */
};
struct drm_nvidia_gem_alloc_nvkms_memory_params {    /* 24 bytes */
	__u32 handle;               /* OUT GEM handle in the stub's DRM file */
	__u8  block_linear;         /* IN  */
	__u8  compressible;         /* IN/OUT */
	__u16 __pad0;
	__u64 memory_size;          /* IN  */
	__u32 flags;                /* IN  */
	__u32 __pad1;
};
struct drm_nvidia_gem_identify_object_params {       /* 8 bytes */
	__u32 handle;               /* IN  GEM handle */
	__u32 object_type;          /* OUT drm_nvidia_gem_object_type */
};
/*
 * GEM_EXPORT_NVKMS_MEMORY (0x09) — #110 dma-buf import keystone.  NVIDIA's EGL
 * calls this on a PRIME-imported render/scanout bo to associate that bo's RM
 * memory object onto a caller-provided nv-export fd (the kernel runs
 * EXPORT_OBJECT_TO_FD under the hood), which it then re-imports via
 * IMPORT_OBJECT_FROM_FD (0x3d06).  Layout MUST match host nvidia-drm-ioctl.h.
 * nvkms_params_ptr points at a 4-byte NvKmsKapiPrivExportMemoryParams { int
 * memFd } — IN: the fd is the export target, value unchanged by the call. */
struct drm_nvidia_gem_export_nvkms_memory_params {   /* 24 bytes */
	__u32 handle;               /* IN  GEM handle */
	__u32 __pad;
	__u64 nvkms_params_ptr;     /* IN  -> { int memFd } */
	__u64 nvkms_params_size;    /* IN  */
};

/*
 * GEM_IMPORT_NVKMS_MEMORY (0x01) — #110 dma-buf EXPORT keystone (the mirror of
 * 0x09 above, and the one that was missing).
 *
 * This is the ioctl NVIDIA's EGL uses to turn an RM memory object into a GEM
 * object it can then PRIME_HANDLE_TO_FD.  It is the whole of
 * eglExportDMABUFImageMESA's kernel work, and therefore the whole of a Wayland
 * GL client's ability to hand a rendered buffer to a compositor.  Measured
 * sequence on the host (LD_PRELOAD ioctl trace, RTX 3050 / 580.173.02):
 *
 *   open("/dev/nvidiactl")                    -> memFd
 *   ioctl(memFd, 'F' nr 0xd4)                 -> RM object parked on that fd
 *   ioctl(renderD128, 'd' nr 0x41, 32 bytes)  -> GEM handle   <-- THIS
 *   ioctl(renderD128, DRM_IOCTL_PRIME_HANDLE_TO_FD)  -> dma-buf fd
 *   ioctl(renderD128, DRM_IOCTL_GEM_CLOSE)
 *
 * Without an entry in nvkvm_drm_ioctls[] the DRM *core* rejects it with
 * -EINVAL before nvkvm sees it at all — which is why the failure showed up as
 * EGL_BAD_MATCH with no `AUDIT unknown ioctl` and no QEMU `DENY` line.
 *
 * Layout MUST match host nvidia-drm-ioctl.h; verified byte-for-byte from the
 * host trace (mem_size=0x40000 for a 256x256 RGBA image, ptr@8, size@16=28,
 * handle@24 written 0->1 by the call).  nvkms_params_ptr points at an
 * NvKmsKapiPrivImportMemoryParams whose first field is { int memFd } — the same
 * shape 0x09 carries, so it is marshalled the same way.
 */
struct drm_nvidia_gem_import_nvkms_memory_params { /* 32 bytes */
	__u64 mem_size;             /* IN  size of the RM allocation */
	__u64 nvkms_params_ptr;     /* IN  -> { int memFd; ... } */
	__u64 nvkms_params_size;    /* IN  */
	__u32 handle;               /* OUT GEM handle in the stub's DRM file */
	__u32 __pad;
};

/*
 * GEM_IDENTIFY_OBJECT (0x0e): NVIDIA's EGL/gbm calls this right after
 * PRIME_FD_TO_HANDLE to learn the imported object's type (NVKMS / DMABUF /
 * USERMEMORY).  PRIME export+import already round-trip through the DRM core to
 * our proxy GEM (#109), so we answer LOCALLY — no host forward, no allowlist
 * surface.  All our proxies back ALLOC_NVKMS_MEMORY bos → NVKMS; anything else
 * (e.g. a shmem dumb fb) is UNKNOWN.  The host driver returns 0 in every case
 * (including UNKNOWN), so we do too.  Without this the DRM core rejects the
 * unknown ioctl with EINVAL and NVIDIA EGL aborts the dma-buf import — the lone
 * blocker that made compositor capture buffers un-importable.
 */
static int nvkvm_drm_gem_identify_object(struct drm_device *dev, void *data,
					 struct drm_file *file)
{
	struct drm_nvidia_gem_identify_object_params *p = data;
	struct drm_gem_object *obj = drm_gem_object_lookup(file, p->handle);

	(void)dev;
	if (obj && obj->funcs == &nvkvm_gem_funcs)
		p->object_type = to_nvkvm_gem(obj)->obj_type;
	else
		p->object_type = NVKVM_GEM_OBJECT_UNKNOWN;
	if (obj)
		drm_gem_object_put(obj);
	return 0;
}

/* Forward an already-kernel-copied DRM param blob to the host render node via
 * the process's isolate.  The DRM core handled the user<->kernel copy using
 * _IOC_SIZE(cmd); we just relay `data` and let the host write results back. */
static int nvkvm_drm_forward(struct drm_file *file, unsigned int cmd, void *data)
{
	struct nvkvm_fd_ctx *ctx = file->driver_priv;
	__u64 fault = 0;
	long r;

	if (!ctx)
		return -EBADF;
	r = nvkvm_virtio_ioctl_on_isolate(ctx, cmd, data, _IOC_SIZE(cmd),
					  NULL, 0, 0, &fault);
	return (r < 0) ? (int)r : 0;
}

#define NVKVM_DRM_FWD(suffix, CMD)					\
	static int nvkvm_drm_fwd_##suffix(struct drm_device *dev,	\
					  void *data,			\
					  struct drm_file *file)	\
	{ (void)dev; return nvkvm_drm_forward(file, (CMD), data); }

/*
 * GET_DEV_INFO (0x03): forward, then OVERWRITE primary_index with OUR primary
 * minor.  The host field is the HOST's DRM card number, and NVIDIA's userspace
 * turns it straight into a path: libEGL stats "/dev/dri/card<primary_index>"
 * while deciding whether it can drive a display connection.
 *
 * On any host where the NVIDIA GPU is not card0 — an iGPU laptop, the common
 * case; measured here as card2 — the guest was told primary_index=2, stat'd
 * /dev/dri/card2, got ENOENT, and NVIDIA's EGL declined the platform:
 *
 *   ioctl(renderD128, 'd' nr 0x43 = GET_DEV_INFO) = 0   -> primary_index=2
 *   newfstatat("/dev/dri/card2") = -1 ENOENT
 *   eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND) = EGL_NO_DISPLAY
 *
 * so every Wayland GL client silently fell back to Mesa/llvmpipe (which is what
 * "GL_RENDERER: llvmpipe" in the old realapp_matrix runs actually was), and a
 * client forced to the NVIDIA vendor died at "failed to initialize EGL display".
 *
 * This is the primary-node twin of the renderD128-vs-renderD129 fix: the guest
 * must be told about ITS OWN nodes.  Our KMS head lives on this same drm_device
 * (DRIVER_MODESET above), so dev->primary->index is exactly the card the guest
 * has.  It also stops a host DRM minor number leaking into the guest.
 */
static int nvkvm_drm_fwd_get_dev_info(struct drm_device *dev, void *data,
				      struct drm_file *file)
{
	struct drm_nvidia_get_dev_info_params *p = data;
	int r = nvkvm_drm_forward(file,
				  DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x03,
					   struct drm_nvidia_get_dev_info_params),
				  data);

	if (r == 0 && dev && dev->primary)
		p->primary_index = dev->primary->index;
	/*
	 * Do NOT advertise sync-fd support.  SEMSURF_FENCE_CREATE returns its
	 * sync fd in an OUT field (fd@16) that we forward verbatim, so the
	 * guest receives the HOST's descriptor number -- meaningless in the
	 * guest process.  Cross-boundary sync-fd passback is unimplemented
	 * (see nvkvm_drm_fwd_semsurf_fence_create).
	 *
	 * Claiming the capability anyway is what actually broke graphics:
	 * libnvidia-egl-wayland took the sync-fd presentation path, waited on
	 * a fence that could never signal, and every GL client hung on its
	 * first eglSwapBuffers -- glmark2 sat on scene 1 with 0 CPU time
	 * indefinitely, while the same weston+glmark2 on the host scored
	 * 21571.  Reporting 0 makes it pick a path we can actually service.
	 */
	if (r == 0)
		p->supports_sync_fd = 0;
	return r;
}
NVKVM_DRM_FWD(dmabuf_supported, DRM_IO(NVKVM_DRM_COMMAND_BASE + 0x0f))
/*
 * GET_DRM_FILE_UNIQUE_ID (0x18): the gbm backend reads this before allocating
 * and uses it ONLY as an opaque per-DRM-file key to dedup its userspace
 * allocator state across gbm_devices wrapping the same fd; it is never sent back
 * to the kernel.  We answer it ENTIRELY guest-side and do NOT forward it: the
 * host impl returns (u64)filep->driver_priv — a host kernel pointer — which
 * would leak a host heap address across the VM boundary (KASLR-defeat aid), the
 * exact class of leak the render-node allowlist exists to block.  ctx->handle_id
 * is already unique per open fd and opaque, so it is a correct, leak-free id.
 */
static int nvkvm_drm_fwd_get_drm_file_unique_id(struct drm_device *dev,
						void *data,
						struct drm_file *file)
{
	struct drm_nvidia_get_drm_file_unique_id_params *p = data;
	struct nvkvm_fd_ctx *ctx = file->driver_priv;

	(void)dev;
	if (!ctx)
		return -EBADF;
	/* Bias by a fixed nonzero constant so the id is never 0 (some callers
	 * treat 0 as "unset"); handle_id is unique per fd within the VM. */
	p->id = 0x6e766b766d000000ULL | (__u64)ctx->handle_id; /* "nvkvm" tag */
	return 0;
}

/*
 * SEMSURF_FENCE_CREATE takes fence_context_handle (a GEM handle from
 * CTX_CREATE) — translate the guest proxy handle to the stub's before
 * forwarding, restore after.  (fd@16 is an OUT sync fd; cross-boundary
 * sync-fd passback is a separate milestone.)
 */
static int nvkvm_drm_fwd_semsurf_fence_create(struct drm_device *dev,
					      void *data,
					      struct drm_file *file)
{
	struct drm_nvidia_semsurf_fence_create_params *p = data;
	struct nvkvm_fd_ctx *ctx = file->driver_priv;
	unsigned int cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x15,
				    struct drm_nvidia_semsurf_fence_create_params);
	__u32 guest_h = p->fence_context_handle;
	__u32 stub_h;
	__u64 fault = 0;
	long r;

	(void)dev;
	if (!ctx)
		return -EBADF;
	stub_h = nvkvm_gem_to_stub(file, guest_h);
	if (stub_h)
		p->fence_context_handle = stub_h;

	r = nvkvm_virtio_ioctl_on_isolate(ctx, cmd, data, sizeof(*p),
					  NULL, 0, 0, &fault);

	p->fence_context_handle = guest_h;   /* round-trip the caller's handle */
	return (r < 0) ? (int)r : 0;
}

/*
 * SEMSURF_FENCE_CTX_CREATE embeds a userspace pointer `nvkms_params_ptr`
 * (-> nvkms_params_size bytes, IN only) that the host kernel reads.  The host
 * has no access to guest VAs, so stage those bytes in the aux slot, zero the
 * pointer (the stub substitutes a host VA at offset 8), and forward.  Mirrors
 * the RM_CONTROL / NVKMS aux pattern; handle@24 comes back inline in `data`.
 */
static int nvkvm_drm_fwd_semsurf_fence_ctx_create(struct drm_device *dev,
						  void *data,
						  struct drm_file *file)
{
	struct drm_nvidia_semsurf_fence_ctx_create_params *p = data;
	struct nvkvm_fd_ctx *ctx = file->driver_priv;
	unsigned int cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x14,
				    struct drm_nvidia_semsurf_fence_ctx_create_params);
	void *aux = NULL;
	size_t aux_sz = 0;
	__u64 orig_ptr;
	__u64 fault = 0;
	long r;

	(void)dev;
	if (!ctx)
		return -EBADF;

	orig_ptr = p->nvkms_params_ptr;
	if (orig_ptr && p->nvkms_params_size > 0 &&
	    p->nvkms_params_size <= NVKVM_SHM_SLOT_DEFAULT_SIZE) {
		aux_sz = p->nvkms_params_size;
		aux = kzalloc(aux_sz, GFP_KERNEL);
		if (!aux)
			return -ENOMEM;
		if (copy_from_user(aux, (void __user *)(uintptr_t)orig_ptr,
				   aux_sz)) {
			kfree(aux);
			return -EFAULT;
		}
		p->nvkms_params_ptr = 0;   /* stub fills host VA at offset 8 */
	}

	r = nvkvm_virtio_ioctl_on_isolate(ctx, cmd, data,
					  sizeof(*p), aux, aux_sz, 0, &fault);

	/* Restore the caller's ptr; the kernel only reads it (IN). */
	p->nvkms_params_ptr = orig_ptr;
	kfree(aux);
	if (r < 0)
		return (int)r;

	/*
	 * The stub created the fence-context GEM object in its own DRM file and
	 * wrote its handle to p->handle.  Mint a guest-core proxy GEM so the ICD's
	 * subsequent core GEM ioctls (GEM_CLOSE) resolve, and hand back the guest
	 * handle instead of the stub's.
	 */
	if (p->handle) {
		__u32 guest_handle = 0;
		int gret = nvkvm_gem_proxy_create(file, ctx, p->handle,
						  PAGE_SIZE, &guest_handle);
		if (gret)
			return gret;
		p->handle = guest_handle;
	}
	return 0;
}

/*
 * GEM_EXPORT_NVKMS_MEMORY (0x09): translate the guest proxy GEM handle to the
 * stub's, and stage the embedded { int memFd } blob in the aux slot with the
 * guest fd swapped for our handle_id (the stub resolves its own local fd, then
 * exports the real bo's RM memory onto it — exactly the EXPORT_OBJECT_TO_FD
 * frontend path, but the fd rides inside the NVKMS params).  Zero the params
 * pointer so no guest VA is forwarded; the stub substitutes a host VA at
 * offset 8.  Both the handle and the memFd are IN/unchanged, so restore them
 * after.  Without this the DRM core rejects 0x09 (-EINVAL) and NVIDIA EGL
 * aborts the dma-buf import before it ever issues 0x3d06.
 */
static int nvkvm_drm_fwd_gem_export_nvkms_memory(struct drm_device *dev,
						 void *data,
						 struct drm_file *file)
{
	struct drm_nvidia_gem_export_nvkms_memory_params *p = data;
	struct nvkvm_fd_ctx *ctx = file->driver_priv;
	unsigned int cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x09,
				    struct drm_nvidia_gem_export_nvkms_memory_params);
	__u32 guest_h = p->handle, stub_h;
	__u64 orig_ptr;
	void *aux = NULL;
	size_t aux_sz = 0;
	__s32 orig_memfd = 0;
	bool have_memfd = false;
	__u64 fault = 0;
	long r;

	(void)dev;
	if (!ctx)
		return -EBADF;

	/* Resolve to the (stub handle, forwarding ctx) this op targets — handles
	 * the cross-isolate compositor-import case by brokering the bo into the
	 * caller's stub (see nvkvm_gem_resolve_fwd).  On a non-proxy handle, fall
	 * back to forwarding as-is on the caller's ctx. */
	{
		int rret = nvkvm_gem_resolve_fwd(file, guest_h, &stub_h, &ctx);
		if (rret == 0 && stub_h)
			p->handle = stub_h;
		else if (rret < 0 && rret != -ENOENT)
			return rret;
	}

	orig_ptr = p->nvkms_params_ptr;
	if (orig_ptr && p->nvkms_params_size >= sizeof(__s32) &&
	    p->nvkms_params_size <= NVKVM_SHM_SLOT_DEFAULT_SIZE) {
		aux_sz = p->nvkms_params_size;
		aux = kzalloc(aux_sz, GFP_KERNEL);
		if (!aux) {
			p->handle = guest_h;
			return -ENOMEM;
		}
		if (copy_from_user(aux, (void __user *)(uintptr_t)orig_ptr,
				   aux_sz)) {
			kfree(aux);
			p->handle = guest_h;
			return -EFAULT;
		}
		/* { int memFd } at offset 0 → swap for our handle_id. */
		memcpy(&orig_memfd, aux, sizeof(orig_memfd));
		have_memfd = true;
		if (orig_memfd >= 0) {
			__s32 hid = guest_fd_to_handle_id(orig_memfd);
			/* FAIL CLOSED: an untranslated value would be forwarded
			 * as a VM-global handle_id, which QEMU's cross-isolate
			 * relay treats as an entitlement.  See nvkvm_main.c. */
			if (hid < 0)
				return -EBADF;
			memcpy(aux, &hid, sizeof(hid));
		}
		p->nvkms_params_ptr = 0;   /* stub fills host VA at offset 8 */
	}

	r = nvkvm_virtio_ioctl_on_isolate(ctx, cmd, data, sizeof(*p),
					  aux, aux_sz, 0, &fault);

	/* memFd is IN (the export keeps its value); restore the caller's fd so a
	 * later read of the params buffer sees its own fd, never our handle_id. */
	if (have_memfd && aux)
		memcpy(aux, &orig_memfd, sizeof(orig_memfd));
	if (r >= 0 && aux &&
	    copy_to_user((void __user *)(uintptr_t)orig_ptr, aux, aux_sz))
		r = -EFAULT;
	p->nvkms_params_ptr = orig_ptr;
	p->handle = guest_h;
	kfree(aux);
	return (r < 0) ? (int)r : 0;
}

/*
 * GEM_IMPORT_NVKMS_MEMORY (0x01): same marshalling as 0x09 (zero the guest VA,
 * stage the { int memFd } blob in the aux slot with the guest fd swapped for
 * our handle_id), plus the OUT half of 0x0b (proxy the stub GEM handle into the
 * guest DRM core so the caller's PRIME_HANDLE_TO_FD / GEM_CLOSE resolve).
 *
 * The proxy is sized to mem_size, which is what makes the resulting guest
 * dma-buf a correctly-sized carrier for the cross-isolate broker: the
 * compositor PRIME-imports it, hits nvkvm_gem_resolve_fwd, and QEMU re-homes
 * the real bo into the compositor's stub.  That import half is already proven
 * byte-exact (tests/perf/apps/xiso_bytes_probe.c); this is the export half.
 */
static int nvkvm_drm_fwd_gem_import_nvkms_memory(struct drm_device *dev,
						 void *data,
						 struct drm_file *file)
{
	struct drm_nvidia_gem_import_nvkms_memory_params *p = data;
	struct nvkvm_fd_ctx *ctx = file->driver_priv;
	unsigned int cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x01,
				    struct drm_nvidia_gem_import_nvkms_memory_params);
	__u64 mem_size = p->mem_size;
	__u64 orig_ptr = p->nvkms_params_ptr;
	__u64 orig_size = p->nvkms_params_size;
	void *aux = NULL;
	size_t aux_sz = 0;
	__s32 orig_memfd = 0;
	bool have_memfd = false;
	__u64 fault = 0;
	long r;

	(void)dev;
	if (!ctx)
		return -EBADF;

	if (orig_ptr && orig_size >= sizeof(__s32) &&
	    orig_size <= NVKVM_SHM_SLOT_DEFAULT_SIZE) {
		aux_sz = orig_size;
		aux = kzalloc(aux_sz, GFP_KERNEL);
		if (!aux)
			return -ENOMEM;
		if (copy_from_user(aux, (void __user *)(uintptr_t)orig_ptr,
				   aux_sz)) {
			kfree(aux);
			return -EFAULT;
		}
		/* { int memFd } at offset 0 -> swap for our handle_id. */
		memcpy(&orig_memfd, aux, sizeof(orig_memfd));
		have_memfd = true;
		if (orig_memfd >= 0) {
			__s32 hid = guest_fd_to_handle_id(orig_memfd);
			/* FAIL CLOSED: an untranslated value would be forwarded
			 * as a VM-global handle_id, which QEMU's cross-isolate
			 * relay treats as an entitlement.  See nvkvm_main.c. */
			if (hid < 0)
				return -EBADF;
			memcpy(aux, &hid, sizeof(hid));
		}
		p->nvkms_params_ptr = 0;   /* stub fills a host VA at offset 8 */
	}

	r = nvkvm_virtio_ioctl_on_isolate(ctx, cmd, data, sizeof(*p),
					  aux, aux_sz, 0, &fault);

	/* memFd is IN and unchanged by the call; never let a stub-local fd or
	 * our handle_id be visible in the caller's params buffer. */
	if (have_memfd && aux)
		memcpy(aux, &orig_memfd, sizeof(orig_memfd));
	if (r >= 0 && aux &&
	    copy_to_user((void __user *)(uintptr_t)orig_ptr, aux, aux_sz))
		r = -EFAULT;
	/* IN fields: restore whatever the stub's pointer-scrubbing wrote back. */
	p->mem_size          = mem_size;
	p->nvkms_params_ptr  = orig_ptr;
	p->nvkms_params_size = orig_size;
	kfree(aux);
	if (r < 0)
		return (int)r;

	if (p->handle) {
		__u32 guest_handle = 0;
		int gret = nvkvm_gem_proxy_create(file, ctx, p->handle,
						  mem_size, &guest_handle);
		if (gret)
			return gret;
		p->handle = guest_handle;
	}
	return 0;
}

/*
 * GEM_ALLOC_NVKMS_MEMORY (0x0b): the NVIDIA gbm backend's scanout-buffer
 * allocation.  Flat scalar params (no embedded pointer) — forward as-is; the
 * host allocates a real bo on the stub's render node and writes its GEM handle
 * to p->handle.  That handle is valid only in the stub's DRM file, so mint a
 * guest-core proxy GEM (sized to the real allocation so AddFB2's size check
 * passes) and hand the guest handle back.  This is what makes gbm_bo_get_handle
 * return a usable card0 handle on the guest → compositors can AddFB2 + flip the
 * NVIDIA scanout bo on the virtual head (was the present-path keystone, #109).
 */
static int nvkvm_drm_fwd_gem_alloc_nvkms_memory(struct drm_device *dev,
						void *data,
						struct drm_file *file)
{
	struct drm_nvidia_gem_alloc_nvkms_memory_params *p = data;
	struct nvkvm_fd_ctx *ctx = file->driver_priv;
	unsigned int cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x0b,
				    struct drm_nvidia_gem_alloc_nvkms_memory_params);
	__u64 memory_size = p->memory_size;
	__u64 fault = 0;
	long r;

	(void)dev;
	if (!ctx)
		return -EBADF;

	r = nvkvm_virtio_ioctl_on_isolate(ctx, cmd, data, sizeof(*p),
					  NULL, 0, 0, &fault);
	if (r < 0)
		return (int)r;

	if (p->handle) {
		__u32 guest_handle = 0;
		int gret = nvkvm_gem_proxy_create(file, ctx, p->handle,
						  memory_size, &guest_handle);
		if (gret)
			return gret;
		p->handle = guest_handle;
	}
	return 0;
}

/* Indexed by (DRM_NVIDIA_* number) = (nr - DRM_COMMAND_BASE).  Gaps have a NULL
 * .func, which the DRM core rejects with -EINVAL (default-deny here too). */
static const struct drm_ioctl_desc nvkvm_drm_ioctls[] = {
	[0x01] = { .cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x01,
				   struct drm_nvidia_gem_import_nvkms_memory_params),
		   .func = nvkvm_drm_fwd_gem_import_nvkms_memory,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_GEM_IMPORT_NVKMS_MEMORY" },
	[0x03] = { .cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x03,
				   struct drm_nvidia_get_dev_info_params),
		   .func = nvkvm_drm_fwd_get_dev_info,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_GET_DEV_INFO" },
	[0x09] = { .cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x09,
				   struct drm_nvidia_gem_export_nvkms_memory_params),
		   .func = nvkvm_drm_fwd_gem_export_nvkms_memory,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_GEM_EXPORT_NVKMS_MEMORY" },
	[0x0b] = { .cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x0b,
				   struct drm_nvidia_gem_alloc_nvkms_memory_params),
		   .func = nvkvm_drm_fwd_gem_alloc_nvkms_memory,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_GEM_ALLOC_NVKMS_MEMORY" },
	[0x0e] = { .cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x0e,
				   struct drm_nvidia_gem_identify_object_params),
		   .func = nvkvm_drm_gem_identify_object,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_GEM_IDENTIFY_OBJECT" },
	[0x0f] = { .cmd = DRM_IO(NVKVM_DRM_COMMAND_BASE + 0x0f),
		   .func = nvkvm_drm_fwd_dmabuf_supported,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_DMABUF_SUPPORTED" },
	[0x18] = { .cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x18,
				   struct drm_nvidia_get_drm_file_unique_id_params),
		   .func = nvkvm_drm_fwd_get_drm_file_unique_id,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_GET_DRM_FILE_UNIQUE_ID" },
	[0x14] = { .cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x14,
				   struct drm_nvidia_semsurf_fence_ctx_create_params),
		   .func = nvkvm_drm_fwd_semsurf_fence_ctx_create,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_SEMSURF_FENCE_CTX_CREATE" },
	[0x15] = { .cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x15,
				   struct drm_nvidia_semsurf_fence_create_params),
		   .func = nvkvm_drm_fwd_semsurf_fence_create,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_SEMSURF_FENCE_CREATE" },
};

/* Each open of the render node gets its own forwarding context, sharing the
 * process's per-mm session + isolate (so renderD128 ⇄ /dev/nvidia0 correlate). */
static int nvkvm_drm_open(struct drm_device *dev, struct drm_file *file)
{
	struct nvkvm_fd_ctx *ctx;

	(void)dev;
	ctx = nvkvm_fd_ctx_open_dev(NVKVM_DEV_DRM_RD(0), O_RDWR);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	file->driver_priv = ctx;
	return 0;
}

static void nvkvm_drm_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct nvkvm_fd_ctx *ctx = file->driver_priv;

	(void)dev;
	if (ctx) {
		nvkvm_fd_ctx_close(ctx);
		file->driver_priv = NULL;
	}
}

const struct file_operations nvkvm_drm_fops = {   /* F-4: non-static for embedded-fd type-check */
	.owner          = THIS_MODULE,
	.open           = drm_open,
	.release        = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl   = drm_compat_ioctl,
	.poll           = drm_poll,
	.read           = drm_read,
	.mmap           = drm_gem_mmap,   /* #102: mmap dumb (shmem) scanout buffers */
	.llseek         = noop_llseek,
	/* Required by drm_open_helper() on >= 6.12 or every open() of the DRM
	 * nodes returns -EINVAL; see NVKVM_DRM_FOP_FLAGS in nvkvm_compat.h. */
	NVKVM_DRM_FOP_FLAGS
};

static const struct drm_driver nvkvm_drm_driver = {
	/* DRIVER_GEM: the DRM core only inits the per-file GEM object_idr (via
	 * drm_gem_open) and wires the core GEM ioctls (GEM_CLOSE, etc.) when this
	 * is set — required for our proxy GEM handles to resolve. */
	/* DRIVER_MODESET|ATOMIC: the guest-emulated virtual KMS head (#102,
	 * nvkvm_kms.c) lives on this same device so render + scanout share one DRM
	 * device (no cross-device PRIME). */
	.driver_features = DRIVER_RENDER | DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.open            = nvkvm_drm_open,
	.postclose       = nvkvm_drm_postclose,
	.ioctls          = nvkvm_drm_ioctls,
	.num_ioctls      = ARRAY_SIZE(nvkvm_drm_ioctls),
	.fops            = &nvkvm_drm_fops,
	/* #102: shmem-backed dumb buffers for the virtual KMS head's scanout
	 * (compositor/modetest fbs). Distinct from the proxy GEM render objects. */
	.dumb_create     = drm_gem_shmem_dumb_create,
	/* VERSION values — driver-constant, verified against host nvidia-drm. */
	.name            = "nvidia-drm",
	.desc            = "NVIDIA DRM driver",
	NVKVM_DRM_DRIVER_DATE
	.major           = 0,
	.minor           = 0,
	.patchlevel      = 0,
};

/* The NVIDIA Vulkan ICD requires the GPU's PCI device to be bound to the
 * "nvidia" kernel driver (it reads DRIVER=nvidia from the device's uevent).
 * Our emulated identity device has no real hardware, so this driver only CLAIMS
 * the device — it touches no registers/BARs; all GPU I/O is forwarded via the
 * virtio device + GPA window.  Naming it "nvidia" makes the uevent match the
 * host exactly. */
static int nvkvm_gpu_pci_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	(void)id;
	pci_set_drvdata(pdev, NULL);
	pci_info(pdev, "nvkvm: claimed NVIDIA-id device for DRM identity (no HW access)\n");
	return 0;
}

static void nvkvm_gpu_pci_remove(struct pci_dev *pdev)
{
	(void)pdev;
}

static const struct pci_device_id nvkvm_gpu_pci_ids[] = {
	{ PCI_DEVICE(NVKVM_PCI_VENDOR_NVIDIA, PCI_ANY_ID) },
	{ 0 }
};

static struct pci_driver nvkvm_gpu_pci_driver = {
	.name     = "nvidia",
	.id_table = nvkvm_gpu_pci_ids,
	.probe    = nvkvm_gpu_pci_probe,
	.remove   = nvkvm_gpu_pci_remove,
};
static bool nvkvm_gpu_pci_registered;

/* Register the render node under the emulated NVIDIA PCI device (vendor 0x10DE)
 * as parent, so the DRM core builds /sys/.../<nvidia-pci>/drm/renderD128 and the
 * ICD's renderD128 -> device -> vendor walk sees 0x10DE.  QEMU exposes that
 * identity-only PCI device (nvkvm-gpu).  If it is absent, fall back to the
 * virtio device as parent (node still registers, but the ICD won't bind it).
 * Graphics is optional: failure is logged, not fatal. */
int nvkvm_drm_init(struct device *fallback_parent)
{
	struct pci_dev *pdev;
	struct device *parent;
	struct drm_device *ddev;
	int ret;

	/* Bind the emulated NVIDIA-id device to the "nvidia" driver first, so it
	 * is bound (DRIVER=nvidia) by the time we parent the render node to it.
	 * pci_register_driver probes matching devices synchronously. */
	ret = pci_register_driver(&nvkvm_gpu_pci_driver);
	if (ret)
		pr_warn("nvkvm: pci_register_driver(nvidia) failed: %d\n", ret);
	else
		nvkvm_gpu_pci_registered = true;

	pdev = pci_get_device(NVKVM_PCI_VENDOR_NVIDIA, PCI_ANY_ID, NULL);
	parent = pdev ? &pdev->dev : fallback_parent;
	if (!pdev)
		pr_warn("nvkvm: no NVIDIA-id PCI device found; DRM parent falls back "
			"to virtio (Vulkan ICD will not bind)\n");

	ddev = drm_dev_alloc(&nvkvm_drm_driver, parent);
	if (IS_ERR(ddev)) {
		pr_warn("nvkvm: drm_dev_alloc failed: %ld (graphics disabled)\n",
			PTR_ERR(ddev));
		pci_dev_put(pdev);
		return PTR_ERR(ddev);
	}
	/* #102: bring up the guest-emulated virtual KMS head before the device
	 * goes live (mode objects must exist at register time). Non-fatal: on
	 * failure we still register as a render-only node. */
	ret = nvkvm_kms_init(ddev);
	if (ret)
		pr_warn("nvkvm: virtual KMS head init failed: %d (render-only)\n", ret);

	ret = drm_dev_register(ddev, 0);
	if (ret) {
		pr_warn("nvkvm: drm_dev_register failed: %d (graphics disabled)\n",
			ret);
		drm_dev_put(ddev);
		pci_dev_put(pdev);
		return ret;
	}
	nvkvm.drm_dev = ddev;
	/* The drm_device now holds its own reference on the parent; drop ours. */
	pci_dev_put(pdev);
	pr_info("nvkvm: registered nvidia-drm render node under %s (primary minor %d)\n",
		dev_name(parent),
		ddev->primary ? ddev->primary->index : -1);
	return 0;
}

void nvkvm_drm_fini(void)
{
	if (nvkvm.drm_dev) {
		drm_dev_unregister(nvkvm.drm_dev);
		drm_dev_put(nvkvm.drm_dev);
		nvkvm.drm_dev = NULL;
	}
	if (nvkvm_gpu_pci_registered) {
		pci_unregister_driver(&nvkvm_gpu_pci_driver);
		nvkvm_gpu_pci_registered = false;
	}
}
