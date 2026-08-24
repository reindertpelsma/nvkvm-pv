/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_drm_abi.h — version-keyed layout of the nvidia-drm GET_DEV_INFO
 * parameter struct.
 *
 * WHY THIS FILE EXISTS
 *
 * `struct drm_nvidia_get_dev_info_params` (DRM_NVIDIA_GET_DEV_INFO, nr 0x03) is
 * not one struct.  NVIDIA has edited it four times inside the range nvkvm
 * supports, twice by INSERTING a field in the middle rather than appending —
 * which shifts every field after the insertion point.  nvkvm_drm.c hardcoded
 * the newest layout, and on an older host that shift is silent and total:
 *
 *   Measured on an RTX 4090 / 570.133.20 host with a SteamOS guest.  The host
 *   returns 32 bytes, not 36.  Read as 9 words you get supports_alloc=6 and
 *   generic_page_kind=2 — neither is a legal value.  Read as 8 you get
 *   supports_alloc=1, generic_page_kind=6 (the correct generic kind for
 *   Turing+) and primary_index=0 on a host whose NVIDIA card IS card0.
 *
 *   With the 36-byte struct on that host, `p->primary_index = primary->index`
 *   landed on **supports_alloc** (handing the guest supports_alloc=0) and
 *   `p->supports_sync_fd = 0` cleared **supports_semsurf**, leaving
 *   supports_sync_fd advertised as 1 — the exact hang that line exists to
 *   prevent.  supports_alloc=0 makes libnvidia-allocator's GBM backend refuse
 *   the device, Mesa falls back to llvmpipe, the compositor gets no NVIDIA EGL,
 *   and nothing is ever scanned out.  Fixing the layout produced a working
 *   in-guest NVIDIA GL stack (OpenGL 4.6.0 NVIDIA 570.133.20, gbm backend
 *   "nvidia").
 *
 * The mirror-image bug is just as real: pinning the 32-byte layout (as an
 * earlier fix did) reintroduces the same llvmpipe fallback on 575 and every
 * branch above it, where the guest's own supports_alloc read lands on
 * primary_index.  There is no single correct layout.  Hence a table.
 *
 * THE MEASURED BOUNDARIES
 *
 * Extracted from `struct drm_nvidia_get_dev_info_params` in NVIDIA
 * open-gpu-kernel-modules at ALL 216 published tags, 515.43.04 → 610.57.04
 * (kernel-open/nvidia-drm/nvidia-drm-ioctl.h; renamed to
 * kernel-open/nvidia-drm/nv_drm_common_ioctl.h at 590, contents unchanged).
 * Four distinct layouts, three boundaries, every one of them adjacent-tag
 * exact:
 *
 *   word          0        1            2               3              4
 *   ------------------------------------------------------------------------
 *   <=535 (20 B)  gpu_id   primary_idx  generic_kind    kind_gen       sector
 *   545.23 (28 B) gpu_id   primary_idx  generic_kind    kind_gen       sector
 *                  (+ supports_sync_fd@20, supports_semsurf@24)
 *   545.29 (32 B) gpu_id   primary_idx  supports_alloc  generic_kind   kind_gen
 *                  (+ sector@20, supports_sync_fd@24, supports_semsurf@28)
 *   >=575 (36 B)  gpu_id   mig_device   primary_idx     supports_alloc generic_kind
 *                  (+ kind_gen@20, sector@24, sync_fd@28, semsurf@32)
 *
 *   | first tag with it | last tag with it | size | what changed              |
 *   |-------------------|------------------|------|---------------------------|
 *   | 515.43.04         | 535.309.01       | 20 B | (the original)            |
 *   | 545.23.06         | 545.23.08        | 28 B | +supports_sync_fd, +semsurf (appended) |
 *   | 545.29.02         | 570.211.01       | 32 B | +supports_alloc INSERTED at word 2 |
 *   | 575.51.02         | 610.57.04        | 36 B | +mig_device INSERTED at word 1 |
 *
 * Two of those boundaries deserve naming:
 *
 *   - **575 is a clean major boundary.**  Every 570 tag through 570.211.01 (all
 *     25 of them) is 32 bytes; every 575 tag from 575.51.02 (the first) is 36.
 *     No 571..574 branch was ever published, so nothing sits in the gap.
 *
 *   - **545 splits MID-BRANCH**, at 545.23.08 → 545.29.02.  A major-only key
 *     mis-reads the two 545.23.x releases.  This is the same vendor habit that
 *     put the NVKMS enum split inside the 570 branch
 *     (src/qemu/nvkvm_nvkms_allowlist.h) — hence major AND minor here too.
 *
 * WHY NOT KEY ON enum nvkvm_abi_id
 *
 * Because NVKVM_ABI_570 deliberately means "570 == 575 layouts" for the RM/UVM
 * sizes it was built for (src/common/nvkvm_abi.h:311-382 buckets 566..579 into
 * it), so its bucket STRADDLES exactly the 575 boundary this file is about.
 * NVKVM_ABI_545 straddles the 545.29 one likewise.  The RM/UVM profile answers
 * a different question and its boundaries are elsewhere; borrowing it here
 * would be right by luck at best.  This takes the parsed version instead, from
 * the same `nvkvm.driver_version` string the profile is selected from.
 *
 * WHY NOT DERIVE THE SIZE FROM _IOC_SIZE(cmd)
 *
 * It is tempting — DRM hands the driver a buffer of max(user _IOC_SIZE,
 * registered size), and over 515..610 the four layouts happen to have four
 * distinct sizes, so size would be a bijection to layout *today*.  Three
 * reasons it is the wrong key anyway:
 *
 *   1. A DRM ioctl handler never sees `cmd`.  drm_ioctl_desc::func takes
 *      (dev, data, file); getting at _IOC_SIZE means interposing our own
 *      .unlocked_ioctl/.compat_ioctl and stashing the size per call — new
 *      module-wide plumbing for a value the module already holds.
 *   2. Size is a proxy for layout, not the layout.  The bug being fixed here is
 *      NVIDIA inserting a field in the MIDDLE of a published struct; a
 *      same-size reorder (exactly what happened to the NVKMS command enum at
 *      570.207) would slip through a size-keyed decoder silently, which is the
 *      failure mode this whole file exists to stop.  A version-keyed table gets
 *      a new row and a new measurement instead.
 *   3. _IOC_SIZE tells you what the GUEST's userspace expects.  The field
 *      offsets this handler writes at must match what the HOST driver wrote,
 *      and the host driver version is the authoritative, already-plumbed key
 *      (read from the shm control block at probe, before nvkvm_drm_init).  In
 *      nvkvm's deployment the two coincide because the guest's NVIDIA libraries
 *      are staged from the host — but nothing enforces that, and keying on the
 *      guest's number would make a staging mistake silently corrupt the reply
 *      instead of merely mismatching.
 *
 * The good half of the idea is kept: this fills only the fields the host
 * version actually has (offset -1 == "this field does not exist in that
 * release"), rather than parallel struct definitions.
 *
 * Pure ints + static inline, no kernel-only API, so tests/unit can include it
 * directly and pin every row against the vendor headers
 * (tests/unit/test_drm_devinfo.c).
 */
#ifndef NVKVM_DRM_ABI_H
#define NVKVM_DRM_ABI_H

/*
 * The WIDEST layout (575.51.02 .. 610.57.04).  This is what the DRM ioctl table
 * registers, and it is deliberately the largest of the four: drm_ioctl() sizes
 * its bounce buffer as max(user _IOC_SIZE, registered size), so registering the
 * widest guarantees the buffer is >= 36 bytes no matter which layout the caller
 * uses, and every offset in the table below is in bounds.  It copies in/out
 * only the caller's _IOC_SIZE, so an older caller is unaffected by the floor.
 *
 * Field NAMES are for reading; the handler indexes by the offsets below.
 */
struct drm_nvidia_get_dev_info_params {       /* 36 bytes, all scalars */
	unsigned int gpu_id, mig_device, primary_index, supports_alloc;
	unsigned int generic_page_kind, page_kind_generation, sector_layout;
	unsigned int supports_sync_fd, supports_semsurf;
};

/* One row of the table above.  Every offset is a byte offset from the start of
 * the params blob; -1 means THE FIELD DOES NOT EXIST in that release (not
 * "offset zero"), and the handler must not write it. */
struct nvkvm_drm_devinfo_layout {
	int known;                    /* 0: version unparseable, row is a guess */
	unsigned size;                /* bytes to forward to the host driver    */
	int gpu_id_off;
	int mig_device_off;           /* -1 below 575                           */
	int primary_index_off;
	int supports_alloc_off;       /* -1 below 545.29                        */
	int generic_page_kind_off;
	int page_kind_generation_off;
	int sector_layout_off;
	int supports_sync_fd_off;     /* -1 below 545.23                        */
	int supports_semsurf_off;     /* -1 below 545.23                        */
};

/*
 * Select the layout for a host driver version.
 *
 * Takes major AND minor because the 545 boundary falls inside the branch (see
 * the table above).  Patch is not needed: no boundary has ever fallen inside a
 * 545.x/570.x/575.x minor for this struct — 545.23.06 and 545.23.08 agree, and
 * so do all 25 570 tags.
 */
static inline struct nvkvm_drm_devinfo_layout
nvkvm_drm_devinfo_layout_for_version(unsigned major, unsigned minor)
{
	/* 575.51.02 .. 610.57.04 — mig_device inserted at word 1. */
	struct nvkvm_drm_devinfo_layout l = {
		1, 36, 0, 4, 8, 12, 16, 20, 24, 28, 32
	};

	/*
	 * An unparseable/absent version string yields major 0.  That cannot
	 * happen on a working box — nvkvm_negotiate_version() populates
	 * nvkvm.driver_version and FAILS the probe if it cannot, and
	 * nvkvm_drm_init() only runs after it — so this is belt-and-braces.
	 * Hold the newest measured layout (what the code did unconditionally
	 * before this table existed, so no box regresses) but flag it not-known
	 * so the caller can say so out loud rather than guess in silence.
	 */
	if (major == 0) {
		l.known = 0;
		return l;
	}

	/* 515.43.04 .. 535.309.01, all 20 bytes.  No supports_alloc, no
	 * supports_sync_fd, no supports_semsurf: the semaphore-surface fence
	 * ioctls those two describe do not exist before 545 either. */
	if (major < 545) {
		struct nvkvm_drm_devinfo_layout o = {
			1, 20, 0, -1, 4, -1, 8, 12, 16, -1, -1
		};
		return o;
	}

	/* 545.23.06 / 545.23.08 — supports_sync_fd + supports_semsurf appended,
	 * supports_alloc not yet present. */
	if (major == 545 && minor < 29) {
		struct nvkvm_drm_devinfo_layout o = {
			1, 28, 0, -1, 4, -1, 8, 12, 16, 20, 24
		};
		return o;
	}

	/* 545.29.02 .. 570.211.01 — supports_alloc INSERTED at word 2. */
	if (major < 575) {
		struct nvkvm_drm_devinfo_layout o = {
			1, 32, 0, -1, 4, 8, 12, 16, 20, 24, 28
		};
		return o;
	}

	/*
	 * 575.51.02 .. 610.57.04 measured; anything ABOVE 610.57.04 is an
	 * EXTRAPOLATION.  NVIDIA guarantees no ABI stability across releases and
	 * has moved this struct twice already, so when a new branch appears,
	 * read its header and add a row rather than trusting this fallthrough.
	 */
	return l;
}

/*
 * The two guest-side rewrites nvkvm makes to a GET_DEV_INFO reply, applied at
 * the offsets `lay` says those fields live at.  Split out of
 * nvkvm_drm_fwd_get_dev_info() so tests/unit can drive the REAL code rather
 * than a copy of it: everything version-dependent about the handler is here,
 * and the handler is left with the DRM plumbing.
 *
 *   - primary_index  <- the GUEST's DRM card minor.  The host field holds the
 *                       HOST's card number, which NVIDIA's userspace turns
 *                       straight into a "/dev/dri/card%d" path that does not
 *                       exist in the guest.
 *   - supports_sync_fd <- 0.  We cannot pass a host sync fd back across the
 *                       boundary; advertising it hangs every GL client on its
 *                       first eglSwapBuffers.  Absent (-1) before 545.23.06.
 *
 * `blob` must be at least lay.size bytes.  Nothing else in the reply is
 * touched — in particular supports_alloc, generic_page_kind,
 * page_kind_generation, sector_layout and supports_semsurf are the host's
 * answers and getting one of them clobbered by a mis-sized struct is exactly
 * the bug this file documents.
 */
static inline void nvkvm_drm_devinfo_fixup(struct nvkvm_drm_devinfo_layout lay,
					   void *blob, unsigned primary_index)
{
	unsigned char *b = (unsigned char *)blob;

	if (lay.primary_index_off >= 0)
		*(unsigned int *)(b + lay.primary_index_off) = primary_index;
	if (lay.supports_sync_fd_off >= 0)
		*(unsigned int *)(b + lay.supports_sync_fd_off) = 0u;
}

#endif /* NVKVM_DRM_ABI_H */
