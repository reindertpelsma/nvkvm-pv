/*
 * ns_probe_test.c — does nvkvm_iso_probe_namespaces() tell the truth?
 *
 * Runs on the HOST (like uid_isolate_test.c). It includes
 * src/qemu/nvkvm_isolate_uid.h directly, so it exercises THE SAME probe the
 * QEMU device runs when NVKVM_ISOLATE_MODE=auto.
 *
 * THE BUG THIS EXISTS FOR
 * =======================
 * The probe used to clone(2) with the production namespace flags and have the
 * child attempt mount(MS_REC|MS_PRIVATE) — deliberately, to catch Ubuntu's
 * kernel.apparmor_restrict_unprivileged_userns=1, where the clone succeeds and
 * the mount is refused. What it never attempted was the step BETWEEN those
 * two, which the real spawn does and which can fail on its own: the PARENT
 * writing the child's setgroups/uid_map/gid_map (nvkvm_map_child_userns()).
 *
 * That write maps `0 <euid> 1`. When QEMU runs as uid 0 — which is how Kata
 * launches it — that maps UID 0 of the parent namespace, and since Linux 5.12
 * the kernel requires CAP_SETFCAP in the parent namespace to do so
 * (user_namespaces(7), "Defining user and group ID mappings"). CAP_SYS_ADMIN
 * alone is not enough.
 *
 * So on a host with CAP_SYS_ADMIN and without CAP_SETFCAP the old probe
 * reported `namespace` AVAILABLE, `auto` pinned the strongest rung, and every
 * isolate then died at the map write. Fail-OPEN: the probe promised a rung
 * that could not run, and the guest got no GPU at all.
 *
 * WHAT IS ASSERTED
 * ================
 * For each capability set below, the probe's verdict must match GROUND TRUTH,
 * where ground truth is measured independently of nvkvm: clone the same
 * namespaces, write the same `0 <euid> 1` map by hand, and see what the kernel
 * says. The oracle is deliberately NOT a call into nvkvm code — a test that
 * called the same function as the probe would pass even if both were wrong.
 *
 *   caps                      expected (uid 0, kernel >= 5.12)
 *   ------------------------- -------------------------------------------
 *   full                      available
 *   SYS_ADMIN + SETFCAP       available
 *   SYS_ADMIN only            UNAVAILABLE, and the map write is the reason
 *   SETUID,SETGID,SYS_CHROOT  UNAVAILABLE  (the steamos container set)
 *   none                      UNAVAILABLE
 *
 * The third row is the regression. Against the pre-fix probe it FAILS with
 * "probe said available, ground truth says the map write is refused".
 *
 *   gcc -O0 -D_GNU_SOURCE -o /tmp/ns_probe_test ns_probe_test.c
 *   sudo /tmp/ns_probe_test
 *
 * Exit: 0 = every case agreed
 *       1 = at least one disagreement (a real regression)
 *       2 = could not run privileged (needs uid 0 + CAP_SETPCAP to build the
 *           capability sets) — SKIP, and per the house rule in
 *           tests/validate.sh a check that could not run is never a pass.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/capability.h>

#include "../../src/qemu/nvkvm_isolate_uid.h"

/*
 * The probe grew an out-param naming which step failed. Keep this test
 * buildable against a tree that predates it, so "prove it fails first" is one
 * compile away and does not need the test to be edited.
 */
#ifdef NVKVM_ISO_NS_STEP_AWARE
# define HAVE_NS_STEP 1
#else
# define HAVE_NS_STEP 0
#endif

static int failures;
static int checks;

static void ok(const char *what, const char *detail)
{
	printf("  ok    %-36s %s\n", what, detail ? detail : "");
	checks++;
}

static void fail(const char *what, const char *detail)
{
	printf("  FAIL  %-36s %s\n", what, detail ? detail : "");
	failures++;
	checks++;
}

/* ── Ground truth, independent of nvkvm ──────────────────────────────────── */

enum gt { GT_OK = 0, GT_CLONE, GT_MAP, GT_MOUNT };

static const char *gt_name(enum gt g)
{
	switch (g) {
	case GT_OK:    return "usable";
	case GT_CLONE: return "clone refused";
	case GT_MAP:   return "uid_map write refused";
	case GT_MOUNT: return "in-namespace mount refused";
	}
	return "?";
}

/*
 * Do by hand exactly what the real isolate spawn does, in the same order, and
 * report which step the kernel refused. Written out rather than calling
 * nvkvm_map_child_userns() on purpose: this is the oracle, and an oracle that
 * shares an implementation with the thing under test cannot catch it being
 * wrong. It must stay in step with nvkvm_isolate.c's map content — the map is
 * `setgroups: deny`, `uid_map: 0 <euid> 1`, `gid_map: 0 <egid> 1`.
 */
static enum gt ground_truth(int *errno_out)
{
	unsigned long flags = CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET |
			      CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWNS |
			      (unsigned long)SIGCHLD;
	int p[2], status = 0, e = 0;
	char path[64], buf[64];
	pid_t pid;
	int fd;

	*errno_out = 0;
	if (pipe(p) != 0) { *errno_out = errno; return GT_CLONE; }

	pid = (pid_t)syscall(SYS_clone, flags, (void *)0, (void *)0,
			     (void *)0, 0UL);
	if (pid == 0) {
		char go;
		close(p[1]);
		if (read(p[0], &go, 1) != 1)
			_exit(126);
		close(p[0]);
		if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
			_exit(errno & 0x7f ? errno & 0x7f : 1);
		_exit(0);
	}
	if (pid < 0) {
		*errno_out = errno;
		close(p[0]); close(p[1]);
		return GT_CLONE;
	}
	close(p[0]);

#define GT_WRITE(which, val)                                                  \
	do {                                                                  \
		snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, which); \
		fd = open(path, O_WRONLY);                                    \
		if (fd < 0 || write(fd, (val), strlen(val)) !=                \
				(ssize_t)strlen(val)) {                       \
			e = errno ? errno : EPERM;                            \
			if (fd >= 0) close(fd);                               \
			close(p[1]);                                          \
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) \
				;                                             \
			*errno_out = e;                                       \
			return GT_MAP;                                        \
		}                                                             \
		close(fd);                                                    \
	} while (0)

	GT_WRITE("setgroups", "deny");
	snprintf(buf, sizeof(buf), "0 %u 1\n", (unsigned)geteuid());
	GT_WRITE("uid_map", buf);
	snprintf(buf, sizeof(buf), "0 %u 1\n", (unsigned)getegid());
	GT_WRITE("gid_map", buf);
#undef GT_WRITE

	if (write(p[1], "x", 1) != 1) {
		*errno_out = errno;
		close(p[1]);
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
		return GT_MAP;
	}
	close(p[1]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		*errno_out = WEXITSTATUS(status);
		return GT_MOUNT;
	}
	return GT_OK;
}

/* ── Capability plumbing ─────────────────────────────────────────────────── */

static int set_caps(uint64_t mask)
{
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0
	};
	struct __user_cap_data_struct data[2];
	int c;

	/* Bounding set first: PR_CAPBSET_DROP needs CAP_SETPCAP, which we are
	 * about to give up. Dropping from the bounding set does not remove a
	 * capability from permitted, so the capset() below still works. */
	for (c = 0; c <= 63; c++) {
		if (mask & (1ULL << c))
			continue;
		if (prctl(PR_CAPBSET_DROP, c, 0, 0, 0) != 0 && errno != EINVAL)
			return -1;
	}
	memset(data, 0, sizeof(data));
	data[0].effective = data[0].permitted = (uint32_t)(mask & 0xffffffffu);
	data[1].effective = data[1].permitted = (uint32_t)(mask >> 32);
	if (syscall(SYS_capset, &hdr, data) != 0)
		return -1;
	return 0;
}

static void caps_str(uint64_t mask, char *out, size_t sz)
{
	static const struct { int c; const char *n; } names[] = {
		{ CAP_SETUID, "SETUID" }, { CAP_SETGID, "SETGID" },
		{ CAP_SETPCAP, "SETPCAP" }, { CAP_SYS_CHROOT, "SYS_CHROOT" },
		{ CAP_SYS_ADMIN, "SYS_ADMIN" }, { CAP_SETFCAP, "SETFCAP" },
	};
	size_t n = 0;
	unsigned i;

	out[0] = '\0';
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (mask & (1ULL << names[i].c))
			n += snprintf(out + n, n < sz ? sz - n : 0, "%s%s",
				      n ? "," : "", names[i].n);
	if (!out[0])
		snprintf(out, sz, "(none)");
}

/* ── One case ────────────────────────────────────────────────────────────── */

struct outcome { int probe_rc; int probe_err; int probe_step; enum gt gt; int gt_err; };

/* Runs in a forked child so the capability drop does not stick. */
static int run_case(uint64_t mask, struct outcome *o)
{
	int p[2];
	pid_t pid;
	int status = 0;

	if (pipe(p) != 0)
		return -1;
	pid = fork();
	if (pid < 0) { close(p[0]); close(p[1]); return -1; }
	if (pid == 0) {
		struct outcome r;
		int e = 0;
		close(p[0]);
		if (set_caps(mask) != 0)
			_exit(3);
		memset(&r, 0, sizeof(r));
#if HAVE_NS_STEP
		{
			enum nvkvm_iso_ns_step st = NVKVM_ISO_NS_OK;
			r.probe_rc = nvkvm_iso_probe_namespaces(&e, &st);
			r.probe_step = (int)st;
		}
#else
		r.probe_rc = nvkvm_iso_probe_namespaces(&e);
		r.probe_step = -1;
#endif
		r.probe_err = e;
		r.gt = ground_truth(&r.gt_err);
		if (write(p[1], &r, sizeof(r)) != (ssize_t)sizeof(r))
			_exit(4);
		_exit(0);
	}
	close(p[1]);
	if (read(p[0], o, sizeof(*o)) != (ssize_t)sizeof(*o)) {
		close(p[0]);
		waitpid(pid, &status, 0);
		return -1;
	}
	close(p[0]);
	waitpid(pid, &status, 0);
	return 0;
}

static void check(const char *label, uint64_t mask)
{
	struct outcome o;
	char caps[128], detail[320];

	caps_str(mask, caps, sizeof(caps));
	if (run_case(mask, &o) != 0) {
		snprintf(detail, sizeof(detail),
			 "could not run the case (caps=%s)", caps);
		fail(label, detail);
		return;
	}

	if (o.gt == GT_OK && o.probe_rc == 0) {
		snprintf(detail, sizeof(detail),
			 "caps=%s -> both say usable", caps);
		ok(label, detail);
		return;
	}
	if (o.gt != GT_OK && o.probe_rc != 0) {
		const char *step = "?";
#if HAVE_NS_STEP
		switch (o.probe_step) {
		case NVKVM_ISO_NS_CLONE: step = "clone";  break;
		case NVKVM_ISO_NS_MAP:   step = "map";    break;
		case NVKVM_ISO_NS_MOUNT: step = "mount";  break;
		default:                 step = "?";      break;
		}
#endif
		snprintf(detail, sizeof(detail),
			 "caps=%s -> both say unusable (%s; probe step=%s, "
			 "errno=%s)", caps, gt_name(o.gt), step,
			 strerror(o.probe_err));
		ok(label, detail);
		return;
	}
	if (o.probe_rc == 0) {
		snprintf(detail, sizeof(detail),
			 "caps=%s -> PROBE SAID AVAILABLE, but the real spawn's "
			 "%s (errno=%s). auto would pin `namespace` and every "
			 "isolate would then fail.",
			 caps, gt_name(o.gt), strerror(o.gt_err));
		fail(label, detail);
		return;
	}
	snprintf(detail, sizeof(detail),
		 "caps=%s -> probe said UNAVAILABLE (errno=%s) but the real "
		 "spawn works. auto would needlessly degrade the sandbox.",
		 caps, strerror(o.probe_err));
	fail(label, detail);
}

int main(void)
{
	const uint64_t ALL = ~0ULL;
	const uint64_t SYSADM_FCAP = (1ULL << CAP_SYS_ADMIN) | (1ULL << CAP_SETFCAP);
	const uint64_t SYSADM      = (1ULL << CAP_SYS_ADMIN);
	const uint64_t STEAMOS     = (1ULL << CAP_SETUID) | (1ULL << CAP_SETGID) |
				     (1ULL << CAP_SETPCAP) | (1ULL << CAP_SYS_CHROOT);
	bool su = false, sg = false;

	printf("ns_probe_test — does the namespace probe match the real spawn?\n");
	printf("  euid=%u  probe reports the failing step: %s\n\n",
	       (unsigned)geteuid(), HAVE_NS_STEP ? "yes" : "NO (pre-fix probe)");

	if (geteuid() != 0 || nvkvm_iso_have_caps(&su, &sg, NULL) != 0 || !su) {
		printf("  SKIP  needs uid 0 with CAP_SETPCAP/CAP_SETUID to "
		       "construct the capability sets; run under sudo.\n");
		return 2;
	}

	check("full caps",              ALL);
	check("SYS_ADMIN+SETFCAP",      SYSADM_FCAP);
	check("SYS_ADMIN, no SETFCAP",  SYSADM);
	check("steamos container set",  STEAMOS);
	check("no capabilities",        0);

	printf("\n  %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
