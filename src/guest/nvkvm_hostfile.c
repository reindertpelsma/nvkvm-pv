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

/*
 * libcuda and NVML look per-GPU files up as
 * /proc/driver/nvidia/gpus/<bdf>/<leaf>, where <bdf> is the BDF that RM
 * reports for that GPU.  RM here is the HOST's, so those are HOST BDFs --
 * confirmed by strace: on a two-GPU host nvidia-smi probes
 * gpus/0000:00:07.0/ and gpus/0000:00:09.0/, and nothing in the guest lives
 * at 00:09.0.  So the synthetic tree is named from the host's BDFs, one
 * directory per GPU, discovered at init via READ_HOST_FILE.
 *
 * (This used to be a single hardcoded "0000:00:07.0" -- the guest slot the
 * identity PCI device sits at.  That matched only by coincidence, on hosts
 * whose first GPU happens to also be at 00:07.0.)
 */

/* ── proc-entry helpers ───────────────────────────────────────────────────── */

struct nvkvm_hostfile_entry {
	const char *name;        /* basename under parent */
	__u32       file_id;     /* NVKVM_HFILE_* */
	__u32       gpu_index;   /* which host GPU, for per-GPU files; 0 otherwise */
};

static int hostfile_show(struct seq_file *m, void *v)
{
	struct nvkvm_hostfile_entry *e = m->private;
	int shm_slot;
	void *slot_ptr;
	__u32 nbytes = 0;
	int ret;

	(void)v;

	/* Readable with no device attached: procfs is created from module_init,
	 * not from probe.  Say ENODEV rather than ENOMEM -- there is nothing
	 * wrong with memory. */
	if (!nvkvm_transport_ready(&nvkvm))
		return -ENODEV;
	shm_slot = nvkvm_slot_alloc(&nvkvm);
	if (shm_slot < 0)
		return -ENOMEM;
	slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
	if (!slot_ptr) { nvkvm_slot_free(&nvkvm, shm_slot); return -ENODEV; }

	ret = nvkvm_virtio_read_host_file(e->file_id, e->gpu_index,
					  (__u32)shm_slot,
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

	/* Same as hostfile_show(): the sysfs attribute exists with no device. */
	if (!nvkvm_transport_ready(&nvkvm))
		return -ENODEV;
	shm_slot = nvkvm_slot_alloc(&nvkvm);
	if (shm_slot < 0) return -ENOMEM;
	p = nvkvm_slot_addr(&nvkvm, shm_slot);
	if (!p) { nvkvm_slot_free(&nvkvm, shm_slot); return -ENODEV; }

	/* Host-wide file: QEMU ignores gpu_index for these. */
	ret = nvkvm_virtio_read_host_file(file_id, 0, (__u32)shm_slot,
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
/* One "gpus/<host-bdf>/" directory per GPU, plus the entry structs backing
 * its files.  Indexed by host GPU index, which is exactly the gpu_index
 * QEMU resolves against its own sorted BDF list. */
static struct proc_dir_entry *proc_gpu_dirs[NVKVM_MAX_GPUS];
static struct nvkvm_hostfile_entry gpu_info_entries[NVKVM_MAX_GPUS];
static struct nvkvm_hostfile_entry gpu_reg_entries[NVKVM_MAX_GPUS];
static struct kobject *sys_nvidia_kobj;
static struct kobject *sys_nvidia_uvm_kobj;

/*
 * Static stubs.
 *
 * Several files under /proc/driver/nvidia are probed by tooling that treats
 * ENOENT as "no NVIDIA driver present" — nvidia-container-toolkit and most
 * install/detection scripts among them.  Only `version` carries information
 * worth forwarding from the host; the rest exist so a probe finds a file of the
 * right shape, and deliberately disclose nothing about the host:
 *
 *   registry        empty.  The host's is empty too, but even when it is not it
 *                   holds host-wide driver parameter overrides.
 *   suspend         the real file is 0644 and is a COMMAND file: writing to it
 *   suspend_depth   drives host power transitions.  Ours are 0444 and inert.
 *                   Read content matches the host's so a parser sees valid
 *                   options, but nothing here can act on the host.
 *   warnings/README static NVIDIA boilerplate, byte-identical on every install.
 *   patches/README  likewise.
 *   capabilities/   directories only, no entries.  The real tree exposes MIG and
 *                   fabric-imex topology; an empty tree is the honest answer for
 *                   a guest with no MIG, and discloses no host topology.
 */
static int stub_show(struct seq_file *m, void *v)
{
	(void)v;
	seq_puts(m, (const char *)m->private);
	return 0;
}

static int stub_open(struct inode *inode, struct file *file)
{
	return single_open(file, stub_show, pde_data(inode));
}

static const struct proc_ops stub_pops = {
	.proc_open    = stub_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static char stub_registry[]   = "";
static char stub_suspend[]    = "suspend hibernate resume\n";
static char stub_suspend_dep[] = "default modeset uvm\n";
static char stub_warn_readme[] =
	"The NVIDIA graphics driver tries to detect potential problems\n"
	"with the host system and warns about them using the system's\n"
	"logging mechanisms. Important warning message are also logged\n"
	"to dedicated text files in this directory.\n";
static char stub_patch_readme[] =
	"The NVIDIA graphics driver's kernel interface files can be\n"
	"patched to improve compatibility with new Linux kernels or to\n"
	"fix bugs in these files. When applied, each official patch\n"
	"provides a short text file with a short description of itself\n"
	"in this directory.\n";

static struct nvkvm_hostfile_entry version_entry = {
	.name = "version", .file_id = NVKVM_HFILE_NVIDIA_VERSION,
};
static struct nvkvm_hostfile_entry params_entry = {
	.name = "params", .file_id = NVKVM_HFILE_NVIDIA_PARAMS,
};

/* ── host-GPU discovery ──────────────────────────────────────────────────
 *
 * QEMU resolves a per-GPU file request as
 * /proc/driver/nvidia/gpus/<its own sorted BDF list>[gpu_index]/<leaf>, and
 * answers EINVAL once gpu_index runs past the end of that list.  So walking
 * gpu_index upward until the read fails both counts the host's GPUs and,
 * from each `information` body, yields the BDF to name its directory with --
 * without adding a request type, and without the guest ever naming a path.
 */

/* Same shape check QEMU applies to a BDF: DDDD:BB:DD.F, all hex. */
static bool nvkvm_bdf_valid(const char *s)
{
	int i;

	for (i = 0; i < NVKVM_BDF_STRLEN; i++) {
		char c = s[i];

		if (i == 4 || i == 7) {
			if (c != ':') return false;
		} else if (i == 10) {
			if (c != '.') return false;
		} else if (!((c >= '0' && c <= '9') ||
			     (c >= 'a' && c <= 'f') ||
			     (c >= 'A' && c <= 'F'))) {
			return false;
		}
	}
	return true;
}

/* Pull the BDF out of an `information` body's "Bus Location:  0000:00:07.0"
 * line.  Returns false if the line is absent or malformed. */
static bool nvkvm_parse_bus_location(const char *buf, size_t len, char *out)
{
	static const char key[] = "Bus Location:";
	const size_t klen = sizeof(key) - 1;
	size_t i, k;

	if (len < klen)
		return false;
	for (i = 0; i + klen <= len; i++) {
		if (memcmp(buf + i, key, klen))
			continue;
		i += klen;
		while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
			i++;
		if (i + NVKVM_BDF_STRLEN > len)
			return false;
		for (k = 0; k < NVKVM_BDF_STRLEN; k++)
			out[k] = buf[i + k];
		out[NVKVM_BDF_STRLEN] = '\0';
		return nvkvm_bdf_valid(out);
	}
	return false;
}

int nvkvm_hostfile_discover_gpus(void)
{
	int i, shm_slot, count = 0;
	void *slot_ptr;
	char *tmp;

	/* Called from register_devices(), which runs whether or not probe did. */
	if (!nvkvm_transport_ready(&nvkvm))
		return -ENODEV;
	shm_slot = nvkvm_slot_alloc(&nvkvm);
	if (shm_slot < 0)
		return -ENOMEM;
	slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
	if (!slot_ptr) {
		nvkvm_slot_free(&nvkvm, shm_slot);
		return -ENODEV;
	}
	tmp = kmalloc(NVKVM_HFILE_MAX_SIZE, GFP_KERNEL);
	if (!tmp) {
		nvkvm_slot_free(&nvkvm, shm_slot);
		return -ENOMEM;
	}

	for (i = 0; i < NVKVM_MAX_GPUS; i++) {
		__u32 nbytes = 0;
		int ret = nvkvm_virtio_read_host_file(
				NVKVM_HFILE_NVIDIA_INFORMATION, (__u32)i,
				(__u32)shm_slot, NVKVM_HFILE_MAX_SIZE, &nbytes);

		/* EINVAL here is the end of QEMU's BDF list, not a failure. */
		if (ret || nbytes == 0)
			break;
		if (nbytes > NVKVM_HFILE_MAX_SIZE)
			nbytes = NVKVM_HFILE_MAX_SIZE;
		memcpy(tmp, slot_ptr, nbytes);
		if (!nvkvm_parse_bus_location(tmp, nbytes, nvkvm.gpu_bdf[count])) {
			pr_warn("nvkvm: host GPU %d has no parsable Bus Location; stopping discovery\n",
				i);
			break;
		}
		count++;
	}

	kfree(tmp);
	nvkvm_slot_free(&nvkvm, shm_slot);
	return count;
}

int nvkvm_hostfile_init(void)
{
	int ret;

	proc_nvidia_dir = proc_mkdir("driver/nvidia", NULL);
	if (!proc_nvidia_dir)
		return -ENOMEM;

	if (!proc_create_data("params", 0444, proc_nvidia_dir,
			      &hostfile_pops, &params_entry))
		pr_warn("nvkvm: failed to create /proc/driver/nvidia/params\n");

	/* Forwarded from the host: the NVRM banner.  The driver version and build
	 * date are already known to the guest (nvidia-smi reports them, and guest
	 * userspace must be version-matched to work at all), so this discloses
	 * nothing new — but its absence makes detection tooling conclude there is
	 * no driver. */
	if (!proc_create_data("version", 0444, proc_nvidia_dir,
			      &hostfile_pops, &version_entry))
		pr_warn("nvkvm: failed to create /proc/driver/nvidia/version\n");

	/* Inert stubs — see the comment above stub_show(). */
	proc_create_data("registry", 0444, proc_nvidia_dir,
			 &stub_pops, stub_registry);
	proc_create_data("suspend", 0444, proc_nvidia_dir,
			 &stub_pops, stub_suspend);
	proc_create_data("suspend_depth", 0444, proc_nvidia_dir,
			 &stub_pops, stub_suspend_dep);
	{
		struct proc_dir_entry *d;

		d = proc_mkdir("warnings", proc_nvidia_dir);
		if (d)
			proc_create_data("README", 0444, d, &stub_pops,
					 stub_warn_readme);
		d = proc_mkdir("patches", proc_nvidia_dir);
		if (d)
			proc_create_data("README", 0444, d, &stub_pops,
					 stub_patch_readme);
		/* Directories only: an empty capabilities tree is the honest
		 * answer for a guest with no MIG, and leaks no host topology. */
		d = proc_mkdir("capabilities", proc_nvidia_dir);
		if (d) {
			proc_mkdir("mig", d);
			proc_mkdir("gpu0", d);
		}
	}

	/* One directory per host GPU, named with that GPU's host BDF.  Anything
	 * beyond what discovery found (an explicit num_gpus= override that
	 * overshoots) gets no directory: better a missing dir than one named
	 * after a GPU that is not there. */
	proc_gpus_dir = proc_mkdir("gpus", proc_nvidia_dir);
	if (proc_gpus_dir) {
		int i;

		for (i = 0; i < nvkvm.num_gpus && i < NVKVM_MAX_GPUS; i++) {
			if (!nvkvm.gpu_bdf[i][0])
				continue;

			proc_gpu_dirs[i] = proc_mkdir(nvkvm.gpu_bdf[i],
						      proc_gpus_dir);
			if (!proc_gpu_dirs[i]) {
				pr_warn("nvkvm: could not create gpus/%s\n",
					nvkvm.gpu_bdf[i]);
				continue;
			}

			gpu_info_entries[i].name      = "information";
			gpu_info_entries[i].file_id   = NVKVM_HFILE_NVIDIA_INFORMATION;
			gpu_info_entries[i].gpu_index = (__u32)i;
			gpu_reg_entries[i].name       = "registry";
			gpu_reg_entries[i].file_id    = NVKVM_HFILE_NVIDIA_REG_BASE;
			gpu_reg_entries[i].gpu_index  = (__u32)i;

			proc_create_data("information", 0444, proc_gpu_dirs[i],
					 &hostfile_pops, &gpu_info_entries[i]);
			proc_create_data("registry", 0444, proc_gpu_dirs[i],
					 &hostfile_pops, &gpu_reg_entries[i]);
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
	{
		int i;

		for (i = 0; i < NVKVM_MAX_GPUS; i++) {
			if (!proc_gpu_dirs[i])
				continue;
			remove_proc_subtree(nvkvm.gpu_bdf[i], proc_gpus_dir);
			proc_gpu_dirs[i] = NULL;
		}
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
