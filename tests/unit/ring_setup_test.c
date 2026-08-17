/* ring_setup_test.c — Phase 2 of the command buffer (docs/design/command_buffer.md).
 *
 * Exercises the EXACT QEMU↔isolate SETUP_RING handshake end-to-end on the host,
 * with no GPU and no QEMU/VM:
 *
 *   parent ("QEMU")   : memfd_create → ftruncate → mmap(MAP_SHARED) → init both
 *                       ring control blocks → write probe → socketpair → fork →
 *                       sendmsg(SETUP_RING + memfd via SCM_RIGHTS) → recv
 *                       RING_READY → verify QEMU→child (echo) AND child→QEMU
 *                       (resp_data) shared-memory visibility.
 *   child ("isolate") : recvmsg(SETUP_RING + fd) → validate geometry → mmap the
 *                       SAME memfd → read probe, echo it, write masked reply →
 *                       send RING_READY.
 *
 * This is the risky part of Phase 2 — cross-process MAP_SHARED visibility over
 * a fd passed by SCM_RIGHTS — proven independently of the NVIDIA driver.  The
 * full path (real stub binary under seccomp + guest KVM memslot) is validated
 * on the GPU host.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "nvkvm_ring.h"
#include "../../src/common/nvkvm_isolate_proto.h"

/* The child validates geometry exactly like the real stub handler. */
static int geometry_ok(const struct isolate_cmd_setup_ring *c)
{
	return nvkvm_ring_size_ok(c->ring_bytes) &&
	       c->req_off == 0 &&
	       c->resp_off == (uint32_t)nvkvm_ring_resp_off(c->ring_bytes) &&
	       c->region_size >= (uint32_t)nvkvm_ring_region_size(c->ring_bytes) &&
	       c->region_size <= (16u << 20);
}

/* ── child: the "isolate" ────────────────────────────────────────────────── */
static int child_main(int sock)
{
	union {
		uint32_t type;
		struct isolate_cmd_setup_ring setup;
	} cmd;
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { &cmd, sizeof(cmd) };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = cbuf, .msg_controllen = sizeof(cbuf),
	};
	ssize_t n = recvmsg(sock, &msg, 0);
	if (n < (ssize_t)sizeof(cmd.setup) || cmd.type != ISOLATE_CMD_SETUP_RING)
		return 1;

	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	if (!cm || cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS ||
	    cm->cmsg_len != CMSG_LEN(sizeof(int)))
		return 2;
	int fd;
	memcpy(&fd, CMSG_DATA(cm), sizeof(int));

	if (!geometry_ok(&cmd.setup))
		return 3;

	void *base = mmap(NULL, cmd.setup.region_size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd, 0);
	close(fd);
	if (base == MAP_FAILED)
		return 4;

	struct nvkvm_ring *req  = (struct nvkvm_ring *)base;
	struct nvkvm_ring *resp =
		(struct nvkvm_ring *)((uint8_t *)base + cmd.setup.resp_off);
	/* control blocks initialised by the parent: rings must read as empty */
	if (req->size != cmd.setup.ring_bytes || resp->size != cmd.setup.ring_bytes ||
	    req->head != req->tail || resp->head != resp->tail)
		return 5;

	uint8_t *req_data  = (uint8_t *)req  + sizeof(struct nvkvm_ring);
	uint8_t *resp_data = (uint8_t *)resp + sizeof(struct nvkvm_ring);
	uint64_t v;
	memcpy(&v, req_data, sizeof(v));
	uint64_t reply = v ^ NVKVM_RING_PROBE_MASK;
	memcpy(resp_data, &reply, sizeof(reply));

	struct isolate_resp_ring_ready rr = {
		.type = ISOLATE_RESP_RING_READY, .error = 0, .probe_seen = v,
	};
	if (send(sock, &rr, sizeof(rr), 0) != (ssize_t)sizeof(rr))
		return 6;
	return 0;
}

/* ── parent: the "QEMU" side ─────────────────────────────────────────────── */
int main(void)
{
	uint32_t ring_bytes = NVKVM_RING_DEFAULT_BYTES;
	uint64_t region = nvkvm_ring_region_size(ring_bytes);
	region = (region + 4095) & ~4095ULL;

	int mfd = memfd_create("nvkvm-ring", MFD_CLOEXEC);
	if (mfd < 0 || ftruncate(mfd, (off_t)region) < 0) {
		perror("memfd"); return 1;
	}
	void *qva = mmap(NULL, region, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
	if (qva == MAP_FAILED) { perror("mmap"); return 1; }

	uint64_t resp_off = nvkvm_ring_resp_off(ring_bytes);
	struct nvkvm_ring *req  = (struct nvkvm_ring *)qva;
	struct nvkvm_ring *resp = (struct nvkvm_ring *)((uint8_t *)qva + resp_off);
	memset(req, 0, sizeof(*req));   req->size  = ring_bytes;
	memset(resp, 0, sizeof(*resp)); resp->size = ring_bytes;

	uint64_t probe = 0x6e766b766d000000ULL | 0x1234u;
	uint8_t *req_data  = (uint8_t *)req  + sizeof(struct nvkvm_ring);
	uint8_t *resp_data = (uint8_t *)resp + sizeof(struct nvkvm_ring);
	memcpy(req_data, &probe, sizeof(probe));
	memset(resp_data, 0, sizeof(uint64_t));

	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) { perror("sp"); return 1; }

	pid_t pid = fork();
	if (pid == 0) {
		close(sv[0]);
		_exit(child_main(sv[1]));
	}
	close(sv[1]);

	struct isolate_cmd_setup_ring hdr = {
		.type = ISOLATE_CMD_SETUP_RING,
		.region_size = (uint32_t)region,
		.req_off = 0,
		.resp_off = (uint32_t)resp_off,
		.ring_bytes = ring_bytes,
	};
	struct iovec iov = { &hdr, sizeof(hdr) };
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = cbuf, .msg_controllen = sizeof(cbuf),
	};
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &mfd, sizeof(int));
	if (sendmsg(sv[0], &msg, 0) < 0) { perror("sendmsg"); return 1; }

	struct isolate_resp_ring_ready rr;
	ssize_t n = recv(sv[0], &rr, sizeof(rr), 0);

	int status = 0;
	waitpid(pid, &status, 0);

	int fail = 0;
	if (n != (ssize_t)sizeof(rr) || rr.type != ISOLATE_RESP_RING_READY) {
		printf("setup   : FAIL — no/short RING_READY (n=%zd)\n", n); fail = 1;
	} else if (rr.error != 0) {
		printf("setup   : FAIL — child error %d\n", rr.error); fail = 1;
	} else if (rr.probe_seen != probe) {
		printf("setup   : FAIL — QEMU→child broken (echo=0x%llx want=0x%llx)\n",
		       (unsigned long long)rr.probe_seen, (unsigned long long)probe);
		fail = 1;
	} else {
		uint64_t back = 0;
		memcpy(&back, resp_data, sizeof(back));   /* child wrote via shared mem */
		if (back != (probe ^ NVKVM_RING_PROBE_MASK)) {
			printf("setup   : FAIL — child→QEMU broken (back=0x%llx want=0x%llx)\n",
			       (unsigned long long)back,
			       (unsigned long long)(probe ^ NVKVM_RING_PROBE_MASK));
			fail = 1;
		}
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("setup   : FAIL — child exit status %d\n", status); fail = 1;
	}
	if (!fail)
		printf("setup   : memfd+SCM_RIGHTS+MAP_SHARED probe round-trip OK "
		       "(region=%llu B, ring_bytes=%u)\n",
		       (unsigned long long)region, ring_bytes);

	munmap(qva, region);
	close(mfd);
	printf(fail ? "RING SETUP TEST: FAIL\n" : "RING SETUP TEST: PASS\n");
	return fail;
}
