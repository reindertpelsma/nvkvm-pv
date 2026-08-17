/*
 * test_open_scm.c — verify the OPEN_DEVICE / OPEN_DEVICE_RESP protocol
 * round-trips a file descriptor over SOCK_SEQPACKET via SCM_RIGHTS in a
 * single sendmsg, atomic with the response struct.
 *
 * The real stub is a static-no-libc binary that opens /dev/nvidia*; we
 * can't link or run it here. Instead a worker thread plays the stub
 * role, reading ISOLATE_CMD_OPEN_DEVICE and replying with a memfd via
 * SCM_RIGHTS. This validates the wire contract before integration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <fcntl.h>

#include "../../src/common/nvkvm_isolate_proto.h"

#ifndef SYS_memfd_create
#define SYS_memfd_create 319
#endif

static int memfd_create_local(const char *name, unsigned flags)
{
	return (int)syscall(SYS_memfd_create, name, flags);
}

/* ────────────────────────────────────────────────────────────────────────── */

struct stub_arg {
	int sock;
	int reply_fd_to_send;     /* fd the "stub" hands back via SCM_RIGHTS  */
	int reply_status;         /* 0 = success path; otherwise negative err */
	uint32_t expect_txn_id;
	uint32_t expect_handle_id;
	uint32_t expect_dev_id;
	int seen_cmd;             /* set by stub thread when cmd parsed       */
};

static void *stub_thread(void *p)
{
	struct stub_arg *a = p;
	union {
		uint32_t type;
		struct isolate_cmd_open_device open_dev;
	} cmd;
	char cmsg_buf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { &cmd, sizeof(cmd) };
	struct msghdr msg = {
		.msg_iov     = &iov,
		.msg_iovlen  = 1,
		.msg_control = cmsg_buf,
		.msg_controllen = sizeof(cmsg_buf),
	};

	ssize_t n = recvmsg(a->sock, &msg, 0);
	if (n < (ssize_t)sizeof(struct isolate_cmd_open_device)) {
		fprintf(stderr, "stub: recvmsg returned %zd\n", n);
		return NULL;
	}
	if (cmd.type != ISOLATE_CMD_OPEN_DEVICE) {
		fprintf(stderr, "stub: bad cmd type 0x%x\n", cmd.type);
		return NULL;
	}
	if (cmd.open_dev.txn_id    != a->expect_txn_id    ||
	    cmd.open_dev.handle_id != a->expect_handle_id ||
	    cmd.open_dev.dev_id    != a->expect_dev_id) {
		fprintf(stderr, "stub: field mismatch\n");
		return NULL;
	}
	a->seen_cmd = 1;

	/* Build the reply: response struct + SCM_RIGHTS(reply_fd) atomically. */
	struct isolate_resp_open_device resp = {
		.type   = ISOLATE_RESP_OPEN_DEVICE,
		.txn_id = cmd.open_dev.txn_id,
		.retval = a->reply_status,
	};
	struct iovec riov = { &resp, sizeof(resp) };
	char rcmsg[CMSG_SPACE(sizeof(int))];
	struct msghdr rmsg = {
		.msg_iov    = &riov,
		.msg_iovlen = 1,
	};
	if (a->reply_status == 0 && a->reply_fd_to_send >= 0) {
		rmsg.msg_control    = rcmsg;
		rmsg.msg_controllen = sizeof(rcmsg);
		struct cmsghdr *cm = CMSG_FIRSTHDR(&rmsg);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type  = SCM_RIGHTS;
		cm->cmsg_len   = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &a->reply_fd_to_send, sizeof(int));
		rmsg.msg_controllen = cm->cmsg_len;
	}
	ssize_t w = sendmsg(a->sock, &rmsg, 0);
	if (w < 0)
		fprintf(stderr, "stub: sendmsg err %d\n", errno);
	return NULL;
}

/* ────────────────────────────────────────────────────────────────────────── */

static int do_open_roundtrip(int sock, uint32_t txn, uint32_t hid,
			     uint32_t dev, int *fd_out, int *status_out)
{
	struct isolate_cmd_open_device cmd = {
		.type      = ISOLATE_CMD_OPEN_DEVICE,
		.handle_id = hid,
		.dev_id    = dev,
		.flags     = O_RDWR,
		.txn_id    = txn,
	};
	if (send(sock, &cmd, sizeof(cmd), 0) != (ssize_t)sizeof(cmd))
		return -1;

	struct isolate_resp_open_device resp = {0};
	struct iovec iov = { &resp, sizeof(resp) };
	char cmsg_buf[CMSG_SPACE(sizeof(int))];
	struct msghdr msg = {
		.msg_iov     = &iov,
		.msg_iovlen  = 1,
		.msg_control = cmsg_buf,
		.msg_controllen = sizeof(cmsg_buf),
	};
	ssize_t n = recvmsg(sock, &msg, 0);
	if (n != (ssize_t)sizeof(resp))
		return -1;
	if (resp.type != ISOLATE_RESP_OPEN_DEVICE) return -1;
	if (resp.txn_id != txn) return -1;
	*status_out = resp.retval;

	*fd_out = -1;
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	     cm; cm = CMSG_NXTHDR(&msg, cm)) {
		if (cm->cmsg_level == SOL_SOCKET &&
		    cm->cmsg_type  == SCM_RIGHTS &&
		    cm->cmsg_len   == CMSG_LEN(sizeof(int))) {
			memcpy(fd_out, CMSG_DATA(cm), sizeof(int));
		}
	}
	return 0;
}

static void test_success_path(void)
{
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);

	int memfd = memfd_create_local("scm_test", 0);
	assert(memfd >= 0);
	const char payload[] = "OPEN_DEVICE/SCM_RIGHTS smoke";
	assert(pwrite(memfd, payload, sizeof(payload), 0) == sizeof(payload));

	struct stub_arg arg = {
		.sock              = sv[1],
		.reply_fd_to_send  = memfd,
		.reply_status      = 0,
		.expect_txn_id     = 0xC0DE,
		.expect_handle_id  = 7,
		.expect_dev_id     = 0,   /* NVKVM_DEV_CTL */
	};
	pthread_t th;
	pthread_create(&th, NULL, stub_thread, &arg);

	int got_fd = -1, status = 0;
	int rc = do_open_roundtrip(sv[0], 0xC0DE, 7, 0, &got_fd, &status);
	pthread_join(th, NULL);

	assert(rc == 0);
	assert(arg.seen_cmd == 1);
	assert(status == 0);
	assert(got_fd >= 0);
	assert(got_fd != memfd);  /* must be a fresh fd in our table */

	char buf[64] = {0};
	ssize_t r = pread(got_fd, buf, sizeof(buf), 0);
	assert(r > 0);
	assert(strcmp(buf, payload) == 0);

	close(got_fd);
	close(memfd);
	close(sv[0]); close(sv[1]);
	puts("PASS test_success_path");
}

static void test_error_no_fd(void)
{
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);

	struct stub_arg arg = {
		.sock             = sv[1],
		.reply_fd_to_send = -1,
		.reply_status     = -ENOENT,
		.expect_txn_id    = 0xBEEF,
		.expect_handle_id = 1234,
		.expect_dev_id    = 99,   /* nonsense; stub still echoes */
	};
	pthread_t th;
	pthread_create(&th, NULL, stub_thread, &arg);

	int got_fd = 0xAAAAAAAA, status = 0;
	int rc = do_open_roundtrip(sv[0], 0xBEEF, 1234, 99, &got_fd, &status);
	pthread_join(th, NULL);

	assert(rc == 0);
	assert(status == -ENOENT);
	assert(got_fd == -1);  /* no SCM_RIGHTS attached on error */

	close(sv[0]); close(sv[1]);
	puts("PASS test_error_no_fd");
}

static void test_txn_id_echoed(void)
{
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);

	int memfd = memfd_create_local("scm_txn", 0);
	assert(memfd >= 0);

	struct stub_arg arg = {
		.sock             = sv[1],
		.reply_fd_to_send = memfd,
		.reply_status     = 0,
		.expect_txn_id    = 0xFFFFFFF0u,  /* near-wrap, exercises u32 */
		.expect_handle_id = 1,
		.expect_dev_id    = 16,           /* NVKVM_DEV_GPU(0) */
	};
	pthread_t th;
	pthread_create(&th, NULL, stub_thread, &arg);

	int got_fd = -1, status = 0;
	int rc = do_open_roundtrip(sv[0], 0xFFFFFFF0u, 1, 16, &got_fd, &status);
	pthread_join(th, NULL);

	assert(rc == 0 && status == 0 && got_fd >= 0);
	close(got_fd);
	close(memfd);
	close(sv[0]); close(sv[1]);
	puts("PASS test_txn_id_echoed");
}

int main(void)
{
	test_success_path();
	test_error_no_fd();
	test_txn_id_echoed();
	puts("ALL OPEN_SCM TESTS PASSED");
	return 0;
}
