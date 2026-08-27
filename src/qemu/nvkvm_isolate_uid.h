/*
 * nvkvm_isolate_uid.h — UID-separation isolation mode for isolates.
 *
 * WHY THIS EXISTS
 * ===============
 * The default isolate sandbox (nvkvm_isolate.c) clones the stub into fresh
 * user + pid + net + ipc + uts + mount namespaces.  CLONE_NEWUSER is the
 * keystone: it is what lets an unprivileged QEMU create the other four.
 *
 * Unprivileged containers routinely make that impossible:
 *   - sysctl kernel.unprivileged_userns_clone=0   (Debian/Ubuntu patch)
 *   - sysctl user.max_user_namespaces=0           (upstream, per-userns)
 *   - a Docker/Kubernetes seccomp profile that returns EPERM for clone()
 *     with CLONE_NEWUSER
 *   - an LSM (AppArmor userns rules, SELinux) denying the transition
 * In such an environment nvkvm cannot spawn an isolate at all, even though
 * /dev/kvm and the GPU are both usable.  This header provides the fallback
 * boundary: give each isolate a unique high UID/GID and setresgid/setresuid
 * to it, so isolation rests on ordinary POSIX DAC instead of namespaces.
 *
 * THIS IS A WEAKER BOUNDARY.  It is opt-in and never selected automatically.
 * See docs/internal/isolate-model.md ("Isolation modes") for the honest
 * per-property comparison; the short version is that UID mode keeps
 * cross-isolate memory/ptrace/signal separation and loses everything the
 * namespaces provided (process-table, network, filesystem, IPC, UTS).
 *
 * Everything here is header-only and freestanding-ish (POSIX + Linux caps)
 * so that tests/security/uid_isolate_test.c exercises THE SAME CODE the
 * device runs, rather than a re-implementation that can drift.
 */

#ifndef NVKVM_ISOLATE_UID_H
#define NVKVM_ISOLATE_UID_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <signal.h>
#include <sys/wait.h>
#include <linux/capability.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>

/*
 * This header is included from several translation units — the QEMU device
 * (whose qemu/osdep.h defines _GNU_SOURCE), the unit-test targets (whose
 * osdep.h stub does not), and tests/security/uid_isolate_test.c.  Rather than
 * force _GNU_SOURCE on every consumer, fill in the Linux/glibc extensions the
 * same way nvkvm_isolate.c already does for MS_REC / MS_PRIVATE / PR_CAP_AMBIENT:
 * fixed ABI constants, guarded so a libc that does declare them still wins.
 */
#ifndef CLONE_NEWNS
#define CLONE_NEWNS     0x00020000
#endif
#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS    0x04000000
#endif
#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC    0x08000000
#endif
#ifndef CLONE_NEWUSER
#define CLONE_NEWUSER   0x10000000
#endif
#ifndef CLONE_NEWPID
#define CLONE_NEWPID    0x20000000
#endif
#ifndef CLONE_NEWNET
#define CLONE_NEWNET    0x40000000
#endif
#ifndef O_PATH
#define O_PATH          010000000
#endif

/*
 * setresuid/setresgid/getresuid/getresgid/setgroups go through syscall(2)
 * rather than the glibc wrappers, for two reasons.  One, the wrappers need
 * _GNU_SOURCE (see above).  Two — and this is the load-bearing one — glibc's
 * wrappers implement the "setxid" broadcast that applies the change to every
 * thread via a signal.  These are only ever called in the just-forked isolate
 * child, which has exactly one thread, so the broadcast is pure overhead and a
 * signal-based mechanism is the last thing wanted between fork and exec.
 */
#define NVKVM_SETRESUID(r, e, s)  syscall(SYS_setresuid, (r), (e), (s))
#define NVKVM_SETRESGID(r, e, s)  syscall(SYS_setresgid, (r), (e), (s))
#define NVKVM_GETRESUID(r, e, s)  syscall(SYS_getresuid, (r), (e), (s))
#define NVKVM_GETRESGID(r, e, s)  syscall(SYS_getresgid, (r), (e), (s))
#define NVKVM_SETGROUPS(n, list)  syscall(SYS_setgroups, (n), (list))

/* ── Isolation layers and the four presets ───────────────────────────────
 *
 * The boundary is built from independent, cumulative LAYERS, not from four
 * exclusive code paths.  Four preset names select points on a degradation
 * ladder, ordered by the requirement each rung gives up:
 *
 *   preset      layers                            requires
 *   ---------   -------------------------------   ------------------------------
 *   namespace   ns + seccomp   (the default)      CAP_SYS_ADMIN, or a userns not
 *                                                 blocked by seccomp/AppArmor
 *   uid         uid + seccomp                     CAP_SETUID + CAP_SETGID
 *   seccomp     seccomp                           nothing — PR_SET_NO_NEW_PRIVS
 *                                                 and SECCOMP_SET_MODE_FILTER
 *                                                 both work fully unprivileged
 *   none        (nothing)                         nothing
 *
 * `seccomp` is a real rung, not filler: a hardened container that drops
 * CAP_SETUID kills uid mode outright while leaving the 20-syscall allowlist
 * completely functional.
 *
 * Because the layers are independent, combinations outside the four presets
 * are expressible without a fifth code path — `namespace+uid` (both, where
 * both are available) and `uid+chroot` (the container recommendation) are
 * just two layer bits each.
 *
 * PARSING RULE: every token turns on its layer, and seccomp is implied by any
 * confinement layer, because it is the floor of every real mode.  The only way
 * to end up without seccomp is the exclusive token `none`, which additionally
 * requires an explicit acknowledgement (see nvkvm_iso_needs_unsafe_ack).
 */
#define NVKVM_ISO_LAYER_SECCOMP 0x1u  /* stub's 20-syscall allowlist          */
#define NVKVM_ISO_LAYER_UID     0x2u  /* unique high UID/GID per isolate      */
#define NVKVM_ISO_LAYER_NS      0x4u  /* user+pid+net+ipc+uts+mount namespaces */
/*
 * chroot("/dev") confinement, an ADD-ON to uid mode for hosts that grant
 * CAP_SYS_CHROOT but not CAP_SYS_ADMIN — the shape of a stock Docker
 * container (measured CapEff: CHOWN DAC_OVERRIDE FOWNER FSETID KILL SETGID
 * SETUID SETPCAP NET_BIND_SERVICE SYS_CHROOT AUDIT_WRITE SETFCAP, i.e. the
 * Docker default minus MKNOD and NET_RAW).  pivot_root(2) needs CAP_SYS_ADMIN
 * in the target mount namespace and so is unavailable there; chroot(2) is not.
 *
 * We chroot to /dev itself rather than to a scratch directory, because a
 * scratch root would need the nvidia device nodes inside it and we can create
 * neither mounts (no CAP_SYS_ADMIN) nor device nodes (no CAP_MKNOD).  Rooting
 * at /dev keeps every node the stub opens reachable while making the rest of
 * the filesystem — including /proc and /etc — simply not exist for it.
 *
 * What this prevents and what it does not is spelled out in
 * docs/internal/isolate-model.md; the short version is that it removes host
 * filesystem access and /proc process enumeration, and does NOT remove access
 * to other /dev nodes (/dev/nvidia-uvm, sometimes /dev/kvm) or to /dev/shm.
 */
#define NVKVM_ISO_LAYER_CHROOT  0x8u  /* chroot("/dev") + clamped /dev dirfd  */

#define NVKVM_ISO_LAYERS_ALL \
	(NVKVM_ISO_LAYER_SECCOMP | NVKVM_ISO_LAYER_UID | \
	 NVKVM_ISO_LAYER_NS | NVKVM_ISO_LAYER_CHROOT)

/* Any layer that confines beyond the seccomp floor. */
#define NVKVM_ISO_LAYERS_CONFINING \
	(NVKVM_ISO_LAYER_UID | NVKVM_ISO_LAYER_NS | NVKVM_ISO_LAYER_CHROOT)

/*
 * `none` removes every boundary, so it is the one setting where a typo is
 * catastrophic.  It must be acknowledged explicitly:
 *     NVKVM_ISOLATE_MODE=none
 *     NVKVM_ISOLATE_UNSAFE_ACK=i-understand-this-removes-all-isolation
 * There is no precedent for a dangerous-option acknowledgement elsewhere in
 * the tree; this establishes one.
 */
/*
 * Sentinel meaning "not yet resolved — probe the ladder".  A high bit so it
 * can never be confused with a layer combination.
 */
#define NVKVM_ISO_MODE_AUTO        0x80000000u

#define NVKVM_ISO_UNSAFE_ACK_ENV   "NVKVM_ISOLATE_UNSAFE_ACK"
#define NVKVM_ISO_UNSAFE_ACK_VALUE "i-understand-this-removes-all-isolation"

/*
 * UID window.  Each isolate slot i (0 .. NVKVM_ISOLATE_MAX-1) gets
 * uid = gid = base + i, so a VM reserves [base, base + NVKVM_ISOLATE_MAX).
 * Uniqueness while live is a consequence of slot exclusivity: the slot is
 * only released in nvkvm_isolate_kill() AFTER waitpid() has reaped the
 * child, so no two live isolates of one VM can ever share a uid, and a uid
 * is never re-issued while a process still holds it.
 */
#define NVKVM_ISO_UID_BASE_DEFAULT   500000u
#define NVKVM_ISO_UID_SLOTS          4096u   /* == NVKVM_ISOLATE_MAX */

/*
 * Lowest base we accept.  Above 65535 keeps us clear of every real account,
 * of `nobody` (65534) and of the kernel's overflow uid/gid, which is exactly
 * the collision we must not have: an isolate sharing a uid with a real host
 * account could read and write that account's files.
 */
#define NVKVM_ISO_UID_BASE_MIN       65536u
#define NVKVM_ISO_UID_BASE_MAX       (0xFFFF0000u - NVKVM_ISO_UID_SLOTS)

struct nvkvm_isolate_cfg {
	unsigned  mode;        /* bitmask of NVKVM_ISO_MODE_*  */
	uint32_t  uid_base;    /* first uid/gid of this VM's window */
	bool      uid_base_explicit; /* an operator named it: never relocate */
};

/* ── Mode string parsing ─────────────────────────────────────────────────
 *
 * Accepts a '+' or ',' separated list of preset/layer tokens:
 *   namespace | ns | userns   → NVKVM_ISO_LAYER_NS
 *   uid | uidsep              → NVKVM_ISO_LAYER_UID
 *   chroot                    → NVKVM_ISO_LAYER_CHROOT (add-on to uid)
 *   seccomp                   → NVKVM_ISO_LAYER_SECCOMP (implied by the above)
 *   auto                      → probe the ladder at realize (exclusive)
 *   none | off                → no layers at all (exclusive, needs the ack)
 * Returns 0 on success (*out set), -1 on an unrecognised or illegal token.
 *
 * Deliberately strict: an unknown or misspelled mode is an error, never a
 * silent fallback to the default.  Silently degrading a security boundary
 * because someone typo'd "namesapce" is exactly the failure we refuse.
 */
static inline int nvkvm_iso_mode_parse(const char *s, unsigned *out,
				       char *err, size_t errsz)
{
	unsigned m = 0;
	bool saw_none = false, saw_auto = false, saw_any = false;
	char buf[128];

	if (!s || !*s) {
		if (err && errsz) snprintf(err, errsz, "empty isolation mode");
		return -1;
	}
	if (strlen(s) >= sizeof(buf)) {
		if (err && errsz) snprintf(err, errsz, "isolation mode string too long");
		return -1;
	}
	snprintf(buf, sizeof(buf), "%s", s);

	for (char *tok = strtok(buf, "+,| \t"); tok; tok = strtok(NULL, "+,| \t")) {
		saw_any = true;
		if (!strcmp(tok, "namespace") || !strcmp(tok, "ns") ||
		    !strcmp(tok, "userns"))
			m |= NVKVM_ISO_LAYER_NS;
		else if (!strcmp(tok, "uid") || !strcmp(tok, "uidsep"))
			m |= NVKVM_ISO_LAYER_UID;
		else if (!strcmp(tok, "chroot"))
			m |= NVKVM_ISO_LAYER_CHROOT;
		else if (!strcmp(tok, "seccomp"))
			m |= NVKVM_ISO_LAYER_SECCOMP;
		else if (!strcmp(tok, "auto"))
			saw_auto = true;
		else if (!strcmp(tok, "none") || !strcmp(tok, "off"))
			saw_none = true;
		else {
			if (err && errsz)
				snprintf(err, errsz,
					 "unknown isolation mode '%s' (presets: "
					 "auto, namespace, uid, seccomp, none; "
					 "combinations: uid+chroot, "
					 "namespace+uid)", tok);
			return -1;
		}
	}
	if (!saw_any) {
		if (err && errsz) snprintf(err, errsz, "empty isolation mode");
		return -1;
	}
	if (saw_none && (m || saw_auto)) {
		if (err && errsz)
			snprintf(err, errsz,
				 "isolation mode 'none' is exclusive — it cannot "
				 "be combined with any other layer");
		return -1;
	}
	if (saw_auto && m) {
		if (err && errsz)
			snprintf(err, errsz,
				 "isolation mode 'auto' is exclusive — it picks "
				 "the strongest available rung by probing; "
				 "name a rung explicitly to pin it");
		return -1;
	}
	if (saw_auto) {
		*out = NVKVM_ISO_MODE_AUTO;
		return 0;
	}
	/*
	 * 'chroot' is an add-on to uid mode, not a mode of its own, and it is
	 * meaningless alongside 'namespace' (which already pivot_roots into a
	 * minimal read-only root containing nothing but the nvidia nodes —
	 * strictly stronger than chroot("/dev")).  Reject both mistakes rather
	 * than quietly ignoring the token: an operator who wrote 'chroot' and
	 * got no chroot has a boundary they did not choose.
	 */
	if ((m & NVKVM_ISO_LAYER_CHROOT) && !(m & NVKVM_ISO_LAYER_UID)) {
		if (err && errsz)
			snprintf(err, errsz,
				 "isolation mode 'chroot' is an add-on to 'uid' "
				 "(use 'uid+chroot'), not a mode on its own");
		return -1;
	}
	if ((m & NVKVM_ISO_LAYER_CHROOT) && (m & NVKVM_ISO_LAYER_NS)) {
		if (err && errsz)
			snprintf(err, errsz,
				 "isolation mode 'chroot' cannot be combined "
				 "with 'namespace': namespace mode already "
				 "pivot_roots into a minimal read-only root, "
				 "which is strictly stronger");
		return -1;
	}
	/* seccomp is the floor of every real mode. */
	if (m & NVKVM_ISO_LAYERS_CONFINING)
		m |= NVKVM_ISO_LAYER_SECCOMP;
	*out = saw_none ? 0u : m;
	return 0;
}

/* True when this configuration removes every boundary and therefore needs the
 * explicit NVKVM_ISOLATE_UNSAFE_ACK acknowledgement. */
static inline bool nvkvm_iso_needs_unsafe_ack(unsigned mode)
{
	return (mode & NVKVM_ISO_LAYERS_ALL) == 0;
}

static inline const char *nvkvm_iso_mode_str(unsigned mode)
{
	switch (mode & NVKVM_ISO_LAYERS_ALL) {
	case NVKVM_ISO_LAYER_NS | NVKVM_ISO_LAYER_SECCOMP:
		return "namespace";
	case NVKVM_ISO_LAYER_UID | NVKVM_ISO_LAYER_SECCOMP:
		return "uid";
	case NVKVM_ISO_LAYER_UID | NVKVM_ISO_LAYER_CHROOT | NVKVM_ISO_LAYER_SECCOMP:
		return "uid+chroot";
	case NVKVM_ISO_LAYER_NS | NVKVM_ISO_LAYER_UID | NVKVM_ISO_LAYER_SECCOMP:
		return "namespace+uid";
	case NVKVM_ISO_LAYER_SECCOMP:
		return "seccomp";
	case 0:
		return "none";
	default:
		return "custom";
	}
}

/* ── uid for an isolate slot ─────────────────────────────────────────────
 * Returns 0 and sets *uid_out, or -1 if the slot is outside the window.
 */
static inline int nvkvm_iso_uid_for_slot(uint32_t base, uint32_t slot,
					 uid_t *uid_out)
{
	if (slot >= NVKVM_ISO_UID_SLOTS)
		return -1;
	if (base < NVKVM_ISO_UID_BASE_MIN || base > NVKVM_ISO_UID_BASE_MAX)
		return -1;
	*uid_out = (uid_t)(base + slot);
	return 0;
}

/* ── Capability probe ────────────────────────────────────────────────────
 *
 * Ask the kernel directly rather than inferring from geteuid(): QEMU may be
 * running as a non-root user that was granted file capabilities, or as root
 * inside a container whose bounding set has had CAP_SETUID removed — both
 * cases where euid is a lie.  capget() on pid 0 (self) is the ground truth.
 *
 * Checks the EFFECTIVE set: that is what setresuid() actually consults.
 */
static inline int nvkvm_iso_have_caps(bool *setuid_ok, bool *setgid_ok,
				      bool *chroot_ok)
{
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0
	};
	struct __user_cap_data_struct data[2];

	memset(data, 0, sizeof(data));
	if (syscall(SYS_capget, &hdr, data) != 0)
		return -1;
	/* CAP_SETGID = 6, CAP_SETUID = 7 — both in the low 32-bit word. */
	*setgid_ok = (data[0].effective & (1u << CAP_SETGID)) != 0;
	*setuid_ok = (data[0].effective & (1u << CAP_SETUID)) != 0;
	if (chroot_ok)
		*chroot_ok = (data[0].effective & (1u << CAP_SYS_CHROOT)) != 0;
	return 0;
}

/*
 * Is one arbitrary capability in our EFFECTIVE set?  1 = yes, 0 = no,
 * -1 = capget failed.  Used for CAP_SETPCAP, which no rung *requires* but
 * whose absence silently weakens the uid rungs -- see nvkvm_iso_auto_select().
 */
static inline int nvkvm_iso_cap_effective(int cap)
{
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0
	};
	struct __user_cap_data_struct data[2];

	memset(data, 0, sizeof(data));
	if (syscall(SYS_capget, &hdr, data) != 0)
		return -1;
	return (data[cap >> 5].effective & (1u << (cap & 31))) != 0;
}

static inline int nvkvm_iso_have_setid_caps(bool *setuid_ok, bool *setgid_ok)
{
	return nvkvm_iso_have_caps(setuid_ok, setgid_ok, NULL);
}

/*
 * Is `path` openable read/write by an arbitrary unrelated uid?
 *   1 = yes, 0 = no (and *mode_out holds the mode for the diagnostic),
 *  -1 = stat failed (ENOENT etc).
 *
 * This matters because UID mode drops ALL supplementary groups before
 * dropping privilege (see nvkvm_iso_drop_privilege), so group-based access
 * to /dev/nvidia* — the common `root:video 0660` layout — is gone.  The
 * other-bits are the only thing left.  Checking here converts a confusing
 * runtime "open ctl/gpu FAILED r1=-13" into a startup error that names the
 * file and its mode.
 */
static inline int nvkvm_iso_node_world_rw(const char *path, unsigned *mode_out)
{
	struct stat st;
	if (stat(path, &st) != 0)
		return -1;
	if (mode_out)
		*mode_out = (unsigned)(st.st_mode & 07777);
	return ((st.st_mode & S_IROTH) && (st.st_mode & S_IWOTH)) ? 1 : 0;
}

/* ── Parent-side userns mapping (SHARED with the real spawn) ─────────────
 *
 * These two functions came out of nvkvm_isolate.c so that
 * nvkvm_iso_probe_namespaces() below and nvkvm_isolate_create()'s real spawn
 * path call THE SAME code, for the same reason the rest of this header is
 * header-only: a probe that attempts a different thing than the spawn performs
 * is a probe that lies.  It did lie -- see the probe's own comment.
 *
 * Nothing else changed in the move; nvkvm_isolate.c calls
 * nvkvm_map_child_userns() with exactly the arguments it always did.
 */
/* Parent-side: write one /proc/<pid>/<which> map file for the child's userns. */
static inline int nvkvm_write_child_map(pid_t pid, const char *which, const char *val)
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
static inline int nvkvm_map_child_userns(pid_t pid, uid_t extra_uid, gid_t extra_gid)
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

/* ── Ladder probes (used by mode `auto`) ─────────────────────────────────
 *
 * Every probe ATTEMPTS the thing rather than inferring it from configuration,
 * because inference is measurably wrong.  On a stock `docker run` container
 * kernel.unprivileged_userns_clone reads 1 and user.max_user_namespaces reads
 * 55416 — both saying user namespaces are available — while the default
 * seccomp profile and the docker-default AppArmor policy refuse the clone.
 * An operator reading those sysctls picks `namespace` and hits a runtime
 * failure; attempting the clone simply knows.
 */

/*
 * Which privileged step of the real spawn failed.  The real spawn does three
 * of them and the probe must attempt all three, in the same order, or it can
 * report a rung that then cannot run.
 */
/* Feature macro: the probe reports WHICH step failed.  An enum constant is
 * invisible to #ifdef, and tests/security/ns_probe_test.c needs to build
 * against both this header and the pre-fix one to demonstrate the regression. */
#define NVKVM_ISO_NS_STEP_AWARE 1

enum nvkvm_iso_ns_step {
	NVKVM_ISO_NS_OK    = 0,
	NVKVM_ISO_NS_CLONE,   /* clone(CLONE_NEWUSER|...) itself was refused   */
	NVKVM_ISO_NS_MAP,     /* parent could not write the child's uid/gid map */
	NVKVM_ISO_NS_MOUNT,   /* child could not USE the namespace it got      */
};

/*
 * Turn a probe failure into the sentence an operator can act on.  Same
 * treatment the clone-failure path in nvkvm_isolate.c already gets: name the
 * likely cause and the knob, because "EPERM writing uid_map" is otherwise a
 * mystery even to someone who knows namespaces.
 */
static inline void nvkvm_iso_ns_step_explain(enum nvkvm_iso_ns_step step,
					     int err, char *out, size_t outsz)
{
	switch (step) {
	case NVKVM_ISO_NS_CLONE:
		snprintf(out, outsz,
			 "clone(CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNET|"
			 "CLONE_NEWIPC|CLONE_NEWUTS|CLONE_NEWNS) failed: %s. "
			 "Typical under the default Docker seccomp/AppArmor "
			 "profile, which blocks CLONE_NEWUSER regardless of "
			 "kernel.unprivileged_userns_clone and "
			 "user.max_user_namespaces.", strerror(err));
		return;
	case NVKVM_ISO_NS_MAP:
		snprintf(out, outsz,
			 "the namespace was created but its uid/gid map could "
			 "not be written: %s. The map is `0 <euid> 1`, so when "
			 "QEMU runs as uid 0 it maps UID 0 of the PARENT "
			 "namespace, and since Linux 5.12 that requires "
			 "CAP_SETFCAP in the parent namespace (user_namespaces(7), "
			 "\"writing to uid_map and gid_map\"); no other "
			 "capability substitutes for it, CAP_SYS_ADMIN "
			 "included. Either add CAP_SETFCAP "
			 "(--cap-add=SETFCAP) or run QEMU as a non-root uid, "
			 "for which the rule does not apply.", strerror(err));
		return;
	case NVKVM_ISO_NS_MOUNT:
		snprintf(out, outsz,
			 "the namespace was created and mapped but could not be "
			 "used: mount(MS_REC|MS_PRIVATE) on / failed: %s. "
			 "Typical on Ubuntu 23.10+ with the default "
			 "kernel.apparmor_restrict_unprivileged_userns=1, which "
			 "lets an unprivileged process create the user namespace "
			 "and then refuses the mount operations inside it.",
			 strerror(err));
		return;
	case NVKVM_ISO_NS_OK:
	default:
		snprintf(out, outsz, "no failure");
		return;
	}
}

/*
 * Can we create AND USE the namespace set the sandbox needs?  Performs a real
 * clone(2) with the production flags and then every other privileged step the
 * real spawn performs, in the same order, using the same code:
 *
 *   1. clone(CLONE_NEWUSER|NEWPID|NEWNET|NEWIPC|NEWUTS|NEWNS)
 *   2. parent writes setgroups/uid_map/gid_map  <- nvkvm_map_child_userns()
 *   3. child does mount(NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL)
 *
 * Returns 0 if all three succeed; -1 with *err_out set and *step_out naming
 * the step that failed.  Both out-params are optional.
 *
 * STEP 2 IS WHY THIS FUNCTION LOOKS EXPENSIVE FOR A PROBE, and it is not
 * optional.  An earlier version stopped after step 3 without ever attempting
 * step 2, which made it fail OPEN on a real configuration: with CAP_SYS_ADMIN
 * and without CAP_SETFCAP the clone succeeds, the mount succeeds, the probe
 * says "namespace available", `auto` pins the strongest rung -- and then every
 * isolate dies in nvkvm_map_child_userns() with EPERM, so the guest gets no
 * GPU at all.  Measured; tests/security/ns_probe_test.c is the regression test.
 *
 * The sync pipe is the same handshake the real spawn uses (pipe2(O_CLOEXEC),
 * child blocks on a 1-byte read, EOF means the parent's map write failed and
 * the child must not proceed).  Reusing the shape matters: the child MUST be
 * alive while the parent writes its map, and it must do the mount AFTER the
 * map lands, because that is the order and the credential state the real
 * isolate runs in.
 */
static inline int nvkvm_iso_probe_namespaces(int *err_out,
					     enum nvkvm_iso_ns_step *step_out)
{
	unsigned long flags = CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET |
			      CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWNS |
			      (unsigned long)SIGCHLD;
	int syncpipe[2] = { -1, -1 };
	int err = 0, status = 0, map_rc;
	enum nvkvm_iso_ns_step step = NVKVM_ISO_NS_OK;
	pid_t pid;

	/* pipe2 via syscall(): this header is included by translation units that
	 * do not define _GNU_SOURCE, so the libc wrapper may not be declared. */
#ifdef SYS_pipe2
	if (syscall(SYS_pipe2, syncpipe, O_CLOEXEC) != 0)
#else
	if (pipe(syncpipe) != 0)
#endif
	{
		/* Not a statement about namespaces, but we cannot run the probe
		 * without it.  Report as the first step so the caller degrades
		 * rather than trusting an answer we never obtained. */
		err = errno;
		step = NVKVM_ISO_NS_CLONE;
		goto out;
	}

	pid = (pid_t)syscall(SYS_clone, flags, (void *)0, (void *)0,
			     (void *)0, 0UL);
	if (pid == 0) {
		char go;
		close(syncpipe[1]);
		/* Same fail-closed handshake as the real child: a short read
		 * means the parent could not map us, and we must not report
		 * success by exiting 0. */
		if (read(syncpipe[0], &go, 1) != 1)
			_exit(126);
		close(syncpipe[0]);
		/*
		 * USE the namespace, do not merely enter it.  A successful
		 * clone(CLONE_NEWUSER|...) does NOT mean the namespace is
		 * usable: on Ubuntu 23.10 and later, with the default
		 * kernel.apparmor_restrict_unprivileged_userns=1, an
		 * unprivileged process creates the user namespace fine and is
		 * then refused the mount operations inside it.  A probe that
		 * only clones reports "namespace available", auto selects the
		 * strongest rung, and every isolate afterwards dies in
		 * nvkvm_child_enter_mount_ns() -- measured as
		 * mount(NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL) = -1 EACCES,
		 * with QEMU running as a non-root user, which is exactly how
		 * libvirt runs it.
		 *
		 * This is the first privileged thing the real child does after
		 * the map lands, and it is the operation that gets refused.
		 */
		if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
			_exit(errno & 0x7f ? errno & 0x7f : 1);
		_exit(0);
	}
	if (pid < 0) {
		err = errno;
		step = NVKVM_ISO_NS_CLONE;
		close(syncpipe[0]);
		close(syncpipe[1]);
		goto out;
	}

	close(syncpipe[0]);
	/*
	 * extra_uid/extra_gid are 0 because `auto` only ever selects
	 * NVKVM_ISO_LAYER_NS|NVKVM_ISO_LAYER_SECCOMP -- no UID layer -- and
	 * that is the rung this probe decides.  Combined `namespace+uid` writes
	 * the two-line map instead, which additionally needs CAP_SETUID and
	 * CAP_SETGID in the parent; it is never auto-selected and is validated
	 * up front by nvkvm_iso_cfg_validate(), which demands both.
	 */
	errno = 0;
	map_rc = nvkvm_map_child_userns(pid, 0, 0);
	if (map_rc != 0)
		err = errno ? errno : EPERM;
	else if (write(syncpipe[1], "x", 1) != 1) {
		err = errno ? errno : EIO;
		map_rc = -1;
	}
	/* On failure this close is the EOF that makes the child _exit(126). */
	close(syncpipe[1]);

	/* Reap.  If QEMU's own SIGCHLD handling got there first waitpid fails
	 * with ECHILD, which tells us nothing about the probe -- the steps we
	 * care about have already reported themselves. */
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;

	if (map_rc != 0) {
		step = NVKVM_ISO_NS_MAP;
		goto out;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		/* The namespace exists but cannot be used.  Treat exactly as
		 * "namespace unavailable" so auto falls to the next rung.
		 * 126 cannot appear here: it means the child saw EOF on the
		 * sync pipe, which only happens when map_rc != 0 above. */
		err = WEXITSTATUS(status);
		step = NVKVM_ISO_NS_MOUNT;
		goto out;
	}

out:
	if (err_out)  *err_out  = (step == NVKVM_ISO_NS_OK) ? 0 : err;
	if (step_out) *step_out = step;
	return (step == NVKVM_ISO_NS_OK) ? 0 : -1;
}

/*
 * Can we do UID separation?  Needs CAP_SETUID + CAP_SETGID, and — because the
 * drop clears supplementary groups — device nodes openable by "other".
 * Returns 0 / -1 with a reason string.
 */
static inline int nvkvm_iso_probe_uid(char *why, size_t whysz)
{
	bool su = false, sg = false;
	unsigned m = 0;

	if (nvkvm_iso_have_caps(&su, &sg, NULL) != 0) {
		snprintf(why, whysz, "capget failed: %s", strerror(errno));
		return -1;
	}
	if (!su || !sg) {
		snprintf(why, whysz, "CAP_SETUID=%s CAP_SETGID=%s",
			 su ? "yes" : "no", sg ? "yes" : "no");
		return -1;
	}
	if (nvkvm_iso_node_world_rw("/dev/nvidiactl", &m) == 0) {
		snprintf(why, whysz,
			 "/dev/nvidiactl is mode 0%o, not other-rw (uid mode "
			 "drops supplementary groups)", m);
		return -1;
	}
	return 0;
}

/*
 * Is seccomp filter mode available at all?  Passing a NULL filter to
 * seccomp(SECCOMP_SET_MODE_FILTER, ...) faults BEFORE installing anything, so
 * EFAULT is the "supported" answer and nothing is applied to this process.
 * ENOSYS means no seccomp syscall; EINVAL means CONFIG_SECCOMP_FILTER is off.
 *
 * EACCES IS ALSO A "SUPPORTED" ANSWER, and this is the one non-obvious case.
 * The kernel refuses SECCOMP_SET_MODE_FILTER with EACCES when the caller has
 * neither no_new_privs nor CAP_SYS_ADMIN.  That is a statement about *this*
 * process, not about the host: the filter is installed by the STUB, which sets
 * PR_SET_NO_NEW_PRIVS in main() before it spawns its workers and only then
 * calls seccomp() with TSYNC.  QEMU deliberately does not set no_new_privs on
 * itself just to run a probe -- it is irreversible and would apply to every
 * later execve QEMU does.  So treating EACCES as unsupported would make `auto`
 * refuse to start on a host where the stub's filter would install perfectly.
 *
 * MEASURED (kernel 7.0.0-29, 2026-08-27): this kernel checks the filter
 * pointer first, so the probe returns EFAULT in every combination tested --
 * root with full caps, root with CapEff=0, uid 65534, with and without
 * no_new_privs.  EACCES was never observed, so this arm is defensive against a
 * kernel that orders the two checks the other way, not a fix for an observed
 * failure.  It is the fail-CLOSED direction (the old code would have refused
 * to start), which is why it was not the bug the namespace probe had.
 */
static inline int nvkvm_iso_probe_seccomp(int *err_out)
{
	errno = 0;
	long r = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, (void *)0);
	if (r == 0)                     /* should be impossible; be conservative */
		return 0;
	if (errno == EFAULT || errno == EACCES) {
		if (err_out) *err_out = 0;
		return 0;
	}
	if (err_out) *err_out = errno;
	return -1;
}

/*
 * Append to a report buffer, clamped.
 *
 * snprintf() returns the length it WOULD have written, so the idiom
 * `n += snprintf(buf + n, sz - n, ...)` walks n past the end of the buffer as
 * soon as anything truncates -- after which `buf + n` is out of bounds and
 * `sz - n` underflows to a huge size_t.  auto_select() used that idiom and the
 * step-aware explanations below are long enough to reach it, so clamp once
 * here rather than reasoning aboutevery call site.
 */
static inline size_t nvkvm_iso_report_append(char *buf, size_t sz, size_t n,
					     const char *fmt, ...)
{
	va_list ap;
	int w;

	if (n >= sz)
		return sz ? sz - 1 : 0;
	va_start(ap, fmt);
	w = vsnprintf(buf + n, sz - n, fmt, ap);
	va_end(ap);
	if (w < 0)
		return n;
	n += (size_t)w;
	return n >= sz ? sz - 1 : n;
}

/*
 * Resolve mode `auto`: try namespace, fall back to uid (with +chroot when
 * CAP_SYS_CHROOT allows it), fall back to seccomp.
 *
 * **It never selects `none`.** The ladder stops at seccomp; if even seccomp
 * cannot be installed this returns -1 and the caller refuses to start. `none`
 * is reachable only by explicit configuration plus the acknowledgement.
 *
 * `report` receives a multi-line account of what was attempted and why each
 * stronger rung was rejected — the caller logs it at warning level whenever
 * the result is weaker than `namespace`, so an operator reading their logs
 * discovers the weaker boundary without going looking for it.
 */
static inline int nvkvm_iso_auto_select(unsigned *mode_out, char *report,
					size_t reportsz, char *err,
					size_t errsz)
{
	char why[192];
	char nswhy[512];
	enum nvkvm_iso_ns_step nsstep = NVKVM_ISO_NS_OK;
	int e = 0;
	size_t n = 0;

	if (nvkvm_iso_probe_namespaces(&e, &nsstep) == 0) {
		*mode_out = NVKVM_ISO_LAYER_NS | NVKVM_ISO_LAYER_SECCOMP;
		snprintf(report, reportsz,
			 "isolate mode auto -> namespace (strongest rung; "
			 "clone(CLONE_NEWUSER|...), the uid/gid map write and "
			 "the in-namespace mount all succeeded)");
		return 0;
	}
	/* Name the step that actually failed.  The old text blamed the clone
	 * unconditionally, which was wrong the moment the probe grew a second
	 * failure mode -- and the map-write mode is the one nobody can debug
	 * from a bare EPERM. */
	nvkvm_iso_ns_step_explain(nsstep, e, nswhy, sizeof(nswhy));
	n = nvkvm_iso_report_append(report, reportsz, n,
				    "isolate mode auto: 'namespace' UNAVAILABLE — %s\n",
				    nswhy);

	why[0] = '\0';
	if (nvkvm_iso_probe_uid(why, sizeof(why)) == 0) {
		bool su = false, sg = false, cc = false;
		nvkvm_iso_have_caps(&su, &sg, &cc);
		*mode_out = NVKVM_ISO_LAYER_UID | NVKVM_ISO_LAYER_SECCOMP |
			    (cc ? NVKVM_ISO_LAYER_CHROOT : 0u);
		n = nvkvm_iso_report_append(report, reportsz, n,
			 "isolate mode auto -> %s. %s"
			 "This is a MATERIALLY WEAKER boundary than namespace "
			 "mode: no pid, mount, net, ipc or uts isolation. See "
			 "docs/internal/isolate-model.md before relying on it.",
			 nvkvm_iso_mode_str(*mode_out),
			 cc ? "CAP_SYS_CHROOT is present, so chroot(\"/dev\") "
			      "confinement is included. "
			    : "CAP_SYS_CHROOT is ABSENT, so the isolate can "
			      "read the whole host filesystem. ");
		/*
		 * CAP_SETPCAP is not required by this rung and must not gate it
		 * -- rejecting it here would push a host that grants
		 * SETUID/SETGID and not SETPCAP all the way down to the
		 * `seccomp` rung, which does not separate isolates from each
		 * other at all.  But its absence is not free either:
		 * nvkvm_drop_caps_pre()'s PR_CAPBSET_DROP loop needs it and
		 * ignores its return value, so without it the isolate silently
		 * keeps a FULL capability bounding set.  Measured: with
		 * CAP_SETPCAP out of the bounding set every PR_CAPBSET_DROP
		 * returns EPERM and CapBnd stays at 000001fffffffeff.
		 *
		 * That is inert in practice -- after the uid drop the isolate
		 * holds no permitted caps, no_new_privs is set, and execve is
		 * not in the stub's seccomp allowlist, so there is no path to
		 * regain anything -- but "inert in practice" is not something
		 * an operator should have to rediscover from /proc.  Say it.
		 */
		if (nvkvm_iso_cap_effective(CAP_SETPCAP) == 0) {
			n = nvkvm_iso_report_append(report, reportsz, n,
				 " CAP_SETPCAP is ABSENT, so the isolate's "
				 "capability BOUNDING set cannot be dropped and "
				 "stays full (CapBnd unchanged in "
				 "/proc/<isolate>/status). Its effective and "
				 "permitted sets are still zeroed and "
				 "no_new_privs is still set, so this is a "
				 "weakened defence in depth rather than a live "
				 "capability -- add --cap-add=SETPCAP to close "
				 "it.");
		}
		(void)n;
		return 0;
	}
	n = nvkvm_iso_report_append(report, reportsz, n,
				    "isolate mode auto: 'uid' UNAVAILABLE — %s.\n",
				    why);

	if (nvkvm_iso_probe_seccomp(&e) == 0) {
		*mode_out = NVKVM_ISO_LAYER_SECCOMP;
		n = nvkvm_iso_report_append(report, reportsz, n,
			 "isolate mode auto -> seccomp (LOWEST rung auto will "
			 "ever select; it never selects 'none'). The isolate "
			 "gets the 20-syscall allowlist and nothing else: it "
			 "runs as the QEMU user, in QEMU's namespaces, with "
			 "the whole host filesystem visible. Isolates are NOT "
			 "separated from each other.");
		return 0;
	}
	snprintf(err, errsz,
		 "isolate mode auto: every rung failed. namespace: %s uid: %s; "
		 "seccomp: SECCOMP_SET_MODE_FILTER unavailable (%s). auto will "
		 "not select 'none', so there is nothing left to fall back to — "
		 "refusing to start. Set NVKVM_ISOLATE_MODE=none plus %s=%s if "
		 "you genuinely want no confinement at all.",
		 nswhy, why, strerror(e), NVKVM_ISO_UNSAFE_ACK_ENV,
		 NVKVM_ISO_UNSAFE_ACK_VALUE);
	return -1;
}

/* ── Configuration validation ────────────────────────────────────────────
 *
 * Called ONCE, up front, at device realize — not at the first setresuid().
 * Returns 0 if the configured mode can actually run on this host, -1 with a
 * human-readable reason in `err`.  Never falls back to another mode.
 */
static inline int nvkvm_iso_cfg_validate(const struct nvkvm_isolate_cfg *cfg,
					 char *err, size_t errsz)
{
	if (!(cfg->mode & NVKVM_ISO_LAYER_UID))
		return 0;   /* nothing UID-specific to check */

	if (cfg->uid_base < NVKVM_ISO_UID_BASE_MIN ||
	    cfg->uid_base > NVKVM_ISO_UID_BASE_MAX) {
		snprintf(err, errsz,
			 "isolate uid base %u out of range (must be %u..%u): "
			 "a base at or below 65535 can collide with real host "
			 "accounts, `nobody`, or the kernel overflow uid",
			 cfg->uid_base, NVKVM_ISO_UID_BASE_MIN,
			 NVKVM_ISO_UID_BASE_MAX);
		return -1;
	}

	bool can_setuid = false, can_setgid = false, can_chroot = false;
	if (nvkvm_iso_have_caps(&can_setuid, &can_setgid, &can_chroot) != 0) {
		snprintf(err, errsz,
			 "capget() failed (%s): cannot verify CAP_SETUID/"
			 "CAP_SETGID, refusing to start in uid isolation mode",
			 strerror(errno));
		return -1;
	}
	if (!can_setuid || !can_setgid) {
		snprintf(err, errsz,
			 "isolate mode '%s' needs CAP_SETUID and CAP_SETGID in "
			 "the QEMU process, but capget() reports setuid=%s "
			 "setgid=%s (euid=%u). Run QEMU as root, or grant "
			 "'cap_setuid,cap_setgid+ep', or add --cap-add=SETUID "
			 "--cap-add=SETGID to the container. Refusing to start: "
			 "nvkvm will not silently fall back to a weaker mode.",
			 nvkvm_iso_mode_str(cfg->mode),
			 can_setuid ? "yes" : "NO", can_setgid ? "yes" : "NO",
			 (unsigned)geteuid());
		return -1;
	}

	if ((cfg->mode & NVKVM_ISO_LAYER_CHROOT) && !can_chroot) {
		snprintf(err, errsz,
			 "isolate mode '%s' needs CAP_SYS_CHROOT, which capget() "
			 "reports as absent (euid=%u). Add --cap-add=SYS_CHROOT, "
			 "or drop '+chroot' from NVKVM_ISOLATE_MODE and accept "
			 "the wider filesystem exposure documented in "
			 "docs/internal/isolate-model.md. Refusing to start "
			 "rather than silently running without the chroot.",
			 nvkvm_iso_mode_str(cfg->mode), (unsigned)geteuid());
		return -1;
	}

	/*
	 * Supplementary groups are dropped, so /dev/nvidiactl must be readable
	 * and writable by "other".  Without this the stub's very first open
	 * fails with EACCES and forwarding comes up silently off.
	 */
	unsigned m = 0;
	int rw = nvkvm_iso_node_world_rw("/dev/nvidiactl", &m);
	if (rw == 0) {
		snprintf(err, errsz,
			 "/dev/nvidiactl is mode 0%o: not readable/writable by "
			 "'other'. uid isolation mode drops all supplementary "
			 "groups, so group-based access does not apply to the "
			 "isolate. chmod 0666 /dev/nvidiactl /dev/nvidia* (the "
			 "NVIDIA installer's default) or use namespace mode.", m);
		return -1;
	}
	/* rw < 0 (missing node) is not our error to report — device realize
	 * already opens /dev/nvidiactl itself and fails with a better message. */
	return 0;
}

/* ── chroot confinement (child side) ─────────────────────────────────────
 *
 * Child-side confinement for UID mode on hosts that grant CAP_SYS_CHROOT but
 * not CAP_SYS_ADMIN — the shape of a stock Docker container.  Runs in the
 * forked child, BEFORE the privilege drop (chroot needs CAP_SYS_CHROOT) and
 * before the capability drop.  Returns 0 / -1; the caller _exit()s on failure,
 * so a stub never runs with a confinement that only half applied.
 *
 * We chroot to /dev itself.  The obvious alternative — a scratch root holding
 * only the nvidia nodes, mirroring what nvkvm_child_enter_mount_ns() builds —
 * is not reachable here: populating it needs either bind mounts (CAP_SYS_ADMIN)
 * or mknod (CAP_MKNOD), and the target environment has neither.  Rooting at
 * /dev keeps every node the stub opens reachable and makes /proc, /etc, /home,
 * /sys and the rest of the host filesystem simply not exist for it.
 *
 * Three details are load-bearing:
 *
 *  1. chdir("/") AFTER chroot.  chroot(2) does not move the cwd, and a cwd
 *     left outside the new root is the textbook escape — the process just
 *     walks "../../.." out of it.  This is the same class of bug as the
 *     O_PATH host-/dev dirfd the tree already fixed once.
 *  2. The /dev dirfd handed to the stub is the chroot root itself, opened
 *     AFTER the chroot.  ".." at the process root is clamped by the kernel to
 *     the root, so openat(dd, "../../etc/shadow") cannot walk out — the same
 *     property pivot_root gives namespace mode, obtained differently.
 *  3. All capabilities, CAP_SYS_CHROOT included, are dropped immediately after
 *     (nvkvm_drop_all_caps), and no_new_privs is set.  A chroot is escapable
 *     by a process that RETAINS CAP_SYS_CHROOT (the classic double-chroot
 *     trick) or that holds a directory fd from outside; the caller closes every
 *     inherited fd before this runs, and the cap drop closes the other route.
 *
 * This is weaker than the namespace sandbox and is not sold as equivalent:
 * /dev/shm stays writable and shared with the host, and every other node in
 * /dev that the isolate's uid may open — /dev/nvidia-uvm, often /dev/kvm —
 * stays reachable.  See docs/internal/isolate-model.md.
 */
static inline int nvkvm_iso_enter_chroot(int dev_dirfd_slot)
{
	int dd;

	if (chroot("/dev") < 0)
		return -1;
	if (chdir("/") < 0)          /* MUST follow chroot — see (1) above */
		return -1;
	dd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (dd < 0)
		return -1;
	if (dd != dev_dirfd_slot) {
		if (dup2(dd, dev_dirfd_slot) < 0) {
			close(dd);
			return -1;
		}
		close(dd);
	}
	return 0;
}

/* ── The privilege drop (child side) ─────────────────────────────────────
 *
 * Runs in the forked/cloned child, after any mount-namespace work (which
 * needs CAP_SYS_ADMIN) and before nvkvm_drop_all_caps().  Returns 0 on a
 * verified-irreversible drop, -1 on ANY problem — the caller _exit()s.
 *
 * The classic ordering bug this avoids: setuid() BEFORE setgid() leaves the
 * process unprivileged and therefore unable to change its gid, so it keeps
 * the parent's (root's) group — a drop that only half happened.  GID first,
 * then UID, then verify.
 *
 * There is deliberately no "tolerate EPERM from setgroups" escape hatch. An
 * earlier version had one, for combined namespace+uid mode where the parent
 * wrote "deny" to /proc/<pid>/setgroups and setgroups(2) is then permanently
 * EPERM. That produced a uid-separated isolate still carrying the host root
 * group (measured: `Uid: 500004 ... Groups: 0`). The parent now leaves the
 * policy at its default for combined mode instead, so clearing the group list
 * must succeed in every mode and a failure here is a failure, full stop.
 */
static inline int nvkvm_iso_drop_privilege(uid_t uid, gid_t gid)
{
	uid_t ru, eu, su;
	gid_t rg, eg, sg;

	/* Refuse to "drop" to a privileged id — a misconfigured base that
	 * resolved to 0 must fail loudly, not run the stub as root. */
	if (uid == 0 || gid == 0)
		return -1;

	/* 1. Supplementary groups first, while we still have CAP_SETGID. */
	if (NVKVM_SETGROUPS(0, NULL) != 0)
		return -1;

	/* 2. GID before UID.  setresgid sets real, effective AND saved, so
	 *    there is no saved-gid left to restore from. */
	if (NVKVM_SETRESGID(gid, gid, gid) != 0)
		return -1;

	/* 3. Then UID.  Same reasoning: saved-uid must go too, otherwise the
	 *    process can seteuid() straight back to root. */
	if (NVKVM_SETRESUID(uid, uid, uid) != 0)
		return -1;

	/* 4. Verify all three of each actually changed. */
	if (NVKVM_GETRESUID(&ru, &eu, &su) != 0 ||
	    NVKVM_GETRESGID(&rg, &eg, &sg) != 0)
		return -1;
	if (ru != uid || eu != uid || su != uid)
		return -1;
	if (rg != gid || eg != gid || sg != gid)
		return -1;
	if (syscall(SYS_getgroups, 0, NULL) != 0)
		return -1;

	/*
	 * 5. Verify the drop is IRREVERSIBLE.  We do not assume it: we try to
	 *    undo it and require every attempt to fail.  If any of these ever
	 *    succeeded the isolate would be running as root and the whole mode
	 *    would be a fiction, so this check runs in production, not only in
	 *    the test.
	 */
	if (NVKVM_SETRESUID(0, 0, 0) == 0)                return -1;
	if (syscall(SYS_setuid, 0) == 0)                  return -1;
	if (NVKVM_SETRESUID((uid_t)-1, 0, (uid_t)-1) == 0) return -1;  /* seteuid */
	if (NVKVM_SETRESGID(0, 0, 0) == 0)                return -1;
	if (NVKVM_SETRESGID((gid_t)-1, 0, (gid_t)-1) == 0) return -1;  /* setegid */
	/* And confirm nothing moved. */
	if (NVKVM_GETRESUID(&ru, &eu, &su) != 0 ||
	    ru != uid || eu != uid || su != uid)
		return -1;
	if (NVKVM_GETRESGID(&rg, &eg, &sg) != 0 ||
	    rg != gid || eg != gid || sg != gid)
		return -1;

	return 0;
}

#endif /* NVKVM_ISOLATE_UID_H */
