/* SPDX-License-Identifier: GPL-2.0 */
/*
 * stub_clone3.S — minimal clone3 trampoline for the freestanding stub.
 *
 * x86_64 only.  Caller signature:
 *
 *   long fs_clone3_run(struct clone_args *args, size_t args_sz,
 *                       void (*entry)(void *arg), void *arg);
 *
 * Returns child tid (>0) in parent, never returns in child (entry must end
 * with SYS_exit; if it returns, we fall through to SYS_exit ourselves).
 *
 * Implementation: pin entry + arg into r12/r13 (callee-saved by SysV ABI,
 * also unmodified by the syscall instruction itself) so they survive the
 * clone3 syscall in the child's new context.  The kernel sets the child's
 * RSP to args->stack + args->stack_size before returning to userspace.
 *
 * This is the "10% of uwgsocks/preload/core/clone3_asm.S" version — no
 * sigaltstack, no SIGSYS, no futex wait between parent and child.
 */
	.text
	/*
	 * _start — process entry point.  No libc, so we set up the call to
	 * main() ourselves: zero the frame pointer (so unwinders stop here)
	 * and call main.  On return, exit_group(main's return value).
	 *
	 * The kernel sets rsp to argc, then argv[0..argc-1], NULL, envp[]...
	 * main() takes (argc, argv): QEMU passes "--no-seccomp" there when the
	 * configured isolation mode has the seccomp layer switched off.  argv
	 * is the mechanism this file's own comment nominated for a debug hatch
	 * (the environment is deliberately cleared before exec), and it has the
	 * useful property of showing up in ps — a stub running without seccomp
	 * cannot hide.  Read argc/argv BEFORE realigning rsp, which destroys
	 * the pointer to them.
	 */
	.globl	_start
	.type	_start, @function
_start:
	xorq	%rbp, %rbp		/* clear frame pointer */
	movq	(%rsp), %rdi		/* argc                */
	leaq	8(%rsp), %rsi		/* argv                */
	andq	$-16, %rsp		/* 16-byte align stack */
	callq	main
	movq	%rax, %rdi		/* exit code = main's return */
	movq	$231, %rax		/* SYS_exit_group */
	syscall
	ud2
	.size	_start, .-_start

	.globl	fs_clone3_run
	.type	fs_clone3_run, @function
fs_clone3_run:
	/* save callee-saved regs we use */
	pushq	%r12
	pushq	%r13

	movq	%rdx, %r12		/* entry_fn */
	movq	%rcx, %r13		/* entry_arg */

	movq	$435, %rax		/* SYS_clone3 */
	/* %rdi = args, %rsi = args_sz already set by caller (sysv ABI) */
	syscall

	testq	%rax, %rax
	js	.Lparent_err		/* clone3 returned negative errno */
	jnz	.Lparent_ok		/* clone3 returned tid > 0 in parent */

	/* CHILD: rsp is now args->stack + args->stack_size (kernel set it).
	 * r12 = entry, r13 = arg.  Call entry(arg). */
	movq	%r13, %rdi
	callq	*%r12
	/* If entry returns, exit the thread.  SYS_exit (60), arg = 0. */
	movq	$60, %rax
	xorq	%rdi, %rdi
	syscall
	/* unreachable */
	ud2

.Lparent_ok:
.Lparent_err:
	popq	%r13
	popq	%r12
	retq
	.size	fs_clone3_run, .-fs_clone3_run

	.globl	fs_clone_run
	.type	fs_clone_run, @function
/*
 * fs_clone_run — legacy clone(2) fallback for fs_clone3_run.
 *
 * Caller signature:
 *   long fs_clone_run(unsigned long flags, void *child_stack_top,
 *                     void (*entry)(void *arg), void *arg);
 *
 * Needed because Docker's default seccomp profile answers clone3 with
 * ENOSYS (deliberately, so that glibc falls back to clone) — a freestanding
 * stub that only knows clone3 cannot spawn a single worker there, and every
 * GPU ioctl then fails.  See nvkvm_stub.c's worker spawn loop.
 *
 * clone(2) on x86_64 is syscall 56 with
 *   rdi = flags, rsi = child_stack, rdx = ptid, r10 = ctid, r8 = tls
 * and, unlike clone3, the CHILD returns from the syscall with rax = 0 and
 * rsp = child_stack (which must therefore be the TOP of the region, already
 * 16-byte aligned).  The child-side sequence below is identical to the
 * clone3 path's.
 */
fs_clone_run:
	pushq	%r12
	pushq	%r13

	movq	%rdx, %r12		/* entry_fn  */
	movq	%rcx, %r13		/* entry_arg */

	/* rdi = flags and rsi = child_stack are already in place. */
	xorq	%rdx, %rdx		/* ptid = NULL */
	xorq	%r10, %r10		/* ctid = NULL */
	xorq	%r8,  %r8		/* tls  = NULL */
	movq	$56, %rax		/* SYS_clone   */
	syscall

	testq	%rax, %rax
	js	.Lc_parent		/* negative errno in parent */
	jnz	.Lc_parent		/* tid > 0 in parent        */

	/* CHILD: rsp = child_stack (we supplied it), r12 = entry, r13 = arg. */
	xorq	%rbp, %rbp		/* stop unwinders here */
	movq	%r13, %rdi
	callq	*%r12
	/* If entry returns, exit just this thread. */
	movq	$60, %rax		/* SYS_exit */
	xorq	%rdi, %rdi
	syscall
	ud2

.Lc_parent:
	popq	%r13
	popq	%r12
	retq
	.size	fs_clone_run, .-fs_clone_run

	/* Mark the stack non-executable so the static binary has no
	 * PT_GNU_STACK with X bit set. */
	.section .note.GNU-stack,"",@progbits
