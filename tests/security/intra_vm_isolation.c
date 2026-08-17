/*
 * intra_vm_isolation.c — #65: intra-VM process isolation (access rights).
 *
 * Runs ENTIRELY inside one guest VM.  Forks two processes; each opens its own
 * /dev/nvidiactl (so each is a distinct guest session → isolate → host RM
 * client) and allocates its own root client + device.  The child then tries to
 * RM_DUP_OBJECT the PARENT's device purely by naming the parent's
 * (hClient, hObject).  The host nvidia driver must refuse — a process cannot
 * reach another process's RM objects without explicit sharing — proving that
 * nvkvm's access model isolates co-tenant processes WITHIN a VM, not just
 * across VMs (the cross-VM case is tests/security/poc_cross_proc_dup.c).
 *
 * This is the intra-VM complement: QEMU deliberately does NOT police intra-VM
 * access (SECURITY_MODEL.md §1) — isolation here is enforced by the real
 * driver's per-client reach-gate, which nvkvm preserves by giving each guest
 * process its own host client.
 *
 * Exit 0 = isolation holds (dup denied).  Exit 1 = HOLE (dup succeeded).
 * Exit 2 = inconclusive (setup error).
 *
 * Build (in guest):  gcc -O0 -o /tmp/iso intra_vm_isolation.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

struct nvos21 {            /* NV_ESC_RM_ALLOC, 32B */
	uint32_t h_root, h_object_parent, h_object_new, h_class;
	uint64_t p_alloc_parms;
	uint32_t status, _pad;
};
struct nvos55 {            /* NV_ESC_RM_DUP_OBJECT, 28B (575 SDK) */
	uint32_t h_client, h_parent, h_object, h_client_src, h_src_object, flags, status;
};
struct nv0080 {            /* NV0080_ALLOC_PARAMETERS — NV01_DEVICE_0 */
	uint32_t device_id, h_client_share, h_target_client, h_target_device, flags, _pad0;
	uint64_t va_space_size, va_start_internal, va_limit_internal;
	uint32_t va_mode, _pad1;
};

#define NV_IOWR(nr, sz) (0xc0000000u | ((unsigned)(sz) << 16) | (0x46u << 8) | (nr))
#define ESC_RM_ALLOC       0x2b
#define ESC_RM_DUP_OBJECT  0x34
#define NV01_ROOT          0x0
#define NV01_DEVICE_0      0x80

/* Allocate root client + device with the given handle bases on a fresh fd.
 * Returns fd (>=0) and fills *client/*device, or -1. */
static int alloc_client_device(uint32_t client, uint32_t device,
			       uint32_t *client_out, uint32_t *device_out)
{
	int fd = open("/dev/nvidiactl", O_RDWR);
	if (fd < 0) { perror("open /dev/nvidiactl"); return -1; }

	struct nvos21 a = { .h_object_new = client, .h_class = NV01_ROOT };
	if (ioctl(fd, NV_IOWR(ESC_RM_ALLOC, sizeof a), &a) < 0 || a.status != 0) {
		fprintf(stderr, "client alloc failed errno=%d status=0x%x\n",
			errno, a.status);
		close(fd); return -1;
	}
	client = a.h_object_new ? a.h_object_new : client;

	struct nv0080 dp = { .device_id = 0 };
	struct nvos21 ad = {
		.h_root = client, .h_object_parent = client,
		.h_object_new = device, .h_class = NV01_DEVICE_0,
		.p_alloc_parms = (uint64_t)(uintptr_t)&dp,
	};
	if (ioctl(fd, NV_IOWR(ESC_RM_ALLOC, sizeof ad), &ad) < 0 || ad.status != 0) {
		fprintf(stderr, "device alloc failed errno=%d status=0x%x\n",
			errno, ad.status);
		close(fd); return -1;
	}
	device = ad.h_object_new ? ad.h_object_new : device;
	*client_out = client; *device_out = device;
	return fd;
}

int main(void)
{
	int to_child[2], to_parent[2];
	if (pipe(to_child) || pipe(to_parent)) { perror("pipe"); return 2; }

	pid_t pid = fork();
	if (pid < 0) { perror("fork"); return 2; }

	if (pid == 0) {
		/* ── CHILD: distinct process → distinct host RM client ── */
		close(to_child[1]); close(to_parent[0]);
		uint32_t my_c = 0, my_d = 0;
		int fd = alloc_client_device(0xbbbb0001, 0xbbbb0d01, &my_c, &my_d);
		if (fd < 0) { char e = 'E'; if (write(to_parent[1], &e, 1)) {} _exit(2); }

		uint32_t victim[2] = {0, 0};
		if (read(to_child[0], victim, sizeof victim) != (ssize_t)sizeof victim)
			_exit(2);
		fprintf(stderr, "child: my client=0x%x; victim=(0x%x,0x%x)\n",
			my_c, victim[0], victim[1]);

		struct nvos55 d = {
			.h_client = my_c, .h_parent = my_d,
			.h_object = (my_c & 0xffff0000u) | 0x0abc,
			.h_client_src = victim[0], .h_src_object = victim[1],
		};
		int r = ioctl(fd, NV_IOWR(ESC_RM_DUP_OBJECT, sizeof d), &d);
		fprintf(stderr, "child: DUP victim=(0x%x,0x%x) -> ioctl=%d errno=%d status=0x%x\n",
			victim[0], victim[1], r, errno, d.status);

		int verdict;
		if (r == 0 && d.status == 0) {
			printf("HOLE — child duped the parent's object (intra-VM isolation BROKEN)\n");
			verdict = 1;
		} else if (d.status == 0x1e) {
			printf("INCONCLUSIVE — dup rejected at parent validation (0x1e) before access check\n");
			verdict = 2;
		} else {
			printf("ISOLATED — driver refused cross-process dup (status=0x%x); "
			       "intra-VM process isolation holds\n", d.status);
			verdict = 0;
		}
		char vc = (char)verdict; if (write(to_parent[1], &vc, 1)) {}
		_exit(verdict);
	}

	/* ── PARENT (victim): allocate, publish handles, hold until child done ── */
	close(to_child[0]); close(to_parent[1]);
	uint32_t my_c = 0, my_d = 0;
	int fd = alloc_client_device(0xaaaa0001, 0xaaaa0d01, &my_c, &my_d);
	if (fd < 0) return 2;
	fprintf(stderr, "parent: victim client=0x%x device=0x%x\n", my_c, my_d);
	uint32_t pub[2] = { my_c, my_d };
	if (write(to_child[1], pub, sizeof pub) != (ssize_t)sizeof pub) return 2;

	char vc = 2;
	if (read(to_parent[0], &vc, 1) != 1) vc = 2;  /* child verdict */
	int st; waitpid(pid, &st, 0);
	(void)fd;
	return (vc == 'E') ? 2 : (int)vc;
}
