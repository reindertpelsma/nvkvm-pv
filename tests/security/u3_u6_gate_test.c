/*
 * u3_u6_gate_test.c — adversarial probe for the U-3 and U-6 host-side gates.
 *
 * Runs INSIDE the guest.  It does what a malicious guest userspace would do
 * if the guest kernel module's sanitiser were skipped: name a UVM VA range
 * nobody established, and select an NVOS32 function that hands a guest VA to
 * pin_user_pages().  Both must be refused by QEMU.
 *
 * This is a boundary test, not a functional one: every call here is EXPECTED
 * to fail.  A success is the bug.  Check QEMU's stdout for the matching
 * "nvkvm: DENY ..." lines.
 *
 *   gcc -O0 -o /tmp/u3_u6_gate_test u3_u6_gate_test.c && /tmp/u3_u6_gate_test
 *
 * Exit: 0 = every probe was refused, 1 = at least one was accepted.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define UVM_INITIALIZE        0x30000001u
#define UVM_VALIDATE_VA_RANGE 72u
#define UVM_MIGRATE           51u

/* NV_ESC_RM_VID_HEAP_CONTROL — _IOWR('F', 0x4a, sizeof(NVOS32_PARAMETERS)) */
#define NVOS32_SIZE 184u
#define NV_ESC_RM_VID_HEAP_CONTROL _IOWR('F', 0x4a, char[NVOS32_SIZE])

static int failures;

static void expect_refused(const char *what, int rc, int err)
{
	if (rc == 0) {
		printf("  FAIL %-34s ACCEPTED (rc=0)\n", what);
		failures++;
	} else {
		printf("  ok   %-34s refused (rc=%d errno=%d %s)\n",
		       what, rc, err, strerror(err));
	}
}

int main(void)
{
	/* ── U-6: UVM VA ranges nobody owns ───────────────────────────── */
	int uvm = open("/dev/nvidia-uvm", O_RDWR);
	if (uvm < 0) {
		printf("SKIP: /dev/nvidia-uvm: %s\n", strerror(errno));
	} else {
		struct { uint64_t flags; uint32_t rm_status, rsvd; } init = { 0, 0, 0 };
		ioctl(uvm, UVM_INITIALIZE, &init);

		struct { uint64_t base, length; uint32_t rm_status, rsvd; } va;

		/* An address nvkvm never established for this UVM fd. */
		memset(&va, 0, sizeof(va));
		va.base = 0x7f0000000000ULL;   /* squarely inside where QEMU's
						* own libraries and heap live */
		va.length = 0x1000;
		expect_refused("U-6 VALIDATE_VA_RANGE(unowned)",
			       ioctl(uvm, UVM_VALIDATE_VA_RANGE, &va), errno);

		/* Same, via the command that would otherwise reach UVM's
		 * pageable-migration path over QEMU's own anonymous pages. */
		struct { uint64_t base, length; uint8_t uuid[16];
			 uint32_t flags, fd, rm_status, rsvd; } mig;
		memset(&mig, 0, sizeof(mig));
		mig.base   = 0x7f0000000000ULL;
		mig.length = 0x1000;
		expect_refused("U-6 MIGRATE(unowned)",
			       ioctl(uvm, UVM_MIGRATE, &mig), errno);

		/* Unaligned / wrapping ranges must not be accepted either. */
		memset(&va, 0, sizeof(va));
		va.base = 0x7f0000000123ULL;
		va.length = 0x1000;
		expect_refused("U-6 VALIDATE_VA_RANGE(unaligned)",
			       ioctl(uvm, UVM_VALIDATE_VA_RANGE, &va), errno);

		memset(&va, 0, sizeof(va));
		va.base = 0xfffffffffffff000ULL;
		va.length = 0x8000;
		expect_refused("U-6 VALIDATE_VA_RANGE(wrapping)",
			       ioctl(uvm, UVM_VALIDATE_VA_RANGE, &va), errno);

		close(uvm);
	}

	/* ── U-3: NVOS32 function gate ────────────────────────────────── */
	int ctl = open("/dev/nvidiactl", O_RDWR);
	if (ctl < 0) {
		printf("SKIP: /dev/nvidiactl: %s\n", strerror(errno));
	} else {
		unsigned char p[NVOS32_SIZE];
		uint32_t fn;

		/* function 27 = NVOS32_FUNCTION_ALLOC_OS_DESCRIPTOR.  The
		 * driver would hand data.AllocOsDesc.descriptor straight to
		 * os_lock_user_pages() == pin_user_pages(). */
		memset(p, 0, sizeof(p));
		fn = 27; memcpy(p + 8, &fn, 4);
		*(uint64_t *)(p + 40 + 20) = 0x7f0000000000ULL; /* descriptor */
		expect_refused("U-3 VID_HEAP_CONTROL(fn=27)",
			       ioctl(ctl, NV_ESC_RM_VID_HEAP_CONTROL, p), errno);

		/* function 19 = HW_ALLOC — carries bindResultFunc / pHandle. */
		memset(p, 0, sizeof(p));
		fn = 19; memcpy(p + 8, &fn, 4);
		expect_refused("U-3 VID_HEAP_CONTROL(fn=19)",
			       ioctl(ctl, NV_ESC_RM_VID_HEAP_CONTROL, p), errno);

		/* function 3 = FREE — not needed by the working stack. */
		memset(p, 0, sizeof(p));
		fn = 3; memcpy(p + 8, &fn, 4);
		expect_refused("U-3 VID_HEAP_CONTROL(fn=3)",
			       ioctl(ctl, NV_ESC_RM_VID_HEAP_CONTROL, p), errno);

		close(ctl);
	}

	printf(failures ? "GATE_TEST FAIL (%d accepted)\n"
			: "GATE_TEST PASS (%d accepted)\n", failures);
	return failures ? 1 : 0;
}
