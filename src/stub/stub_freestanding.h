/* SPDX-License-Identifier: GPL-2.0 */
/*
 * stub_freestanding.h — no-libc runtime primitives for the nvkvm stub.
 *
 * The stub builds with -nostdlib -static -fPIE so its address-space layout
 * is a tiny, predictable blob that can be fexecve'd into a fresh process
 * with nothing inherited from QEMU.  This header replaces libc bits the
 * stub needs:
 *
 *   - syscall macros (no errno; negative return = -errno, kernel convention)
 *   - futex-based mutex / cond / once
 *   - raw clone3 thread spawn (no pthread)
 *   - tiny dprintf-style writer (for diagnostics)
 *
 * No TLS.  fault_addr tracking lives in a per-worker slot indexed by the
 * worker's own thread id stash, not glibc TLS.
 *
 * x86_64 only — aarch64 can be added later.
 */
#ifndef NVKVM_STUB_FREESTANDING_H
#define NVKVM_STUB_FREESTANDING_H

#include <stdint.h>
#include <stddef.h>
#include <linux/futex.h>

/* ── Inline syscalls — kernel convention: negative return = -errno ──────── */

static inline long sc0(long n)
{
	long r;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n) : "rcx", "r11", "memory");
	return r;
}
static inline long sc1(long n, long a)
{
	long r;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a) : "rcx", "r11", "memory");
	return r;
}
static inline long sc2(long n, long a, long b)
{
	long r;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
	return r;
}
static inline long sc3(long n, long a, long b, long c)
{
	long r;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c)
		: "rcx", "r11", "memory");
	return r;
}
static inline long sc4(long n, long a, long b, long c, long d)
{
	long r;
	register long r10 __asm__("r10") = d;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
		: "rcx", "r11", "memory");
	return r;
}
static inline long sc5(long n, long a, long b, long c, long d, long e)
{
	long r;
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
		: "rcx", "r11", "memory");
	return r;
}
static inline long sc6(long n, long a, long b, long c, long d, long e, long f)
{
	long r;
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	register long r9  __asm__("r9")  = f;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory");
	return r;
}

/* ── Futex-based mutex / cond ───────────────────────────────────────────── */

/* States: 0 = unlocked, 1 = locked uncontended, 2 = locked with waiters. */
struct fs_mutex { int v; };
#define FS_MUTEX_INIT { 0 }

static inline void fs_mutex_lock(struct fs_mutex *m)
{
	int c;
	c = __atomic_load_n(&m->v, __ATOMIC_RELAXED);
	if (c == 0 &&
	    __atomic_compare_exchange_n(&m->v, &c, 1, 0,
					__ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	while (1) {
		int prev = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
		if (prev == 0)
			return;
		sc6(202 /*SYS_futex*/, (long)&m->v, FUTEX_WAIT_PRIVATE,
		    2, 0, 0, 0);
	}
}

static inline void fs_mutex_unlock(struct fs_mutex *m)
{
	int prev = __atomic_exchange_n(&m->v, 0, __ATOMIC_RELEASE);
	if (prev == 2)
		sc6(202 /*SYS_futex*/, (long)&m->v, FUTEX_WAKE_PRIVATE,
		    1, 0, 0, 0);
}

/* Generation-counter condvar.  Waiter samples gen, drops mutex, futex_waits
 * for gen change, reacquires mutex.  Broadcast: increment gen, wake all. */
struct fs_cond { uint32_t gen; };
#define FS_COND_INIT { 0 }

static inline void fs_cond_wait(struct fs_cond *c, struct fs_mutex *m)
{
	uint32_t gen = __atomic_load_n(&c->gen, __ATOMIC_RELAXED);
	fs_mutex_unlock(m);
	sc6(202 /*SYS_futex*/, (long)&c->gen, FUTEX_WAIT_PRIVATE,
	    (long)gen, 0, 0, 0);
	fs_mutex_lock(m);
}

static inline void fs_cond_signal(struct fs_cond *c)
{
	__atomic_fetch_add(&c->gen, 1, __ATOMIC_RELEASE);
	sc6(202 /*SYS_futex*/, (long)&c->gen, FUTEX_WAKE_PRIVATE,
	    1, 0, 0, 0);
}

static inline void fs_cond_broadcast(struct fs_cond *c)
{
	__atomic_fetch_add(&c->gen, 1, __ATOMIC_RELEASE);
	sc6(202 /*SYS_futex*/, (long)&c->gen, FUTEX_WAKE_PRIVATE,
	    0x7fffffff, 0, 0, 0);
}

/* One-shot init via atomic state machine. */
struct fs_once { int v; };
#define FS_ONCE_INIT { 0 }

static inline void fs_once(struct fs_once *o, void (*fn)(void))
{
	int expected = 0;
	if (__atomic_compare_exchange_n(&o->v, &expected, 1, 0,
					__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
		fn();
		__atomic_store_n(&o->v, 2, __ATOMIC_RELEASE);
		sc6(202 /*SYS_futex*/, (long)&o->v, FUTEX_WAKE_PRIVATE,
		    0x7fffffff, 0, 0, 0);
		return;
	}
	while (__atomic_load_n(&o->v, __ATOMIC_ACQUIRE) != 2)
		sc6(202 /*SYS_futex*/, (long)&o->v, FUTEX_WAIT_PRIVATE,
		    1, 0, 0, 0);
}

/* ── Worker thread spawn via clone3 ─────────────────────────────────────── */

struct clone_args {
	uint64_t flags;
	uint64_t pidfd;
	uint64_t child_tid;
	uint64_t parent_tid;
	uint64_t exit_signal;
	uint64_t stack;
	uint64_t stack_size;
	uint64_t tls;
};

/* SYS_clone3 = 435 on x86_64; stack provided by caller. */
#define FS_CLONE3_NR 435

/*
 * fs_clone3_run — spawn a worker thread that runs `entry(arg)` on the
 * supplied stack.  Implemented in stub_clone3.S.  Returns child tid in
 * the parent.  Entry function must end with SYS_exit; if it returns the
 * trampoline calls SYS_exit for it.
 *
 * args->stack must point at the BASE of the allocated region; the kernel
 * sets the child's RSP to (stack + stack_size) on x86_64.
 * Required flags (caller-supplied): CLONE_VM | CLONE_FS | CLONE_FILES |
 *   CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM.
 */
extern long fs_clone3_run(struct clone_args *args, size_t args_sz,
			  void (*entry)(void *arg), void *arg);

/*
 * fs_clone_run — legacy clone(2) spawn, used only when clone3 is unavailable.
 *
 * Docker's default seccomp profile returns ENOSYS for clone3 on purpose, so
 * that libcs fall back to clone(2).  Without this fallback the stub cannot
 * start a worker inside a stock container and every forwarded ioctl fails.
 *
 * Unlike fs_clone3_run, child_stack_top is the TOP of the region (clone(2)
 * takes the stack pointer directly, not base+size) and must be 16-byte
 * aligned.  Same flags, same entry contract.
 */
extern long fs_clone_run(unsigned long flags, void *child_stack_top,
			 void (*entry)(void *arg), void *arg);

/* ── Tiny formatted output (no libc) ────────────────────────────────────────
 *
 * Supports just the conversions the stub actually uses:
 *   %d   signed int
 *   %u   unsigned int
 *   %x   unsigned int, lowercase hex
 *   %lx  unsigned long, lowercase hex
 *   %ld  signed long
 *   %lu  unsigned long
 *   %llx unsigned long long, lowercase hex
 *   %lld signed long long
 *   %llu unsigned long long
 *   %p   pointer (printed as 0x%lx)
 *   %s   const char *
 *   %c   single character
 *   %%   literal '%'
 * Width / precision are not supported — keep call sites simple.
 */

struct fs_fmt_sink {
	char  *buf;     /* may be NULL for "discard" sink (count-only) */
	size_t cap;     /* total bytes in buf incl. terminator */
	size_t pos;     /* bytes written so far excl. terminator */
};

static inline void fs__emit(struct fs_fmt_sink *s, char c)
{
	if (s->buf && s->pos + 1 < s->cap)
		s->buf[s->pos] = c;
	s->pos++;
}

static inline void fs__emit_str(struct fs_fmt_sink *s, const char *p)
{
	if (!p) p = "(null)";
	while (*p) fs__emit(s, *p++);
}

static inline void fs__emit_u64(struct fs_fmt_sink *s, unsigned long long v,
				unsigned base, int upper)
{
	char tmp[32];
	int n = 0;
	if (v == 0) { fs__emit(s, '0'); return; }
	while (v && n < (int)sizeof(tmp)) {
		unsigned d = (unsigned)(v % base);
		tmp[n++] = (d < 10) ? (char)('0' + d) :
			   (char)((upper ? 'A' : 'a') + (d - 10));
		v /= base;
	}
	while (n--) fs__emit(s, tmp[n]);
}

static inline void fs__emit_i64(struct fs_fmt_sink *s, long long v)
{
	if (v < 0) { fs__emit(s, '-'); v = -v; }
	fs__emit_u64(s, (unsigned long long)v, 10, 0);
}

static inline size_t fs_vformat(struct fs_fmt_sink *s, const char *fmt,
				__builtin_va_list ap)
{
	const char *p = fmt;
	while (*p) {
		if (*p != '%') { fs__emit(s, *p++); continue; }
		p++;
		int lng = 0;
		while (*p == 'l') { lng++; p++; }
		switch (*p) {
		case 'd':
			if (lng >= 2) fs__emit_i64(s,
				__builtin_va_arg(ap, long long));
			else if (lng == 1) fs__emit_i64(s,
				(long long)__builtin_va_arg(ap, long));
			else fs__emit_i64(s,
				(long long)__builtin_va_arg(ap, int));
			break;
		case 'u':
			if (lng >= 2) fs__emit_u64(s,
				__builtin_va_arg(ap, unsigned long long), 10, 0);
			else if (lng == 1) fs__emit_u64(s,
				(unsigned long long)__builtin_va_arg(ap, unsigned long),
				10, 0);
			else fs__emit_u64(s,
				(unsigned long long)__builtin_va_arg(ap, unsigned),
				10, 0);
			break;
		case 'x':
			if (lng >= 2) fs__emit_u64(s,
				__builtin_va_arg(ap, unsigned long long), 16, 0);
			else if (lng == 1) fs__emit_u64(s,
				(unsigned long long)__builtin_va_arg(ap, unsigned long),
				16, 0);
			else fs__emit_u64(s,
				(unsigned long long)__builtin_va_arg(ap, unsigned),
				16, 0);
			break;
		case 'p': {
			void *v = __builtin_va_arg(ap, void *);
			fs__emit(s, '0'); fs__emit(s, 'x');
			fs__emit_u64(s, (unsigned long long)(uintptr_t)v, 16, 0);
			break;
		}
		case 's':
			fs__emit_str(s, __builtin_va_arg(ap, const char *));
			break;
		case 'c':
			fs__emit(s, (char)__builtin_va_arg(ap, int));
			break;
		case '%':
			fs__emit(s, '%');
			break;
		default:
			fs__emit(s, '%');
			if (*p) fs__emit(s, *p);
			break;
		}
		if (*p) p++;
	}
	if (s->buf && s->cap > 0) {
		size_t term = (s->pos < s->cap) ? s->pos : (s->cap - 1);
		s->buf[term] = '\0';
	}
	return s->pos;
}

__attribute__((format(printf, 3, 4)))
static inline int fs_snprintf(char *buf, size_t cap, const char *fmt, ...)
{
	__builtin_va_list ap;
	struct fs_fmt_sink s = { .buf = buf, .cap = cap, .pos = 0 };
	__builtin_va_start(ap, fmt);
	size_t n = fs_vformat(&s, fmt, ap);
	__builtin_va_end(ap);
	return (int)n;
}

/* fs_dprintf(fd, fmt, ...) — writes formatted output to fd via SYS_write.
 * Uses a fixed 512-byte stack buffer; longer messages are truncated. */
__attribute__((format(printf, 2, 3)))
static inline int fs_dprintf(int fd, const char *fmt, ...)
{
	char buf[512];
	__builtin_va_list ap;
	struct fs_fmt_sink s = { .buf = buf, .cap = sizeof(buf), .pos = 0 };
	__builtin_va_start(ap, fmt);
	(void)fs_vformat(&s, fmt, ap);
	__builtin_va_end(ap);
	size_t out = (s.pos < sizeof(buf) - 1) ? s.pos : (sizeof(buf) - 1);
	long r = sc3(1 /*SYS_write*/, (long)fd, (long)buf, (long)out);
	return (int)r;
}

#endif /* NVKVM_STUB_FREESTANDING_H */
