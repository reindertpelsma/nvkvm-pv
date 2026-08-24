/*
 * nvkvm_isolate.c — QEMU-side isolate process manager (multi-inflight)
 *
 * Each isolate has a dedicated reader thread that multiplexes IOCTL responses
 * by txn_id onto per-caller condvars allocated on the callers' stacks.
 * Non-IOCTL commands serialize via sync_cmd_lock (a real one-at-a-time gate,
 * audit F3-1) and hand off through sync_lock + sync_cond.
 * All socket writes go through write_lock (prevents partial-send interleaving).
 *
 * The stub binary is embedded as nvkvm_stub_elf[] generated at build time
 * from src/stub/nvkvm_stub; loaded via memfd_create + fexecve (no disk file).
 *
 * Lock order: sync_cmd_lock > sync_lock > write_lock > lock
 * (present_lock/xiso_lock/loop_lock are independent outer gates over their own
 * *_sync_lock; they nest write_lock beneath them but never sync_lock.)
 */

#include "qemu/osdep.h"
#include "nvkvm_drm_node.h"
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
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
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
/* Same guard, same reason: the DRM render-node fds parked for the stub. */
#ifndef NVKVM_DRM_FD_MAX
#define NVKVM_DRM_FD_MAX 8
#endif
#ifndef NVKVM_DRM_FD
#define NVKVM_DRM_FD(k)  (NVKVM_DEV_DIRFD + 1 + (k))
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
/*
 * Split into two halves because UID separation has to happen BETWEEN them.
 *
 * PR_CAPBSET_DROP requires CAP_SETPCAP, and setresuid() to a non-zero uid
 * clears the whole permitted set — so calling the bounding-set loop after the
 * uid drop silently does nothing. Measured on a real isolate before this was
 * split: Uid/Gid/CapEff/Groups were all correctly dropped while CapBnd was
 * still 000001ffffffffff, i.e. the full set. That is inert in practice (with
 * no_new_privs set, no permitted caps, a non-root uid and no execve in the
 * seccomp allowlist there is no path to regain anything) but it is a silent
 * divergence from namespace mode, and "inert in practice" is not a property
 * worth relying on when the fix is an ordering change.
 *
 * Order is now: pre() -> [uid drop] -> post(), and for the modes without a uid
 * drop the two run back-to-back, which is byte-for-byte the previous sequence.
 */
static void nvkvm_drop_caps_pre(void)
{
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
		_exit(126);
	prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
	/* Drop the capability bounding set (so caps can't be regained on exec).
	 * Needs CAP_SETPCAP, which we still hold here.  Dropping a capability
	 * from the BOUNDING set does not remove it from permitted/effective, so
	 * the loop can drop CAP_SETPCAP itself and keep going, and the uid drop
	 * that follows still has its CAP_SETUID/CAP_SETGID. */
	for (int c = 0; c <= 63; c++)
		prctl(PR_CAPBSET_DROP, c, 0, 0, 0);  /* EINVAL past last cap: ok */
}

static void nvkvm_drop_caps_post(void)
{
	/* A uid change resets dumpable to fs.suid_dumpable; pin it back to 0. */
	prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
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
	/*
	 * setgroups policy.
	 *
	 * Plain namespace mode writes "deny", unchanged: it is what stops an
	 * unprivileged process in a user namespace from dropping a supplementary
	 * group to get past a negative group permission.
	 *
	 * Combined namespace+uid mode must NOT write it, and the reason is
	 * measured rather than theoretical.  With "deny", setgroups(2) is
	 * permanently EPERM inside the namespace, so the child cannot run
	 * setgroups(0, NULL) and keeps QEMU's inherited supplementary group 0 —
	 * observed on a real isolate as `Uid: 500004 ... Groups: 0`, i.e. a uid
	 * separated isolate still carrying the host root group.  That defeats
	 * half the point of adding uid separation on top.
	 *
	 * Leaving the policy at its default ("allow") lets the child clear the
	 * group list outright, which is strictly stronger than being unable to
	 * change it.  The exposure "deny" exists to prevent does not apply: the
	 * child is blocked on the sync pipe until we release it, its very first
	 * act is setgroups(0, NULL), and after the uid drop it holds no
	 * CAP_SETGID — nor is setgroups in the stub's seccomp allowlist — so it
	 * can never add a group back.
	 */
	if (!extra_uid && nvkvm_write_child_map(pid, "setgroups", "deny") < 0)
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
/*
 * Open the host's NVIDIA render nodes and park them at NVKVM_DRM_FD(k), while
 * the child still has the privileges to do it.
 *
 * Must run BEFORE the mount-namespace pivot or the chroot (the nodes are
 * addressed by their real path here) and before the uid drop (afterwards the
 * isolate has no group that can open a 0660 root:render node).  The fds are
 * deliberately NOT O_CLOEXEC: surviving the exec into the stub is the point.
 *
 * Returns how many were parked, so the caller can keep closefrom() off them.
 */
static unsigned nvkvm_child_park_drm_fds(void)
{
	unsigned k;

	for (k = 0; k < NVKVM_DRM_FD_MAX; k++) {
		char node[64];
		int fd;

		if (!nvkvm_nvidia_render_path(k, node, sizeof(node)))
			break;                    /* no k-th NVIDIA GPU */
		fd = open(node, O_RDWR);
		if (fd < 0)
			break;                    /* graphics-only; non-fatal */
		if (fd != NVKVM_DRM_FD(k)) {
			if (dup2(fd, NVKVM_DRM_FD(k)) < 0) {
				close(fd);
				break;
			}
			close(fd);
		}
	}
	return k;
}

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
	 * Create the subdir in the sandbox /dev and bind the NVIDIA nodes in.
	 *
	 * RENUMBER them while binding: the k-th NVIDIA node on the host, whatever
	 * minor it landed on, appears in the sandbox as renderD(128+k).  The stub
	 * resolves NVKVM_DEV_DRM_RD(k) to exactly that name, so this is what makes
	 * the guest's k-th render node mean "the k-th NVIDIA GPU" rather than
	 * "host minor 128+k".  Those differ on any machine where another GPU
	 * probes first — a hybrid-graphics laptop is the common case — and there
	 * the guest's DRM node used to fail to open with ENOENT while CUDA and
	 * headless EGL kept working, so nothing caught it.  Graphics-only;
	 * absence is non-fatal. */
	mkdir("/proc/dev/dri", 0755);
	for (unsigned k = 0; k < 8; k++) {
		int fd, minor = nvkvm_nvidia_render_minor(k);

		if (minor < 0)
			break;                    /* no k-th NVIDIA node */
		snprintf(src, sizeof(src), "/dev/dri/renderD%d", minor);
		if (access(src, F_OK) != 0)
			continue;
		snprintf(dst, sizeof(dst), "/proc/dev/dri/renderD%u", 128 + k);
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
	/* Audit R4-L2: this used to ignore its return, the same fail-open the
	 * MS_REMOUNT below already learned about.  pivot_root(".", ".") leaves
	 * the OLD host root stacked on the current directory; if the detach
	 * fails it stays overmounted and path resolution from the stub reaches
	 * the host filesystem again — i.e. the whole point of the pivot is
	 * silently undone.  Fail closed: the caller _exit(126)s the child. */
	if (umount2(".", MNT_DETACH) < 0)
		return -1;
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
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS   1033
#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#define F_SEAL_WRITE  0x0008
#endif
static inline int nvkvm_memfd_create(const char *name, unsigned int flags)
{
	return (int)syscall(SYS_memfd_create, name, (unsigned long)flags);
}

#include "nvkvm_isolate.h"
#include "virtio_nvgpu.h"
#include "nvkvm_present_egl.h"   /* S-4: nvkvm_present_forget_isolate() */

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

/*
 * Move an about-to-be-exec'd image descriptor clear of stdio.
 *
 * open()/memfd_create() return the LOWEST free descriptor.  When QEMU runs
 * with stdin closed that is fd 0 -- and the very next thing the child does is
 * dup2(sv[1], STDIN_FILENO), which destroys the image fd before it can be
 * parked at 3.  The child then dup2()s the COMMAND SOCKET to fd 3 and
 * fexecve()s it, so exec fails with EACCES and the isolate dies at 127 with no
 * diagnostic.  Measured, not theorised: strace of tests/unit/test_isolate
 * showed openat(...mock_stub) = 0, dup2(4,0) = 0, dup2(0,3) = 3,
 * execveat(3,...) = EACCES.  It fails CLOSED (no stub runs), so this is an
 * availability bug rather than an escape, but it takes the whole isolate with
 * it whenever fd 0 happens to be free.
 *
 * Returns a descriptor >= 3 (possibly the original), or -1.  CLOEXEC is kept
 * on the duplicate; the caller's dup2() to fd 3 clears it, as before.
 */
static int nvkvm_fd_clear_of_stdio(int fd)
{
	if (fd < 0 || fd >= 3)
		return fd;
	int nfd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
	if (nfd < 0)
		return -1;
	close(fd);
	return nfd;
}

/*
 * Park a /proc/self/fd directory fd for the fallback below, BEFORE the pivot.
 *
 * The fallback used to open "/proc/self/fd" at the point of use, which is
 * after nvkvm_child_enter_mount_ns()/nvkvm_iso_enter_chroot() have already put
 * the child in an empty root with no /proc.  The open therefore could not
 * succeed there, and the old code treated that as "nothing to do" and
 * returned.  So on any host where close_range(2) is unavailable (pre-5.9) or
 * denied by an outer seccomp policy, closefrom did nothing at all and the stub
 * inherited QEMU's descriptors -- the exact M6 exposure the function exists to
 * prevent, failing OPEN.
 *
 * Call this in the child while /proc still resolves.  The returned dirfd is
 * lifted clear of the fixed descriptor range that nvkvm_child_park_drm_fds()
 * and NVKVM_DEV_DIRFD dup2() into, so parking cannot silently overwrite it.
 * Returns -1 if /proc is unavailable; the caller then relies on close_range or
 * the RLIMIT_NOFILE loop.
 */
static int nvkvm_isolate_open_procfd(void)
{
	int dfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		return -1;

	int minfd = NVKVM_DRM_FD(NVKVM_DRM_FD_MAX - 1) + 1;
	if (dfd < minfd) {
		int high = fcntl(dfd, F_DUPFD_CLOEXEC, minfd);
		close(dfd);
		dfd = high;
	}
	return dfd;
}

/*
 * Returns 0 if every inherited fd at or above `first` is now closed, -1 if
 * that could not be established.  `procfd` is the dirfd from
 * nvkvm_isolate_open_procfd() (may be -1); this function consumes it.
 *
 * -1 is not advisory.  The caller must _exit() rather than exec the stub:
 * "we could not prove the descriptors are gone" and "the descriptors are gone"
 * are not the same statement, and only one of them is safe to exec on.
 */
static int nvkvm_isolate_closefrom(int first, int procfd)
{
	long r = syscall(__NR_close_range, first, ~0U, 0);
	if (r == 0)
		return 0;

	/* Fallback 1: walk the dirfd parked before the pivot.  We can't use
	 * opendir here (allocates), so dump a getdents64 buffer.  This is exact
	 * -- it names every open descriptor -- so it is preferred over the
	 * RLIMIT_NOFILE sweep below when it completes. */
	int dfd = procfd;
	if (dfd >= 0) {
		bool failed = false;
		char buf[4096];
		while (1) {
			long n = syscall(__NR_getdents64, dfd, buf, sizeof(buf));
			if (n == 0)
				break;
			/* A short read is end-of-directory; an error is NOT, and
			 * the old `n <= 0` conflated the two and reported success
			 * for a failed walk. */
			if (n < 0) {
				failed = true;
				break;
			}
			for (long off = 0; off < n; ) {
				struct linux_dirent64 {
					unsigned long  d_ino;
					long           d_off;
					unsigned short d_reclen;
					unsigned char  d_type;
					char           d_name[];
				} *de = (void *)(buf + off);
				if (de->d_reclen == 0 ||
				    off + de->d_reclen > n) {
					failed = true;
					break;
				}
				off += de->d_reclen;
				if (de->d_name[0] == '.')
					continue;
				int fd = atoi(de->d_name);
				if (fd >= first && fd != dfd)
					close(fd);
			}
			if (failed)
				break;
		}
		close(dfd);
		if (!failed)
			return 0;
	}

	/* Fallback 2: a finite RLIMIT_NOFILE still bounds the descriptor space,
	 * so closing every number in it is complete, if slower.  An infinite or
	 * unreadable limit gives no such bound -- report failure instead of
	 * pretending. */
	struct rlimit lim;
	if (getrlimit(RLIMIT_NOFILE, &lim) != 0 || lim.rlim_cur == RLIM_INFINITY)
		return -1;
	for (rlim_t fd = (rlim_t)first; fd < lim.rlim_cur; fd++)
		close((int)fd);
	return 0;
}

/* ── Bounded waits (audit F1-1 / F2-1, hang audit 2026-08) ───────────────── */

/*
 * WHY every QEMU->stub round-trip now has a deadline.
 *
 * The synchronous isolate commands (CLOSE_HANDLE, MMAP, MUNMAP, POLL, UNPOLL,
 * COPY_HANDLE, SETUP_RING, PRESENT_EXPORT, XISO_IMPORT, REALIZE_UVM_FD)
 * dispatch INLINE on the single virtio TX thread with the QEMU BQL held (see
 * the threading note at the top of virtio_nvgpu.c).  Every one of them used to
 * park in an untimed pthread_cond_wait, so a stub that simply never answered —
 * which an unprivileged guest process can arrange just by keeping the SPSC ring
 * continuously fed, starving the stub's control-socket service edge — wedged
 * the main loop, QMP, all timers and every vCPU until SIGKILL.
 *
 * We cannot fix that properly here: taking the round-trip off the BQL is a
 * threading-model change the owner has not asked for.  What a deadline DOES buy
 * is a downgrade from "one guest process hangs the whole VMM" to "one
 * misbehaving isolate is killed": on expiry we declare the isolate dead, which
 * tears down its socket, unblocks every other waiter on it, and lets the guest
 * see -ETIMEDOUT.  RESIDUAL RISK: for up to the timeout below the BQL is still
 * held, so the VM is still stalled — just bounded now, not forever.
 *
 * Choice of value: 30 s.  These commands are open/close/mmap/munmap, poll
 * registration and PRIME export/import — none of them queues GPU work or waits
 * on a channel, so the slowest realistic one is REALIZE_UVM_FD (UVM_INITIALIZE
 * + per-GPU register + one alloc + mmap) on a heavily contended host, which is
 * milliseconds in practice and orders of magnitude inside this budget.  The
 * risk we are trading against is a FALSE timeout killing a healthy isolate,
 * which is worse than the hang it prevents, so the value is deliberately far
 * more generous than the work it bounds rather than tuned close to it.
 */
#define NVKVM_ISO_SYNC_TIMEOUT_MS   30000u

/*
 * Re-check cadence inside a wait.  A waiter wakes this often to re-evaluate
 * "is my isolate still alive and still mine" so a teardown that happens while
 * it is parked is noticed promptly instead of at the full deadline.
 */
#define NVKVM_ISO_WAIT_SLICE_MS       250u

/*
 * Receive timeout on QEMU's end of the isolate socket (audit F2-1).
 *
 * The reader thread blocks in recv() for a blob whose length the STUB
 * declared; a stub that announces a payload and never sends it parked the
 * reader forever, and nvkvm_isolate_kill() then close()d the socket and
 * pthread_join()ed that thread — but close() does NOT wake a peer already
 * inside recv() on the same file description, so the join never returned (with
 * the BQL held, from the TX handler).  The kill path now shutdown()s first,
 * which does wake it; this timeout is the belt-and-braces so no recv on this
 * socket can block indefinitely even if shutdown is somehow not reached.
 *
 * 5 s rather than something tight: the follow-up param/aux blobs are sent
 * back-to-back with their header by the stub worker, so any legitimate gap is
 * sub-millisecond; the only cost of a large value is how long a wedged blob
 * read persists, and the only cost of a small one is a false teardown.
 */
#define NVKVM_ISO_SOCK_RCVTIMEO_MS   5000u

/*
 * Condvar clock.  Defaults to CLOCK_REALTIME (what pthread_cond_init(NULL)
 * uses); we switch to CLOCK_MONOTONIC when the platform allows, so that an NTP
 * step or a settimeofday() cannot either fire a deadline early or postpone it
 * indefinitely.  Set once, at table init, before any condvar is created.
 */
static clockid_t                 nvkvm_iso_clockid = CLOCK_REALTIME;
static pthread_condattr_t        nvkvm_iso_condattr;
static const pthread_condattr_t *nvkvm_iso_condattr_p;

static void nvkvm_iso_cond_attr_init(void)
{
	static bool done;
	if (done)
		return;
	done = true;
	if (pthread_condattr_init(&nvkvm_iso_condattr) == 0 &&
	    pthread_condattr_setclock(&nvkvm_iso_condattr,
				      CLOCK_MONOTONIC) == 0) {
		nvkvm_iso_clockid    = CLOCK_MONOTONIC;
		nvkvm_iso_condattr_p = &nvkvm_iso_condattr;
	}
}

static void nvkvm_iso_deadline(struct timespec *ts, unsigned ms)
{
	clock_gettime(nvkvm_iso_clockid, ts);
	ts->tv_sec  += (time_t)(ms / 1000u);
	ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_nsec -= 1000000000L;
		ts->tv_sec  += 1;
	}
}

static bool nvkvm_iso_deadline_passed(const struct timespec *ts)
{
	struct timespec now;
	clock_gettime(nvkvm_iso_clockid, &now);
	return now.tv_sec > ts->tv_sec ||
	       (now.tv_sec == ts->tv_sec && now.tv_nsec >= ts->tv_nsec);
}

/*
 * Declare an isolate dead from a WAITER (deadline expiry), as opposed to the
 * orderly nvkvm_isolate_kill() path.
 *
 * Deliberately does NOT close the socket: the reader thread is still using that
 * descriptor and closing it here would race a reused fd number (the C-1 class
 * of bug).  shutdown() wakes the reader (and any sender blocked in send()) and
 * leaves the fd valid; the reader then unwinds and wakes every other pending
 * caller with -ECONNRESET.  SIGKILL stops the stub from continuing to consume
 * the ring.  Slot teardown (waitpid, fd close, ring unmap) still happens in
 * nvkvm_isolate_kill() when the guest or session teardown gets there.
 */
static void nvkvm_isolate_declare_dead(struct nvkvm_isolate *iso,
				       const char *why)
{
	pid_t pid = 0;
	int   fd  = -1;

	pthread_mutex_lock(&iso->lock);
	if (iso->alive) {
		iso->alive = false;
		pid = iso->pid;
		fd  = iso->sock_fd;
	}
	pthread_mutex_unlock(&iso->lock);

	if (pid <= 0 && fd < 0)
		return;   /* somebody else already declared it */

	fprintf(stderr, "nvkvm: isolate %u declared dead: %s\n", iso->id, why);
	if (fd >= 0)
		shutdown(fd, SHUT_RDWR);
	if (pid > 0)
		kill(pid, SIGKILL);
}

/*
 * Bounded wait on one of the response slots.
 *
 * `mtx`/`cond`/`done` name the slot (shared sync_*, present_*, xiso_* or
 * loop_*); `mtx` must be held on entry and is still held on return.  Returns:
 *    0            the reader delivered a response (*done is true)
 *   -ENODEV       the isolate died / the slot was reused underneath us
 *   -ETIMEDOUT    timeout_ms elapsed with no response
 *
 * timeout_ms == 0 means "no overall deadline" — used only by ENTER_LOOP, whose
 * duration is bounded by the guest's ring traffic and is therefore legitimately
 * unbounded; it still wakes every slice to re-check liveness.
 */
static int nvkvm_iso_slot_wait(struct nvkvm_isolate *iso,
			       pthread_mutex_t *mtx, pthread_cond_t *cond,
			       const bool *done, unsigned timeout_ms)
{
	uint32_t id0;
	struct timespec end;

	/*
	 * A-14: id/in_use/alive are documented as protected by iso->lock, so
	 * both the snapshot here and the re-check below take it.  The lock
	 * order this establishes (mtx -> iso->lock) is the one the file already
	 * uses: nvkvm_iso_slot_wait_or_die() calls nvkvm_isolate_declare_dead()
	 * with `mtx` held, and that takes iso->lock.  Nothing anywhere takes a
	 * sync/present/xiso/loop slot lock while holding iso->lock, and this
	 * function is never called with mtx == &iso->lock, so there is no
	 * inversion.
	 */
	pthread_mutex_lock(&iso->lock);
	id0 = iso->id;
	pthread_mutex_unlock(&iso->lock);

	if (timeout_ms)
		nvkvm_iso_deadline(&end, timeout_ms);

	while (!*done) {
		struct timespec slice;

		nvkvm_iso_deadline(&slice, NVKVM_ISO_WAIT_SLICE_MS);
		if (timeout_ms && (slice.tv_sec > end.tv_sec ||
				   (slice.tv_sec == end.tv_sec &&
				    slice.tv_nsec > end.tv_nsec)))
			slice = end;
		pthread_cond_timedwait(cond, mtx, &slice);
		if (*done)
			break;
		/*
		 * Liveness re-check.  Under iso->lock (A-14): these fields are
		 * written under it by kill/declare_dead/create, so reading them
		 * bare was a data race whose torn or stale result decides
		 * whether a parked caller unwinds.  The reader thread also sets
		 * the slot on death, so this is the backstop for a reader that
		 * is itself wedged.  Once per NVKVM_ISO_WAIT_SLICE_MS, so the
		 * added contention on a lock whose contract is "held briefly"
		 * is nil.
		 */
		pthread_mutex_lock(&iso->lock);
		bool gone = !iso->in_use || iso->id != id0 || !iso->alive;
		pthread_mutex_unlock(&iso->lock);
		if (gone)
			return -ENODEV;
		if (timeout_ms && nvkvm_iso_deadline_passed(&end))
			return -ETIMEDOUT;
	}
	return 0;
}

/*
 * As above, but a deadline expiry is treated as isolate death rather than as
 * "wait a bit longer": a stub that has not answered a bounded command inside
 * NVKVM_ISO_SYNC_TIMEOUT_MS is not slow, it is not coming back, and every
 * other caller parked on it (plus, for the inline commands, the whole VMM) is
 * paying for it.  Called with `mtx` held; the shutdown/SIGKILL it performs
 * take no locks that can order against it.
 */
static int nvkvm_iso_slot_wait_or_die(struct nvkvm_isolate *iso,
				      pthread_mutex_t *mtx,
				      pthread_cond_t *cond,
				      const bool *done, unsigned timeout_ms,
				      const char *what)
{
	int rc = nvkvm_iso_slot_wait(iso, mtx, cond, done, timeout_ms);
	if (rc == -ETIMEDOUT)
		nvkvm_isolate_declare_dead(iso, what);
	return rc;
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

/*
 * Variant for OPEN_DEVICE: also carries the SCM_RIGHTS fd.
 *
 * R2-M1 follow-up: the reader's defensive close loop above closes any fd
 * attached to a response type that has no business carrying one -- but it
 * EXEMPTS the two types that do, and both of those hand the fd to a
 * single-slot field.  Overwriting that field without closing what was there
 * drops the previous fd on the floor inside QEMU, which is the same
 * fd-exhaustion DoS the close loop exists to prevent, reached by the two
 * doors it deliberately leaves open.  A stub does it by answering when
 * nobody asked: every unsolicited ISOLATE_RESP_OPEN_DEVICE (or
 * _PRESENT_EXPORT) with an fd attached leaks the last one, in a loop, until
 * the VMM is out of descriptors.  It also covers the honest race -- a
 * response that arrives after its waiter timed out leaves an fd in the slot
 * that nothing ever consumes.
 *
 * Close before overwrite, which is what nvkvm_present_egl.c:810 already does
 * for the same shape of single-slot fd handoff ("drop the frame the display
 * hasn't taken yet").  Guarded on >= 0 so the sentinel is never closed, and
 * done under the same lock as the store so a waiter cannot be reading the
 * field while we retire it.
 */
static void reader_signal_sync_open(struct nvkvm_isolate *iso, int err, int fd)
{
	pthread_mutex_lock(&iso->sync_lock);
	if (iso->sync_open_fd >= 0 && iso->sync_open_fd != fd)
		close(iso->sync_open_fd);
	iso->sync_error    = err;
	iso->sync_open_fd  = fd;
	iso->sync_done     = true;
	pthread_cond_signal(&iso->sync_cond);
	pthread_mutex_unlock(&iso->sync_lock);
}

/* PRESENT_EXPORT (#106): dedicated slot, carries the dma-buf SCM_RIGHTS fd.
 * Same close-before-overwrite as reader_signal_sync_open above; see there. */
static void reader_signal_present(struct nvkvm_isolate *iso, int err, int fd)
{
	pthread_mutex_lock(&iso->present_sync_lock);
	if (iso->present_fd >= 0 && iso->present_fd != fd)
		close(iso->present_fd);
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

/*
 * ENTER_LOOP (audit F3-1): dedicated slot, mirroring present/xiso.  Factored
 * out so the reader-exit path can wake a stranded pump too.
 */
static void reader_signal_loop(struct nvkvm_isolate *iso, int err, uint64_t head)
{
	pthread_mutex_lock(&iso->loop_sync_lock);
	iso->loop_error = err;
	iso->loop_head  = head;
	iso->loop_done  = true;
	pthread_cond_signal(&iso->loop_cond);
	pthread_mutex_unlock(&iso->loop_sync_lock);
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
		/*
		 * F2-1: the socket carries SO_RCVTIMEO now, so an idle reader
		 * wakes periodically instead of parking forever.  That is the
		 * point (a kill can no longer depend on close() waking us), but
		 * it means EAGAIN here is the NORMAL idle case, not an error —
		 * treat it as "nothing yet" and only unwind if the isolate has
		 * been torn down underneath us.  EINTR likewise.
		 */
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			      errno == EINTR)) {
			pthread_mutex_lock(&iso->lock);
			bool gone = !iso->alive || iso->sock_fd < 0;
			pthread_mutex_unlock(&iso->lock);
			if (gone)
				break;
			continue;
		}
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
			 *
			 * F2-1: param_size/aux_size are values the STUB chose,
			 * so this recv is a blocking wait on a length the other
			 * side declared.  SO_RCVTIMEO bounds it: a stub that
			 * announces a payload and never sends it now yields
			 * EAGAIN (n < 0) and we unwind through reader_exit,
			 * where every waiter is woken with -ECONNRESET, instead
			 * of parking here forever with a killer blocked on our
			 * pthread_join.
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

		case ISOLATE_RESP_LOOP_EXITED:
			/* F3-1: own slot — it must not clobber (or be clobbered
			 * by) a short TX-thread command sharing sync_*. */
			reader_signal_loop(iso, u.loop_exited.error,
					   u.loop_exited.head);
			break;

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
	/* …and any pending ENTER_LOOP pump (dedicated slot, F3-1). */
	reader_signal_loop(iso, -ECONNRESET, 0);

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

bool nvkvm_isolate_cfg_is_unconfined(const struct nvkvm_isolate_table *t)
{
	return (t->cfg.mode & NVKVM_ISO_LAYERS_ALL) == 0;
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

void nvkvm_isolate_table_init(struct nvkvm_isolate_table *t,
			      struct nvkvm_handle_table *handles)
{
	memset(t, 0, sizeof(*t));
	t->handles = handles;
	pthread_mutex_init(&t->lock, NULL);
	pthread_mutex_init(&t->nvkms_vblank_lock, NULL);
	/* F1-1: every condvar below is waited on with a deadline now, so pin
	 * them to CLOCK_MONOTONIC before creating any of them — a wall-clock
	 * step must not be able to fire a timeout early or defer it. */
	nvkvm_iso_cond_attr_init();
	nvkvm_isolate_cfg_resolve(&t->cfg, t->cfg_error, sizeof(t->cfg_error),
				  t->cfg_report, sizeof(t->cfg_report));
	t->next_id = 1;
	for (int i = 0; i < NVKVM_ISOLATE_MAX; i++) {
		struct nvkvm_isolate *iso = &t->isolates[i];
		/*
		 * EVERY descriptor field starts at -1, not at the 0 the memset
		 * above leaves behind.  0 is a perfectly good fd number, and the
		 * teardown paths in this file are all written as "if (fd >= 0)
		 * close(fd)" -- so a descriptor field that is still zero because
		 * nobody ever filled it reads as "I own fd 0" and gets closed.
		 * alloc_isolate_slot() does exactly that for sync_open_fd on the
		 * FIRST claim of every slot: it closed QEMU's fd 0 out from under
		 * whoever actually owned it.  In this file's own create path that
		 * is immediately fatal, because socketpair() runs before the slot
		 * is claimed -- once fd 0 is free, the very next isolate's
		 * command socket IS fd 0, and claiming the slot closes it, so the
		 * isolate is born with a dead socket (measured: sendmsg ->
		 * ENOTSOCK, then every later call -ENOENT once the reader's
		 * recvmsg on the same number fails).  In a real QEMU fd 0 belongs
		 * to whatever the process was started with -- a monitor chardev,
		 * a logfile, a migration stream -- and closing it hands that
		 * number to the next open(), which is a silent cross-wiring, not
		 * a crash.  Keep this list exhaustive when adding a field.
		 */
		iso->sock_fd      = -1;
		iso->sync_open_fd = -1;
		iso->ring_memfd   = -1;
		iso->ring_kvm_slot = -1;
		pthread_mutex_init(&iso->lock,       NULL);
		pthread_mutex_init(&iso->write_lock, NULL);
		pthread_mutex_init(&iso->sync_cmd_lock, NULL);
		pthread_mutex_init(&iso->sync_lock,  NULL);
		pthread_cond_init(&iso->sync_cond,   nvkvm_iso_condattr_p);
		pthread_mutex_init(&iso->present_lock,      NULL);
		pthread_mutex_init(&iso->present_sync_lock, NULL);
		pthread_cond_init(&iso->present_cond,       nvkvm_iso_condattr_p);
		pthread_mutex_init(&iso->xiso_lock,         NULL);
		pthread_mutex_init(&iso->xrm_lock,          NULL);
		pthread_mutex_init(&iso->xiso_sync_lock,    NULL);
		pthread_cond_init(&iso->xiso_cond,          nvkvm_iso_condattr_p);
		pthread_mutex_init(&iso->loop_lock,         NULL);
		pthread_mutex_init(&iso->loop_sync_lock,    NULL);
		pthread_cond_init(&iso->loop_cond,          nvkvm_iso_condattr_p);
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
		pthread_mutex_destroy(&iso->sync_cmd_lock);
		pthread_mutex_destroy(&iso->sync_lock);
		pthread_cond_destroy(&iso->sync_cond);
		pthread_mutex_destroy(&iso->present_lock);
		pthread_mutex_destroy(&iso->present_sync_lock);
		pthread_cond_destroy(&iso->present_cond);
		pthread_mutex_destroy(&iso->xiso_lock);
		pthread_mutex_destroy(&iso->xrm_lock);
		pthread_mutex_destroy(&iso->xiso_sync_lock);
		pthread_cond_destroy(&iso->xiso_cond);
		pthread_mutex_destroy(&iso->loop_lock);
		pthread_mutex_destroy(&iso->loop_sync_lock);
		pthread_cond_destroy(&iso->loop_cond);
	}
	pthread_mutex_destroy(&t->nvkms_vblank_lock);
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
			/*
			 * Retire, don't just forget: a response that raced its
			 * waiter's timeout can leave a live fd in either slot,
			 * and the old occupant's reader is long joined, so this
			 * is the last chance to close it.  Without this the
			 * close-before-overwrite above still leaks one fd per
			 * slot across a kill/create cycle.
			 */
			if (iso->sync_open_fd >= 0)
				close(iso->sync_open_fd);
			iso->sync_open_fd = -1;
			if (iso->present_fd >= 0)
				close(iso->present_fd);
			iso->present_fd   = -1;
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
			/*
			 * Reset the cross-isolate RM relay list.  It was the one
			 * piece of per-slot state nothing cleared, and the usual
			 * "is this still the isolate I mean" guard does not catch
			 * it: ids are handed out modulo NVKVM_ISOLATE_MAX and the
			 * slot index is `id % NVKVM_ISOLATE_MAX`, so a slot reused
			 * after a full lap of the id space comes back with the
			 * IDENTICAL id.  iso->id == isolate_id in
			 * nvkvm_isolate_note_foreign_handle() is therefore true for
			 * the new occupant, which inherits the dead one's list and
			 * is told "already relayed" for handles it has never seen.
			 * The relay is skipped, the fd never arrives, and
			 * cross-isolate sharing (CUDA VMM shareable handles / the
			 * NCCL SHM transport) is broken for that slot until the VM
			 * restarts.  A guest forces it deterministically with
			 * NVKVM_ISOLATE_MAX short-lived processes.
			 *
			 * Under xrm_lock because the broker runs on QEMU's pooled
			 * IOCTL workers, not on this thread.  No cycle: the two xrm
			 * helpers take xrm_lock and nothing else, and this is the
			 * only place that takes it under t->lock.
			 *
			 * Here rather than in nvkvm_isolate_kill(): this is the one
			 * site that claims a slot (the only `in_use = true` in the
			 * file), so clearing here makes "a fresh occupant starts
			 * with an empty relay list" structural rather than a
			 * property of whichever teardown path ran last.
			 */
			pthread_mutex_lock(&iso->xrm_lock);
			iso->xrm_n = 0;
			memset(iso->xrm_handles, 0, sizeof(iso->xrm_handles));
			iso->xrm_accepting = true;
			/* Same argument as the relay list: a slot reused after a
			 * full lap of the id space comes back with the identical
			 * id, so a fresh occupant must not inherit the dead one's
			 * vblank ownership records.  The VM-wide counter was
			 * already returned by nvkvm_isolate_kill(). */
			memset(iso->nvkms_vblank, 0, sizeof(iso->nvkms_vblank));
			iso->nvkms_vblank_seq = 1;
			pthread_mutex_unlock(&iso->xrm_lock);
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

	/*
	 * F2-1: bound every recv on QEMU's end.  Only sv[0] (ours) gets it —
	 * the stub's end is a separate socket and keeps its blocking reads, so
	 * this changes nothing about how the stub waits for work.  Without it
	 * the reader thread can sit in recv() for a blob length the stub
	 * declared and never sent, and a killer that close()s the fd and joins
	 * that thread never returns, because close() does not wake a peer
	 * already inside recv() on the same file description.  Best-effort:
	 * a kernel that refuses the option leaves us where we were, and the
	 * shutdown() in the kill path is the primary fix regardless.
	 */
	{
		struct timeval tv = {
			.tv_sec  = NVKVM_ISO_SOCK_RCVTIMEO_MS / 1000,
			.tv_usec = (NVKVM_ISO_SOCK_RCVTIMEO_MS % 1000) * 1000,
		};
		setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

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
	 * Make the NVIDIA render node reachable under the name the stub asks
	 * for, once per process.  In namespace mode the sandbox bind-mounts it;
	 * without a mount namespace (the default unprivileged-container case)
	 * only a symlink in the container's own /dev can do it.  Harmless when
	 * the node is already at renderD128, which is the single-GPU case.
	 */
	if (!use_ns) {
		static bool aliased;
		if (!aliased) {
			aliased = true;
			for (unsigned k = 0; k < 8; k++) {
				int rc = nvkvm_drm_node_alias(k);
				if (rc > 0)
					fprintf(stderr,
						"nvkvm: /dev/dri/renderD%u -> the NVIDIA node "
						"(host minor %d); the guest DRM node needs "
						"this name\n",
						128 + k, nvkvm_nvidia_render_minor(k));
				else if (rc < 0)
					fprintf(stderr,
						"nvkvm: warning: /dev/dri/renderD%u is taken by "
						"another device, so the guest's DRM render node "
						"%u will not open (CUDA and headless EGL are "
						"unaffected)\n", 128 + k, k);
				if (nvkvm_nvidia_render_minor(k) < 0)
					break;
			}
		}
	}

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
		/*
		 * MFD_ALLOW_SEALING so we can seal the image WRITE-shut below.
		 * A memfd is always O_RDWR -- there is no O_RDONLY memfd -- and
		 * this fd is dup2()'d to fd 3 for fexecve, which clears
		 * FD_CLOEXEC, and closefrom() starts at 4.  So the stub runs with
		 * a live read-write descriptor for its own text.  Without the
		 * seal it can mmap(PROT_WRITE, MAP_SHARED, 3, 0) a writable alias
		 * of its own running image and rewrite itself, which defeats the
		 * seccomp PROT_EXEC denial that bounds the severity of every
		 * other stub finding.  (The on-disk spawn path below opens the
		 * binary O_RDONLY, so only this path is affected.)
		 */
		int mfd = nvkvm_memfd_create("nvkvm_stub",
					     MFD_CLOEXEC | MFD_ALLOW_SEALING);
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
		/*
		 * Seal before fork/exec, while the image is still ours alone.
		 * F_SEAL_WRITE makes mmap(PROT_WRITE, MAP_SHARED) return EPERM
		 * and write() return EPERM for every holder of the fd, forever;
		 * SHRINK|GROW pin the length; SEAL stops the set being reopened.
		 * F_SEAL_WRITE requires no outstanding writable mapping -- we
		 * only ever write(2) this fd, never mmap it -- and does not
		 * affect execve.  Fail CLOSED: if the seal does not take, the
		 * writable alias exists and we must not spawn.
		 */
		if (fcntl(mfd, F_ADD_SEALS,
			  F_SEAL_SEAL | F_SEAL_SHRINK |
			  F_SEAL_GROW | F_SEAL_WRITE) < 0) {
			int e = errno;
			fprintf(stderr, "nvkvm: refusing to spawn isolate: "
				"could not seal stub memfd: %s\n", strerror(e));
			close(mfd);
			close(sv[0]);
			close(sv[1]);
			iso->in_use = false;
			return -e;
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
			/* Clear of stdio BEFORE stdin is rewired: see
			 * nvkvm_fd_clear_of_stdio().  A memfd handed back as
			 * fd 0 would otherwise be destroyed by the dup2 below
			 * and we would fexecve the command socket. */
			mfd = nvkvm_fd_clear_of_stdio(mfd);
			if (mfd < 0)
				_exit(127);
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
			/* DRM render nodes, opened while still privileged and
			 * parked for the stub (see NVKVM_DRM_FD).  Before the
			 * pivot/chroot, which take the real paths away. */
			/* Park in EVERY mode, not just uid-drop.  The stub
			 * addresses the node by descriptor NUMBER, so a mode
			 * that parked nothing left NVKVM_DRM_FD(0) pointing at
			 * whatever unrelated fd happened to sit there — measured:
			 * /dev/nvidiactl — and the stub dup'd that and answered
			 * every DRM ioctl with EINVAL.  See the --drm-fds argv
			 * below, which stops the stub guessing either way. */
			unsigned drm_n = nvkvm_child_park_drm_fds();
			/* Park the /proc dirfd for closefrom's fallback while
			 * /proc still resolves -- after the pivot below it does
			 * not.  See nvkvm_isolate_open_procfd(). */
			int procfd = nvkvm_isolate_open_procfd();
			/* Empty RO mount ns (parks /dev O_PATH at NVKVM_DEV_DIRFD). */
			if (use_ns && nvkvm_child_enter_mount_ns() < 0)
				_exit(126);
			/* uid+chroot: root at /dev, dirfd = the root (parks at
			 * NVKVM_DEV_DIRFD too).  Needs CAP_SYS_CHROOT, so it
			 * must precede both drops below. */
			if (use_chroot && nvkvm_iso_enter_chroot(NVKVM_DEV_DIRFD) < 0)
				_exit(124);
			{
				int keep = (use_ns || use_chroot)
					   ? NVKVM_DEV_DIRFD + 1 : 4;
				if (drm_n) {
					/* Keep the parked DRM fds.  When this mode
					 * parked no dirfd, close it here so raising
					 * the floor does not leak it. */
					if (!(use_ns || use_chroot))
						close(NVKVM_DEV_DIRFD);
					keep = NVKVM_DRM_FD(drm_n - 1) + 1;
				}
				if (nvkvm_isolate_closefrom(keep, procfd) < 0)
					_exit(126);
			}
			/* no_new_privs + bounding set, while CAP_SETPCAP is still
			 * held; then the uid drop (needs CAP_SETUID, which it then
			 * removes); then the rest of the cap teardown. */
			if (harden)
				nvkvm_drop_caps_pre();
			/* setgroups is left at its default "allow" for combined
			 * mode (see nvkvm_map_child_userns), so clearing the
			 * group list must succeed in every mode. */
			if (use_uid &&
			    nvkvm_iso_drop_privilege(run_uid, (gid_t)run_uid) != 0)
				_exit(125);
			if (harden)
				nvkvm_drop_caps_post();
			/* The stub applies its seccomp allowlist unless told
			 * otherwise.  argv is the mechanism the stub itself
			 * nominates for this ("Re-add via argv if a debug hatch
			 * is ever needed", src/stub/nvkvm_stub.c) — the env is
			 * deliberately cleared, and argv has the useful property
			 * of being visible in ps, so a stub running without
			 * seccomp cannot hide. */
			char drmarg[24];
			snprintf(drmarg, sizeof(drmarg), "--drm-fds=%u", drm_n);
			const char *argv[] = { "nvkvm_stub", NULL, NULL, NULL };
			int ai = 1;
			if (!use_seccomp)
				argv[ai++] = "--no-seccomp";
			argv[ai++] = drmarg;
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
			/* Clear of stdio BEFORE stdin is rewired: see
			 * nvkvm_fd_clear_of_stdio().  open() returns the lowest
			 * free fd, which is 0 when the parent has stdin closed. */
			binfd = nvkvm_fd_clear_of_stdio(binfd);
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
			/* DRM render nodes, opened while still privileged and
			 * parked for the stub (see NVKVM_DRM_FD).  Before the
			 * pivot/chroot, which take the real paths away. */
			/* Park in EVERY mode, not just uid-drop.  The stub
			 * addresses the node by descriptor NUMBER, so a mode
			 * that parked nothing left NVKVM_DRM_FD(0) pointing at
			 * whatever unrelated fd happened to sit there — measured:
			 * /dev/nvidiactl — and the stub dup'd that and answered
			 * every DRM ioctl with EINVAL.  See the --drm-fds argv
			 * below, which stops the stub guessing either way. */
			unsigned drm_n = nvkvm_child_park_drm_fds();
			/* Park the /proc dirfd for closefrom's fallback while
			 * /proc still resolves -- after the pivot below it does
			 * not.  See nvkvm_isolate_open_procfd(). */
			int procfd = nvkvm_isolate_open_procfd();
			if (use_ns && nvkvm_child_enter_mount_ns() < 0)
				_exit(126);
			if (use_chroot && nvkvm_iso_enter_chroot(NVKVM_DEV_DIRFD) < 0)
				_exit(124);
			{
				int keep = (use_ns || use_chroot)
					   ? NVKVM_DEV_DIRFD + 1 : 4;
				if (drm_n) {
					/* Keep the parked DRM fds.  When this mode
					 * parked no dirfd, close it here so raising
					 * the floor does not leak it. */
					if (!(use_ns || use_chroot))
						close(NVKVM_DEV_DIRFD);
					keep = NVKVM_DRM_FD(drm_n - 1) + 1;
				}
				if (nvkvm_isolate_closefrom(keep, procfd) < 0)
					_exit(126);
			}
			if (harden)
				nvkvm_drop_caps_pre();
			/* setgroups is left at its default "allow" for combined
			 * mode (see nvkvm_map_child_userns), so clearing the
			 * group list must succeed in every mode. */
			if (use_uid &&
			    nvkvm_iso_drop_privilege(run_uid, (gid_t)run_uid) != 0)
				_exit(125);
			if (harden)
				nvkvm_drop_caps_post();
			char drmarg[24];
			snprintf(drmarg, sizeof(drmarg), "--drm-fds=%u", drm_n);
			const char *argv[] = { "nvkvm_stub", NULL, NULL, NULL };
			int ai = 1;
			if (!use_seccomp)
				argv[ai++] = "--no-seccomp";
			argv[ai++] = drmarg;
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
			/* Log the full explanation once per QEMU process: every
			 * guest process that opens the GPU triggers another
			 * create, and repeating a six-line paragraph per attempt
			 * buries the rest of the log.  Measured: 12 identical
			 * copies from a single validate.sh run. */
			static bool clone_fail_explained;
			if ((e == EPERM || e == EINVAL || e == ENOSPC) &&
			    clone_fail_explained)
				fprintf(stderr,
				  "nvkvm: clone(CLONE_NEWUSER|...) failed again: "
				  "%s (isolate not created; see the first "
				  "occurrence above)\n", strerror(e));
			else if (e == EPERM || e == EINVAL || e == ENOSPC) {
				clone_fail_explained = true;
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
static void nvkvm_nvkms_vblank_release(struct nvkvm_isolate_table *t,
				       unsigned count);

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

	bool was_alive = iso->alive;
	int  peek_fd   = iso->sock_fd;
	iso->alive = false;
	pthread_mutex_unlock(&iso->lock);

	/*
	 * Stop pooled IOCTL workers reserving new foreign relays against a slot
	 * that is on its way out.  A reservation taken after the drain below
	 * would leave an isolate reference on a handle with no isolate left to
	 * release it, which pins the handle's fd open for the life of the VM.
	 * Existing reservations are drained further down, after the stub process
	 * has been reaped -- at that point no SCM_RIGHTS receiver can still be
	 * holding one.
	 */
	pthread_mutex_lock(&iso->xrm_lock);
	iso->xrm_accepting = false;
	pthread_mutex_unlock(&iso->xrm_lock);

	/*
	 * Audit C-1 (unchanged intent): the fd is claimed — snapshotted AND
	 * nulled — under write_lock, so an IOCTL_ON_ISOLATE running on a
	 * thread-pool worker either finishes its send first or sees -1 and
	 * skips, and exactly one caller ends up owning the descriptor to close.
	 *
	 * Audit F5-1: the ISOLATE_CMD_EXIT send used to happen a few lines up,
	 * under iso->lock, whose own contract says "held briefly; never during
	 * blocking I/O" — and a plain send() on a SEQPACKET socketpair whose
	 * peer has stopped reading blocks once the buffer fills, which is
	 * exactly the state a wedged stub is in.  It happens here instead:
	 * outside iso->lock and non-blocking.  It is best-effort in every sense
	 * — the shutdown() below delivers EOF to the stub's reader loop, which
	 * terminates it just as EXIT does, and the SIGKILL further down is the
	 * backstop.
	 *
	 * trylock first, because a sender already blocked in send() holds
	 * write_lock and will not release it until the shutdown we have not
	 * performed yet; taking it blocking here would be the very hang we are
	 * removing.  If we lose that race we claim the fd after the shutdown,
	 * where the blocking acquire is guaranteed to complete.
	 */
	int sock_fd = -1;
	if (pthread_mutex_trylock(&iso->write_lock) == 0) {
		sock_fd = iso->sock_fd;
		iso->sock_fd = -1;
		if (was_alive && sock_fd >= 0) {
			struct isolate_cmd_exit cmd = { .type = ISOLATE_CMD_EXIT };
			send(sock_fd, &cmd, sizeof(cmd),
			     MSG_NOSIGNAL | MSG_DONTWAIT);
		}
		pthread_mutex_unlock(&iso->write_lock);
	}

	/*
	 * Audit F2-1: shutdown BEFORE anything blocks on this isolate.
	 *
	 * close() does not wake a peer thread that is already inside recv() on
	 * the same file description, so the old sequence (close, then
	 * pthread_join the reader) deadlocked whenever the reader was parked in
	 * the blob recv for a length the stub had declared and never sent — with
	 * this function running on the TX thread under the BQL, i.e. wedging the
	 * whole VMM.  shutdown() does wake it, immediately, and additionally
	 * unblocks any sender stuck in send() on this socket, which is what
	 * makes the write_lock acquisition below guaranteed to complete.  An
	 * AF_UNIX shutdown does not discard what is already queued to the peer,
	 * so the EXIT above is still delivered before the stub sees EOF.  The fd
	 * stays open here on purpose; it is closed only after the join, so the
	 * reader can never touch a recycled descriptor.
	 */
	if (peek_fd >= 0)
		shutdown(peek_fd, SHUT_RDWR);

	if (sock_fd < 0) {
		/* trylock lost the race (or another caller already claimed the
		 * fd).  The shutdown has since unblocked whoever held it, so
		 * this acquire completes; a second -1 here just means somebody
		 * else owns the close. */
		pthread_mutex_lock(&iso->write_lock);
		sock_fd = iso->sock_fd;
		iso->sock_fd = -1;
		pthread_mutex_unlock(&iso->write_lock);
	}

	/*
	 * Join the reader thread: it has already signaled all pending IOCTL
	 * callers and the sync waiter (if any).  After join, no thread accesses
	 * iso's internals through the reader path.  Bounded by the shutdown
	 * above plus SO_RCVTIMEO, so this cannot be the hang it once was.
	 */
	if (iso->reader_started) {
		pthread_join(iso->reader_tid, NULL);
		iso->reader_started = false;
	}

	/* Safe only now: the reader is gone, so no thread can recv() on a
	 * descriptor number the kernel is free to hand out again. */
	if (sock_fd >= 0)
		close(sock_fd);

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
	 * Drain the cross-isolate relay list while the slot is still marked
	 * in_use, so alloc_isolate_slot() cannot hand it to a new occupant
	 * between the snapshot and the clear.  Every delivered relay took one
	 * isolate reference on the owner's handle; the isolate that held them is
	 * now dead, so they are ours to drop.  Entries with generation 0 are
	 * reservations whose delivery never completed -- no reference was taken
	 * for them, and the worker doing that relay owns the rollback.
	 */
	struct nvkvm_xrm_handle xrm[NVKVM_XRM_MAX];
	unsigned xrm_n;
	unsigned vblank_n = 0;
	pthread_mutex_lock(&iso->xrm_lock);
	xrm_n = iso->xrm_n;
	memcpy(xrm, iso->xrm_handles, xrm_n * sizeof(xrm[0]));
	iso->xrm_n = 0;
	/* The stub is dead, so every NVKMS vblank control it held is gone with
	 * its fds.  Return that many units to the VM-wide budget, or a guest
	 * exhausts it permanently by creating and killing isolates. */
	for (unsigned i = 0; i < NVKVM_NVKMS_VBLANK_MAX; i++) {
		if (iso->nvkms_vblank[i].reservation != 0)
			vblank_n++;
	}
	memset(iso->nvkms_vblank, 0, sizeof(iso->nvkms_vblank));
	pthread_mutex_unlock(&iso->xrm_lock);
	nvkvm_nvkms_vblank_release(t, vblank_n);
	if (t->handles) {
		for (unsigned i = 0; i < xrm_n; i++) {
			if (xrm[i].generation != 0)
				nvkvm_handle_unref_isolate_generation(
					t->handles, xrm[i].handle_id,
					xrm[i].generation);
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

	/*
	 * S-4: invalidate this isolate's entries in the console's imported-buffer
	 * cache.  Nothing invalidated on isolate death, so a dead compositor's
	 * EGLImage kept its dma-buf — and the VRAM behind it — pinned for the
	 * life of the VM, and its cache slot went on answering for whatever bo id
	 * the next isolate reused.
	 *
	 * Hooked HERE, in kill(), rather than at the individual call sites: this
	 * is the single choke point every isolate death funnels through — the
	 * NVKVM_REQ_KILL_ISOLATE handler, nvkvm_isolate_kill_session() below, and
	 * nvkvm_isolate_table_fini()'s teardown loop all land here — so the
	 * invariant "an isolate cannot die without its cache entries dying with
	 * it" is structural instead of a list of call sites that has to be kept
	 * in sync.  Two of those three had no hook at all until now.
	 *
	 * The KILL_ISOLATE handler also calls it (nvkvm_isolate_handlers.c),
	 * which now makes that path record the id twice.  Deliberately left
	 * alone: that file belongs to another change, and the duplicate is inert
	 * — forget_isolate() only appends to a dead-id list that the main loop
	 * drains on the next gfx_update, and the second reap of an already-
	 * invalidated entry is a no-op.  The undrained-frame close() is guarded
	 * by p->fd >= 0, so it cannot double-close either.
	 *
	 * Lock-safety (the whole point of this audit): called with NO isolate
	 * lock held, and forget_isolate() takes only the present context's own
	 * p->lock, which nothing in this file ever holds.  nvkvm_present_egl.c
	 * makes no call back into the isolate layer, so the two lock domains
	 * cannot form a cycle.  It performs no GL work either — it records the
	 * dead id and schedules a BH; the eglDestroyImageKHR/glDeleteTextures
	 * happen in nvkvm_present_reap_dead() on the main loop, where the GL
	 * context is current.  So this is safe on the TX thread and on the
	 * unrealize path alike.
	 *
	 * At device unrealize it is a no-op by construction, not by luck:
	 * virtio_nvgpu_device_unrealize() runs nvkvm_present_console_fini()
	 * (which NULLs nv->present_ctx before freeing it) before
	 * nvkvm_isolate_table_fini(), so forget_isolate() returns at its !p
	 * guard rather than touching freed memory.
	 */
	if (t->nv)
		nvkvm_present_forget_isolate((VirtIONvgpu *)t->nv, isolate_id);

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
	/* F3-1: the real one-at-a-time gate.  Unlike sync_lock this is held
	 * across the WHOLE round-trip and is not dropped by the cond wait, so a
	 * second sender cannot walk in, clear sync_done, and steal the reply
	 * meant for a parked predecessor. */
	pthread_mutex_lock(&iso->sync_cmd_lock);
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done  = false;
	iso->sync_error = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, cmd_buf, cmd_size);
	pthread_mutex_unlock(&iso->write_lock);

	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		pthread_mutex_unlock(&iso->sync_cmd_lock);
		return (int)sr;
	}

	int rc = nvkvm_iso_slot_wait_or_die(iso, &iso->sync_lock,
					    &iso->sync_cond, &iso->sync_done,
					    NVKVM_ISO_SYNC_TIMEOUT_MS,
					    "sync command never answered");
	int result = rc ? rc : iso->sync_error;
	pthread_mutex_unlock(&iso->sync_lock);
	pthread_mutex_unlock(&iso->sync_cmd_lock);
	return result;
}

/*
 * Like sync_send_recv but via sendmsg (for SCM_RIGHTS); returns sync_error.
 */
static int sync_sendmsg_recv(struct nvkvm_isolate *iso, struct msghdr *msg)
{
	pthread_mutex_lock(&iso->sync_cmd_lock);   /* F3-1, see sync_send_recv */
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done  = false;
	iso->sync_error = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_sendmsg_fd(iso->sock_fd, msg);
	pthread_mutex_unlock(&iso->write_lock);

	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		pthread_mutex_unlock(&iso->sync_cmd_lock);
		return (int)sr;
	}

	int rc = nvkvm_iso_slot_wait_or_die(iso, &iso->sync_lock,
					    &iso->sync_cond, &iso->sync_done,
					    NVKVM_ISO_SYNC_TIMEOUT_MS,
					    "sync fd-passing command never answered");
	int result = rc ? rc : iso->sync_error;
	pthread_mutex_unlock(&iso->sync_lock);
	pthread_mutex_unlock(&iso->sync_cmd_lock);
	return result;
}

/*
 * Like sync_send_recv but returns the MMAP retval on success.
 */
static int sync_send_recv_mmap(struct nvkvm_isolate *iso,
				const void *cmd_buf, size_t cmd_size)
{
	pthread_mutex_lock(&iso->sync_cmd_lock);   /* F3-1, see sync_send_recv */
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done        = false;
	iso->sync_error       = 0;
	iso->sync_mmap_retval = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, cmd_buf, cmd_size);
	pthread_mutex_unlock(&iso->write_lock);

	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		pthread_mutex_unlock(&iso->sync_cmd_lock);
		return (int)sr;
	}

	int rc = nvkvm_iso_slot_wait_or_die(iso, &iso->sync_lock,
					    &iso->sync_cond, &iso->sync_done,
					    NVKVM_ISO_SYNC_TIMEOUT_MS,
					    "MMAP never answered");
	int result = rc ? rc
		     : (iso->sync_error ? iso->sync_error : iso->sync_mmap_retval);
	pthread_mutex_unlock(&iso->sync_lock);
	pthread_mutex_unlock(&iso->sync_cmd_lock);
	return result;
}

/* ── Handle distribution ────────────────────────────────────────────────── */

/*
 * Cross-isolate RM export/import bookkeeping.  See nvkvm_isolate.h and
 * nvkvm_xrm_materialise() in nvkvm_isolate_handlers.c for what this is for.
 */
bool nvkvm_isolate_note_foreign_handle(struct nvkvm_isolate_table *t,
				       uint32_t isolate_id, uint32_t handle_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return true;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];
	bool present = true;

	pthread_mutex_lock(&iso->xrm_lock);
	if (iso->in_use && iso->id == isolate_id) {
		present = false;
		for (unsigned i = 0; i < iso->xrm_n; i++) {
			if (iso->xrm_handles[i].handle_id == handle_id) {
				present = true;
				break;
			}
		}
		if (!iso->xrm_accepting) {
			/* Teardown has started.  Decline rather than record a
			 * relay whose reference nothing will be left to drop. */
			present = true;
		} else if (!present) {
			if (iso->xrm_n < NVKVM_XRM_MAX)
				iso->xrm_handles[iso->xrm_n++] =
					(struct nvkvm_xrm_handle){
						.handle_id = handle_id,
					};
			else
				present = true;   /* full: decline, do not evict */
		}
	}
	pthread_mutex_unlock(&iso->xrm_lock);
	return present;
}

bool nvkvm_isolate_finalize_foreign_handle(struct nvkvm_isolate_table *t,
					   uint32_t isolate_id,
					   uint32_t handle_id,
					   uint64_t generation)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX || generation == 0)
		return false;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];
	bool found = false;

	pthread_mutex_lock(&iso->xrm_lock);
	if (iso->in_use && iso->id == isolate_id && iso->xrm_accepting) {
		for (unsigned i = 0; i < iso->xrm_n; i++) {
			if (iso->xrm_handles[i].handle_id == handle_id &&
			    iso->xrm_handles[i].generation == 0) {
				iso->xrm_handles[i].generation = generation;
				found = true;
				break;
			}
		}
	}
	pthread_mutex_unlock(&iso->xrm_lock);
	return found;
}

bool nvkvm_isolate_forget_foreign_handle(struct nvkvm_isolate_table *t,
					 uint32_t isolate_id, uint32_t handle_id,
					 uint64_t *generation_out)
{
	if (generation_out)
		*generation_out = 0;
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return false;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];
	uint64_t generation = 0;
	bool found = false;

	pthread_mutex_lock(&iso->xrm_lock);
	if (iso->in_use && iso->id == isolate_id) {
		for (unsigned i = 0; i < iso->xrm_n; i++) {
			if (iso->xrm_handles[i].handle_id == handle_id) {
				generation = iso->xrm_handles[i].generation;
				found = true;
				iso->xrm_handles[i] =
					iso->xrm_handles[--iso->xrm_n];
				break;
			}
		}
	}
	pthread_mutex_unlock(&iso->xrm_lock);
	if (generation_out)
		*generation_out = generation;
	return found;
}

/* ── NVKMS vblank semaphore quota ────────────────────────────────────────── */

static void nvkvm_nvkms_vblank_release(struct nvkvm_isolate_table *t,
				       unsigned count)
{
	if (count == 0)
		return;
	pthread_mutex_lock(&t->nvkms_vblank_lock);
	t->nvkms_vblank_total = count <= t->nvkms_vblank_total ?
		t->nvkms_vblank_total - count : 0;
	pthread_mutex_unlock(&t->nvkms_vblank_lock);
}

uint64_t nvkvm_isolate_nvkms_vblank_reserve(struct nvkvm_isolate_table *t,
					    uint32_t isolate_id,
					    uint32_t handle_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return 0;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];
	uint64_t reservation = 0;

	/*
	 * Take the VM-wide unit FIRST and unconditionally.  Checking a counter,
	 * forwarding the ioctl, and incrementing afterwards lets every
	 * concurrent worker read the same under-limit value and all succeed.
	 */
	pthread_mutex_lock(&t->nvkms_vblank_lock);
	if (t->nvkms_vblank_total >= NVKVM_NVKMS_VBLANK_MAX) {
		pthread_mutex_unlock(&t->nvkms_vblank_lock);
		return 0;
	}
	t->nvkms_vblank_total++;
	pthread_mutex_unlock(&t->nvkms_vblank_lock);

	pthread_mutex_lock(&iso->xrm_lock);
	if (iso->in_use && iso->id == isolate_id && iso->xrm_accepting) {
		for (unsigned i = 0; i < NVKVM_NVKMS_VBLANK_MAX; i++) {
			struct nvkvm_nvkms_vblank *v = &iso->nvkms_vblank[i];
			if (v->reservation != 0)
				continue;
			reservation = iso->nvkms_vblank_seq++;
			if (reservation == 0)
				reservation = iso->nvkms_vblank_seq++;
			*v = (struct nvkvm_nvkms_vblank){
				.reservation = reservation,
				.handle_id   = handle_id,
			};
			break;
		}
	}
	pthread_mutex_unlock(&iso->xrm_lock);
	if (reservation == 0)
		nvkvm_nvkms_vblank_release(t, 1);
	return reservation;
}

void nvkvm_isolate_nvkms_vblank_finish(struct nvkvm_isolate_table *t,
				       uint32_t isolate_id, uint64_t reservation,
				       bool success, uint32_t device_handle,
				       uint32_t disp_handle,
				       uint32_t control_handle)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX ||
	    reservation == 0)
		return;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];
	bool released = false;

	pthread_mutex_lock(&iso->xrm_lock);
	if (iso->in_use && iso->id == isolate_id) {
		for (unsigned i = 0; i < NVKVM_NVKMS_VBLANK_MAX; i++) {
			struct nvkvm_nvkms_vblank *v = &iso->nvkms_vblank[i];
			if (v->reservation != reservation)
				continue;
			if (!success || control_handle == 0) {
				/* Nothing was created host-side, or we could not
				 * read back what identifies it -- either way we
				 * could never retire this entry, so do not keep
				 * charging for it. */
				memset(v, 0, sizeof(*v));
				released = true;
			} else {
				v->device_handle  = device_handle;
				v->disp_handle    = disp_handle;
				v->control_handle = control_handle;
			}
			break;
		}
	}
	pthread_mutex_unlock(&iso->xrm_lock);
	if (released)
		nvkvm_nvkms_vblank_release(t, 1);
}

void nvkvm_isolate_nvkms_vblank_retire(struct nvkvm_isolate_table *t,
				       uint32_t isolate_id, uint32_t handle_id,
				       uint32_t device_handle,
				       uint32_t disp_handle,
				       uint32_t control_handle)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX ||
	    control_handle == 0)
		return;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];
	bool released = false;

	pthread_mutex_lock(&iso->xrm_lock);
	if (iso->in_use && iso->id == isolate_id) {
		for (unsigned i = 0; i < NVKVM_NVKMS_VBLANK_MAX; i++) {
			struct nvkvm_nvkms_vblank *v = &iso->nvkms_vblank[i];
			/* All four must match: a DISABLE naming someone else's
			 * control must not free this isolate's quota. */
			if (v->reservation != 0 && v->control_handle != 0 &&
			    v->handle_id == handle_id &&
			    v->device_handle == device_handle &&
			    v->disp_handle == disp_handle &&
			    v->control_handle == control_handle) {
				memset(v, 0, sizeof(*v));
				released = true;
				break;
			}
		}
	}
	pthread_mutex_unlock(&iso->xrm_lock);
	if (released)
		nvkvm_nvkms_vblank_release(t, 1);
}

void nvkvm_isolate_nvkms_vblank_purge_handle(struct nvkvm_isolate_table *t,
					     uint32_t isolate_id,
					     uint32_t handle_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];
	unsigned released = 0;

	pthread_mutex_lock(&iso->xrm_lock);
	if (iso->in_use && iso->id == isolate_id) {
		for (unsigned i = 0; i < NVKVM_NVKMS_VBLANK_MAX; i++) {
			if (iso->nvkms_vblank[i].reservation != 0 &&
			    iso->nvkms_vblank[i].handle_id == handle_id) {
				memset(&iso->nvkms_vblank[i], 0,
				       sizeof(iso->nvkms_vblank[i]));
				released++;
			}
		}
	}
	pthread_mutex_unlock(&iso->xrm_lock);
	nvkvm_nvkms_vblank_release(t, released);
}

int nvkvm_isolate_send_handle_generation(struct nvkvm_isolate_table *t,
					 struct nvkvm_handle_table *ht,
					 uint32_t isolate_id, uint32_t handle_id,
					 uint64_t *generation_out)
{
	if (generation_out)
		*generation_out = 0;
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);

	if (!valid)
		return -ENOENT;

	/*
	 * Never carry the handle table's raw fd INTEGER across the blocking
	 * send/ack round-trip below.  The old code read h->fd, dropped every
	 * lock, and only then did the sendmsg; a concurrent session teardown
	 * running on another thread can close that descriptor in that window,
	 * and the kernel is free to hand the same number straight back to an
	 * unrelated open() in QEMU.  The relay would then pass THAT file to the
	 * isolate over SCM_RIGHTS.  The refcount was also only taken after the
	 * send succeeded, so nothing stopped the close in the first place.
	 *
	 * Acquire a duplicate and the isolate reference together, under the
	 * handle-table lock, while identity still holds: the dup keeps this open
	 * file description alive until sendmsg has taken its own SCM_RIGHTS
	 * reference, and the refcount makes nvkvm_handle_close() return -EBUSY
	 * for the duration.  The generation is reported so the eventual release
	 * can name this incarnation of the slot and not a later one.
	 */
	int h_dev_id = 0;
	uint64_t generation = 0;
	int fd = nvkvm_handle_acquire_fd_ref_isolate(ht, handle_id, &h_dev_id,
						     &generation);
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
	/* Our own copy has done its job either way: on success the kernel holds
	 * an SCM_RIGHTS reference for the stub, on failure nothing does. */
	close(fd);
	if (ret == 0) {
		if (generation_out)
			*generation_out = generation;
	} else {
		/* Roll the reservation back, or a failed delivery pins the
		 * owner's fd open until the VM exits. */
		nvkvm_handle_unref_isolate_generation(ht, handle_id, generation);
	}
	return ret;
}

int nvkvm_isolate_send_handle(struct nvkvm_isolate_table *t,
			      struct nvkvm_handle_table *ht,
			      uint32_t isolate_id, uint32_t handle_id)
{
	return nvkvm_isolate_send_handle_generation(t, ht, isolate_id, handle_id,
						    NULL);
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
		nvkvm_window_restore_anon(qva, region);
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
				nvkvm_window_restore_anon(target, region);
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
	 * replies LOOP_EXITED (which the reader thread delivers via loop_cond).
	 * The caller runs on QEMU's thread pool, so a long loop does not stall
	 * the main loop.  Slow-path IOCTLs that arrive while the stub loops use
	 * the independent per-txn pending mechanism, not the sync slot.
	 *
	 * F3-1: this used to share the sync_* slot with the short TX-thread
	 * commands, and because it is BOTH long-lived and issued from a
	 * different thread it was the one command that made "two live commands
	 * on a one-command slot" actually reachable — a concurrent MMAP would
	 * clear sync_done under a parked pump and then collect whichever reply
	 * arrived first.  It now has its own slot, and loop_lock serialises
	 * ENTER_LOOP callers among themselves.
	 *
	 * The wait deliberately has NO overall deadline: the loop's lifetime is
	 * whatever the guest's ring traffic makes it, so any fixed budget would
	 * be a false-positive machine.  It is bounded instead by liveness — the
	 * slot wait re-checks in_use/id/alive every slice — which is safe here
	 * precisely because this caller does not hold the BQL.
	 */
	pthread_mutex_lock(&iso->loop_lock);
	pthread_mutex_lock(&iso->loop_sync_lock);
	iso->loop_done  = false;
	iso->loop_error = 0;
	iso->loop_head  = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, &cmd, sizeof(cmd));
	pthread_mutex_unlock(&iso->write_lock);
	if (sr < 0) {
		pthread_mutex_unlock(&iso->loop_sync_lock);
		pthread_mutex_unlock(&iso->loop_lock);
		return (int)sr;
	}

	int wrc = nvkvm_iso_slot_wait(iso, &iso->loop_sync_lock, &iso->loop_cond,
				      &iso->loop_done, 0 /* no deadline */);
	int err;
	if (wrc) {
		err = -ENODEV;                 /* torn down / reused while parked */
	} else {
		err = iso->loop_error;
		if (head_out)
			*head_out = iso->loop_head;
	}
	pthread_mutex_unlock(&iso->loop_sync_lock);
	pthread_mutex_unlock(&iso->loop_lock);
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
	pthread_mutex_lock(&iso->sync_cmd_lock);   /* F3-1, see sync_send_recv */
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done    = false;
	iso->sync_error   = 0;
	iso->sync_open_fd = -1;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, &cmd, sizeof(cmd));
	pthread_mutex_unlock(&iso->write_lock);
	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		pthread_mutex_unlock(&iso->sync_cmd_lock);
		return (int)sr;
	}

	int wrc = nvkvm_iso_slot_wait_or_die(iso, &iso->sync_lock,
					     &iso->sync_cond, &iso->sync_done,
					     NVKVM_ISO_SYNC_TIMEOUT_MS,
					     "OPEN_DEVICE never answered");
	int err = wrc ? wrc : iso->sync_error;
	int fd  = wrc ? -1  : iso->sync_open_fd;
	iso->sync_open_fd = -1;
	pthread_mutex_unlock(&iso->sync_lock);
	pthread_mutex_unlock(&iso->sync_cmd_lock);

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


/* ── PRESENT export timing (#106 hot path) ───────────────────────────────────
 *
 * present_mean in nvkvm_present_egl.c does NOT cover this.  It starts after the
 * slot handoff, on the consumer thread, so it times the import/readback and is
 * blind to the export round-trip that happens first on the virtio TX thread.
 * Anyone measuring the cost of the per-frame export with present_mean measures
 * the wrong thing and sees no change however much the export improves.
 *
 * Two numbers, because they answer different questions:
 *   lock_us  — time spent waiting for present_lock, i.e. queueing behind another
 *              frame's round-trip.
 *   rt_us    — the round-trip itself, send -> stub -> response.
 * Both are BQL-held (see the F4-1 note below), so their sum is vCPU stall time,
 * which is the number that actually matters.
 *
 * One-shot line on the first export so the cost is visible without a debug
 * build (same idea as the #106 proof); a periodic summary under
 * NVKVM_EXPORT_TIMING=1.
 */
static unsigned long nvkvm_exp_n;
static double nvkvm_exp_rt_us, nvkvm_exp_rt_max;
static double nvkvm_exp_lock_us, nvkvm_exp_lock_max;
static pthread_mutex_t nvkvm_exp_stats_lock = PTHREAD_MUTEX_INITIALIZER;

static bool nvkvm_export_timing(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("NVKVM_EXPORT_TIMING");
		on = (e && *e && *e != '0') ? 1 : 0;
	}
	return on == 1;
}

static double nvkvm_exp_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static void nvkvm_exp_record(double lock_us, double rt_us)
{
	unsigned long n;

	pthread_mutex_lock(&nvkvm_exp_stats_lock);
	nvkvm_exp_n++;
	n = nvkvm_exp_n;
	nvkvm_exp_rt_us   += rt_us;
	nvkvm_exp_lock_us += lock_us;
	if (rt_us > nvkvm_exp_rt_max) {
		nvkvm_exp_rt_max = rt_us;
	}
	if (lock_us > nvkvm_exp_lock_max) {
		nvkvm_exp_lock_max = lock_us;
	}
	double rt_mean   = nvkvm_exp_rt_us / n;
	double lock_mean = nvkvm_exp_lock_us / n;
	double rt_max = nvkvm_exp_rt_max, lock_max = nvkvm_exp_lock_max;
	pthread_mutex_unlock(&nvkvm_exp_stats_lock);

	if (n == 1) {
		fprintf(stderr,
			"nvkvm present #106: first PRESENT_EXPORT round-trip "
			"%.0f us (lock wait %.0f us).  This runs per flip on "
			"the TX thread under the BQL.\n", rt_us, lock_us);
	}
	if (nvkvm_export_timing() && (n % 300) == 0) {
		fprintf(stderr,
			"nvkvm export: n=%lu rt_mean=%.0fus rt_max=%.0fus "
			"lock_mean=%.0fus lock_max=%.0fus stall_mean=%.0fus\n",
			n, rt_mean, rt_max, lock_mean, lock_max,
			rt_mean + lock_mean);
	}
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
	double t_pre = nvkvm_exp_now_us();
	pthread_mutex_lock(&iso->present_lock);
	double t_locked = nvkvm_exp_now_us();
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

	/*
	 * F4-1: present_lock is held across this whole round-trip (by design —
	 * see the header), and this runs inline on the TX thread under the BQL,
	 * so an untimed wait here parked the VMM AND every later frame behind
	 * it.  Bounded now; RESIDUAL RISK: the BQL is still held for up to the
	 * timeout, which only moving present off the TX thread would fix.
	 */
	int wrc = nvkvm_iso_slot_wait_or_die(iso, &iso->present_sync_lock,
					     &iso->present_cond,
					     &iso->present_done,
					     NVKVM_ISO_SYNC_TIMEOUT_MS,
					     "PRESENT_EXPORT never answered");
	int err = wrc ? wrc : iso->present_err;
	int fd  = wrc ? -1  : iso->present_fd;
	iso->present_fd = -1;
	pthread_mutex_unlock(&iso->present_sync_lock);
	pthread_mutex_unlock(&iso->present_lock);
	nvkvm_exp_record(t_locked - t_pre, nvkvm_exp_now_us() - t_locked);

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

	/* F4-1: same shape as present — xiso_lock spans the round-trip and the
	 * broker runs inline on the TX thread under the BQL, so the wait must be
	 * bounded.  Same residual risk. */
	int wrc = nvkvm_iso_slot_wait_or_die(iso, &iso->xiso_sync_lock,
					     &iso->xiso_cond, &iso->xiso_done,
					     NVKVM_ISO_SYNC_TIMEOUT_MS,
					     "XISO_IMPORT never answered");
	int err = wrc ? wrc : iso->xiso_err;
	uint32_t gem = wrc ? 0 : iso->xiso_gem;
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
	if (ret == 0) {
		/* Closing the modeset fd destroys every vblank control opened
		 * through it, so the quota those controls held is free again. */
		nvkvm_isolate_nvkms_vblank_purge_handle(t, isolate_id, handle_id);
		/*
		 * If this handle got here as a cross-isolate relay, drop the
		 * record along with the reference -- and release exactly the
		 * generation the relay pinned.  Leaving the record behind meant
		 * the slot still owed a reference at teardown, and a bare
		 * unref would happily decrement whatever occupies the handle id
		 * now if it has been closed and reissued since.
		 */
		uint64_t generation = 0;
		bool foreign = nvkvm_isolate_forget_foreign_handle(
			t, isolate_id, handle_id, &generation);
		if (!foreign)
			nvkvm_handle_unref_isolate(ht, handle_id);
		else if (generation != 0)
			nvkvm_handle_unref_isolate_generation(ht, handle_id,
							      generation);
		/* foreign with generation 0: delivery never completed, so no
		 * reference was ever taken and the relay worker owns the
		 * rollback.  Dropping one here would underflow the count. */
	}
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
	 *
	 * F1-1: this is the ONE wait deliberately left without a deadline, and
	 * the reasons are that (a) its duration is legitimately unbounded — it
	 * is a real guest GPU ioctl, which may sit in the driver for as long as
	 * the guest's own work takes, so any budget here is a false-positive
	 * machine that would kill healthy isolates mid-compute, and (b) unlike
	 * the sync commands it does NOT hold the BQL: IOCTL_ON_ISOLATE is
	 * offloaded to QEMU's thread pool (see virtio_nvgpu.c), so a stuck one
	 * costs a pool thread, not the main loop.  Liveness still comes from the
	 * reader, which wakes every pending caller with -ECONNRESET when the
	 * socket dies — and the socket now dies promptly in every teardown path
	 * (shutdown + SO_RCVTIMEO).  RESIDUAL RISK: a stub that stays alive and
	 * answers nothing can still tie up thread-pool workers, one per
	 * in-flight ioctl.
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

	pthread_mutex_lock(&iso->sync_cmd_lock);   /* F3-1, see sync_send_recv */
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
		pthread_mutex_unlock(&iso->sync_cmd_lock);
		return (int)sr;
	}

	int wrc = nvkvm_iso_slot_wait_or_die(iso, &iso->sync_lock,
					     &iso->sync_cond, &iso->sync_done,
					     NVKVM_ISO_SYNC_TIMEOUT_MS,
					     "REALIZE_UVM_FD never answered");
	int err           = wrc ? wrc : iso->sync_error;
	uint64_t host_va  = wrc ? 0 : iso->sync_realize_host_va;
	uint64_t out_len  = wrc ? 0 : iso->sync_realize_length;
	uint64_t token    = wrc ? 0 : iso->sync_realize_token;
	uint32_t rm_st    = wrc ? 0 : iso->sync_realize_rm_status;
	pthread_mutex_unlock(&iso->sync_lock);
	pthread_mutex_unlock(&iso->sync_cmd_lock);

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
