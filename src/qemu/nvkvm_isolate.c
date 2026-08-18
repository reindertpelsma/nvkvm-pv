/*
 * nvkvm_isolate.c — QEMU-side isolate process manager (multi-inflight)
 *
 * Each isolate has a dedicated reader thread that multiplexes IOCTL responses
 * by txn_id onto per-caller condvars allocated on the callers' stacks.
 * Non-IOCTL commands serialize via sync_lock + sync_cond (one at a time).
 * All socket writes go through write_lock (prevents partial-send interleaving).
 *
 * The stub binary is embedded as nvkvm_stub_elf[] generated at build time
 * from src/stub/nvkvm_stub; loaded via memfd_create + fexecve (no disk file).
 *
 * Lock order: sync_lock > write_lock > lock
 */

#include "qemu/osdep.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <linux/capability.h>

/* Isolation-mode config + the UID-separation primitives (header-only so the
 * security test in tests/security/uid_isolate_test.c exercises the same code
 * this device runs, not a copy of it). */
#include "nvkvm_isolate_uid.h"

#ifndef MS_REC
#define MS_REC      16384
#endif
#ifndef MS_PRIVATE
#define MS_PRIVATE  (1UL << 18)
#endif
/* Also defined in nvkvm_isolate_proto.h; guard so the QEMU build tree's
 * header-copy timing can't break compilation (fixed protocol value). */
#ifndef NVKVM_DEV_DIRFD
#define NVKVM_DEV_DIRFD 4
#endif

#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT            47
#define PR_CAP_AMBIENT_CLEAR_ALL  4
#endif

/*
 * Isolate lockdown (audit C6/hardening, HARDENING_PLAN.md Phase 0).
 *
 * Run in the just-forked child, before exec, while still privileged enough to
 * create namespaces.  Turns the stub into a rootless, namespaced, capability-
 * less sandbox so a stub RCE cannot reach the host.  Sequence (this commit,
 * step A1 — no pid/mount ns yet):
 *   1. CLONE_NEWUSER + map ns-root 0 -> our euid/egid (rootless; we get full
 *      caps INSIDE the userns, none on the host).
 *   2. CLONE_NEWNET|NEWIPC|NEWUTS (kills network/SysV-IPC/hostname reach).
 *   3. PR_SET_NO_NEW_PRIVS + PR_SET_DUMPABLE=0.
 *   4. Drop every capability: bounding set, effective/permitted/inheritable,
 *      and ambient.  After this the stub is fully unprivileged.
 * Returns 0 on success, -1 on any failure (caller fail-closes unless the
 * NVKVM_ISOLATE_NO_HARDEN escape hatch is set).
 */
static void nvkvm_drop_all_caps(void)
{
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
		_exit(126);
	prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
	/* Drop the capability bounding set (so caps can't be regained on exec). */
	for (int c = 0; c <= 63; c++)
		prctl(PR_CAPBSET_DROP, c, 0, 0, 0);  /* EINVAL past last cap: ok */
	/* Zero effective/permitted/inheritable. */
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0 };
	struct __user_cap_data_struct data[2] = { {0,0,0}, {0,0,0} };
	syscall(SYS_capset, &hdr, data);
	/* Clear ambient set. */
	prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
}

/* Parent-side: write one /proc/<pid>/<which> map file for the child's userns. */
static int nvkvm_write_child_map(pid_t pid, const char *which, const char *val)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, which);
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	size_t len = strlen(val);
	ssize_t n = write(fd, val, len);
	close(fd);
	return (n == (ssize_t)len) ? 0 : -1;
}

/*
 * Parent-side: set up the rootless uid/gid mapping for a child that was
 * clone()'d with CLONE_NEWUSER.  ns-root 0 -> our euid/egid (single-line map,
 * permitted even for an unprivileged parent).  Returns 0 / -1.
 *
 * COMBINED namespace+uid mode needs a SECOND line.  The child becomes ns-root
 * (uid 0) so it can mount/pivot_root, and only then drops to its per-isolate
 * uid — but setresuid() to an id that is not mapped into the user namespace
 * fails with EINVAL, so the target uid must appear in the map.  We map it
 * identity (host uid N -> ns uid N) so the host-visible uid really is the
 * separated one; a namespaced-but-unmapped uid would give a process that looks
 * separated from the inside and is uid `nobody` on the host, which is the
 * opposite of what this mode is for.
 *
 * Writing a multi-line map requires CAP_SETUID/CAP_SETGID in the PARENT user
 * namespace.  UID mode already demands both (checked up front in
 * nvkvm_iso_cfg_validate), so this adds no new requirement.  `setgroups: deny`
 * is still written — it is always permitted and is strictly stronger than the
 * setgroups(0, NULL) the child would otherwise do.
 */
static int nvkvm_map_child_userns(pid_t pid, uid_t extra_uid, gid_t extra_gid)
{
	char map[128];
	if (nvkvm_write_child_map(pid, "setgroups", "deny") < 0)
		return -1;
	if (extra_uid)
		snprintf(map, sizeof(map), "0 %u 1\n%u %u 1\n",
			 (unsigned)geteuid(), (unsigned)extra_uid,
			 (unsigned)extra_uid);
	else
		snprintf(map, sizeof(map), "0 %u 1\n", (unsigned)geteuid());
	if (nvkvm_write_child_map(pid, "uid_map", map) < 0)
		return -1;
	if (extra_gid)
		snprintf(map, sizeof(map), "0 %u 1\n%u %u 1\n",
			 (unsigned)getegid(), (unsigned)extra_gid,
			 (unsigned)extra_gid);
	else
		snprintf(map, sizeof(map), "0 %u 1\n", (unsigned)getegid());
	if (nvkvm_write_child_map(pid, "gid_map", map) < 0)
		return -1;
	return 0;
}

/*
 * Spawn the isolate child.  In namespace mode, clone() it directly into fresh
 * user + pid + net + ipc + uts + mount namespaces (CLONE_NEWUSER lets an
 * unprivileged parent create the rest; the child is PID 1 of the new pid ns and
 * clone() returns its real host pid — no intermediate process, no second fork).
 * A NULL child stack with no CLONE_VM makes the raw clone behave like fork.
 *
 * In UID-separation mode (NVKVM_ISO_LAYER_UID without NVKVM_ISO_LAYER_NS) there
 * are no namespaces to create — that is the entire point, since this mode
 * exists for hosts where CLONE_NEWUSER is unavailable — so a plain fork() is
 * used and the boundary is established later by nvkvm_iso_drop_privilege().
 *
 * Returns child pid (>0) / 0 in child / -1 on error, like fork().
 */
static pid_t nvkvm_isolate_spawn(unsigned mode)
{
	if (!(mode & NVKVM_ISO_LAYER_NS))
		return fork();
	unsigned long flags = CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET |
			      CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWNS |
			      (unsigned long)SIGCHLD;
	return (pid_t)syscall(SYS_clone, flags, (void *)0, (void *)0,
			      (void *)0, 0UL);
}

/*
 * Child-side: build a MINIMAL root containing only the nvidia device nodes the
 * stub opens, pivot into it, and hand the stub a /dev dirfd that cannot escape.
 * Runs in the clone()'d child (PID 1, ns-root with CAP_SYS_ADMIN in its userns),
 * before caps are dropped.  Returns 0 / -1.
 *
 * SECURITY: an earlier version parked an O_PATH handle to the *whole host /dev*
 * at NVKVM_DEV_DIRFD and pivoted into an empty tmpfs.  That handle was an escape
 * hatch — a compromised stub could openat(dd, "../../etc/shadow", O_RDONLY) and
 * read any host file, because the dirfd's ".." resolved to the (still-referenced)
 * host root above /dev.  Fix (the runc device-bind idiom): construct a tmpfs root
 * holding ONLY /dev/nvidia*, pivot into it, then open the dirfd AFTER the pivot so
 * its ".." is the sandbox root, which contains nothing but those nodes.
 *
 * We build the root on a tmpfs mounted over /proc (guaranteed to exist; also masks
 * the host /proc).  Bind-mounting EXISTING device nodes is permitted in our userns
 * (mknod is not — hence touch-then-bind), and we deliberately omit MS_NODEV on the
 * binds so the nodes stay openable.  /dev/nvidia-uvm is intentionally absent: UVM
 * is opened by QEMU, never by the sandboxed stub.
 */
static int nvkvm_child_enter_mount_ns(void)
{
	static const char *const nodes[] = {
		"nvidiactl", "nvidia0", "nvidia1", "nvidia2", "nvidia3",
		"nvidia4", "nvidia5", "nvidia6", "nvidia7",
		"nvidia-modeset",   /* NVKMS — graphics (Vulkan/EGL); non-fatal */
	};
	char src[64], dst[80];
	int dd;

	/* Don't let our mount changes propagate back to the host. */
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		return -1;
	/* Scratch tmpfs that becomes the sandbox root; mode 0755 so we can
	 * populate it and the stub can traverse to /dev. */
	if (mount("tmpfs", "/proc", "tmpfs",
		  MS_NOSUID | MS_NOEXEC, "size=256k,mode=0755") < 0)
		return -1;
	if (mkdir("/proc/dev", 0755) < 0)
		return -1;
	for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		int fd;
		snprintf(src, sizeof(src), "/dev/%s", nodes[i]);
		if (access(src, F_OK) != 0)
			continue;                 /* node absent (e.g. nvidiaN) */
		snprintf(dst, sizeof(dst), "/proc/dev/%s", nodes[i]);
		fd = open(dst, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
		if (fd >= 0)
			close(fd);                /* empty bind target */
		if (mount(src, dst, NULL, MS_BIND, NULL) < 0 && i == 0)
			return -1;                /* nvidiactl is mandatory */
	}
	/* nvidia-drm render node(s) for graphics (Vulkan) live under dri/.
	 * Create the subdir in the sandbox /dev and bind each present
	 * renderD12{8..} node.  Graphics-only — absence is non-fatal. */
	mkdir("/proc/dev/dri", 0755);
	for (int n = 128; n < 128 + 8; n++) {
		int fd;
		snprintf(src, sizeof(src), "/dev/dri/renderD%d", n);
		if (access(src, F_OK) != 0)
			continue;
		snprintf(dst, sizeof(dst), "/proc/dev/dri/renderD%d", n);
		fd = open(dst, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
		if (fd >= 0)
			close(fd);
		mount(src, dst, NULL, MS_BIND, NULL);
	}
	/* pivot_root into the minimal tmpfs; detach the old (host) root. */
	if (chdir("/proc") < 0)
		return -1;
	if (syscall(SYS_pivot_root, ".", ".") < 0)
		return -1;
	umount2(".", MNT_DETACH);
	if (chdir("/") < 0)
		return -1;
	/* Restricted /dev dirfd, opened AFTER the pivot: its ".." is the sandbox
	 * root (holds only /dev/nvidia*), so openat(dd,"..") cannot reach the host. */
	dd = open("/dev", O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (dd < 0)
		return -1;
	if (dd != NVKVM_DEV_DIRFD) {
		if (dup2(dd, NVKVM_DEV_DIRFD) < 0) {
			close(dd);
			return -1;
		}
		close(dd);
	}
	/* Seal the root read-only now that the nvidia binds are in place (they are
	 * separate mounts, unaffected, and stay openable).  Audit R4-L1: this used
	 * to ignore the return — a silent partial fail-open (the stub would run
	 * with a writable root tmpfs if the remount failed).  Fail closed: the
	 * caller _exit(126)s the child, so a weakened sandbox never runs. */
	if (mount(NULL, "/", NULL,
		  MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NOEXEC, NULL) < 0)
		return -1;
	return 0;
}

/* memfd_create may not be in older glibc headers; use syscall directly. */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
static inline int nvkvm_memfd_create(const char *name, unsigned int flags)
{
	return (int)syscall(SYS_memfd_create, name, (unsigned long)flags);
}

#include "nvkvm_isolate.h"
#include "virtio_nvgpu.h"

#include "../../src/common/nvkvm_isolate_proto.h"
#include "../../src/common/nvkvm_ring.h"

#ifdef NVKVM_STUB_EMBEDDED
#include "nvkvm_stub_bin.h"
static const unsigned char *stub_elf     = nvkvm_stub;
/* xxd -i emits `unsigned int nvkvm_stub_len = N;` — a mutable object, not a
 * constant expression, so it cannot initialise a static. sizeof on the array
 * is a constant expression and is independent of the xxd version. */
static unsigned int         stub_elf_len = sizeof(nvkvm_stub);
#else
static const unsigned char *stub_elf     = NULL;
static unsigned int         stub_elf_len = 0;
#endif

/*
 * Close every inherited fd from STDERR+1 up to RLIMIT_NOFILE in the just-
 * forked child, before fexecve/execl.  Without this the stub inherits
 * QEMU's KVM vm fd, the memory-backend fds, every other isolate's socket-
 * pair, and so on — turning any stub RCE into "set arbitrary host memory
 * region visible to the guest" via KVM_SET_USER_MEMORY_REGION.  Audit M6.
 *
 * We prefer the close_range(2) syscall (Linux 5.9+, ~always present on
 * vast.ai kernels) because it's O(1) at the kernel level and atomically
 * closes a range without needing to readdir /proc/self/fd.  Falls back
 * to the classic dirent loop if close_range isn't available.
 */
#ifndef CLOSE_RANGE_UNSHARE
#define CLOSE_RANGE_UNSHARE  (1U << 1)
#endif

static void nvkvm_isolate_closefrom(int first)
{
	long r = syscall(__NR_close_range, first, ~0U, 0);
	if (r == 0)
		return;
	/* Fallback: iterate /proc/self/fd.  We can't use opendir here
	 * (allocates), so dump a getdents64 buffer.  Best-effort. */
	int dfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		return;
	char buf[4096];
	while (1) {
		long n = syscall(__NR_getdents64, dfd, buf, sizeof(buf));
		if (n <= 0)
			break;
		for (long off = 0; off < n; ) {
			struct linux_dirent64 {
				unsigned long  d_ino;
				long           d_off;
				unsigned short d_reclen;
				unsigned char  d_type;
				char           d_name[];
			} *de = (void *)(buf + off);
			off += de->d_reclen;
			if (de->d_name[0] == '.')
				continue;
			int fd = atoi(de->d_name);
			if (fd >= first && fd != dfd)
				close(fd);
		}
	}
	close(dfd);
}

/* ── In-flight IOCTL request (lives on the caller's stack) ──────────────── */

struct nvkvm_pending_ioctl {
	uint32_t        txn_id;
	bool            done;       /* set by reader thread */
	pthread_cond_t  cond;       /* signaled by reader, waited under iso->lock */

	/* Caller's output buffers — written by reader before done=true */
	void           *param_buf;
	size_t          param_cap;
	void           *aux_buf;
	size_t          aux_cap;

	/* Response fields (written by reader before done=true) */
	int             error;      /* transport error (-errno), 0 on success */
	int32_t         retval;
	uint32_t        nvstatus;
	uint64_t        fault_addr;

	struct nvkvm_pending_ioctl *next; /* intrusive list, protected by iso->lock */
};

/* ── Socket I/O helpers ─────────────────────────────────────────────────── */

static ssize_t sock_send_full(int fd, const void *buf, size_t len)
{
	size_t done = 0;
	while (done < len) {
		ssize_t n = send(fd, (const char *)buf + done, len - done,
				 MSG_NOSIGNAL);
		if (n <= 0)
			return n < 0 ? -errno : -ECONNRESET;
		done += (size_t)n;
	}
	return (ssize_t)done;
}

/* Send fd via SCM_RIGHTS together with a RECEIVE_FD header. */
static ssize_t sock_sendmsg_fd(int sock, struct msghdr *msg)
{
	ssize_t n = sendmsg(sock, msg, MSG_NOSIGNAL);
	return (n < 0) ? -errno : n;
}

/* ── Reader thread ──────────────────────────────────────────────────────── */

/*
 * Maximum payload size for IOCTL param/aux blobs.  If the isolate sends
 * a blob larger than this, it is truncated/discarded (protocol violation).
 */
#define MAX_IOCTL_PAYLOAD  (64 * 1024)

/* Drain one SEQPACKET message; for SEQPACKET a single recv consumes one msg. */
static void drain_message(int fd)
{
	char buf[MAX_IOCTL_PAYLOAD];
	recv(fd, buf, sizeof(buf), 0);
}

static void reader_signal_sync(struct nvkvm_isolate *iso, int err,
				int mmap_retval)
{
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_error       = err;
	iso->sync_mmap_retval = mmap_retval;
	iso->sync_done        = true;
	/* F-5: broadcast (not signal) so a stale ENTER_LOOP waiter on a reused
	 * slot and a fresh waiter both re-evaluate their identity predicate. */
	pthread_cond_broadcast(&iso->sync_cond);
	pthread_mutex_unlock(&iso->sync_lock);
}

/* Variant for OPEN_DEVICE: also carries the SCM_RIGHTS fd. */
static void reader_signal_sync_open(struct nvkvm_isolate *iso, int err, int fd)
{
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_error    = err;
	iso->sync_open_fd  = fd;
	iso->sync_done     = true;
	pthread_cond_signal(&iso->sync_cond);
	pthread_mutex_unlock(&iso->sync_lock);
}

/* PRESENT_EXPORT (#106): dedicated slot, carries the dma-buf SCM_RIGHTS fd. */
static void reader_signal_present(struct nvkvm_isolate *iso, int err, int fd)
{
	pthread_mutex_lock(&iso->present_sync_lock);
	iso->present_err  = err;
	iso->present_fd   = fd;
	iso->present_done = true;
	pthread_cond_signal(&iso->present_cond);
	pthread_mutex_unlock(&iso->present_sync_lock);
}

/*
 * XISO_IMPORT (#110): dedicated slot, mirrors the present one.  Factored out of
 * the response arm so the reader-exit path can wake a stranded waiter too — a
 * stub that dies mid-broker otherwise leaves nvkvm_isolate_xiso_import blocked
 * on xiso_cond forever while it still holds xiso_lock, and because the broker
 * runs inline on the virtio TX thread that wedges the whole guest's GPU I/O,
 * not just this isolate.  Same failure the sync/present slots already guard.
 */
static void reader_signal_xiso(struct nvkvm_isolate *iso, int err, uint32_t gem)
{
	pthread_mutex_lock(&iso->xiso_sync_lock);
	iso->xiso_err  = err;
	iso->xiso_gem  = gem;
	iso->xiso_done = true;
	pthread_cond_signal(&iso->xiso_cond);
	pthread_mutex_unlock(&iso->xiso_sync_lock);
}

static void *isolate_reader_fn(void *arg)
{
	struct nvkvm_isolate *iso = arg;

	union {
		uint32_t                            type;
		struct isolate_resp_ok              ok;
		struct isolate_resp_error           err;
		struct isolate_resp_ioctl           ioctl;
		struct isolate_resp_mmap            mmap;
		struct isolate_resp_poll_event      poll_event;
		struct isolate_resp_open_device     open_dev;
		struct isolate_resp_realize_uvm     realize;
		struct isolate_resp_ring_ready      ring_ready;
		struct isolate_resp_loop_exited     loop_exited;
		struct isolate_resp_present_export  present_export;
		struct isolate_resp_xiso_import     xiso_import;
	} u;

	for (;;) {
		/*
		 * For SOCK_SEQPACKET one recvmsg() reads exactly one message.
		 * A buffer larger than the message is fine; excess bytes are
		 * discarded. Use recvmsg + cmsg buffer so the OPEN_DEVICE
		 * response can deliver its SCM_RIGHTS fd in the same call —
		 * other response types simply ignore the cmsg slot.
		 */
		char cmsg_buf[CMSG_SPACE(sizeof(int))];
		struct iovec iov = { .iov_base = &u, .iov_len = sizeof(u) };
		struct msghdr msg = {
			.msg_iov        = &iov,
			.msg_iovlen     = 1,
			.msg_control    = cmsg_buf,
			.msg_controllen = sizeof(cmsg_buf),
		};
		ssize_t n = recvmsg(iso->sock_fd, &msg, 0);
		if (n <= 0)
			break;

		/*
		 * R2-M1: only ISOLATE_RESP_OPEN_DEVICE legitimately carries an
		 * SCM_RIGHTS fd.  A compromised stub could attach a fd to ANY
		 * other response type; if we don't consume it, it leaks into
		 * QEMU's fd table (eventual fd-exhaustion DoS of the VMM).  Close
		 * any received fd on every non-OPEN_DEVICE response.  (cmsg_buf is
		 * one-fd-sized, so the kernel already closed any truncated extras.)
		 */
		if (u.type != ISOLATE_RESP_OPEN_DEVICE &&
		    u.type != ISOLATE_RESP_PRESENT_EXPORT) {
			for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm;
			     cm = CMSG_NXTHDR(&msg, cm)) {
				if (cm->cmsg_level == SOL_SOCKET &&
				    cm->cmsg_type == SCM_RIGHTS) {
					int nfd = (int)((cm->cmsg_len - CMSG_LEN(0)) /
							sizeof(int));
					int *fds = (int *)CMSG_DATA(cm);
					for (int i = 0; i < nfd; i++)
						if (fds[i] >= 0)
							close(fds[i]);
				}
			}
		}

		switch (u.type) {
		case ISOLATE_RESP_OK:
			reader_signal_sync(iso, 0, 0);
			break;

		case ISOLATE_RESP_ERROR:
			reader_signal_sync(iso, -(int)u.err.err, 0);
			break;

		case ISOLATE_RESP_MMAP:
			reader_signal_sync(iso, 0, u.mmap.retval);
			break;

		case ISOLATE_RESP_IOCTL: {
			uint32_t txn_id     = u.ioctl.txn_id;
			int32_t  retval     = u.ioctl.retval;
			uint32_t nvstatus   = u.ioctl.nvstatus;
			uint64_t fault_addr = u.ioctl.fault_addr;
			uint32_t param_size = u.ioctl.param_size;
			uint32_t aux_size   = u.ioctl.aux_size;

			/*
			 * Locate the pending caller AND remove it from the list
			 * under the lock (audit R2-H2).  A compromised stub could
			 * echo the same txn_id twice; if we left the entry on the
			 * list, the 2nd response could re-find `p` and recv() into
			 * p->param_buf after the 1st response woke the caller, which
			 * then removes+destroys its stack-allocated `pending` and
			 * returns — a use-after-free write inside QEMU.  Claiming
			 * (removing) the entry here makes a duplicate txn_id find
			 * nothing (→ drained).  The single live response is safe:
			 * the caller cannot wake until we set p->done below, which
			 * happens only after the recv into p's buffers.
			 */
			pthread_mutex_lock(&iso->lock);
			struct nvkvm_pending_ioctl **pp = &iso->pending_head;
			while (*pp && (*pp)->txn_id != txn_id)
				pp = &(*pp)->next;
			struct nvkvm_pending_ioctl *p = *pp;
			if (p)
				*pp = p->next;   /* claim: off the list */
			pthread_mutex_unlock(&iso->lock);

			/*
			 * Read param blob.  We're the only reader on this
			 * socket so we can do this without the lock.
			 */
			if (param_size > 0) {
				if (p && p->param_buf &&
				    param_size <= (uint32_t)p->param_cap) {
					n = recv(iso->sock_fd, p->param_buf,
						 p->param_cap, 0);
					if (n <= 0)
						goto reader_exit;
				} else {
					drain_message(iso->sock_fd);
					if (p)
						p->param_buf = NULL;
				}
			}

			/* Read aux blob. */
			if (aux_size > 0) {
				if (p && p->aux_buf &&
				    aux_size <= (uint32_t)p->aux_cap) {
					n = recv(iso->sock_fd, p->aux_buf,
						 p->aux_cap, 0);
					if (n <= 0)
						goto reader_exit;
				} else {
					drain_message(iso->sock_fd);
					if (p)
						p->aux_buf = NULL;
				}
			}

			if (p) {
				pthread_mutex_lock(&iso->lock);
				p->retval     = retval;
				p->nvstatus   = nvstatus;
				p->fault_addr = fault_addr;
				p->error      = 0;
				p->done       = true;
				pthread_cond_signal(&p->cond);
				pthread_mutex_unlock(&iso->lock);
			}
			break;
		}

		case ISOLATE_RESP_POLL_EVENT:
			/* #127: a registered host os-event fd became ready in the
			 * stub. Wake the matching guest fd via vq_evt (push is
			 * marshalled onto the device AioContext internally). */
			if (iso->nv)
				nvkvm_virtio_push_evt((VirtIONvgpu *)iso->nv, iso->id,
						      u.poll_event.handle_id,
						      u.poll_event.revents);
			break;

		case ISOLATE_RESP_REALIZE_UVM: {
			pthread_mutex_lock(&iso->sync_lock);
			iso->sync_realize_host_va   = u.realize.host_va;
			iso->sync_realize_length    = u.realize.length;
			iso->sync_realize_token     = u.realize.realize_token;
			iso->sync_realize_rm_status = u.realize.rm_status;
			iso->sync_error             = u.realize.retval;
			iso->sync_done              = true;
			pthread_cond_signal(&iso->sync_cond);
			pthread_mutex_unlock(&iso->sync_lock);
			break;
		}

		case ISOLATE_RESP_RING_READY: {
			pthread_mutex_lock(&iso->sync_lock);
			iso->sync_ring_probe = u.ring_ready.probe_seen;
			iso->sync_error      = u.ring_ready.error;
			iso->sync_done       = true;
			pthread_cond_signal(&iso->sync_cond);
			pthread_mutex_unlock(&iso->sync_lock);
			break;
		}

		case ISOLATE_RESP_LOOP_EXITED: {
			pthread_mutex_lock(&iso->sync_lock);
			iso->sync_loop_head = u.loop_exited.head;
			iso->sync_error     = u.loop_exited.error;
			iso->sync_done      = true;
			pthread_cond_signal(&iso->sync_cond);
			pthread_mutex_unlock(&iso->sync_lock);
			break;
		}

		case ISOLATE_RESP_OPEN_DEVICE: {
			int got_fd = -1;
			for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
			     cm; cm = CMSG_NXTHDR(&msg, cm)) {
				if (cm->cmsg_level == SOL_SOCKET &&
				    cm->cmsg_type  == SCM_RIGHTS &&
				    cm->cmsg_len   == CMSG_LEN(sizeof(int))) {
					memcpy(&got_fd, CMSG_DATA(cm), sizeof(int));
				}
			}
			int err = u.open_dev.retval;
			if (err && got_fd >= 0) {
				/* Stub claimed failure but still sent a fd — be
				 * defensive: close the orphan so we don't leak. */
				close(got_fd);
				got_fd = -1;
			}
			reader_signal_sync_open(iso, err, got_fd);
			break;
		}

		case ISOLATE_RESP_PRESENT_EXPORT: {
			int got_fd = -1;
			for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
			     cm; cm = CMSG_NXTHDR(&msg, cm)) {
				if (cm->cmsg_level == SOL_SOCKET &&
				    cm->cmsg_type  == SCM_RIGHTS &&
				    cm->cmsg_len   == CMSG_LEN(sizeof(int))) {
					memcpy(&got_fd, CMSG_DATA(cm), sizeof(int));
				}
			}
			int err = u.present_export.retval;
			if (err && got_fd >= 0) {
				close(got_fd);
				got_fd = -1;
			}
			reader_signal_present(iso, err, got_fd);
			break;
		}

		case ISOLATE_RESP_XISO_IMPORT: {
			reader_signal_xiso(iso, u.xiso_import.retval,
					   u.xiso_import.gem_handle);
			break;
		}

		default:
			NVKVM_DBG(
				"nvkvm_isolate: unknown response type 0x%x\n",
				u.type);
			break;
		}
	}

reader_exit:
	/* Wake every pending IOCTL caller with a transport error. */
	pthread_mutex_lock(&iso->lock);
	iso->alive = false;
	for (struct nvkvm_pending_ioctl *p = iso->pending_head; p; p = p->next) {
		p->error = -ECONNRESET;
		p->done  = true;
		pthread_cond_signal(&p->cond);
	}
	iso->pending_head = NULL;
	pthread_mutex_unlock(&iso->lock);

	/* Wake any pending sync command too. */
	reader_signal_sync(iso, -ECONNRESET, 0);
	/* …and any pending present-export waiter (dedicated slot, #106). */
	reader_signal_present(iso, -ECONNRESET, -1);
	/* …and any pending cross-isolate import waiter (dedicated slot, #110). */
	reader_signal_xiso(iso, -ECONNRESET, 0);

	return NULL;
}

/* ── Table management ───────────────────────────────────────────────────── */

/* ── Isolation-mode configuration ───────────────────────────────────────
 *
 * Env-var driven, matching the existing isolate knobs (NVKVM_ISOLATE_NO_HARDEN,
 * NVKVM_RING_DISABLE, NVKVM_STUB_PATH, ...):
 *
 *   NVKVM_ISOLATE_MODE       auto (default) | namespace | uid | uid+chroot |
 *                            namespace+uid | seccomp | none
 *   NVKVM_ISOLATE_UID_BASE   first uid/gid of this VM's window (default 500000)
 *   NVKVM_ISOLATE_UNSAFE_ACK required, with the exact acknowledgement string,
 *                            before mode 'none' is accepted
 *   NVKVM_ISOLATE_NO_HARDEN  legacy hatch — maps to the 'seccomp' rung, which
 *                            is exactly what it does today (it turns off the
 *                            namespaces; it has never turned off the stub's
 *                            seccomp filter).  Kept back-compatible on purpose:
 *                            silently making an existing hatch WEAKER than it
 *                            was would be its own security bug.
 *
 * Resolution is strict for explicitly named modes.  `auto` is the exception,
 * and a deliberate one: it probes the ladder by ATTEMPTING each rung, which is
 * the only reliable detection (the kernel sysctls report user namespaces as
 * available inside a stock Docker container that blocks them via seccomp and
 * AppArmor).  auto never selects 'none'.
 */
static void nvkvm_isolate_cfg_resolve(struct nvkvm_isolate_cfg *cfg,
				      char *err, size_t errsz,
				      char *report, size_t reportsz)
{
	const char *mode_s = getenv("NVKVM_ISOLATE_MODE");
	const char *base_s = getenv("NVKVM_ISOLATE_UID_BASE");
	const char *ack_s  = getenv(NVKVM_ISO_UNSAFE_ACK_ENV);

	err[0] = '\0';
	report[0] = '\0';
	cfg->mode     = NVKVM_ISO_MODE_AUTO;          /* default: probe */
	cfg->uid_base = NVKVM_ISO_UID_BASE_DEFAULT;

	if (mode_s && *mode_s) {
		if (nvkvm_iso_mode_parse(mode_s, &cfg->mode, err, errsz) != 0)
			return;
	} else if (getenv("NVKVM_ISOLATE_NO_HARDEN") != NULL) {
		cfg->mode = NVKVM_ISO_LAYER_SECCOMP;
		snprintf(report, reportsz,
			 "isolate mode: NVKVM_ISOLATE_NO_HARDEN=1 selects the "
			 "'seccomp' rung (namespaces off, stub seccomp filter "
			 "still applied — unchanged from previous releases). "
			 "Prefer NVKVM_ISOLATE_MODE=seccomp.");
	}

	if (base_s && *base_s) {
		char *end = NULL;
		unsigned long v = strtoul(base_s, &end, 0);
		if (!end || *end || v == 0 || v > 0xFFFFFFFFUL) {
			snprintf(err, errsz,
				 "NVKVM_ISOLATE_UID_BASE='%s' is not a number",
				 base_s);
			return;
		}
		cfg->uid_base = (uint32_t)v;
	}

	/*
	 * 'none' removes every layer, so it is the one setting where a typo is
	 * catastrophic.  Require the acknowledgement rather than accepting a
	 * bare mode=none.
	 */
	if (cfg->mode != NVKVM_ISO_MODE_AUTO &&
	    nvkvm_iso_needs_unsafe_ack(cfg->mode) &&
	    !(ack_s && !strcmp(ack_s, NVKVM_ISO_UNSAFE_ACK_VALUE))) {
		snprintf(err, errsz,
			 "isolation mode 'none' removes every boundary (no "
			 "namespaces, no uid separation, no seccomp filter) and "
			 "must be acknowledged explicitly. Set %s=%s alongside "
			 "it, or use 'seccomp' — the lowest rung that still "
			 "confines anything.",
			 NVKVM_ISO_UNSAFE_ACK_ENV, NVKVM_ISO_UNSAFE_ACK_VALUE);
		return;
	}

	if (cfg->mode == NVKVM_ISO_MODE_AUTO) {
		if (nvkvm_iso_auto_select(&cfg->mode, report, reportsz,
					  err, errsz) != 0)
			cfg->mode = NVKVM_ISO_MODE_AUTO;   /* stays invalid */
	}
}

/*
 * True when the resolved configuration is weaker than namespace mode and the
 * operator should be told loudly, at every start.
 */
bool nvkvm_isolate_cfg_is_degraded(const struct nvkvm_isolate_table *t)
{
	return (t->cfg.mode & NVKVM_ISO_LAYER_NS) == 0;
}

const char *nvkvm_isolate_cfg_report(const struct nvkvm_isolate_table *t)
{
	return t->cfg_report;
}

int nvkvm_isolate_cfg_check(const struct nvkvm_isolate_table *t,
			    char *err, size_t errsz)
{
	if (t->cfg_error[0]) {
		snprintf(err, errsz, "%s", t->cfg_error);
		return -1;
	}
	return nvkvm_iso_cfg_validate(&t->cfg, err, errsz);
}

const char *nvkvm_isolate_cfg_describe(const struct nvkvm_isolate_table *t,
				       char *buf, size_t bufsz)
{
	if (t->cfg.mode & NVKVM_ISO_LAYER_UID)
		snprintf(buf, bufsz,
			 "isolate sandbox: %s (uid window %u..%u, %u slots)",
			 nvkvm_iso_mode_str(t->cfg.mode), t->cfg.uid_base,
			 t->cfg.uid_base + NVKVM_ISO_UID_SLOTS - 1,
			 NVKVM_ISO_UID_SLOTS);
	else
		snprintf(buf, bufsz, "isolate sandbox: %s",
			 nvkvm_iso_mode_str(t->cfg.mode));
	return buf;
}

void nvkvm_isolate_table_init(struct nvkvm_isolate_table *t)
{
	memset(t, 0, sizeof(*t));
	pthread_mutex_init(&t->lock, NULL);
	nvkvm_isolate_cfg_resolve(&t->cfg, t->cfg_error, sizeof(t->cfg_error),
				  t->cfg_report, sizeof(t->cfg_report));
	t->next_id = 1;
	for (int i = 0; i < NVKVM_ISOLATE_MAX; i++) {
		struct nvkvm_isolate *iso = &t->isolates[i];
		iso->sock_fd = -1;
		pthread_mutex_init(&iso->lock,       NULL);
		pthread_mutex_init(&iso->write_lock, NULL);
		pthread_mutex_init(&iso->sync_lock,  NULL);
		pthread_cond_init(&iso->sync_cond,   NULL);
		pthread_mutex_init(&iso->present_lock,      NULL);
		pthread_mutex_init(&iso->present_sync_lock, NULL);
		pthread_cond_init(&iso->present_cond,       NULL);
		pthread_mutex_init(&iso->xiso_lock,         NULL);
		pthread_mutex_init(&iso->xiso_sync_lock,    NULL);
		pthread_cond_init(&iso->xiso_cond,          NULL);
		iso->present_fd = -1;
	}
}

void nvkvm_isolate_table_fini(struct nvkvm_isolate_table *t)
{
	for (int i = 1; i < NVKVM_ISOLATE_MAX; i++) {
		struct nvkvm_isolate *iso = &t->isolates[i];
		if (iso->in_use)
			nvkvm_isolate_kill(t, iso->id);
		pthread_mutex_destroy(&iso->lock);
		pthread_mutex_destroy(&iso->write_lock);
		pthread_mutex_destroy(&iso->sync_lock);
		pthread_cond_destroy(&iso->sync_cond);
		pthread_mutex_destroy(&iso->present_lock);
		pthread_mutex_destroy(&iso->present_sync_lock);
		pthread_cond_destroy(&iso->present_cond);
		pthread_mutex_destroy(&iso->xiso_lock);
		pthread_mutex_destroy(&iso->xiso_sync_lock);
		pthread_cond_destroy(&iso->xiso_cond);
	}
	pthread_mutex_destroy(&t->lock);
}

static struct nvkvm_isolate *alloc_isolate_slot(struct nvkvm_isolate_table *t,
						uint32_t *id_out)
{
	for (int attempt = 0; attempt < NVKVM_ISOLATE_MAX; attempt++) {
		uint32_t id = t->next_id++;
		if (t->next_id >= NVKVM_ISOLATE_MAX)
			t->next_id = 1;
		if (id == 0)
			continue;
		struct nvkvm_isolate *iso = &t->isolates[id % NVKVM_ISOLATE_MAX];
		if (!iso->in_use) {
			iso->id           = id;
			iso->in_use       = true;
			iso->alive        = false;
			iso->sock_fd      = -1;
			iso->pending_head = NULL;
			iso->next_txn_id  = 1;
			iso->nv           = t->nv;  /* #127: owning device for vq_evt push */
			/* F-5 (security_audit_2026_06_01): do NOT reset sync_done here.
			 * Every sync op resets it under sync_lock before its own wait;
			 * resetting it here under iso->lock is a cross-lock data race that
			 * can re-park a stale ENTER_LOOP waiter from a just-killed slot. */
			iso->sync_open_fd = -1;
			iso->reader_started = false;
			iso->run_uid      = 0;
			iso->run_gid      = 0;
			iso->ring_memfd   = -1;
			iso->ring_qva     = NULL;
			iso->ring_region_size = 0;
			iso->ring_bytes   = 0;
			iso->ring_gpa     = 0;
			iso->ring_kvm_slot = -1;
			iso->ring_ready   = false;
			*id_out = id;
			return iso;
		}
	}
	return NULL;
}

/* ── Spawn isolate ──────────────────────────────────────────────────────── */

int nvkvm_isolate_create(struct nvkvm_isolate_table *t,
			 uint32_t session_id,
			 void *nv,
			 uint32_t *isolate_id_out)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0)
		return -errno;

	/* Remember the owning device so ring setup/teardown can use the sparse
	 * GPA window allocator (idempotent — same nv every call). */
	if (nv)
		t->nv = nv;

	pthread_mutex_lock(&t->lock);
	uint32_t id;
	struct nvkvm_isolate *iso = alloc_isolate_slot(t, &id);
	if (!iso) {
		pthread_mutex_unlock(&t->lock);
		close(sv[0]);
		close(sv[1]);
		return -EMFILE;
	}
	iso->session_id = session_id;
	pthread_mutex_unlock(&t->lock);

	pid_t pid;
	/*
	 * Phase 0 lockdown.  In namespace mode we clone() the child directly
	 * into fresh user/pid/net/ipc/uts/mount namespaces (it is PID 1 of the new
	 * pid ns; clone() returns its real host pid).  The child blocks on syncpipe
	 * until we write its uid/gid maps from the parent, then drops all caps and
	 * execs.  No intermediate process.
	 */
	const unsigned mode = t->cfg.mode;
	const unsigned layers = mode & NVKVM_ISO_LAYERS_ALL;
	const bool use_ns     = (layers & NVKVM_ISO_LAYER_NS)      != 0;
	const bool use_uid    = (layers & NVKVM_ISO_LAYER_UID)     != 0;
	const bool use_chroot = (layers & NVKVM_ISO_LAYER_CHROOT)  != 0;
	const bool use_seccomp = (layers & NVKVM_ISO_LAYER_SECCOMP) != 0;
	/* Capability/no_new_privs drop belongs to every rung that confines at
	 * all — including the bare 'seccomp' rung, whose TSYNC filter REQUIRES
	 * no_new_privs on every thread. */
	const bool harden     = (layers != 0);

	/*
	 * UID-separation mode: derive this isolate's unique host uid/gid from its
	 * slot index.  The slot is held exclusively from alloc_isolate_slot()
	 * until nvkvm_isolate_kill() clears in_use — which happens only AFTER
	 * waitpid() has reaped the stub — so the uid cannot be in use by another
	 * live isolate of this VM, and is not re-issued while its previous holder
	 * still exists.  The scan below turns that argument into an enforced
	 * check rather than an invariant maintained by inspection.
	 */
	uid_t run_uid = 0;
	if (use_uid) {
		uint32_t slot = id % NVKVM_ISOLATE_MAX;
		if (nvkvm_iso_uid_for_slot(t->cfg.uid_base, slot, &run_uid) != 0) {
			fprintf(stderr,
				"nvkvm: isolate slot %u has no uid in the "
				"configured window (base=%u); refusing to spawn "
				"an isolate without uid separation\n",
				slot, t->cfg.uid_base);
			close(sv[0]); close(sv[1]); iso->in_use = false;
			return -ERANGE;
		}
		pthread_mutex_lock(&t->lock);
		for (int i = 0; i < NVKVM_ISOLATE_MAX; i++) {
			struct nvkvm_isolate *o = &t->isolates[i];
			if (o != iso && o->in_use && o->run_uid == run_uid) {
				pthread_mutex_unlock(&t->lock);
				fprintf(stderr,
					"nvkvm: uid %u already held by live "
					"isolate %u — refusing to reuse it\n",
					(unsigned)run_uid, o->id);
				close(sv[0]); close(sv[1]); iso->in_use = false;
				return -EADDRINUSE;
			}
		}
		iso->run_uid = run_uid;
		iso->run_gid = (gid_t)run_uid;
		pthread_mutex_unlock(&t->lock);
	}

	int syncpipe[2] = { -1, -1 };
	if (use_ns && pipe2(syncpipe, O_CLOEXEC) < 0) {
		int e = errno;
		close(sv[0]); close(sv[1]); iso->in_use = false;
		return -e;
	}

	if (stub_elf && stub_elf_len > 0) {
		int mfd = nvkvm_memfd_create("nvkvm_stub", MFD_CLOEXEC);
		if (mfd < 0) {
			close(sv[0]);
			close(sv[1]);
			iso->in_use = false;
			return -errno;
		}
		if (write(mfd, stub_elf, stub_elf_len) != (ssize_t)stub_elf_len) {
			close(mfd);
			close(sv[0]);
			close(sv[1]);
			iso->in_use = false;
			return -EIO;
		}
		lseek(mfd, 0, SEEK_SET);

		pid = nvkvm_isolate_spawn(mode);
		if (pid == 0) {
			if (use_ns) {
				/* Wait for the parent to install our uid/gid maps. */
				char go;
				close(syncpipe[1]);
				if (read(syncpipe[0], &go, 1) != 1)
					_exit(126);
				close(syncpipe[0]);
			}
			dup2(sv[1], STDIN_FILENO);
			/* Park the memfd at fd 3 so closefrom preserves it. */
			if (mfd != 3) {
				dup2(mfd, 3);
				close(mfd);
				mfd = 3;
			}
			/* F-6 (security_audit_2026_06_01): don't let the stub inherit
			 * QEMU's stdout/stderr — a compromised stub could write attacker
			 * bytes into the host terminal / log / supervisor pipe. Redirect
			 * 1,2 to /dev/null (opened here, before the mount-ns pivot, while
			 * /dev/null still resolves). Keep stdio only under NVKVM_STUB_DEBUG=1. */
			{
				const char *dbg = getenv("NVKVM_STUB_DEBUG");
				if (!(dbg && *dbg == '1')) {
					int dn = open("/dev/null", O_RDWR);
					if (dn >= 0) {
						dup2(dn, STDOUT_FILENO);
						dup2(dn, STDERR_FILENO);
						if (dn > 3) close(dn);
					}
				}
			}
			/* Empty RO mount ns (parks /dev O_PATH at NVKVM_DEV_DIRFD). */
			if (use_ns && nvkvm_child_enter_mount_ns() < 0)
				_exit(126);
			/* uid+chroot: root at /dev, dirfd = the root (parks at
			 * NVKVM_DEV_DIRFD too).  Needs CAP_SYS_CHROOT, so it
			 * must precede both drops below. */
			if (use_chroot && nvkvm_iso_enter_chroot(NVKVM_DEV_DIRFD) < 0)
				_exit(124);
			nvkvm_isolate_closefrom((use_ns || use_chroot)
						? NVKVM_DEV_DIRFD + 1 : 4);
			/* UID separation goes here: AFTER the mount-ns work (which
			 * needs CAP_SYS_ADMIN in the userns) and BEFORE the cap
			 * drop (setresuid needs CAP_SETUID, which that drop
			 * removes).  Verified-irreversible or _exit. */
			if (use_uid &&
			    nvkvm_iso_drop_privilege(run_uid, (gid_t)run_uid,
						     use_ns) != 0)
				_exit(125);
			if (harden)
				nvkvm_drop_all_caps();   /* no_new_privs + dumpable + caps */
			/* The stub applies its seccomp allowlist unless told
			 * otherwise.  argv is the mechanism the stub itself
			 * nominates for this ("Re-add via argv if a debug hatch
			 * is ever needed", src/stub/nvkvm_stub.c) — the env is
			 * deliberately cleared, and argv has the useful property
			 * of being visible in ps, so a stub running without
			 * seccomp cannot hide. */
			const char *argv[] = { "nvkvm_stub", NULL, NULL };
			if (!use_seccomp)
				argv[1] = "--no-seccomp";
			const char *envp[] = { NULL };  /* M6: drop QEMU env */
			fexecve(mfd, (char *const *)argv, (char *const *)envp);
			_exit(127);
		}
		close(mfd);
	} else {
		const char *stub_path = getenv("NVKVM_STUB_PATH");
		if (!stub_path)
			stub_path = "/usr/lib/nvkvm/nvkvm_stub";

		/* M6: in NVKVM_STUB_DEBUG=1 mode, keep the parent's environment
		 * so LD_PRELOAD-based instrumentation still works.  Production
		 * runs clear everything. */
		const char *dbg_mode = getenv("NVKVM_STUB_DEBUG");
		bool keep_env = (dbg_mode && *dbg_mode == '1');

		pid = nvkvm_isolate_spawn(mode);
		if (pid == 0) {
			if (use_ns) {
				char go;
				close(syncpipe[1]);
				if (read(syncpipe[0], &go, 1) != 1)
					_exit(126);
				close(syncpipe[0]);
			}
			/* Open the stub binary as an fd BEFORE pivot_root — after we
			 * pivot into the empty tmpfs root, stub_path no longer exists,
			 * so exec must go through this fd (fexecve), not the path. */
			int binfd = open(stub_path, O_RDONLY | O_CLOEXEC);
			if (binfd < 0)
				_exit(127);
			dup2(sv[1], STDIN_FILENO);
			if (binfd != 3) {
				dup2(binfd, 3);
				close(binfd);
				binfd = 3;
			}
			/* F-6: redirect the stub's stdout/stderr to /dev/null so a
			 * compromised stub can't spoof into QEMU's inherited host
			 * terminal/log. keep_env (NVKVM_STUB_DEBUG=1) keeps stdio too. */
			if (!keep_env) {
				int dn = open("/dev/null", O_RDWR);
				if (dn >= 0) {
					dup2(dn, STDOUT_FILENO);
					dup2(dn, STDERR_FILENO);
					if (dn > 3) close(dn);
				}
			}
			if (use_ns && nvkvm_child_enter_mount_ns() < 0)
				_exit(126);
			if (use_chroot && nvkvm_iso_enter_chroot(NVKVM_DEV_DIRFD) < 0)
				_exit(124);
			nvkvm_isolate_closefrom((use_ns || use_chroot)
						? NVKVM_DEV_DIRFD + 1 : 4);
			if (use_uid &&
			    nvkvm_iso_drop_privilege(run_uid, (gid_t)run_uid,
						     use_ns) != 0)
				_exit(125);
			if (harden)
				nvkvm_drop_all_caps();
			const char *argv[] = { "nvkvm_stub", NULL, NULL };
			if (!use_seccomp)
				argv[1] = "--no-seccomp";
			const char *empty_env[] = { NULL };
			fexecve(binfd, (char *const *)argv,
				keep_env ? environ : (char *const *)empty_env);
			_exit(127);
		}
	}

	if (pid < 0) {
		int e = errno;
		if (use_ns) {
			close(syncpipe[0]); close(syncpipe[1]);
			/* The single most likely reason clone(CLONE_NEWUSER) fails
			 * on an otherwise healthy host, and the reason UID mode
			 * exists.  Name it instead of returning a bare -EPERM. */
			if (e == EPERM || e == EINVAL || e == ENOSPC)
				fprintf(stderr,
				  "nvkvm: clone(CLONE_NEWUSER|...) failed: %s. "
				  "User namespaces are unavailable in this "
				  "environment. Do NOT conclude anything from "
				  "kernel.unprivileged_userns_clone or "
				  "user.max_user_namespaces: on a stock Docker "
				  "container both read as permissive (1 and "
				  "55416, measured) while the default seccomp "
				  "profile and AppArmor still refuse the clone. "
				  "NVKVM_ISOLATE_MODE=uid (or uid+chroot) is the "
				  "namespace-free alternative — read "
				  "docs/internal/isolate-model.md first, it is a "
				  "materially weaker boundary.\n", strerror(e));
		}
		close(sv[0]);
		close(sv[1]);
		iso->in_use = false;
		return -e;
	}

	/* clone() returns the stub's real host pid directly — no intermediate. */
	pid_t stub_pid = pid;
	if (use_ns) {
		close(syncpipe[0]);
		/* Combined mode also maps the per-isolate uid identity-wise, so the
		 * child can setresuid() to it once the mount ns is built. */
		int rc = nvkvm_map_child_userns(pid, use_uid ? run_uid : 0,
						use_uid ? (gid_t)run_uid : 0);
		/* Signal the child to proceed (or, on failure, let its read see EOF
		 * → _exit(126) → we fail closed below). */
		if (rc == 0)
			rc = (write(syncpipe[1], "x", 1) == 1) ? 0 : -1;
		close(syncpipe[1]);
		if (rc < 0) {
			kill(pid, SIGKILL);
			waitpid(pid, NULL, 0);
			close(sv[0]); close(sv[1]); iso->in_use = false;
			return -EPERM;
		}
	}

	close(sv[1]);

	iso->pid     = stub_pid;
	iso->sock_fd = sv[0];
	iso->alive   = true;

	/* Start the reader thread before announcing success. */
	if (pthread_create(&iso->reader_tid, NULL, isolate_reader_fn, iso)) {
		int e = errno;
		close(iso->sock_fd);
		iso->sock_fd = -1;
		kill(stub_pid, SIGKILL);   /* the grandchild stub, not the reaped intermediate */
		waitpid(stub_pid, NULL, 0);
		iso->in_use = false;
		return -e;
	}
	iso->reader_started = true;

	/*
	 * Set up the command-buffer ring (docs/design/command_buffer.md).
	 * Pure optimisation: failure is logged and ignored — the isolate keeps
	 * serving every ioctl over the existing IOCTL/MMAP path.  NVKVM_RING_DISABLE
	 * skips it entirely (debugging / A-B perf comparison).
	 */
	if (getenv("NVKVM_RING_DISABLE") == NULL) {
		int rret = nvkvm_isolate_ring_setup(t, id, nv);
		if (rret != 0)
			NVKVM_DBG(
				"nvkvm_isolate: ring setup for isolate %u failed: %d "
				"(falling back to socket path)\n", id, rret);
	}

	*isolate_id_out = id;

	if (use_uid)
		NVKVM_DBG(
			"nvkvm_isolate: created isolate %u pid=%d sock=%d "
			"uid=%u gid=%u mode=%s\n",
			id, stub_pid, sv[0], (unsigned)run_uid,
			(unsigned)run_uid, nvkvm_iso_mode_str(mode));
	else
		NVKVM_DBG(
			"nvkvm_isolate: created isolate %u pid=%d sock=%d\n",
			id, stub_pid, sv[0]);
	return 0;
}

/*
 * Return the host pid of a live isolate by id, or 0.  Used by the GET_PID_INFO
 * translator to map a guest pid's owning isolate to the real host pid the kernel
 * can resolve — QEMU thereby validates that a per-pid query targets a managed
 * isolate of this VM, never an arbitrary host pid.
 */
pid_t nvkvm_isolate_host_pid(struct nvkvm_isolate_table *t, uint32_t isolate_id)
{
	pid_t pid = 0;
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return 0;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];
	pthread_mutex_lock(&iso->lock);
	if (iso->in_use && iso->id == isolate_id && iso->alive)
		pid = iso->pid;
	pthread_mutex_unlock(&iso->lock);
	return pid;
}

/* ── Kill isolate ───────────────────────────────────────────────────────── */

static void ring_qva_unmap(void *nv, uint64_t ring_gpa, void *qva,
			   uint64_t region);

int nvkvm_isolate_kill(struct nvkvm_isolate_table *t, uint32_t isolate_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;

	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	if (!iso->in_use || iso->id != isolate_id) {
		pthread_mutex_unlock(&iso->lock);
		return -ENOENT;
	}

	if (iso->alive && iso->sock_fd >= 0) {
		struct isolate_cmd_exit cmd = { .type = ISOLATE_CMD_EXIT };
		/* Best-effort; ignore error — we're shutting down anyway. */
		send(iso->sock_fd, &cmd, sizeof(cmd), MSG_NOSIGNAL);
	}

	iso->alive = false;
	pthread_mutex_unlock(&iso->lock);

	/*
	 * Audit C-1: an IOCTL_ON_ISOLATE runs on a QEMU thread-pool worker and
	 * sends on iso->sock_fd under write_lock, on a DIFFERENT thread than this
	 * kill (the TX thread).  Tear the fd down under write_lock too, so we
	 * either wait for an in-flight send to finish or a later one observes
	 * sock_fd==-1 and skips — never a close()+reuse race where a worker
	 * writes isolate bytes into a recycled fd.
	 */
	pthread_mutex_lock(&iso->write_lock);
	int sock_fd = iso->sock_fd;
	iso->sock_fd = -1;
	pthread_mutex_unlock(&iso->write_lock);

	/* Closing the socket makes the reader thread's recv() return 0/error. */
	if (sock_fd >= 0)
		close(sock_fd);

	/*
	 * Join the reader thread: it has already signaled all pending IOCTL
	 * callers and the sync waiter (if any).  After join, no thread accesses
	 * iso's internals through the reader path.
	 */
	if (iso->reader_started) {
		pthread_join(iso->reader_tid, NULL);
		iso->reader_started = false;
	}

	pid_t pid = iso->pid;
	if (pid > 0) {
		int status;
		/*
		 * C-2: KILL runs on the single TX thread, so a fixed 500 ms sleep
		 * here stalled ALL virtio processing for the whole VM on every
		 * teardown (a guest CREATE/KILL loop could wedge throughput).
		 * The stub already got ISOLATE_CMD_EXIT + a closed socket, so it
		 * exits promptly; poll for that in short steps and break as soon
		 * as it's reaped — typical stall ~10 ms.  SIGKILL only if it
		 * overstays the budget (avoids a premature mid-ioctl kill).
		 */
		int reaped = 0;
		for (int i = 0; i < 50; i++) {     /* up to ~500 ms, 10 ms steps */
			if (waitpid(pid, &status, WNOHANG) != 0) {
				reaped = 1;
				break;
			}
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
			nanosleep(&ts, NULL);
		}
		if (!reaped) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
		}
	}

	/*
	 * Tear down the command-buffer ring.  The stub is already dead (its
	 * mapping went with it), so we only release QEMU's own mapping + memfd
	 * and, once Phase 4 installs it, the guest KVM memslot.
	 */
	pthread_mutex_lock(&iso->lock);
	void *ring_qva    = iso->ring_qva;
	uint64_t ring_sz  = iso->ring_region_size;
	int ring_mfd      = iso->ring_memfd;
	uint64_t ring_gpa = iso->ring_gpa;
	iso->ring_qva    = NULL;
	iso->ring_memfd  = -1;
	iso->ring_kvm_slot = -1;
	iso->ring_ready  = false;
	iso->ring_gpa    = 0;
	iso->pid    = 0;
	/*
	 * Release the UID-separation id.  Safe here and nowhere earlier: the
	 * waitpid() above has already reaped the stub, so no process is running
	 * as this uid any more.  Clearing it before the reap would let the next
	 * isolate to land in this slot share a uid with a still-live process.
	 */
	iso->run_uid = 0;
	iso->run_gid = 0;
	iso->in_use = false;
	pthread_mutex_unlock(&iso->lock);

	/* Window-aware: ring_gpa != 0 → restore anon backing + free the window
	 * extent; private fallback → plain munmap. */
	if (ring_qva && ring_qva != MAP_FAILED && ring_sz)
		ring_qva_unmap(t->nv, ring_gpa, ring_qva, ring_sz);
	if (ring_mfd >= 0)
		close(ring_mfd);

	NVKVM_DBG( "nvkvm_isolate: killed isolate %u\n", isolate_id);
	return 0;
}

void nvkvm_isolate_kill_session(struct nvkvm_isolate_table *t,
				uint32_t session_id)
{
	for (int i = 1; i < NVKVM_ISOLATE_MAX; i++) {
		struct nvkvm_isolate *iso = &t->isolates[i];
		if (iso->in_use && iso->session_id == session_id)
			nvkvm_isolate_kill(t, iso->id);
	}
}

/* ── Sync command helpers ───────────────────────────────────────────────── */

/*
 * Send a fixed-size command and wait for an OK/ERROR response.
 * The reader thread delivers the response via sync_cond.
 * Caller must NOT hold iso->lock.
 */
static int sync_send_recv(struct nvkvm_isolate *iso,
			  const void *cmd_buf, size_t cmd_size)
{
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done  = false;
	iso->sync_error = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, cmd_buf, cmd_size);
	pthread_mutex_unlock(&iso->write_lock);

	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		return (int)sr;
	}

	while (!iso->sync_done)
		pthread_cond_wait(&iso->sync_cond, &iso->sync_lock);
	int result = iso->sync_error;
	pthread_mutex_unlock(&iso->sync_lock);
	return result;
}

/*
 * Like sync_send_recv but via sendmsg (for SCM_RIGHTS); returns sync_error.
 */
static int sync_sendmsg_recv(struct nvkvm_isolate *iso, struct msghdr *msg)
{
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done  = false;
	iso->sync_error = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_sendmsg_fd(iso->sock_fd, msg);
	pthread_mutex_unlock(&iso->write_lock);

	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		return (int)sr;
	}

	while (!iso->sync_done)
		pthread_cond_wait(&iso->sync_cond, &iso->sync_lock);
	int result = iso->sync_error;
	pthread_mutex_unlock(&iso->sync_lock);
	return result;
}

/*
 * Like sync_send_recv but returns the MMAP retval on success.
 */
static int sync_send_recv_mmap(struct nvkvm_isolate *iso,
				const void *cmd_buf, size_t cmd_size)
{
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done        = false;
	iso->sync_error       = 0;
	iso->sync_mmap_retval = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, cmd_buf, cmd_size);
	pthread_mutex_unlock(&iso->write_lock);

	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		return (int)sr;
	}

	while (!iso->sync_done)
		pthread_cond_wait(&iso->sync_cond, &iso->sync_lock);
	int result = iso->sync_error ? iso->sync_error : iso->sync_mmap_retval;
	pthread_mutex_unlock(&iso->sync_lock);
	return result;
}

/* ── Handle distribution ────────────────────────────────────────────────── */

int nvkvm_isolate_send_handle(struct nvkvm_isolate_table *t,
			      struct nvkvm_handle_table *ht,
			      uint32_t isolate_id, uint32_t handle_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	struct nvkvm_handle *h = valid ? nvkvm_handle_get(ht, handle_id) : NULL;
	int fd        = (h && h->fd >= 0) ? h->fd : -1;
	int h_dev_id  = h ? h->dev_id : 0;
	pthread_mutex_unlock(&iso->lock);

	if (!valid)
		return -ENOENT;
	if (fd < 0)
		return -EBADF;

	struct isolate_cmd_receive_fd hdr = {
		.type      = ISOLATE_CMD_RECEIVE_FD,
		.handle_id = handle_id,
		.dev_id    = (uint32_t)h_dev_id,
	};
	struct msghdr   msg  = { 0 };
	struct iovec    iov  = { .iov_base = &hdr, .iov_len = sizeof(hdr) };
	char            cbuf[CMSG_SPACE(sizeof(int))];

	msg.msg_iov        = &iov;
	msg.msg_iovlen     = 1;
	msg.msg_control    = cbuf;
	msg.msg_controllen = sizeof(cbuf);

	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type  = SCM_RIGHTS;
	cm->cmsg_len   = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &fd, sizeof(int));

	int ret = sync_sendmsg_recv(iso, &msg);
	if (ret == 0)
		nvkvm_handle_ref_isolate(ht, handle_id);
	return ret;
}

/* ── Command-buffer ring setup ──────────────────────────────────────────── */

/*
 * Undo the ring's QEMU-side mapping, window-aware.  Inside the sparse window we
 * must NOT munmap (that would punch a hole in the window's single VMA/memslot);
 * instead restore the anonymous backing in place and return the GPA extent to
 * the window allocator.  A private fallback mapping is plain-munmap'd.
 */
static void ring_qva_unmap(void *nv, uint64_t ring_gpa, void *qva,
			   uint64_t region)
{
	if (qva == MAP_FAILED || !qva)
		return;
	if (nv && ring_gpa) {
		mmap(qva, region, PROT_READ | PROT_WRITE,
		     MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE | MAP_FIXED,
		     -1, 0);
		nvkvm_sparse_gpa_free((VirtIONvgpu *)nv, ring_gpa, region);
	} else {
		munmap(qva, region);
	}
}

int nvkvm_isolate_ring_setup(struct nvkvm_isolate_table *t, uint32_t isolate_id,
			     void *nv)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool ok = iso->in_use && iso->id == isolate_id && iso->alive &&
		  iso->ring_memfd < 0;   /* not already set up */
	pthread_mutex_unlock(&iso->lock);
	if (!ok)
		return -EINVAL;

	uint32_t ring_bytes = NVKVM_RING_DEFAULT_BYTES;
	uint64_t region = nvkvm_ring_region_size(ring_bytes);
	region = (region + 4095) & ~4095ULL;   /* page-round for mmap/ftruncate */

	int mfd = nvkvm_memfd_create("nvkvm-ring", MFD_CLOEXEC);
	if (mfd < 0)
		return -errno;
	if (ftruncate(mfd, (off_t)region) < 0) {
		int e = -errno; close(mfd); return e;
	}

	/*
	 * Place the ring memfd into the sparse GPA window so the guest can map
	 * it, exactly like MMAP_ON_ISOLATE places a device fd: allocate a window
	 * GPA, MAP_FIXED the memfd over the window's anonymous backing at that
	 * VA.  The window's single pre-installed KVM memslot then maps
	 * [gpa, gpa+region) → these memfd pages — no new memslot, no overlap.
	 * If the window isn't available yet (BAR unprogrammed) we fall back to a
	 * private mapping: the QEMU↔isolate ring still works, but it's not
	 * guest-visible (ring_gpa stays 0 → the guest uses the virtqueue path).
	 */
	uint64_t ring_gpa = 0;
	void    *qva      = MAP_FAILED;
	if (nv) {
		ring_gpa = nvkvm_sparse_gpa_alloc((VirtIONvgpu *)nv, region);
		void *target = ring_gpa ?
			nvkvm_gpa_to_vmm_va((VirtIONvgpu *)nv, ring_gpa, region) : NULL;
		if (target) {
			qva = mmap(target, region, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_FIXED, mfd, 0);
			if (qva == MAP_FAILED) {
				/* Restore the anon backing we clobbered so the
				 * window stays fully mapped for KVM. */
				mmap(target, region, PROT_READ | PROT_WRITE,
				     MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE |
				     MAP_FIXED, -1, 0);
				nvkvm_sparse_gpa_free((VirtIONvgpu *)nv,
						      ring_gpa, region);
				ring_gpa = 0;
			}
		} else {
			ring_gpa = 0;   /* window full / not ready */
		}
	}
	if (qva == MAP_FAILED) {
		/* Fallback: private mapping (not guest-visible). */
		ring_gpa = 0;
		qva = mmap(NULL, region, PROT_READ | PROT_WRITE,
			   MAP_SHARED, mfd, 0);
		if (qva == MAP_FAILED) {
			int e = -errno; close(mfd); return e;
		}
	}

	/* Initialise both ring control blocks: head==tail==0 ⇒ empty. */
	uint64_t resp_off = nvkvm_ring_resp_off(ring_bytes);
	struct nvkvm_ring *req  = (struct nvkvm_ring *)qva;
	struct nvkvm_ring *resp = (struct nvkvm_ring *)((uint8_t *)qva + resp_off);
	memset(req, 0, sizeof(*req));   req->size  = ring_bytes;
	memset(resp, 0, sizeof(*resp)); resp->size = ring_bytes;

	/* Bidirectional shared-memory self-test probe (see proto header). */
	uint64_t probe = 0x6e766b766d000000ULL | isolate_id;   /* "nvkvm\0\0\0" | id */
	uint8_t *req_data  = (uint8_t *)req  + sizeof(struct nvkvm_ring);
	uint8_t *resp_data = (uint8_t *)resp + sizeof(struct nvkvm_ring);
	memcpy(req_data, &probe, sizeof(probe));
	memset(resp_data, 0, sizeof(uint64_t));

	struct isolate_cmd_setup_ring hdr = {
		.type        = ISOLATE_CMD_SETUP_RING,
		.region_size = (uint32_t)region,
		.req_off     = 0,
		.resp_off    = (uint32_t)resp_off,
		.ring_bytes  = ring_bytes,
	};
	struct msghdr msg = { 0 };
	struct iovec  iov = { .iov_base = &hdr, .iov_len = sizeof(hdr) };
	char          cbuf[CMSG_SPACE(sizeof(int))];
	msg.msg_iov        = &iov;
	msg.msg_iovlen     = 1;
	msg.msg_control    = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type  = SCM_RIGHTS;
	cm->cmsg_len   = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &mfd, sizeof(int));

	int ret = sync_sendmsg_recv(iso, &msg);   /* reader fills sync_ring_probe */
	if (ret != 0) {
		ring_qva_unmap(nv, ring_gpa, qva, region); close(mfd); return ret;
	}

	/* Verify QEMU→isolate (probe echoed) and isolate→QEMU (resp_data). */
	pthread_mutex_lock(&iso->sync_lock);
	uint64_t echoed = iso->sync_ring_probe;
	pthread_mutex_unlock(&iso->sync_lock);
	uint64_t back = 0;
	memcpy(&back, resp_data, sizeof(back));
	if (echoed != probe || back != (probe ^ NVKVM_RING_PROBE_MASK)) {
		NVKVM_DBG(
			"nvkvm_isolate: ring %u self-test FAILED "
			"(echo=0x%llx back=0x%llx want_echo=0x%llx want_back=0x%llx)\n",
			isolate_id,
			(unsigned long long)echoed, (unsigned long long)back,
			(unsigned long long)probe,
			(unsigned long long)(probe ^ NVKVM_RING_PROBE_MASK));
		ring_qva_unmap(nv, ring_gpa, qva, region); close(mfd); return -EPROTO;
	}

	/* Self-test passed — wipe the probe so the data regions start clean. */
	memset(req_data, 0, sizeof(uint64_t));
	memset(resp_data, 0, sizeof(uint64_t));

	pthread_mutex_lock(&iso->lock);
	iso->ring_memfd       = mfd;
	iso->ring_qva         = qva;
	iso->ring_region_size = region;
	iso->ring_bytes       = ring_bytes;
	iso->ring_gpa         = ring_gpa;  /* sparse-window GPA, or 0 if private */
	iso->ring_kvm_slot    = -1;        /* in-window: no dedicated slot */
	iso->ring_ready       = true;
	pthread_mutex_unlock(&iso->lock);

	NVKVM_DBG(
		"nvkvm_isolate: ring %u ready (region=%llu B, ring_bytes=%u, "
		"resp_off=%llu, gpa=0x%llx %s) — bidirectional probe OK\n",
		isolate_id, (unsigned long long)region, ring_bytes,
		(unsigned long long)resp_off, (unsigned long long)ring_gpa,
		ring_gpa ? "guest-visible" : "private");
	return 0;
}

int nvkvm_isolate_ring_info(struct nvkvm_isolate_table *t, uint32_t isolate_id,
			    uint64_t *gpa, uint32_t *region_size,
			    uint32_t *resp_off, uint32_t *ring_bytes)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	int rc = -ENODEV;
	if (iso->in_use && iso->id == isolate_id && iso->alive &&
	    iso->ring_ready && iso->ring_gpa) {
		if (gpa)         *gpa         = iso->ring_gpa;
		if (region_size) *region_size = (uint32_t)iso->ring_region_size;
		if (resp_off)    *resp_off    = (uint32_t)nvkvm_ring_resp_off(iso->ring_bytes);
		if (ring_bytes)  *ring_bytes  = iso->ring_bytes;
		rc = 0;
	}
	pthread_mutex_unlock(&iso->lock);
	return rc;
}

int nvkvm_isolate_interrupt(struct nvkvm_isolate_table *t,
			    uint32_t isolate_id, uint32_t target_txn)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	struct isolate_cmd_interrupt cmd = {
		.type       = ISOLATE_CMD_INTERRUPT,
		.target_txn = target_txn,
	};

	/*
	 * Fire-and-forget under write_lock — no sync_lock, no response wait.
	 * The reader thread is the sole reader; the stub posts SIGUSR1 to the
	 * worker and the interrupted ioctl's result comes back on the normal
	 * IOCTL response path.  write_lock just serialises this write against
	 * concurrent command writers on the same socket.
	 */
	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive &&
		     iso->sock_fd >= 0;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, &cmd, sizeof(cmd));
	pthread_mutex_unlock(&iso->write_lock);
	return sr < 0 ? (int)sr : 0;
}

int nvkvm_isolate_enter_loop(struct nvkvm_isolate_table *t, uint32_t isolate_id,
			     uint32_t idle_us, uint64_t *head_out)
{
	if (head_out)
		*head_out = 0;
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive &&
		     iso->ring_ready && iso->sock_fd >= 0;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENODEV;

	struct isolate_cmd_enter_loop cmd = {
		.type    = ISOLATE_CMD_ENTER_LOOP,
		.idle_us = idle_us,
	};

	/*
	 * Sync send: this BLOCKS until the stub's consumer loop idles out and
	 * replies LOOP_EXITED (which the reader thread delivers via sync_cond).
	 * The caller runs on QEMU's thread pool, so a long loop does not stall
	 * the main loop.  Slow-path IOCTLs that arrive while the stub loops use
	 * the independent per-txn pending mechanism, not sync_lock.
	 */
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done      = false;
	iso->sync_error     = 0;
	iso->sync_loop_head = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, &cmd, sizeof(cmd));
	pthread_mutex_unlock(&iso->write_lock);
	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		return (int)sr;
	}

	/* F-5: ENTER_LOOP runs on the thread pool (not the serialized TX thread),
	 * so guard against the slot being killed+reused under us: bail if our
	 * identity no longer holds. The kill path also sets sync_done via
	 * reader_signal_sync (broadcast), so this is belt-and-suspenders. */
	while (!iso->sync_done && iso->id == isolate_id && iso->alive)
		pthread_cond_wait(&iso->sync_cond, &iso->sync_lock);
	int err;
	if (!iso->sync_done) {
		err = -ENODEV;                 /* torn down / reused while parked */
	} else {
		err = iso->sync_error;
		if (head_out)
			*head_out = iso->sync_loop_head;
	}
	pthread_mutex_unlock(&iso->sync_lock);
	return err;
}

int nvkvm_isolate_open_device(struct nvkvm_isolate_table *t,
			      uint32_t isolate_id, uint32_t handle_id,
			      uint32_t dev_id, uint32_t flags,
			      int *fd_out)
{
	if (fd_out)
		*fd_out = -1;
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	uint32_t txn_id = iso->next_txn_id++;
	if (iso->next_txn_id == 0)
		iso->next_txn_id = 1;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_open_device cmd = {
		.type      = ISOLATE_CMD_OPEN_DEVICE,
		.handle_id = handle_id,
		.dev_id    = dev_id,
		.flags     = flags,
		.txn_id    = txn_id,
	};

	/* Inline sync_send_recv pattern; we also need the received fd, which
	 * the reader stashes in iso->sync_open_fd before signaling. */
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done    = false;
	iso->sync_error   = 0;
	iso->sync_open_fd = -1;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, &cmd, sizeof(cmd));
	pthread_mutex_unlock(&iso->write_lock);
	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		return (int)sr;
	}

	while (!iso->sync_done)
		pthread_cond_wait(&iso->sync_cond, &iso->sync_lock);
	int err = iso->sync_error;
	int fd  = iso->sync_open_fd;
	iso->sync_open_fd = -1;
	pthread_mutex_unlock(&iso->sync_lock);

	if (err) {
		if (fd >= 0)
			close(fd);
		return err;
	}
	if (fd < 0)
		return -EPROTO;   /* stub said success but sent no fd */

	if (fd_out)
		*fd_out = fd;
	else
		close(fd);
	return 0;
}

int nvkvm_isolate_present_export(struct nvkvm_isolate_table *t,
				 uint32_t isolate_id, uint32_t handle_id,
				 uint32_t gem_handle, int *fd_out)
{
	if (fd_out)
		*fd_out = -1;
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	uint32_t txn_id = iso->next_txn_id++;
	if (iso->next_txn_id == 0)
		iso->next_txn_id = 1;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_present_export cmd = {
		.type       = ISOLATE_CMD_PRESENT_EXPORT,
		.handle_id  = handle_id,
		.gem_handle = gem_handle,
		.txn_id     = txn_id,
	};

	/* present_lock serializes present-export callers (held across the whole
	 * round-trip); present_sync_lock + present_cond are the reader handoff. */
	pthread_mutex_lock(&iso->present_lock);
	pthread_mutex_lock(&iso->present_sync_lock);
	iso->present_done = false;
	iso->present_err  = 0;
	iso->present_fd   = -1;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, &cmd, sizeof(cmd));
	pthread_mutex_unlock(&iso->write_lock);
	if (sr < 0) {
		pthread_mutex_unlock(&iso->present_sync_lock);
		pthread_mutex_unlock(&iso->present_lock);
		return (int)sr;
	}

	while (!iso->present_done)
		pthread_cond_wait(&iso->present_cond, &iso->present_sync_lock);
	int err = iso->present_err;
	int fd  = iso->present_fd;
	iso->present_fd = -1;
	pthread_mutex_unlock(&iso->present_sync_lock);
	pthread_mutex_unlock(&iso->present_lock);

	if (err) {
		if (fd >= 0)
			close(fd);
		return err;
	}
	if (fd < 0)
		return -EPROTO;   /* stub said success but sent no fd */
	if (fd_out)
		*fd_out = fd;
	else
		close(fd);
	return 0;
}

/*
 * Cross-isolate import (#110): hand `dmabuf_fd` (a host dma-buf the OWNER stub
 * exported) to the IMPORTER isolate's stub, which PRIME_FD_TO_HANDLEs it into a
 * local GEM and returns the handle.  The caller still owns dmabuf_fd afterwards
 * (the stub takes its own reference via the SCM dup + PRIME import).
 */
int nvkvm_isolate_xiso_import(struct nvkvm_isolate_table *t,
			      uint32_t isolate_id, uint32_t handle_id,
			      int dmabuf_fd, uint32_t *gem_out)
{
	if (gem_out)
		*gem_out = 0;
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	if (dmabuf_fd < 0)
		return -EINVAL;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	uint32_t txn_id = iso->next_txn_id++;
	if (iso->next_txn_id == 0)
		iso->next_txn_id = 1;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_xiso_import cmd = {
		.type      = ISOLATE_CMD_XISO_IMPORT,
		.handle_id = handle_id,
		.txn_id    = txn_id,
	};
	struct msghdr msg = { 0 };
	struct iovec  iov = { .iov_base = &cmd, .iov_len = sizeof(cmd) };
	char          cbuf[CMSG_SPACE(sizeof(int))];
	msg.msg_iov        = &iov;
	msg.msg_iovlen     = 1;
	msg.msg_control    = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type  = SCM_RIGHTS;
	cm->cmsg_len   = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &dmabuf_fd, sizeof(int));

	pthread_mutex_lock(&iso->xiso_lock);
	pthread_mutex_lock(&iso->xiso_sync_lock);
	iso->xiso_done = false;
	iso->xiso_err  = 0;
	iso->xiso_gem  = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_sendmsg_fd(iso->sock_fd, &msg);
	pthread_mutex_unlock(&iso->write_lock);
	if (sr < 0) {
		pthread_mutex_unlock(&iso->xiso_sync_lock);
		pthread_mutex_unlock(&iso->xiso_lock);
		return (int)sr;
	}

	while (!iso->xiso_done)
		pthread_cond_wait(&iso->xiso_cond, &iso->xiso_sync_lock);
	int err = iso->xiso_err;
	uint32_t gem = iso->xiso_gem;
	pthread_mutex_unlock(&iso->xiso_sync_lock);
	pthread_mutex_unlock(&iso->xiso_lock);

	if (err)
		return err;
	if (gem == 0)
		return -EPROTO;
	if (gem_out)
		*gem_out = gem;
	return 0;
}

int nvkvm_isolate_close_handle(struct nvkvm_isolate_table *t,
				struct nvkvm_handle_table *ht,
				uint32_t isolate_id, uint32_t handle_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_close_fd cmd = {
		.type      = ISOLATE_CMD_CLOSE_FD,
		.handle_id = handle_id,
	};
	int ret = sync_send_recv(iso, &cmd, sizeof(cmd));
	if (ret == 0)
		nvkvm_handle_unref_isolate(ht, handle_id);
	return ret;
}

/* ── Ioctl forwarding (async, multi-inflight) ───────────────────────────── */

int nvkvm_isolate_ioctl(struct nvkvm_isolate_table *t,
			uint32_t isolate_id, uint32_t handle_id,
			unsigned int cmd,
			void *param_buf, size_t param_size,
			void *aux_buf, size_t aux_size,
			uint32_t flags,
			uint32_t *nvstatus_out,
			uint64_t *fault_addr_out)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	/* Build a pending slot on the caller's stack. */
	struct nvkvm_pending_ioctl pending = {
		.done      = false,
		.error     = 0,
		.param_buf = param_buf,
		.param_cap = param_size,
		.aux_buf   = aux_buf,
		.aux_cap   = aux_size,
	};
	pthread_cond_init(&pending.cond, NULL);

	/* Validate and register. */
	pthread_mutex_lock(&iso->lock);
	if (!iso->in_use || iso->id != isolate_id || !iso->alive) {
		pthread_mutex_unlock(&iso->lock);
		pthread_cond_destroy(&pending.cond);
		return -ENOENT;
	}
	pending.txn_id = iso->next_txn_id++;
	if (iso->next_txn_id == 0)
		iso->next_txn_id = 1;
	pending.next      = iso->pending_head;
	iso->pending_head = &pending;
	pthread_mutex_unlock(&iso->lock);

	/* Send command under write_lock. */
	struct isolate_cmd_ioctl hdr = {
		.type        = ISOLATE_CMD_IOCTL,
		.handle_id   = handle_id,
		.cmd         = (uint32_t)cmd,
		.param_size  = (uint32_t)param_size,
		.aux_size    = (uint32_t)aux_size,
		.flags       = flags,
		.txn_id      = pending.txn_id,
		.abi_profile = t->abi_profile,   /* #81 */
	};

	pthread_mutex_lock(&iso->write_lock);
	/* C-1: snapshot the fd under write_lock; kill() nulls it under the same
	 * lock, so a concurrent teardown is either ordered before us (we see -1
	 * and skip) or after (our send completes before close()). */
	int sfd = iso->sock_fd;
	ssize_t sr = (sfd < 0) ? -EPIPE
			      : sock_send_full(sfd, &hdr, sizeof(hdr));
	if (sr >= 0 && param_size > 0)
		sr = sock_send_full(sfd, param_buf, param_size);
	if (sr >= 0 && aux_size > 0)
		sr = sock_send_full(sfd, aux_buf, aux_size);
	pthread_mutex_unlock(&iso->write_lock);

	/*
	 * Wait for the reader thread to deliver the response.
	 * If the send failed, the reader will notice the dead socket and
	 * signal us with -ECONNRESET.  Either way we always wait.
	 */
	pthread_mutex_lock(&iso->lock);
	while (!pending.done)
		pthread_cond_wait(&pending.cond, &iso->lock);
	/* Remove from pending list. */
	struct nvkvm_pending_ioctl **pp = &iso->pending_head;
	while (*pp && *pp != &pending)
		pp = &(*pp)->next;
	if (*pp)
		*pp = pending.next;
	pthread_mutex_unlock(&iso->lock);
	pthread_cond_destroy(&pending.cond);

	/* Prefer the transport error from the send over the reader's error. */
	if (sr < 0 && !pending.error)
		return (int)sr;
	if (pending.error)
		return pending.error;

	if (nvstatus_out)
		*nvstatus_out = pending.nvstatus;
	if (fault_addr_out)
		*fault_addr_out = pending.fault_addr;
	return pending.retval;
}

/* ── Mmap / munmap ──────────────────────────────────────────────────────── */

int nvkvm_isolate_mmap(struct nvkvm_isolate_table *t,
		       uint32_t isolate_id, uint32_t handle_id,
		       uint64_t gva, uint64_t length, uint64_t offset,
		       int prot, int map_flags)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_mmap cmd = {
		.type      = ISOLATE_CMD_MMAP,
		.handle_id = handle_id,
		.gva       = gva,
		.length    = length,
		.offset    = offset,
		.prot      = (uint32_t)prot,
		.map_flags = (uint32_t)map_flags,
	};
	return sync_send_recv_mmap(iso, &cmd, sizeof(cmd));
}

int nvkvm_isolate_munmap(struct nvkvm_isolate_table *t,
			 uint32_t isolate_id, uint64_t gva, uint64_t length)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_munmap cmd = {
		.type   = ISOLATE_CMD_MUNMAP,
		.gva    = gva,
		.length = length,
	};
	return sync_send_recv(iso, &cmd, sizeof(cmd));
}

/* ── REALIZE_UVM_FD ─────────────────────────────────────────────────────── */

int nvkvm_isolate_realize_uvm_fd(struct nvkvm_isolate_table *t,
				 uint32_t isolate_id,
				 uint32_t mode,
				 const void *state, uint32_t state_size,
				 const void *intent, uint32_t intent_size,
				 uint32_t prot, uint32_t map_flags,
				 uint64_t length, uint64_t host_va_hint,
				 uint64_t offset,
				 uint64_t *host_va_out, uint64_t *length_out,
				 uint64_t *token_out, uint32_t *rm_status_out)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_realize_uvm_fd cmd = {
		.type         = ISOLATE_CMD_REALIZE_UVM_FD,
		.mode         = mode,
		.state_size   = state_size,
		.intent_size  = intent_size,
		.prot         = prot,
		.map_flags    = map_flags,
		.length       = length,
		.host_va_hint = host_va_hint,
		.offset       = offset,
	};

	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done              = false;
	iso->sync_error             = 0;
	iso->sync_realize_host_va   = 0;
	iso->sync_realize_length    = 0;
	iso->sync_realize_token     = 0;
	iso->sync_realize_rm_status = 0;

	/* All three writes (header + state + intent) must reach the stub
	 * atomically wrt other senders — hold write_lock across them. */
	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr1 = sock_send_full(iso->sock_fd, &cmd, sizeof(cmd));
	ssize_t sr2 = 0, sr3 = 0;
	if (sr1 >= 0 && state_size > 0)
		sr2 = sock_send_full(iso->sock_fd, state, state_size);
	if (sr1 >= 0 && sr2 >= 0 && intent_size > 0)
		sr3 = sock_send_full(iso->sock_fd, intent, intent_size);
	pthread_mutex_unlock(&iso->write_lock);

	ssize_t sr = (sr1 < 0) ? sr1 : ((sr2 < 0) ? sr2 : sr3);
	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		return (int)sr;
	}

	while (!iso->sync_done)
		pthread_cond_wait(&iso->sync_cond, &iso->sync_lock);
	int err           = iso->sync_error;
	uint64_t host_va  = iso->sync_realize_host_va;
	uint64_t out_len  = iso->sync_realize_length;
	uint64_t token    = iso->sync_realize_token;
	uint32_t rm_st    = iso->sync_realize_rm_status;
	pthread_mutex_unlock(&iso->sync_lock);

	if (host_va_out)   *host_va_out   = host_va;
	if (length_out)    *length_out    = out_len;
	if (token_out)     *token_out     = token;
	if (rm_status_out) *rm_status_out = rm_st;
	return err;
}

/* ── Poll / unpoll ──────────────────────────────────────────────────────── */

int nvkvm_isolate_poll(struct nvkvm_isolate_table *t,
		       uint32_t isolate_id, uint32_t handle_id,
		       uint32_t events)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_poll cmd = {
		.type      = ISOLATE_CMD_POLL,
		.handle_id = handle_id,
		.events    = events,
	};
	return sync_send_recv(iso, &cmd, sizeof(cmd));
}

int nvkvm_isolate_unpoll(struct nvkvm_isolate_table *t,
			 uint32_t isolate_id, uint32_t handle_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_unpoll cmd = {
		.type      = ISOLATE_CMD_UNPOLL,
		.handle_id = handle_id,
	};
	return sync_send_recv(iso, &cmd, sizeof(cmd));
}
