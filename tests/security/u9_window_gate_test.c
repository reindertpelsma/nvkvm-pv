/*
 * u9_window_gate_test.c — adversarial probe for the U-9 guest-mapping window,
 *                         against the REAL stub over the REAL command socket.
 *
 * NEEDS NO GPU, NO VM, NO GUEST.  It is in tests/security rather than
 * tests/unit only because it drives a spawned binary rather than a linked
 * function; run it on any Linux box:
 *
 *   make -C src/stub
 *   gcc -O0 -Wall -o /tmp/u9_window_gate_test tests/security/u9_window_gate_test.c
 *   /tmp/u9_window_gate_test src/stub/nvkvm_stub
 *
 * Exit: 0 = every out-of-window probe was refused AND every in-window one was
 * accepted, 1 otherwise.
 *
 * WHY THIS EXISTS ALONGSIDE tests/unit/test_stub_window.c.  That suite pins
 * the PREDICATE (extracted from source) and the ALLOCATOR that feeds it.
 * Neither can see whether the predicate is actually WIRED INTO the command
 * handlers — a stub with stub_window_contains() defined and never called
 * passes the whole unit suite.  This one sends real ISOLATE_CMD_MMAP /
 * ISOLATE_CMD_MUNMAP / ISOLATE_CMD_WINDOW_INFO messages to a real stub
 * process and looks at what comes back, so it fails if the call site is
 * removed.  That is the gap the A-1 work's two false passes lived in.
 *
 * TWO THINGS THIS TEST GOT WRONG FIRST, both recorded because they are the
 * shape of a green result that measures nothing:
 *
 *   1. A hardcoded "outside" address of 0x7f1122200000 was ACCEPTED, and the
 *      gate was right: the window is 1 TiB and that run had placed it at
 *      0x7e46fb400000, so the address really was inside. Guest VAs and the
 *      window are drawn from the same band of the address space — an
 *      "obviously outside" constant is not outside. Every probe below is
 *      derived from the base WINDOW_INFO reports, never guessed.
 *
 *   2. The MMAP probe asserted only "retval != 0" at first. It got -EBADF,
 *      because handle_id 0 resolves to nothing and the handle lookup used to
 *      run before the window check — so the address was never examined and
 *      the case would have passed with the gate deleted. The fix was on both
 *      sides: the stub now checks the address FIRST (a control behind another
 *      check is a control a later edit can bypass), and this test asserts the
 *      exact errno that only the window check produces.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>

/* src/common/nvkvm_isolate_proto.h — duplicated, not included, because that
 * header pulls in the rest of the isolate protocol.  The wire shapes are
 * pinned here on purpose: if they drift, this test fails loudly rather than
 * silently probing the wrong offsets. */
#define CMD_MMAP        4
#define CMD_MUNMAP      5
#define CMD_WINDOW_INFO 16
#define RESP_OK         0x10
#define RESP_ERROR      0x11
#define RESP_MMAP       0x13
#define RESP_WINDOW     0x1b

struct cmd_munmap { uint32_t type, reserved; uint64_t win_va, length; };
struct cmd_mmap   { uint32_t type, handle_id; uint64_t win_va, length, offset;
		    uint32_t prot, map_flags; };
struct cmd_wi     { uint32_t type, reserved; };
struct resp_wi    { uint32_t type; int32_t retval; uint64_t base, size; };
struct resp_mmap  { uint32_t type; int32_t retval; uint64_t host_va; };

#define PAGE 0x1000ULL

static int sock, pass, total;

static void chk(const char *name, int cond, const char *detail)
{
	total++;
	if (cond) { pass++; printf("  ok   %-50s %s\n", name, detail); }
	else        printf("  FAIL %-50s %s\n", name, detail);
}

static int rt(const void *c, size_t n, void *r, size_t rn)
{
	if (send(sock, c, n, 0) < 0) return -1;
	return (int)recv(sock, r, rn, 0);
}

int main(int argc, char **argv)
{
	int sv[2];
	char detail[192];

	if (argc < 2) {
		fprintf(stderr, "usage: %s path/to/nvkvm_stub\n", argv[0]);
		return 2;
	}
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)) { perror("socketpair"); return 2; }

	pid_t p = fork();
	if (p == 0) {
		/* The stub's command socket is SOCK_FD == STDIN_FILENO. */
		dup2(sv[1], 0);
		close(sv[0]);
		execl(argv[1], argv[1], "--no-seccomp", (char *)NULL);
		_exit(127);
	}
	close(sv[1]);
	sock = sv[0];
	usleep(200000);

	printf("U-9  the window gate, at the call sites, in the real stub\n\n");

	struct cmd_wi wi = { .type = CMD_WINDOW_INFO };
	struct resp_wi rw;
	memset(&rw, 0, sizeof rw);
	int n = rt(&wi, sizeof wi, &rw, sizeof rw);
	snprintf(detail, sizeof detail, "base=0x%llx size=0x%llx (%llu GiB)",
		 (unsigned long long)rw.base, (unsigned long long)rw.size,
		 (unsigned long long)(rw.size >> 30));
	chk("WINDOW_INFO reports a reservation",
	    n == (int)sizeof rw && rw.type == RESP_WINDOW && rw.retval == 0 &&
	    rw.base && rw.size, detail);
	if (!rw.base || !rw.size) {
		printf("\nno window — nothing further can be measured\n");
		kill(p, SIGKILL); waitpid(p, NULL, 0);
		return 1;
	}

	/* ── MUNMAP.  Every address is derived from the reported base; see
	 * note (1) in the header for why a constant would be wrong. ─────── */
	struct { uint64_t addr; const char *name; int want_ok; } cases[] = {
		{ rw.base,                      "in:  the first page",           1 },
		{ rw.base + rw.size - PAGE,     "in:  the last page",            1 },
		{ rw.base + (rw.size / 2),      "in:  the middle",               1 },
		{ rw.base - PAGE,               "out: one page below the base",  0 },
		{ rw.base + rw.size,            "out: exactly at the end",       0 },
		{ rw.base + rw.size + PAGE,     "out: one page past the end",    0 },
		{ 0x401000ULL,                  "out: the stub's own text",      0 },
		{ 0ULL,                         "out: address zero",             0 },
		{ ~0ULL - PAGE,                 "out: an address that wraps",    0 },
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		struct { uint32_t type, err; } gr;
		struct cmd_munmap mu = { .type = CMD_MUNMAP,
					 .win_va = cases[i].addr,
					 .length = PAGE };
		memset(&gr, 0, sizeof gr);
		n = rt(&mu, sizeof mu, &gr, sizeof gr);
		int got_ok   = (n >= 4 && gr.type == RESP_OK);
		int got_deny = (n >= 4 && gr.type == RESP_ERROR);
		snprintf(detail, sizeof detail, "0x%llx -> %s",
			 (unsigned long long)cases[i].addr,
			 gr.type == RESP_OK    ? "RESP_OK" :
			 gr.type == RESP_ERROR ? "RESP_ERROR (DENY)" : "(no reply)");
		/*
		 * A refusal must be an explicit RESP_ERROR, NOT merely "no
		 * RESP_OK".  Measured against a deliberately gate-removed
		 * build: the 0x401000 probe made the stub unmap its own text
		 * and die, after which every later probe returned nothing --
		 * and an assertion written as `!got_ok` scored all three as
		 * refusals.  A dead isolate is the bug, not the control.
		 */
		chk(cases[i].name,
		    cases[i].want_ok ? got_ok : got_deny, detail);
	}

	/* ── MMAP.  handle_id 0 resolves to nothing, so the two refusals are
	 * distinguishable by errno and that is the whole point: -EFAULT means
	 * the window check ran and refused the address; -EBADF means it did
	 * not run and the handle lookup answered first.  See note (2). ──── */
	{
		struct cmd_mmap mm = { .type = CMD_MMAP, .handle_id = 0,
				       .win_va = rw.base + rw.size + PAGE,
				       .length = PAGE, .prot = 3, .map_flags = 1 };
		struct resp_mmap rm;
		memset(&rm, 0, sizeof rm);
		n = rt(&mm, sizeof mm, &rm, sizeof rm);
		snprintf(detail, sizeof detail, "retval=%d (want -14 EFAULT)",
			 rm.retval);
		chk("out-of-window MMAP is refused BY THE WINDOW",
		    n >= 8 && rm.type == RESP_MMAP && rm.retval == -14, detail);
	}
	{
		/* The control for the case above: an in-window address must get
		 * PAST the window check and fail on the handle instead.  Without
		 * this, a gate that refused everything would look identical. */
		struct cmd_mmap mm = { .type = CMD_MMAP, .handle_id = 0,
				       .win_va = rw.base, .length = PAGE,
				       .prot = 3, .map_flags = 1 };
		struct resp_mmap rm;
		memset(&rm, 0, sizeof rm);
		n = rt(&mm, sizeof mm, &rm, sizeof rm);
		snprintf(detail, sizeof detail, "retval=%d (want -9 EBADF)",
			 rm.retval);
		chk("in-window MMAP gets past the window to the handle",
		    n >= 8 && rm.type == RESP_MMAP && rm.retval == -9, detail);
	}

	printf("\n%d/%d probes behaved as required\n", pass, total);
	kill(p, SIGKILL);
	waitpid(p, NULL, 0);
	return pass == total ? 0 : 1;
}
