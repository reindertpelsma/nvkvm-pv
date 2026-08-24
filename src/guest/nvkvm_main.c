// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_main.c — nvkvm-guest kernel module
 *
 * Exposes /dev/nvidia*, /dev/nvidiactl, and /dev/nvidia-uvm inside a KVM
 * guest VM and forwards all ioctls to the QEMU virtio-nvgpu backend running
 * on the host.
 *
 * Security model
 * ==============
 * This module sits at the guest-userspace → guest-kernel boundary. It treats
 * all data from userspace as untrusted and validates it before forwarding:
 *
 *   - All ioctl parameter sizes are checked against a compile-time table
 *     before the blob is copied from userspace.
 *   - The isolate's userspace VA layout mirrors the guest process's, so the
 *     host backend can dereference embedded pointers as-is. Pointer fields
 *     are preserved verbatim; no zeroing or translation is performed.
 *   - mmap requests are validated for range/flag sanity before forwarding.
 *
 * The module does NOT trust the host either: any host response that would
 * trigger mmap of guest physical memory is validated for GPA range bounds
 * before vm_insert_pfn is called.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/scatterlist.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/completion.h>

#include "../../src/common/nvkvm_proto.h"
#include "../../src/abi/nvgpu.h"
#include "../../src/abi/uvm.h"
#include "nvkvm.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("nvkvm contributors");
MODULE_DESCRIPTION("NVIDIA GPU passthrough for KVM guests via virtio-nvgpu");
MODULE_VERSION("0.1.0");

/* ── Global state ─────────────────────────────────────────────────────────── */

struct nvkvm_state nvkvm;

/* ── Module parameters ────────────────────────────────────────────────────── */

/*
 * How many /dev/nvidia<N> nodes to expose.  -1 (the default) asks the host,
 * via nvkvm_hostfile_discover_gpus(), and exposes exactly that many -- which
 * is what makes a multi-GPU host usable without the guest being told about
 * it.  A positive value overrides the probe; exposing more nodes than the
 * host has just means opens on the extra ones fail.
 */
static int num_gpus = -1;
module_param(num_gpus, int, 0444);
MODULE_PARM_DESC(num_gpus,
		 "Number of /dev/nvidia* devices to expose (default -1 = autodetect from the host)");

/* ── Forward declarations ─────────────────────────────────────────────────── */

static int  nvkvm_open(struct inode *inode, struct file *filp);
static int  nvkvm_release(struct inode *inode, struct file *filp);
static long nvkvm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static int  nvkvm_mmap(struct file *filp, struct vm_area_struct *vma);
static __poll_t nvkvm_poll(struct file *filp, poll_table *wait);

const struct file_operations nvkvm_fops = {   /* F-4: non-static so embedded-fd translation can type-check against it */
	.owner          = THIS_MODULE,
	.open           = nvkvm_open,
	.release        = nvkvm_release,
	.unlocked_ioctl = nvkvm_ioctl,
	.compat_ioctl   = nvkvm_ioctl,
	.mmap           = nvkvm_mmap,
	.poll           = nvkvm_poll,
};

/* ── Device registration ──────────────────────────────────────────────────── */

/*
 * libnvidia-ml and nvidia-smi call mknodat(195, 255) before opening nvidiactl
 * (the major is hardcoded in the library).  We must register at that exact
 * major so the device can be opened after the library recreates the node.
 * Similarly, nvidia0 lives at major 195 minor 0 in the real NVIDIA driver.
 * Fall back to dynamic allocation if 195 is already taken.
 */
#define NV_NVIDIA_MAJOR 195

static char *nvkvm_devnode(NVKVM_DEVNODE_DEV dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;
}

static int __init register_devices(void)
{
	int ret, i, detected;
	dev_t devno;

	nvkvm.class = nvkvm_class_create("nvkvm");
	if (IS_ERR(nvkvm.class))
		return PTR_ERR(nvkvm.class);
	nvkvm.class->devnode = nvkvm_devnode;

	/* /dev/nvidiactl — prefer major 195 minor 255 (NVIDIA standard) */
	devno = MKDEV(NV_NVIDIA_MAJOR, NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE);
	ret = register_chrdev_region(devno, 1, "nvidiactl");
	if (ret)
		ret = alloc_chrdev_region(&devno,
					  NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE,
					  1, "nvidiactl");
	if (ret)
		goto err_class;
	nvkvm.ctl_major = MAJOR(devno);
	cdev_init(&nvkvm.ctl_cdev, &nvkvm_fops);
	nvkvm.ctl_cdev.owner = THIS_MODULE;
	ret = cdev_add(&nvkvm.ctl_cdev, devno, 1);
	if (ret)
		goto err_ctl_region;
	device_create(nvkvm.class, NULL, devno, NULL, "nvidiactl");

	/* /dev/nvidia0 .. /dev/nvidia{N-1} — prefer major 195 minor 0..N-1.
	 *
	 * Discovery runs unconditionally, even under an explicit num_gpus=,
	 * because it is also what fills nvkvm.gpu_bdf[] for the synthetic
	 * /proc/driver/nvidia/gpus/<bdf>/ tree that nvkvm_hostfile_init()
	 * builds a few lines later. */
	detected = nvkvm_hostfile_discover_gpus();
	if (detected < 0) {
		/*
		 * -ENODEV here is not a failure to discover, it is the absence of
		 * the device itself.  It used to surface as "-12" (-ENOMEM), which
		 * sent people looking for a memory problem; say what it is.
		 */
		if (detected == -ENODEV)
			pr_warn("nvkvm: no nvkvm virtio device is attached to this VM.\n"
				"nvkvm: the device nodes below are created but every open will\n"
				"nvkvm: return -ENODEV. Add nvkvm's device to the QEMU command line.\n");
		else
			pr_warn("nvkvm: host GPU discovery failed (%d); assuming 1\n",
				detected);
		detected = 0;
	}
	if (num_gpus > 0)
		nvkvm.num_gpus = min(num_gpus, NVKVM_MAX_GPUS);
	else
		nvkvm.num_gpus = detected > 0 ? detected : 1;
	pr_info("nvkvm: host reports %d GPU(s); exposing %d\n",
		detected, nvkvm.num_gpus);
	devno = MKDEV(NV_NVIDIA_MAJOR, 0);
	ret = register_chrdev_region(devno, nvkvm.num_gpus, "nvidia");
	if (ret)
		ret = alloc_chrdev_region(&devno, 0, nvkvm.num_gpus, "nvidia");
	if (ret)
		goto err_ctl;
	nvkvm.gpu_devno_base = devno;
	nvkvm.gpu_major = MAJOR(nvkvm.gpu_devno_base);
	for (i = 0; i < nvkvm.num_gpus; i++) {
		devno = MKDEV(nvkvm.gpu_major, i);
		cdev_init(&nvkvm.gpu_cdevs[i], &nvkvm_fops);
		nvkvm.gpu_cdevs[i].owner = THIS_MODULE;
		ret = cdev_add(&nvkvm.gpu_cdevs[i], devno, 1);
		if (ret) {
			pr_err("nvkvm: failed to add /dev/nvidia%d: %d\n", i, ret);
			goto err_gpu;
		}
		device_create(nvkvm.class, NULL, devno, NULL, "nvidia%d", i);
	}

	/* /dev/nvidia-uvm (minor 0) + /dev/nvidia-uvm-tools (minor 1) — dynamic
	 * major, two minors.  libcuda mknods /dev/nvidia-uvm-tools if absent
	 * and fails with CUDA_ERROR_OPERATING_SYSTEM (304) when mknod is
	 * denied — the simplest fix is to expose it from the module. */
	ret = alloc_chrdev_region(&nvkvm.uvm_devno, 0, 2, "nvidia-uvm");
	if (ret)
		goto err_gpu;
	nvkvm.uvm_major = MAJOR(nvkvm.uvm_devno);
	cdev_init(&nvkvm.uvm_cdev, &nvkvm_fops);
	nvkvm.uvm_cdev.owner = THIS_MODULE;
	ret = cdev_add(&nvkvm.uvm_cdev, nvkvm.uvm_devno, 2);
	if (ret)
		goto err_uvm_region;
	device_create(nvkvm.class, NULL, nvkvm.uvm_devno, NULL, "nvidia-uvm");
	device_create(nvkvm.class, NULL,
		      MKDEV(nvkvm.uvm_major, 1), NULL, "nvidia-uvm-tools");

	/*
	 * /dev/nvidia-modeset (major 195, minor 254) — NVKMS config device.
	 * libnvidia-glsi opens it during EGL/Vulkan device init.  Non-fatal if
	 * registration fails (e.g. minor already taken): graphics degrades but
	 * the compute path is unaffected.  nvkvm_devnode() makes it 0666 so an
	 * unprivileged guest process can open it.
	 *
	 * Only when QEMU enabled graphics for this VM.  The virtio probe runs
	 * before register_devices (register_virtio_driver probes synchronously),
	 * so graphics_enabled already reflects the host's NVKVM_CONFIG_F_GRAPHICS
	 * by now; compute-only VMs never create the modeset device.
	 */
#ifdef NVKVM_GRAPHICS
	if (nvkvm.graphics_enabled) {
		dev_t mdev = MKDEV(NV_MAJOR_DEVICE_NUMBER,
				   NV_MINOR_DEVICE_NUMBER_MODESET);
		if (register_chrdev_region(mdev, 1, "nvidia-modeset") == 0) {
			cdev_init(&nvkvm.modeset_cdev, &nvkvm_fops);
			nvkvm.modeset_cdev.owner = THIS_MODULE;
			if (cdev_add(&nvkvm.modeset_cdev, mdev, 1) == 0) {
				device_create(nvkvm.class, NULL, mdev, NULL,
					      "nvidia-modeset");
				nvkvm.modeset_registered = true;
			} else {
				unregister_chrdev_region(mdev, 1);
				pr_warn("nvkvm: nvidia-modeset cdev_add failed\n");
			}
		} else {
			pr_warn("nvkvm: could not reserve nvidia-modeset (195:254)\n");
		}
	}
#endif /* NVKVM_GRAPHICS */

	pr_info("nvkvm: registered nvidiactl (major %u), nvidia0-%d (major %u), nvidia-uvm/uvm-tools (major %u)\n",
		nvkvm.ctl_major, nvkvm.num_gpus - 1, nvkvm.gpu_major,
		nvkvm.uvm_major);
	return 0;

err_uvm_region:
	unregister_chrdev_region(nvkvm.uvm_devno, 2);
err_gpu:
	while (--i >= 0) {
		cdev_del(&nvkvm.gpu_cdevs[i]);
		device_destroy(nvkvm.class, MKDEV(nvkvm.gpu_major, i));
	}
	unregister_chrdev_region(nvkvm.gpu_devno_base, nvkvm.num_gpus);
err_ctl:
	cdev_del(&nvkvm.ctl_cdev);
	device_destroy(nvkvm.class, MKDEV(nvkvm.ctl_major,
				    NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE));
err_ctl_region:
	unregister_chrdev_region(MKDEV(nvkvm.ctl_major,
				       NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE), 1);
err_class:
	class_destroy(nvkvm.class);
	return ret;
}

/* Tear down /dev/nvidia-modeset. Idempotent — safe to call from probe (when
 * QEMU disables graphics) and from module teardown. */
static void nvkvm_modeset_unregister(void)
{
	if (!nvkvm.modeset_registered)
		return;
	{
		dev_t mdev = MKDEV(NV_MAJOR_DEVICE_NUMBER,
				   NV_MINOR_DEVICE_NUMBER_MODESET);
		device_destroy(nvkvm.class, mdev);
		cdev_del(&nvkvm.modeset_cdev);
		unregister_chrdev_region(mdev, 1);
	}
	nvkvm.modeset_registered = false;
}

static void unregister_devices(void)
{
	int i;

	nvkvm_modeset_unregister();

	device_destroy(nvkvm.class, MKDEV(nvkvm.uvm_major, 1));
	device_destroy(nvkvm.class, nvkvm.uvm_devno);
	cdev_del(&nvkvm.uvm_cdev);
	unregister_chrdev_region(nvkvm.uvm_devno, 2);

	for (i = nvkvm.num_gpus - 1; i >= 0; i--) {
		device_destroy(nvkvm.class, MKDEV(nvkvm.gpu_major, i));
		cdev_del(&nvkvm.gpu_cdevs[i]);
	}
	unregister_chrdev_region(nvkvm.gpu_devno_base, nvkvm.num_gpus);

	device_destroy(nvkvm.class,
		       MKDEV(nvkvm.ctl_major,
			     NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE));
	cdev_del(&nvkvm.ctl_cdev);
	unregister_chrdev_region(MKDEV(nvkvm.ctl_major,
				       NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE),
				 1);

	class_destroy(nvkvm.class);
}

/* ── Device open/release ──────────────────────────────────────────────────── */

/*
 * Map this session's command-buffer ring (docs/design/command_buffer.md).
 * QEMU placed the ring memfd in the sparse GPA window at isolate create; we
 * fetch its GPA and memremap it so the kmd can run the SPSC fast path.  Pure
 * optimisation: any failure leaves ring_base NULL and the session keeps using
 * the virtqueue path.  Called once, under isolate_lock.
 */
/* ── Command-buffer fast path (docs/design/command_buffer.md, Phase 4c) ──────
 *
 * Producers (any guest thread issuing a flat RM_CONTROL) serialise on
 * ring_lock, so the request ring has ONE producer and the response ring ONE
 * consumer (the lock holder reads back its own response) — SPSC preserved.
 * A per-session pump kthread keeps the isolate spinning via ENTER_LOOP while
 * work is queued; producers wake it after publishing.  Level-triggered
 * re-evaluation (the pump re-checks has_work after every enter_loop) is the
 * lost-wakeup-free keystone — see the design doc.
 */
#define NVKVM_RING_WAIT_SPIN_TIGHT   200000u  /* tight cpu_relax spins        */
#define NVKVM_RING_WAIT_MAX_RESCHED  5000u    /* resched rounds before giving up */

/* Tunable idle window passed to the stub (spin iterations before the consumer
 * loop exits).  0 → stub default (2000).  Crank it up to keep the isolate
 * spinning across inter-control gaps so one enter_loop batches many controls —
 * the lever to test whether virtqueue-txn count actually drops. */
static unsigned int nvkvm_ring_idle_us;
module_param_named(ring_idle_us, nvkvm_ring_idle_us, uint, 0644);
MODULE_PARM_DESC(ring_idle_us, "stub consumer-loop idle window in spin iters (0=default; capped in stub)");

/*
 * "Is there queued work?" for the pump.
 *
 * s->req_ring is NULL-stored by nvkvm_session_put() under sessions_lock —
 * which this kthread does not hold, and which is released BEFORE
 * nvkvm_session_stop_pump() runs, so the pump is deliberately still alive
 * across that store.  The predicate therefore has to load the pointer exactly
 * once and use that value; the previous form loaded s->req_ring twice per
 * evaluation (test, then argument), leaving the compiler free to re-load a
 * pointer that had become NULL in between and hand it to nvkvm_ring_has_work()
 * to dereference.  READ_ONCE + a local closes the window.
 */
static bool nvkvm_pump_has_work(struct nvkvm_session *s)
{
	struct nvkvm_ring *r = READ_ONCE(s->req_ring);

	return r && nvkvm_ring_has_work(r);
}

static int nvkvm_pump_fn(void *data)
{
	struct nvkvm_session *s = data;

	while (!kthread_should_stop()) {
		wait_event_interruptible(s->pump_wq,
			kthread_should_stop() || nvkvm_pump_has_work(s));
		while (!kthread_should_stop() && nvkvm_pump_has_work(s)) {
			u64 head = 0;
			int ret = nvkvm_virtio_enter_loop((unsigned int)s->id,
							  nvkvm_ring_idle_us,
							  &head);
			if (ret) {
				/* dead isolate / missing ring → stop driving */
				if (!kthread_should_stop())
					msleep(2);
				break;
			}
		}
	}
	return 0;
}

void nvkvm_session_stop_pump(struct nvkvm_session *session)
{
	if (session->pump_task) {
		kthread_stop(session->pump_task);   /* wakes + waits for exit */
		session->pump_task = NULL;
	}
}

/*
 * Spin on the response ring for txn's reply; copy param/aux back in place.
 * Caller holds ring_lock (sole consumer).  Returns 0/-errno, or
 * NVKVM_RING_TRY_PUNT if the stub declined to execute (use the slow path).
 */
static int nvkvm_ring_wait_resp(struct nvkvm_session *s, u32 txn,
				void *params_buf, size_t param_size,
				void *aux_buf, size_t aux_size,
				u32 *nvstatus_out)
{
	u32 tight = 0, resched = 0;

	for (;;) {
		u8 *pay;
		u32 len;
		u64 total;
		int rc = nvkvm_ring_peek(s->resp_ring, s->ring_bytes, &pay, &len, &total);

		if (rc == NVKVM_RING_OK) {
			struct nvkvm_ring_ioctl_resp rh;
			int out = NVKVM_RING_TRY_PUNT;

			if (len >= sizeof(rh)) {
				memcpy(&rh, pay, sizeof(rh));
				if (rh.txn_id == txn) {
					if (rh.flags & NVKVM_RING_RESP_PUNT) {
						out = NVKVM_RING_TRY_PUNT;
					} else {
						if (rh.param_size &&
						    rh.param_size <= param_size &&
						    sizeof(rh) + rh.param_size <= len)
							memcpy(params_buf,
							       pay + sizeof(rh),
							       rh.param_size);
						if (rh.aux_size &&
						    rh.aux_size <= aux_size &&
						    sizeof(rh) + rh.param_size +
							    rh.aux_size <= len)
							memcpy(aux_buf,
							       pay + sizeof(rh) +
								     rh.param_size,
							       rh.aux_size);
						if (nvstatus_out)
							*nvstatus_out = rh.nvstatus;
						out = (rh.retval < 0) ?
							rh.retval : 0;
					}
					nvkvm_ring_pop(s->resp_ring, total);
					return out;
				}
			}
			/* malformed / stale record → drop and keep looking */
			nvkvm_ring_pop(s->resp_ring, total);
			continue;
		}
		if (rc == NVKVM_RING_BAD)
			return -EIO;

		if (++tight < NVKVM_RING_WAIT_SPIN_TIGHT) {
			cpu_relax();
			continue;
		}
		tight = 0;
		if (++resched > NVKVM_RING_WAIT_MAX_RESCHED)
			return -EIO;   /* stub wedged */
		cond_resched();
	}
}

/*
 * Try to forward a flat RM_CONTROL over the SPSC ring.  Eligibility mirrors the
 * stub's accept set; the stub PUNTs anything needing per-control marshalling
 * (InfoList/GET_BUILD_VERSION/EXPORT) and the caller falls back to the
 * virtqueue.  Controls needing guest- or QEMU-side handling (GET_PID_INFO,
 * EXPORT) are excluded by the caller before reaching here.
 */
/*
 * Runtime toggle for the SPSC command-buffer fast path.  Default OFF: the ring
 * is correct + validated (HW: 1446 flat RM_CONTROLs/decode offloaded, byte-exact
 * matmul, 4x multi-proc, zero regression) but measurement showed it does NOT
 * improve LLM decode throughput — control-RTT is only ~1-2% of per-token time
 * (the bottleneck is GPU compute + the mapped doorbell/fence launch path), and
 * keeping the isolate spinning costs host CPU.  Enable it for workloads that ARE
 * control-latency-bound:  echo 1 > /sys/module/nvkvm_guest/parameters/ring_enable
 * See docs/design/command_buffer.md "Measured results".
 */
static bool nvkvm_ring_enable;   /* default false */
module_param_named(ring_enable, nvkvm_ring_enable, bool, 0644);
MODULE_PARM_DESC(ring_enable, "route flat RM_CONTROLs over the SPSC fast ring (default off)");

int nvkvm_session_ring_try(struct nvkvm_fd_ctx *ctx, unsigned int cmd,
			   void *params_buf, size_t param_size,
			   void *aux_buf, size_t aux_size, u32 *nvstatus_out)
{
	struct nvkvm_session *s = ctx->session;

	if (!nvkvm_ring_enable)
		return NVKVM_RING_TRY_PUNT;
	struct nvkvm_ring_ioctl_req rh;
	u32 payload, txn;
	u64 total;
	u8 *p;
	int rc;

	if (!s->req_ring || !s->pump_task)
		return NVKVM_RING_TRY_PUNT;
	if (_IOC_NR(cmd) != NV_ESC_RM_CONTROL ||
	    param_size != sizeof(struct nvos54_parameters) ||
	    param_size > NVKVM_RING_MAX_PARAM ||
	    aux_size > NVKVM_RING_MAX_AUX)
		return NVKVM_RING_TRY_PUNT;

	mutex_lock(&s->ring_lock);
	if (!s->req_ring) {
		mutex_unlock(&s->ring_lock);
		return NVKVM_RING_TRY_PUNT;
	}

	payload = (u32)sizeof(rh) + (u32)param_size + (u32)aux_size;
	p = nvkvm_ring_reserve(s->req_ring, s->ring_bytes, payload, &total);
	if (!p) {                       /* ring full → slow path this time */
		mutex_unlock(&s->ring_lock);
		return NVKVM_RING_TRY_PUNT;
	}

	txn = ++s->ring_txn_next;
	if (!txn)
		txn = ++s->ring_txn_next;
	rh.txn_id      = txn;
	rh.handle_id   = ctx->handle_id;
	rh.cmd         = cmd;
	rh.param_size  = (u32)param_size;
	rh.aux_size    = (u32)aux_size;
	rh.abi_profile = 0;   /* RM_CONTROL nvstatus offset is fixed (nvos54@28) */
	memcpy(p, &rh, sizeof(rh));
	if (param_size)
		memcpy(p + sizeof(rh), params_buf, param_size);
	if (aux_size)
		memcpy(p + sizeof(rh) + param_size, aux_buf, aux_size);
	nvkvm_ring_commit(s->req_ring, total);

	wake_up(&s->pump_wq);   /* drive the stub */

	rc = nvkvm_ring_wait_resp(s, txn, params_buf, param_size,
				  aux_buf, aux_size, nvstatus_out);
	mutex_unlock(&s->ring_lock);
	return rc;
}

static void nvkvm_session_setup_ring(struct nvkvm_session *session)
{
	u64 gpa = 0;
	u32 ring_bytes = 0, region;
	void *base;
	int ret;

	if (session->ring_base)
		return;   /* already mapped */

	ret = nvkvm_virtio_setup_ring((unsigned int)session->id,
				      &gpa, &ring_bytes);
	if (ret || !gpa || !nvkvm_ring_size_ok(ring_bytes)) {
		pr_info("nvkvm: session %d: no command-buffer ring (ret=%d) — virtqueue path\n",
			session->id, ret);
		return;
	}

	region = (u32)nvkvm_ring_region_size(ring_bytes);
	if (!nvkvm_gpa_in_mmap_window((unsigned long)gpa, region)) {
		pr_warn("nvkvm: session %d ring gpa 0x%llx outside mmap window\n",
			session->id, gpa);
		return;
	}

	/* WB-cached, directly dereferenceable; arch_memremap_wb handles the
	 * non-RAM (reservation-BAR) range on x86. */
	base = memremap((resource_size_t)gpa, region, MEMREMAP_WB);
	if (!base) {
		pr_warn("nvkvm: session %d memremap(0x%llx, %u) failed\n",
			session->id, gpa, region);
		return;
	}

	session->ring_base        = base;
	session->ring_gpa         = gpa;
	session->ring_region_size = region;
	session->ring_bytes       = ring_bytes;
	session->req_ring  = (struct nvkvm_ring *)base;
	session->resp_ring = (struct nvkvm_ring *)
		((u8 *)base + nvkvm_ring_resp_off(ring_bytes));

	/*
	 * 3-way probe: QEMU initialised both control blocks with size==ring_bytes
	 * and head==tail==0.  Reading them back here proves the guest maps the
	 * SAME physical pages as QEMU and the isolate (all three share one memfd).
	 */
	if (session->req_ring->size == ring_bytes &&
	    session->resp_ring->size == ring_bytes &&
	    session->req_ring->head == session->req_ring->tail) {
		pr_info("nvkvm: session %d RING MAPPED gpa=0x%llx bytes=%u — 3-way OK (req.size=%llu resp.size=%llu)\n",
			session->id, gpa, ring_bytes,
			(unsigned long long)session->req_ring->size,
			(unsigned long long)session->resp_ring->size);
		/* Start the pump that keeps the isolate spinning on the ring. */
		session->pump_task = kthread_run(nvkvm_pump_fn, session,
						 "nvkvm-pump-%d", session->id);
		if (IS_ERR(session->pump_task)) {
			pr_warn("nvkvm: session %d pump start failed — disabling ring\n",
				session->id);
			session->pump_task = NULL;
			memunmap(base);
			session->ring_base = NULL;
			session->req_ring = session->resp_ring = NULL;
		}
	} else {
		pr_warn("nvkvm: session %d ring readback mismatch (req.size=%llu resp.size=%llu want=%u) — disabling ring\n",
			session->id,
			(unsigned long long)session->req_ring->size,
			(unsigned long long)session->resp_ring->size, ring_bytes);
		memunmap(base);
		session->ring_base = NULL;
		session->req_ring = session->resp_ring = NULL;
	}
}

/* Ensure the session has an isolate; creates one if isolate_id == 0. */
static int nvkvm_ensure_isolate(struct nvkvm_session *session)
{
	__u32 isolate_id;
	int ret;

	mutex_lock(&session->isolate_lock);
	if (session->isolate_id) {
		mutex_unlock(&session->isolate_lock);
		return 0;
	}
	ret = nvkvm_virtio_create_isolate((unsigned int)session->id, &isolate_id);
	if (ret == 0) {
		session->isolate_id = isolate_id;
		nvkvm_session_setup_ring(session);   /* map the ring (best-effort) */
	}
	mutex_unlock(&session->isolate_lock);
	return ret;
}

/*
 * Build a forwarding fd-context for dev_id: get/create the per-mm session,
 * ensure its isolate, and open the device handle on the stub.  Shared by the
 * char-device open path (nvkvm_open) and the DRM render-node driver
 * (nvkvm_drm.c) so both share the SAME session/isolate for a given mm — that
 * is what lets the renderD128 handle correlate with the process's /dev/nvidia0
 * RM device.  Returns ERR_PTR on failure.  Does NOT allocate UVM state; the
 * UVM caller adds it.
 */
/* ── #101 async-event registry ──────────────────────────────────────────────
 * VQ_EVT notifications from the host carry (isolate_id, handle_id). An OS-event
 * fd's poll() blocks on ctx->poll_wq; this registry lets the VQ_EVT virtqueue
 * callback (softirq) find the matching ctx and wake it immediately, instead of
 * libnvidia-* falling back to a ~18 ms poll-timeout-then-recheck per completion
 * (the NVENC throughput bottleneck). The shared lock also pins ctx lifetime:
 * a concurrent close()'s unregister blocks until any in-flight deliver() ends. */
static LIST_HEAD(nvkvm_evt_ctx_list);
static DEFINE_SPINLOCK(nvkvm_evt_ctx_lock);

void nvkvm_evt_ctx_register(struct nvkvm_fd_ctx *ctx)
{
	unsigned long flags;
	spin_lock_irqsave(&nvkvm_evt_ctx_lock, flags);
	list_add(&ctx->evt_node, &nvkvm_evt_ctx_list);
	spin_unlock_irqrestore(&nvkvm_evt_ctx_lock, flags);
}

void nvkvm_evt_ctx_unregister(struct nvkvm_fd_ctx *ctx)
{
	unsigned long flags;
	spin_lock_irqsave(&nvkvm_evt_ctx_lock, flags);
	if (!list_empty(&ctx->evt_node))
		list_del_init(&ctx->evt_node);
	spin_unlock_irqrestore(&nvkvm_evt_ctx_lock, flags);
}

void nvkvm_evt_deliver(__u32 isolate_id, __u32 handle_id, __u32 events)
{
	struct nvkvm_fd_ctx *ctx;
	unsigned long flags;
	spin_lock_irqsave(&nvkvm_evt_ctx_lock, flags);
	list_for_each_entry(ctx, &nvkvm_evt_ctx_list, evt_node) {
		if (ctx->handle_id == handle_id && ctx->session &&
		    ctx->session->isolate_id == isolate_id) {
			atomic_or((int)events, &ctx->poll_events);
			/* #127: the host's one-shot arm fired; let the next
			 * wait re-arm. */
			atomic_set(&ctx->poll_armed, 0);
			wake_up_interruptible(&ctx->poll_wq);
		}
	}
	spin_unlock_irqrestore(&nvkvm_evt_ctx_lock, flags);
}

struct nvkvm_fd_ctx *nvkvm_fd_ctx_open_dev(int dev_id, unsigned int flags)
{
	struct nvkvm_fd_ctx *ctx;
	__u32 handle_id = 0;
	int ret;

	/*
	 * The single gate for BOTH open paths -- nvkvm_open() on the five
	 * character devices and nvkvm_drm_open() on the DRM node.  With no nvkvm
	 * virtio device the transport was never initialised, and continuing here
	 * reaches nvkvm_ensure_isolate() -> ... -> nvkvm_send_sync() and a NULL
	 * deref.  Refusing the open makes an unsupported QEMU merely useless
	 * rather than fatal: userspace gets ENODEV from open(), which every
	 * NVIDIA client already handles as "no GPU here".
	 */
	if (!nvkvm_transport_ready(&nvkvm)) {
		pr_warn_once("nvkvm: no nvkvm virtio device -- refusing opens with -ENODEV.\n"
			     "nvkvm: the module was force-loaded (modules-load.d) on a VM whose QEMU\n"
			     "nvkvm: exports no nvkvm device. Nothing here can work; this is not a bug\n"
			     "nvkvm: in the guest image.\n");
		return ERR_PTR(-ENODEV);
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx->dev_id  = dev_id;
	refcount_set(&ctx->refs, 1);   /* G-6: the open file's reference */
	ctx->session = nvkvm_session_get_or_create(current->mm, current->tgid);
	if (IS_ERR(ctx->session)) {
		ret = PTR_ERR(ctx->session);
		kfree(ctx);
		return ERR_PTR(ret);
	}
	init_waitqueue_head(&ctx->poll_wq);
	atomic_set(&ctx->poll_events, 0);
	atomic_set(&ctx->poll_armed, 0);   /* #127 */
	spin_lock_init(&ctx->mmap_lock);
	INIT_LIST_HEAD(&ctx->mmap_regions);
	mutex_init(&ctx->cpu_pages_lock);
	INIT_LIST_HEAD(&ctx->cpu_pages);
	INIT_LIST_HEAD(&ctx->evt_node);   /* #101: not yet in the registry */

	ret = nvkvm_ensure_isolate(ctx->session);
	if (ret)
		goto err;
	ret = nvkvm_virtio_open_nvidia_handle(dev_id, flags,
					      (unsigned int)ctx->session->id,
					      &handle_id);
	if (ret)
		goto err;
	ctx->handle_id = handle_id;
	nvkvm_evt_ctx_register(ctx);   /* #101: now discoverable by VQ_EVT */
	return ctx;
err:
	nvkvm_session_put(ctx->session);
	kfree(ctx);
	return ERR_PTR(ret);
}

/* Actual teardown — runs only when the last reference (open file + any proxy
 * GEMs) drops.  See the refs comment in struct nvkvm_fd_ctx. */
static void nvkvm_fd_ctx_destroy(struct nvkvm_fd_ctx *ctx)
{
	/* #101: leave the async-event registry first so no VQ_EVT deliver() can
	 * touch this ctx after we start tearing it down (unregister blocks until
	 * any in-flight deliver() under the shared lock completes). */
	nvkvm_evt_ctx_unregister(ctx);

	if (ctx->handle_id && ctx->session->isolate_id)
		nvkvm_virtio_close_handle_on_isolate(ctx->handle_id,
						     ctx->session->isolate_id);
	if (ctx->handle_id) {
		nvkvm_virtio_close_handle(ctx->handle_id);
		ctx->handle_id = 0;
	}

	/* Tear down any CPU page migrations for this fd */
	nvkvm_cpu_pages_free(ctx);
	mutex_destroy(&ctx->cpu_pages_lock);

	/* Tear down any mmap regions owned by this FD */
	nvkvm_mmap_release_fd(ctx);

	/* Free UVM state-machine state if this was a UVM fd. */
	if (ctx->uvm_state) {
		struct nvkvm_uvm_gpu_reg *g, *gtmp;
		struct nvkvm_uvm_vas_reg *v, *vtmp;
		struct nvkvm_uvm_range_group *r, *rtmp;
		struct nvkvm_uvm_mapping_intent *i, *itmp;
		struct nvkvm_uvm_realization *re, *retmp;
		list_for_each_entry_safe(g, gtmp,
			&ctx->uvm_state->registered_gpus, list)
			kfree(g);
		list_for_each_entry_safe(v, vtmp,
			&ctx->uvm_state->registered_va_spaces, list)
			kfree(v);
		list_for_each_entry_safe(r, rtmp,
			&ctx->uvm_state->range_groups, list)
			kfree(r);
		list_for_each_entry_safe(i, itmp,
			&ctx->uvm_state->intents, list) {
			kfree(i->params);
			kfree(i);
		}
		list_for_each_entry_safe(re, retmp,
			&ctx->uvm_state->realizations, list)
			kfree(re);
		mutex_destroy(&ctx->uvm_state->lock);
		kfree(ctx->uvm_state);
	}

	nvkvm_session_put(ctx->session);
	kfree(ctx);
}

/* Drop the open file's reference (char-device release / DRM postclose). The
 * ctx survives until any proxy GEMs that referenced it are freed too. */
void nvkvm_fd_ctx_close(struct nvkvm_fd_ctx *ctx)
{
	nvkvm_fd_ctx_put(ctx);
}

void nvkvm_fd_ctx_get(struct nvkvm_fd_ctx *ctx)
{
	refcount_inc(&ctx->refs);
}

void nvkvm_fd_ctx_put(struct nvkvm_fd_ctx *ctx)
{
	if (refcount_dec_and_test(&ctx->refs))
		nvkvm_fd_ctx_destroy(ctx);
}

static int nvkvm_open(struct inode *inode, struct file *filp)
{
	struct nvkvm_fd_ctx *ctx;
	int dev_id;

	/*
	 * Determine which device is being opened.
	 *
	 * nvidiactl and nvidia0..N share major 195 (NV_NVIDIA_MAJOR) so we MUST
	 * check BOTH major AND minor to identify nvidiactl (minor=255).  Checking
	 * major alone incorrectly classifies every nvidia0 open as NVKVM_DEV_CTL.
	 *
	 * nvidia-uvm has a dynamic (distinct) major so major comparison is enough.
	 */
	if (imajor(inode) == nvkvm.ctl_major &&
	    iminor(inode) == NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE)
		dev_id = NVKVM_DEV_CTL;
	else if (imajor(inode) == NV_MAJOR_DEVICE_NUMBER &&
		 iminor(inode) == NV_MINOR_DEVICE_NUMBER_MODESET)
		dev_id = NVKVM_DEV_MODESET;
	else if (imajor(inode) == nvkvm.uvm_major)
		dev_id = NVKVM_DEV_UVM;
	else
		dev_id = NVKVM_DEV_GPU(iminor(inode));

	ctx = nvkvm_fd_ctx_open_dev(dev_id, filp->f_flags);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	/* State-machine state for UVM fds — see docs/STATE_MACHINE_PLAN.md. */
	if (dev_id == NVKVM_DEV_UVM) {
		ctx->uvm_state = kzalloc(sizeof(*ctx->uvm_state), GFP_KERNEL);
		if (!ctx->uvm_state) {
			nvkvm_fd_ctx_close(ctx);
			return -ENOMEM;
		}
		mutex_init(&ctx->uvm_state->lock);
		INIT_LIST_HEAD(&ctx->uvm_state->registered_gpus);
		INIT_LIST_HEAD(&ctx->uvm_state->registered_va_spaces);
		INIT_LIST_HEAD(&ctx->uvm_state->range_groups);
		INIT_LIST_HEAD(&ctx->uvm_state->intents);
		INIT_LIST_HEAD(&ctx->uvm_state->realizations);
	}

	filp->private_data = ctx;
	pr_debug("nvkvm: opened dev_id=%d handle_id=%u isolate_id=%u tgid=%d\n",
		 dev_id, ctx->handle_id,
		 ctx->session->isolate_id, current->tgid);
	return 0;
}

static int nvkvm_release(struct inode *inode, struct file *filp)
{
	struct nvkvm_fd_ctx *ctx = filp->private_data;

	if (!ctx)
		return 0;

	nvkvm_fd_ctx_close(ctx);
	filp->private_data = NULL;
	return 0;
}

/* ── ioctl forwarding ─────────────────────────────────────────────────────── */

/*
 * GPU process enumeration (nvidia-smi).  NV2080_CTRL_CMD_GPU_GET_PIDS asks the
 * RM "which pids have GPU allocations".  We must NOT forward it: the stub runs
 * in its own pid namespace and as a distinct RM client, so the host RM (a)
 * cannot see other isolates' pids and (b) would report host pids meaningless
 * to the guest.  Instead the guest module — the authority on which guest
 * processes hold GPU sessions — synthesizes the answer from its session table.
 * Returns 1 if it handled the control (caller copies params_buf back + returns
 * 0), 0 to fall through to normal forwarding.
 */
#define NVKVM_NV2080_GET_PIDS      0x2080018du
#define NVKVM_GET_PIDS_MAX         950u
struct nvkvm_get_pids_params {
	__u32 id_type;
	__u32 id;
	__u32 pid_tbl_count;
	__u32 pid_tbl[NVKVM_GET_PIDS_MAX];
};

static int nvkvm_synth_get_pids(void *params_buf, __u32 param_size)
{
	struct nvos54_parameters *ctl = params_buf;
	struct nvkvm_get_pids_params *gp;
	struct nvkvm_session *s;
	void __user *up;
	size_t outsz;
	__u32 n = 0;
	int id;

	if (param_size != sizeof(*ctl) || ctl->cmd != NVKVM_NV2080_GET_PIDS)
		return 0;
	up = (void __user *)(uintptr_t)ctl->params;
	if (!up || ctl->params_size < offsetof(struct nvkvm_get_pids_params, pid_tbl))
		return 0;

	gp = kzalloc(sizeof(*gp), GFP_KERNEL);
	if (!gp)
		return 0;   /* fall through to forwarding on OOM */

	/*
	 * The guest's GPU processes = the processes holding nvkvm sessions,
	 * rendered in the *caller's* pid namespace.  pid_vnr() returns the
	 * number the calling task's active pid ns uses for that pid, or 0 if
	 * the process is not visible from that namespace.  Skipping the 0
	 * cases means a process inside a guest container only ever sees the
	 * GPU processes within its own ns (Docker-on-guest isolation), and
	 * the init-ns querier sees the global guest pids — exactly mirroring
	 * how the real nvidia driver scopes GET_PIDS by current's pid ns.
	 */
	mutex_lock(&nvkvm.sessions_lock);
	idr_for_each_entry(&nvkvm.sessions_idr, s, id) {
		pid_t vnr;

		if (n >= NVKVM_GET_PIDS_MAX || !s->tgid_pid)
			continue;
		vnr = pid_vnr(s->tgid_pid);   /* in current's pid ns */
		if (vnr)
			gp->pid_tbl[n++] = (__u32)vnr;
	}
	mutex_unlock(&nvkvm.sessions_lock);
	gp->pid_tbl_count = n;

	outsz = offsetof(struct nvkvm_get_pids_params, pid_tbl) + (size_t)n * 4;
	if (outsz > ctl->params_size)
		outsz = ctl->params_size;
	if (copy_to_user(up, gp, outsz)) {
		kfree(gp);
		return 0;
	}
	kfree(gp);
	ctl->status = 0;   /* NV_OK — caller writes params_buf back to user */
	pr_info_ratelimited("nvkvm: synthesized GET_PIDS — %u guest pid(s)\n", n);
	return 1;
}

/*
 * NV2080_CTRL_CMD_GPU_GET_PID_INFO (0x2080018e) — per-pid GPU memory, used by
 * nvidia-smi after GET_PIDS.  The inner params (in aux_buf) hold pidInfoList[]:
 * count @0, then 56-byte entries @8 (pid @+0).  The host RM can't resolve guest
 * pids, so we translate each pid to a tag encoding its owning isolate
 * (0x80000000 | isolate_id) which QEMU swaps for the real host pid (validated
 * against this VM's isolates) before the kernel sees it; the kernel then fills
 * real memory for that host pid.  pid-ns correctness: we match via pid_vnr() in
 * the CALLER's ns, so only sessions visible in that ns resolve — a foreign or
 * out-of-ns pid is zeroed (kernel returns NOT_FOUND, no cross-ns/tenant leak).
 * Originals are saved and restored into aux_buf on the response so nvidia-smi
 * sees the pid it asked about alongside the real memory.
 */
#define NVKVM_NV2080_GET_PID_INFO 0x2080018eu
#define NVKVM_PIDINFO_LIST_OFF    8u
/* sizeof(NV2080_CTRL_GPU_PID_INFO) = 72: pid@0,index@4,result@8,data@16
 * (6×NvU64 vidMemUsage union), smcSubscription@64.  Verified via sizeof on the
 * 575 open-driver SDK headers. */
#define NVKVM_PIDINFO_STRIDE      72u
#define NVKVM_PIDINFO_MAX         200u

static void nvkvm_get_pid_info_tag(void *aux, __u32 aux_size,
				   __u32 **save_out, __u32 *n_out)
{
	__u32 count = 0, i, n = 0;
	__u32 *save;

	*save_out = NULL;
	*n_out = 0;
	if (aux_size < NVKVM_PIDINFO_LIST_OFF + 4)
		return;
	memcpy(&count, aux, 4);
	if (count > NVKVM_PIDINFO_MAX)
		count = NVKVM_PIDINFO_MAX;
	save = kzalloc((size_t)NVKVM_PIDINFO_MAX * sizeof(__u32), GFP_KERNEL);
	if (!save)
		return;

	for (i = 0; i < count; i++) {
		__u32 off = NVKVM_PIDINFO_LIST_OFF + i * NVKVM_PIDINFO_STRIDE;
		__u32 pid, iso = 0, repl;
		struct nvkvm_session *s;
		int id;

		if ((size_t)off + 4 > aux_size)
			break;
		memcpy(&pid, (char *)aux + off, 4);
		save[i] = pid;
		/* Reverse of GET_PIDS: which session's tgid, in the caller's pid
		 * ns, equals this queried pid?  pid_vnr() is ns-relative. */
		mutex_lock(&nvkvm.sessions_lock);
		idr_for_each_entry(&nvkvm.sessions_idr, s, id) {
			if (s->tgid_pid && s->isolate_id &&
			    (__u32)pid_vnr(s->tgid_pid) == pid) {
				iso = s->isolate_id;
				break;
			}
		}
		mutex_unlock(&nvkvm.sessions_lock);
		/* tag → QEMU resolves to host pid; 0 → kernel NOT_FOUND (filtered). */
		repl = iso ? (0x80000000u | (iso & 0x7fffffffu)) : 0u;
		memcpy((char *)aux + off, &repl, 4);
		n = i + 1;
	}
	*save_out = save;
	*n_out = n;
}

static void nvkvm_get_pid_info_restore(void *aux, __u32 aux_size,
				       __u32 *save, __u32 n)
{
	__u32 i;
	for (i = 0; i < n; i++) {
		__u32 off = NVKVM_PIDINFO_LIST_OFF + i * NVKVM_PIDINFO_STRIDE;
		if ((size_t)off + 4 > aux_size)
			break;
		memcpy((char *)aux + off, &save[i], 4);
	}
}

/*
 * nvkvm_ioctl — validate and forward an ioctl to the host.
 *
 * Security: we validate param_size against the known ABI size before copying
 * from userspace. Pointer fields in the parameter blob are zeroed or replaced
 * with offsets into the aux slot; the host never receives raw guest VA values.
 */
/*
 * RM_CONTROL commands whose inner params begin with an embedded list preamble
 * { u32 count@0; pad@4; NvP64 ptr@8 } that the driver writes through. Returns
 * the size in bytes of ONE list element, or 0 if the command has no such list.
 *
 *   GET_INFO family  → 8 bytes/entry (NvxxxCtrlXxxInfo = {u32 index; u32 data})
 *   GET_CAPS family  → 1 byte/entry  (the count field is a byte length)
 *
 * The two families are structurally identical (u32@0, ptr@8); only the unit of
 * the count differs, so they share one handler parameterised by this size.
 */
static unsigned int nvkvm_ctrl_list_entry_size(__u32 cmd)
{
	switch (cmd) {
	case NV0041_CTRL_CMD_GET_SURFACE_INFO:
	case NV0080_CTRL_CMD_GR_GET_INFO:
	case NV2080_CTRL_CMD_BIOS_GET_INFO:
	case NV2080_CTRL_CMD_GR_GET_INFO:
	case NV2080_CTRL_CMD_FB_GET_INFO:
	case NV2080_CTRL_CMD_BUS_GET_INFO:
		return NVXXX_CTRL_XXX_INFO_ENTRY_SIZE; /* 8 */
	case NV2080_CTRL_CMD_GPU_GET_ENGINES:
		return 4; /* engineList is NvU32[engineCount] */
	case NV0080_CTRL_CMD_GPU_GET_CLASSLIST:
		return 4; /* classList is NvU32[numClasses] (NVENC engine discovery) */
	case NV0080_CTRL_CMD_GR_GET_CAPS:
	case NV0080_CTRL_CMD_FB_GET_CAPS:
	case NV0080_CTRL_CMD_HOST_GET_CAPS:
	case NV0080_CTRL_CMD_FIFO_GET_CAPS:
	case NV0080_CTRL_CMD_MSENC_GET_CAPS:
	case NV0080_CTRL_CMD_BSP_GET_CAPS_V2:
		return 1; /* capsTblSize is a byte count */
	}
	return 0;
}

/*
 * RM_ALLOC alloc-params for a class with no entry in the size-by-hClass tables
 * below.
 *
 * The tables are exhaustive only for the classes somebody has already been
 * bitten by.  When a class is missing, `ap_size` stays 0, the guest copies
 * nothing, and the stub writes a NULL `pAllocParms` into the forwarded NVOS21 /
 * NVOS64 — so the host RM allocates the object from an ALL-DEFAULT parameter
 * block while the caller believes it asked for something specific.  Nothing is
 * denied and no status is non-zero; the object is simply not the one the caller
 * requested, and the damage surfaces several layers later.
 *
 * That has now cost this project five separate bugs (#84 twice, #99, #101, and
 * the Hopper Vulkan failure: HOPPER_USERMODE_A / 0xc661 was unsized, so its
 * `bBar1Mapping` never reached RM, `GET_ADDR_SPACE_TYPE` answered REGMEM where
 * bare metal answers VIDMEM, and the userspace driver freed the channel it had
 * just built and then allocated HOPPER_COMPUTE_A under the dead handle).
 *
 * So stop failing silently.  For an unsized class, forward a bounded window of
 * the caller's buffer instead of nothing, and leave the caller's
 * `alloc_parms_size` alone — a native caller routinely passes 0 there and lets
 * RM size the struct by class, which is exactly the behaviour we want to
 * reproduce.  The window stops at the end of the page holding the pointer, so
 * it can never fault into an unmapped neighbour: the first byte is mapped
 * because userspace just handed us the pointer.
 */
#define NVKVM_ALLOC_PARMS_PROBE 256u

static size_t nvkvm_alloc_parms_probe_len(__u64 uptr)
{
	size_t to_page_end = (size_t)PAGE_SIZE - (size_t)(uptr & (PAGE_SIZE - 1));

	return min_t(size_t, (size_t)NVKVM_ALLOC_PARMS_PROBE, to_page_end);
}

static long nvkvm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct nvkvm_fd_ctx *ctx = filp->private_data;
	void *params_buf = NULL;
	void __user *uparams = (void __user *)arg;
	size_t param_size;
	void *aux_buf = NULL;
	size_t aux_size = 0;
	void __user *aux_uptr = NULL;
	__u32 *gpi_save = NULL;          /* GET_PID_INFO original pids to restore */
	__u32 gpi_n = 0;
	long ret;

	if (!ctx)
		return -EBADF;

	/* Validate and get the expected parameter size for this ioctl */
	param_size = nvkvm_ioctl_param_size(cmd);
	if (param_size == (size_t)-1) {
		pr_warn("nvkvm: AUDIT unknown ioctl cmd=0x%x type=0x%x nr=0x%x iocsz=%u\n",
			cmd, _IOC_TYPE(cmd), _IOC_NR(cmd), _IOC_SIZE(cmd));
		return -ENOTTY;
	}

	/* PARANOID forwarding-fidelity audit (#84): if our param_size differs
	 * from the size the caller encoded in the cmd (_IOC_SIZE), we will
	 * forward a TRUNCATED/over-long, malformed buffer to the host kernel —
	 * the low bytes match but the call is wrong (cf. the NVOS32 88-vs-184
	 * bug).  Log every mismatch so EGL/graphics-path ioctls get caught.
	 *
	 * Skip UVM cmds (_IOC_TYPE == 0): UVM's command numbers (0x3000xxxx)
	 * encode a bogus _IOC_SIZE (e.g. UVM_INITIALIZE 0x30000001 → 0x3000)
	 * that the UVM driver ignores — it reads its own per-cmd struct.  These
	 * are false positives that would otherwise mask a real 'F'/'d'/'m'
	 * mismatch in the log. */
	if (_IOC_TYPE(cmd) != 0 &&
	    _IOC_SIZE(cmd) && (size_t)_IOC_SIZE(cmd) != param_size)
		pr_warn("nvkvm: AUDIT param_size MISMATCH cmd=0x%x type=0x%x nr=0x%x iocsz=%u our=%zu\n",
			cmd, _IOC_TYPE(cmd), _IOC_NR(cmd), _IOC_SIZE(cmd), param_size);

	if (param_size > NVKVM_SHM_SLOT_DEFAULT_SIZE)
		return -EINVAL;

	/* Copy parameters from userspace into a kernel buffer */
	if (param_size > 0) {
		params_buf = kzalloc(param_size, GFP_KERNEL);
		if (!params_buf)
			return -ENOMEM;

		/*
		 * Some UVM ioctls (notably UVM_DEINITIALIZE) are called with a
		 * NULL arg by libnvml.  The native NVIDIA UVM driver accepts NULL
		 * and treats it as "use empty/default params".  Mirror that
		 * behaviour: skip the copy and forward the zero-initialised buffer.
		 */
		if (uparams != NULL &&
		    copy_from_user(params_buf, uparams, param_size)) {
			kfree(params_buf);
			return -EFAULT;
		}
	}

	/*
	 * nvidia-smi process enumeration (GET_PIDS): synthesize from our session
	 * table instead of forwarding — the host RM can't see the guest's pids
	 * (pid namespace + per-isolate RM client).  Simulated entirely guest-side.
	 */
	if (_IOC_NR(cmd) == NV_ESC_RM_CONTROL && params_buf &&
	    nvkvm_synth_get_pids(params_buf, param_size)) {
		long sr = 0;
		if (uparams && copy_to_user(uparams, params_buf, param_size))
			sr = -EFAULT;
		kfree(params_buf);
		return sr;
	}

	/*
	 * State-machine Steps B+C: record UVM config + state-registration
	 * cmds into ctx->uvm_state.  ALSO still forward each so the
	 * kernel side stays functional — recording is additive between
	 * Step B and Step E.  The full short-circuit lands at Step E
	 * along with REALIZE_UVM_MAPPING.  Per docs/STATE_MACHINE_PLAN.md.
	 *
	 * Per-cmd recording — these are tiny structs the guest module
	 * trivially digests.  All allocations are GFP_KERNEL; mutex
	 * held only across the recording, not across the forward.
	 */
	if (ctx->dev_id == NVKVM_DEV_UVM && ctx->uvm_state && params_buf) {
		struct nvkvm_uvm_fd_state *st = ctx->uvm_state;
		switch (cmd) {
		case UVM_INITIALIZE:
			if (param_size >= sizeof(struct uvm_initialize_params)) {
				struct uvm_initialize_params *p = params_buf;
				mutex_lock(&st->lock);
				st->init_flags  = p->flags;
				st->initialized = true;
				mutex_unlock(&st->lock);
			}
			break;
		case UVM_REGISTER_GPU:
			if (param_size >= sizeof(struct uvm_register_gpu_params)) {
				struct uvm_register_gpu_params *p = params_buf;
				struct nvkvm_uvm_gpu_reg *g =
					kzalloc(sizeof(*g), GFP_KERNEL);
				if (g) {
					memcpy(g->gpu_uuid, p->gpu_uuid.uuid, 16);
					mutex_lock(&st->lock);
					list_add_tail(&g->list, &st->registered_gpus);
					mutex_unlock(&st->lock);
				}
			}
			break;
		case UVM_REGISTER_GPU_VASPACE:
			if (param_size >= sizeof(struct uvm_register_gpu_vaspace_params)) {
				struct uvm_register_gpu_vaspace_params *p = params_buf;
				struct nvkvm_uvm_vas_reg *v =
					kzalloc(sizeof(*v), GFP_KERNEL);
				if (v) {
					/* p->rm_ctrl_fd is the guest userspace fd.
					 * Translate to QEMU-side handle_id so the
					 * stub can resolve it via handle_lookup at
					 * realize time. */
					__s32 hid = ((int32_t)p->rm_ctrl_fd >= 0)
						? guest_fd_to_handle_id((int)p->rm_ctrl_fd)
						: -1;
					memcpy(v->gpu_uuid, p->gpu_uuid.uuid, 16);
					v->rm_ctrl_fd_handle_id =
						(hid >= 0) ? (__u32)hid : 0;
					v->h_client    = p->h_client;
					v->h_va_space  = p->h_va_space;
					mutex_lock(&st->lock);
					list_add_tail(&v->list,
						      &st->registered_va_spaces);
					mutex_unlock(&st->lock);
				}
			}
			break;
		case UVM_CREATE_RANGE_GROUP:
			if (param_size >= sizeof(struct uvm_create_range_group_params)) {
				struct uvm_create_range_group_params *p = params_buf;
				struct nvkvm_uvm_range_group *r =
					kzalloc(sizeof(*r), GFP_KERNEL);
				if (r) {
					r->range_group_id = p->range_group_id;
					mutex_lock(&st->lock);
					list_add_tail(&r->list, &st->range_groups);
					mutex_unlock(&st->lock);
				}
			}
			break;
		/* UVM_REGISTER_CHANNEL also fits here; skip until needed
		 * (libcuda calls it but the kernel-side channel is already
		 * known via RM handles — recording is for parity, not
		 * correctness). */
		case UVM_ALLOC_SEMAPHORE_POOL:
			if (param_size >=
			    sizeof(struct uvm_alloc_semaphore_pool_params)) {
				struct uvm_alloc_semaphore_pool_params *p = params_buf;
				struct nvkvm_uvm_mapping_intent *m =
					kzalloc(sizeof(*m), GFP_KERNEL);
				if (m) {
					m->mode  = NVKVM_UVM_REALIZE_MODE_SEM_POOL;
					m->base  = p->base;
					m->length = p->length;
					m->params_size = sizeof(*p);
					m->params = kmemdup(p, sizeof(*p), GFP_KERNEL);
					if (!m->params) {
						kfree(m);
					} else {
						mutex_lock(&st->lock);
						list_add_tail(&m->list, &st->intents);
						mutex_unlock(&st->lock);
					}
				}
				/* Step E plan was to short-circuit here and
				 * replay on a fresh UVM fd at mmap time.  That
				 * model loses the kernel-side RM↔UVM bindings
				 * built on the original fd (NV_ERR_PAGE_TABLE_-
				 * NOT_AVAIL).  Until realize-on-existing-fd is
				 * implemented, keep recording but still forward
				 * SEM_POOL so the original fd is the live one. */
			}
			break;
		default:
			break;
		}
		/* Fall through to normal forward path. */
	}

	/*
	 * Save caller-supplied user-space pointer / size fields BEFORE any
	 * sanitizer (inline or via nvkvm_sanitize_ioctl_params) runs, so we
	 * can restore them on the response.  CUDA verifies these round-trip
	 * unchanged.
	 *
	 * Bug we already hit: capturing AFTER the inline sanitizer captured
	 * `alloc_parms_size = 0x38` (the class-derived size we filled in for
	 * the host driver) instead of CUDA's original 0 — and the restore
	 * then "restored" that bogus value back over the kernel's writeback.
	 */
	u64 orig_nvos54_params = 0;       /* RM_CONTROL nvos54.params  */
	u64 orig_nvos64_alloc  = 0;       /* RM_ALLOC nvos64.p_alloc_parms */
	u64 orig_nvos64_rights = 0;       /* RM_ALLOC nvos64.p_rights_requested */
	u32 orig_nvos64_size   = 0;       /* RM_ALLOC nvos64.alloc_parms_size */
	u64 orig_nvos21_alloc  = 0;       /* RM_ALLOC nvos21.p_alloc_parms */
	bool have_nvos64_orig  = false;
	__s32 orig_uvm_mm_init_fd = 0;
	bool have_uvm_mm_init     = false;
	__u32 orig_uvm_rm_ctrl_fd = 0;
	bool have_uvm_rm_ctrl     = false;
	/*
	 * Byte offset of the rm_ctrl_fd the sanitizer swapped for a handle_id.
	 * 16 for uvm_register_gpu_vaspace_params / uvm_register_channel_params
	 * (both open with a 16-byte uvm_uuid); UVM_MAP_EXTERNAL_ALLOCATION's is
	 * version-variant and comes from the ABI profile — see the capture site.
	 */
	unsigned uvm_rm_ctrl_off  = 16;
	/* EXPORT_OBJECT_TO_FD (ctrl 0x3d05): frontend fd embedded in the INNER
	 * control params (aux) at offset 16 — translate guest-fd→handle_id, save
	 * the caller's fd to restore on the response (fd is IN/OUT, value kept). */
	__s32 orig_export_fd = 0;
	bool have_export_fd  = false;
	__s32 orig_import_fd = 0;          /* IMPORT_OBJECT_FROM_FD (0x3d06) fd@0 */
	bool have_import_fd  = false;
	/* Embedded-fd fields in frontend ioctls: sanitizer overwrites these
	 * with handle_ids; capture the caller's original guest-fd so the
	 * response round-trips libcuda's value unchanged. */
	__s32 orig_fe_ctl_fd = 0;     bool have_fe_ctl_fd = false;     /* REGISTER_FD       */
	__u32 orig_fe_os_evt_fd = 0;  bool have_fe_os_evt_fd = false;  /* ALLOC/FREE_OS_EVT */
	__s32 orig_fe_nvos02_fd = 0;  bool have_fe_nvos02_fd = false;  /* RM_ALLOC_MEMORY   */
	__s32 orig_fe_nvos33_fd = 0;  bool have_fe_nvos33_fd = false;  /* RM_MAP_MEMORY     */
	u64 orig_modeset_addr = 0;    bool have_modeset = false;       /* NVKMS address ptr */
	/* NVKMS REGISTER_SURFACE embedded plane fds (IN): swapped for handle_ids
	 * before forwarding, restored to the caller's fds on the response. */
	__s32 orig_regsurf_fd[NVKVM_NVKMS_MAX_PLANES] = {0};
	unsigned orig_regsurf_off[NVKVM_NVKMS_MAX_PLANES] = {0};
	int      regsurf_nfd = 0;
	if (ctx->dev_id == NVKVM_DEV_MODESET && params_buf &&
	    param_size >= NVKVM_NVKMS_PARAMS_SIZE) {
		orig_modeset_addr =
			*(u64 *)((char *)params_buf + NVKVM_NVKMS_ADDR_OFF);
		have_modeset = true;
	} else if (_IOC_NR(cmd) == NV_ESC_RM_CONTROL && params_buf &&
	    param_size == sizeof(struct nvos54_parameters)) {
		orig_nvos54_params =
			((struct nvos54_parameters *)params_buf)->params;
	} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC && params_buf &&
		   param_size == sizeof(struct nvos64_parameters)) {
		struct nvos64_parameters *a = params_buf;
		orig_nvos64_alloc  = a->p_alloc_parms;
		orig_nvos64_rights = a->p_rights_requested;
		orig_nvos64_size   = a->alloc_parms_size;
		have_nvos64_orig   = true;
	} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC && params_buf &&
		   param_size == sizeof(struct nvos21_parameters)) {
		orig_nvos21_alloc =
			((struct nvos21_parameters *)params_buf)->p_alloc_parms;
	} else if (cmd == UVM_MM_INITIALIZE && params_buf &&
		   param_size == sizeof(struct uvm_mm_initialize_params)) {
		orig_uvm_mm_init_fd =
			((struct uvm_mm_initialize_params *)params_buf)->uvm_fd;
		have_uvm_mm_init = true;
	} else if (params_buf &&
		   (cmd == UVM_REGISTER_GPU_VASPACE ||
		    cmd == UVM_REGISTER_CHANNEL ||
		    cmd == UVM_MAP_EXTERNAL_ALLOCATION)) {
		/*
		 * #81: UVM_MAP_EXTERNAL_ALLOCATION's rm_ctrl_fd is NOT at 16.
		 * nvkvm_sanitize_ioctl_params() writes the handle_id at
		 * nvkvm_prof()->uvm_map_ext_fd_off (1184 in the pre-V550
		 * 1-entry layout, 9248 in the V550 256-entry one); capturing
		 * and restoring offset 16 for it therefore restored a field
		 * nobody touched and left the sanitizer's handle_id standing at
		 * the real one, handing an internal VM-global id back to
		 * userspace.  Index by the same profile offset the sanitizer
		 * uses so capture and restore cannot disagree.
		 */
		if (cmd == UVM_MAP_EXTERNAL_ALLOCATION)
			uvm_rm_ctrl_off = nvkvm_prof()->uvm_map_ext_fd_off;
		if (param_size >= (size_t)uvm_rm_ctrl_off + sizeof(__u32)) {
			orig_uvm_rm_ctrl_fd =
				*(__u32 *)((char *)params_buf + uvm_rm_ctrl_off);
			have_uvm_rm_ctrl = true;
		}
	} else if (_IOC_NR(cmd) == NV_ESC_REGISTER_FD && params_buf &&
		   param_size >= sizeof(struct nv_ioctl_register_fd)) {
		orig_fe_ctl_fd =
			((struct nv_ioctl_register_fd *)params_buf)->ctl_fd;
		have_fe_ctl_fd = true;
	} else if ((_IOC_NR(cmd) == NV_ESC_ALLOC_OS_EVENT ||
		    _IOC_NR(cmd) == NV_ESC_FREE_OS_EVENT) && params_buf &&
		   param_size >= sizeof(struct nv_ioctl_alloc_os_event)) {
		orig_fe_os_evt_fd =
			((struct nv_ioctl_alloc_os_event *)params_buf)->fd;
		have_fe_os_evt_fd = true;
	} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC_MEMORY && params_buf &&
		   param_size >= sizeof(struct nv_ioctl_nvos02_parameters_with_fd)) {
		orig_fe_nvos02_fd =
			((struct nv_ioctl_nvos02_parameters_with_fd *)params_buf)->fd;
		have_fe_nvos02_fd = true;
	} else if (_IOC_NR(cmd) == NV_ESC_RM_MAP_MEMORY && params_buf &&
		   param_size >= sizeof(struct nv_ioctl_nvos33_parameters_with_fd)) {
		orig_fe_nvos33_fd =
			((struct nv_ioctl_nvos33_parameters_with_fd *)params_buf)->fd;
		have_fe_nvos33_fd = true;
	}

	/* NVOS56 UPDATE_DEVICE_MAPPING_INFO: the kernel uses pOldCpuAddress
	 * as a lookup key on the host driver's CpuMapping table for the
	 * calling process.  Because our stub mmaps the BAR at its own VAs
	 * (which the kernel registered), and libcuda passes its guest-side
	 * VAs (which the stub never had), the lookup ALWAYS fails on this
	 * path and the kernel returns NV_ERR_OBJECT_NOT_FOUND (0x57).  The
	 * mapping still works because we install the BAR pages into the
	 * guest's GPA window via QEMU, so this ioctl is effectively
	 * informational.  We save the caller's values so we can fake
	 * success on the response path. */
	__u64 orig_nvos56_old = 0, orig_nvos56_new = 0;
	bool fake_nvos56_ok = false;
	if (_IOC_NR(cmd) == NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO && params_buf &&
	    param_size == sizeof(struct nvos56_parameters)) {
		struct nvos56_parameters *p = params_buf;
		orig_nvos56_old = p->p_old_cpu_address;
		orig_nvos56_new = p->p_new_cpu_address;
		fake_nvos56_ok  = true;
	}

	/* NV_ESC_RM_UNMAP_MEMORY (nvos34): same VA-lookup pattern as NVOS56.
	 * Kernel looks up CpuMapping by pLinearAddress; the stub's mmap'd VA
	 * was registered there but libcuda passes its own guest-side VA, so
	 * the lookup always returns NV_ERR_OBJECT_NOT_FOUND.  The unmap
	 * itself is handled by our QEMU/KVM region-uninstall path (the GPA
	 * goes away when the guest munmaps); the kernel record is informational
	 * in our forwarded model.  Fake success on response. */
	__u64 orig_nvos34_va = 0;
	bool fake_nvos34_ok = false;
	if (_IOC_NR(cmd) == NV_ESC_RM_UNMAP_MEMORY && params_buf &&
	    param_size == sizeof(struct nv_ioctl_nvos34_parameters)) {
		struct nv_ioctl_nvos34_parameters *p = params_buf;
		orig_nvos34_va = p->p_linear_address;
		fake_nvos34_ok = true;
	}

	/*
	 * For ioctls with embedded secondary buffers: extract the secondary data
	 * BEFORE the sanitizer zeroes the pointer fields.  We carry the data in
	 * the aux slot so the host can reconstruct it without ever seeing a raw
	 * guest VA.
	 *
	 * NV_ESC_RM_CONTROL: secondary buffer = cmd-specific params (in+out).
	 * NV_ESC_RM_ALLOC (NVOS21): secondary buffer = class-specific alloc params
	 *   (input only; size determined by h_class).
	 * NV_ESC_RM_ALLOC (NVOS64): secondary buffer = class-specific alloc params
	 *   (input only; alloc_parms_size tells us the size).
	 */
	/*
	 * NVKMS (/dev/nvidia-modeset): the wrapper's single embedded user ptr
	 * `address` (offset 8) points at `size` (offset 4) bytes of inner params
	 * the host kernel reads AND writes.  Stage them in the aux slot exactly
	 * like RM_CONTROL: copy in, zero the ptr (the stub substitutes a host VA
	 * at offset 8), copy back after the ioctl via the generic aux writeback.
	 */
	if (ctx->dev_id == NVKVM_DEV_MODESET && params_buf &&
	    param_size >= NVKVM_NVKMS_PARAMS_SIZE) {
		__u32 inner_sz  = *(__u32 *)((char *)params_buf + 4);
		__u64 inner_ptr = *(__u64 *)((char *)params_buf +
					     NVKVM_NVKMS_ADDR_OFF);
		if (inner_sz > 0 && inner_ptr != 0) {
			if (inner_sz > NVKVM_SHM_SLOT_DEFAULT_SIZE) {
				kfree(params_buf);
				return -EINVAL;
			}
			aux_uptr = (void __user *)(uintptr_t)inner_ptr;
			aux_buf = kzalloc(inner_sz, GFP_KERNEL);
			if (!aux_buf) {
				kfree(params_buf);
				return -ENOMEM;
			}
			if (copy_from_user(aux_buf, aux_uptr, inner_sz)) {
				kfree(aux_buf);
				kfree(params_buf);
				return -EFAULT;
			}
			aux_size = inner_sz;
			/* Zero the ptr so we never forward a guest VA. */
			*(__u64 *)((char *)params_buf + NVKVM_NVKMS_ADDR_OFF) = 0;

			/*
			 * REGISTER_SURFACE registers the (semaphore-)surface by
			 * fd (useFd=TRUE); the inner params carry up to 3 plane
			 * fds the host NVKMS dups.  The host can't see guest fds,
			 * so swap each for our handle_id (the stub resolves its
			 * own local fd), exactly like EXPORT_OBJECT_TO_FD.  Save
			 * the caller's fds to restore on the response.
			 */
			if (*(__u32 *)params_buf == NVKVM_NVKMS_CMD_REGISTER_SURFACE &&
			    inner_sz >= NVKVM_NVKMS_REGSURF_PLANE0_OFF +
					NVKVM_NVKMS_MAX_PLANES *
					NVKVM_NVKMS_REGSURF_PLANE_STRIDE &&
			    *(__u8 *)((char *)aux_buf +
				      NVKVM_NVKMS_REGSURF_USEFD_OFF)) {
				unsigned i;
				for (i = 0; i < NVKVM_NVKMS_MAX_PLANES; i++) {
					unsigned off = NVKVM_NVKMS_REGSURF_PLANE0_OFF +
						       i * NVKVM_NVKMS_REGSURF_PLANE_STRIDE;
					__s32 gfd;
					memcpy(&gfd, (char *)aux_buf + off, sizeof(gfd));
					if (gfd <= 0)
						continue;
					orig_regsurf_fd[regsurf_nfd]  = gfd;
					orig_regsurf_off[regsurf_nfd] = off;
					regsurf_nfd++;
					{
						__s32 hid = guest_fd_to_handle_id(gfd);
						if (hid < 0)   /* fail closed */
							return -EBADF;
						memcpy((char *)aux_buf + off,
						       &hid, sizeof(hid));
					}
				}
			}
		}
	} else if (_IOC_NR(cmd) == NV_ESC_RM_CONTROL && params_buf) {
		struct nvos54_parameters *ctrl = params_buf;
		if (ctrl->params_size > 0 && ctrl->params != 0) {
			if (ctrl->params_size > NVKVM_SHM_SLOT_DEFAULT_SIZE) {
				kfree(params_buf);
				return -EINVAL;
			}
			aux_uptr = (void __user *)(uintptr_t)ctrl->params;
			aux_buf = kzalloc(ctrl->params_size, GFP_KERNEL);
			if (!aux_buf) {
				kfree(params_buf);
				return -ENOMEM;
			}
			if (copy_from_user(aux_buf, aux_uptr, ctrl->params_size)) {
				kfree(aux_buf);
				kfree(params_buf);
				return -EFAULT;
			}
			aux_size = ctrl->params_size;

			/* GET_PID_INFO: tag each guest pid with its owning isolate
			 * (ns-filtered) so QEMU can resolve the real host pid; save
			 * originals to restore on the response. */
			if (ctrl->cmd == NVKVM_NV2080_GET_PID_INFO)
				nvkvm_get_pid_info_tag(aux_buf, aux_size,
						       &gpi_save, &gpi_n);

			/* NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD (0x3d05): the
			 * inner params carry a frontend fd at offset 16 (the "empty
			 * fd" libGLX exports the object onto).  Like REGISTER_FD, the
			 * stub needs our handle_id there so it can resolve its own
			 * local fd; save the caller's guest fd to restore on response
			 * (fd is IN/OUT, value unchanged by the export). */
			if (ctrl->cmd == 0x3d05 && aux_size >= 20) {
				__s32 gfd;
				memcpy(&gfd, (char *)aux_buf + 16, sizeof(gfd));
				orig_export_fd = gfd;
				have_export_fd = true;
				if (gfd >= 0) {
					__s32 hid = guest_fd_to_handle_id(gfd);
					if (hid < 0)   /* fail closed -- see 0x3d06 below */
						return -EBADF;
					memcpy((char *)aux_buf + 16, &hid,
					       sizeof(hid));
				}
			}

			/* NV0000_CTRL_CMD_OS_UNIX_IMPORT_OBJECT_FROM_FD (0x3d06,
			 * #110) and _GET_EXPORT_OBJECT_INFO (0x3d08).  Both are
			 * the read side of 0x3d05 and both carry the source
			 * nv-export fd at inner offset 0 (NvS32 fd; 0x3d06 then
			 * has the NV0000_CTRL_OS_UNIX_EXPORT_OBJECT, 0x3d08 the
			 * deviceInstance/maxObjects it fills in).  Swap our
			 * handle_id in so the stub resolves its own local fd; the
			 * fd is IN (unchanged), so save it to restore on the
			 * response.
			 *
			 * 0x3d08 was missing here, and it is the FIRST of the two
			 * libcuda issues: cuMemImportFromShareableHandle calls it
			 * to learn which device an exported fd's objects are
			 * parented by, before it calls 0x3d06 at all.  MEASURED on
			 * the bare-metal host with tools/nv_ioctl_trace.c: the
			 * order is 0x3d05 (exporter) -> 0x3d08 -> 0x3d06
			 * (importer).  Without the translation the guest's own fd
			 * NUMBER was forwarded to a stub in which it means
			 * something else entirely. */
			if ((ctrl->cmd == 0x3d06 || ctrl->cmd == 0x3d08) &&
			    aux_size >= 4) {
				__s32 gfd;
				memcpy(&gfd, aux_buf, sizeof(gfd));
				orig_import_fd = gfd;
				have_import_fd = true;
				if (gfd >= 0) {
					__s32 hid = guest_fd_to_handle_id(gfd);
					/* FAIL CLOSED.  guest_fd_to_handle_id()
					 * returns -EBADF for any integer that is
					 * not an open nvkvm fd, so letting the
					 * untranslated value through would forward
					 * a guest-chosen integer as a VM-global
					 * handle_id -- and QEMU's cross-isolate
					 * relay treats it as an entitlement.
					 * Handle ids are sequential from 1, so it
					 * would be guessable.  The legitimate path
					 * never reaches this branch: a real
					 * SCM_RIGHTS-received nvkvm fd always
					 * translates. */
					if (hid < 0)
						return -EBADF;
					memcpy(aux_buf, &hid, sizeof(hid));
				}
			}

			/*
			 * Commands that embed an `NvxxxCtrlXxxGetInfoParams` preamble
			 * (info_list_size, pad, info_list pointer) at the start of the
			 * inner params struct. The driver writes through the
			 * info_list pointer, so we extend aux_buf to hold the list,
			 * copy the user's list into the extension, and zero the
			 * pointer field — QEMU substitutes a host VA pointing at the
			 * extension. After the ioctl we copy the list back to the
			 * original user-space address (saved off below). See gVisor
			 * `ctrlIoctlHasInfoList` for the reference implementation.
			 */
			/*
			 * NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST has two embedded
			 * NvP64 list pointers (PChannelHandleList, PChannelList),
			 * each pointing at a u32 array of NumChannels entries.
			 * Both lists are IN+OUT for the driver: it reads them
			 * to filter then writes back the results.  Extend
			 * aux_buf to hold both lists inline, copy the user
			 * buffers into the extension, and zero the pointer
			 * fields — stub substitutes host VAs pointing at the
			 * extension area.  After the ioctl we copy the lists
			 * back to the original user buffers.  See gVisor
			 * ctrlDevFIFOGetChannelList.
			 */
			if (ctrl->cmd == NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST &&
			    ctrl->params_size >= 24) {
				__u32 num_channels = *(__u32 *)aux_buf;
				__u64 p_handles    = *(__u64 *)((char *)aux_buf + 8);
				__u64 p_list       = *(__u64 *)((char *)aux_buf + 16);
				if (num_channels > 0 && num_channels <= 4096 &&
				    p_handles && p_list) {
					size_t list_bytes = (size_t)num_channels *
							    sizeof(__u32);
					size_t ext = ctrl->params_size +
						     2 * list_bytes;
					void *ext_buf = kzalloc(ext, GFP_KERNEL);
					if (!ext_buf) {
						kfree(aux_buf);
						kfree(params_buf);
						return -ENOMEM;
					}
					memcpy(ext_buf, aux_buf, ctrl->params_size);
					if (copy_from_user((char *)ext_buf +
							   ctrl->params_size,
							   (void __user *)(uintptr_t)p_handles,
							   list_bytes) ||
					    copy_from_user((char *)ext_buf +
							   ctrl->params_size +
							   list_bytes,
							   (void __user *)(uintptr_t)p_list,
							   list_bytes)) {
						kfree(ext_buf);
						kfree(aux_buf);
						kfree(params_buf);
						return -EFAULT;
					}
					/* Zero ptrs; stub fills them */
					*(__u64 *)((char *)ext_buf + 8)  = 0;
					*(__u64 *)((char *)ext_buf + 16) = 0;
					kfree(aux_buf);
					aux_buf  = ext_buf;
					aux_size = ext;
				}
			}

			{
				unsigned int entry_size =
					nvkvm_ctrl_list_entry_size(ctrl->cmd);
				if (entry_size &&
				    ctrl->params_size >= 16 /* size(4)+pad(4)+ptr(8) */) {
					__u32 list_size = *(__u32 *)aux_buf;
					__u64 list_ptr  = *(__u64 *)((char *)aux_buf + 8);
					if (list_size > 0 && list_size <= 65536 && list_ptr != 0) {
						size_t list_bytes =
							(size_t)list_size *
							entry_size;
						size_t ext = ctrl->params_size + list_bytes;
						void *ext_buf = kzalloc(ext, GFP_KERNEL);
						if (!ext_buf) {
							kfree(aux_buf);
							kfree(params_buf);
							return -ENOMEM;
						}
						memcpy(ext_buf, aux_buf, ctrl->params_size);
						if (copy_from_user((char *)ext_buf +
								   ctrl->params_size,
								   (void __user *)(uintptr_t)list_ptr,
								   list_bytes)) {
							kfree(ext_buf);
							kfree(aux_buf);
							kfree(params_buf);
							return -EFAULT;
						}
						/* Zero info_list ptr; QEMU/stub fills with host VA */
						*(__u64 *)((char *)ext_buf + 8) = 0;
						kfree(aux_buf);
						aux_buf  = ext_buf;
						aux_size = ext;
					}
				}
			}

			/*
			 * NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION has embedded
			 * pointer fields (pDriverVersionBuffer etc.) pointing to
			 * guest user-space string buffers.  The host NVIDIA driver
			 * can't write to guest VAs.  Extend aux_buf to hold the
			 * strings inline, zero the embedded pointers (QEMU replaces
			 * them with host VAs pointing into the extension), then copy
			 * the strings back to the original user-space buffers after
			 * the ioctl returns.
			 */
			if (ctrl->cmd == NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION &&
			    ctrl->params_size >=
			    sizeof(struct nv0000_ctrl_system_get_build_version_params)) {
				struct nv0000_ctrl_system_get_build_version_params *ver =
					aux_buf;
				__u32 sz = ver->size_of_strings;

				if (sz > 0 && sz <= 512) {
					size_t ext = ctrl->params_size + 3 * sz;
					void *ext_buf = kzalloc(ext, GFP_KERNEL);

					if (!ext_buf) {
						kfree(aux_buf);
						kfree(params_buf);
						return -ENOMEM;
					}
					memcpy(ext_buf, aux_buf, ctrl->params_size);
					/* Zero guest VA pointers; QEMU fills in host VAs */
					((struct nv0000_ctrl_system_get_build_version_params *)
					 ext_buf)->p_driver_version_buffer = 0;
					((struct nv0000_ctrl_system_get_build_version_params *)
					 ext_buf)->p_version_buffer = 0;
					((struct nv0000_ctrl_system_get_build_version_params *)
					 ext_buf)->p_title_buffer = 0;
					kfree(aux_buf);
					aux_buf  = ext_buf;
					aux_size = ext;
				}
			}

			/*
			 * #79: NV0000_CTRL_CMD_GPU_GET_ID_INFO (0x202) has an
			 * output pointer szName@16 (a guest VA the host driver
			 * can't write).  The name is optional — cuInit doesn't
			 * need it and nvidia-smi gets the model elsewhere — so
			 * zero the pointer rather than forward a raw guest VA the
			 * stub would deref (the driver null-checks szName).  This
			 * closes the last unsanitized embedded pointer among the
			 * allowlisted control cmds.
			 */
			if (ctrl->cmd == 0x00000202u && ctrl->params_size >= 24)
				*(__u64 *)((char *)aux_buf + 16) = 0;
		}
	} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC &&
		   param_size == sizeof(struct nvos21_parameters) && params_buf) {
		struct nvos21_parameters *alloc = params_buf;
		if (alloc->p_alloc_parms != 0) {
			/* Same kernel-writes-back-aux requirement as the nvos64
			 * path: track the user pointer so the alloc params copy
			 * back after the ioctl. */
			aux_uptr = (void __user *)(uintptr_t)alloc->p_alloc_parms;
			size_t ap_size = 0;

			switch (alloc->h_class) {
			case NV01_DEVICE_0:
				ap_size = sizeof(struct nv0080_alloc_parameters);
				break;
			case NV20_SUBDEVICE_0:
				ap_size = sizeof(struct nv2080_alloc_parameters);
				break;
			case RM_USER_SHARED_DATA:
				ap_size = nvkvm_prof()->nv00de_alloc_size;  /* #81 */
				break;
			case FERMI_VASPACE_A:
				ap_size = nvkvm_prof()->vaspace_alloc_size; /* #81: 48 / V580 56 */
				break;
			case NV01_MEMORY_VIRTUAL:
				/* 0x70: NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS is 24B and
				 * distinct from the mem_alloc_size (NV_MEMORY_ALLOCATION_
				 * PARAMS) group below.  libGLX's EGL device enum allocs
				 * this with alloc_parms_size=0; without this case we copy
				 * 0 bytes -> kernel sees hVASpace=0 -> NV_ERR_INVALID_
				 * ARGUMENT (#84). */
				ap_size = sizeof(struct nv_memory_virtual_allocation_params);
				break;
			case NV_SEMAPHORE_SURFACE:
				/* 0xda: 16B; same alloc_parms_size=0 path as 0x70 in the
				 * EGL device-enum sequence (#84). Without this the kernel
				 * sees empty params -> NV_ERR_INVALID_ARGUMENT (0x1f) and
				 * libnvidia-eglcore later NULL-derefs the missing object. */
				ap_size = sizeof(struct nv_semaphore_surface_alloc_parameters);
				break;
			case NV01_CONTEXT_DMA:
				/* 0x0002: 32B; NVENC binds a context-DMA with size=0. Without
				 * this the kernel sees empty params -> INVALID_ARGUMENT and
				 * InitializeEncoder fails (#99). */
				ap_size = sizeof(struct nv_context_dma_allocation_params);
				break;
			case NVENC_SW_SESSION:
				/* 0xa0bc: 20B; NVENC session-tracking object, size=0.
				 * Same pattern as NV01_CONTEXT_DMA (#99). */
				ap_size = sizeof(struct nva0bc_alloc_parameters);
				break;
			case NV50_THIRD_PARTY_P2P:
				/* 0x503c: 4B; libcuda allocs this during cuInit with
				 * alloc_parms_size=0. Without this case we copy 0 bytes
				 * -> host RM returns NV_ERR_NOT_SUPPORTED (0x56) and
				 * cuInit fails CUDA_ERROR_NOT_SUPPORTED (801). */
				ap_size = sizeof(struct nv503c_alloc_parameters);
				break;
			case NV50_MEMORY_VIRTUAL:
			case NV01_MEMORY_LOCAL_USER:
			case NV01_MEMORY_SYSTEM:
				ap_size = nvkvm_prof()->mem_alloc_size;     /* #81 */
				break;
			case KEPLER_CHANNEL_GROUP_A:
				ap_size = sizeof(struct nv_channel_group_allocation_parameters);
				break;
			case FERMI_CONTEXT_SHARE_A:
				ap_size = sizeof(struct nv_ctxshare_allocation_parameters);
				break;
			case TURING_CHANNEL_GPFIFO_A:
			case AMPERE_CHANNEL_GPFIFO_A:
			case HOPPER_CHANNEL_GPFIFO_A:
			case BLACKWELL_CHANNEL_GPFIFO_A:
			case BLACKWELL_CHANNEL_GPFIFO_B:
				/* 0xc96f/0xca6f: Blackwell reuses NV_CHANNEL_ALLOC_PARAMS
				 * unchanged, so it shares chan_alloc_size (368 on 580).
				 * MEASURED on an RTX 5090: libcuda allocs 0xc96f with
				 * alloc_parms_size=0 as the last step of cuCtxCreate;
				 * without these cases we copy 0 bytes -> host RM returns
				 * NV_ERR_INVALID_ARGUMENT (0x1f) and cuCtxCreate fails
				 * CUDA_ERROR_INVALID_VALUE (1) (#101). */
				ap_size = nvkvm_prof()->chan_alloc_size;    /* #81: base / V570 +8 */
				break;
			case NV01_EVENT_OS_EVENT:
			case NV01_EVENT:   /* graphics path: outer hClass 0x0005, NV0005 params */
				ap_size = sizeof(struct nv0005_alloc_parameters);
				break;
			case GF100_DISP_SW:
				ap_size = NV9072_ALLOC_PARAMS_SIZE;
				break;
			case NV_MEMORY_MAPPER:
				ap_size = NV_MEMORY_MAPPER_ALLOC_PARAMS_SIZE;
				break;
			case VOLTA_DMA_COPY_A:
			case TURING_DMA_COPY_A:
			case AMPERE_DMA_COPY_A:
			case AMPERE_DMA_COPY_B:
			case HOPPER_DMA_COPY_A:
			case BLACKWELL_DMA_COPY_A:
			case BLACKWELL_DMA_COPY_B:
				/* 0xc9b5/0xcab5: both Blackwell copy-engine classes take
				 * NVB0B5_ALLOCATION_PARAMETERS like every other _DMA_COPY_.
				 * _A's id was wrong (0xcbb5) until #101, so neither real
				 * class had an entry here. */
				ap_size = sizeof(struct nvb0b5_allocation_parameters);
				break;
			case GT200_DEBUGGER:
				ap_size = sizeof(struct nv83de_alloc_parameters);
				break;
			case VOLTA_COMPUTE_A: case VOLTA_COMPUTE_B:
			case TURING_COMPUTE_A: case AMPERE_COMPUTE_A:
			case AMPERE_COMPUTE_B: case ADA_COMPUTE_A:
			case HOPPER_COMPUTE_A: case BLACKWELL_COMPUTE_A:
			case BLACKWELL_COMPUTE_B:
			case VOLTA_A: case TURING_A: case AMPERE_A:
			case AMPERE_B: case ADA_A: case HOPPER_A:
				ap_size = sizeof(struct nv_gr_allocation_parameters);
				break;
			}
			if (ap_size == 0) {
				/* Unsized class: forward a bounded window rather
				 * than a NULL parameter block.  NVOS21 has no
				 * paramsSize field at all, so RM always sizes by
				 * class here — it just needs the bytes. */
				ap_size = nvkvm_alloc_parms_probe_len(
						alloc->p_alloc_parms);
				pr_warn_ratelimited(
					"nvkvm: RM_ALLOC hClass=0x%x has no alloc-param size entry; forwarding a %zu-byte window\n",
					alloc->h_class, ap_size);
			}
			if (ap_size > 0) {
				aux_buf = kzalloc(ap_size, GFP_KERNEL);
				if (!aux_buf) {
					kfree(params_buf);
					return -ENOMEM;
				}
				if (copy_from_user(aux_buf,
						   (void __user *)(uintptr_t)alloc->p_alloc_parms,
						   ap_size)) {
					kfree(aux_buf);
					kfree(params_buf);
					return -EFAULT;
				}
				aux_size = ap_size;
			}
		}
	} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC &&
		   param_size == sizeof(struct nvos64_parameters) && params_buf) {
		struct nvos64_parameters *alloc = params_buf;
		if (alloc->p_alloc_parms != 0) {
			/* Track the original user pointer so the kernel-written
			 * alloc params (vaSize, vaBase, etc.) get copied back to
			 * userspace after the ioctl. Without this libcuda sees
			 * stale zeros and decides the GPU context is unusable
			 * (CUDA_ERROR_ILLEGAL_STATE on cuCtxCreate). */
			aux_uptr = (void __user *)(uintptr_t)alloc->p_alloc_parms;
			/* CUDA often leaves alloc_parms_size=0 and relies on the
			 * driver to size the buffer by hClass. Honor an explicit
			 * size if provided, otherwise fall back to the per-class
			 * size (same table as the nvos21 path above). */
			size_t ap_size = alloc->alloc_parms_size;
			bool ap_guessed = false;
			if (ap_size == 0) {
				switch (alloc->h_class) {
				case NV01_DEVICE_0:
					ap_size = sizeof(struct nv0080_alloc_parameters);
					break;
				case NV20_SUBDEVICE_0:
					ap_size = sizeof(struct nv2080_alloc_parameters);
					break;
				case RM_USER_SHARED_DATA:
					ap_size = nvkvm_prof()->nv00de_alloc_size;  /* #81 */
					break;
				case FERMI_VASPACE_A:
					ap_size = nvkvm_prof()->vaspace_alloc_size; /* #81 */
					break;
				case NV01_MEMORY_VIRTUAL:
					/* 0x70: 24B NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS,
					 * distinct from the mem_alloc_size group.  libGLX's
					 * EGL enum allocs this (nvos64) with size=0; without
					 * this the kernel sees hVASpace=0 -> INVALID_ARGUMENT
					 * and graphics bails (#84). */
					ap_size = sizeof(struct nv_memory_virtual_allocation_params);
					break;
				case NV_SEMAPHORE_SURFACE:
					/* 0xda: 16B; libGLX EGL enum allocs this (nvos64)
					 * with size=0. Without it the kernel sees empty
					 * params -> NV_ERR_INVALID_ARGUMENT (0x1f) and
					 * libnvidia-eglcore NULL-derefs later (#84). */
					ap_size = sizeof(struct nv_semaphore_surface_alloc_parameters);
					break;
					case NV01_CONTEXT_DMA:
						/* 0x0002: 32B; NVENC binds a context-DMA with size=0 ->
						 * without this the kernel sees empty params -> INVALID_
						 * ARGUMENT and InitializeEncoder fails (#99). */
						ap_size = sizeof(struct nv_context_dma_allocation_params);
						break;
					case NVENC_SW_SESSION:
						/* 0xa0bc: 20B; NVENC session-tracking object, size=0.
						 * Same pattern as NV01_CONTEXT_DMA (#99). */
						ap_size = sizeof(struct nva0bc_alloc_parameters);
						break;
					case NV50_THIRD_PARTY_P2P:
						/* 0x503c: 4B; libcuda allocs this during cuInit with
						 * alloc_parms_size=0. Without this case we copy 0 bytes
						 * -> host RM returns NV_ERR_NOT_SUPPORTED (0x56) and
						 * cuInit fails CUDA_ERROR_NOT_SUPPORTED (801). */
						ap_size = sizeof(struct nv503c_alloc_parameters);
						break;
				case NV50_MEMORY_VIRTUAL:
				case NV01_MEMORY_LOCAL_USER:
				case NV01_MEMORY_SYSTEM:
					ap_size = nvkvm_prof()->mem_alloc_size;     /* #81 */
					break;
				case KEPLER_CHANNEL_GROUP_A:
					ap_size = sizeof(struct nv_channel_group_allocation_parameters);
					break;
				case FERMI_CONTEXT_SHARE_A:
					ap_size = sizeof(struct nv_ctxshare_allocation_parameters);
					break;
				case TURING_CHANNEL_GPFIFO_A:
				case AMPERE_CHANNEL_GPFIFO_A:
				case HOPPER_CHANNEL_GPFIFO_A:
				case BLACKWELL_CHANNEL_GPFIFO_A:
				case BLACKWELL_CHANNEL_GPFIFO_B:
					/* 0xc96f/0xca6f: same NV_CHANNEL_ALLOC_PARAMS as the
					 * older GPFIFO classes. This is the path libcuda
					 * actually takes on Blackwell (nvos64, size=0) and the
					 * one whose absence broke cuCtxCreate (#101); see the
					 * nvos21 switch above. */
					ap_size = nvkvm_prof()->chan_alloc_size;    /* #81 */
					break;
				case NV01_EVENT_OS_EVENT:
				case NV01_EVENT:
					ap_size = sizeof(struct nv0005_alloc_parameters);
					break;
				case GF100_DISP_SW:
					ap_size = NV9072_ALLOC_PARAMS_SIZE;
					break;
				case NV_MEMORY_MAPPER:
					ap_size = NV_MEMORY_MAPPER_ALLOC_PARAMS_SIZE;
					break;
				case VOLTA_DMA_COPY_A:
				case TURING_DMA_COPY_A:
				case AMPERE_DMA_COPY_A:
				case AMPERE_DMA_COPY_B:
				case HOPPER_DMA_COPY_A:
				case BLACKWELL_DMA_COPY_A:
				case BLACKWELL_DMA_COPY_B:
					/* 0xc9b5/0xcab5: see the nvos21 switch above (#101). */
					ap_size = sizeof(struct nvb0b5_allocation_parameters);
					break;
				case GT200_DEBUGGER:
					ap_size = sizeof(struct nv83de_alloc_parameters);
					break;
				case VOLTA_COMPUTE_A: case VOLTA_COMPUTE_B:
				case TURING_COMPUTE_A: case AMPERE_COMPUTE_A:
				case AMPERE_COMPUTE_B: case ADA_COMPUTE_A:
				case HOPPER_COMPUTE_A: case BLACKWELL_COMPUTE_A:
				case BLACKWELL_COMPUTE_B:
				case VOLTA_A: case TURING_A: case AMPERE_A:
				case AMPERE_B: case ADA_A: case HOPPER_A:
					ap_size = sizeof(struct nv_gr_allocation_parameters);
					break;
				}
				if (ap_size == 0) {
					/* Unsized class.  Forward a bounded
					 * window and, unlike the known-class
					 * arm below, do NOT write a size back
					 * into alloc_parms_size: the caller
					 * passed 0 on purpose so that RM sizes
					 * the struct by class, and a guessed
					 * size is exactly the thing RM would
					 * reject. */
					ap_size = nvkvm_alloc_parms_probe_len(
							alloc->p_alloc_parms);
					ap_guessed = true;
					pr_warn_ratelimited(
						"nvkvm: RM_ALLOC hClass=0x%x has no alloc-param size entry; forwarding a %zu-byte window\n",
						alloc->h_class, ap_size);
				}
			}
			if (ap_size > NVKVM_SHM_SLOT_DEFAULT_SIZE) {
				kfree(params_buf);
				return -EINVAL;
			}
			if (ap_size > 0) {
				aux_buf = kzalloc(ap_size, GFP_KERNEL);
				if (!aux_buf) {
					kfree(params_buf);
					return -ENOMEM;
				}
				if (copy_from_user(aux_buf,
						   (void __user *)(uintptr_t)alloc->p_alloc_parms,
						   ap_size)) {
					kfree(aux_buf);
					kfree(params_buf);
					return -EFAULT;
				}
				aux_size = ap_size;
				/* Sync the size back so the host driver sees a
				 * consistent (params, paramsSize) pair — but only
				 * when the size is a MEASURED one.  For a guessed
				 * window the caller's 0 is the correct value to
				 * forward; see nvkvm_alloc_parms_probe_len(). */
				if (!ap_guessed)
					alloc->alloc_parms_size = (u32)ap_size;

				/* NV01_EVENT_OS_EVENT: Data is an FD pointing at one
				 * of the /dev/nvidia* char devices (not a generic
				 * eventfd — gVisor confirms via the frontendFD type
				 * check, and the driver verifies f->f_op ==
				 * nv_frontend_fops in osUserHandleToKernelPtr).  The
				 * fd libcuda passes IS already opened through our
				 * module, so we have a handle_id for it.  Same
				 * translation pattern as UVM_MM_INITIALIZE's
				 * uvm_fd: replace the user-supplied guest fd with
				 * its handle_id; the stub maps that to its local
				 * /dev/nvidia* fd. */
				if ((alloc->h_class == NV01_EVENT_OS_EVENT ||
				     alloc->h_class == NV01_EVENT) &&
				    ap_size >= sizeof(struct nv0005_alloc_parameters)) {
					struct nv0005_alloc_parameters *ep = aux_buf;
					int user_fd = (int)(int32_t)ep->data;
					if (user_fd >= 0) {
						struct file *f = fget(user_fd);
						__u32 hid = 0;
						if (f) {
							/* F-4: only read private_data if it's our fd */
							if (nvkvm_file_is_ours(f)) {
								struct nvkvm_fd_ctx *other =
									f->private_data;
								if (other && other->handle_id)
									hid = other->handle_id;
							}
							fput(f);
						}
						if (hid > 0)
							ep->data = hid;
					}
				}
			}
		}
	}

	/*
	 * Sanitize embedded pointer and FD fields before forwarding.
	 * The sanitizer replaces guest VA pointers with slot references and
	 * translates session-local FD tokens to their host-facing equivalents.
	 */
	ret = nvkvm_sanitize_ioctl_params(ctx, cmd, params_buf, param_size);
	if (ret) {
		kfree(aux_buf);
		kfree(params_buf);
		return ret;
	}

	/* Forward to host via the appropriate path */
	if (ctx->handle_id && ctx->session->isolate_id) {
		/*
		 * Isolate path: ioctl runs in the isolate process so the NVIDIA
		 * driver sees a valid VA space.  On EFAULT the isolate returns
		 * the faulting GVA; we map the backing region and retry.
		 *
		 * UVM ioctls also go through the isolate — the UVM kernel must
		 * track the GPU-memory owner, which is the isolate.  Embedded
		 * fd_tokens have already been translated to handle_ids in the
		 * sanitizer; the stub looks them up via its handle table.
		 */
		__u32 ioctl_flags = 0;
		__u64 fault_addr  = 0;
		int retries;
		int ring_rc = NVKVM_RING_TRY_PUNT;
		int vcache_miss = 0;

		/* Any UVM range teardown invalidates the VALIDATE cache (#94). */
		if (cmd == UVM_UNMAP_EXTERNAL || cmd == UVM_FREE ||
		    cmd == UVM_UNREGISTER_GPU_VASPACE || cmd == UVM_UNREGISTER_GPU ||
		    cmd == UVM_DESTROY_RANGE_GROUP)
			nvkvm_session_vcache_clear(ctx->session);

		/*
		 * UVM_VALIDATE_VA_RANGE cache (#94): libcuda re-validates the SAME
		 * (base,len) range ~1000x per pageable cuMemcpy, each a ~191us
		 * forwarded round-trip (the DtoH bottleneck).  It's an idempotent
		 * registration check, so serve a cached rm_status locally.  The
		 * cache is cleared on any UVM teardown / new migration so a stale
		 * "valid" can never outlive the range's registration.
		 */
		if (cmd == UVM_VALIDATE_VA_RANGE && params_buf && param_size >= 20) {
			struct nvkvm_session *s = ctx->session;
			u64 vbase = *(u64 *)params_buf;
			u64 vlen  = *(u64 *)((char *)params_buf + 8);
			unsigned long vfl;
			int hit = 0, k;
			u32 st = 0;

			spin_lock_irqsave(&s->vcache_lock, vfl);
			for (k = 0; k < NVKVM_VCACHE_N; k++)
				if (s->vcache[k].valid &&
				    s->vcache[k].base == vbase &&
				    s->vcache[k].len  == vlen) {
					st = s->vcache[k].status;
					hit = 1;
					break;
				}
			spin_unlock_irqrestore(&s->vcache_lock, vfl);
			if (hit) {
				*(u32 *)((char *)params_buf + 16) = st;
				ret = 0;
				goto forwarded;   /* skip the forward entirely */
			}
			vcache_miss = 1;   /* forward, then cache the result */
		}

		/*
		 * Command-buffer fast path: flat RM_CONTROLs that need no guest-
		 * or QEMU-side special handling ride the SPSC ring (the hot
		 * decode-poll controls).  GET_PID_INFO (gpi_save) and
		 * EXPORT_OBJECT_TO_FD (have_export_fd) are excluded — they need
		 * host-VMM handling; everything else the stub PUNTs if it requires
		 * per-control marshalling, and we drop to the virtqueue below.
		 */
		if (!gpi_save && !have_export_fd && !have_import_fd)
			ring_rc = nvkvm_session_ring_try(ctx, cmd,
							 params_buf, param_size,
							 aux_buf, aux_size, NULL);
		if (ring_rc != NVKVM_RING_TRY_PUNT) {
			ret = ring_rc;
			goto forwarded;
		}

		/* Re-upload already-migrated CPU pages before the GPU reads
		 * them: the slot may still hold a previous buffer's bytes if
		 * this address was reused.  Symmetric with the writeback below. */
		nvkvm_cpu_pages_refresh(ctx);

#define NVKVM_MAX_EFAULT_RETRIES 128
		for (retries = 0; retries < NVKVM_MAX_EFAULT_RETRIES; retries++) {
			fault_addr = 0;
			ret = nvkvm_virtio_ioctl_on_isolate(ctx, cmd,
							    params_buf, param_size,
							    aux_buf, aux_size,
							    ioctl_flags,
							    &fault_addr);
			if (ret != -EFAULT || !fault_addr)
				break;

			ioctl_flags |= NVKVM_IOCTL_FL_RETRY_EFAULT;

			if (nvkvm_efault_resolve(ctx, fault_addr)) {
				ret = -EFAULT;
				break;
			}
		}
forwarded:;

		/* Cache a freshly-forwarded VALIDATE result (miss path). */
		if (vcache_miss && ret == 0 && params_buf && param_size >= 20) {
			struct nvkvm_session *s = ctx->session;
			u64 vbase = *(u64 *)params_buf;
			u64 vlen  = *(u64 *)((char *)params_buf + 8);
			u32 st    = *(u32 *)((char *)params_buf + 16);
			unsigned long vfl;
			int slot;
			spin_lock_irqsave(&s->vcache_lock, vfl);
			slot = s->vcache_next++ % NVKVM_VCACHE_N;
			s->vcache[slot].base   = vbase;
			s->vcache[slot].len    = vlen;
			s->vcache[slot].status = st;
			s->vcache[slot].valid  = true;
			spin_unlock_irqrestore(&s->vcache_lock, vfl);
		}

		/*
		 * Write back any CPU pages migrated during this ioctl.
		 * The NVIDIA driver may have written results into the isolate's
		 * copy of the page (e.g. DtoH data); copy it back to the guest.
		 */
		nvkvm_cpu_pages_writeback(ctx);

		/* GET_PID_INFO: restore the caller's own pids into aux_buf (QEMU
		 * had swapped them for host pids); the kernel-filled memory data
		 * stays, so nvidia-smi sees its pid + real usage. */
		if (gpi_save)
			nvkvm_get_pid_info_restore(aux_buf, aux_size,
						   gpi_save, gpi_n);

		/* EXPORT_OBJECT_TO_FD: restore the caller's own fd in the inner
		 * params (we swapped it for a handle_id; the export keeps the fd
		 * value, so libGLX must read its own fd back). */
		if (have_export_fd && aux_buf && aux_size >= 20)
			memcpy((char *)aux_buf + 16, &orig_export_fd,
			       sizeof(orig_export_fd));

		/* IMPORT_OBJECT_FROM_FD: restore the caller's own fd at offset 0
		 * (we swapped it for a handle_id; the import keeps the fd). */
		if (have_import_fd && aux_buf && aux_size >= 4)
			memcpy(aux_buf, &orig_import_fd, sizeof(orig_import_fd));
	} else {
		/* Open establishes ctx->handle_id and ctx->session->isolate_id;
		 * an ioctl on a ctx missing either is a logic bug. The legacy
		 * non-isolate fallback (NVKVM_REQ_IOCTL) was removed in Step 3d. */
		ret = -EBADF;
	}

	/*
	 * Restore the user-space pointer fields the sanitizer (and stub) blanked.
	 * CUDA expects to read its own pointer back unchanged across the ioctl;
	 * not restoring them causes cuInit to give up with CUDA_ERROR_NO_DEVICE.
	 * Only restore on success-ish responses — if the driver returned an
	 * error we still keep the host's writes (status field etc.) but the
	 * caller's pointer should round-trip.
	 */
	if (params_buf && ret != -ENOMEM && ret != -ENOSPC && ret != -EFAULT) {
		if (have_modeset) {
			/* NVKMS: restore the caller's address ptr (round-trips
			 * unchanged; the inner params came back via the aux slot). */
			*(u64 *)((char *)params_buf + NVKVM_NVKMS_ADDR_OFF) =
				orig_modeset_addr;
		} else if (_IOC_NR(cmd) == NV_ESC_RM_CONTROL && orig_nvos54_params &&
		    param_size == sizeof(struct nvos54_parameters)) {
			((struct nvos54_parameters *)params_buf)->params =
				orig_nvos54_params;
		} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC &&
			   param_size == sizeof(struct nvos64_parameters)) {
			struct nvos64_parameters *a = params_buf;
			if (orig_nvos64_alloc)  a->p_alloc_parms      = orig_nvos64_alloc;
			if (orig_nvos64_rights) a->p_rights_requested = orig_nvos64_rights;
			/* paramsSize must round-trip too — the host driver
			 * does not write it; CUDA verifies the OUT value
			 * matches the IN. Our sanitizer fills it in by class
			 * when CUDA leaves it at 0; restore the caller's
			 * original value here. */
			if (have_nvos64_orig)   a->alloc_parms_size   = orig_nvos64_size;
		} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC && orig_nvos21_alloc &&
			   param_size == sizeof(struct nvos21_parameters)) {
			((struct nvos21_parameters *)params_buf)->p_alloc_parms =
				orig_nvos21_alloc;
		} else if (have_uvm_mm_init &&
			   param_size == sizeof(struct uvm_mm_initialize_params)) {
			((struct uvm_mm_initialize_params *)params_buf)->uvm_fd =
				orig_uvm_mm_init_fd;
		} else if (have_uvm_rm_ctrl &&
			   param_size >= (size_t)uvm_rm_ctrl_off + sizeof(__u32)) {
			*(__u32 *)((char *)params_buf + uvm_rm_ctrl_off) =
				orig_uvm_rm_ctrl_fd;
		} else if (have_fe_ctl_fd && _IOC_NR(cmd) == NV_ESC_REGISTER_FD &&
			   param_size >= sizeof(struct nv_ioctl_register_fd)) {
			((struct nv_ioctl_register_fd *)params_buf)->ctl_fd =
				orig_fe_ctl_fd;
		} else if (have_fe_os_evt_fd &&
			   (_IOC_NR(cmd) == NV_ESC_ALLOC_OS_EVENT ||
			    _IOC_NR(cmd) == NV_ESC_FREE_OS_EVENT) &&
			   param_size >= sizeof(struct nv_ioctl_alloc_os_event)) {
			((struct nv_ioctl_alloc_os_event *)params_buf)->fd =
				orig_fe_os_evt_fd;
		} else if (have_fe_nvos02_fd && _IOC_NR(cmd) == NV_ESC_RM_ALLOC_MEMORY &&
			   param_size >= sizeof(struct nv_ioctl_nvos02_parameters_with_fd)) {
			((struct nv_ioctl_nvos02_parameters_with_fd *)params_buf)->fd =
				orig_fe_nvos02_fd;
		} else if (have_fe_nvos33_fd && _IOC_NR(cmd) == NV_ESC_RM_MAP_MEMORY &&
			   param_size >= sizeof(struct nv_ioctl_nvos33_parameters_with_fd)) {
			((struct nv_ioctl_nvos33_parameters_with_fd *)params_buf)->fd =
				orig_fe_nvos33_fd;
		} else if (fake_nvos56_ok &&
			   _IOC_NR(cmd) == NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO &&
			   param_size == sizeof(struct nvos56_parameters)) {
			struct nvos56_parameters *p = params_buf;
			/* Restore caller's pointer values so libcuda sees them
			 * unchanged across the ioctl, and force status=NV_OK
			 * to mask the kernel's unavoidable OBJECT_NOT_FOUND. */
			p->p_old_cpu_address = orig_nvos56_old;
			p->p_new_cpu_address = orig_nvos56_new;
			p->status = 0;
		} else if (fake_nvos34_ok &&
			   _IOC_NR(cmd) == NV_ESC_RM_UNMAP_MEMORY &&
			   param_size == sizeof(struct nv_ioctl_nvos34_parameters)) {
			struct nv_ioctl_nvos34_parameters *p = params_buf;
			/* Same VA-lookup-fail pattern as NVOS56; fake success. */
			p->p_linear_address = orig_nvos34_va;
			p->status = 0;
		}
	}

	/*
	 * Always copy parameters back to userspace, even on driver error.
	 * The NVIDIA RM may populate response fields even when returning an error
	 * (e.g. CHECK_VERSION_STR fills version_string and returns EINVAL).
	 * Only skip if the transport itself failed (ret == -ENOMEM / -ENOSPC)
	 * and params_buf was never written to shared memory.
	 */
	if (param_size > 0 && uparams != NULL &&
	    ret != -ENOMEM && ret != -ENOSPC && ret != -EFAULT) {
		if (copy_to_user(uparams, params_buf, param_size))
			ret = -EFAULT;
	}

	/* Copy back the aux buffer (RM_CONTROL output params) to userspace */
	if (aux_buf && aux_uptr && ret != -ENOMEM && ret != -ENOSPC &&
	    ret != -EFAULT) {
		/*
		 * For NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION: the original
		 * user-space pointer fields were replaced with zeros before
		 * forwarding to QEMU.  QEMU wrote the version strings into the
		 * extension area of aux_buf (at offsets sizeof(params), +sz, +2*sz).
		 * Copy the strings back to the original user-space buffer addresses
		 * that the caller passed in, then copy only the base params struct
		 * to aux_uptr so the embedded pointers in userspace remain valid.
		 */
		if (_IOC_NR(cmd) == NV_ESC_RM_CONTROL && params_buf) {
			struct nvos54_parameters *ctrl = params_buf;

			/*
			 * InfoList family (NV2080_CTRL_CMD_GR_GET_INFO and friends):
			 * aux_buf is sized to params_size + list_size*8; the trailing
			 * region holds the driver-written list. Copy it back to the
			 * original user-space list address, then restore the pointer
			 * in aux_buf and write only the base params struct out.
			 */
			/* FIFO_GET_CHANNELLIST response: copy both lists back to
			 * the original user pointers, then restore those pointers
			 * in aux_buf so userspace sees them unchanged. */
			if (ctrl->cmd == NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST &&
			    ctrl->params_size >= 24 &&
			    aux_size > ctrl->params_size) {
				struct {
					__u32 num_channels;
					__u32 pad;
					__u64 p_handles;
					__u64 p_list;
				} orig;
				if (!copy_from_user(&orig, aux_uptr, sizeof(orig)) &&
				    orig.num_channels > 0) {
					size_t list_bytes = (size_t)orig.num_channels *
							    sizeof(__u32);
					if (aux_size >= ctrl->params_size + 2 * list_bytes) {
						/* __must_check; see the note on the version
						 * strings below -- unchecked, this fails the
						 * build under RHEL's -Werror and silently
						 * reports success on a bad guest pointer. */
						if (orig.p_handles &&
						    copy_to_user(
							(void __user *)(uintptr_t)orig.p_handles,
							(char *)aux_buf + ctrl->params_size,
							list_bytes))
							ret = -EFAULT;
						if (orig.p_list &&
						    copy_to_user(
							(void __user *)(uintptr_t)orig.p_list,
							(char *)aux_buf + ctrl->params_size +
							list_bytes,
							list_bytes))
							ret = -EFAULT;
					}
					/* Restore pointers */
					*(__u64 *)((char *)aux_buf + 8)  = orig.p_handles;
					*(__u64 *)((char *)aux_buf + 16) = orig.p_list;
				}
				if (copy_to_user(aux_uptr, aux_buf, ctrl->params_size))
					ret = -EFAULT;
				goto done_aux_copy;
			}

			{
				unsigned int entry_size =
					nvkvm_ctrl_list_entry_size(ctrl->cmd);
				if (entry_size &&
				    ctrl->params_size >= 16 &&
				    aux_size > ctrl->params_size) {
					struct {
						__u32 list_size;
						__u32 pad;
						__u64 list_ptr;
					} orig;
					if (!copy_from_user(&orig, aux_uptr, sizeof(orig)) &&
					    orig.list_size > 0 && orig.list_ptr != 0) {
						size_t list_bytes =
							(size_t)orig.list_size *
							entry_size;
						if (aux_size >= ctrl->params_size + list_bytes) {
							if (copy_to_user(
								(void __user *)(uintptr_t)orig.list_ptr,
								(char *)aux_buf + ctrl->params_size,
								list_bytes))
								ret = -EFAULT;
						}
						/* Restore original pointer so userspace sees its VA */
						*(__u64 *)((char *)aux_buf + 8) = orig.list_ptr;
					}
					if (copy_to_user(aux_uptr, aux_buf, ctrl->params_size))
						ret = -EFAULT;
					goto done_aux_copy;
				}
			}

			if (ctrl->cmd == NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION &&
			    ctrl->params_size >=
			    sizeof(struct nv0000_ctrl_system_get_build_version_params) &&
			    aux_size > ctrl->params_size) {
				/*
				 * aux_size = params_size + 3*sz; the extension area
				 * holds [drv_string][ver_string][title_string].
				 * We read the original user-space string pointers from
				 * the ORIGINAL copy of the params struct (in aux_uptr).
				 */
				struct nv0000_ctrl_system_get_build_version_params orig;
				__u32 sz;

				if (!copy_from_user(&orig, aux_uptr, sizeof(orig))) {
					/*
					 * SECOND fetch of size_of_strings: the
					 * allocation above was sized from the FIRST
					 * fetch (out of our own copy).  Userspace can
					 * change the field in between, so `sz` here is
					 * NOT a safe bound on aux_buf -- bounding it
					 * against the ABI max only proves it is a legal
					 * size, not that we allocated that much.  Check
					 * it against the extension we actually have, the
					 * same way the FIFO_GET_CHANNELLIST and InfoList
					 * writebacks above already do; without this a
					 * shrink-then-grow reads ~1.5 KiB of adjacent
					 * slab out to userspace.
					 */
					size_t need;

					sz = orig.size_of_strings;
					need = (size_t)ctrl->params_size +
					       3 * (size_t)sz;
					if (sz > 0 && sz <= 512 &&
					    aux_size >= need) {
						char *ext = (char *)aux_buf +
							    ctrl->params_size;
						/*
						 * Copy version strings to original user buffers.
						 *
						 * The return value is not optional.  copy_to_user()
						 * is __must_check, and RHEL builds modules with
						 * -Werror, so ignoring it does not merely warn --
						 * it fails the build outright on a CentOS/RHEL 9
						 * guest (measured on 5.14.0-737.el9).  It was also
						 * wrong on its own terms: a guest that passes a bad
						 * pointer here got a success return and then read
						 * whatever was already in its buffer.  Same
						 * -EFAULT convention as the aux_buf copy below.
						 */
						if (orig.p_driver_version_buffer &&
						    copy_to_user((void __user *)(uintptr_t)
								 orig.p_driver_version_buffer,
								 ext, sz))
							ret = -EFAULT;
						if (orig.p_version_buffer &&
						    copy_to_user((void __user *)(uintptr_t)
								 orig.p_version_buffer,
								 ext + sz, sz))
							ret = -EFAULT;
						if (orig.p_title_buffer &&
						    copy_to_user((void __user *)(uintptr_t)
								 orig.p_title_buffer,
								 ext + 2 * sz, sz))
							ret = -EFAULT;
					}
					/*
					 * Restore the original user-space pointer values
					 * in aux_buf before copying it back so CUDA can
					 * still read its own pointer fields from the
					 * returned params struct.
					 */
					((struct nv0000_ctrl_system_get_build_version_params *)
					 aux_buf)->p_driver_version_buffer =
						orig.p_driver_version_buffer;
					((struct nv0000_ctrl_system_get_build_version_params *)
					 aux_buf)->p_version_buffer = orig.p_version_buffer;
					((struct nv0000_ctrl_system_get_build_version_params *)
					 aux_buf)->p_title_buffer = orig.p_title_buffer;
				}
				/* Copy only the base params struct (not the extension) */
				if (copy_to_user(aux_uptr, aux_buf, ctrl->params_size))
					ret = -EFAULT;
				goto done_aux_copy;
			}
		}
		/* NVKMS REGISTER_SURFACE: put the caller's own plane fds back
		 * (we swapped them for handle_ids); the fd is IN, value kept. */
		{
			int k;
			for (k = 0; k < regsurf_nfd; k++)
				if (orig_regsurf_off[k] + sizeof(__s32) <= aux_size)
					memcpy((char *)aux_buf + orig_regsurf_off[k],
					       &orig_regsurf_fd[k], sizeof(__s32));
		}
		if (copy_to_user(aux_uptr, aux_buf, aux_size))
			ret = -EFAULT;
done_aux_copy:
		;
	}

	kfree(gpi_save);
	kfree(aux_buf);
	kfree(params_buf);
	return ret;
}

/* ── mmap ─────────────────────────────────────────────────────────────────── */

static int nvkvm_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct nvkvm_fd_ctx *ctx = filp->private_data;

	if (!ctx)
		return -EBADF;

	return nvkvm_mmap_request(ctx, vma);
}

/* ── poll ─────────────────────────────────────────────────────────────────── */

static __poll_t nvkvm_poll(struct file *filp, poll_table *wait)
{
	struct nvkvm_fd_ctx *ctx = filp->private_data;
	__poll_t events;

	if (!ctx)
		return EPOLLERR;

	poll_wait(filp, &ctx->poll_wq, wait);
	events = atomic_xchg(&ctx->poll_events, 0);

	/* #127: about to block with nothing pending — ask the host to relay this
	 * os-event fd's readiness over VQ_EVT, so a CUDA blocking-sync wait
	 * (CU_CTX_SCHED_BLOCKING_SYNC, blocking cuEventSynchronize, NCCL) wakes
	 * promptly instead of eating libnvidia's ~18ms poll-timeout fallback.
	 * One-shot: the stub disarms after firing and nvkvm_evt_deliver clears
	 * poll_armed, so the next wait re-arms.  The arm is a fast control-plane
	 * round-trip done at most once per wait. */
	if (!events && ctx->handle_id && ctx->session &&
	    ctx->session->isolate_id &&
	    atomic_cmpxchg(&ctx->poll_armed, 0, 1) == 0)
		nvkvm_virtio_poll_arm(ctx->session->isolate_id,
				      ctx->handle_id, 1);
	return events;
}

/* ── virtio device probe/remove ───────────────────────────────────────────── */

static int nvkvm_virtio_probe(struct virtio_device *vdev);
static void nvkvm_virtio_remove(struct virtio_device *vdev);

static const struct virtio_device_id nvkvm_virtio_id_table[] = {
	{ VIRTIO_ID_NVGPU, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver nvkvm_virtio_driver = {
	.driver.name  = "virtio-nvgpu",
	.driver.owner = THIS_MODULE,
	.id_table     = nvkvm_virtio_id_table,
	.probe        = nvkvm_virtio_probe,
	.remove       = nvkvm_virtio_remove,
};

static int nvkvm_virtio_probe(struct virtio_device *vdev)
{
	int ret;

	pr_info("nvkvm: probe called for virtio device id=0x%x\n",
		vdev->id.device);

	ret = nvkvm_virtio_init(vdev, &nvkvm);
	if (ret) {
		dev_err(&vdev->dev, "nvkvm: virtio init failed: %d\n", ret);
		return ret;
	}

	/*
	 * Read the host driver version from the shared memory control block
	 * and confirm the protocol version matches before accepting ioctls.
	 */
	ret = nvkvm_negotiate_version(&nvkvm);
	if (ret) {
		dev_err(&vdev->dev,
			"nvkvm: protocol version negotiation failed: %d\n",
			ret);
		nvkvm_virtio_fini(&nvkvm);
		return ret;
	}

	/*
	 * Graphics stack (nvidia-drm render node + /dev/nvidia-modeset).  QEMU
	 * dictates availability via NVKVM_CONFIG_F_GRAPHICS: compute-only VMs
	 * skip the DRM node and drop the modeset device (registered at module
	 * init) so the guest exposes no graphics surface.  Non-fatal either way:
	 * compute works without it.  Parent = the virtio device so the DRM core
	 * builds /sys/.../<virtio-dev>/drm/renderD128 that the NVIDIA ICD needs.
	 */
#ifdef NVKVM_GRAPHICS
	if (nvkvm.graphics_enabled)
		nvkvm_drm_init(&vdev->dev);
	else {
		nvkvm_modeset_unregister();
		pr_info("nvkvm: graphics disabled by host — compute-only\n");
	}
#else
	/* Compute-only build (NVKVM_GRAPHICS=0): no DRM/KMS code is linked in, so
	 * never expose a graphics surface regardless of what the host advertises. */
	nvkvm_modeset_unregister();
	pr_info("nvkvm: compute-only build (no DRM/KMS/modeset)\n");
#endif

	return 0;
}

static void nvkvm_virtio_remove(struct virtio_device *vdev)
{
#ifdef NVKVM_GRAPHICS
	nvkvm_drm_fini();
#endif
	nvkvm_virtio_fini(&nvkvm);
}

/* ── Module init/exit ─────────────────────────────────────────────────────── */

static int __init nvkvm_init(void)
{
	int ret;

	memset(&nvkvm, 0, sizeof(nvkvm));
	mutex_init(&nvkvm.sessions_lock);
	idr_init(&nvkvm.sessions_idr);

	ret = register_virtio_driver(&nvkvm_virtio_driver);
	if (ret) {
		pr_err("nvkvm: failed to register virtio driver: %d\n", ret);
		return ret;
	}

	ret = register_devices();
	if (ret) {
		unregister_virtio_driver(&nvkvm_virtio_driver);
		return ret;
	}

	nvkvm_hostfile_init();

	pr_info("nvkvm: NVIDIA GPU passthrough guest module loaded\n");
	return 0;
}

static void __exit nvkvm_exit(void)
{
	nvkvm_hostfile_exit();
	unregister_devices();
	unregister_virtio_driver(&nvkvm_virtio_driver);
	idr_destroy(&nvkvm.sessions_idr);
	pr_info("nvkvm: module unloaded\n");
}

module_init(nvkvm_init);
module_exit(nvkvm_exit);
