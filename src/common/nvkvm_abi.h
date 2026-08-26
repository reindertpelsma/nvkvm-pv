/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_abi.h — per-driver-version ABI profile (multi-driver support, #81).
 *
 * The NVIDIA RM/UVM ABI is mostly stable across open-driver versions, but a
 * handful of struct sizes/offsets change at known version boundaries (mirrors
 * gVisor nvproxy's version-keyed tables; see docs/audits/abi_profile_spec.md).
 * Rather than hardcode the 575 layout, every version-variant site consults a
 * profile selected from the host driver's major version.
 *
 * Shared by all three components (guest kernel module, QEMU device, freestanding
 * stub), so it uses only plain ints + static inlines — no libc, no kernel-only
 * or QEMU-only API.
 *
 * Selection is deterministic from the version MAJOR, so QEMU and the guest
 * independently parse the same `NV_ESC_CHECK_VERSION_STR` string and arrive at
 * the same profile; QEMU additionally stamps the profile id into each
 * ISOLATE_CMD_IOCTL (the `reserved` field) so the stub uses the matching
 * offsets without parsing anything.
 *
 * Values MEASURED by compiling sizeof/offsetof against open-gpu-kernel-modules
 * with tools/abi_derive.sh, which sweeps every OGKM branch (515..610) plus
 * early/late tags inside each branch.  Regenerate with
 *
 *     tools/abi_derive.sh              # full matrix + distinct-layout summary
 *     tools/abi_derive.sh --reference-check
 *
 * and tests/abi_parity asserts the table against it.  Every number below is a
 * compiled measurement at a named tag; nothing is interpolated between branches.
 *
 * OGKM begins at 515 (NVIDIA's first open release), so 470 and earlier CANNOT
 * be derived and have no profile here.  Do not guess one.
 *
 * ---------------------------------------------------------------------------
 * THE SWEEP FOUND EIGHT DISTINCT LAYOUTS, NOT FOUR, AND TWO OF THE BOUNDARIES
 * FALL *INSIDE* A BRANCH.  That is why selection takes major.minor.patch and
 * not just the major:
 *
 *   - 535 splits at the Confidential-Computing channel fields.  535.43.02 and
 *     535.54.03 measure chan_alloc_size 304; 535.86.05 and everything after it
 *     measure 360 (NV_CHANNEL_ALLOC_PARAMS gained encryptIv/decryptIv/
 *     hmacNonce, +56 B).  535.54.03 is the ORIGINAL 535 GA release, so this is
 *     not a hypothetical.  The split is NOT monotonic in the version string:
 *     the long-lived 535.43.x maintenance train picked the fields up at
 *     535.43.08 (2023-08-17), which is *newer* in wall-clock time than
 *     535.54.03 (2023-06-14) despite sorting older.
 *
 *   - 550 splits at the UVM per-GPU attribute array.  550.40.07 measures the
 *     old 1200 B UVM_MAP_EXTERNAL_ALLOCATION_PARAMS; 550.40.53 and everything
 *     after measure 9264 B.  A profile keyed to "550" alone hands a 550.40.07
 *     host uvm_map_ext_fd_off = 9248 instead of 1184 — an embedded-fd fixup
 *     ~8 KiB past the real field, which is the exact silent-corruption mode
 *     this table exists to prevent.
 *
 * Two further corrections to the old four-row table, both bucket errors:
 *
 *   - 545 was bucketed into the 535 row (`major <= 545`), but 545 measures
 *     mem_alloc_size 128 / nv00de_alloc_size 8 — the V545 layout — while the
 *     535 row says 120 / 4.  The old comment on the 535 row even said
 *     "pre-V545 mem/nv00de" while the bucket swallowed 545 itself.
 *
 *   - 610 was bucketed into the 580 row (`major >= 580`), but 610 measures
 *     chan_alloc_size 376 (NV_CHANNEL_ALLOC_PARAMS gained hHandleVASpace).
 *
 * The 570 profile is byte-identical to nvkvm's historical 575 hardcodes (575
 * shares 570's layouts), so existing 575 behavior is unchanged.
 */
#ifndef NVKVM_ABI_H
#define NVKVM_ABI_H

/* Profile ids are the major version at which the layout FIRST appears.  They go
 * on the wire (`abi_profile` in nvkvm_isolate_proto.h / nvkvm_ring_ioctl.h), so
 * the four original values keep their meaning; the new ids are additions. */
enum nvkvm_abi_id {
	NVKVM_ABI_515 = 515,   /* 515..520: as 525 but NV00DE class absent        */
	NVKVM_ABI_525 = 525,   /* 525..535.54.03: pre-CC channel (304)            */
	NVKVM_ABI_535 = 535,   /* 535.86.05+: CC channel (360), pre-V545 mem      */
	NVKVM_ABI_545 = 545,   /* 545..550.40.07: V545 mem/nv00de, pre-V550 UVM   */
	NVKVM_ABI_550 = 550,   /* 550.40.53..565: V550 UVM (9264B)                */
	NVKVM_ABI_570 = 570,   /* == 575 layouts: V550 UVM, V570 channel, pre-580 */
	NVKVM_ABI_580 = 580,   /* 580..595: V580 VASPACE + V580 NVOS46 (each +8)  */
	NVKVM_ABI_610 = 610,   /* 610+: V610 channel (+hHandleVASpace, 376)       */
};

/* UVM_REGISTER_GPU_PARAMS is invariant across every published OGKM tag nvkvm
 * supports.  MEASURED on 2026-08-26, not asserted: tools/abi_derive.sh
 * --all-published-supported compiled and RAN a sizeof/offsetof probe against
 * each of the 216 official numeric tags from 515.43.04 through 610.57.04 (the
 * complete set git ls-remote returns for major 515..610).  All 216 produced one
 * layout -- size 40, rmCtrlFd 24, hClient 28, hSmcPartRef 32, rmStatus 36 --
 * with zero clone failures and zero MISSING cells.  There is therefore no
 * version boundary to put in nvkvm_abi_profile: these are universal wire
 * constants.  OGKM's first release is 515, so 470 and earlier cannot be probed
 * at all; that is a documented gap in the evidence, not a measured zero.
 *
 * The evidence is COMMITTED, not merely cited.  An earlier revision of this
 * comment made the same claim while the only artifact was a scratch file
 * outside the repository -- it would not have survived a clone, and covered 37
 * tags rather than 216.  The sweep now lives at
 * tests/abi_parity/ogkm_register_gpu.tsv (one row per tag, with the unedited
 * 14-field output beside it in ogkm_abi_sweep_20260826.tsv), and
 * tests/abi_parity/ogkm_fixture_test.go holds the three constants below against
 * every row.  Re-running the sweep is a reviewable diff to that file.
 *
 * Keep the offsets explicit because this ioctl embeds a frontend fd.  Treating
 * +24 as rmStatus (the old 32-byte model) both suppressed managed fallback GPU
 * registration and made the dormant REALIZE replay let the driver write eight
 * bytes beyond a 32-byte stack object.
 */
#define NVKVM_UVM_REGISTER_GPU_SIZE       40u
#define NVKVM_UVM_REGISTER_GPU_FD_OFF     24u
#define NVKVM_UVM_REGISTER_GPU_STATUS_OFF 36u

struct nvkvm_abi_profile {
	unsigned id;                 /* enum nvkvm_abi_id                         */

	/* UVM ioctl param sizes / embedded-fd offset (V550 grew the per-GPU
	 * attribute array from 1 to 256 entries → +9180 bytes). */
	unsigned uvm_map_ext_size;   /* UVM_MAP_EXTERNAL_ALLOCATION params size   */
	unsigned uvm_map_ext_fd_off; /* rm_ctrl_fd offset inside that struct      */
	unsigned uvm_sem_pool_size;  /* UVM_ALLOC_SEMAPHORE_POOL params size      */

	/* RM_ALLOC class-specific alloc-param sizes (the guest forwards exactly
	 * this many bytes of libcuda's p_alloc_parms). */
	unsigned chan_alloc_size;    /* {TURING,AMPERE,HOPPER}_CHANNEL_GPFIFO_A
				      * + BLACKWELL_CHANNEL_GPFIFO_A/B: one
				      * NV_CHANNEL_ALLOC_PARAMS for every GPFIFO
				      * class, so all of them share this size.   */
	unsigned vaspace_alloc_size; /* FERMI_VASPACE_A                           */
	unsigned mem_alloc_size;     /* NV50_MEMORY_VIRTUAL / LOCAL_USER / SYSTEM */
	unsigned nv00de_alloc_size;  /* RM_USER_SHARED_DATA                       */

	/* Frontend NVOS46 (NV_ESC_RM_MAP_MEMORY_DMA, NR 0x57): V580 grew it by 8
	 * (Flags2 + KindOverride), moving the status field. */
	unsigned nvos46_size;        /* NVOS46 total size                         */
	unsigned nvos46_status_off;  /* offset of the status u32 in NVOS46        */
};

/* Profile table.  Index by enum; keep all compiled in (nvproxy-style).
 *
 * Each row's comment names the OGKM tags whose compiled probes produced it.  A
 * row is only as wide as the tags that were actually measured — if you widen a
 * range, measure the new endpoint first (tools/abi_derive.sh --tags "<tag>").
 */
static const struct nvkvm_abi_profile nvkvm_abi_profiles[] = {
	{
		/* 515.43.04, 515.57, 515.105.01, 520.56.06, 520.61.07.
		 * Identical to the 525 row except that NV00DE / RM_USER_SHARED_DATA
		 * does not exist anywhere in the 515/520 trees (grep: zero hits), so
		 * the class cannot be allocated and there is no size to forward.
		 * 0 records "absent" — it is NOT a measured size of 0, and it makes a
		 * stray forward a no-op instead of a wrong-length copy.
		 * chan_alloc_size measured through the pre-rename spelling
		 * NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS in nvos.h (alloc/alloc_channel.h
		 * does not exist before 525); same struct, so it is a measurement. */
		.id = NVKVM_ABI_515,
		.uvm_map_ext_size = 1200,  .uvm_map_ext_fd_off = 1184,
		.uvm_sem_pool_size = 1184,
		.chan_alloc_size = 304,
		.vaspace_alloc_size = 48,
		.mem_alloc_size = 120,
		.nv00de_alloc_size = 0,    /* class absent pre-525, see above */
		.nvos46_size = 56,         .nvos46_status_off = 48,
	},
	{
		/* 525.47.04, 525.85.05, 525.147.05, 530.30.02, 530.41.03,
		 * 535.43.02, 535.54.03.
		 * Pre-Confidential-Computing channel struct: 304 B.  535.54.03 (the
		 * original 535 GA) lives HERE, not in the 535 row. */
		.id = NVKVM_ABI_525,
		.uvm_map_ext_size = 1200,  .uvm_map_ext_fd_off = 1184,
		.uvm_sem_pool_size = 1184,
		.chan_alloc_size = 304,
		.vaspace_alloc_size = 48,
		.mem_alloc_size = 120,
		.nv00de_alloc_size = 4,
		.nvos46_size = 56,         .nvos46_status_off = 48,
	},
	{
		/* 535.43.08 .. 535.43.28, 535.86.05, 535.86.10, 535.98, 535.104.05,
		 * 535.113.01, 535.129.03, 535.146.02, 535.154.05, 535.161.07,
		 * 535.171.04, 535.179, 535.183.01, 535.216.01, 535.247.01, 535.309.01
		 * — all measured, all identical.
		 * +56 B over the 525 row: NV_CHANNEL_ALLOC_PARAMS gained encryptIv,
		 * decryptIv and hmacNonce (Confidential Computing). */
		.id = NVKVM_ABI_535,
		.uvm_map_ext_size = 1200,  .uvm_map_ext_fd_off = 1184,
		.uvm_sem_pool_size = 1184,
		.chan_alloc_size = 360,
		.vaspace_alloc_size = 48,
		.mem_alloc_size = 120,
		.nv00de_alloc_size = 4,
		.nvos46_size = 56,         .nvos46_status_off = 48,
	},
	{
		/* 545.23.06, 545.23.08, 545.29.02, 545.29.03, 545.29.06, 550.40.07.
		 * V545 grew NV_MEMORY_ALLOCATION_PARAMS to 128 and NV00DE to 8, but
		 * the V550 UVM array growth has NOT landed yet — so this is neither
		 * the 535 row nor the 550 row.  The old table had no such row and
		 * bucketed 545 into 535 (mem 120 / nv00de 4: both wrong).
		 * 550.40.07 measures identically and therefore shares this profile. */
		.id = NVKVM_ABI_545,
		.uvm_map_ext_size = 1200,  .uvm_map_ext_fd_off = 1184,
		.uvm_sem_pool_size = 1184,
		.chan_alloc_size = 360,
		.vaspace_alloc_size = 48,
		.mem_alloc_size = 128,
		.nv00de_alloc_size = 8,
		.nvos46_size = 56,         .nvos46_status_off = 48,
	},
	{
		/* 550.40.53 .. 550.40.85, 550.54.14, 550.54.15, 550.76, 550.90.07,
		 * 550.127.05, 550.163.01, 555.42.02, 555.52.04, 555.58.02, 560.28.03,
		 * 560.31.02, 560.35.03, 565.57.01, 565.77 — all measured, identical.
		 * V550 grew the UVM per-GPU attribute array from 1 to 256 entries
		 * (+8064 B: 1200 -> 9264). */
		.id = NVKVM_ABI_550,
		.uvm_map_ext_size = 9264,  .uvm_map_ext_fd_off = 9248,
		.uvm_sem_pool_size = 9248,
		.chan_alloc_size = 360,
		.vaspace_alloc_size = 48,
		.mem_alloc_size = 128,
		.nv00de_alloc_size = 8,
		.nvos46_size = 56,         .nvos46_status_off = 48,
	},
	{
		/* 570.86.15, 570.123.01, 570.144, 570.172.08, 570.211.01,
		 * 575.51.02, 575.57.08, 575.64.05 — all measured, identical.
		 * 360 -> 368: NV_CHANNEL_ALLOC_PARAMS gained tpcConfigID (DTD-PG). */
		.id = NVKVM_ABI_570,       /* == 575 (current default) */
		.uvm_map_ext_size = 9264,  .uvm_map_ext_fd_off = 9248,
		.uvm_sem_pool_size = 9248,
		.chan_alloc_size = 368,    /* NV_CHANNEL_ALLOC_PARAMS_V570 */
		.vaspace_alloc_size = 48,
		.mem_alloc_size = 128,     /* NV_MEMORY_ALLOCATION_PARAMS_V545 */
		.nv00de_alloc_size = 8,
		.nvos46_size = 56,         .nvos46_status_off = 48,
	},
	{
		/* 580.65.06, 580.94.02, 580.95.05, 580.126.09, 580.178.04,
		 * 590.44.01, 590.48.01, 595.44.02, 595.58.03, 595.91.07
		 * — all measured, all identical.  590 and 595 introduce NO layout
		 * change of their own: they are measured members of the 580 row, not
		 * assumed ones. */
		.id = NVKVM_ABI_580,
		.uvm_map_ext_size = 9264,  .uvm_map_ext_fd_off = 9248,
		.uvm_sem_pool_size = 9248,
		.chan_alloc_size = 368,
		.vaspace_alloc_size = 56,  /* NV_VASPACE_ALLOCATION_PARAMETERS_V580 (+Pasid) */
		.mem_alloc_size = 128,
		.nv00de_alloc_size = 8,
		.nvos46_size = 64,         .nvos46_status_off = 56, /* NVOS46_V580 (+Flags2,KindOverride) */
	},
	{
		/* 610.43.02, 610.43.03, 610.57.04 — measured, identical.
		 * 368 -> 376: NV_CHANNEL_ALLOC_PARAMS gained hHandleVASpace alongside
		 * the existing pointer-based hVASpace. */
		.id = NVKVM_ABI_610,
		.uvm_map_ext_size = 9264,  .uvm_map_ext_fd_off = 9248,
		.uvm_sem_pool_size = 9248,
		.chan_alloc_size = 376,    /* NV_CHANNEL_ALLOC_PARAMS_V610 */
		.vaspace_alloc_size = 56,
		.mem_alloc_size = 128,
		.nv00de_alloc_size = 8,
		.nvos46_size = 64,         .nvos46_status_off = 56,
	},
};

/* Parse the leading integer (major) of an "MMM.mm.pp" version string. */
static inline unsigned nvkvm_abi_parse_major(const char *vs)
{
	unsigned m = 0;
	if (!vs)
		return 0;
	while (*vs >= '0' && *vs <= '9') {
		m = m * 10u + (unsigned)(*vs - '0');
		vs++;
	}
	return m;
}

/* Parse all three components of "MMM.mm.pp".
 *
 * Needed because two measured layout boundaries fall inside a branch (535 at
 * the CC channel fields, 550 at the V550 UVM array), so the major alone cannot
 * select a profile.  Missing components read as 0: "550.76" -> 550.76.0, which
 * is what the ranges below expect.  Non-numeric junk terminates a component.
 */
static inline void nvkvm_abi_parse_version(const char *vs, unsigned *major,
					   unsigned *minor, unsigned *patch)
{
	unsigned v[3] = { 0, 0, 0 };
	unsigned i = 0;

	if (vs) {
		for (i = 0; i < 3; i++) {
			while (*vs >= '0' && *vs <= '9') {
				v[i] = v[i] * 10u + (unsigned)(*vs - '0');
				vs++;
			}
			if (*vs != '.')
				break;
			vs++;
		}
	}
	*major = v[0];
	*minor = v[1];
	*patch = v[2];
}

/* Map a profile id to its table entry; defaults to 570 (== 575).
 *
 * The fallback used to be `&nvkvm_abi_profiles[1]` with the comment
 * "570/575 default" — but index 1 is the **550** row, not 570 (the table order
 * is 535, 550, 570, 580).  Look the default up by id so the code and the
 * comment cannot drift apart again.
 *
 * On a 570/575 host the old fallback happened to be harmless: the only fields
 * the stub reads through this path (uvm_map_ext_fd_off, nvos46_status_off) are
 * identical in the 550 and 570 rows.  It is NOT harmless elsewhere — on a 535
 * host an unstamped job would have taken uvm_map_ext_fd_off = 9248 instead of
 * the measured 1184, i.e. an embedded-fd fixup ~8 KiB past the real field.
 */
static inline const struct nvkvm_abi_profile *nvkvm_abi_by_id(unsigned id)
{
	unsigned i;
	unsigned dflt = 0;
	for (i = 0; i < sizeof(nvkvm_abi_profiles) / sizeof(nvkvm_abi_profiles[0]); i++) {
		if (nvkvm_abi_profiles[i].id == id)
			return &nvkvm_abi_profiles[i];
		if (nvkvm_abi_profiles[i].id == NVKVM_ABI_570)
			dflt = i;
	}
	return &nvkvm_abi_profiles[dflt];
}

/* Map a full host driver version to a profile id.
 *
 * Every boundary below is a MEASURED one: the two tags named in each comment
 * are adjacent OGKM releases whose compiled probes disagree.  Nothing here is
 * interpolated — regenerate with tools/abi_derive.sh.
 */
static inline unsigned nvkvm_abi_id_for_version(unsigned major, unsigned minor,
						unsigned patch)
{
	/* An unparseable/absent version string yields major 0.  Fall back to the
	 * same default nvkvm_abi_by_id() uses rather than letting 0 slide into the
	 * 515 row — whose nv00de_alloc_size is the "class absent" sentinel 0 and
	 * would silently zero-size every RM_USER_SHARED_DATA alloc. */
	if (major == 0)
		return NVKVM_ABI_570;

	/* 470 and earlier predate open-gpu-kernel-modules, so there is no source
	 * to probe and NO MEASURED LAYOUT.  This is a nearest-known fallback, not
	 * a measurement; treat a sub-515 host as unsupported rather than trusting
	 * it.  (NV00DE genuinely did not exist that far back either, so the 515
	 * row's sentinel is at least not obviously wrong here.) */
	if (major <= 520)
		return NVKVM_ABI_515;   /* 515/520: NV00DE class absent entirely     */
	if (major <= 530)
		return NVKVM_ABI_525;   /* 525.147.05 == 530.41.03 == 525 row        */

	if (major == 535) {
		/* Confidential-Computing channel fields (+56 B) landed between
		 * 535.54.03 (measured 304) and 535.86.05 (measured 360).  The
		 * 535.43.x maintenance train is chronologically interleaved and
		 * picked them up at 535.43.08 (535.43.02 = 304, 535.43.08 = 360),
		 * so this cannot be written as a single numeric comparison. */
		if (minor == 43 && patch < 8)
			return NVKVM_ABI_525;   /* 535.43.02 measured 304 */
		if (minor == 54)
			return NVKVM_ABI_525;   /* 535.54.03 (535 GA) measured 304 */
		return NVKVM_ABI_535;           /* 535.86.05+ measured 360 */
	}

	/* 536..544: no such branch was ever published.  Hold the last MEASURED
	 * layout below the hole (535) rather than letting the range checks below
	 * carry a hypothetical version forward into a much newer row. */
	if (major < 545)
		return NVKVM_ABI_535;

	if (major == 545)
		return NVKVM_ABI_545;   /* V545 mem/nv00de, pre-V550 UVM             */

	/* 546..549: likewise a hole; 545 is the last measured layout below it. */
	if (major < 550)
		return NVKVM_ABI_545;

	if (major == 550) {
		/* V550 UVM array growth (1200 -> 9264) landed between 550.40.07
		 * (measured 1200) and 550.40.53 (measured 9264). */
		if (minor == 40 && patch < 53)
			return NVKVM_ABI_545;
		return NVKVM_ABI_550;
	}

	if (major <= 565)
		return NVKVM_ABI_550;   /* 555.58.02, 560.35.03, 565.77 all measured */
	if (major <= 579)
		return NVKVM_ABI_570;   /* 570.211.01 == 575.64.05                   */
	if (major <= 595)
		return NVKVM_ABI_580;   /* 580/590/595 measured byte-identical       */

	/* 596..609: no OGKM branch was ever published in that range, so there is
	 * nothing to measure; 595's layout is the last known-good one. */
	if (major < 610)
		return NVKVM_ABI_580;

	/* 610.57.04 measured.  Anything ABOVE 610 is an EXTRAPOLATION — NVIDIA
	 * guarantees no ABI stability across releases, and this table has been
	 * wrong in exactly this way before.  Re-run tools/abi_derive.sh when a new
	 * branch appears and add a row rather than trusting this fallthrough. */
	return NVKVM_ABI_610;
}

/* Major-only selection, kept for callers that genuinely have nothing else.
 *
 * LOSSY: it cannot see the two intra-branch boundaries, so it answers with the
 * layout used by the MAJORITY of releases in the branch — right for 535.86.05+
 * and 550.40.53+, wrong for 535.43.02/535.54.03 and 550.40.07.  If you hold a
 * full version string, call nvkvm_abi_id_for_version() (or
 * nvkvm_abi_for_version()) instead.
 */
static inline unsigned nvkvm_abi_id_for_major(unsigned major)
{
	return nvkvm_abi_id_for_version(major, ~0u, ~0u);
}

/* Convenience: profile from a version string.
 *
 * This is what QEMU and the guest both call on the NV_ESC_CHECK_VERSION_STR
 * result, so it must use the FULL version — a major-only lookup here would
 * mis-key 535.54.03 and 550.40.07 (see nvkvm_abi_id_for_version).
 */
static inline const struct nvkvm_abi_profile *nvkvm_abi_for_version(const char *vs)
{
	unsigned major, minor, patch;

	nvkvm_abi_parse_version(vs, &major, &minor, &patch);
	return nvkvm_abi_by_id(nvkvm_abi_id_for_version(major, minor, patch));
}

#endif /* NVKVM_ABI_H */
