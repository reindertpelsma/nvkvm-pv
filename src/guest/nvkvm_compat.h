/*
 * nvkvm_compat.h — in-kernel API differences across supported kernels.
 *
 * The guest module is built against the guest's own headers, so it meets
 * whatever kernel the guest runs.  Until 2026-08-19 it carried no version
 * guards at all and therefore built only on a narrow band around 6.8, which
 * excluded both of the widely deployed LTS kernels below it (5.15, 6.1) and
 * everything current above it (6.12+).  Every guard here exists because a
 * build failed without it; see docs/reference/guest-kernels.md for the matrix
 * and tests/kernel_matrix.sh to reproduce it.
 *
 * Rule for this file: shim the SPELLING, never the semantics.  Where an API
 * gained meaning that older kernels lack (per-VMA locking), the shim compiles
 * to nothing on kernels that do not need it — which is correct there, not a
 * silent downgrade.
 */
#ifndef NVKVM_COMPAT_H
#define NVKVM_COMPAT_H

#include <linux/version.h>

/*
 * RHEL/CentOS report an ancient LINUX_VERSION_CODE and then backport large
 * parts of 6.x underneath it, so the version alone answers the wrong
 * question.  On CentOS Stream 9 (5.14.0-737.el9, RHEL_RELEASE_CODE 2313 =
 * 9.9) every guard below fired, and every one of them collided with an API
 * the kernel already had: class_create had lost its owner argument,
 * vm_flags_clear and vma_start_write were already defined, and devnode
 * already took a const struct device *.  The module did not compile at all.
 *
 * RHEL exports RHEL_RELEASE_CODE for exactly this.  NVKVM_KERNEL_LT() asks
 * "does this kernel BEHAVE like it predates x.y.z", which is the question
 * every guard here actually wants answered.
 *
 * The 9.0 bound is deliberately coarse: it is where this was measured, not
 * where each backport landed.  If an earlier 9.x minor turns out to lack one
 * of these, tighten that guard then -- from a build failure, not a guess.
 */
#if defined(RHEL_RELEASE_CODE) && defined(RHEL_RELEASE_VERSION)
#  define NVKVM_RHEL_GE(a, b)   (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(a, b))
#else
#  define NVKVM_RHEL_GE(a, b)   0
#endif

#define NVKVM_KERNEL_LT(a, b, c) \
	(LINUX_VERSION_CODE < KERNEL_VERSION(a, b, c) && !NVKVM_RHEL_GE(9, 0))

/*
 * class_create() lost its `owner` argument in 6.4 (commit 1aaba11da9aa).
 * Older kernels take (owner, name).
 */
#if NVKVM_KERNEL_LT(6, 4, 0)
#define nvkvm_class_create(name)   class_create(THIS_MODULE, (name))
#else
#define nvkvm_class_create(name)   class_create(name)
#endif

/*
 * vm_flags_set()/vm_flags_clear() arrived in 6.3, when vma->vm_flags became
 * __private to force writers through an accessor.  Before that the field is
 * written directly, which is what the accessor does anyway.
 */
#if NVKVM_KERNEL_LT(6, 3, 0)
static inline void vm_flags_set(struct vm_area_struct *vma, unsigned long f)
{
	vma->vm_flags |= f;
}
static inline void vm_flags_clear(struct vm_area_struct *vma, unsigned long f)
{
	vma->vm_flags &= ~f;
}
#endif

/*
 * vma_start_write() arrived in 6.4 with CONFIG_PER_VMA_LOCK.  On kernels
 * without per-VMA locks there is no such lock to take and mmap_lock alone is
 * the whole exclusion, so the empty shim is the correct behaviour there rather
 * than a weakening of it.
 */
#if NVKVM_KERNEL_LT(6, 4, 0)
static inline void vma_start_write(struct vm_area_struct *vma)
{
}
#endif

/*
 * class->devnode() took a non-const struct device * until 6.2 (commit
 * ff62b8e6588f constified the whole family).  Spell the parameter through this
 * macro so one definition satisfies both.
 */
#if NVKVM_KERNEL_LT(6, 2, 0)
#define NVKVM_DEVNODE_DEV   struct device *
#else
#define NVKVM_DEVNODE_DEV   const struct device *
#endif

/*
 * pud_large()/pmd_large() were removed in 6.11 in favour of pud_leaf()/
 * pmd_leaf(), which mean the same thing and have existed since 5.6 — so the
 * leaf spelling is simply used everywhere and only the old names need shimming
 * for kernels that predate nothing.  Guard defensively in case a very old tree
 * lacks them.
 */
#ifndef pud_leaf
#define pud_leaf(pud)   pud_large(pud)
#endif
#ifndef pmd_leaf
#define pmd_leaf(pmd)   pmd_large(pmd)
#endif

/*
 * Zapping a sub-range of one VMA has had three spellings and only one of them
 * is exported to modules on every kernel we target.
 *
 *   zap_page_range()          removed in 6.4
 *   zap_page_range_single()   exported only in a narrow window -- measured
 *                             absent from Module.symvers on 6.6 and on 6.19,
 *                             present on 6.8 and 6.12
 *   zap_vma_ptes()            exported on 5.15, 6.1, 6.6, 6.8, 6.12, 6.14, 6.19
 *
 * So use zap_vma_ptes().  It is a thin wrapper that returns without doing
 * anything unless the VMA is VM_PFNMAP or VM_MIXEDMAP, and otherwise calls
 * zap_page_range_single() itself; the only caller here has already converted
 * the VMA to VM_PFNMAP, so the precondition holds and the behaviour is
 * identical.  It returns void, so that caller checks the precondition itself
 * rather than trusting a silent no-op.
 */
#define nvkvm_zap_range(vma, addr, size) \
	zap_vma_ptes((vma), (addr), (size))

/*
 * struct drm_driver lost its `date` field in 6.14 (commit 9f9ec6d5b4d1); the
 * value was never used for anything but the DRM_IOCTL_VERSION string.
 *
 * RHEL backports DRM aggressively for hardware enablement: 5.14.0-737.el9
 * already has the field removed, several major versions ahead of its own
 * version code.
 */
#if NVKVM_KERNEL_LT(6, 14, 0)
#define NVKVM_DRM_DRIVER_DATE   .date = "20160202",
#else
#define NVKVM_DRM_DRIVER_DATE
#endif

/*
 * 6.12 moved FMODE_UNSIGNED_OFFSET into struct file_operations as the
 * fop_flags bit FOP_UNSIGNED_OFFSET (commit 210a03c9d51a), and drm_open_helper()
 * now *requires* every DRM driver's fops to declare it:
 *
 *     if (WARN_ON_ONCE(!(filp->f_op->fop_flags & FOP_UNSIGNED_OFFSET)))
 *             return -EINVAL;
 *
 * Upstream drivers get it for free from DEFINE_DRM_GEM_FOPS(); ours is
 * hand-rolled, so it has to be set explicitly.  Without it EVERY open of
 * /dev/dri/card0 and /dev/dri/renderD128 fails with EINVAL on >= 6.12 -- which
 * is invisible on a 6.8 guest and takes out Xorg entirely on a 6.14 one.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define NVKVM_DRM_FOP_FLAGS   .fop_flags = FOP_UNSIGNED_OFFSET,
#else
#define NVKVM_DRM_FOP_FLAGS
#endif

/*
 * The maple-tree VMA iterator (VMA_ITERATOR / for_each_vma) arrived in 6.1.
 * Before that a mm's VMAs are a plain linked list through vm_next, walked
 * under the same mmap_read_lock.  Same traversal, same order, same lock.
 *
 * RHEL 9 backported the maple tree, so mm->mmap and vma->vm_next are gone
 * there despite the 5.14 version code -- hence NVKVM_KERNEL_LT rather than a
 * bare comparison.
 */
#if NVKVM_KERNEL_LT(6, 1, 0)
#define VMA_ITERATOR(name, mm, addr) \
	struct vm_area_struct *name = (mm)->mmap
#define for_each_vma(vmi, vma) \
	for ((vma) = (vmi); (vma); (vma) = (vma)->vm_next)
#endif

/*
 * hrtimer_init() plus a manual ->function assignment became hrtimer_setup() in
 * 6.15 (commit 8fa7292fee5c), which does both and is the only spelling left.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 15, 0)
#define nvkvm_hrtimer_setup(t, fn, clk, mode)   do {    \
		hrtimer_init((t), (clk), (mode));       \
		(t)->function = (fn);                   \
	} while (0)
#else
#define nvkvm_hrtimer_setup(t, fn, clk, mode)           \
	hrtimer_setup((t), (fn), (clk), (mode))
#endif

/*
 * page_mapcount() was removed in 6.16.  It answered "how many page tables map
 * THIS page", which is exactly what nvkvm_cpu_pages_migrate_range() asks before
 * it relocates a range: >1 means something else already maps these pages and
 * relocating would desynchronise the two views (the silent-corruption bug in
 * docs/investigations/shared-mapping-desync/).
 *
 * The replacement, folio_mapcount(), counts mappings of the whole FOLIO.  For
 * an order-0 folio -- one page -- that is the same number the old call gave,
 * exactly.  For a large folio (THP) it is the sum across the folio, so it can
 * read HIGHER than the true per-page count.  There is no exported precise
 * per-page count on 6.16: folio_precise_page_mapcount() lives in mm/internal.h
 * and is not available to modules.
 *
 * Over-reporting is the direction we want.  An over-count refuses a
 * registration that might have been safe -- an honest -EINVAL the caller sees.
 * An under-count would let us relocate a range somebody else maps, which is
 * silent data corruption.  Given the choice, refuse.
 *
 * Verified by compiling against 6.16.12-valve24.5 (SteamOS); before this shim
 * the guest module did not build there at all.
 * Spelled as a macro, not an inline: this header is pulled in via nvkvm.h
 * before <linux/mm.h> in most translation units, so an inline body here would
 * reference folio_mapcount() before mm.h declares it -- which fails to build
 * with "static declaration follows non-static declaration".  A macro defers
 * that lookup to the call site, where mm.h is already included.
 */
/*
 * THRESHOLD IS 6.12, NOT 6.16.  This shim was written against SteamOS 6.16
 * and guarded at 6.16, but page_mapcount() was removed from the public
 * headers well before that -- MEASURED by tests/kernel_matrix.sh: 6.8
 * (ubuntu 24.04) builds, 6.12.107 (debian 13) and 6.14.11 (ubuntu 25.04)
 * both fail with "implicit declaration of function 'page_mapcount'".
 * Guarding at 6.16 left every kernel in 6.12..6.15 taking the removed
 * spelling, so the guest module did not build on two current distros.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define nvkvm_page_mapcount(page)   folio_mapcount(page_folio(page))
#else
#define nvkvm_page_mapcount(page)   page_mapcount(page)
#endif

/*
 * PDE_DATA() was lowercased to pde_data() in 5.17 (commit 359745d78351).
 * RHEL 9 carries the lowercase spelling at version code 5.14.
 */
#if NVKVM_KERNEL_LT(5, 17, 0)
#define pde_data(inode)   PDE_DATA(inode)
#endif

#endif /* NVKVM_COMPAT_H */
