/*
 * test_drm_devinfo.c — the nvidia-drm GET_DEV_INFO params layout, pinned per
 *                      driver version, and the two guest-side rewrites that
 *                      index into it.
 *
 * WHAT WENT WRONG, AND WHY A SIZE ASSERTION ALONE WOULD NOT HAVE CAUGHT IT
 *
 * src/guest/nvkvm_drm.c hardcoded ONE layout for
 * `struct drm_nvidia_get_dev_info_params`.  NVIDIA has edited that struct four
 * times inside the range nvkvm supports, and twice by INSERTING a field in the
 * middle, so on any host outside the branch the hardcode was captured from,
 * every field after the insertion point is off by one word:
 *
 *   Measured, RTX 4090 / 570.133.20 host, SteamOS guest: the host returns 32
 *   bytes, not 36.  With the 36-byte struct,
 *   `p->primary_index = dev->primary->index` landed on **supports_alloc**
 *   (guest got supports_alloc=0) and `p->supports_sync_fd = 0` cleared
 *   **supports_semsurf**, leaving supports_sync_fd=1 — the exact hang that line
 *   exists to prevent.  supports_alloc=0 makes libnvidia-allocator's GBM
 *   backend refuse the device, Mesa falls back to llvmpipe, the compositor
 *   never gets an NVIDIA EGL display, and nothing is scanned out at all.
 *
 *   And the mirror image: pinning the 32-byte layout reintroduces exactly that
 *   on 575 and every branch above it.  Neither struct is "the" struct.
 *
 * So the property under test is NOT "the table has the right sizes".  It is:
 *
 *     FOR EVERY SUPPORTED HOST VERSION, THE TWO WRITES nvkvm MAKES LAND ON
 *     primary_index AND supports_sync_fd — AND ON NOTHING ELSE.
 *
 * §3 below checks exactly that, field by field, on a host-shaped reply.  §4 is
 * its negative control: it feeds the SAME buffer through the neighbouring
 * layout and asserts the documented damage appears.  Without §4 a table of
 * offsets could be self-consistently wrong and §3 would still be green.
 *
 * WHERE THE EXPECTED VALUES COME FROM
 *
 * `struct drm_nvidia_get_dev_info_params` was extracted from NVIDIA
 * open-gpu-kernel-modules at ALL 216 published tags, 515.43.04 → 610.57.04
 * (kernel-open/nvidia-drm/nvidia-drm-ioctl.h, renamed to
 * .../nv_drm_common_ioctl.h at 590 with identical contents).  Four distinct
 * layouts; all three boundaries are adjacent-tag exact:
 *
 *   515.43.04 .. 535.309.01   20 B   the original
 *   545.23.06 .. 545.23.08    28 B   +supports_sync_fd, +supports_semsurf (appended)
 *   545.29.02 .. 570.211.01   32 B   +supports_alloc INSERTED at word 2
 *   575.51.02 .. 610.57.04    36 B   +mig_device INSERTED at word 1
 *
 * Note 545: that boundary is INSIDE the branch, which is why the selector takes
 * major AND minor — the same shape as the NVKMS enum split inside 570
 * (src/qemu/nvkvm_nvkms_allowlist.h, pinned by test_nvkms_allowlist.c).
 *
 * NOTHING HERE IS A COPY OF PRODUCTION CODE.  §1–§4 call
 * nvkvm_drm_devinfo_layout_for_version() and nvkvm_drm_devinfo_fixup() out of
 * src/guest/nvkvm_drm_abi.h directly — the same inlines the guest module
 * compiles.  §5's structs are EXTRACTED from src/guest/nvkvm_drm.c at build
 * time (the drm_param_structs.inc rule in the Makefile) between the
 * NVKVM_DRM_PARAM_STRUCTS_BEGIN/_END markers, same technique as
 * test_stub_ptr_sanitize.c and test_kvm_slot.c, same reason: a copy drifts, an
 * extraction cannot.  Lose the markers and the .inc is empty and this file
 * fails to compile.
 */
#include <linux/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "nvkvm_abi.h"          /* nvkvm_abi_parse_version() — the real parser */
#include "nvkvm_drm_abi.h"      /* the code under test                        */
#include "drm_param_structs.inc"/* extracted from src/guest/nvkvm_drm.c       */

static int tests_run, tests_passed;

static void ok(const char *name, bool cond, const char *detail)
{
	tests_run++;
	if (cond) {
		tests_passed++;
		printf("  ok   %-56s %s\n", name, detail ? detail : "");
	} else {
		printf("  FAIL %-56s %s\n", name, detail ? detail : "");
	}
}

/* ── §1  the version → layout table, pinned tag by tag ────────────────────── */

struct expect {
	const char *version;   /* the exact OGKM tag the header was read at     */
	unsigned    size;
	int         gpu_id, mig_device, primary_index, supports_alloc;
	int         generic_page_kind, page_kind_generation, sector_layout;
	int         supports_sync_fd, supports_semsurf;
};

/* Every row below was read out of that tag's own header.  The four distinct
 * layouts are repeated per tag deliberately: the point is which VERSIONS map to
 * which layout, and both sides of all three boundaries are named. */
static const struct expect expects[] = {
	/* --- 20 B: 515.43.04 .. 535.309.01 --------------------------------- */
	{ "515.43.04",  20, 0, -1, 4, -1,  8, 12, 16, -1, -1 },
	{ "525.147.05", 20, 0, -1, 4, -1,  8, 12, 16, -1, -1 },
	{ "530.41.03",  20, 0, -1, 4, -1,  8, 12, 16, -1, -1 },
	{ "535.54.03",  20, 0, -1, 4, -1,  8, 12, 16, -1, -1 },
	{ "535.309.01", 20, 0, -1, 4, -1,  8, 12, 16, -1, -1 },  /* last 20 B  */
	/* --- 28 B: the two 545.23.x releases, and only those --------------- */
	{ "545.23.06",  28, 0, -1, 4, -1,  8, 12, 16, 20, 24 },  /* first 28 B */
	{ "545.23.08",  28, 0, -1, 4, -1,  8, 12, 16, 20, 24 },  /* last 28 B  */
	/* --- 32 B: 545.29.02 .. 570.211.01 — the MID-BRANCH boundary ------- */
	{ "545.29.02",  32, 0, -1, 4,  8, 12, 16, 20, 24, 28 },  /* first 32 B */
	{ "545.29.06",  32, 0, -1, 4,  8, 12, 16, 20, 24, 28 },
	{ "550.40.07",  32, 0, -1, 4,  8, 12, 16, 20, 24, 28 },
	{ "550.163.01", 32, 0, -1, 4,  8, 12, 16, 20, 24, 28 },
	{ "555.58.02",  32, 0, -1, 4,  8, 12, 16, 20, 24, 28 },
	{ "560.35.03",  32, 0, -1, 4,  8, 12, 16, 20, 24, 28 },
	{ "565.77",     32, 0, -1, 4,  8, 12, 16, 20, 24, 28 },
	{ "570.133.20", 32, 0, -1, 4,  8, 12, 16, 20, 24, 28 },  /* the host the
								  * bug was
								  * measured on */
	{ "570.211.01", 32, 0, -1, 4,  8, 12, 16, 20, 24, 28 },  /* last 32 B  */
	/* --- 36 B: 575.51.02 .. 610.57.04 ---------------------------------- */
	{ "575.51.02",  36, 0,  4, 8, 12, 16, 20, 24, 28, 32 },  /* first 36 B */
	{ "575.64.05",  36, 0,  4, 8, 12, 16, 20, 24, 28, 32 },
	{ "580.105.08", 36, 0,  4, 8, 12, 16, 20, 24, 28, 32 },
	{ "580.159.04", 36, 0,  4, 8, 12, 16, 20, 24, 28, 32 },
	{ "590.48.01",  36, 0,  4, 8, 12, 16, 20, 24, 28, 32 },  /* header
								  * renamed here,
								  * struct is not */
	{ "595.84",     36, 0,  4, 8, 12, 16, 20, 24, 28, 32 },
	{ "610.43.02",  36, 0,  4, 8, 12, 16, 20, 24, 28, 32 },
	{ "610.57.04",  36, 0,  4, 8, 12, 16, 20, 24, 28, 32 },  /* newest tag
								  * measured   */
};

static struct nvkvm_drm_devinfo_layout layout_of(const char *version)
{
	unsigned major = 0, minor = 0, patch = 0;

	nvkvm_abi_parse_version(version, &major, &minor, &patch);
	return nvkvm_drm_devinfo_layout_for_version(major, minor);
}

static void t_table(void)
{
	size_t i;

	printf("\n§1  the version -> layout table, at every measured OGKM tag\n\n");
	for (i = 0; i < sizeof(expects) / sizeof(expects[0]); i++) {
		const struct expect *e = &expects[i];
		struct nvkvm_drm_devinfo_layout l = layout_of(e->version);
		char detail[160];
		bool match =
			l.known &&
			l.size                    == e->size &&
			l.gpu_id_off              == e->gpu_id &&
			l.mig_device_off          == e->mig_device &&
			l.primary_index_off       == e->primary_index &&
			l.supports_alloc_off      == e->supports_alloc &&
			l.generic_page_kind_off   == e->generic_page_kind &&
			l.page_kind_generation_off == e->page_kind_generation &&
			l.sector_layout_off       == e->sector_layout &&
			l.supports_sync_fd_off    == e->supports_sync_fd &&
			l.supports_semsurf_off    == e->supports_semsurf;

		snprintf(detail, sizeof(detail),
			 "%uB  mig@%d prim@%d alloc@%d syncfd@%d  (want %uB mig@%d prim@%d alloc@%d syncfd@%d)",
			 l.size, l.mig_device_off, l.primary_index_off,
			 l.supports_alloc_off, l.supports_sync_fd_off,
			 e->size, e->mig_device, e->primary_index,
			 e->supports_alloc, e->supports_sync_fd);
		ok(e->version, match, detail);
	}
}

/* ── §2  the compiled-in struct IS the widest row ─────────────────────────── */

/*
 * nvkvm_drm_ioctls[0x03] registers DRM_IOWR(..., struct
 * drm_nvidia_get_dev_info_params), and drm_ioctl() sizes its bounce buffer as
 * max(caller _IOC_SIZE, that).  If the struct ever stopped being the WIDEST of
 * the four layouts, the buffer could be smaller than lay.size and every offset
 * the handler writes at would be a potential overrun.  So this is a bounds
 * argument, not bookkeeping.
 */
static void t_struct_is_widest(void)
{
	struct nvkvm_drm_devinfo_layout newest = layout_of("610.57.04");
	size_t i;
	unsigned widest = 0;
	char detail[128];

	printf("\n§2  the registered struct is the widest layout (the buffer floor)\n\n");

	for (i = 0; i < sizeof(expects) / sizeof(expects[0]); i++)
		if (expects[i].size > widest)
			widest = expects[i].size;

	snprintf(detail, sizeof(detail), "sizeof=%zu widest measured=%u",
		 sizeof(struct drm_nvidia_get_dev_info_params), widest);
	ok("sizeof(params) >= every measured layout",
	   sizeof(struct drm_nvidia_get_dev_info_params) >= widest, detail);

	ok("sizeof(params) == the newest row",
	   sizeof(struct drm_nvidia_get_dev_info_params) == newest.size, detail);

#define FIELD_AT(f, off) do {                                                 \
		char d[96];                                                   \
		snprintf(d, sizeof(d), "offsetof=%zu row=%d",                 \
			 offsetof(struct drm_nvidia_get_dev_info_params, f),   \
			 (off));                                              \
		ok("params." #f " matches the newest row",                    \
		   (int)offsetof(struct drm_nvidia_get_dev_info_params, f)    \
			   == (off), d);                                      \
	} while (0)
	FIELD_AT(gpu_id,               newest.gpu_id_off);
	FIELD_AT(mig_device,           newest.mig_device_off);
	FIELD_AT(primary_index,        newest.primary_index_off);
	FIELD_AT(supports_alloc,       newest.supports_alloc_off);
	FIELD_AT(generic_page_kind,    newest.generic_page_kind_off);
	FIELD_AT(page_kind_generation, newest.page_kind_generation_off);
	FIELD_AT(sector_layout,        newest.sector_layout_off);
	FIELD_AT(supports_sync_fd,     newest.supports_sync_fd_off);
	FIELD_AT(supports_semsurf,     newest.supports_semsurf_off);
#undef FIELD_AT
}

/* ── §3  the fixup lands on the right fields, and only those ──────────────── */

/* A host reply, laid out the way `lay` says the host driver lays it out, with a
 * distinct sentinel in every field so any mis-placed write is identifiable. */
enum {
	S_GPU_ID   = 0x11110000u, S_MIG      = 0x22220000u,
	S_PRIMARY  = 0x33330000u, S_ALLOC    = 0x44440000u,
	S_KIND     = 0x55550000u, S_KINDGEN  = 0x66660000u,
	S_SECTOR   = 0x77770000u, S_SYNCFD   = 0x88880000u,
	S_SEMSURF  = 0x99990000u,
};

static void put(unsigned char *buf, int off, unsigned v)
{
	if (off >= 0)
		memcpy(buf + off, &v, sizeof(v));
}

static unsigned get(const unsigned char *buf, int off)
{
	unsigned v = 0;

	if (off >= 0)
		memcpy(&v, buf + off, sizeof(v));
	return v;
}

static void fill_host_reply(unsigned char *buf,
			    struct nvkvm_drm_devinfo_layout l)
{
	memset(buf, 0xee, 64);   /* poison past the struct too */
	put(buf, l.gpu_id_off,               S_GPU_ID);
	put(buf, l.mig_device_off,           S_MIG);
	put(buf, l.primary_index_off,        S_PRIMARY);
	put(buf, l.supports_alloc_off,       S_ALLOC);
	put(buf, l.generic_page_kind_off,    S_KIND);
	put(buf, l.page_kind_generation_off, S_KINDGEN);
	put(buf, l.sector_layout_off,        S_SECTOR);
	put(buf, l.supports_sync_fd_off,     S_SYNCFD);
	put(buf, l.supports_semsurf_off,     S_SEMSURF);
}

/*
 * The value the handler writes into primary_index.  Deliberately NOT 0 and not
 * one of the sentinels above: 0 is what a zeroed buffer, a cleared sync_fd and
 * a genuinely-card0 guest all read as, so a test that wrote 0 could not tell
 * "primary_index was set" from "something else was cleared here".  That
 * ambiguity is precisely what made the original bug survive — on the box it was
 * measured on, the NVIDIA card WAS card0, so the clobbered supports_alloc read
 * 0 and looked like a plausible answer.
 */
#define GUEST_CARD_MINOR 7u

/* Bytes past the struct must never be touched: drm_ioctl()'s bounce buffer is
 * max(caller _IOC_SIZE, 36), so on a 20-byte host there are 16 bytes of buffer
 * after the struct that belong to nobody. */
static bool tail_intact(const unsigned char *buf, unsigned size)
{
	unsigned i;

	for (i = size; i < 64; i++)
		if (buf[i] != 0xee)
			return false;
	return true;
}

static void t_fixup_per_version(void)
{
	static const char *const hosts[] = {
		"535.309.01",   /* 20 B — no sync_fd field to clear at all */
		"545.23.08",    /* 28 B — sync_fd present, supports_alloc not */
		"570.133.20",   /* 32 B — the host the bug was measured on   */
		"580.159.04",   /* 36 B                                       */
		"610.57.04",    /* 36 B                                       */
	};
	size_t i;

	printf("\n§3  the two rewrites land on primary_index and supports_sync_fd, and nothing else\n\n");

	for (i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
		struct nvkvm_drm_devinfo_layout l = layout_of(hosts[i]);
		unsigned char buf[64];
		char name[96], detail[160];
		bool untouched;

		fill_host_reply(buf, l);
		nvkvm_drm_devinfo_fixup(l, buf, GUEST_CARD_MINOR);

		snprintf(name, sizeof(name), "%s: primary_index := guest minor",
			 hosts[i]);
		snprintf(detail, sizeof(detail), "%uB, prim@%d reads 0x%08x",
			 l.size, l.primary_index_off,
			 get(buf, l.primary_index_off));
		ok(name, get(buf, l.primary_index_off) == GUEST_CARD_MINOR,
		   detail);

		/* Where the field exists, it must read 0.  Where it does NOT
		 * (below 545.23.06) the meaningful claim is that nothing was
		 * written past the end of the host's struct — a handler that
		 * cleared "supports_sync_fd" at the 575 offset on a 20-byte host
		 * would be scribbling 8 bytes beyond it. */
		snprintf(name, sizeof(name), "%s: supports_sync_fd cleared%s",
			 hosts[i],
			 l.supports_sync_fd_off < 0 ? " (absent: tail untouched)" : "");
		snprintf(detail, sizeof(detail), "syncfd@%d reads 0x%08x, tail past %uB intact=%d",
			 l.supports_sync_fd_off,
			 get(buf, l.supports_sync_fd_off),
			 l.size, (int)tail_intact(buf, l.size));
		ok(name,
		   tail_intact(buf, l.size) &&
			   (l.supports_sync_fd_off < 0 ||
			    get(buf, l.supports_sync_fd_off) == 0),
		   detail);

		/* THE ONE THAT ACTUALLY CAUGHT THE BUG.  supports_alloc=0 is
		 * what made the GBM backend refuse the device; supports_semsurf
		 * being cleared instead of sync_fd is what left sync_fd
		 * advertised.  Both are "a neighbour got written". */
		untouched =
			get(buf, l.gpu_id_off) == S_GPU_ID &&
			(l.mig_device_off < 0 ||
			 get(buf, l.mig_device_off) == S_MIG) &&
			(l.supports_alloc_off < 0 ||
			 get(buf, l.supports_alloc_off) == S_ALLOC) &&
			get(buf, l.generic_page_kind_off) == S_KIND &&
			get(buf, l.page_kind_generation_off) == S_KINDGEN &&
			get(buf, l.sector_layout_off) == S_SECTOR &&
			(l.supports_semsurf_off < 0 ||
			 get(buf, l.supports_semsurf_off) == S_SEMSURF);
		snprintf(name, sizeof(name),
			 "%s: every OTHER field is the host's answer", hosts[i]);
		snprintf(detail, sizeof(detail),
			 "alloc@%d=0x%08x semsurf@%d=0x%08x kind@%d=0x%08x",
			 l.supports_alloc_off, get(buf, l.supports_alloc_off),
			 l.supports_semsurf_off, get(buf, l.supports_semsurf_off),
			 l.generic_page_kind_off,
			 get(buf, l.generic_page_kind_off));
		ok(name, untouched, detail);
	}
}

/* ── §4  negative control: the WRONG layout does the documented damage ────── */

/*
 * This is the half of the suite that keeps §3 honest.  §3 would stay green
 * against a table that was self-consistently wrong (say, every offset shifted
 * the same way), because it builds the reply from the same table it then
 * checks.  §4 crosses the streams: build the reply the way the HOST lays it
 * out, then apply the NEIGHBOURING version's offsets — i.e. exactly what the
 * old hardcoded struct did — and assert the specific corruption that was
 * observed on hardware appears.  If a future edit makes the two layouts agree
 * (or makes the fixup ignore the layout), these go red.
 */
static void t_wrong_layout_corrupts(void)
{
	struct nvkvm_drm_devinfo_layout host570 = layout_of("570.133.20");
	struct nvkvm_drm_devinfo_layout host580 = layout_of("580.159.04");
	unsigned char buf[64];
	char detail[160];

	printf("\n§4  negative control — applying the neighbouring layout corrupts, exactly as measured\n\n");

	/* The bug as it actually presented: 580-shaped (36 B) offsets applied to
	 * a 570 host's 32-byte reply. */
	fill_host_reply(buf, host570);
	nvkvm_drm_devinfo_fixup(host580, buf, GUEST_CARD_MINOR);

	snprintf(detail, sizeof(detail), "supports_alloc@%d now 0x%08x (was 0x%08x)",
		 host570.supports_alloc_off,
		 get(buf, host570.supports_alloc_off), (unsigned)S_ALLOC);
	ok("36B offsets on a 570 host clobber supports_alloc",
	   get(buf, host570.supports_alloc_off) == GUEST_CARD_MINOR, detail);

	snprintf(detail, sizeof(detail), "supports_semsurf@%d now 0x%08x, sync_fd@%d still 0x%08x",
		 host570.supports_semsurf_off,
		 get(buf, host570.supports_semsurf_off),
		 host570.supports_sync_fd_off,
		 get(buf, host570.supports_sync_fd_off));
	ok("36B offsets on a 570 host clear supports_semsurf, not sync_fd",
	   get(buf, host570.supports_semsurf_off) == 0 &&
		   get(buf, host570.supports_sync_fd_off) == (unsigned)S_SYNCFD,
	   detail);

	snprintf(detail, sizeof(detail), "primary_index@%d still 0x%08x (the HOST's card number)",
		 host570.primary_index_off, get(buf, host570.primary_index_off));
	ok("36B offsets on a 570 host leave the host's primary_index in place",
	   get(buf, host570.primary_index_off) == (unsigned)S_PRIMARY, detail);

	/* And the mirror image, which is what pinning the 32-byte layout would
	 * have shipped to every 575/580/590/595/610 host. */
	fill_host_reply(buf, host580);
	nvkvm_drm_devinfo_fixup(host570, buf, GUEST_CARD_MINOR);

	snprintf(detail, sizeof(detail), "mig_device@%d now 0x%08x (was 0x%08x)",
		 host580.mig_device_off, get(buf, host580.mig_device_off),
		 (unsigned)S_MIG);
	ok("32B offsets on a 580 host clobber mig_device",
	   get(buf, host580.mig_device_off) == GUEST_CARD_MINOR, detail);

	snprintf(detail, sizeof(detail), "sector_layout@%d now 0x%08x, sync_fd@%d still 0x%08x",
		 host580.sector_layout_off, get(buf, host580.sector_layout_off),
		 host580.supports_sync_fd_off,
		 get(buf, host580.supports_sync_fd_off));
	ok("32B offsets on a 580 host clear sector_layout, not sync_fd",
	   get(buf, host580.sector_layout_off) == 0 &&
		   get(buf, host580.supports_sync_fd_off) == (unsigned)S_SYNCFD,
	   detail);
}

/* ── §5  the OTHER seven param structs — the sweep, pinned ────────────────── */

/*
 * One version-dependent DRM struct hardcoded to a single layout implies others
 * might be, so every `drm_nvidia_*_params` struct nvkvm defines was swept
 * against the same 216 OGKM tags.  GET_DEV_INFO is the ONLY one that MOVES A
 * FIELD.  Six of the other seven are byte-identical at every tag they exist at:
 *
 *   semsurf_fence_ctx_create  32 B  since 545.23.06 (first tag with it)
 *   semsurf_fence_create      24 B  since 545.23.06
 *   get_drm_file_unique_id     8 B  since 555.42.02 (first tag with it)
 *   gem_identify_object        8 B  since 515.43.04 (and the
 *                                   drm_nvidia_gem_object_type enum values
 *                                   NVKMS/DMABUF/USERMEMORY/UNKNOWN = 0/1/2/
 *                                   0x7fffffff are unchanged at every tag)
 *   gem_export_nvkms_memory   24 B  since 515.43.04
 *   gem_import_nvkms_memory   32 B  since 515.43.04
 *
 * The seventh, gem_alloc_nvkms_memory, DID change — and the honest version is
 * that nvkvm's 24-byte definition is wrong on a 515/520 host, where the vendor
 * struct is 16 bytes:
 *
 *   515.43.04 .. 515.105.01  16 B  handle@0 block_linear@4 compressible@5
 *                                  __pad@6 memory_size@8
 *   520.56.06 .. 610.57.04   24 B  + flags@16   (545.23.06 additionally spells
 *                                  the trailing implicit padding out as a named
 *                                  __pad1; no offset changes)
 *
 * It is an APPEND, not an insertion, so nothing shifts: handle@0 and
 * memory_size@8 — the only fields nvkvm_drm_fwd_gem_alloc_nvkms_memory() reads
 * or writes — are identical in both.  On a 515 host the effect is eight extra
 * bytes forwarded and returned untouched, not a field landing on its neighbour.
 * No 515/520 driver builds against kernel 6.8 (NVKVM_ABI_515 and _525 are the
 * two profile rows that have never been booted), so this is recorded, not
 * keyed.  t_gem_alloc_515() below pins the fact so it cannot be forgotten if
 * that ever changes.
 *
 * The DRM_NVIDIA_* command NUMBERS were swept too: no ioctl was ever
 * renumbered across 515..610; the only changes are additions (530 added the
 * prime-fence and permissions verbs, 545 the semsurf ones, 555
 * GET_DRM_FILE_UNIQUE_ID, 595 the ROI ones) and the removal of the pre-530
 * FENCE_CONTEXT_CREATE/GEM_FENCE_ATTACH pair at 0x05/0x06, whose numbers were
 * then REUSED by PRIME_FENCE_CONTEXT_CREATE/GEM_PRIME_FENCE_ATTACH — nvkvm
 * wires neither.
 *
 * These are compile-time asserts on the EXTRACTED production definitions, so
 * they cost nothing at runtime and cannot pass against a drifted copy.  They
 * are also reported as runtime cases so the tally in run_tests.sh sees them.
 */
#define PIN(s, want_size)                                                     \
	_Static_assert(sizeof(struct s) == (want_size),                       \
		       "struct " #s " changed size; re-sweep the vendor headers")
#define PIN_OFF(s, f, want_off)                                               \
	_Static_assert(offsetof(struct s, f) == (want_off),                   \
		       "struct " #s "." #f " moved; re-sweep the vendor headers")

PIN(drm_nvidia_semsurf_fence_ctx_create_params, 32);
PIN_OFF(drm_nvidia_semsurf_fence_ctx_create_params, index, 0);
PIN_OFF(drm_nvidia_semsurf_fence_ctx_create_params, nvkms_params_ptr, 8);
PIN_OFF(drm_nvidia_semsurf_fence_ctx_create_params, nvkms_params_size, 16);
PIN_OFF(drm_nvidia_semsurf_fence_ctx_create_params, handle, 24);

PIN(drm_nvidia_semsurf_fence_create_params, 24);
PIN_OFF(drm_nvidia_semsurf_fence_create_params, fence_context_handle, 0);
PIN_OFF(drm_nvidia_semsurf_fence_create_params, timeout_value_ms, 4);
PIN_OFF(drm_nvidia_semsurf_fence_create_params, wait_value, 8);
PIN_OFF(drm_nvidia_semsurf_fence_create_params, fd, 16);

PIN(drm_nvidia_get_drm_file_unique_id_params, 8);
PIN_OFF(drm_nvidia_get_drm_file_unique_id_params, id, 0);

PIN(drm_nvidia_gem_alloc_nvkms_memory_params, 24);
PIN_OFF(drm_nvidia_gem_alloc_nvkms_memory_params, handle, 0);
PIN_OFF(drm_nvidia_gem_alloc_nvkms_memory_params, block_linear, 4);
PIN_OFF(drm_nvidia_gem_alloc_nvkms_memory_params, compressible, 5);
PIN_OFF(drm_nvidia_gem_alloc_nvkms_memory_params, memory_size, 8);
PIN_OFF(drm_nvidia_gem_alloc_nvkms_memory_params, flags, 16);

PIN(drm_nvidia_gem_identify_object_params, 8);
PIN_OFF(drm_nvidia_gem_identify_object_params, handle, 0);
PIN_OFF(drm_nvidia_gem_identify_object_params, object_type, 4);

PIN(drm_nvidia_gem_export_nvkms_memory_params, 24);
PIN_OFF(drm_nvidia_gem_export_nvkms_memory_params, handle, 0);
PIN_OFF(drm_nvidia_gem_export_nvkms_memory_params, nvkms_params_ptr, 8);
PIN_OFF(drm_nvidia_gem_export_nvkms_memory_params, nvkms_params_size, 16);

PIN(drm_nvidia_gem_import_nvkms_memory_params, 32);
PIN_OFF(drm_nvidia_gem_import_nvkms_memory_params, mem_size, 0);
PIN_OFF(drm_nvidia_gem_import_nvkms_memory_params, nvkms_params_ptr, 8);
PIN_OFF(drm_nvidia_gem_import_nvkms_memory_params, nvkms_params_size, 16);
PIN_OFF(drm_nvidia_gem_import_nvkms_memory_params, handle, 24);

static void t_other_structs(void)
{
	printf("\n§5  the other seven param structs — swept, and stable at every tag\n\n");

#define CASE(s, sz)                                                           \
	do {                                                                  \
		char d[64];                                                   \
		snprintf(d, sizeof(d), "sizeof=%zu", sizeof(struct s));       \
		ok(#s, sizeof(struct s) == (sz), d);                          \
	} while (0)
	CASE(drm_nvidia_semsurf_fence_ctx_create_params, 32);
	CASE(drm_nvidia_semsurf_fence_create_params, 24);
	CASE(drm_nvidia_get_drm_file_unique_id_params, 8);
	CASE(drm_nvidia_gem_alloc_nvkms_memory_params, 24);
	CASE(drm_nvidia_gem_identify_object_params, 8);
	CASE(drm_nvidia_gem_export_nvkms_memory_params, 24);
	CASE(drm_nvidia_gem_import_nvkms_memory_params, 32);
#undef CASE
}

/*
 * The one other struct the sweep flagged.  This is not "a test that the code is
 * right" — nvkvm's definition is knowingly the 520+ one — it is a test that the
 * two facts stay true together: the definition is 24 bytes, AND the fields
 * nvkvm actually touches sit where the 16-byte 515 struct also put them, which
 * is the entire reason the mismatch is benign.  If someone later adds a handler
 * that reads flags@16, or a 515 host is brought up, this is where to look.
 */
static void t_gem_alloc_515(void)
{
	char d[96];

	printf("\n§5b  gem_alloc_nvkms_memory: 16 B on 515, 24 B from 520.56.06 — an APPEND, so benign\n\n");

	snprintf(d, sizeof(d), "sizeof=%zu (the 520+ layout)",
		 sizeof(struct drm_nvidia_gem_alloc_nvkms_memory_params));
	ok("nvkvm carries the 520.56.06+ definition",
	   sizeof(struct drm_nvidia_gem_alloc_nvkms_memory_params) == 24, d);

	/* Both fields nvkvm reads/writes are inside the 16-byte 515 struct and
	 * at the same offsets, which is what makes the extra 8 bytes inert. */
	snprintf(d, sizeof(d), "handle@%zu memory_size@%zu (515 struct is 16 B)",
		 offsetof(struct drm_nvidia_gem_alloc_nvkms_memory_params, handle),
		 offsetof(struct drm_nvidia_gem_alloc_nvkms_memory_params, memory_size));
	ok("the fields nvkvm touches are 515-compatible",
	   offsetof(struct drm_nvidia_gem_alloc_nvkms_memory_params, handle) == 0 &&
		   offsetof(struct drm_nvidia_gem_alloc_nvkms_memory_params,
			    memory_size) == 8 &&
		   offsetof(struct drm_nvidia_gem_alloc_nvkms_memory_params,
			    memory_size) + 8 <= 16,
	   d);

	snprintf(d, sizeof(d), "flags@%zu",
		 offsetof(struct drm_nvidia_gem_alloc_nvkms_memory_params, flags));
	ok("flags is the appended field, past the 515 struct",
	   offsetof(struct drm_nvidia_gem_alloc_nvkms_memory_params, flags) >= 16,
	   d);
}

/* ── §6  an unparseable version is flagged, not silently guessed ──────────── */

static void t_unknown_version(void)
{
	struct nvkvm_drm_devinfo_layout l = layout_of("");
	struct nvkvm_drm_devinfo_layout newest = layout_of("610.57.04");
	char detail[96];

	printf("\n§6  an unparseable driver version is marked not-known\n\n");

	snprintf(detail, sizeof(detail), "known=%d size=%u", l.known, l.size);
	ok("empty version string -> known == 0", l.known == 0, detail);
	/* It still has to answer with SOMETHING, and the newest layout is what
	 * the code did unconditionally before this table existed, so no box that
	 * works today regresses.  The handler pr_warn_once()s about it. */
	ok("...and falls back to the newest measured layout",
	   l.size == newest.size &&
		   l.primary_index_off == newest.primary_index_off &&
		   l.supports_sync_fd_off == newest.supports_sync_fd_off,
	   detail);
}

int main(void)
{
	printf("test_drm_devinfo — nvidia-drm GET_DEV_INFO layout, per driver version\n");
	t_table();
	t_struct_is_widest();
	t_fixup_per_version();
	t_wrong_layout_corrupts();
	t_other_structs();
	t_gem_alloc_515();
	t_unknown_version();
	printf("\n%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
