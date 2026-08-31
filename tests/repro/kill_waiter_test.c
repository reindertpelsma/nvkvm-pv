/*
 * kill_waiter_test.c -- adversarial probe for the object-keyed sharing
 * mapcount-credit mechanism (nvkvm_shared_resolve() / shared_claim->refs,
 * src/guest/nvkvm_mmap.c). See
 * docs/investigations/shared-registration-two-processes/README.md for the
 * full safety analysis this backs.
 *
 * Question: can a "credited waiter" (a process that found a pending claim
 * and incremented its refs, letting the claimant relocate past a mapcount
 * this waiter is responsible for) walk away -- e.g. get killed -- WITHOUT
 * ever reconciling its own view (share or re-claim), leaving a stranded
 * view behind that the claimant wrongly relocated past?
 *
 * Shape: CLAIMANT registers a LARGE (32 MiB) MAP_SHARED buffer so its own
 * migrate_range() call takes a real, observable amount of wall-clock time
 * (multiple 2 MiB chunks, each a virtio round trip). WAITER, started
 * shortly after, registers the SAME buffer -- it should find the claimant's
 * pending claim and block in nvkvm_shared_resolve()'s wait_for_completion(),
 * which is UNINTERRUPTIBLE (does not respond to signals). We then:
 *   1. poll for WAITER to reach D (uninterruptible sleep) state,
 *   2. SIGKILL it,
 *   3. confirm it is STILL ALIVE (not yet reaped) a moment later --
 *      SIGKILL must NOT take effect while it is credited/asleep,
 *   4. wait for it to actually exit, and confirm it did (it can only exit
 *      after wait_for_completion() returns, i.e. after the claim resolves
 *      and it re-decides -- share or re-claim -- exactly once).
 *
 * PASS: WAITER is observed in D state, survives the SIGKILL while
 *       credited, and only exits after the claimant is done.
 * INCONCLUSIVE (exit 3): the waiter was never observed in D state -- a
 *       timing miss, not a finding about the mechanism. Re-run.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

typedef int CUresult; typedef int CUdevice; typedef void *CUcontext;
extern CUresult cuInit(unsigned);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuCtxCreate_v2(CUcontext *, unsigned, CUdevice);
extern CUresult cuMemHostRegister_v2(void *, size_t, unsigned);
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02
#define LEN (32u * 1024u * 1024u)   /* 32 MiB -> 16 x 2 MiB chunks */

static int proc_state(pid_t pid)
{
	char path[64], line[256];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
	fclose(f);
	char *rp = strrchr(line, ')');
	if (!rp) return -1;
	return rp[2];
}

/* Poll until the waiter enters D state, or give up after ~2s. */
static int wait_for_D(pid_t pid)
{
	for (int i = 0; i < 400; i++) {
		int s = proc_state(pid);
		if (s == 'D') return s;
		if (s < 0 || s == 'Z') return s;
		usleep(5 * 1000);
	}
	return proc_state(pid);
}

int main(void)
{
	volatile unsigned *B = mmap(NULL, LEN, PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (B == MAP_FAILED) { perror("mmap"); return 2; }

	int p_ready[2];
	if (pipe(p_ready)) { perror("pipe"); return 2; }

	pid_t claimant = fork();
	if (claimant < 0) { perror("fork"); return 2; }
	if (claimant == 0) {
		CUdevice d; CUcontext c;
		if (cuInit(0) || cuDeviceGet(&d, 0) || cuCtxCreate_v2(&c, 0, d))
			_exit(2);
		char x = 1;
		if (write(p_ready[1], &x, 1) != 1) _exit(3);
		CUresult rc = cuMemHostRegister_v2((void *)B, LEN,
						   CU_MEMHOSTREGISTER_DEVICEMAP);
		printf("  claimant: register (32 MiB) rc=%d\n", rc);
		fflush(stdout);
		_exit(rc == 0 ? 0 : 1);
	}
	char x;
	if (read(p_ready[0], &x, 1) != 1) {
		fprintf(stderr, "claimant died early\n");
		return 2;
	}
	/* Claimant has a live CUDA context and is about to call register();
	 * give it a moment to actually enter migrate_range() and post its
	 * claim before the waiter's own (independent) CUDA context creation
	 * finishes -- context creation can itself take longer than a 2 MiB
	 * chunk upload, so without this the waiter can occasionally race
	 * ahead and become the claimant instead of the waiter. */
	usleep(5 * 1000);

	pid_t waiter = fork();
	if (waiter < 0) { perror("fork"); return 2; }
	if (waiter == 0) {
		CUdevice d; CUcontext c;
		if (cuInit(0) || cuDeviceGet(&d, 0) || cuCtxCreate_v2(&c, 0, d))
			_exit(2);
		CUresult rc = cuMemHostRegister_v2((void *)B, LEN,
						   CU_MEMHOSTREGISTER_DEVICEMAP);
		printf("  waiter  : register (32 MiB) rc=%d\n", rc);
		fflush(stdout);
		_exit(rc == 0 ? 0 : 1);
	}

	int st = wait_for_D(waiter);
	printf("  parent  : waiter pid=%d state=%c (D=uninterruptible sleep expected)\n",
	       waiter, st > 0 ? st : '?');
	fflush(stdout);
	if (st != 'D') {
		printf("VERDICT: INCONCLUSIVE -- waiter never observed in D state "
		       "(timing missed the window, not a mechanism finding)\n");
		waitpid(waiter, NULL, 0);
		waitpid(claimant, NULL, 0);
		return 3;
	}

	printf("  parent  : sending SIGKILL to waiter (currently D)...\n");
	fflush(stdout);
	kill(waiter, SIGKILL);

	usleep(20 * 1000);
	int st2 = proc_state(waiter);
	printf("  parent  : waiter state 20ms after SIGKILL = %c "
	       "(still D/alive => SIGKILL deferred, as expected; Z/gone => "
	       "took effect immediately)\n", st2 > 0 ? st2 : '?');
	fflush(stdout);
	int survived_kill = (st2 == 'D' || st2 == 'S' || st2 == 'R');

	int wst = 0;
	waitpid(waiter, &wst, 0);
	int wexit_signaled = WIFSIGNALED(wst);
	printf("  parent  : waiter reaped, signaled=%d (expected 1 -- it did "
	       "die from SIGKILL, just only once its claim resolved)\n",
	       wexit_signaled);

	int cst = 0;
	waitpid(claimant, &cst, 0);
	int claimant_rc = WIFEXITED(cst) ? WEXITSTATUS(cst) : 99;
	printf("  parent  : claimant reaped, exit=%d\n", claimant_rc);

	int ok = survived_kill && wexit_signaled && claimant_rc == 0;
	printf("\nVERDICT: %s\n", ok
	       ? "PASS -- credited waiter survived SIGKILL until its claim "
		 "resolved, then died"
	       : "FAIL -- see flags above");
	return ok ? 0 : 1;
}
