/*
 * handle_outlives_creator_session.c -- does a shared handle survive its
 * CREATING process exiting while a sibling's isolate still references it?
 *
 * Object-keyed sharing (fix/cow-private-registration) made a QEMU-side
 * handle's lifetime cross sessions for the first time: session A creates the
 * handle (migrates the buffer, opens the memfd), session B's isolate maps
 * that SAME handle via the share path. If session A's process then exits
 * while B is still using it, nvkvm_handle_close_session() must NOT force-
 * close the fd out from under B's isolate -- that would be a live
 * use-after-free in QEMU's own process, not merely a guest-visible error.
 *
 * Shape, made deterministic (no reliance on which sibling happens to win
 * the object-keyed claim, unlike fork_both_register.c):
 *
 *   1. main() forks a HELPER first. The helper alone registers
 *      B = MAP_SHARED|MAP_ANONYMOUS -- no concurrent claimant exists yet, so
 *      it is guaranteed to be the CREATOR (the real pin-and-relocate path,
 *      not the share path). It signals success over a pipe and _exit()s
 *      immediately -- no cuMemHostUnregister, no munmap, no cleanup. That
 *      tears its session down (nvkvm_session_destroy) with a live sibling
 *      isolate still referencing its handle.
 *   2. main() (the SURVIVOR) waitpid()s the helper -- by the time that
 *      returns, the guest kernel has already run the helper's exit_files()
 *      fd-close path, which is what drives the host-side session teardown,
 *      so the race is already armed, not merely "helper is exiting".
 *   3. THEN, and only then, main() brings its own CUDA context up and
 *      registers the SAME B -- with the helper's session already gone, this
 *      must take the SHARE path against a handle whose creating session no
 *      longer exists.
 *   4. main() writes two different patterns through its own registered
 *      pointer and reads them back. If the handle's fd was force-closed
 *      underneath it, either the mapping never worked (register fails) or a
 *      UAF/recycled-fd would show up as wrong data on readback.
 *
 * PASS: helper registered and exited cleanly, main's registration (the share
 *       path, against an already-dead creating session) succeeded, and both
 *       readbacks are byte-exact.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

typedef int CUresult; typedef int CUdevice; typedef void *CUcontext;
extern CUresult cuInit(unsigned);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuCtxCreate_v2(CUcontext *, unsigned, CUdevice);
extern CUresult cuMemHostRegister_v2(void *, size_t, unsigned);
extern CUresult cuMemHostUnregister(void *);

#define LEN   (64u * 1024u)
#define PAT1  0xD00DFEEDu
#define PAT2  0xCAFEF00Du
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02

static unsigned count_eq(volatile unsigned *p, unsigned v)
{
	unsigned n = 0;
	for (unsigned i = 0; i < LEN/4; i++) if (p[i] == v) n++;
	return n;
}

int main(void)
{
	volatile unsigned *B = mmap(NULL, LEN, PROT_READ|PROT_WRITE,
				    MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (B == MAP_FAILED) { perror("mmap"); return 2; }
	/*
	 * Deliberately NOT touched here. Linux does not eagerly copy PTEs for
	 * a shared file-backed VMA at fork() (the same fact
	 * fork_both_register.c's bug hinged on) -- but if THIS process wrote
	 * to B before forking, its own PTE would already be resident, and the
	 * helper's otherwise-sole registration below would see mapcount 2
	 * with nobody (yet) waiting to explain it, refusing for the same
	 * reason shared_view_desync.c's parent is refused. Untouched, this
	 * process contributes nothing until it deliberately registers, well
	 * after the helper is gone -- exactly the shape this test wants.
	 */

	int pipefd[2];
	if (pipe(pipefd)) { perror("pipe"); return 2; }

	pid_t pid = fork();
	if (pid < 0) { perror("fork"); return 2; }

	if (pid == 0) {                          /* ---- helper: the CREATOR ---- */
		CUdevice d; CUcontext c;
		unsigned char ok = 0;
		if (!(cuInit(0) || cuDeviceGet(&d, 0) || cuCtxCreate_v2(&c, 0, d)))
			ok = (cuMemHostRegister_v2((void *)B, LEN,
						   CU_MEMHOSTREGISTER_DEVICEMAP) == 0);
		printf("  helper: cuda+register B = %s -- exiting now, no cleanup\n",
		       ok ? "OK" : "FAILED");
		fflush(stdout);
		if (write(pipefd[1], &ok, 1) != 1) _exit(3);
		/* THE POINT: exit immediately. No cuMemHostUnregister, no
		 * munmap. _exit(), not return, so no atexit/libc teardown
		 * that might itself touch CUDA state runs either. */
		_exit(ok ? 0 : 1);
	}

	/* ---- main (this process): the SURVIVOR, registers only AFTER the
	 * helper -- and its session -- are gone. ---- */
	unsigned char helper_ok = 0;
	if (read(pipefd[0], &helper_ok, 1) != 1) {
		printf("  main  : helper died before registering\n");
		return 2;
	}
	int st = 0;
	waitpid(pid, &st, 0);
	int helper_exit = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
	printf("  main  : helper reaped, exit=%d (registered=%d)\n",
	       helper_exit, helper_ok);
	if (!helper_ok) {
		printf("VERDICT: could not set up the shape -- helper's own "
		       "registration failed\n");
		return 2;
	}
	/* Small extra margin past waitpid for any async host-side teardown
	 * work beyond the guest-side fd close waitpid already ordered after. */
	usleep(50 * 1000);

	CUdevice d; CUcontext c;
	int pc = !(cuInit(0) || cuDeviceGet(&d, 0) || cuCtxCreate_v2(&c, 0, d));
	int rb = pc && (cuMemHostRegister_v2((void *)B, LEN,
					     CU_MEMHOSTREGISTER_DEVICEMAP) == 0);
	printf("  main  : cuda=%d register B (share, creator already dead) = %s\n",
	       pc, rb ? "OK" : "FAILED");
	if (!rb) {
		printf("VERDICT: FAIL -- share against a dead creating session "
		       "was refused\n");
		return 1;
	}

	for (unsigned i = 0; i < LEN/4; i++) B[i] = PAT1;
	unsigned seen1 = count_eq(B, PAT1);
	for (unsigned i = 0; i < LEN/4; i++) B[i] = PAT2;
	unsigned seen2 = count_eq(B, PAT2);
	printf("  main  : wrote PAT1, read back %u/%u; wrote PAT2, read back %u/%u\n",
	       seen1, LEN/4, seen2, LEN/4);

	cuMemHostUnregister((void *)B);

	int ok = (seen1 == LEN/4 && seen2 == LEN/4);
	printf("\nVERDICT: %s\n", ok
	       ? "PASS -- handle survived its creating session's exit"
	       : "FAIL -- readback wrong after the creating session exited "
		 "(stale/UAF fd?)");
	return ok ? 0 : 1;
}
