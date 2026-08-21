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
 *   515.43.04        16        17       18     -      -       -       -
 *   515.105.01       16        17       18     -      -       -       -
 *   525.47.04        16        17       18     -      -       -       -
 *   525.147.05       16        17       18     -      -       -       -
 *   535.43.02        16        17       18     -      -       -       -
 *   535.309.01       16        17       18     -      -       -       -
 *   545.23.06        16        17       18     -      -       -      59
 *   545.29.06        16        17       18     -      -       -      59
 *   550.40.07        16        17       18    60     61      62      59
 *   550.163.01       16        17       18    60     61      62      59
 *   555.58.02        16        17       18    60     61      62      59
 *   560.35.03        16        17       18    60     61      62      59
 *   565.57.01        16        17       18    60     61      62      59
 *   565.77           16        17       18    60     61      62      59
 *   570.86.16        16        17       18    60     61      62      59
 *   570.195.03       16        17       18    60     61      62      59
 *   570.207          17        18       19    61     62      63      60   <-- SPLIT
 *   570.211.01       17        18       19    61     62      63      60
 *   575.51.02        17        18       19    61     62      63      60
 *   575.64.05        17        18       19    61     62      63      60
 *   580.65.06        17        18       19    61     62      63      60
 *   580.178.04       17        18       19    61     62      63      60
 *   590.44.01        17        18       19    60     61      62      59
 *   595.91.07        17        18       19    60     61      62      59
 *   610.43.02        17        18       19    60     61      62      59
 *   610.57.04        17        18       19    60     61      62      59
 *
 * The 570 branch renumbers MID-BRANCH, between 570.195.03 and 570.207.  A gate
 * keyed on the major alone is therefore still wrong -- it would admit
 * SET_FLIPLOCK_GROUP (60) on 570.207+ -- which is why the first version of this
 * fix was wrong and why these rows carry a minor.
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
	unsigned    major, minor;
	int         reg, unreg, grant;
	int         en, dis, accel;   /* -1 = absent on this branch */
	int         fliplock;         /* -1 = absent */
};

static const struct row ROWS[] = {
	{ "515.43.04",  515,  43, 16, 17, 18, -1, -1, -1, -1 },
	{ "515.105.01", 515, 105, 16, 17, 18, -1, -1, -1, -1 },
	{ "525.47.04",  525,  47, 16, 17, 18, -1, -1, -1, -1 },
	{ "525.147.05", 525, 147, 16, 17, 18, -1, -1, -1, -1 },
	{ "535.43.02",  535,  43, 16, 17, 18, -1, -1, -1, -1 },
	{ "535.309.01", 535, 309, 16, 17, 18, -1, -1, -1, -1 },
	{ "545.23.06",  545,  23, 16, 17, 18, -1, -1, -1, 59 },
	{ "545.29.06",  545,  29, 16, 17, 18, -1, -1, -1, 59 },
	{ "550.40.07",  550,  40, 16, 17, 18, 60, 61, 62, 59 },
	{ "550.163.01", 550, 163, 16, 17, 18, 60, 61, 62, 59 },
	{ "555.58.02",  555,  58, 16, 17, 18, 60, 61, 62, 59 },
	{ "560.35.03",  560,  35, 16, 17, 18, 60, 61, 62, 59 },
	{ "565.57.01",  565,  57, 16, 17, 18, 60, 61, 62, 59 },
	{ "565.77",     565,  77, 16, 17, 18, 60, 61, 62, 59 },
	{ "570.86.16",  570,  86, 16, 17, 18, 60, 61, 62, 59 },
	{ "570.195.03", 570, 195, 16, 17, 18, 60, 61, 62, 59 },
	{ "570.207",    570, 207, 17, 18, 19, 61, 62, 63, 60 },
	{ "570.211.01", 570, 211, 17, 18, 19, 61, 62, 63, 60 },
	{ "575.51.02",  575,  51, 17, 18, 19, 61, 62, 63, 60 },
	{ "575.64.05",  575,  64, 17, 18, 19, 61, 62, 63, 60 },
	{ "580.65.06",  580,  65, 17, 18, 19, 61, 62, 63, 60 },
	{ "580.178.04", 580, 178, 17, 18, 19, 61, 62, 63, 60 },
	{ "590.44.01",  590,  44, 17, 18, 19, 60, 61, 62, 59 },
	{ "590.48.01",  590,  48, 17, 18, 19, 60, 61, 62, 59 },
	{ "595.44.02",  595,  44, 17, 18, 19, 60, 61, 62, 59 },
	{ "595.91.07",  595,  91, 17, 18, 19, 60, 61, 62, 59 },
	{ "610.43.02",  610,  43, 17, 18, 19, 60, 61, 62, 59 },
	{ "610.57.04",  610,  57, 17, 18, 19, 60, 61, 62, 59 },
};

int main(void)
{
	/* The escape hatch must not be inherited from the caller's env. */
	unsetenv("NVKVM_NVKMS_EXTRA_ALLOW");

	for (unsigned i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
		const struct row *r = &ROWS[i];
		unsigned m = r->major, mi = r->minor;

		/* Device alloc/free: index 0/1 on every tag measured. */
		CHECK(nvkvm_nvkms_cmd_allowed_ver(0, m, mi),
		      "%s: ALLOC_DEVICE (0) denied", r->tag);
		CHECK(nvkvm_nvkms_cmd_allowed_ver(1, m, mi),
		      "%s: FREE_DEVICE (1) denied", r->tag);

		/* The surface ops the gate exists to permit. */
		CHECK(nvkvm_nvkms_cmd_allowed_ver((uint32_t)r->reg, m, mi),
		      "%s: REGISTER_SURFACE (%d) denied", r->tag, r->reg);
		CHECK(nvkvm_nvkms_cmd_allowed_ver((uint32_t)r->unreg, m, mi),
		      "%s: UNREGISTER_SURFACE (%d) denied", r->tag, r->unreg);

		/* The two that must never get through, on any branch. */
		CHECK(!nvkvm_nvkms_cmd_allowed_ver((uint32_t)r->grant, m, mi),
		      "%s: GRANT_SURFACE (%d) ADMITTED", r->tag, r->grant);
		if (r->fliplock >= 0)
			CHECK(!nvkvm_nvkms_cmd_allowed_ver(
				      (uint32_t)r->fliplock, m, mi),
			      "%s: SET_FLIPLOCK_GROUP (%d) ADMITTED",
			      r->tag, r->fliplock);

		/* vblank-sem trio where the branch has it. */
		if (r->en >= 0) {
			CHECK(nvkvm_nvkms_cmd_allowed_ver((uint32_t)r->en, m, mi),
			      "%s: ENABLE_VBLANK_SEM (%d) denied", r->tag, r->en);
			CHECK(nvkvm_nvkms_cmd_allowed_ver((uint32_t)r->dis, m, mi),
			      "%s: DISABLE_VBLANK_SEM (%d) denied", r->tag, r->dis);
			CHECK(nvkvm_nvkms_cmd_allowed_ver((uint32_t)r->accel, m, mi),
			      "%s: ACCEL_VBLANK_SEM (%d) denied", r->tag, r->accel);
		} else {
			/* Out of range for this branch: 60..63 name nothing,
			 * so nothing there may be admitted. */
			for (uint32_t c = 60; c <= 63; c++)
				CHECK(!nvkvm_nvkms_cmd_allowed_ver(c, m, mi),
				      "%s: %u admitted but branch has no "
				      "vblank-sem ops", r->tag, c);
		}
	}

	/* Regression guards, each stated as its own case.
	 *
	 * The ORIGINAL hardcoded list was {0,1,17,18,60,61,62}: on 570.86 that
	 * admitted GRANT_SURFACE(18), and on 575/580 SET_FLIPLOCK_GROUP(60). */
	CHECK(!nvkvm_nvkms_cmd_allowed_ver(18, 570, 86),
	      "570.86: GRANT_SURFACE(18) still admitted");
	CHECK(!nvkvm_nvkms_cmd_allowed_ver(60, 580, 178),
	      "580.178: SET_FLIPLOCK_GROUP(60) still admitted");
	CHECK(!nvkvm_nvkms_cmd_allowed_ver(60, 575, 51),
	      "575.51: SET_FLIPLOCK_GROUP(60) still admitted");

	/* The FIRST attempt at the fix keyed on the major alone, which is still
	 * wrong: it put all of 550..574 in the unshifted regime and so admitted
	 * SET_FLIPLOCK_GROUP(60) on 570.207+, where the branch has already
	 * renumbered.  This is the case that caught it. */
	CHECK(!nvkvm_nvkms_cmd_allowed_ver(60, 570, 207),
	      "570.207: SET_FLIPLOCK_GROUP(60) admitted (major-only keying)");
	CHECK(!nvkvm_nvkms_cmd_allowed_ver(60, 570, 211),
	      "570.211: SET_FLIPLOCK_GROUP(60) admitted (major-only keying)");
	/* ...and the same split must not cost 570.195 its vblank ops. */
	CHECK(nvkvm_nvkms_cmd_allowed_ver(60, 570, 195),
	      "570.195: ENABLE_VBLANK_SEM(60) wrongly denied");

	/* An unknown or unparseable branch admits device alloc/free and nothing
	 * else -- extrapolating the numbering is what let FLIPLOCK in. */
	unsigned unknown[] = { 0, 470, 514, 611, 700 };
	for (unsigned i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
		unsigned m = unknown[i];
		CHECK(nvkvm_nvkms_cmd_allowed_ver(0, m, 0),
		      "unknown %u: ALLOC_DEVICE denied", m);
		CHECK(nvkvm_nvkms_cmd_allowed_ver(1, m, 0),
		      "unknown %u: FREE_DEVICE denied", m);
		for (uint32_t c = 2; c <= 70; c++)
			CHECK(!nvkvm_nvkms_cmd_allowed_ver(c, m, 0),
			      "unknown %u: cmdType %u admitted", m, c);
	}

	/* The escape hatch still works, and only for what it names. */
	setenv("NVKVM_NVKMS_EXTRA_ALLOW", "33,34", 1);
	CHECK(nvkvm_nvkms_cmd_allowed_ver(33, 575, 51), "extra-allow 33 refused");
	CHECK(nvkvm_nvkms_cmd_allowed_ver(34, 575, 51), "extra-allow 34 refused");
	CHECK(!nvkvm_nvkms_cmd_allowed_ver(35, 575, 51), "extra-allow leaked 35");
	unsetenv("NVKVM_NVKMS_EXTRA_ALLOW");

	printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
	return tests_failed ? 1 : 0;
}
