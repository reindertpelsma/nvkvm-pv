// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_hostfile.c — synthesize the proc/sys files libcuda probes,
 * fronting them with live reads of the host-side originals via virtio.
 *
 * libcuda parses /proc/driver/nvidia/params and checks
 * /sys/module/nvidia{,_uvm}/initstate during cuInit / cuCtxCreate.  The
 * real nvidia.ko is not loaded in the guest, so these paths don't exist
 * and libcuda bails into CUDA_ERROR_UNKNOWN (999).
 *
 * This module installs proc entries and sysfs kobjects that delegate
 * each read to QEMU via NVKVM_REQ_READ_HOST_FILE.  The selection enum
 * is the security boundary: the guest never names a path, just a
 * file_id; QEMU's whitelist maps it to a host path.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>

#include "nvkvm.h"

/* PCI BDF of the virtio-nvgpu device in the guest.  libcuda probes
 * /proc/driver/nvidia/gpus/<bdf>/... using the *guest* BDF, so we expose
 * the synthetic dir under that name. */
#define NVKVM_GUEST_GPU_BDF  "0000:00:07.0"

/* ── proc-entry helpers ───────────────────────────────────────────────────── */

struct nvkvm_hostfile_entry {
	const char *name;        /* basename under parent */
	__u32       file_id;     /* NVKVM_HFILE_* */
};

static int hostfile_show(struct seq_file *m, void *v)
{
	struct nvkvm_hostfile_entry *e = m->private;
	int shm_slot;
	void *slot_ptr;
	__u32 nbytes = 0;
	int ret;

	(void)v;

	shm_slot = nvkvm_slot_alloc(&nvkvm);
	if (shm_slot < 0)
		return -ENOMEM;
	slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
	if (!slot_ptr) { nvkvm_slot_free(&nvkvm, shm_slot); return -ENOMEM; }

	ret = nvkvm_virtio_read_host_file(e->file_id, (__u32)shm_slot,
					  NVKVM_HFILE_MAX_SIZE, &nbytes);
	if (ret == 0 && nbytes > 0 && nbytes <= NVKVM_HFILE_MAX_SIZE)
		seq_write(m, slot_ptr, nbytes);

	nvkvm_slot_free(&nvkvm, shm_slot);
	return ret;
}

static int hostfile_open(struct inode *inode, struct file *file)
{
	struct nvkvm_hostfile_entry *e = pde_data(inode);
	return single_open(file, hostfile_show, e);
}

static const struct proc_ops hostfile_pops = {
	.proc_open    = hostfile_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ── sysfs (kobject) helpers for /sys/module/nvidia{,_uvm}/initstate ─── */

struct nvkvm_sys_kobj {
	struct kobject *kobj;
	struct nvkvm_hostfile_entry entry;
};

static ssize_t initstate_show(struct kobject *kobj, struct kobj_attribute *a,
			      char *buf)
{
	__u32 file_id;
	int shm_slot;
	void *p;
	__u32 nbytes = 0;
	int ret;

	(void)a;
	file_id = !strcmp(kobject_name(kobj), "nvidia_uvm")
		  ? NVKVM_HFILE_NVIDIA_UVM_INITSTATE
		  : NVKVM_HFILE_NVIDIA_INITSTATE;

	shm_slot = nvkvm_slot_alloc(&nvkvm);
	if (shm_slot < 0) return -ENOMEM;
	p = nvkvm_slot_addr(&nvkvm, shm_slot);
	if (!p) { nvkvm_slot_free(&nvkvm, shm_slot); return -ENOMEM; }

	ret = nvkvm_virtio_read_host_file(file_id, (__u32)shm_slot,
					  NVKVM_HFILE_MAX_SIZE, &nbytes);
	if (ret == 0 && nbytes > 0 && nbytes <= PAGE_SIZE - 1) {
		memcpy(buf, p, nbytes);
	} else {
		nbytes = 0;
	}
	nvkvm_slot_free(&nvkvm, shm_slot);
	return (ssize_t)nbytes;
}

static struct kobj_attribute initstate_attr =
	__ATTR(initstate, 0444, initstate_show, NULL);

/* Module-global state */
static struct proc_dir_entry *proc_nvidia_dir;
static struct proc_dir_entry *proc_gpus_dir;
static struct proc_dir_entry *proc_gpu_bdf_dir;
static struct kobject *sys_nvidia_kobj;
static struct kobject *sys_nvidia_uvm_kobj;

static struct nvkvm_hostfile_entry params_entry = {
	.name = "params", .file_id = NVKVM_HFILE_NVIDIA_PARAMS,
};
static struct nvkvm_hostfile_entry info_entry = {
	.name = "information", .file_id = NVKVM_HFILE_NVIDIA_INFORMATION,
};
static struct nvkvm_hostfile_entry reg_entry = {
	.name = "registry", .file_id = NVKVM_HFILE_NVIDIA_REG_BASE,
};

int nvkvm_hostfile_init(void)
{
	int ret;

	proc_nvidia_dir = proc_mkdir("driver/nvidia", NULL);
	if (!proc_nvidia_dir)
		return -ENOMEM;

	if (!proc_create_data("params", 0444, proc_nvidia_dir,
			      &hostfile_pops, &params_entry))
		pr_warn("nvkvm: failed to create /proc/driver/nvidia/params\n");

	proc_gpus_dir = proc_mkdir("gpus", proc_nvidia_dir);
	if (proc_gpus_dir) {
		proc_gpu_bdf_dir = proc_mkdir(NVKVM_GUEST_GPU_BDF, proc_gpus_dir);
		if (proc_gpu_bdf_dir) {
			proc_create_data("information", 0444, proc_gpu_bdf_dir,
					 &hostfile_pops, &info_entry);
			proc_create_data("registry", 0444, proc_gpu_bdf_dir,
					 &hostfile_pops, &reg_entry);
		}
	}

	sys_nvidia_kobj = kobject_create_and_add("nvidia", THIS_MODULE->mkobj.kobj.parent);
	if (sys_nvidia_kobj) {
		ret = sysfs_create_file(sys_nvidia_kobj, &initstate_attr.attr);
		if (ret) pr_warn("nvkvm: nvidia/initstate sysfs failed %d\n", ret);
	}
	sys_nvidia_uvm_kobj = kobject_create_and_add("nvidia_uvm",
						     THIS_MODULE->mkobj.kobj.parent);
	if (sys_nvidia_uvm_kobj) {
		ret = sysfs_create_file(sys_nvidia_uvm_kobj, &initstate_attr.attr);
		if (ret) pr_warn("nvkvm: nvidia_uvm/initstate sysfs failed %d\n", ret);
	}
	return 0;
}

void nvkvm_hostfile_exit(void)
{
	if (sys_nvidia_uvm_kobj) {
		sysfs_remove_file(sys_nvidia_uvm_kobj, &initstate_attr.attr);
		kobject_put(sys_nvidia_uvm_kobj);
		sys_nvidia_uvm_kobj = NULL;
	}
	if (sys_nvidia_kobj) {
		sysfs_remove_file(sys_nvidia_kobj, &initstate_attr.attr);
		kobject_put(sys_nvidia_kobj);
		sys_nvidia_kobj = NULL;
	}
	if (proc_gpu_bdf_dir) {
		remove_proc_subtree(NVKVM_GUEST_GPU_BDF, proc_gpus_dir);
		proc_gpu_bdf_dir = NULL;
	}
	if (proc_gpus_dir) {
		remove_proc_subtree("gpus", proc_nvidia_dir);
		proc_gpus_dir = NULL;
	}
	if (proc_nvidia_dir) {
		remove_proc_subtree("driver/nvidia", NULL);
		proc_nvidia_dir = NULL;
	}
}
