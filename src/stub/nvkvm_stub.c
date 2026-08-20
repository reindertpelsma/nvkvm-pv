/*
 * nvkvm_stub.c — nvkvm isolate stub process (multi-threaded)
 *
 * Freestanding static binary — built with -nostdlib -static -fPIE and linked
 * against -lgcc only.  No libc, no pthread; all primitives come from
 * stub_freestanding.h (futex-based mutex/cond, raw syscall wrappers,
 * clone3 trampoline, tiny printf).  See audit C7.
 *
 * Threading model
 * ===============
 * One reader thread reads framed commands from the QEMU socket (fd 0).
 * Non-IOCTL commands (RECEIVE_FD, CLOSE_FD, MMAP, MUNMAP, POLL, UNPOLL, EXIT)
 * are dispatched inline on the reader thread — they are fast and non-blocking.
 * IOCTL commands are queued to a pool of NVKVM_STUB_WORKERS worker threads.
 * Each worker executes the ioctl and writes the response back with the echoed
 * txn_id so QEMU can match it to the waiting caller.
 *
 * All socket writes are protected by write_mutex.
 * The fd_table is protected by fd_mutex (workers and the reader share it).
 *
 * Self-relocation
 * ===============
 * Applies R_X86_64_RELATIVE entries from the ELF dynamic section before any
 * code that touches global data runs (constructor priority 101).
 */

#include <stdint.h>
#include <stddef.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <linux/futex.h>
#include <linux/elf.h>
#include <asm/unistd.h>

#include "stub_freestanding.h"
#include "../common/nvkvm_isolate_proto.h"
#include "../common/nvkvm_ring.h"
#include "../common/nvkvm_ring_ioctl.h"
#include "../common/nvkvm_abi.h"

/* fcntl(2) F_DUPFD_CLOEXEC — not pulled in by the freestanding headers. */
#define NVKVM_F_DUPFD_CLOEXEC 1030
/*
 * U-1: the ring path needs the same default-deny control allowlist QEMU
 * applies on the virtqueue path.  The table is generated data with no QEMU
 * dependencies, so both host-side components include it directly rather than
 * keeping two copies that can drift.
 */
#include "../qemu/nvkvm_ctrl_allowlist.h"

/* ── Constants we'd otherwise pull from libc headers ─────────────────────── */

/* From <asm-generic/errno-base.h> — the kernel UAPI errno values we use. */
#ifndef EPERM
#define EPERM     1
#define EINTR     4
#define EIO       5
#define EBADF     9
#define EAGAIN   11
#define EWOULDBLOCK EAGAIN
#define ENOMEM   12
#define EFAULT   14
#define EINVAL   22
#define ENODEV   19
#define ENOSYS   38
#define E2BIG     7
#endif
#ifndef ENOTSUP
#define ENOTSUP  95  /* EOPNOTSUPP */
#endif

/* From <asm-generic/fcntl.h> + <linux/fcntl.h>. */
#ifndef O_RDWR
#define O_RDWR    00000002
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#ifndef AT_FDCWD
#define AT_FDCWD  -100
#endif

/* From <asm-generic/mman-common.h> / <linux/mman.h>. */
#ifndef PROT_READ
#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#endif
#ifndef MAP_PRIVATE
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_GROWSDOWN  0x0100
#endif
#ifndef MAP_SHARED
#define MAP_SHARED     0x01
#endif
#define MAP_FAILED ((void *)-1L)

/* From <linux/eventfd.h>. */
#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   O_CLOEXEC
#define EFD_NONBLOCK  04000  /* O_NONBLOCK */

/* From <linux/sched.h> — CLONE_* flags. */
#define CLONE_VM       0x00000100
#define CLONE_FS       0x00000200
#define CLONE_FILES    0x00000400
#define CLONE_SIGHAND  0x00000800
#define CLONE_THREAD   0x00010000
#define CLONE_SYSVSEM  0x00040000

/* Signal numbers + sigaction shape (kernel ABI, not glibc-augmented). */
#define SIGSEGV     11
#define SIGUSR1     10
#define SA_SIGINFO  0x00000004
#define SA_RESTORER 0x04000000

/* STDIN_FILENO / STDERR_FILENO are libc macros; we'd rather not pull
 * <unistd.h>.  Define directly. */
#define STDIN_FD   0
#define STDERR_FD  2

/* prctl op: from <linux/prctl.h>. */
#define PR_SET_NO_NEW_PRIVS 38

/* socket cmsg / msghdr — kernel UAPI exposes the data but the userland
 * struct shapes are normally defined in <bits/socket.h>.  Hand-roll them. */
typedef unsigned int  socklen_t;
typedef long          ssize_t;
typedef long          off_t;

struct iovec {
	void  *iov_base;
	size_t iov_len;
};

struct msghdr {
	void           *msg_name;
	socklen_t       msg_namelen;
	struct iovec   *msg_iov;
	size_t          msg_iovlen;
	void           *msg_control;
	size_t          msg_controllen;
	int             msg_flags;
};

struct cmsghdr {
	size_t   cmsg_len;
	int      cmsg_level;
	int      cmsg_type;
};

#define SOL_SOCKET   1
#define SCM_RIGHTS   1

/* CMSG_* alignment helpers — same definitions glibc uses, derived from the
 * kernel ABI.  Aligns on sizeof(size_t) boundaries. */
#define _CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_LEN(len)   (_CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_SPACE(len) (_CMSG_ALIGN(sizeof(struct cmsghdr)) + _CMSG_ALIGN(len))
#define CMSG_DATA(cmsg) ((unsigned char *)((struct cmsghdr *)(cmsg) + 1))
#define CMSG_FIRSTHDR(mhdr) \
	((size_t)(mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? \
	 (struct cmsghdr *)(mhdr)->msg_control : (struct cmsghdr *)0)

/* siginfo_t + sigaction (kernel layout — what rt_sigaction actually expects).
 *
 * The kernel's struct sigaction is much smaller than glibc's libc-side one;
 * what we register is `struct kernel_sigaction` which has 4 fields. */
typedef struct {
	int       si_signo;
	int       si_errno;
	int       si_code;
	int       _pad0;
	/* Real siginfo_t has a union here covering ~112 bytes.  For SIGSEGV
	 * the field we care about is si_addr at offset 16 (after the three
	 * ints + 4-byte pad).  Use the kernel's _sifields._sigfault layout. */
	void     *si_addr;
	char      _pad1[112 - 16 - sizeof(void *)];
} siginfo_t;

typedef struct { unsigned long sig[1]; } sigset_t;

struct kernel_sigaction {
	void   (*sa_handler_fn)(int, siginfo_t *, void *);
	unsigned long sa_flags;
	void   (*sa_restorer)(void);
	sigset_t sa_mask;
};

/* ELF types for self-relocation come from <linux/elf.h> (included above);
 * the DT_/R_X86_64_RELATIVE constants live in <linux/elf.h> too. */
#ifdef NVKVM_STUB_EMBEDDED
#  ifndef R_X86_64_RELATIVE
#    define R_X86_64_RELATIVE 8
#  endif
#endif

/* sched_setaffinity etc. are not used.  We also need MAP_FAILED already defined
 * above.  poll.h-style structs unused in the stub (POLL handler is a stub). */

/*
 * UVM ioctl struct layouts.  We need just enough to find the embedded fd
 * field offset for each ioctl; full kernel headers aren't available here.
 */
struct nvkvm_stub_uvm_mm_initialize_params {
	int32_t  uvm_fd;       /* offset 0  */
	uint32_t rm_status;
};
struct nvkvm_stub_uvm_uuid { uint8_t b[16]; };
struct nvkvm_stub_uvm_register_gpu_vaspace_params {
	struct nvkvm_stub_uvm_uuid gpu_uuid; /* 16 */
	uint32_t rm_ctrl_fd;                 /* offset 16 */
	uint32_t h_client;
	uint32_t h_va_space;
	uint32_t rm_status;
};
struct nvkvm_stub_uvm_register_channel_params {
	struct nvkvm_stub_uvm_uuid gpu_uuid;
	uint32_t rm_ctrl_fd;                 /* offset 16 */
	uint32_t h_client;
	uint32_t h_channel;
	uint32_t rm_status;
	uint64_t base;
	uint64_t length;
};

/*
 * KVM ioctl number for KVM_SET_USER_MEMORY_REGION on x86_64.
 * From <linux/kvm.h>:
 *   _IOW(KVMIO=0xAE, 0x46, struct kvm_userspace_memory_region)
 *   struct is 32 bytes (slot+flags+gpa+size+userspace_addr).
 */

/*
 * UVM ioctl numbers we recognise for embedded-fd translation.
 * These mirror the values in src/abi/uvm.h.
 */
#define NVKVM_STUB_UVM_MM_INITIALIZE          75
#define NVKVM_STUB_UVM_REGISTER_GPU_VASPACE   25
#define NVKVM_STUB_UVM_REGISTER_CHANNEL       27
#define NVKVM_STUB_UVM_MAP_EXTERNAL_ALLOCATION 33

#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER  (1UL << 3)
#endif
#ifndef SECCOMP_FILTER_FLAG_TSYNC
#define SECCOMP_FILTER_FLAG_TSYNC         (1UL << 0)
#endif

/*
 * UVM file ownership work-around.
 *
 * The NVIDIA UVM driver's UVM_MM_INITIALIZE rejects (NV_ERR_INVALID_ARGUMENT)
 * when the file passed via uvm_fd was opened by a different mm than the
 * caller. Since QEMU opens /dev/nvidia-uvm and passes it via SCM_RIGHTS to
 * us, the file's owning mm is QEMU and our MM_INITIALIZE call is rejected.
 *
 * Fix: open /dev/nvidia-uvm twice in the stub itself (before seccomp), and
 * when QEMU sends a RECEIVE_FD with dev_id == NVKVM_DEV_UVM, drop the
 * SCM_RIGHTS fd and use one of our local opens instead.
 */
#define NVKVM_STUB_UVM_LOCAL_POOL_SIZE 2
static int  uvm_local_fds[NVKVM_STUB_UVM_LOCAL_POOL_SIZE];
static int  uvm_local_next_idx = 0;
static struct fs_mutex uvm_local_lock = FS_MUTEX_INIT;

/* ── Syscall wrappers (freestanding — return negative -errno) ───────────── */

static __attribute__((noreturn)) void stub_exit(int code)
{
	sc1(__NR_exit_group, code);
	__builtin_unreachable();
}

/* Freestanding memcpy — the compiler lowers variable-length __builtin_memcpy
 * to a memcpy() call.  no-tree-loop-distribute-patterns stops GCC from
 * recognising this byte loop as memcpy and emitting a self-call. */
__attribute__((used, optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}

static long stub_read(int fd, void *buf, size_t n)
{
	return sc3(__NR_read, fd, (long)buf, (long)n);
}

static long stub_write(int fd, const void *buf, size_t n)
{
	return sc3(__NR_write, fd, (long)buf, (long)n);
}

static long stub_recvmsg(int fd, struct msghdr *m, int fl)
{
	return sc3(__NR_recvmsg, fd, (long)m, fl);
}

static long stub_sendmsg(int fd, const struct msghdr *m, int fl)
{
	return sc3(__NR_sendmsg, fd, (long)m, fl);
}

/* #127 — minimal freestanding poll(2) for the os-event relay. */
struct stub_pollfd {
	int   fd;
	short events;
	short revents;
};
#define STUB_POLLIN   0x0001
#define STUB_POLLERR  0x0008
#define STUB_POLLHUP  0x0010
/* ppoll(fds, nfds, NULL timeout, NULL sigmask, 0) — blocks until an fd is ready
 * or a signal arrives (returns -EINTR). */
static long stub_ppoll(struct stub_pollfd *fds, unsigned long nfds)
{
	/* ppoll(fds, nfds, tmo=NULL, sigmask=NULL, sigsetsize=0); sc6 is a
	 * 6-arg syscall wrapper, so pad the unused 6th slot. */
	return sc6(__NR_ppoll, (long)fds, (long)nfds, 0, 0, 0, 0);
}

static long stub_openat(int dfd, const char *path, int flags)
{
	return sc3(__NR_openat, dfd, (long)path, flags);
}

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif
#ifndef O_PATH
#define O_PATH 0x200000
#endif

/*
 * Device-node opening that survives the empty mount namespace.  When the
 * isolate is hardened, QEMU passes an O_PATH handle to the host /dev at
 * NVKVM_DEV_DIRFD; we open nodes relative to it.  When un-hardened the fd is
 * not a directory and we fall back to the absolute /dev path.  Set once at
 * startup by stub_detect_dev_dirfd() (before pivot_root makes /dev vanish).
 */
static int g_dev_dirfd = -1;

static void stub_detect_dev_dirfd(void)
{
	/* openat(fd, ".") succeeds only if fd is a directory. */
	long r = sc3(__NR_openat, NVKVM_DEV_DIRFD, (long)".",
		     O_PATH | O_DIRECTORY);
	if (r >= 0) {
		sc1(__NR_close, r);
		g_dev_dirfd = NVKVM_DEV_DIRFD;
	}
}

/* Open /dev/<name> in a mount-ns-agnostic way (name is relative, no /dev/). */
static long stub_open_dev(const char *name, int flags)
{
	if (g_dev_dirfd >= 0)
		return stub_openat(g_dev_dirfd, name, flags);
	/* Un-hardened fallback: build "/dev/<name>". */
	char p[40];
	const char pre[] = "/dev/";
	unsigned i = 0;
	for (; i < sizeof(pre) - 1; i++)
		p[i] = pre[i];
	unsigned j = 0;
	while (name[j] && i < sizeof(p) - 1)
		p[i++] = name[j++];
	p[i] = 0;
	return stub_openat(AT_FDCWD, p, flags);
}

#ifndef __NR_eventfd2
#define __NR_eventfd2 290   /* x86-64 */
#endif

static int stub_eventfd2(unsigned int initval, int flags)
{
	return (int)sc2(__NR_eventfd2, initval, flags);
}

static long stub_ioctl(int fd, unsigned long req, void *arg)
{
	return sc3(__NR_ioctl, fd, (long)req, (long)arg);
}

static void *stub_mmap(void *a, size_t l, int p, int f, int fd, off_t o)
{
	long r = sc6(__NR_mmap, (long)a, (long)l, p, f, fd, (long)o);
	/* Kernel returns -errno in [-4095, -1] on failure; map to MAP_FAILED
	 * so callers can compare against it the same way they would with
	 * the libc mmap(2). */
	if ((unsigned long)r >= (unsigned long)-4095L)
		return MAP_FAILED;
	return (void *)r;
}

static long stub_munmap(void *a, size_t l)
{
	return sc2(__NR_munmap, (long)a, (long)l);
}

static long stub_close(int fd) { return sc1(__NR_close, fd); }

static long stub_prctl(int op, unsigned long a2, unsigned long a3,
		       unsigned long a4, unsigned long a5)
{
	return sc5(__NR_prctl, op, (long)a2, (long)a3, (long)a4, (long)a5);
}

static long stub_seccomp(unsigned int op, unsigned int fl, const void *arg)
{
	return sc3(__NR_seccomp, op, fl, (long)arg);
}

/* Restorer trampoline for rt_sigaction.  The kernel requires SA_RESTORER on
 * x86_64 — if absent, returning from a signal handler delivers SIGSEGV.
 * We point sa_restorer at this 4-byte stub: `mov $15, %eax; syscall`
 * (SYS_rt_sigreturn). */
__attribute__((naked,unused)) static void stub_sigreturn_trampoline(void)
{
	__asm__ volatile (
		"movl $15, %eax\n\t"   /* SYS_rt_sigreturn */
		"syscall\n\t");
}

static long stub_sigaction(int sig, const struct kernel_sigaction *act,
			   struct kernel_sigaction *oact)
{
	return sc4(__NR_rt_sigaction, sig, (long)act, (long)oact,
		   sizeof(sigset_t));
}

/* ── Constants ────────────────────────────────────────────────────────────── */

#define SOCK_FD          STDIN_FD
#define NVKVM_STUB_WORKERS   16
/*
 * Handle IDs in QEMU are a global monotonic counter that never resets, so
 * after a few thousand cumulative opens across multiple CUDA processes
 * within one VM boot they can exceed 4 K.  Sized to 64 K to outlast any
 * realistic workload before a VM restart.  Each entry is 4 bytes ⇒ 256 KB
 * per stub address-space, which is fine.  When the counter ever wraps
 * past this we'll redesign the lookup as a hash; until then, blow it up.
 */
#define MAX_HANDLES      65536
#define MAX_PARAM_SIZE   (256 * 1024)
#define MAX_INFLIGHT     64   /* max concurrent IOCTL jobs */

/* ── Mutex helpers (futex-based, see stub_freestanding.h) ───────────────── */

static struct fs_mutex write_mutex  = FS_MUTEX_INIT;
static struct fs_mutex fd_mutex     = FS_MUTEX_INIT;


/* RM_CONTROL commands with an embedded { u32 count@0; pad; NvP64 ptr@8 } list
 * the driver writes through. Returns one element's size in bytes (8 for the
 * GET_INFO family, 1 for the GET_CAPS family whose count is a byte length), or
 * 0 if none. MUST stay in sync with the guest nvkvm_ctrl_list_entry_size(). */
static uint32_t nvkvm_ctrl_list_entry_size(uint32_t cmd)
{
	switch (cmd) {
	case 0x00410110U: /* NV0041_CTRL_CMD_GET_SURFACE_INFO */
	case 0x00801104U: /* NV0080_CTRL_CMD_GR_GET_INFO */
	case 0x20800802U: /* NV2080_CTRL_CMD_BIOS_GET_INFO */
	case 0x20801201U: /* NV2080_CTRL_CMD_GR_GET_INFO */
	case 0x20801301U: /* NV2080_CTRL_CMD_FB_GET_INFO */
	case 0x20801802U: /* NV2080_CTRL_CMD_BUS_GET_INFO */
		return 8;
	case 0x20800123U: /* NV2080_CTRL_CMD_GPU_GET_ENGINES (engineList NvU32[]) */
	case 0x00800201U: /* NV0080_CTRL_CMD_GPU_GET_CLASSLIST (classList NvU32[]; NVENC) */
		return 4;
	case 0x00801102U: /* NV0080_CTRL_CMD_GR_GET_CAPS */
	case 0x00801301U: /* NV0080_CTRL_CMD_FB_GET_CAPS */
	case 0x00801401U: /* NV0080_CTRL_CMD_HOST_GET_CAPS */
	case 0x00801701U: /* NV0080_CTRL_CMD_FIFO_GET_CAPS */
	case 0x00801b01U: /* NV0080_CTRL_CMD_MSENC_GET_CAPS */
	case 0x00801c02U: /* NV0080_CTRL_CMD_BSP_GET_CAPS_V2 */
		return 1;
	}
	return 0;
}

/* ── Command-buffer ring (docs/design/command_buffer.md) ─────────────────────
 * QEMU mints a memfd holding the request + response SPSC rings and sends it
 * via SCM_RIGHTS (ISOLATE_CMD_SETUP_RING).  We mmap the same memfd MAP_SHARED;
 * the guest sees the same pages through a KVM memslot (Phase 4).  Phase 2 only
 * maps + self-tests it; Phase 3 spins a consumer thread on g_req_ring.
 */
static void              *g_ring_base;       /* base of the mmapped region    */
static uint64_t           g_ring_region_size;
static struct nvkvm_ring *g_req_ring;        /* guest→isolate (we consume)    */
static struct nvkvm_ring *g_resp_ring;       /* isolate→guest (we produce)    */
static uint32_t           g_ring_bytes;

/* ── Handle fd table ─────────────────────────────────────────────────────── */

static int handle_fds[MAX_HANDLES];

static void handle_table_init(void)
{
	for (int i = 0; i < MAX_HANDLES; i++)
		handle_fds[i] = -1;
}

static int handle_lookup(uint32_t id)
{
	if (id >= MAX_HANDLES) return -1;
	return handle_fds[id];
}

static void handle_store(uint32_t id, int fd)
{
	if (id < MAX_HANDLES) handle_fds[id] = fd;
}

/* ── #127 os-event poll set ──────────────────────────────────────────────────
 * The guest arms a poll on an os-event fd via ISOLATE_CMD_POLL; the main reader
 * loop ppoll()s the host fd alongside the control socket and sends
 * ISOLATE_RESP_POLL_EVENT when it fires (one-shot — the guest re-arms on its
 * next poll()).  ISOLATE_CMD_POLL/UNPOLL may be dispatched from the reader loop
 * or the ring drain-edge, so guard the table with poll_lock. */
#define NVKVM_POLL_MAX 256
static struct fs_mutex poll_lock = FS_MUTEX_INIT;
static int      poll_fds[NVKVM_POLL_MAX];
static uint32_t poll_handles[NVKVM_POLL_MAX];
static int      poll_n;

static void poll_arm(uint32_t handle_id)
{
	int fd = handle_lookup(handle_id);
	if (fd < 0) return;
	fs_mutex_lock(&poll_lock);
	for (int i = 0; i < poll_n; i++)
		if (poll_handles[i] == handle_id) { fs_mutex_unlock(&poll_lock); return; }
	if (poll_n < NVKVM_POLL_MAX) {
		poll_fds[poll_n]     = fd;
		poll_handles[poll_n] = handle_id;
		poll_n++;
	}
	fs_mutex_unlock(&poll_lock);
}

static void poll_disarm(uint32_t handle_id)
{
	fs_mutex_lock(&poll_lock);
	for (int i = 0; i < poll_n; i++)
		if (poll_handles[i] == handle_id) {
			poll_fds[i]     = poll_fds[poll_n - 1];
			poll_handles[i] = poll_handles[poll_n - 1];
			poll_n--;
			break;
		}
	fs_mutex_unlock(&poll_lock);
}

static void handle_remove(uint32_t id)
{
	if (id < MAX_HANDLES) {
		poll_disarm(id);   /* #127: drop a closed os-event fd from the poll set */
		if (handle_fds[id] >= 0) stub_close(handle_fds[id]);
		handle_fds[id] = -1;
	}
}

/* ── Socket I/O ───────────────────────────────────────────────────────────── */

static int send_full(const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
		long n = stub_write(SOCK_FD, p, len);
		/* A stray SIGUSR1 (txn interrupt, #73) can hit a worker mid-send;
		 * retry rather than corrupt the response framing. */
		if (n == -EINTR) continue;
		if (n <= 0) return -1;
		p += n; len -= (size_t)n;
	}
	return 0;
}

static int recv_full(void *buf, size_t len)
{
	char *p = buf;
	while (len > 0) {
		long n = stub_read(SOCK_FD, p, len);
		if (n == -EINTR) continue;
		if (n <= 0) return -1;
		p += n; len -= (size_t)n;
	}
	return 0;
}

static int locked_send(const void *buf, size_t len)
{
	fs_mutex_lock(&write_mutex);
	int r = send_full(buf, len);
	fs_mutex_unlock(&write_mutex);
	return r;
}

static int send_ok(void)
{
	struct isolate_resp_ok r = { .type = ISOLATE_RESP_OK };
	return locked_send(&r, sizeof(r));
}

static int send_error(int err)
{
	struct isolate_resp_error r = {
		.type = ISOLATE_RESP_ERROR, .err = err < 0 ? -err : err };
	return locked_send(&r, sizeof(r));
}

static int send_ring_ready(int error, uint64_t probe_seen)
{
	struct isolate_resp_ring_ready r = {
		.type       = ISOLATE_RESP_RING_READY,
		.error      = error,
		.probe_seen = probe_seen,
	};
	return locked_send(&r, sizeof(r));
}

/* ── SIGSEGV handler ──────────────────────────────────────────────────────── */

/*
 * Per-worker fault address slot indexed by worker id.
 *
 * Each worker gets a unique slot id in [0, NVKVM_STUB_WORKERS); the id is
 * stashed in a thread-local-ish way via a syscall-safe lookup: workers call
 * worker_self() which reads the gettid() syscall return and matches it
 * against worker_tids[].  Set once at worker spawn time.  This avoids glibc
 * TLS (no __thread, no tcbhead_t) — the stub has no TLS image.
 *
 * Slot 0 is reserved for the main/reader thread (it can also fault on
 * stub_ioctl).
 */
#define WORKER_SLOT_MAX (NVKVM_STUB_WORKERS + 1)
static volatile int       worker_tids[WORKER_SLOT_MAX];      /* tid → slot */
static volatile uint64_t  worker_fault_addr[WORKER_SLOT_MAX];

/*
 * txn currently executing in each worker's stub_ioctl (0 = idle).  Set by the
 * worker immediately before the ioctl and cleared immediately after, so the
 * reader thread can map an ISOLATE_CMD_INTERRUPT(target_txn) to the worker's
 * tid and post SIGUSR1, making the in-flight host ioctl return -EINTR (#73).
 */
static volatile uint32_t  worker_inflight_txn[WORKER_SLOT_MAX];

/* Our own pid (== tgid), cached before seccomp so tgkill needs no getpid. */
static int stub_pid;

static int stub_gettid(void)
{
	return (int)sc0(__NR_gettid);
}

static int stub_tgkill(int tgid, int tid, int sig)
{
	return (int)sc3(__NR_tgkill, tgid, tid, sig);
}

static int worker_self_slot(void)
{
	int tid = stub_gettid();
	for (int i = 0; i < WORKER_SLOT_MAX; i++)
		if (worker_tids[i] == tid)
			return i;
	return 0;  /* fall back to reader-thread slot if unregistered */
}

static void sigsegv_handler(int sig, siginfo_t *info, void *ctx)
{
	(void)sig; (void)ctx;
	int slot = worker_self_slot();
	worker_fault_addr[slot] = (uint64_t)(uintptr_t)info->si_addr;
	/*
	 * Audit R2-H1: a SIGSEGV here is a *stub-side* bad dereference (a bug in
	 * one of the embedded-pointer rewrites — the nvidia driver's own bad
	 * accesses return -EFAULT, they don't raise SIGSEGV).  Returning from
	 * this handler re-executes the faulting instruction → an infinite
	 * SIGSEGV loop that pins the worker and a host core (a DoS).  The normal
	 * forwarding path never faults (matmul/cuInit/7B are green), so instead
	 * of looping we terminate the isolate cleanly: SYS_exit_group is
	 * async-signal-safe, and QEMU's reader sees the dead socket and signals
	 * every pending caller -ECONNRESET.  One isolate dies; no host-core burn,
	 * no cross-tenant impact.
	 */
	stub_exit(139);  /* 128 + SIGSEGV */
}

static uint64_t get_fault_addr(void)
{
	return worker_fault_addr[worker_self_slot()];
}

static void clear_fault_addr(void)
{
	worker_fault_addr[worker_self_slot()] = 0;
}

/*
 * SIGUSR1 handler — deliberately empty.  Its only purpose is to interrupt a
 * blocking ioctl(2): registered WITHOUT SA_RESTART so the syscall returns
 * -EINTR instead of auto-restarting.  Posted by the reader thread (tgkill) to
 * the worker running an interrupted txn (#73).
 */
static void sigusr1_handler(int sig, siginfo_t *info, void *ctx)
{
	(void)sig; (void)info; (void)ctx;
}

/*
 * Find the worker currently executing target_txn and post SIGUSR1 to it.
 * Called on the reader thread for ISOLATE_CMD_INTERRUPT.  Best-effort: if no
 * worker holds the txn (already finished, or not yet entered the ioctl) we do
 * nothing — the normal IOCTL response path still delivers a result.
 */
static void interrupt_txn(uint32_t target_txn)
{
	if (target_txn == 0)
		return;
	for (int i = 1; i < WORKER_SLOT_MAX; i++) {
		if (worker_inflight_txn[i] == target_txn) {
			int tid = worker_tids[i];
			if (tid > 0)
				stub_tgkill(stub_pid, tid, SIGUSR1);
			return;
		}
	}
}

/* ── Thread pool ──────────────────────────────────────────────────────────── */

struct ioctl_job {
	uint32_t txn_id;
	uint32_t handle_id;
	uint32_t cmd;
	uint32_t flags;
	uint32_t abi_profile;   /* #81: host driver ABI id (version-variant offsets) */
	/* param and aux blobs are malloc'd; worker frees them */
	void    *param_buf;
	uint32_t param_size;
	void    *aux_buf;
	uint32_t aux_size;
	int      valid;   /* 1 = slot occupied */
};

static void *blob_alloc(size_t size);

static struct ioctl_job   job_queue[MAX_INFLIGHT];
static struct fs_mutex    queue_mutex = FS_MUTEX_INIT;
static struct fs_cond     queue_cond  = FS_COND_INIT;
static volatile int       stub_exiting = 0;

static void job_queue_init(void)
{
	for (int i = 0; i < MAX_INFLIGHT; i++)
		job_queue[i].valid = 0;
}

static void enqueue_job(const struct ioctl_job *job)
{
	fs_mutex_lock(&queue_mutex);
	for (int i = 0; i < MAX_INFLIGHT; i++) {
		if (!job_queue[i].valid) {
			job_queue[i] = *job;
			job_queue[i].valid = 1;
			fs_cond_signal(&queue_cond);
			fs_mutex_unlock(&queue_mutex);
			return;
		}
	}
	fs_mutex_unlock(&queue_mutex);

	/*
	 * Queue full.  NEVER drop silently — a dropped IOCTL job means no
	 * ISOLATE_RESP_IOCTL is ever sent, so the guest blocks forever in
	 * wait_for_completion (uninterruptible D state, wedging the GPU
	 * context).  Send an explicit error response so the caller fails
	 * cleanly, and free the job's blobs (the worker would have freed them).
	 */
	struct isolate_resp_ioctl resp = {
		.type   = ISOLATE_RESP_IOCTL,
		.txn_id = job->txn_id,
		.retval = -ENOMEM,
	};
	locked_send(&resp, sizeof(resp));
	if (job->param_buf)
		stub_munmap(job->param_buf,
			    (job->param_size + 4095) & ~4095UL);
	if (job->aux_buf)
		stub_munmap(job->aux_buf,
			    (job->aux_size + 4095) & ~4095UL);
}

static int dequeue_job(struct ioctl_job *out)
{
	fs_mutex_lock(&queue_mutex);
	while (!stub_exiting) {
		for (int i = 0; i < MAX_INFLIGHT; i++) {
			if (job_queue[i].valid) {
				*out = job_queue[i];
				job_queue[i].valid = 0;
				fs_mutex_unlock(&queue_mutex);
				return 1;
			}
		}
		fs_cond_wait(&queue_cond, &queue_mutex);
	}
	fs_mutex_unlock(&queue_mutex);
	return 0;
}

/*
 * U-2/U-4 helper: clear a pointer field inside the aux blob, fail-closed.
 *
 * Every per-command rewrite below is guarded by a count or size the GUEST
 * supplied.  When such a guard does not fire the field must end up 0, never
 * whatever the guest wrote — the driver walks these as user pointers
 * (embedded_param_copy.c) and copies in AND out through them.  The write is
 * bounded by aux_size so a short blob is partially cleared rather than
 * overrun: a partly-guest-controlled pointer is still an attacker-influenced
 * pointer, so clear whatever of it exists.
 */
static void aux_clear_ptr(void *aux_buf, uint32_t aux_size, uint32_t off)
{
	if (!aux_buf || off >= aux_size)
		return;
	uint32_t n = aux_size - off;
	if (n > 8)
		n = 8;
	uint64_t zero = 0;
	__builtin_memcpy((char *)aux_buf + off, &zero, n);
}

static void worker_thread(void *arg)
{
	/* arg = (void *)(uintptr_t)(slot_id + 1) — non-zero so we can
	 * distinguish from a NULL/default.  Register our slot ourselves so
	 * SIGSEGV from the very first ioctl correctly routes to our slot
	 * (the parent's worker_tids[] write may race the first ioctl call). */
	int slot = (int)(uintptr_t)arg;
	if (slot > 0 && slot < WORKER_SLOT_MAX)
		worker_tids[slot] = stub_gettid();
	struct ioctl_job job;

	while (dequeue_job(&job)) {
		fs_mutex_lock(&fd_mutex);
		int fd = handle_lookup(job.handle_id);
		fs_mutex_unlock(&fd_mutex);

		struct isolate_resp_ioctl resp = {
			.type     = ISOLATE_RESP_IOCTL,
			.txn_id   = job.txn_id,
		};

		if (fd < 0) {
			resp.retval = -EBADF;
			goto send_resp;
		}

		/*
		 * Wire aux buffer pointer into param blob.
		 *
		 * RM_CONTROL (nvos54, 32 bytes) and RM_ALLOC (nvos21 32 bytes,
		 * nvos64 48 bytes) both have their embedded pointer field at
		 * offset 16 (after four 4-byte handles/integers).  The guest
		 * zeroes this field and puts the secondary buffer in the aux
		 * slot; we restore the host-accessible address here so the
		 * driver can dereference it.
		 */
		unsigned job_type = (job.cmd >> 8) & 0xff;
		unsigned job_nr   = job.cmd & 0xff;
		/* Embedded ptr at offset 8 (not 16): the NVKMS wrapper, the DRM
		 * SEMSURF_FENCE_CTX_CREATE (type 'd', nr 0x54), DRM
		 * GEM_EXPORT_NVKMS_MEMORY (type 'd', nr 0x49, #110) and DRM
		 * GEM_IMPORT_NVKMS_MEMORY (type 'd', nr 0x41, the dma-buf EXPORT
		 * half of #110) all carry their single user ptr
		 * (nvkms_params_ptr) there. */
		/*
		 * U-2 (docs/internal/audit-guest-pointers.md) — FAIL CLOSED.
		 *
		 * Both rewrites below used to be gated on `job.aux_size > 0`, a
		 * value that arrives straight off the virtqueue from the guest
		 * (virtio_nvgpu.c:900-922).  A guest that sent aux_size == 0 fell
		 * out of BOTH branches and nothing wrote the pointer field at
		 * all — its own 8 bytes reached stub_ioctl() and the driver
		 * verbatim.  Those fields are NVOS54.params (control.c:262
		 * copy_from_user AND copy_to_user, paramsSize bytes),
		 * NVOS21/NVOS64.pAllocParms, and NvKmsIoctlParams.address on the
		 * host-global /dev/nvidia-modeset — i.e. an arbitrary read and
		 * write at a guest-named address inside the isolate.
		 * aux_size == 0 happens on the ordinary live path (measured: 11
		 * RM_CONTROL and 24 RM_ALLOC calls in one CUDA run), so this was
		 * not a theoretical corner.
		 *
		 * Decide the pointer's OFFSET from the cmd — which selects the
		 * struct layout and which a guest cannot use to skip the write —
		 * then write either the aux pointer or an explicit 0.  Never
		 * leave the guest's bytes in a pointer field.  The offset-16
		 * zeroing is scoped to the two NRs whose offset 16 is definitely
		 * a pointer (RM_CONTROL 0x2a, RM_ALLOC 0x2b); other ioctls keep
		 * real data there (e.g. NVOS33.offset) and are left alone.
		 */
		int ptr_off = -1;
		if ((job.cmd == NVKVM_NVKMS_IOCTL_CMD &&
		     job.param_size >= NVKVM_NVKMS_PARAMS_SIZE) ||
		    (job_type == 'd' &&
		     (job_nr == 0x54 || job_nr == 0x49 || job_nr == 0x41) &&
		     job.param_size >= 16)) {
			ptr_off = NVKVM_NVKMS_ADDR_OFF;
		} else if (job_type == 'F' && (job_nr == 0x2a || job_nr == 0x2b) &&
			   job.param_size >= 24) {
			ptr_off = 16;
		} else if (job.aux_size > 0 && job.param_size >= 24) {
			/* Unchanged legacy generic case: some other cmd that
			 * shipped an aux blob.  Only ever reached with
			 * aux_size > 0, so behaviour here is exactly as before. */
			ptr_off = 16;
		}
		if (ptr_off >= 0) {
			uint64_t ptr_val = (job.aux_size > 0)
				? (uint64_t)(uintptr_t)job.aux_buf : 0;
			__builtin_memcpy((char *)job.param_buf + ptr_off,
					 &ptr_val, sizeof(uint64_t));
		}

		/*
		 * NVKMS REGISTER_SURFACE (sub-cmd 17): the inner params carry up
		 * to 3 plane fds (useFd=TRUE) holding our handle_ids; resolve each
		 * to the stub's local fd so NVKMS dups the real memory object.
		 * Restore the handle_ids after the ioctl (never leak a stub fd).
		 */
		int     regsurf_off[NVKVM_NVKMS_MAX_PLANES];
		int32_t regsurf_hid[NVKVM_NVKMS_MAX_PLANES];
		int     regsurf_n = 0;
		if (job.cmd == NVKVM_NVKMS_IOCTL_CMD && job.param_size >= 4 &&
		    job.aux_size >= NVKVM_NVKMS_REGSURF_PLANE0_OFF +
				    NVKVM_NVKMS_MAX_PLANES *
				    NVKVM_NVKMS_REGSURF_PLANE_STRIDE) {
			uint32_t subcmd;
			__builtin_memcpy(&subcmd, job.param_buf, sizeof(subcmd));
			if (subcmd == NVKVM_NVKMS_CMD_REGISTER_SURFACE &&
			    *((unsigned char *)job.aux_buf +
			      NVKVM_NVKMS_REGSURF_USEFD_OFF)) {
				for (unsigned i = 0; i < NVKVM_NVKMS_MAX_PLANES; i++) {
					unsigned off = NVKVM_NVKMS_REGSURF_PLANE0_OFF +
						       i * NVKVM_NVKMS_REGSURF_PLANE_STRIDE;
					int32_t hid;
					__builtin_memcpy(&hid, (char *)job.aux_buf + off,
							 sizeof(hid));
					if (hid <= 0)
						continue;
					int lfd = handle_lookup((uint32_t)hid);
					if (lfd >= 0) {
						int32_t lfd32 = lfd;
						__builtin_memcpy((char *)job.aux_buf + off,
								 &lfd32, sizeof(lfd32));
						regsurf_off[regsurf_n] = (int)off;
						regsurf_hid[regsurf_n] = hid;
						regsurf_n++;
					}
				}
			}
		}

		/*
		 * DRM GEM_EXPORT_NVKMS_MEMORY (type 'd', nr 0x49) and
		 * GEM_IMPORT_NVKMS_MEMORY (type 'd', nr 0x41), #110: the aux blob
		 * starts with { int memFd } at offset 0, carrying our handle_id;
		 * map it to the stub's local fd so the kernel exports the bo's RM
		 * memory onto a real fd in this process (0x49, exactly the
		 * EXPORT_OBJECT_TO_FD path), or finds the RM object parked on it
		 * (0x41, the dma-buf export half).  Restore the handle_id after.
		 */
		int     drm_export_fd_off = -1;
		int32_t drm_export_fd_hid = 0;
		if (job_type == 'd' && (job_nr == 0x49 || job_nr == 0x41) &&
		    job.aux_size >= sizeof(int32_t)) {
			int32_t hid;
			__builtin_memcpy(&hid, job.aux_buf, sizeof(hid));
			if (hid > 0) {
				int lfd = handle_lookup((uint32_t)hid);
				if (lfd >= 0) {
					int32_t lfd32 = lfd;
					__builtin_memcpy(job.aux_buf, &lfd32,
							 sizeof(lfd32));
					drm_export_fd_off = 0;
					drm_export_fd_hid = hid;
				}
			}
		}

		/*
		 * GET_BUILD_VERSION (inner cmd=0x101) has three embedded string
		 * pointer fields (p_driver_version_buffer, p_version_buffer,
		 * p_title_buffer) at aux_buf offsets 8, 16, 24.  The guest fills
		 * these with guest-VA buffers that are not valid in the stub's
		 * address space; the host driver calls copy_to_user with them,
		 * fails silently, and leaves changelist_number=0, which causes
		 * cuInit to return CUDA_ERROR_SYSTEM_NOT_READY.
		 *
		 * Fix: allocate stub-local string buffers and redirect the
		 * pointer fields.  After the ioctl, zero them before sending the
		 * aux_buf back so the guest cannot see stub VAs.
		 */
		uint32_t str_sz = 0;
		uint32_t info_list_size = 0; /* if non-zero, info_list pointer must be re-zeroed after the ioctl */
		uint32_t info_list_base = 0; /* base offset of info_list area in aux_buf */
		/* EXPORT_OBJECT_TO_FD (inner ctrl 0x3d05): handle_id→local fd at
		 * aux offset 16; restore the handle_id after the ioctl. */
		int      export_fd_off   = -1;
		int32_t  export_fd_saved = 0;
		if ((job.cmd & 0xff) == 0x2a &&        /* NV_ESC_RM_CONTROL */
		    job.aux_size > 0 && job.param_size >= 12) {
			uint32_t inner_cmd;
			__builtin_memcpy(&inner_cmd,
					 (char *)job.param_buf + 8,
					 sizeof(uint32_t));
			/* InfoList family (NV2080_CTRL_CMD_GR_GET_INFO etc.):
			 * aux_buf layout is [base_params][list_size*8 bytes of list].
			 * The base params has list_size at offset 0 and the info_list
			 * pointer at offset 8 (currently zero — guest cleared it). We
			 * point info_list at the extension area so the host driver
			 * writes into our own memory. After the ioctl we zero the
			 * pointer again so we don't leak a host VA back to the guest. */
			uint32_t list_esz = nvkvm_ctrl_list_entry_size(inner_cmd);
			if (list_esz && job.aux_size >= 4) {
				/* GET_INFO family → 8-byte entries; GET_CAPS family
				 * → 1-byte (the count is a byte length). Mirrors the
				 * guest nvkvm_ctrl_list_entry_size(). */
				uint32_t ls = 0;
				__builtin_memcpy(&ls, job.aux_buf, sizeof(uint32_t));
				/* base_size is whatever the guest sent before the
				 * extension; we recover it as aux_size - ls*esz. */
				int wired = 0;
				if (ls > 0 && (size_t)ls * list_esz < job.aux_size) {
					uint32_t base = (uint32_t)(job.aux_size - (size_t)ls * list_esz);
					if (base >= 16) {
						uint64_t list_va =
							(uint64_t)(uintptr_t)
							((char *)job.aux_buf + base);
						__builtin_memcpy((char *)job.aux_buf + 8,
								 &list_va, 8);
						info_list_size = ls;
						info_list_base = base;
						wired = 1;
					}
				}
				/*
				 * U-4 — FAIL CLOSED.  `ls` is the count the GUEST
				 * wrote at aux_buf+0 and job.aux_size is the length
				 * the GUEST declared, so the guard above is a test
				 * on guest-supplied values.  Choosing ls such that
				 * ls*esz >= aux_size (or base < 16) used to skip the
				 * rewrite and LEAVE the guest's own 8 bytes at
				 * aux_buf+8 — which is exactly the pointer
				 * embedded_param_copy.c walks: GR_GET_INFO copies
				 * ls*8 bytes IN and OUT at that address with no
				 * SKIP_COPYIN, i.e. an arbitrary read AND write in
				 * the isolate.  Zero it instead: the driver then
				 * gets a NULL list pointer and fails the call.
				 */
				if (!wired)
					aux_clear_ptr(job.aux_buf, job.aux_size, 8);
			}
			if (inner_cmd == 0x00003d05U &&
			    job.aux_size >= 20) {
				/* NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD:
				 * frontend fd at aux offset 16 carries a handle_id
				 * (guest translated it); map to our local fd so the
				 * kernel associates the object with a real fd in this
				 * process.  Restore the handle_id after the ioctl. */
				int32_t hid;
				__builtin_memcpy(&hid,
						 (char *)job.aux_buf + 16,
						 sizeof(hid));
				export_fd_saved = hid;
				int lfd = (hid > 0) ? handle_lookup((uint32_t)hid) : -1;
				if (lfd >= 0) {
					int32_t lfd32 = lfd;
					__builtin_memcpy((char *)job.aux_buf + 16,
							 &lfd32, sizeof(lfd32));
					export_fd_off = 16;
				}
			}
			if (inner_cmd == 0x00003d06U &&
			    job.aux_size >= 4) {
				/* NV0000_CTRL_CMD_OS_UNIX_IMPORT_OBJECT_FROM_FD
				 * (#110): the source nv-export fd is at aux offset
				 * 0 and carries a handle_id; map it to our local fd
				 * so the kernel imports the object into the caller's
				 * RM client.  Reuse export_fd_off/saved (restore
				 * writes the handle_id back at that offset). */
				int32_t hid;
				__builtin_memcpy(&hid, job.aux_buf, sizeof(hid));
				export_fd_saved = hid;
				int lfd = (hid > 0) ? handle_lookup((uint32_t)hid) : -1;
				if (lfd >= 0) {
					int32_t lfd32 = lfd;
					__builtin_memcpy(job.aux_buf, &lfd32,
							 sizeof(lfd32));
					export_fd_off = 0;
				}
			}
			if (inner_cmd == 0x0080170dU) {
				/* NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST.  Layout:
				 *   [params 24 bytes][handles N*4][list N*4]
				 * Guest zeroed the two embedded pointers at
				 * offsets 8 and 16; point them at our extension.
				 * After the ioctl we zero them again so we don't
				 * leak host VAs back. */
				uint32_t nc = 0;
				if (job.aux_size >= 4)
					__builtin_memcpy(&nc, job.aux_buf,
							 sizeof(uint32_t));
				if (nc > 0 && nc <= 4096 &&
				    job.aux_size >= 24 + (size_t)nc * 8) {
					uint64_t p_handles =
						(uint64_t)(uintptr_t)
						((char *)job.aux_buf + 24);
					uint64_t p_list = p_handles + (size_t)nc * 4;
					__builtin_memcpy((char *)job.aux_buf + 8,
							 &p_handles, 8);
					__builtin_memcpy((char *)job.aux_buf + 16,
							 &p_list, 8);
					/* flag: re-zero after ioctl */
					info_list_size = nc;  /* repurpose flag */
					info_list_base = 0xFFFFFFFFU; /* sentinel */
				} else {
					/* U-4 — FAIL CLOSED.  `nc` and aux_size are
					 * both guest-chosen; on guard failure the
					 * guest's own two pointers used to survive
					 * at aux+8 / aux+16 and reach the driver. */
					aux_clear_ptr(job.aux_buf, job.aux_size, 8);
					aux_clear_ptr(job.aux_buf, job.aux_size, 16);
				}
			}
			if (inner_cmd == 0x00000101U) {    /* GET_BUILD_VERSION */
				/*
				 * nv0000_ctrl_system_get_build_version_params is
				 * 40 bytes; the guest extends aux_buf to
				 * 40 + 3*sz so the host driver can write strings
				 * into the extension area without accessing guest VAs.
				 * Point the embedded string pointer fields at the
				 * extension region already present in job.aux_buf.
				 */
				uint32_t sz = 0;
				if (job.aux_size >= 4)
					__builtin_memcpy(&sz, job.aux_buf, sizeof(uint32_t));
				if (sz > 0 && sz <= 512 &&
				    job.aux_size >= 40 + (size_t)sz * 3) {
					str_sz = sz; /* flag: zero pointers after ioctl */
					uint64_t p1, p2, p3;
					p1 = (uint64_t)(uintptr_t)((char *)job.aux_buf + 40);
					p2 = p1 + sz;
					p3 = p1 + 2 * sz;
					__builtin_memcpy((char *)job.aux_buf +  8, &p1, 8);
					__builtin_memcpy((char *)job.aux_buf + 16, &p2, 8);
					__builtin_memcpy((char *)job.aux_buf + 24, &p3, 8);
				} else {
					/* U-4 — FAIL CLOSED.  `sz` and aux_size are
					 * guest-chosen; on guard failure the guest's
					 * three string pointers used to survive and
					 * be written through by the driver. */
					aux_clear_ptr(job.aux_buf, job.aux_size,  8);
					aux_clear_ptr(job.aux_buf, job.aux_size, 16);
					aux_clear_ptr(job.aux_buf, job.aux_size, 24);
				}
			}
		}


		/*
		 * UVM ioctls with embedded fd fields carry a handle_id (assigned
		 * by QEMU) in those fields.  Translate to our local fd before
		 * calling ioctl so the UVM driver sees a real fd.
		 */
		int32_t saved_uvm_embedded_fd = 0;
		size_t  uvm_embedded_fd_off   = 0;
		int     uvm_has_embedded_fd   = 0;
		if (job.param_size >= 4) {
			switch (job.cmd) {
			case NVKVM_STUB_UVM_MM_INITIALIZE:
				uvm_embedded_fd_off =
				    offsetof(struct nvkvm_stub_uvm_mm_initialize_params, uvm_fd);
				uvm_has_embedded_fd = 1;
				break;
			case NVKVM_STUB_UVM_REGISTER_GPU_VASPACE:
				uvm_embedded_fd_off =
				    offsetof(struct nvkvm_stub_uvm_register_gpu_vaspace_params, rm_ctrl_fd);
				uvm_has_embedded_fd = 1;
				break;
			case NVKVM_STUB_UVM_REGISTER_CHANNEL:
				uvm_embedded_fd_off =
				    offsetof(struct nvkvm_stub_uvm_register_channel_params, rm_ctrl_fd);
				uvm_has_embedded_fd = 1;
				break;
			case NVKVM_STUB_UVM_MAP_EXTERNAL_ALLOCATION:
				/* #81: rm_ctrl_fd offset is version-variant — 9248 for
				 * the V550 256-entry layout (550.54.14+, incl 575/580),
				 * 1184 for the pre-V550 1-entry layout (535).
				 * (This comment said "68" — a leftover from the
				 * arithmetic-derived 535 row that commit 377bee5
				 * replaced with MEASURED values.  Re-measured
				 * 2026-08-17 with tools/abi_derive.sh against OGKM
				 * 535.183.01: uvm_map_ext_fd_off = 1184.) */
				uvm_embedded_fd_off =
					nvkvm_abi_by_id(job.abi_profile)->uvm_map_ext_fd_off;
				uvm_has_embedded_fd = 1;
				break;
			}
		}
		if (uvm_has_embedded_fd &&
		    job.param_size >= uvm_embedded_fd_off + 4) {
			int32_t hid;
			__builtin_memcpy(&hid,
					 (char *)job.param_buf + uvm_embedded_fd_off,
					 sizeof(int32_t));
			saved_uvm_embedded_fd = hid;
			int local_fd = (hid > 0) ? handle_lookup((uint32_t)hid) : -1;
			if (local_fd < 0) {
				fs_dprintf(STDERR_FD,
					"nvkvm_stub: UVM cmd=0x%x: handle_id=%d not in stub table\n",
					job.cmd, hid);
				resp.retval = -EBADF;
				goto send_resp;
			}
			int32_t lfd32 = local_fd;
			__builtin_memcpy((char *)job.param_buf + uvm_embedded_fd_off,
					 &lfd32, sizeof(int32_t));
		}

		/*
		 * Frontend ioctls with embedded fd fields:
		 *   NV_ESC_RM_MAP_MEMORY   — fd at offset 48 in
		 *     nv_ioctl_nvos33_parameters_with_fd.
		 *   NV_ESC_RM_ALLOC_MEMORY — fd at offset 40 in
		 *     nv_ioctl_nvos02_parameters_with_fd.
		 * The guest sanitizer puts a handle_id there; the stub maps
		 * handle_id → its local fd via handle_lookup, calls ioctl,
		 * restores the handle_id on the way back.  Same shape as the
		 * UVM block above.  cmd encoding has TYPE='F' so check the
		 * low byte (_IOC_NR) against 0x4e / 0x27.
		 */
		int32_t saved_fe_embedded_fd = 0;
		size_t  fe_embedded_fd_off   = 0;
		int     fe_has_embedded_fd   = 0;
		if (((job.cmd >> 8) & 0xff) == 'F') {
			switch (job.cmd & 0xff) {
			case 0x4e:  /* NV_ESC_RM_MAP_MEMORY */
				fe_embedded_fd_off = 48;
				fe_has_embedded_fd = 1;
				break;
			case 0x27:  /* NV_ESC_RM_ALLOC_MEMORY */
				/* nv_ioctl_nvos02_parameters_with_fd: 56 bytes.
				 * NVOS02_PARAMETERS occupies +0..+47 (status@+40),
				 * then fd@+48, pad@+52. */
				fe_embedded_fd_off = 48;
				fe_has_embedded_fd = 1;
				break;
			case 0xce:  /* NV_ESC_ALLOC_OS_EVENT */
			case 0xcf:  /* NV_ESC_FREE_OS_EVENT */
				/* both have { hClient, hDevice, fd, status } */
				fe_embedded_fd_off = 8;
				fe_has_embedded_fd = 1;
				break;
			case 0xc9:  /* NV_ESC_REGISTER_FD */
				/* struct { __s32 ctl_fd; } — fd at offset 0. */
				fe_embedded_fd_off = 0;
				fe_has_embedded_fd = 1;
				break;
			}
		}
		/*
		 * Audit P2-1 (live-path G-2): neutralise NV_ESC_RM_IDLE_CHANNELS
		 * (nr 0x41) HERE, at the stub boundary.  The earlier dispatch.c
		 * fix was dead code (never wired into the IOCTL_ON_ISOLATE path),
		 * so the guest-controlled NvP64 array pointers were reaching the
		 * host driver, which would walk them as user pointers in the
		 * stub's address space.  We do not marshal the per-channel arrays
		 * (NVOS30 num_channels@12, p_clients@16, p_devices@24,
		 * p_channels@32); force the single-channel form by zeroing
		 * num_channels and the three array pointers.  job.param_buf is a
		 * private recv'd copy, so this is race-free (not subject to the
		 * SHM double-fetch, audit P2-2). */
		if (((job.cmd >> 8) & 0xff) == 'F' &&
		    (job.cmd & 0xff) == 0x41 /* NV_ESC_RM_IDLE_CHANNELS */ &&
		    job.param_size >= 40) {
			__builtin_memset((char *)job.param_buf + 12, 0, 28);
		}
		if (fe_has_embedded_fd &&
		    job.param_size >= fe_embedded_fd_off + 4) {
			int32_t hid;
			__builtin_memcpy(&hid,
					 (char *)job.param_buf + fe_embedded_fd_off,
					 sizeof(int32_t));
			saved_fe_embedded_fd = hid;
			if (hid > 0) {
				int local_fd = handle_lookup((uint32_t)hid);
				if (local_fd < 0) {
					fs_dprintf(STDERR_FD,
						"nvkvm_stub: FE cmd=0x%x: "
						"embedded handle_id=%d not in "
						"stub table\n", job.cmd, hid);
					resp.retval = -EBADF;
					goto send_resp;
				}
				int32_t lfd32 = local_fd;
				__builtin_memcpy((char *)job.param_buf +
						 fe_embedded_fd_off,
						 &lfd32, sizeof(int32_t));
			}
		}

		/*
		 * RM_ALLOC NV01_EVENT_OS_EVENT (hClass=0x79): the alloc
		 * params struct (NV0005_ALLOC_PARAMETERS) is in aux_buf,
		 * with Data (a 64-bit field containing the fd) at offset
		 * 16.  The guest replaced Data with the handle_id of an
		 * eventfd-typed handle we created in QEMU.  Translate
		 * back to our local fd before the driver sees it; restore
		 * the handle_id on the way back so the guest's view of
		 * the data field is unchanged.
		 */
		int32_t saved_alloc_event_fd = 0;
		int     have_alloc_event_fd  = 0;
		/* Audit G-5: only the nvos64 RM_ALLOC form (param_size 48 > the
		 * 32-byte nvos21 form) carries a guest-translated handle_id in
		 * Data; the guest's nvos21 branch leaves Data as a raw guest fd.
		 * Gate on the nvos64 size so a raw nvos21 fd is never misread as
		 * a handle_id (intra-VM fd/handle confusion). */
		if (((job.cmd >> 8) & 0xff) == 'F' &&
		    (job.cmd & 0xff) == 0x2b /* NV_ESC_RM_ALLOC */ &&
		    job.aux_size >= sizeof(uint64_t) * 3 &&
		    job.param_size > 32) {
			uint32_t h_class = 0;
			__builtin_memcpy(&h_class,
					 (char *)job.param_buf + 12, /* nvos21+nvos64 alias */
					 sizeof(uint32_t));
			if (h_class == 0x79 || h_class == 0x05) {  /* OS_EVENT + NV01_EVENT */
				uint64_t data64 = 0;
				__builtin_memcpy(&data64,
						 (char *)job.aux_buf + 16,
						 sizeof(uint64_t));
				int32_t hid = (int32_t)data64;
				saved_alloc_event_fd = hid;
				if (hid > 0) {
					int local_fd = handle_lookup((uint32_t)hid);
					if (local_fd < 0) {
						resp.retval = -EBADF;
						goto send_resp;
					}
					uint64_t lfd64 = (uint64_t)(uint32_t)local_fd;
					__builtin_memcpy((char *)job.aux_buf + 16,
							 &lfd64, sizeof(uint64_t));
					have_alloc_event_fd = 1;
				}
			}
		}


		/*
		 * NV_ESC_RM_ALLOC_MEMORY + hClass==NV01_MEMORY_SYSTEM_OS_DESCRIPTOR
		 * needs the kernel to pin libcuda's guest pages.  The guest module
		 * now migrates those pages onto memfds and MAP_FIXED-installs them
		 * at the same VA in our mm before forwarding the ioctl, so by the
		 * time we hit the kernel pin_user_pages walks our pagetables and
		 * finds tmpfs pages that alias libcuda's guest userspace.  No
		 * stub-local backing allocation is needed here.
		 */
		clear_fault_addr();
		/* Publish our in-flight txn so the reader can SIGUSR1 us if the
		 * guest signals this ioctl (#73).  Cleared the instant the ioctl
		 * returns so a late interrupt lands on a no-op handler, not on
		 * the post-processing/send path. */
		worker_inflight_txn[slot] = job.txn_id;
		uint64_t _tsc0 = __builtin_ia32_rdtsc();
		long ret  = stub_ioctl(fd, job.cmd, job.param_buf);
		uint64_t _dcyc = __builtin_ia32_rdtsc() - _tsc0;
		worker_inflight_txn[slot] = 0;
		/* DIAG: log slow forwarded ioctls (>2ms @ 2595MHz) — find the slow
		 * cuMemcpyDtoH op in the stub. */
		if (_dcyc > 5000000ULL)
			fs_dprintf(STDERR_FD,
				"PROF slow stub ioctl nr=0x%x type=0x%x %u us\n",
				(unsigned)(job.cmd & 0xff),
				(unsigned)((job.cmd >> 8) & 0xff),
				(unsigned)(_dcyc / 2595));
		/* sc*: negative return is -errno, matching kernel convention. */
		int  err  = (ret < 0) ? (int)(-ret) : 0;
		if (ret < 0) ret = -1;  /* normalise to (-1, errno) for callers */

		/* Restore embedded fd so the guest sees its own handle_id back. */
		if (uvm_has_embedded_fd &&
		    job.param_size >= uvm_embedded_fd_off + 4) {
			__builtin_memcpy((char *)job.param_buf + uvm_embedded_fd_off,
					 &saved_uvm_embedded_fd, sizeof(int32_t));
		}
		if (have_alloc_event_fd) {
			uint64_t hid64 = (uint64_t)(uint32_t)saved_alloc_event_fd;
			__builtin_memcpy((char *)job.aux_buf + 16,
					 &hid64, sizeof(uint64_t));
		}
		if (fe_has_embedded_fd &&
		    job.param_size >= fe_embedded_fd_off + 4) {
			__builtin_memcpy((char *)job.param_buf + fe_embedded_fd_off,
					 &saved_fe_embedded_fd, sizeof(int32_t));
		}

		/* Zero the embedded pointer field (don't leak host VA): nvos54/
		 * nvos64 at offset 16, NVKMS wrapper at offset 8. */
		if (job.aux_size > 0 &&
		    ((job.cmd == NVKVM_NVKMS_IOCTL_CMD &&
		      job.param_size >= NVKVM_NVKMS_PARAMS_SIZE) ||
		     (((job.cmd >> 8) & 0xff) == 'd' &&
		      ((job.cmd & 0xff) == 0x54 || (job.cmd & 0xff) == 0x41) &&
		      job.param_size >= 16))) {
			uint64_t zero = 0;
			__builtin_memcpy((char *)job.param_buf + NVKVM_NVKMS_ADDR_OFF,
					 &zero, sizeof(uint64_t));
		} else if (job.aux_size > 0 && job.param_size >= 24) {
			uint64_t zero = 0;
			__builtin_memcpy((char *)job.param_buf + 16, &zero,
					 sizeof(uint64_t));
		}

		/* Zero GET_BUILD_VERSION embedded string pointer fields (host VAs) */
		if (str_sz > 0) {
			uint64_t z = 0;
			__builtin_memcpy((char *)job.aux_buf +  8, &z, 8);
			__builtin_memcpy((char *)job.aux_buf + 16, &z, 8);
			__builtin_memcpy((char *)job.aux_buf + 24, &z, 8);
		}

		/* Zero the InfoList pointer we set above (don't leak host VA). The
		 * driver-written list contents in [info_list_base ..] are preserved
		 * — the guest module copies them out to the original user buffer. */
		if (info_list_size > 0) {
			uint64_t z = 0;
			__builtin_memcpy((char *)job.aux_buf + 8, &z, 8);
			/* FIFO_GET_CHANNELLIST has a second pointer at +16. */
			if (info_list_base == 0xFFFFFFFFU)
				__builtin_memcpy((char *)job.aux_buf + 16, &z, 8);
		}
		(void)info_list_base;

		/* EXPORT_OBJECT_TO_FD: put the handle_id back at aux+16 (the guest
		 * then restores its own fd) — never leak the stub's local fd. */
		if (export_fd_off >= 0)
			__builtin_memcpy((char *)job.aux_buf + export_fd_off,
					 &export_fd_saved, sizeof(export_fd_saved));

		/* DRM GEM_EXPORT_NVKMS_MEMORY: put the handle_id back at aux+0
		 * over the stub fd we substituted (the guest restores its own
		 * fd); never leak the stub's local fd. */
		if (drm_export_fd_off >= 0)
			__builtin_memcpy(job.aux_buf, &drm_export_fd_hid,
					 sizeof(drm_export_fd_hid));

		/* NVKMS REGISTER_SURFACE: restore handle_ids over the stub fds we
		 * substituted into the plane slots (don't leak stub fds). */
		for (int k = 0; k < regsurf_n; k++)
			__builtin_memcpy((char *)job.aux_buf + regsurf_off[k],
					 &regsurf_hid[k], sizeof(int32_t));

		/*
		 * Extract NvStatus from the response struct.
		 *   nvos54 (RM_CONTROL, 32 bytes): status at offset 28
		 *   nvos21 (RM_ALLOC,  32 bytes): status at offset 28
		 *   nvos64 (RM_ALLOC,  48 bytes): status at offset 40
		 *     (after hRoot/parent/new/class, pAllocParms, pRightsRequested,
		 *      paramsSize, flags — then status; see gVisor frontend.go).
		 */
		uint32_t nvstatus = 0;
		if (((job.cmd >> 8) & 0xff) == 'F') {
			/* Frontend ioctls: nvstatus offset depends on the
			 * specific NVOS* struct, not just total size — multiple
			 * structs share the same byte length but place Status
			 * at different offsets.  Dispatch by _IOC_NR. */
			unsigned nr = job.cmd & 0xff;
			int off = -1;
			switch (nr) {
			case 0x27: off = 40; break; /* NV_ESC_RM_ALLOC_MEMORY: NVOS02 status at +40, fd at +48 */
			case 0x29: off = 12; break; /* NV_ESC_RM_FREE: nvos00 status at +12 */
			case 0x2a: off = 28; break; /* NV_ESC_RM_CONTROL: nvos54 status at +28 */
			case 0x2b: /* NV_ESC_RM_ALLOC: nvos21=32B status@28, nvos64=48B status@40 */
				off = (job.param_size == 48) ? 40 : 28;
				break;
			case 0x34: off = 24; break; /* NV_ESC_RM_DUP_OBJECT: nvos55 28B status@24 (575 SDK) */
			case 0x35: off = 20; break; /* NV_ESC_RM_SHARE: nvos57 24B status@20 */
			case 0x4a: off = 20; break; /* NV_ESC_RM_VID_HEAP_CONTROL: nvos32 status@20 (184B struct) */
			case 0x4e: off = 40; break; /* NV_ESC_RM_MAP_MEMORY: nvos33_with_fd 56B status@40, fd@48 */
			case 0x4f: off = 24; break; /* NV_ESC_RM_UNMAP_MEMORY: nvos34 32B status@24 */
			case 0x57: /* NV_ESC_RM_MAP_MEMORY_DMA: nvos46 status@48 (V580: @56, #81) */
				off = (int)nvkvm_abi_by_id(job.abi_profile)->nvos46_status_off;
				break;
			case 0x58: off = 40; break; /* NV_ESC_RM_UNMAP_MEMORY_DMA: nvos47 48B status@40 (incl pad0+dmaOff+size) */
			default:
				/* Fall back to size-based heuristic for ioctls
				 * we haven't enumerated yet. */
				if (job.param_size == 48)
					off = 40;
				else if (job.param_size >= 32)
					off = 28;
				else if (job.param_size == 16)
					off = 12;
				break;
			}
			if (off >= 0 && (uint32_t)(off + 4) <= job.param_size)
				__builtin_memcpy(&nvstatus,
						 (char *)job.param_buf + off,
						 sizeof(uint32_t));
		} else if (job.param_size >= 4) {
			/* UVM ioctls (TYPE == 0): rm_status is the last
			 * 4 bytes of the params struct for every UVM cmd
			 * in our ABI (gVisor's "HasStatus" pattern). */
			__builtin_memcpy(&nvstatus,
					 (char *)job.param_buf +
					 (job.param_size - 4),
					 sizeof(uint32_t));
		}

		resp.retval     = err ? -err : (int32_t)ret;
		resp.nvstatus   = nvstatus;
		resp.fault_addr = get_fault_addr();
		resp.param_size = job.param_size;
		resp.aux_size   = job.aux_size;

	send_resp:
		fs_mutex_lock(&write_mutex);
		send_full(&resp, sizeof(resp));
		if (resp.param_size > 0 && job.param_buf)
			send_full(job.param_buf, resp.param_size);
		if (resp.aux_size > 0 && job.aux_buf)
			send_full(job.aux_buf, resp.aux_size);
		fs_mutex_unlock(&write_mutex);

		/* Use stub_munmap to free blobs (allocated from anonymous mmap) */
		if (job.param_buf)
			stub_munmap(job.param_buf,
				    (job.param_size + 4095) & ~4095UL);
		if (job.aux_buf)
			stub_munmap(job.aux_buf,
				    (job.aux_size + 4095) & ~4095UL);
	}
	/* Falling through here means dequeue saw stub_exiting=1 — let the
	 * clone3 trampoline call SYS_exit on our behalf. */
}

/* Allocate a blob buffer via anonymous mmap (no heap/libc needed). */
static void *blob_alloc(size_t size)
{
	if (!size) return NULL;
	size_t aligned = (size + 4095) & ~4095UL;
	void *p = stub_mmap(NULL, aligned, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return (p == MAP_FAILED) ? NULL : p;
}

/*
 * ISOLATE_CMD_SETUP_RING: map the QEMU-minted ring memfd (delivered via
 * SCM_RIGHTS) and run the bidirectional probe self-test.  QEMU is trusted, but
 * the geometry is validated defensively before mmap so a bad size can never
 * blow up the stub.  Phase 2 maps + self-tests only; Phase 3 spins a consumer
 * thread on g_req_ring.  Failure replies RING_READY{error<0} and the isolate
 * keeps serving every ioctl over the existing socket path.
 */
static void handle_setup_ring(const struct isolate_cmd_setup_ring *cmd,
			      struct msghdr *msg_hdr, long msg_len)
{
	struct cmsghdr *cm = CMSG_FIRSTHDR(msg_hdr);
	if (!cm || cm->cmsg_level != SOL_SOCKET ||
	    cm->cmsg_type != SCM_RIGHTS ||
	    cm->cmsg_len != CMSG_LEN(sizeof(int))) {
		send_ring_ready(-EINVAL, 0);
		return;
	}
	int fd;
	__builtin_memcpy(&fd, CMSG_DATA(cm), sizeof(int));

	if (msg_len < (long)sizeof(*cmd) || g_ring_base) {   /* malformed / dup */
		stub_close(fd);
		send_ring_ready(-EINVAL, 0);
		return;
	}

	uint32_t ring_bytes = cmd->ring_bytes;
	uint32_t region     = cmd->region_size;
	uint32_t resp_off   = cmd->resp_off;

	/* Geometry: power-of-two ring, layout matches the shared helpers, the
	 * region holds both [control + data] halves, bounded total size. */
	if (!nvkvm_ring_size_ok(ring_bytes) ||
	    cmd->req_off != 0 ||
	    resp_off != (uint32_t)nvkvm_ring_resp_off(ring_bytes) ||
	    region < (uint32_t)nvkvm_ring_region_size(ring_bytes) ||
	    region > (16u << 20)) {                          /* 16 MiB cap */
		stub_close(fd);
		send_ring_ready(-EINVAL, 0);
		return;
	}

	void *base = stub_mmap(NULL, region, PROT_READ | PROT_WRITE,
			       MAP_SHARED, fd, 0);
	stub_close(fd);                  /* the mapping keeps the memfd alive */
	if (base == MAP_FAILED) {
		send_ring_ready(-ENOMEM, 0);
		return;
	}

	struct nvkvm_ring *req  = (struct nvkvm_ring *)base;
	struct nvkvm_ring *resp = (struct nvkvm_ring *)((uint8_t *)base + resp_off);
	uint8_t *req_data  = (uint8_t *)req  + sizeof(struct nvkvm_ring);
	uint8_t *resp_data = (uint8_t *)resp + sizeof(struct nvkvm_ring);

	/* Probe: read QEMU's word (proves QEMU→isolate when echoed), write the
	 * masked reply (proves isolate→QEMU when QEMU re-reads it). */
	uint64_t v;
	__builtin_memcpy(&v, req_data, sizeof(v));
	uint64_t reply = v ^ NVKVM_RING_PROBE_MASK;
	__builtin_memcpy(resp_data, &reply, sizeof(reply));

	g_ring_base        = base;
	g_ring_region_size = region;
	g_req_ring         = req;
	g_resp_ring        = resp;
	g_ring_bytes       = ring_bytes;

	send_ring_ready(0, v);
}

/* ── Command handlers (reader thread) ───────────────────────────────────── */

/*
 * dev_id values match nvkvm_proto.h (NVKVM_DEV_CTL=0, NVKVM_DEV_UVM=1,
 * NVKVM_DEV_GPU(n)=16+n, NVKVM_DEV_EVENTFD=0xFF). UVM is opened by QEMU and
 * never reaches OPEN_DEVICE — the stub also has its own UVM pool for the
 * file-owner-mm dance (see uvm_local_fds).
 */
static int dev_id_to_path(uint32_t dev_id, char *buf, size_t buflen)
{
	/* Relative names (no "/dev/" prefix) so they work with openat() against
	 * the /dev O_PATH dirfd in an empty mount namespace. */
	if (dev_id == 0) {              /* NVKVM_DEV_CTL */
		if (buflen < sizeof("nvidiactl")) return -1;
		__builtin_memcpy(buf, "nvidiactl", sizeof("nvidiactl"));
		return 0;
	}
	if (dev_id >= 16 && dev_id < 16 + 16) {
		unsigned n = dev_id - 16;
		/* "nvidia" + up to 2 digits + NUL = 9 bytes */
		if (buflen < 9) return -1;
		__builtin_memcpy(buf, "nvidia", 6);
		if (n < 10) {
			buf[6] = '0' + (char)n;
			buf[7] = 0;
		} else {
			buf[6] = '0' + (char)(n / 10);
			buf[7] = '0' + (char)(n % 10);
			buf[8] = 0;
		}
		return 0;
	}
	if (dev_id == 48) {            /* NVKVM_DEV_MODESET */
		if (buflen < sizeof("nvidia-modeset")) return -1;
		__builtin_memcpy(buf, "nvidia-modeset", sizeof("nvidia-modeset"));
		return 0;
	}
	if (dev_id >= 32 && dev_id < 32 + 16) {   /* NVKVM_DEV_DRM_RD(n) */
		/* renderD(128+n) under dri/ — relative to the host /dev dirfd. */
		unsigned minor = 128 + (dev_id - 32);
		/* "dri/renderD" + up to 3 digits + NUL = 15 bytes */
		if (buflen < 15) return -1;
		__builtin_memcpy(buf, "dri/renderD", 11);
		buf[11] = '0' + (char)(minor / 100);
		buf[12] = '0' + (char)((minor / 10) % 10);
		buf[13] = '0' + (char)(minor % 10);
		buf[14] = 0;
		return 0;
	}
	return -1;
}

/*
 * Send an OPEN_DEVICE response. On success the opened fd is attached via
 * SCM_RIGHTS in the same sendmsg as the response struct — QEMU's recvmsg
 * picks both up atomically and there's no window where the fd exists in
 * the stub but not in QEMU. On failure (retval != 0) no ancillary data
 * is attached and the stub holds nothing.
 */
static int send_open_device_resp(uint32_t txn_id, int retval, int fd)
{
	struct isolate_resp_open_device resp = {
		.type   = ISOLATE_RESP_OPEN_DEVICE,
		.txn_id = txn_id,
		.retval = retval,
	};
	struct iovec iov = { &resp, sizeof(resp) };
	char cmsg_buf[CMSG_SPACE(sizeof(int))];
	struct msghdr msg_hdr = {
		.msg_iov     = &iov,
		.msg_iovlen  = 1,
	};
	if (retval == 0 && fd >= 0) {
		msg_hdr.msg_control    = cmsg_buf;
		msg_hdr.msg_controllen = sizeof(cmsg_buf);
		struct cmsghdr *cm = CMSG_FIRSTHDR(&msg_hdr);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type  = SCM_RIGHTS;
		cm->cmsg_len   = CMSG_LEN(sizeof(int));
		__builtin_memcpy(CMSG_DATA(cm), &fd, sizeof(int));
		msg_hdr.msg_controllen = cm->cmsg_len;
	}
	fs_mutex_lock(&write_mutex);
	long r = stub_sendmsg(SOCK_FD, &msg_hdr, 0);
	fs_mutex_unlock(&write_mutex);
	return r < 0 ? -1 : 0;
}

static unsigned stub_drm_fd_count;

static void handle_open_device(struct isolate_cmd_open_device *cmd)
{
	int fd;

	if (cmd->dev_id == 0xFF) {       /* NVKVM_DEV_EVENTFD */
		/* EFD_NONBLOCK | EFD_CLOEXEC = 0x800 | 0x80000 */
		fd = stub_eventfd2(0, /* EFD_NONBLOCK */ 0x800 |
				   /* EFD_CLOEXEC */ 0x80000);
	} else if (cmd->dev_id == 1) {   /* NVKVM_DEV_UVM */
		/* UVM never goes through this path — QEMU opens UVM. */
		send_open_device_resp(cmd->txn_id, -EINVAL, -1);
		return;
	} else {
		char path[24];
		fd = -EBADF;
		/*
		 * DRM render node: use the fd QEMU parked before dropping our
		 * privileges — we cannot open the node ourselves any more (0660
		 * root:render, and we hold no groups).  A private dup per handle,
		 * so one guest process closing its DRM fd does not take the node
		 * away from the others.  Falls through to opening by name when
		 * nothing was parked, which is the un-hardened spawn.
		 */
		if (cmd->dev_id >= 32 &&
		    cmd->dev_id - 32 < stub_drm_fd_count)
			fd = (int)sc3(__NR_fcntl,
				      NVKVM_DRM_FD(cmd->dev_id - 32),
				      NVKVM_F_DUPFD_CLOEXEC, 0);
		if (fd < 0) {
			if (dev_id_to_path(cmd->dev_id, path, sizeof(path)) < 0) {
				send_open_device_resp(cmd->txn_id, -EINVAL, -1);
				return;
			}
			fd = (int)stub_open_dev(path, (int)cmd->flags | O_CLOEXEC);
		}
	}

	if (fd < 0) {
		send_open_device_resp(cmd->txn_id, fd, -1);
		return;
	}

	fs_mutex_lock(&fd_mutex);
	if (cmd->handle_id < MAX_HANDLES &&
	    handle_fds[cmd->handle_id] >= 0) {
		/* QEMU reused an id we already had. Close the prior holder
		 * before overwriting — same defensive policy as RECEIVE_FD. */
		stub_close(handle_fds[cmd->handle_id]);
	}
	handle_store(cmd->handle_id, fd);
	fs_mutex_unlock(&fd_mutex);

	if (send_open_device_resp(cmd->txn_id, 0, fd) < 0) {
		/* sendmsg failure: QEMU socket gone. The fd has been stored
		 * locally but the SCM copy never made it; clean up so we
		 * don't leak. Caller (reader loop) will tear down anyway. */
		fs_mutex_lock(&fd_mutex);
		handle_remove(cmd->handle_id);
		fs_mutex_unlock(&fd_mutex);
	}
}

/* DRM PRIME export (#106 present path). Mirrors <drm/drm.h>:
 *   DRM_IOCTL_PRIME_HANDLE_TO_FD = _IOWR('d', 0x2d, struct drm_prime_handle)
 *   = (3<<30)|(12<<16)|('d'<<8)|0x2d = 0xC00C642D.
 * Flags = DRM_CLOEXEC|DRM_RDWR so the host display/codec can import + map it. */
struct stub_drm_prime_handle { uint32_t handle; uint32_t flags; int32_t fd; };
#define STUB_DRM_IOCTL_PRIME_HANDLE_TO_FD 0xC00C642DUL
#define STUB_DRM_CLOEXEC 0x80000u   /* O_CLOEXEC */
#define STUB_DRM_RDWR    0x2u       /* O_RDWR    */

static int send_present_export_resp(uint32_t txn_id, int retval, int fd)
{
	struct isolate_resp_present_export resp = {
		.type   = ISOLATE_RESP_PRESENT_EXPORT,
		.txn_id = txn_id,
		.retval = retval,
	};
	struct iovec iov = { &resp, sizeof(resp) };
	char cmsg_buf[CMSG_SPACE(sizeof(int))];
	struct msghdr msg_hdr = {
		.msg_iov     = &iov,
		.msg_iovlen  = 1,
	};
	if (retval == 0 && fd >= 0) {
		msg_hdr.msg_control    = cmsg_buf;
		msg_hdr.msg_controllen = sizeof(cmsg_buf);
		struct cmsghdr *cm = CMSG_FIRSTHDR(&msg_hdr);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type  = SCM_RIGHTS;
		cm->cmsg_len   = CMSG_LEN(sizeof(int));
		__builtin_memcpy(CMSG_DATA(cm), &fd, sizeof(int));
		msg_hdr.msg_controllen = cm->cmsg_len;
	}
	fs_mutex_lock(&write_mutex);
	long r = stub_sendmsg(SOCK_FD, &msg_hdr, 0);
	fs_mutex_unlock(&write_mutex);
	return r < 0 ? -1 : 0;
}

/* Export a render-node GEM object as a host dma-buf and hand it to QEMU via
 * SCM_RIGHTS (#106).  The dma-buf is a host buffer reference; the stub never
 * maps or reads it.  Transient: we close our copy right after sending — the
 * SCM_RIGHTS transfer gives QEMU its own reference (kept alive by the kernel
 * across our close). */
static void handle_present_export(struct isolate_cmd_present_export *cmd)
{
	int rfd;

	fs_mutex_lock(&fd_mutex);
	rfd = handle_lookup(cmd->handle_id);
	fs_mutex_unlock(&fd_mutex);
	if (rfd < 0) {
		send_present_export_resp(cmd->txn_id, -EBADF, -1);
		return;
	}

	struct stub_drm_prime_handle p = {
		.handle = cmd->gem_handle,
		.flags  = STUB_DRM_CLOEXEC | STUB_DRM_RDWR,
		.fd     = -1,
	};
	long r = stub_ioctl(rfd, STUB_DRM_IOCTL_PRIME_HANDLE_TO_FD, &p);
	if (r < 0 || p.fd < 0) {
		send_present_export_resp(cmd->txn_id, (r < 0) ? (int)r : -EINVAL, -1);
		return;
	}
	send_present_export_resp(cmd->txn_id, 0, p.fd);
	stub_close(p.fd);
}

/* DRM PRIME import (#110 cross-isolate). Mirrors <drm/drm.h>:
 *   DRM_IOCTL_PRIME_FD_TO_HANDLE = _IOWR('d', 0x2e, struct drm_prime_handle)
 *   = (3<<30)|(12<<16)|('d'<<8)|0x2e = 0xC00C642E.
 * The dma-buf fd was exported by the OWNER stub (PRIME_HANDLE_TO_FD) and relayed
 * here by QEMU via SCM_RIGHTS.  PRIME_FD_TO_HANDLE on our render node creates a
 * real local nvidia-drm GEM backed by the same physical memory, so the caller's
 * subsequent RM export/import (0x09 / 0x3d06) run entirely within this stub. */
#define STUB_DRM_IOCTL_PRIME_FD_TO_HANDLE 0xC00C642EUL

static void handle_xiso_import(struct isolate_cmd_xiso_import *cmd,
			       struct msghdr *msg_hdr)
{
	struct isolate_resp_xiso_import resp = {
		.type   = ISOLATE_RESP_XISO_IMPORT,
		.txn_id = cmd->txn_id,
		.retval = -EINVAL,
		.gem_handle = 0,
	};
	int dbuf = -1;
	int rfd;

	/* The dma-buf fd arrives via SCM_RIGHTS (QEMU is trusted). */
	struct cmsghdr *cm = CMSG_FIRSTHDR(msg_hdr);
	if (cm && cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS &&
	    cm->cmsg_len == CMSG_LEN(sizeof(int)))
		__builtin_memcpy(&dbuf, CMSG_DATA(cm), sizeof(int));
	if (dbuf < 0) {
		locked_send(&resp, sizeof(resp));
		return;
	}

	fs_mutex_lock(&fd_mutex);
	rfd = handle_lookup(cmd->handle_id);
	fs_mutex_unlock(&fd_mutex);
	if (rfd < 0) {
		resp.retval = -EBADF;
		stub_close(dbuf);
		locked_send(&resp, sizeof(resp));
		return;
	}

	struct stub_drm_prime_handle p = { .handle = 0, .flags = 0, .fd = dbuf };
	long r = stub_ioctl(rfd, STUB_DRM_IOCTL_PRIME_FD_TO_HANDLE, &p);
	stub_close(dbuf);   /* PRIME_FD_TO_HANDLE took its own reference */
	if (r < 0) {
		resp.retval = (int)r;
	} else {
		resp.retval = 0;
		resp.gem_handle = p.handle;
	}
	locked_send(&resp, sizeof(resp));
}

static void handle_close_fd(uint32_t handle_id)
{
	fs_mutex_lock(&fd_mutex);
	handle_remove(handle_id);
	fs_mutex_unlock(&fd_mutex);
	send_ok();
}

static void handle_ioctl_cmd(struct isolate_cmd_ioctl *cmd)
{
	if (cmd->param_size > MAX_PARAM_SIZE || cmd->aux_size > MAX_PARAM_SIZE) {
		/* Can't send error with txn_id here in the old format;
		 * send a minimal error response. */
		struct isolate_resp_ioctl resp = {
			.type   = ISOLATE_RESP_IOCTL,
			.txn_id = cmd->txn_id,
			.retval = -EINVAL,
		};
		locked_send(&resp, sizeof(resp));
		/* drain the data that was supposed to follow */
		return;
	}

	struct ioctl_job job = {
		.txn_id      = cmd->txn_id,
		.handle_id   = cmd->handle_id,
		.cmd         = cmd->cmd,
		.flags       = cmd->flags,
		.abi_profile = cmd->abi_profile,   /* #81 */
		.param_size = cmd->param_size,
		.aux_size   = cmd->aux_size,
	};

	/* Read param+aux blobs into per-job buffers.
	 *
	 * Audit G-8 (defense-in-depth): the host driver reads _IOC_SIZE(cmd)
	 * bytes regardless of the guest-supplied param_size.  blob_alloc
	 * already page-rounds (so today's <4 KiB structs are covered), but
	 * make the invariant explicit and future-proof: map at least
	 * _IOC_SIZE bytes (the tail is zero-filled) so an under-sized guest
	 * buffer can never make the driver read past the mapping.  param_size
	 * stays the logical guest size used by the embedded-field bounds
	 * checks; only the allocation is widened.  _IOC_SIZE is capped at one
	 * page so the page-rounded size never changes (keeps every munmap
	 * site, which rounds param_size, consistent). */
	if (cmd->param_size > 0) {
		unsigned ioc_sz = (cmd->cmd >> 16) & 0x3fff;
		size_t   alloc_sz = cmd->param_size;
		if (ioc_sz > 4096u)
			ioc_sz = 4096u;
		if (ioc_sz > alloc_sz)
			alloc_sz = ioc_sz;
		job.param_buf = blob_alloc(alloc_sz);
		if (!job.param_buf || recv_full(job.param_buf, cmd->param_size) < 0) {
			stub_munmap(job.param_buf,
				    (cmd->param_size + 4095) & ~4095UL);
			struct isolate_resp_ioctl resp = {
				.type = ISOLATE_RESP_IOCTL, .txn_id = cmd->txn_id,
				.retval = -ENOMEM };
			locked_send(&resp, sizeof(resp));
			return;
		}
	}
	if (cmd->aux_size > 0) {
		job.aux_buf = blob_alloc(cmd->aux_size);
		if (!job.aux_buf || recv_full(job.aux_buf, cmd->aux_size) < 0) {
			if (job.param_buf)
				stub_munmap(job.param_buf,
					    (cmd->param_size + 4095) & ~4095UL);
			stub_munmap(job.aux_buf,
				    (cmd->aux_size + 4095) & ~4095UL);
			struct isolate_resp_ioctl resp = {
				.type = ISOLATE_RESP_IOCTL, .txn_id = cmd->txn_id,
				.retval = -ENOMEM };
			locked_send(&resp, sizeof(resp));
			return;
		}
	}

	enqueue_job(&job);
}

static void handle_mmap(struct isolate_cmd_mmap *cmd)
{
	fs_mutex_lock(&fd_mutex);
	int fd = handle_lookup(cmd->handle_id);
	fs_mutex_unlock(&fd_mutex);

	struct isolate_resp_mmap r = { .type = ISOLATE_RESP_MMAP };
	if (fd < 0) { r.retval = -EBADF; locked_send(&r, sizeof(r)); return; }

	uint32_t flags = cmd->map_flags | MAP_FIXED;
	void *addr = stub_mmap((void *)(uintptr_t)cmd->gva, (size_t)cmd->length,
			       (int)cmd->prot, (int)flags, fd, (off_t)cmd->offset);
	r.retval = ((uintptr_t)addr == (uintptr_t)MAP_FAILED) ? -ENOMEM : 0;
	locked_send(&r, sizeof(r));
}

static void handle_munmap_cmd(struct isolate_cmd_munmap *cmd)
{
	stub_munmap((void *)(uintptr_t)cmd->gva, (size_t)cmd->length);
	send_ok();
}

/* ── REALIZE_UVM_FD handler ─────────────────────────────────────────────────
 * Replay the per-fd UVM state recorded by the guest module, then run the
 * mode-specific intent ioctl and mmap.  Returns host VA on success.
 *
 * Kernel struct shapes are hand-pinned here (no headers in the -nostdlib
 * stub).  Sizes match src/abi/uvm.h and the kernel-open UVM headers.
 */
#define STUB_UVM_INITIALIZE              0x30000001
#define STUB_UVM_REGISTER_GPU                    37
#define STUB_UVM_REGISTER_GPU_VASPACE            25
#define STUB_UVM_CREATE_RANGE_GROUP              23
#define STUB_UVM_ALLOC_SEMAPHORE_POOL            68

struct stub_uvm_init { uint64_t flags; uint32_t rm_status; uint32_t _pad; };
struct stub_uvm_uuid16 { uint8_t b[16]; };
struct stub_uvm_register_gpu {
	struct stub_uvm_uuid16 uuid;
	uint8_t  numa_enabled;
	uint8_t  _pad0[3];
	int32_t  numa_node_id;
	uint32_t rm_status;
	uint32_t _pad1;
};
struct stub_uvm_register_vas {
	struct stub_uvm_uuid16 uuid;
	uint32_t rm_ctrl_fd;
	uint32_t h_client;
	uint32_t h_va_space;
	uint32_t rm_status;
};
struct stub_uvm_range_group {
	uint64_t range_group_id;
	uint32_t rm_status;
	uint32_t _pad;
};

/* State snapshot layout — must match nvkvm_uvm_state_snapshot in
 * src/common/nvkvm_proto.h.  Kept inline to avoid pulling that header
 * into the stub. */
#define STUB_MAX_REG_GPUS      16
#define STUB_MAX_VA_SPACES     16
#define STUB_MAX_RANGE_GROUPS  16
struct stub_state_snapshot {
	uint64_t init_flags;
	uint32_t n_gpus;
	uint32_t n_va_spaces;
	uint32_t n_range_groups;
	uint32_t _pad0;
	struct { uint8_t uuid[16]; } gpus[STUB_MAX_REG_GPUS];
	struct { uint8_t uuid[16];
		 uint32_t rm_ctrl_fd_handle_id;
		 uint32_t h_client;
		 uint32_t h_va_space;
		 uint32_t _pad; }
		va_spaces[STUB_MAX_VA_SPACES];
	uint64_t range_group_ids[STUB_MAX_RANGE_GROUPS];
};

static void handle_realize_uvm_fd(struct isolate_cmd_realize_uvm_fd *cmd)
{
	struct isolate_resp_realize_uvm resp = {
		.type = ISOLATE_RESP_REALIZE_UVM,
		.txn_id = cmd->txn_id,
	};

	/* 1. Recv the state snapshot. */
	struct stub_state_snapshot state;
	if (cmd->state_size != sizeof(state)) {
		resp.retval = -EINVAL;
		locked_send(&resp, sizeof(resp));
		return;
	}
	if (recv_full(&state, sizeof(state)) < 0) {
		resp.retval = -EIO;
		locked_send(&resp, sizeof(resp));
		return;
	}
	if (state.n_gpus > STUB_MAX_REG_GPUS ||
	    state.n_va_spaces > STUB_MAX_VA_SPACES ||
	    state.n_range_groups > STUB_MAX_RANGE_GROUPS) {
		resp.retval = -EINVAL;
		locked_send(&resp, sizeof(resp));
		return;
	}

	/* 2. Recv the intent blob (mode-specific).  Allocate a page-rounded
	 *    buffer; the SEM_POOL intent is 9248 bytes so we need ~3 pages. */
	if (cmd->intent_size == 0 || cmd->intent_size > 64 * 1024) {
		resp.retval = -EINVAL;
		locked_send(&resp, sizeof(resp));
		return;
	}
	size_t intent_aligned = (cmd->intent_size + 4095) & ~4095UL;
	void *intent_buf = stub_mmap(NULL, intent_aligned,
				     PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (intent_buf == MAP_FAILED) {
		resp.retval = -ENOMEM;
		locked_send(&resp, sizeof(resp));
		return;
	}
	if (recv_full(intent_buf, cmd->intent_size) < 0) {
		stub_munmap(intent_buf, intent_aligned);
		resp.retval = -EIO;
		locked_send(&resp, sizeof(resp));
		return;
	}

	/* 3. Open a fresh /dev/nvidia-uvm in the stub's mm. */
	int uvm_fd = (int)stub_open_dev("nvidia-uvm", O_RDWR | O_CLOEXEC);
	if (uvm_fd < 0) {
		stub_munmap(intent_buf, intent_aligned);
		resp.retval = uvm_fd;  /* already -errno from sc* */
		locked_send(&resp, sizeof(resp));
		return;
	}

	/* 4. UVM_INITIALIZE with recorded flags. */
	struct stub_uvm_init init = { .flags = state.init_flags };
	long ir = stub_ioctl(uvm_fd, STUB_UVM_INITIALIZE, &init);
	if (ir < 0 || init.rm_status != 0) {
		resp.retval = init.rm_status ? 0 : (int32_t)ir;
		resp.rm_status = init.rm_status;
		goto cleanup;
	}

	/* 5. Replay each REGISTER_GPU. */
	for (uint32_t i = 0; i < state.n_gpus; i++) {
		struct stub_uvm_register_gpu rg = {0};
		__builtin_memcpy(rg.uuid.b, state.gpus[i].uuid, 16);
		rg.numa_node_id = -1;
		long r = stub_ioctl(uvm_fd, STUB_UVM_REGISTER_GPU, &rg);
		if (r < 0 || rg.rm_status != 0) {
			resp.rm_status = rg.rm_status;
			resp.retval = rg.rm_status ? 0 : (int32_t)r;
			goto cleanup;
		}
	}

	/* 6. Replay each REGISTER_GPU_VASPACE.
	 * NOTE: rm_ctrl_fd_handle_id is the guest's RM handle.  Translate
	 * to the stub's local nvidiactl fd via handle_lookup. */
	for (uint32_t i = 0; i < state.n_va_spaces; i++) {
		struct stub_uvm_register_vas rv = {0};
		__builtin_memcpy(rv.uuid.b, state.va_spaces[i].uuid, 16);
		int local_fd = handle_lookup(state.va_spaces[i].rm_ctrl_fd_handle_id);
		if (local_fd < 0) {
			resp.retval = -EBADF;
			goto cleanup;
		}
		rv.rm_ctrl_fd = (uint32_t)local_fd;
		rv.h_client   = state.va_spaces[i].h_client;
		rv.h_va_space = state.va_spaces[i].h_va_space;
		long r = stub_ioctl(uvm_fd, STUB_UVM_REGISTER_GPU_VASPACE, &rv);
		if (r < 0 || rv.rm_status != 0) {
			resp.rm_status = rv.rm_status;
			resp.retval = rv.rm_status ? 0 : (int32_t)r;
			goto cleanup;
		}
	}

	/* 7. Replay each CREATE_RANGE_GROUP. */
	for (uint32_t i = 0; i < state.n_range_groups; i++) {
		struct stub_uvm_range_group rgg = {0};
		rgg.range_group_id = state.range_group_ids[i];
		long r = stub_ioctl(uvm_fd, STUB_UVM_CREATE_RANGE_GROUP, &rgg);
		if (r < 0 || rgg.rm_status != 0) {
			resp.rm_status = rgg.rm_status;
			resp.retval = rgg.rm_status ? 0 : (int32_t)r;
			goto cleanup;
		}
	}

	/* 8. Mode-specific intent ioctl.  Currently only SEM_POOL = 1. */
	if (cmd->mode == 1 /* NVKVM_UVM_REALIZE_MODE_SEM_POOL */) {
		long r = stub_ioctl(uvm_fd, STUB_UVM_ALLOC_SEMAPHORE_POOL,
			       intent_buf);
		if (r < 0) {
			resp.retval = (int32_t)r;
			uint32_t *st = (uint32_t *)((char *)intent_buf +
						    cmd->intent_size -
						    sizeof(uint32_t));
			resp.rm_status = *st;
			goto cleanup;
		}
		uint32_t *st = (uint32_t *)((char *)intent_buf +
					    cmd->intent_size -
					    sizeof(uint32_t));
		if (*st != 0) {
			resp.rm_status = *st;
			resp.retval = 0;
			goto cleanup;
		}
	} else {
		resp.retval = -ENOTSUP;
		goto cleanup;
	}

	/* 9. mmap(2) at the requested host VA. */
	uint32_t mmap_flags = cmd->map_flags;
	if (cmd->host_va_hint)
		mmap_flags |= MAP_FIXED;
	void *host_va = stub_mmap((void *)(uintptr_t)cmd->host_va_hint,
				  (size_t)cmd->length,
				  (int)cmd->prot, (int)mmap_flags,
				  uvm_fd, (off_t)cmd->offset);
	if (host_va == MAP_FAILED) {
		resp.retval = -ENOMEM;
		goto cleanup;
	}

	resp.retval = 0;
	resp.host_va = (uint64_t)(uintptr_t)host_va;
	resp.length = cmd->length;
	resp.realize_token = resp.host_va;   /* simplistic: VA is the token */
	/* Keep uvm_fd open — subsequent SIDE_EFFECT ioctls route to it.
	 * For now leak; a future commit will add a realize-token → fd map. */

cleanup:
	stub_munmap(intent_buf, intent_aligned);
	if (resp.retval != 0 || resp.rm_status != 0) {
		stub_close(uvm_fd);
	}
	locked_send(&resp, sizeof(resp));
}

/* One SEQPACKET command message — large enough for any command struct.  Shared
 * by the main reader loop and the consumer loop's drain-edge socket poll. */
union stub_cmd {
	uint32_t                            type;
	struct isolate_cmd_receive_fd       recv_fd;
	struct isolate_cmd_close_fd         close_fd;
	struct isolate_cmd_ioctl            ioctl_cmd;
	struct isolate_cmd_mmap             mmap_cmd;
	struct isolate_cmd_munmap           munmap_cmd;
	struct isolate_cmd_poll             poll_cmd;
	struct isolate_cmd_unpoll           unpoll_cmd;
	struct isolate_cmd_open_device      open_dev;
	struct isolate_cmd_realize_uvm_fd   realize;
	struct isolate_cmd_interrupt        interrupt_cmd;
	struct isolate_cmd_setup_ring       setup_ring;
	struct isolate_cmd_enter_loop       enter_loop;
	struct isolate_cmd_present_export   present_export;
	struct isolate_cmd_xiso_import      xiso_import;
};

/* ── Command-buffer consumer loop (docs/design/command_buffer.md, Phase 3) ───
 *
 * Driven by ISOLATE_CMD_ENTER_LOOP: the reader thread spins on g_req_ring,
 * executes flat RM_CONTROLs inline (no worker hand-off — that latency is the
 * whole point), writes responses to g_resp_ring, and — at every drain edge —
 * polls the socket so slow-path commands are still serviced.  It exits after an
 * idle window with no ring work and no socket traffic, re-checking has_work()
 * once more (the lost-wakeup-free exit edge) before returning the request-ring
 * head (last_processed) to the caller, which ships it back in LOOP_EXITED.
 *
 * TRUST: the request ring is producer-writable by the (untrusted) guest.  Every
 * record is copied into private stack scratch and size-validated before use; a
 * malformed ring is fatal to THIS isolate only (DoS, never OOB).
 */
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40
#endif
#define NVKVM_RING_IDLE_DEFAULT   2000u   /* idle spin iterations before exit  */
/*
 * Audit F1-2: records executed between two forced control-socket polls while
 * the ring stays non-empty.  See the rationale at the use site.
 */
#define NVKVM_RING_SOCKET_POLL_EVERY 64u
#define NVKVM_RING_IDLE_MAX       1000000u /* hard cap so a huge idle_us can't
                                           * wedge guest teardown (kthread_stop
                                           * blocks on the in-flight enter_loop) */
#define NVKVM_RING_RESP_FULL_SPIN 100000u /* bound resp-ring backpressure spins */

static inline void ring_cpu_relax(void)
{
	__builtin_ia32_pause();
}

/* Reader-loop reentrancy guard (the guest pump keeps one enter_loop in flight,
 * but a stray duplicate must not recurse into a nested loop). */
static volatile int g_ring_looping;

/*
 * Write one response record to g_resp_ring.  The stub is the sole producer of
 * the response ring, so reserve/commit are race-free against the guest
 * consumer.  Spins (bounded) if the guest has not drained the ring; persistent
 * fullness is guest misbehaviour and tears down this isolate.
 */
static void ring_write_resp(uint32_t txn_id, int32_t retval, uint32_t nvstatus,
			    uint32_t flags, const void *param, uint32_t param_size,
			    const void *aux, uint32_t aux_size)
{
	uint32_t payload = (uint32_t)sizeof(struct nvkvm_ring_ioctl_resp) +
			   param_size + aux_size;
	uint64_t total;
	uint8_t *p = NULL;

	for (uint32_t spin = 0; ; spin++) {
		p = nvkvm_ring_reserve(g_resp_ring, g_ring_bytes, payload, &total);
		if (p)
			break;
		if (spin >= NVKVM_RING_RESP_FULL_SPIN)
			stub_exit(141);   /* guest not draining responses → DoS-self */
		ring_cpu_relax();
	}

	struct nvkvm_ring_ioctl_resp hdr = {
		.txn_id     = txn_id,
		.retval     = retval,
		.nvstatus   = nvstatus,
		.param_size = param_size,
		.aux_size   = aux_size,
		.flags      = flags,
	};
	__builtin_memcpy(p, &hdr, sizeof(hdr));
	if (param_size)
		__builtin_memcpy(p + sizeof(hdr), param, param_size);
	if (aux_size)
		__builtin_memcpy(p + sizeof(hdr) + param_size, aux, aux_size);
	nvkvm_ring_commit(g_resp_ring, total);
}

/*
 * Decide whether a ring-routed RM_CONTROL needs the per-control marshalling the
 * slow worker path does (embedded inner pointers/fds).  If so we PUNT — the
 * guest re-issues on the virtqueue.  PUNT means "not executed", so a
 * side-effecting control is never run twice.
 */
static int ring_ctrl_must_punt(uint32_t cmd, const void *param,
			       uint32_t param_size, uint32_t aux_size)
{
	unsigned type = (cmd >> 8) & 0xff;
	unsigned nr   = cmd & 0xff;

	/* Only flat NV_ESC_RM_CONTROL (nvos54, 32B) rides the ring. */
	if (type != 'F' || nr != 0x2a || param_size < 32)
		return 1;

	/* Inner control cmd lives at param offset 8 (nvos54.cmd).  Read it
	 * before anything else: the allowlist decision does not depend on
	 * whether there are inner params, and a record with aux_size == 0 used
	 * to skip this block entirely and run ungated (U-1).  A denied cmd is
	 * punted rather than answered here, so the denial is decided in one
	 * place — QEMU's gate on the slow path — and reported to the guest the
	 * way RM reports it. */
	uint32_t inner;
	__builtin_memcpy(&inner, (const uint8_t *)param + 8, sizeof(inner));
	if (!nvkvm_ctrl_cmd_allowed(inner))
		return 1;

	if (aux_size == 0)
		return 0;                 /* no inner params → trivially flat   */
	if (aux_size > NVKVM_RING_MAX_AUX || param_size > NVKVM_RING_MAX_PARAM)
		return 1;

	if (nvkvm_ctrl_list_entry_size(inner))   /* InfoList/Caps family        */
		return 1;
	if (inner == 0x00000101U)                /* GET_BUILD_VERSION (str ptrs)*/
		return 1;
	if (inner == 0x00003d05U)                /* EXPORT_OBJECT_TO_FD (fd)    */
		return 1;
	if (inner == 0x0080170dU)                /* FIFO_GET_CHANNELLIST (ptr)  */
		return 1;
	return 0;
}

/*
 * Execute one request record from the request ring.  The payload was already
 * peeked (pointer into the hostile ring); we copy it into private scratch,
 * validate, run the flat RM_CONTROL inline, and write the response.
 */
static void ring_exec_one(const uint8_t *pay, uint32_t len)
{
	struct nvkvm_ring_ioctl_req rq;
	uint8_t param[NVKVM_RING_MAX_PARAM];
	uint8_t aux[NVKVM_RING_MAX_AUX];

	if (len < sizeof(rq))
		return;                            /* runt record → drop silently */
	__builtin_memcpy(&rq, pay, sizeof(rq));

	if (rq.param_size > NVKVM_RING_MAX_PARAM ||
	    rq.aux_size > NVKVM_RING_MAX_AUX ||
	    (uint64_t)sizeof(rq) + rq.param_size + rq.aux_size > len) {
		ring_write_resp(rq.txn_id, -EINVAL, 0, NVKVM_RING_RESP_PUNT,
				NULL, 0, NULL, 0);
		return;
	}
	if (rq.param_size)
		__builtin_memcpy(param, pay + sizeof(rq), rq.param_size);
	if (rq.aux_size)
		__builtin_memcpy(aux, pay + sizeof(rq) + rq.param_size, rq.aux_size);

	if (ring_ctrl_must_punt(rq.cmd, param, rq.param_size, rq.aux_size)) {
		ring_write_resp(rq.txn_id, 0, 0, NVKVM_RING_RESP_PUNT,
				NULL, 0, NULL, 0);
		return;
	}

	fs_mutex_lock(&fd_mutex);
	int fd = handle_lookup(rq.handle_id);
	fs_mutex_unlock(&fd_mutex);
	if (fd < 0) {
		ring_write_resp(rq.txn_id, -EBADF, 0, 0, NULL, 0, NULL, 0);
		return;
	}

	/* Wire the inner-params pointer (nvos54.params at offset 16) to our local
	 * aux copy so the driver dereferences valid stub memory; zero it on the
	 * way back so we never leak a host VA to the guest. */
	/*
	 * U-2/U-4 fail-closed idiom applied here too.  ring_ctrl_must_punt()
	 * has already established that this record is a flat NV_ESC_RM_CONTROL
	 * (nvos54, param_size >= 32), so offset 16 is NVOS54.params and nothing
	 * else — writing 0 when there are no inner params is unambiguous.
	 * Previously an aux_size == 0 record (which must_punt explicitly
	 * ACCEPTS: "no inner params -> trivially flat") ran with the guest's own
	 * 8 bytes still in params.  This does NOT address U-1 itself, which is
	 * that this path has no allowlist at all; it only stops the pointer
	 * field from being fail-open.
	 */
	{
		uint64_t ptr_val = rq.aux_size
			? (uint64_t)(uintptr_t)aux : 0;
		__builtin_memcpy(param + 16, &ptr_val, sizeof(ptr_val));
	}

	clear_fault_addr();
	long ret = stub_ioctl(fd, rq.cmd, param);
	int  err = (ret < 0) ? (int)(-ret) : 0;

	if (rq.aux_size) {
		uint64_t zero = 0;
		__builtin_memcpy(param + 16, &zero, sizeof(zero));
	}

	uint32_t nvstatus = 0;
	__builtin_memcpy(&nvstatus, param + 28, sizeof(nvstatus)); /* nvos54@28 */

	ring_write_resp(rq.txn_id, err ? -err : (int32_t)ret, nvstatus, 0,
			param, rq.param_size, aux, rq.aux_size);
}

/* Forward decl: drain-edge socket dispatch reuses the main command handler. */
static int stub_dispatch_cmd(union stub_cmd *c, struct msghdr *msg_hdr, long n);

/*
 * Drain-edge socket poll: service at most one pending slow-path command without
 * blocking.  Returns 1 if the caller must terminate the process (EXIT/error), 0
 * otherwise (nothing pending, or a command was dispatched).
 */
static int ring_loop_poll_socket(void)
{
	union stub_cmd cmd;
	char cmsg_buf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { &cmd, sizeof(cmd) };
	struct msghdr msg_hdr = {
		.msg_iov        = &iov,
		.msg_iovlen     = 1,
		.msg_control    = cmsg_buf,
		.msg_controllen = sizeof(cmsg_buf),
	};

	long n = stub_recvmsg(SOCK_FD, &msg_hdr, MSG_DONTWAIT);
	if (n == -EAGAIN || n == -EWOULDBLOCK || n == -EINTR)
		return 0;
	if (n <= 0)
		return 1;                          /* EOF / error → terminate */
	if (n < (long)sizeof(uint32_t))
		return 1;
	return stub_dispatch_cmd(&cmd, &msg_hdr, n);
}

/*
 * The consumer loop itself.  Returns the request-ring head at exit.
 */
static uint64_t ring_consumer_loop(uint32_t idle_us)
{
	if (!g_req_ring || !g_resp_ring)
		return 0;

	uint32_t idle_budget = idle_us ? idle_us : NVKVM_RING_IDLE_DEFAULT;
	uint32_t idle = 0;
	uint32_t since_poll = 0;

	/* Cap the idle window: enter_loop stays in flight for the whole budget,
	 * and the guest pump's kthread_stop blocks on it during teardown — an
	 * unbounded value would wedge teardown.  ~1e6 iters ≈ <1s worst case. */
	if (idle_budget > NVKVM_RING_IDLE_MAX)
		idle_budget = NVKVM_RING_IDLE_MAX;

	for (;;) {
		uint8_t *pay;
		uint32_t len;
		uint64_t total;
		int rc = nvkvm_ring_peek(g_req_ring, g_ring_bytes, &pay, &len, &total);

		if (rc == NVKVM_RING_OK) {
			ring_exec_one(pay, len);
			nvkvm_ring_pop(g_req_ring, total);
			idle = 0;
			/*
			 * Audit F1-2: the ONLY socket-service edge used to be the
			 * empty-ring branch below, so a guest that kept the ring
			 * continuously fed never let this loop reach it.  Nothing
			 * in the stub is harmed by that — but QEMU is: every
			 * synchronous VMM->stub command (CLOSE_HANDLE, MMAP,
			 * MUNMAP, POLL, UNPOLL, PRESENT_EXPORT, XISO_IMPORT,
			 * REALIZE_UVM_FD, EXIT) is answered from this socket and
			 * dispatches inline on QEMU's virtio TX thread with the
			 * BQL held, so "the stub never reads its socket" is
			 * literally "an unprivileged guest process freezes the
			 * whole VMM".  Servicing on a record cadence closes that.
			 *
			 * Cadence, not per-record: this is the measured fast path,
			 * and ring_loop_poll_socket() costs a recvmsg syscall even
			 * when nothing is pending.  One extra syscall per
			 * NVKVM_RING_SOCKET_POLL_EVERY records — each of which
			 * already performs an ioctl syscall of its own — is well
			 * under 2% of the loop's syscall count, while bounding the
			 * service gap to a handful of flat RM controls (single-
			 * digit microseconds each).
			 */
			if (++since_poll >= NVKVM_RING_SOCKET_POLL_EVERY) {
				since_poll = 0;
				if (ring_loop_poll_socket())
					stub_exit(0);   /* EXIT mid-drain */
			}
			continue;
		}
		if (rc == NVKVM_RING_BAD)
			stub_exit(140);            /* corrupt ring → tear down */

		/* Ring empty → service the socket, then consider exiting. */
		since_poll = 0;
		if (ring_loop_poll_socket())
			stub_exit(0);              /* EXIT during loop */
		/* A dispatched command may have produced new ring work; re-check
		 * immediately rather than counting it as idle. */
		if (nvkvm_ring_has_work(g_req_ring)) {
			idle = 0;
			continue;
		}
		if (++idle >= idle_budget) {
			/* Exit edge: one last has_work() before committing. */
			if (nvkvm_ring_has_work(g_req_ring)) {
				idle = 0;
				continue;
			}
			return g_req_ring->head;
		}
		ring_cpu_relax();
	}
}

/*
 * Handle one received command.  Returns 1 if the reader loop should terminate
 * the process (clean EXIT or fatal framing error), 0 to continue.  Called both
 * from the main reader loop and from the consumer loop's drain-edge poll.
 */
static int stub_dispatch_cmd(union stub_cmd *c, struct msghdr *msg_hdr, long n)
{
	switch (c->type) {
	case ISOLATE_CMD_RECEIVE_FD: {
		struct cmsghdr *cm = CMSG_FIRSTHDR(msg_hdr);
		if (!cm || cm->cmsg_level != SOL_SOCKET ||
		    cm->cmsg_type != SCM_RIGHTS) {
			send_error(EINVAL);
			return 0;
		}
		int fd;
		__builtin_memcpy(&fd, CMSG_DATA(cm), sizeof(int));

		/* For UVM, drop the QEMU-owned fd and use one of our pre-opened
		 * local fds whose owning mm is the stub. */
		if (n >= (long)sizeof(struct isolate_cmd_receive_fd) &&
		    c->recv_fd.dev_id == 1 /* NVKVM_DEV_UVM */) {
			fs_mutex_lock(&uvm_local_lock);
			int local = -1;
			if (uvm_local_next_idx < NVKVM_STUB_UVM_LOCAL_POOL_SIZE)
				local = uvm_local_fds[uvm_local_next_idx++];
			fs_mutex_unlock(&uvm_local_lock);
			if (local >= 0) {
				stub_close(fd);
				fd = local;
			}
		}
		fs_mutex_lock(&fd_mutex);
		if (c->recv_fd.handle_id < MAX_HANDLES &&
		    handle_fds[c->recv_fd.handle_id] >= 0)
			stub_close(handle_fds[c->recv_fd.handle_id]);
		handle_store(c->recv_fd.handle_id, fd);
		fs_mutex_unlock(&fd_mutex);
		send_ok();
		return 0;
	}
	case ISOLATE_CMD_CLOSE_FD:
		handle_close_fd(c->close_fd.handle_id);
		return 0;
	case ISOLATE_CMD_IOCTL:
		handle_ioctl_cmd(&c->ioctl_cmd);
		return 0;
	case ISOLATE_CMD_MMAP:
		handle_mmap(&c->mmap_cmd);
		return 0;
	case ISOLATE_CMD_MUNMAP:
		handle_munmap_cmd(&c->munmap_cmd);
		return 0;
	case ISOLATE_CMD_POLL:
		/* #127: arm the host os-event fd; the reader loop ppoll()s it and
		 * sends ISOLATE_RESP_POLL_EVENT when it fires. */
		poll_arm(c->poll_cmd.handle_id);
		send_ok();
		return 0;
	case ISOLATE_CMD_UNPOLL:
		poll_disarm(c->unpoll_cmd.handle_id);
		send_ok();
		return 0;
	case ISOLATE_CMD_OPEN_DEVICE:
		handle_open_device(&c->open_dev);
		return 0;
	case ISOLATE_CMD_PRESENT_EXPORT:
		handle_present_export(&c->present_export);
		return 0;
	case ISOLATE_CMD_XISO_IMPORT:
		handle_xiso_import(&c->xiso_import, msg_hdr);
		return 0;
	case ISOLATE_CMD_REALIZE_UVM_FD:
		handle_realize_uvm_fd(&c->realize);
		return 0;
	case ISOLATE_CMD_INTERRUPT:
		interrupt_txn(c->interrupt_cmd.target_txn);
		return 0;
	case ISOLATE_CMD_SETUP_RING:
		handle_setup_ring(&c->setup_ring, msg_hdr, n);
		return 0;
	case ISOLATE_CMD_ENTER_LOOP: {
		uint64_t head;
		if (g_ring_looping || !g_req_ring) {
			/* Duplicate enter_loop (or no ring) — reply immediately so
			 * the caller's blocking virtqueue request completes. */
			head = g_req_ring ? g_req_ring->head : 0;
		} else {
			g_ring_looping = 1;
			head = ring_consumer_loop(c->enter_loop.idle_us);
			g_ring_looping = 0;
		}
		struct isolate_resp_loop_exited r = {
			.type  = ISOLATE_RESP_LOOP_EXITED,
			.error = g_req_ring ? 0 : -ENODEV,
			.head  = head,
		};
		locked_send(&r, sizeof(r));
		return 0;
	}
	case ISOLATE_CMD_EXIT:
		return 1;
	default:
		return 1;
	}
}

/* ── Seccomp ─────────────────────────────────────────────────────────────── */

/*
 * Apply the seccomp allowlist filter to the stub.  Permits only the syscalls
 * we use; everything else returns EPERM (or SIGSYS on arch mismatch).
 */
/* Instruction capacity of the filter buffer below (audit F8-1). */
#define NVKVM_SECCOMP_FILTER_MAX 96
static long apply_seccomp(void)
{
	/*
	 * Audit F8-1: EMIT used to do an unchecked filter[n++] into a bare
	 * [96] array.  Today the filter is 57 instructions (59 before the
	 * clone3 entry went), so there are 39 slots of headroom and nothing is
	 * wrong — but the failure mode if somebody adds ~20 more allowlist
	 * entries is a stack smash inside the
	 * function that BUILDS the sandbox, which is about the worst place in
	 * the tree to discover an off-by-N.  A _Static_assert cannot see `n`
	 * (it is a runtime counter over macro expansions), so the check is a
	 * runtime one that fails CLOSED: overflow stops writing, apply_seccomp
	 * returns non-zero, and main() treats any non-zero as fatal and exits.
	 * A stub that cannot build its own filter must not run unfiltered.
	 */
	struct sock_filter filter[NVKVM_SECCOMP_FILTER_MAX];
	int n = 0;
	int overflow = 0;

#define EMIT(...) do { \
	struct sock_filter _f = __VA_ARGS__; \
	if (n < (int)(sizeof(filter) / sizeof(filter[0]))) \
		filter[n++] = _f; \
	else \
		overflow = 1; \
} while (0)
#define ALLOW_IF(nr_val) do { \
	EMIT(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (nr_val), 0, 1)); \
	EMIT(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW)); \
} while (0)
/*
 * M-3: allow nr_val (mmap/mprotect) ONLY if it does NOT request PROT_EXEC at
 * all.  Plain W^X (deny only W+X together) is insufficient: an attacker can
 * mmap a page RW, write shellcode, then mprotect it R-X — each step passes W^X
 * but the result is executable attacker code.  The stub's own .text is mapped
 * executable by the ELF loader BEFORE seccomp and it never JITs (libcuda runs
 * in the guest), so no runtime mapping ever needs PROT_EXEC.  Deny it outright.
 * prot is args[2]; on no-match we fall through with nr still loaded.
 */
/*
 * Allow nr_val only when args[1] equals want.  Used for fcntl: the stub needs
 * exactly one command, F_DUPFD_CLOEXEC, to hand out private copies of the DRM
 * render-node fds QEMU parked for it (NVKVM_DRM_FD).  Every other fcntl --
 * including anything that changes file status flags or takes a lock -- stays
 * denied.  Duplicating an fd the process already holds grants no new reach,
 * which is why this is a safe thing to open up and F_SETFL would not be.
 */
#define ALLOW_IF_ARG1_EQ(nr_val, want) do { \
	EMIT(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (nr_val), 0, 4)); \
	EMIT(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, \
		      offsetof(struct seccomp_data, args[1]))); \
	EMIT(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (want), 1, 0)); \
	EMIT(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO | EPERM)); \
	EMIT(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW)); \
	EMIT(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, \
		      offsetof(struct seccomp_data, nr))); \
} while (0)

#define ALLOW_IF_NO_EXEC(nr_val) do { \
	EMIT(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (nr_val), 0, 5)); \
	EMIT(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, \
		      offsetof(struct seccomp_data, args[2]))); \
	EMIT(BPF_STMT(BPF_ALU|BPF_AND|BPF_K, PROT_EXEC)); \
	EMIT(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, PROT_EXEC, 0, 1)); \
	EMIT(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO | EPERM)); \
	EMIT(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW)); \
} while (0)

	/* Arch check */
	EMIT(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, arch)));
	EMIT(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 1, 0));
	EMIT(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS));

	/* Load nr */
	EMIT(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)));

	ALLOW_IF(__NR_read);
	ALLOW_IF(__NR_write);
	ALLOW_IF(__NR_recvmsg);
	ALLOW_IF(__NR_sendmsg);
	ALLOW_IF(__NR_ioctl);
	ALLOW_IF_NO_EXEC(__NR_mmap);
	ALLOW_IF_NO_EXEC(__NR_mprotect);
	ALLOW_IF(__NR_munmap);
	ALLOW_IF(__NR_ppoll);
	ALLOW_IF(__NR_close);
	ALLOW_IF(__NR_exit);
	ALLOW_IF(__NR_exit_group);
	ALLOW_IF(__NR_rt_sigaction);
	ALLOW_IF(__NR_rt_sigreturn);
	ALLOW_IF(__NR_futex);
	/*
	 * Audit F6-1: __NR_clone3 used to be allowed here and is deliberately
	 * gone.  Its only caller is the worker-spawn loop in main(), which
	 * finishes BEFORE apply_seccomp() runs (as does its clone(2) fallback,
	 * which this list already omits), so the entry permitted a syscall the
	 * stub never makes again — while leaving a compromised stub the one
	 * process-creation primitive it had.  Nothing in the tree caps process
	 * count (no RLIMIT_NPROC, no pids cgroup), and a PID namespace does not
	 * cap it either, so that primitive was an unbounded fork bomb against
	 * the host.  Same class of vestigial entry as the R2-L1 cleanup below.
	 */
	ALLOW_IF(__NR_openat);
	ALLOW_IF(__NR_eventfd2);
	ALLOW_IF(__NR_gettid);
	ALLOW_IF(__NR_tgkill);   /* post SIGUSR1 to interrupt a worker's ioctl (#73) */
	ALLOW_IF_ARG1_EQ(__NR_fcntl, NVKVM_F_DUPFD_CLOEXEC);
	/* R2-L1: dropped vestigial entries with no freestanding caller —
	 * clone (clone3 is used), set_robust_list, madvise, lseek, pread64,
	 * readlinkat — to shrink the post-RCE syscall surface. */

	EMIT(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO | EPERM));

#undef ALLOW_IF
#undef ALLOW_IF_ARG1_EQ
#undef EMIT

	/* F8-1: fail closed — a truncated allowlist is not a sandbox. */
	if (overflow || n <= 0)
		return -E2BIG;

	struct sock_fprog prog = {
		.len    = (unsigned short)n,
		.filter = filter,
	};
	stub_prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	/* TSYNC: apply the filter to EVERY thread in the group, not just the
	 * caller.  The worker pool is spawned before this runs (audit C-1); a
	 * non-TSYNC filter would bind to the reader thread only and leave the
	 * workers — which run all attacker-influenced ioctl handling —
	 * completely unsandboxed.  TSYNC requires every thread to already have
	 * no_new_privs, which main() sets before the worker-spawn loop. */
	return stub_seccomp(SECCOMP_SET_MODE_FILTER,
			    SECCOMP_FILTER_FLAG_TSYNC, &prog);
}

/* ── Self-relocation ─────────────────────────────────────────────────────── */

/*
 * apply_relocations() processes R_X86_64_RELATIVE entries in the dynamic
 * section.  Only needed for the NVKVM_STUB_EMBEDDED build path, where the
 * stub binary is loaded via fexecve() from a memfd and there is no dynamic
 * linker to handle RELA entries.  Disk-loaded execution lets the kernel
 * dynamic linker handle relocations before we run.
 *
 * In freestanding mode there are no C runtime constructors, so we call this
 * from main() before any global-data access matters.
 */
#ifdef NVKVM_STUB_EMBEDDED
extern char __ehdr_start[];
extern char _DYNAMIC[] __attribute__((weak));

__attribute__((no_sanitize_address))
static void apply_relocations(void)
{
	/* If the linker resolved the weak _DYNAMIC, it points at our dynamic
	 * section.  If unresolved (-no-pie build with no DT_*), it is 0 — but
	 * the compiler "knows" the symbol exists, so we route through a
	 * volatile pointer to defeat the constant-fold and produce a real
	 * load that may legitimately return 0. */
	Elf64_Dyn *volatile dynp = (Elf64_Dyn *)_DYNAMIC;
	if (!dynp) return;
	unsigned long base = (unsigned long)__ehdr_start;
	Elf64_Dyn *dyn = dynp;
	Elf64_Rela *rela = NULL;
	size_t rela_sz = 0, rela_ent = sizeof(Elf64_Rela);

	for (; dyn->d_tag != DT_NULL; dyn++) {
		switch (dyn->d_tag) {
		case DT_RELA:    rela     = (Elf64_Rela *)(base + dyn->d_un.d_ptr); break;
		case DT_RELASZ:  rela_sz  = dyn->d_un.d_val; break;
		case DT_RELAENT: rela_ent = dyn->d_un.d_val; break;
		}
	}
	if (!rela) return;
	for (size_t i = 0; i < rela_sz / rela_ent; i++) {
		Elf64_Rela *r = (Elf64_Rela *)((char *)rela + i * rela_ent);
		if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE) {
			uint64_t *target = (uint64_t *)(base + r->r_offset);
			*target = base + r->r_addend;
		}
	}
}
#endif /* NVKVM_STUB_EMBEDDED */

/* ── main ────────────────────────────────────────────────────────────────── */

#define WORKER_STACK_SIZE (128 * 1024)  /* 128 KiB per worker */

/* Tiny freestanding strcmp — argv scanning only, no libc available. */
static int stub_streq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

static int stub_strneq(const char *a, const char *b, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 0;
		if (a[i] == '\0')
			return 1;
	}
	return 1;
}

/*
 * How many DRM render-node fds QEMU actually parked at NVKVM_DRM_FD(k).
 * Fails CLOSED at 0: without this the open path dup'd NVKVM_DRM_FD(k)
 * unconditionally, and in any mode that parked nothing that descriptor number
 * was already taken by an unrelated device (measured: /dev/nvidiactl).  The
 * dup then SUCCEEDED, so the open-by-name fallback never ran and every DRM
 * ioctl went to the wrong device and came back EINVAL -- which surfaced as
 * NVIDIA's EGL refusing to claim the render node and every GL client in the
 * guest silently falling back to llvmpipe software rendering.
 */


int main(int argc, char **argv)
{
	/*
	 * The isolation mode's seccomp layer.  On by default: if QEMU passes us
	 * nothing (or an unrecognised argv), we apply the filter.  Only the
	 * explicit "--no-seccomp" turns it off, so a truncated or corrupted
	 * argv fails CLOSED.
	 */
	int want_seccomp = 1;

#ifdef NVKVM_STUB_EMBEDDED
	apply_relocations();
#endif
	for (int i = 1; i < argc && argv && argv[i]; i++) {
		if (stub_streq(argv[i], "--no-seccomp"))
			want_seccomp = 0;
		else if (stub_strneq(argv[i], "--drm-fds=", 10)) {
			const char *v = argv[i] + 10;
			unsigned n = 0;
			for (; *v >= '0' && *v <= '9'; v++)
				n = n * 10u + (unsigned)(*v - '0');
			if (*v == '\0' && n <= NVKVM_DRM_FD_MAX)
				stub_drm_fd_count = n;
		}
	}
	handle_table_init();
	job_queue_init();

	/* Detect whether QEMU handed us an O_PATH /dev dirfd (hardened, empty
	 * mount ns).  Must run before any device open below. */
	stub_detect_dev_dirfd();

	/* Reader thread reserves slot 0 in worker_tids[] so SIGSEGV from
	 * inline (non-worker) ioctls is captured against the right slot. */
	worker_tids[0] = stub_gettid();
	/* Cache our pid (== tgid) for tgkill — gettid on the main thread IS the
	 * tgid, and getpid would otherwise need a seccomp slot at runtime. */
	stub_pid = worker_tids[0];

	/* SIGSEGV handler — per-worker fault address via worker_fault_addr[] */
	struct kernel_sigaction sa = {
		.sa_handler_fn = sigsegv_handler,
		.sa_flags      = SA_SIGINFO | SA_RESTORER,
		.sa_restorer   = stub_sigreturn_trampoline,
	};
	stub_sigaction(SIGSEGV, &sa, NULL);

	/* SIGUSR1 handler — interrupt a worker's blocking ioctl (#73).  No
	 * SA_RESTART, so the ioctl returns -EINTR instead of auto-restarting.
	 * Default SIGUSR1 action is terminate, so this MUST be installed before
	 * any tgkill can arrive. */
	struct kernel_sigaction sa_usr1 = {
		.sa_handler_fn = sigusr1_handler,
		.sa_flags      = SA_RESTORER,
		.sa_restorer   = stub_sigreturn_trampoline,
	};
	stub_sigaction(SIGUSR1, &sa_usr1, NULL);

	/* Set no_new_privs BEFORE spawning workers so they inherit it at clone
	 * time — required for the TSYNC seccomp filter (audit C-1) to attach to
	 * the whole thread group below. */
	stub_prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

	/* Spawn worker threads via clone3.  Each worker gets a fresh stack
	 * and a slot id stashed in worker_tids[] for fault-addr indexing.
	 * We don't pthread_join — workers exit via SYS_exit when dequeue
	 * returns NULL (stub_exiting flag set by reader on EXIT). */
	for (int i = 0; i < NVKVM_STUB_WORKERS; i++) {
		void *stack = stub_mmap(NULL, WORKER_STACK_SIZE,
					PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS |
					MAP_GROWSDOWN, -1, 0);
		if (stack == MAP_FAILED) {
			fs_dprintf(STDERR_FD,
				"nvkvm_stub: worker stack mmap failed\n");
			stub_exit(1);
		}
		struct clone_args ca = {
			.flags = CLONE_VM | CLONE_FS | CLONE_FILES |
				 CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM,
			.stack      = (uint64_t)(uintptr_t)stack,
			.stack_size = WORKER_STACK_SIZE,
		};
		long tid = fs_clone3_run(&ca, sizeof(ca), worker_thread,
					 (void *)(uintptr_t)(i + 1));
		/*
		 * clone3 is not always reachable.  Docker's DEFAULT seccomp
		 * profile answers it with ENOSYS on purpose, so that libcs fall
		 * back to clone(2) — measured on a stock container (kernel 7.0,
		 * Seccomp:2 / Seccomp_filters:1), where a plain clone3 from any
		 * process returns ENOSYS.  With no fallback here every worker
		 * spawn failed, the stub exited, and every forwarded GPU ioctl
		 * failed with the isolate torn down immediately after creation:
		 * cuInit returned CUDA_ERROR_NO_DEVICE and nvidia-smi reported
		 * "couldn't communicate with the NVIDIA driver", with nothing in
		 * the log naming clone3 as the cause.  EPERM covers profiles
		 * that deny rather than stub the call.
		 *
		 * clone(2) takes the stack POINTER, not base+size, so hand it
		 * the (16-byte aligned) top of the same region.
		 */
		if (tid == -38 /*ENOSYS*/ || tid == -1 /*EPERM*/) {
			unsigned char *top = (unsigned char *)stack +
					     WORKER_STACK_SIZE;
			top = (unsigned char *)((uintptr_t)top & ~(uintptr_t)15);
			tid = fs_clone_run((unsigned long)ca.flags, top,
					   worker_thread,
					   (void *)(uintptr_t)(i + 1));
			if (tid >= 0 && i == 0)
				fs_dprintf(STDERR_FD,
					"nvkvm_stub: clone3 unavailable "
					"(seccomp ENOSYS); using clone(2)\n");
		}
		if (tid < 0) {
			fs_dprintf(STDERR_FD,
				"nvkvm_stub: clone3/clone worker %d failed: %ld\n",
				i, tid);
			stub_exit(1);
		}
		/* Slot 0 reserved for reader; workers occupy [1..N]. */
		worker_tids[i + 1] = (int)tid;
	}

	/* Pre-open /dev/nvidia-uvm so the stub itself owns the file's mm
	 * context.  UVM_MM_INITIALIZE only links files whose owner mm matches
	 * the calling task; without this, fds passed via SCM_RIGHTS from
	 * QEMU get rejected with NV_ERR_INVALID_ARGUMENT. */
	for (int i = 0; i < NVKVM_STUB_UVM_LOCAL_POOL_SIZE; i++)
		uvm_local_fds[i] = (int)stub_open_dev("nvidia-uvm",
						O_RDWR | O_CLOEXEC);

	/*
	 * Apply the seccomp allowlist before entering the main loop.  After
	 * this point only the explicitly-allowed syscalls work; anything
	 * else returns -EPERM (or, for the arch-mismatch case, kills the
	 * process).  Audit C6/M-3: the allowlist blocks execve, ptrace, prctl,
	 * init_module, etc. — the dangerous escape primitives — and the
	 * mmap/mprotect entries now enforce W^X (no PROT_WRITE|PROT_EXEC), so a
	 * compromised stub cannot map RWX to inject code.  (openat remains bounded
	 * by the post-pivot /dev dirfd sandbox, which seccomp can't path-filter.)
	 *
	 * Audit F6-1: it now blocks PROCESS CREATION too, which this comment
	 * used to claim while __NR_clone3 sat in the allowlist — clone3 is a
	 * superset of fork, so the claim was false.  fork/vfork/clone/clone3 are
	 * all absent, and every legitimate clone in this process (the worker
	 * pool, a few lines above) has already happened by the time we get here.
	 *
	 * The NVKVM_STUB_NO_SECCOMP env hatch was dropped along with libc:
	 * the parent calls clearenv() before exec so there is no environment
	 * to inspect anyway.  Re-added via argv, as that comment anticipated:
	 * QEMU passes "--no-seccomp" for the isolation modes whose seccomp
	 * layer is off ('none', and only 'none' among the presets).  Absence
	 * of the flag means apply it, so a mangled argv fails closed.
	 */
	if (!want_seccomp) {
		fs_dprintf(STDERR_FD,
			"nvkvm_stub: SECCOMP FILTER DISABLED by --no-seccomp; "
			"this isolate has NO syscall confinement\n");
	} else {
		long sr = apply_seccomp();
		/* R2-L2: TSYNC reports a per-thread sync failure as a POSITIVE
		 * return (the offending tid) and applies the filter to nothing —
		 * treat any non-zero as fatal, not just negative. */
		if (sr != 0) {
			fs_dprintf(STDERR_FD,
				"nvkvm_stub: apply_seccomp failed: %ld\n",
				sr);
			stub_exit(1);
		}
	}

	/*
	 * Reader loop — reads ONE complete SEQPACKET message per iteration.
	 *
	 * SOCK_SEQPACKET preserves message boundaries: a single read() consumes
	 * exactly one send() and discards any excess bytes if the buffer is
	 * smaller than the message.  The old split-read approach (read type,
	 * then read the rest) therefore discarded the body of every message.
	 *
	 * Fix: use recvmsg() with a union buffer large enough for any command
	 * struct, so the entire header arrives in one call.  Ancillary data
	 * (SCM_RIGHTS for RECEIVE_FD) is handled inline.
	 *
	 * Variable-length param/aux blobs for IOCTL are sent as separate
	 * SEQPACKET messages by QEMU and are still read individually below.
	 */
	for (;;) {
		union stub_cmd cmd;
		char cmsg_buf[CMSG_SPACE(sizeof(int))];

		/* #127: if any os-event fds are armed, wait on them alongside the
		 * control socket and relay the ones that fire, then read a command
		 * only if the socket is ready.  When nothing is armed (the common
		 * case) this is skipped and we block in recvmsg exactly as before —
		 * zero behaviour change for the existing fast path. */
		fs_mutex_lock(&poll_lock);
		int armed = poll_n;
		fs_mutex_unlock(&poll_lock);
		if (armed > 0) {
			struct stub_pollfd pfds[1 + NVKVM_POLL_MAX];
			pfds[0].fd = SOCK_FD; pfds[0].events = STUB_POLLIN; pfds[0].revents = 0;
			int nf = 1;
			fs_mutex_lock(&poll_lock);
			for (int i = 0; i < poll_n && nf <= NVKVM_POLL_MAX; i++) {
				pfds[nf].fd = poll_fds[i];
				pfds[nf].events = STUB_POLLIN;
				pfds[nf].revents = 0;
				nf++;
			}
			fs_mutex_unlock(&poll_lock);
			long pr = stub_ppoll(pfds, (unsigned long)nf);
			if (pr < 0) {
				if (pr == -EINTR) continue;
				break;
			}
			for (int i = 1; i < nf; i++) {
				if (!pfds[i].revents) continue;
				uint32_t h = 0;
				fs_mutex_lock(&poll_lock);
				for (int j = 0; j < poll_n; j++)
					if (poll_fds[j] == pfds[i].fd) { h = poll_handles[j]; break; }
				fs_mutex_unlock(&poll_lock);
				if (h) {
					struct isolate_resp_poll_event ev;
					ev.type      = ISOLATE_RESP_POLL_EVENT;
					ev.handle_id = h;
					ev.revents   = (uint32_t)(unsigned short)pfds[i].revents;
					ev.reserved  = 0;
					locked_send(&ev, sizeof(ev));
					poll_disarm(h);   /* one-shot; guest re-arms on next poll() */
				}
			}
			if (!(pfds[0].revents & STUB_POLLIN))
				continue;   /* only os-events fired; no command to read yet */
		}

		struct iovec iov = { &cmd, sizeof(cmd) };
		struct msghdr msg_hdr = {
			.msg_iov        = &iov,
			.msg_iovlen     = 1,
			.msg_control    = cmsg_buf,
			.msg_controllen = sizeof(cmsg_buf),
		};

		long n = stub_recvmsg(SOCK_FD, &msg_hdr, 0);
		if (n == -EINTR)
			continue;   /* stray SIGUSR1; not EOF/error */
		if (n <= 0)
			break;
		if (n < (long)sizeof(uint32_t))
			goto done;
		if (stub_dispatch_cmd(&cmd, &msg_hdr, n))
			goto done;
	}


done:
	stub_exiting = 1;
	fs_cond_broadcast(&queue_cond);
	/* No pthread_join — workers and the main thread share an mm, so the
	 * exit_group(0) below tears down all of them atomically.  Any
	 * in-flight ioctl completes before the kernel reaps the worker. */
	stub_exit(0);
}
