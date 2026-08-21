/*
 * test_nvkms_allowlist.c — the NVKMS gate, against the vendor enum at every
 *                          driver branch nvkvm claims to support.
 *
 * `enum NvKmsIoctlCommand` is a plain unvalued enum, so position IS the wire
 * value, and NVIDIA edits it in the MIDDLE rather than only appending.  The
 * numbers below were extracted from
 * src/nvidia-modeset/interface/nvkms-api.h at each vendor tag; they are the
 * ground truth this gate has to survive, and the reason a bare integer
 * allowlist was wrong:
 *
 *   tag           REGISTER UNREGISTER GRANT  ENABLE DISABLE ACCEL  FLIPLOCK
 *   515.105.01       16        17       18     -      -       -       -
 *   525.105.17       16        17       18     -      -       -       -
 *   535.104.05       16        17       18     -      -       -       -
 *   545.23.06        16        17       18     -      -       -      59
 *   550.54.14        16        17       18    60     61      62      59
 *   565.57.01        16        17       18    60     61      62      59
 *   570.86.16        16        17       18    60     61      62      59
 *   575.51.03        17        18       19    61     62      63      60
 *   580.178.04       17        18       19    61     62      63      60
 *   590.48.01        17        18       19    60     61      62      59
 *   595.84           17        18       19    60     61      62      59
 *   610.43.02        17        18       19    60     61      62      59
 *
 * The two commands that MUST never be admitted on any branch:
 *   GRANT_SURFACE      — a cross-client surface-sharing verb; the gate's own
 *                        header names it as exactly what it exists to deny.
 *   SET_FLIPLOCK_GROUP — a host-global display operation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvkvm_nvkms_allowlist.h"

static int tests_run, tests_failed;

#define CHECK(cond, fmt, ...)                                                 \
	do {                                                                  \
		tests_run++;                                                  \
		if (!(cond)) {                                                \
			tests_failed++;                                       \
			printf("  FAIL %s:%d: " fmt "\n",                     \
			       __FILE__, __LINE__, ##__VA_ARGS__);            \
		}                                                             \
	} while (0)

struct row {
	const char *tag;
	unsigned    major;
	int         reg, unreg, grant;
	int         en, dis, accel;   /* -1 = absent on this branch */
	int         fliplock;         /* -1 = absent */
};

static const struct row ROWS[] = {
	{ "515.105.01", 515, 16, 17, 18, -1, -1, -1, -1 },
	{ "525.105.17", 525, 16, 17, 18, -1, -1, -1, -1 },
	{ "535.104.05", 535, 16, 17, 18, -1, -1, -1, -1 },
	{ "545.23.06",  545, 16, 17, 18, -1, -1, -1, 59 },
	{ "550.54.14",  550, 16, 17, 18, 60, 61, 62, 59 },
	{ "565.57.01",  565, 16, 17, 18, 60, 61, 62, 59 },
	{ "570.86.16",  570, 16, 17, 18, 60, 61, 62, 59 },
	{ "575.51.03",  575, 17, 18, 19, 61, 62, 63, 60 },
	{ "580.178.04", 580, 17, 18, 19, 61, 62, 63, 60 },
	{ "590.48.01",  590, 17, 18, 19, 60, 61, 62, 59 },
	{ "595.84",     595, 17, 18, 19, 60, 61, 62, 59 },
	{ "610.43.02",  610, 17, 18, 19, 60, 61, 62, 59 },
};

int main(void)
{
	/* The escape hatch must not be inherited from the caller's env. */
	unsetenv("NVKVM_NVKMS_EXTRA_ALLOW");

	for (unsigned i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
		const struct row *r = &ROWS[i];
		unsigned m = r->major;

		/* Device alloc/free: index 0/1 on every tag measured. */
		CHECK(nvkvm_nvkms_cmd_allowed_major(0, m),
		      "%s: ALLOC_DEVICE (0) denied", r->tag);
		CHECK(nvkvm_nvkms_cmd_allowed_major(1, m),
		      "%s: FREE_DEVICE (1) denied", r->tag);

		/* The surface ops the gate exists to permit. */
		CHECK(nvkvm_nvkms_cmd_allowed_major((uint32_t)r->reg, m),
		      "%s: REGISTER_SURFACE (%d) denied", r->tag, r->reg);
		CHECK(nvkvm_nvkms_cmd_allowed_major((uint32_t)r->unreg, m),
		      "%s: UNREGISTER_SURFACE (%d) denied", r->tag, r->unreg);

		/* The two that must never get through, on any branch. */
		CHECK(!nvkvm_nvkms_cmd_allowed_major((uint32_t)r->grant, m),
		      "%s: GRANT_SURFACE (%d) ADMITTED", r->tag, r->grant);
		if (r->fliplock >= 0)
			CHECK(!nvkvm_nvkms_cmd_allowed_major(
				      (uint32_t)r->fliplock, m),
			      "%s: SET_FLIPLOCK_GROUP (%d) ADMITTED",
			      r->tag, r->fliplock);

		/* vblank-sem trio where the branch has it. */
		if (r->en >= 0) {
			CHECK(nvkvm_nvkms_cmd_allowed_major((uint32_t)r->en, m),
			      "%s: ENABLE_VBLANK_SEM (%d) denied", r->tag, r->en);
			CHECK(nvkvm_nvkms_cmd_allowed_major((uint32_t)r->dis, m),
			      "%s: DISABLE_VBLANK_SEM (%d) denied", r->tag, r->dis);
			CHECK(nvkvm_nvkms_cmd_allowed_major((uint32_t)r->accel, m),
			      "%s: ACCEL_VBLANK_SEM (%d) denied", r->tag, r->accel);
		} else {
			/* Out of range for this branch: 60..63 name nothing,
			 * so nothing there may be admitted. */
			for (uint32_t c = 60; c <= 63; c++)
				CHECK(!nvkvm_nvkms_cmd_allowed_major(c, m),
				      "%s: %u admitted but branch has no "
				      "vblank-sem ops", r->tag, c);
		}
	}

	/* Regression guard, stated as its own case: the OLD hardcoded list was
	 * {0,1,17,18,60,61,62}.  On 570 that admitted GRANT_SURFACE(18); on 580
	 * it admitted SET_FLIPLOCK_GROUP(60).  Both must now be refused. */
	CHECK(!nvkvm_nvkms_cmd_allowed_major(18, 570),
	      "570: GRANT_SURFACE(18) still admitted");
	CHECK(!nvkvm_nvkms_cmd_allowed_major(60, 580),
	      "580: SET_FLIPLOCK_GROUP(60) still admitted");
	CHECK(!nvkvm_nvkms_cmd_allowed_major(60, 575),
	      "575: SET_FLIPLOCK_GROUP(60) still admitted");

	/* An unknown or unparseable branch admits device alloc/free and nothing
	 * else -- extrapolating the numbering is what let FLIPLOCK in. */
	unsigned unknown[] = { 0, 470, 514, 611, 700 };
	for (unsigned i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
		unsigned m = unknown[i];
		CHECK(nvkvm_nvkms_cmd_allowed_major(0, m),
		      "unknown %u: ALLOC_DEVICE denied", m);
		CHECK(nvkvm_nvkms_cmd_allowed_major(1, m),
		      "unknown %u: FREE_DEVICE denied", m);
		for (uint32_t c = 2; c <= 70; c++)
			CHECK(!nvkvm_nvkms_cmd_allowed_major(c, m),
			      "unknown %u: cmdType %u admitted", m, c);
	}

	/* The escape hatch still works, and only for what it names. */
	setenv("NVKVM_NVKMS_EXTRA_ALLOW", "33,34", 1);
	CHECK(nvkvm_nvkms_cmd_allowed_major(33, 575), "extra-allow 33 refused");
	CHECK(nvkvm_nvkms_cmd_allowed_major(34, 575), "extra-allow 34 refused");
	CHECK(!nvkvm_nvkms_cmd_allowed_major(35, 575), "extra-allow leaked 35");
	unsetenv("NVKVM_NVKMS_EXTRA_ALLOW");

	printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
	return tests_failed ? 1 : 0;
}
