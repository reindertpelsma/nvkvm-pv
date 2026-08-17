/*
 * mock_stub.c — minimal nvkvm_stub lookalike for unit testing.
 *
 * Runs on stdin (a SOCK_SEQPACKET fd, as the real stub does).
 * - IOCTL: echoes txn_id in retval; optional per-command delay via
 *   the low byte of handle_id (delay_ms = handle_id & 0xff).
 * - RECEIVE_FD, CLOSE_FD, MUNMAP, POLL, UNPOLL: sends RESP_OK.
 * - MMAP: sends RESP_MMAP with retval=0.
 * - EXIT: exits 0.
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

/* Include proto directly */
#include "../../src/common/nvkvm_isolate_proto.h"

static void sleep_ms(unsigned int ms)
{
	if (!ms)
		return;
	struct timespec ts = {
		.tv_sec  = ms / 1000,
		.tv_nsec = (long)(ms % 1000) * 1000000L,
	};
	nanosleep(&ts, NULL);
}

static void send_ok(int sock)
{
	struct isolate_resp_ok resp = { .type = ISOLATE_RESP_OK };
	send(sock, &resp, sizeof(resp), 0);
}

int main(void)
{
	int sock = STDIN_FILENO;
	union {
		uint8_t                   raw[65536];
		uint32_t                  type;
		struct isolate_cmd_ioctl  ioctl;
		struct isolate_cmd_mmap   mmap;
		struct isolate_cmd_poll   poll;
		struct isolate_cmd_unpoll unpoll;
	} u;

	for (;;) {
		char cbuf[CMSG_SPACE(sizeof(int))];
		struct iovec iov = { .iov_base = &u, .iov_len = sizeof(u) };
		struct msghdr msg = {
			.msg_iov        = &iov,
			.msg_iovlen     = 1,
			.msg_control    = cbuf,
			.msg_controllen = sizeof(cbuf),
		};

		ssize_t n = recvmsg(sock, &msg, 0);
		if (n <= 0)
			break;

		char drain[65536];

		switch (u.type) {
		case ISOLATE_CMD_RECEIVE_FD:
		case ISOLATE_CMD_CLOSE_FD:
		case ISOLATE_CMD_MUNMAP:
		case ISOLATE_CMD_POLL:
		case ISOLATE_CMD_UNPOLL:
			send_ok(sock);
			break;

		case ISOLATE_CMD_MMAP: {
			struct isolate_resp_mmap resp = {
				.type   = ISOLATE_RESP_MMAP,
				.retval = 0,
			};
			send(sock, &resp, sizeof(resp), 0);
			break;
		}

		case ISOLATE_CMD_IOCTL: {
			uint32_t txn_id    = u.ioctl.txn_id;
			uint32_t param_sz  = u.ioctl.param_size;
			uint32_t aux_sz    = u.ioctl.aux_size;
			unsigned delay_ms  = u.ioctl.handle_id & 0xff;

			/* Drain param and aux blobs (separate SEQPACKET messages) */
			if (param_sz > 0)
				recv(sock, drain,
				     param_sz < sizeof(drain) ? param_sz : sizeof(drain), 0);
			if (aux_sz > 0)
				recv(sock, drain,
				     aux_sz < sizeof(drain) ? aux_sz : sizeof(drain), 0);

			sleep_ms(delay_ms);

			struct isolate_resp_ioctl resp = {
				.type       = ISOLATE_RESP_IOCTL,
				.txn_id     = txn_id,
				.param_size = 0,
				.aux_size   = 0,
				.retval     = 0,
				.nvstatus   = 0,
				.fault_addr = 0,
			};
			send(sock, &resp, sizeof(resp), 0);
			break;
		}

		case ISOLATE_CMD_EXIT:
			_exit(0);

		default:
			break;
		}
	}
	return 0;
}
