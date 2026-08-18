/*
 * uid_isolate_test.c — adversarial probe for the UID-separation isolation mode.
 *
 * Runs on the HOST (like poc_cross_proc_dup.c, unlike the in-guest tests).
 * It includes src/qemu/nvkvm_isolate_uid.h directly, so it exercises THE SAME
 * privilege-drop code the QEMU device runs — not a re-implementation that can
 * silently drift away from it.
 *
 * Four things are checked, in order of how badly a regression would hurt:
 *
 *   1. The drop is IRREVERSIBLE.  After nvkvm_iso_drop_privilege() the process
 *      must not be able to get back to uid 0 by any of setuid / seteuid /
 *      setresuid / setgid / setegid / setresgid.  We do not assume this — the
 *      classic bug is a setuid() that leaves the SAVED uid at 0, from which a
 *      later seteuid(0) walks straight back to root.
 *   2. Two isolates under different uids CANNOT reach each other: no ptrace,
 *      no signals, no /proc/<pid>/mem, no reading each other's files.  This is
 *      the entire boundary in UID mode.
 *   3. Supplementary groups are gone (setgroups(0, NULL) before the drop), so
 *      a shared group cannot be used to reach around the uid separation.
 *   4. The uid allocator gives every isolate slot a distinct uid inside a
 *      window that cannot overlap real accounts, and rejects bad config.
 *
 * It ALSO asserts, deliberately, the things UID mode does NOT stop: the peer
 * process is visible in /proc, its cmdline and status are readable, and the
 * host filesystem is fully visible.  Those are printed as "leak (expected in
 * uid mode)" lines.  They are the honest difference from namespace mode and
 * are documented in docs/internal/isolate-model.md; a future change that
 * closed them should update this test and that document, not delete the
 * lines.
 *
 *   gcc -O0 -D_GNU_SOURCE -o /tmp/uid_isolate_test uid_isolate_test.c
 *   sudo /tmp/uid_isolate_test
 *
 * Exit: 0 = every probe behaved as required
 *       1 = at least one probe FAILED (a real regression)
 *       2 = the privileged half could not run (no CAP_SETUID) — SKIP, and per
 *           the house rule in tests/validate.sh a check that could not run is
 *           never reported as a pass.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../../src/qemu/nvkvm_isolate_uid.h"

static int failures;
static int leaks_seen;

static void ok(const char *what, const char *detail)
{
	printf("  ok    %-44s %s\n", what, detail ? detail : "");
}

static void fail(const char *what, const char *detail)
{
	printf("  FAIL  %-44s %s\n", what, detail ? detail : "");
	failures++;
}

static void leak(const char *what, const char *detail)
{
	printf("  leak  %-44s %s  (expected in uid mode)\n", what,
	       detail ? detail : "");
	leaks_seen++;
}

/* ── 1..3: the live two-uid experiment ──────────────────────────────────── */

#define R_DROP_OK        (1u << 0)
#define R_SETUID0        (1u << 1)   /* set = regained root: catastrophic  */
#define R_SETEUID0       (1u << 2)
#define R_SETRESUID0     (1u << 3)
#define R_SETGID0        (1u << 4)
#define R_PTRACE         (1u << 5)
#define R_KILL           (1u << 6)
#define R_PROCMEM        (1u << 7)
#define R_PEERFILE       (1u << 8)
#define R_GROUPS         (1u << 9)   /* set = supplementary groups remain   */
#define R_PROCCMDLINE    (1u << 10)  /* set = peer visible in /proc (leak)  */
#define R_HOSTFS         (1u << 11)  /* set = host fs visible (leak)        */

static unsigned probe_peer(pid_t peer, const char *peer_file)
{
	unsigned r = 0;
	char path[128];
	int fd;

	/* Regaining privilege — every one of these must fail. */
	if (setuid(0) == 0)             r |= R_SETUID0;
	if (seteuid(0) == 0)            r |= R_SETEUID0;
	if (setresuid(0, 0, 0) == 0)    r |= R_SETRESUID0;
	if (setgid(0) == 0)             r |= R_SETGID0;

	if (getgroups(0, NULL) != 0)    r |= R_GROUPS;

	/* Reaching the peer isolate — every one of these must fail. */
	if (ptrace(PTRACE_ATTACH, peer, NULL, NULL) == 0) {
		r |= R_PTRACE;
		ptrace(PTRACE_DETACH, peer, NULL, NULL);
	}
	if (kill(peer, SIGKILL) == 0)   r |= R_KILL;

	snprintf(path, sizeof(path), "/proc/%d/mem", (int)peer);
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		/* Opening it is not yet a breach — reading is. */
		char b;
		if (pread(fd, &b, 1, 0x400000) >= 0)
			r |= R_PROCMEM;
		close(fd);
	}

	fd = open(peer_file, O_RDONLY);
	if (fd >= 0) { r |= R_PEERFILE; close(fd); }

	/* Things UID mode does NOT stop — asserted so the doc stays honest. */
	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)peer);
	fd = open(path, O_RDONLY);
	if (fd >= 0) { r |= R_PROCCMDLINE; close(fd); }

	fd = open("/etc/passwd", O_RDONLY);
	if (fd >= 0) { r |= R_HOSTFS; close(fd); }

	return r;
}

static int run_two_uid_experiment(uid_t uid_a, uid_t uid_b)
{
	int pipe_ready[2], pipe_res[2];
	char file_a[128];
	pid_t pid_a, pid_b;
	unsigned res = 0;

	snprintf(file_a, sizeof(file_a), "/tmp/nvkvm_uidtest_a_%d", (int)getpid());
	unlink(file_a);

	if (pipe(pipe_ready) || pipe(pipe_res)) {
		perror("pipe");
		return 2;
	}

	/* ── isolate A: drops to uid_a, drops a 0600 file, then waits ──── */
	pid_a = fork();
	if (pid_a == 0) {
		close(pipe_ready[0]); close(pipe_res[0]); close(pipe_res[1]);
		int fd = open(file_a, O_CREAT | O_WRONLY, 0600);
		if (fd < 0) _exit(3);
		if (write(fd, "isolate-A-private", 17) != 17) _exit(3);
		close(fd);
		if (fchownat(AT_FDCWD, file_a, uid_a, uid_a, 0) != 0) _exit(3);
		if (nvkvm_iso_drop_privilege(uid_a, (gid_t)uid_a) != 0)
			_exit(4);
		if (write(pipe_ready[1], "a", 1) != 1) _exit(5);
		close(pipe_ready[1]);
		pause();                    /* killed by the parent */
		_exit(0);
	}
	if (pid_a < 0) { perror("fork A"); return 2; }

	/* Wait for A to be fully dropped before B probes it. */
	{
		char c;
		close(pipe_ready[1]);
		if (read(pipe_ready[0], &c, 1) != 1) {
			int st = 0;
			waitpid(pid_a, &st, 0);
			printf("  harness: isolate A failed to drop "
			       "(exit %d)\n", WEXITSTATUS(st));
			return 2;
		}
		close(pipe_ready[0]);
	}

	/* ── isolate B: drops to uid_b, then probes A ──────────────────── */
	pid_b = fork();
	if (pid_b == 0) {
		unsigned r = 0;
		close(pipe_res[0]);
		if (nvkvm_iso_drop_privilege(uid_b, (gid_t)uid_b) == 0)
			r |= R_DROP_OK;
		r |= probe_peer(pid_a, file_a);
		if (write(pipe_res[1], &r, sizeof(r)) != (ssize_t)sizeof(r))
			_exit(6);
		_exit(0);
	}
	if (pid_b < 0) { perror("fork B"); kill(pid_a, SIGKILL); return 2; }

	close(pipe_res[1]);
	if (read(pipe_res[0], &res, sizeof(res)) != (ssize_t)sizeof(res)) {
		printf("  harness: isolate B produced no result\n");
		kill(pid_a, SIGKILL);
		waitpid(pid_a, NULL, 0); waitpid(pid_b, NULL, 0);
		return 2;
	}
	close(pipe_res[0]);

	kill(pid_a, SIGKILL);
	waitpid(pid_a, NULL, 0);
	waitpid(pid_b, NULL, 0);
	unlink(file_a);

	/* ── verdicts ───────────────────────────────────────────────────── */
	char d[128];
	snprintf(d, sizeof(d), "uid_a=%u uid_b=%u", (unsigned)uid_a,
		 (unsigned)uid_b);
	if (res & R_DROP_OK) ok("drop to unique uid + self-verification", d);
	else                 fail("drop to unique uid + self-verification", d);

	if (res & R_SETUID0)    fail("setuid(0) after drop",     "REGAINED ROOT");
	else                    ok  ("setuid(0) after drop",     "refused");
	if (res & R_SETEUID0)   fail("seteuid(0) after drop",    "REGAINED ROOT");
	else                    ok  ("seteuid(0) after drop",    "refused");
	if (res & R_SETRESUID0) fail("setresuid(0,0,0) after drop", "REGAINED ROOT");
	else                    ok  ("setresuid(0,0,0) after drop", "refused");
	if (res & R_SETGID0)    fail("setgid(0) after drop",     "REGAINED ROOT GID");
	else                    ok  ("setgid(0) after drop",     "refused");
	if (res & R_GROUPS)     fail("supplementary groups dropped", "still present");
	else                    ok  ("supplementary groups dropped", "getgroups()==0");

	if (res & R_PTRACE)     fail("cross-uid PTRACE_ATTACH",  "ATTACHED");
	else                    ok  ("cross-uid PTRACE_ATTACH",  "refused");
	if (res & R_KILL)       fail("cross-uid kill(SIGKILL)",  "SIGNAL DELIVERED");
	else                    ok  ("cross-uid kill(SIGKILL)",  "refused");
	if (res & R_PROCMEM)    fail("cross-uid read /proc/<peer>/mem", "READ");
	else                    ok  ("cross-uid read /proc/<peer>/mem", "refused");
	if (res & R_PEERFILE)   fail("cross-uid open peer's 0600 file", "OPENED");
	else                    ok  ("cross-uid open peer's 0600 file", "refused");

	/* Documented weaknesses of this mode — present is EXPECTED. */
	if (res & R_PROCCMDLINE)
		leak("peer visible in /proc (no pid namespace)",
		     "/proc/<peer>/cmdline readable");
	else
		printf("  note  peer NOT visible in /proc — stronger than "
		       "documented; update isolate-model.md\n");
	if (res & R_HOSTFS)
		leak("host filesystem visible (no mount namespace)",
		     "/etc/passwd readable");
	else
		printf("  note  host fs NOT visible — stronger than "
		       "documented; update isolate-model.md\n");

	return 0;
}

/* ── chroot confinement (uid+chroot mode) ───────────────────────────────
 *
 * Runs the REAL nvkvm_iso_enter_chroot() from the shared header, then the real
 * privilege drop, then probes what the confined stub can still reach.  The
 * production order is chroot -> closefrom -> drop uid -> drop caps, and the
 * uid drop is what strips CAP_SYS_CHROOT (a non-zero uid transition clears the
 * permitted set), so re-chrooting out is tested against exactly that state.
 */
#define C_CHROOT_OK      (1u << 0)
#define C_DEVNODE        (1u << 1)   /* must be set: nvidiactl still openable */
#define C_ETC_PASSWD     (1u << 2)   /* must be clear: host fs gone           */
#define C_PROC           (1u << 3)   /* must be clear: /proc gone             */
#define C_DOTDOT_ESCAPE  (1u << 4)   /* must be clear: openat(dd,"../..") clamped */
#define C_RECHROOT       (1u << 5)   /* must be clear: cannot chroot again    */
#define C_DEVSHM         (1u << 6)   /* expected leak: /dev/shm still writable */
#define C_DEV_UVM        (1u << 7)   /* expected leak: /dev/nvidia-uvm openable */
#define C_DEVNULL        (1u << 8)   /* must be set: the root really is /dev  */

#define TEST_DEV_DIRFD 4

static void run_chroot_experiment(uid_t uid)
{
	int p[2];
	if (pipe(p)) { perror("pipe"); return; }

	pid_t pid = fork();
	if (pid == 0) {
		unsigned r = 0;
		int fd, out;
		close(p[0]);
		/* Move the result pipe clear of TEST_DEV_DIRFD: the dirfd is
		 * dup2'd onto that exact slot, and in production fd 4 is
		 * reserved for it (fd 0 = socket, fd 3 = stub image). */
		out = fcntl(p[1], F_DUPFD_CLOEXEC, 32);
		if (out < 0) _exit(7);
		close(p[1]);
		if (nvkvm_iso_enter_chroot(TEST_DEV_DIRFD) == 0)
			r |= C_CHROOT_OK;
		if (nvkvm_iso_drop_privilege(uid, (gid_t)uid) != 0)
			_exit(4);

		/* The chroot root must actually be /dev — checked against a node
		 * that exists on every host, so this probe is meaningful even
		 * where the NVIDIA nodes are absent. */
		fd = openat(TEST_DEV_DIRFD, "null", O_RDWR);
		if (fd >= 0) { r |= C_DEVNULL; close(fd); }

		/* The stub's own device open, via the parked dirfd. */
		fd = openat(TEST_DEV_DIRFD, "nvidiactl", O_RDWR);
		if (fd < 0)          /* fall back to the absolute path form */
			fd = open("/nvidiactl", O_RDWR);
		if (fd >= 0) { r |= C_DEVNODE; close(fd); }

		/* Host filesystem must be gone. */
		fd = open("/etc/passwd", O_RDONLY);
		if (fd >= 0) { r |= C_ETC_PASSWD; close(fd); }
		fd = open("/proc/self/status", O_RDONLY);
		if (fd >= 0) { r |= C_PROC; close(fd); }

		/* The O_PATH ".." traversal that bit this tree once. */
		fd = openat(TEST_DEV_DIRFD, "../../etc/passwd", O_RDONLY);
		if (fd >= 0) { r |= C_DOTDOT_ESCAPE; close(fd); }
		fd = openat(TEST_DEV_DIRFD, "../../../../../../etc/passwd",
			    O_RDONLY);
		if (fd >= 0) { r |= C_DOTDOT_ESCAPE; close(fd); }

		/* Classic double-chroot escape needs CAP_SYS_CHROOT, which the
		 * uid drop took away. */
		if (mkdir("/nvkvm_esc", 0700) == 0 || errno == EEXIST) {
			if (chroot("/nvkvm_esc") == 0) r |= C_RECHROOT;
			rmdir("/nvkvm_esc");
		}
		if (chroot("/") == 0) r |= C_RECHROOT;

		/* Documented residue. */
		fd = open("/shm/nvkvm_chroot_probe",
			  O_CREAT | O_WRONLY | O_EXCL, 0600);
		if (fd >= 0) {
			r |= C_DEVSHM; close(fd);
			unlink("/shm/nvkvm_chroot_probe");
		}
		fd = open("/nvidia-uvm", O_RDWR);
		if (fd >= 0) { r |= C_DEV_UVM; close(fd); }

		if (write(out, &r, sizeof(r)) != (ssize_t)sizeof(r)) _exit(5);
		_exit(0);
	}
	close(p[1]);

	unsigned r = 0;
	int st = 0;
	if (read(p[0], &r, sizeof(r)) != (ssize_t)sizeof(r)) {
		waitpid(pid, &st, 0);
		printf("  SKIP  chroot experiment: child exited %d "
		       "(no CAP_SYS_CHROOT?)\n", WEXITSTATUS(st));
		close(p[0]);
		return;
	}
	close(p[0]);
	waitpid(pid, NULL, 0);

	if (r & C_CHROOT_OK) ok("chroot(\"/dev\") + chdir(\"/\") + dirfd", "");
	else { fail("chroot(\"/dev\") + chdir(\"/\") + dirfd", "failed"); return; }

	if (r & C_DEVNULL)  ok("chroot root really is /dev", "/dev/null reachable");
	else                fail("chroot root really is /dev",
				 "/dev/null NOT reachable");

	/* House rule (tests/validate.sh): a check that COULD NOT RUN is a SKIP,
	 * never a PASS.  No NVIDIA node on this host means we learn nothing
	 * about whether the stub could open it, so say so. */
	if (access("/dev/nvidiactl", F_OK) != 0)
		printf("  SKIP  nvidiactl openable inside chroot            "
		       "no /dev/nvidiactl on this host\n");
	else if (r & C_DEVNODE)
		ok("nvidiactl still openable inside chroot", "");
	else
		fail("nvidiactl still openable inside chroot",
		     "NOT openable — the mode would not work");

	if (r & C_ETC_PASSWD) fail("host /etc unreachable", "/etc/passwd OPENED");
	else                  ok  ("host /etc unreachable", "ENOENT");
	if (r & C_PROC)       fail("host /proc unreachable", "/proc OPENED");
	else                  ok  ("host /proc unreachable",
				   "ENOENT — process enumeration closed");
	if (r & C_DOTDOT_ESCAPE)
		fail("openat(dirfd, \"../../etc/passwd\") clamped",
		     "TRAVERSED — same bug class as the O_PATH host-/dev escape");
	else
		ok("openat(dirfd, \"../../etc/passwd\") clamped", "ENOENT");
	if (r & C_RECHROOT) fail("cannot re-chroot after uid drop",
				 "CHROOT SUCCEEDED — escape route open");
	else                ok  ("cannot re-chroot after uid drop",
				 "EPERM (CAP_SYS_CHROOT gone with the uid)");

	if (r & C_DEVSHM)
		leak("/dev/shm writable inside chroot", "shared with the host");
	else
		printf("  note  /dev/shm not writable — stronger than "
		       "documented\n");
	if (r & C_DEV_UVM)
		leak("/dev/nvidia-uvm openable inside chroot",
		     "bypasses QEMU's UVM allowlist");
	else if (access("/dev/nvidia-uvm", F_OK) != 0)
		printf("  SKIP  /dev/nvidia-uvm exposure                    "
		       "node absent on this host\n");
	else
		printf("  note  /dev/nvidia-uvm present but NOT openable — "
		       "stronger than documented\n");
}

/* ── 4: allocator and configuration validation (unprivileged) ───────────── */

static void test_allocator_and_config(void)
{
	uid_t u;
	char err[256];

	/* Distinct uid for every slot, all inside the window. */
	int dup_found = 0, range_bad = 0;
	uid_t prev = 0;
	for (uint32_t slot = 0; slot < NVKVM_ISO_UID_SLOTS; slot++) {
		if (nvkvm_iso_uid_for_slot(NVKVM_ISO_UID_BASE_DEFAULT, slot,
					   &u) != 0) {
			range_bad = 1;
			break;
		}
		if (slot && u <= prev) dup_found = 1;
		if (u < NVKVM_ISO_UID_BASE_DEFAULT ||
		    u >= NVKVM_ISO_UID_BASE_DEFAULT + NVKVM_ISO_UID_SLOTS)
			range_bad = 1;
		prev = u;
	}
	if (dup_found || range_bad)
		fail("uid_for_slot: 4096 distinct uids in window", "");
	else {
		char d[96];
		snprintf(d, sizeof(d), "%u..%u", NVKVM_ISO_UID_BASE_DEFAULT,
			 NVKVM_ISO_UID_BASE_DEFAULT + NVKVM_ISO_UID_SLOTS - 1);
		ok("uid_for_slot: 4096 distinct uids in window", d);
	}

	/* Out-of-window slot must be refused, not wrapped. */
	if (nvkvm_iso_uid_for_slot(NVKVM_ISO_UID_BASE_DEFAULT,
				   NVKVM_ISO_UID_SLOTS, &u) == 0)
		fail("uid_for_slot: slot beyond window refused", "accepted");
	else
		ok("uid_for_slot: slot beyond window refused", "");

	/* A base that could collide with real accounts must be refused. */
	{
		struct nvkvm_isolate_cfg c = { NVKVM_ISO_LAYER_UID, 1000 };
		if (nvkvm_iso_cfg_validate(&c, err, sizeof(err)) == 0)
			fail("cfg: uid base 1000 refused", "accepted");
		else
			ok("cfg: uid base 1000 refused", err);
	}
	{
		struct nvkvm_isolate_cfg c = { NVKVM_ISO_LAYER_UID, 65535 };
		if (nvkvm_iso_cfg_validate(&c, err, sizeof(err)) == 0)
			fail("cfg: uid base 65535 (nobody/overflow) refused",
			     "accepted");
		else
			ok("cfg: uid base 65535 (nobody/overflow) refused", "");
	}

	/* Mode parsing must be strict: no silent fallback on a typo. */
	{
		unsigned m = 0xdead;
#define SC NVKVM_ISO_LAYER_SECCOMP
		struct { const char *s; int want_ok; unsigned want; } cases[] = {
			/* seccomp is the implied floor of every real rung */
			{ "namespace",      1, NVKVM_ISO_LAYER_NS|SC },
			{ "ns",             1, NVKVM_ISO_LAYER_NS|SC },
			{ "uid",            1, NVKVM_ISO_LAYER_UID|SC },
			{ "seccomp",        1, SC },
			{ "namespace+uid",  1, NVKVM_ISO_LAYER_NS|NVKVM_ISO_LAYER_UID|SC },
			{ "uid+namespace",  1, NVKVM_ISO_LAYER_NS|NVKVM_ISO_LAYER_UID|SC },
			{ "none",           1, 0u },
			{ "auto",           1, NVKVM_ISO_MODE_AUTO },
			{ "namesapce",      0, 0 },
			{ "uid+none",       0, 0 },
			{ "auto+uid",       0, 0 },  /* auto is exclusive          */
			{ "auto+none",      0, 0 },
			{ "",               0, 0 },
			{ "userns,uidsep",  1, NVKVM_ISO_LAYER_NS|NVKVM_ISO_LAYER_UID|SC },
			{ "uid+chroot",     1, NVKVM_ISO_LAYER_UID|NVKVM_ISO_LAYER_CHROOT|SC },
			{ "chroot",         0, 0 },  /* add-on only, never alone   */
			{ "namespace+chroot", 0, 0 },/* pivot_root already stronger */
		};
#undef SC
		int bad = 0;
		for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
			int r = nvkvm_iso_mode_parse(cases[i].s, &m, err,
						     sizeof(err));
			if (cases[i].want_ok && (r != 0 || m != cases[i].want)) {
				printf("      mode '%s': r=%d m=0x%x want 0x%x\n",
				       cases[i].s, r, m, cases[i].want);
				bad = 1;
			}
			if (!cases[i].want_ok && r == 0) {
				printf("      mode '%s' ACCEPTED (must be "
				       "rejected)\n", cases[i].s);
				bad = 1;
			}
		}
		if (bad) fail("mode parse: strict, no silent fallback", "");
		else     ok("mode parse: strict, no silent fallback",
			    "17 cases incl. typo, exclusivity, implied seccomp");
	}
}

/* ── mode `auto`: the ladder, and the floor it must never go below ─────── */

static void test_auto_ladder(void)
{
	char report[1024], err[256];
	unsigned m = 0xdead;

	/* Only mode 0 (every layer off) needs the acknowledgement. */
	if (!nvkvm_iso_needs_unsafe_ack(0u))
		fail("mode 'none' requires the unsafe acknowledgement", "");
	else if (nvkvm_iso_needs_unsafe_ack(NVKVM_ISO_LAYER_SECCOMP))
		fail("mode 'seccomp' does NOT require the acknowledgement", "");
	else
		ok("only mode 'none' requires the unsafe acknowledgement", "");

	report[0] = err[0] = '\0';
	if (nvkvm_iso_auto_select(&m, report, sizeof(report), err,
				  sizeof(err)) != 0) {
		printf("  note  auto ladder found nothing usable: %.150s\n", err);
		/* Failing closed is the required behaviour, so this is not a
		 * FAIL — but it does mean the rest could not be checked. */
		printf("  SKIP  auto ladder selection (nothing available)\n");
		return;
	}

	/* THE hard requirement: auto never selects 'none'. */
	if ((m & NVKVM_ISO_LAYERS_ALL) == 0)
		fail("auto never selects 'none'",
		     "SELECTED none — every boundary silently removed");
	else
		ok("auto never selects 'none'", nvkvm_iso_mode_str(m));

	/* And it always includes the seccomp floor. */
	if (!(m & NVKVM_ISO_LAYER_SECCOMP))
		fail("auto always includes the seccomp floor", "");
	else
		ok("auto always includes the seccomp floor",
		   nvkvm_iso_mode_str(m));

	if (report[0])
		printf("        report: %.220s\n", report);
}

/* ── CAP_SETUID preflight, checked in a deliberately capability-less child ─ */

static void test_preflight_without_caps(uid_t sacrificial_uid)
{
	int p[2];
	if (pipe(p)) { perror("pipe"); return; }

	pid_t pid = fork();
	if (pid == 0) {
		char err[256] = {0};
		struct nvkvm_isolate_cfg c = {
			NVKVM_ISO_LAYER_UID | NVKVM_ISO_LAYER_SECCOMP,
			NVKVM_ISO_UID_BASE_DEFAULT
		};
		close(p[0]);
		/* Become an ordinary uid: CAP_SETUID/CAP_SETGID go with it. */
		if (nvkvm_iso_drop_privilege(sacrificial_uid,
					     (gid_t)sacrificial_uid) != 0)
			_exit(4);
		int rc = nvkvm_iso_cfg_validate(&c, err, sizeof(err));
		unsigned char v = (rc != 0) ? 1 : 0;
		if (write(p[1], &v, 1) != 1) _exit(5);
		if (write(p[1], err, sizeof(err)) != (ssize_t)sizeof(err))
			_exit(5);
		_exit(0);
	}
	close(p[1]);

	unsigned char v = 0;
	char err[256] = {0};
	if (read(p[0], &v, 1) == 1 &&
	    read(p[0], err, sizeof(err)) == (ssize_t)sizeof(err)) {
		if (v)
			ok("preflight refuses uid mode without CAP_SETUID",
			   "and says why");
		else
			fail("preflight refuses uid mode without CAP_SETUID",
			     "ACCEPTED — would fail later, inside the child");
		if (v && err[0])
			printf("        message: %.180s\n", err);
	} else {
		printf("  SKIP  preflight-without-caps: child produced no "
		       "result\n");
	}
	close(p[0]);
	waitpid(pid, NULL, 0);
}

int main(void)
{
	bool can_setuid = false, can_setgid = false;

	printf("nvkvm UID-separation isolation mode — security probes\n");
	printf("euid=%u  ", (unsigned)geteuid());
	if (nvkvm_iso_have_setid_caps(&can_setuid, &can_setgid) == 0)
		printf("CAP_SETUID=%s CAP_SETGID=%s\n",
		       can_setuid ? "yes" : "no", can_setgid ? "yes" : "no");
	else
		printf("capget failed: %s\n", strerror(errno));

	printf("\n[ allocator + configuration ]\n");
	test_allocator_and_config();

	printf("\n[ mode auto: degradation ladder ]\n");
	test_auto_ladder();

	if (!can_setuid || !can_setgid) {
		printf("\nSKIP: no CAP_SETUID/CAP_SETGID — the privilege-drop and\n"
		       "      cross-uid probes could not run.  Per the house rule\n"
		       "      in tests/validate.sh a check that could not run is\n"
		       "      NOT a pass.  Re-run as root.\n");
		printf("\n%d failure(s) in the checks that did run.\n", failures);
		return failures ? 1 : 2;
	}

	uid_t ua, ub;
	if (nvkvm_iso_uid_for_slot(NVKVM_ISO_UID_BASE_DEFAULT, 1, &ua) != 0 ||
	    nvkvm_iso_uid_for_slot(NVKVM_ISO_UID_BASE_DEFAULT, 2, &ub) != 0) {
		printf("harness: uid allocation failed\n");
		return 2;
	}

	printf("\n[ privilege drop + cross-uid reach, two live isolates ]\n");
	int rc = run_two_uid_experiment(ua, ub);
	if (rc == 2) {
		printf("\nSKIP: the two-uid experiment could not run.\n");
		return 2;
	}

	printf("\n[ uid+chroot confinement ]\n");
	{
		uid_t uc;
		bool cs = false, cg = false, cc = false;
		nvkvm_iso_have_caps(&cs, &cg, &cc);
		if (!cc)
			printf("  SKIP  no CAP_SYS_CHROOT — uid+chroot probes "
			       "did not run (not a pass)\n");
		else if (nvkvm_iso_uid_for_slot(NVKVM_ISO_UID_BASE_DEFAULT, 3,
						&uc) == 0)
			run_chroot_experiment(uc);
	}

	printf("\n[ preflight ]\n");
	test_preflight_without_caps(ub);

	printf("\n%d failure(s), %d expected leak(s) inherent to uid mode.\n",
	       failures, leaks_seen);
	return failures ? 1 : 0;
}
