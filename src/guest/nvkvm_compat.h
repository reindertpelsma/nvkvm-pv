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
 * class_create() lost its `owner` argument in 6.4 (commit 1aaba11da9aa).
 * Older kernels take (owner, name).
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
#define nvkvm_class_create(name)   class_create(THIS_MODULE, (name))
#else
#define nvkvm_class_create(name)   class_create(name)
#endif

/*
 * vm_flags_set()/vm_flags_clear() arrived in 6.3, when vma->vm_flags became
 * __private to force writers through an accessor.  Before that the field is
 * written directly, which is what the accessor does anyway.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
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
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
static inline void vma_start_write(struct vm_area_struct *vma)
{
}
#endif

/*
 * class->devnode() took a non-const struct device * until 6.2 (commit
 * ff62b8e6588f constified the whole family).  Spell the parameter through this
 * macro so one definition satisfies both.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
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
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 14, 0)
#define NVKVM_DRM_DRIVER_DATE   .date = "20160202",
#else
#define NVKVM_DRM_DRIVER_DATE
#endif

/*
 * The maple-tree VMA iterator (VMA_ITERATOR / for_each_vma) arrived in 6.1.
 * Before that a mm's VMAs are a plain linked list through vm_next, walked
 * under the same mmap_read_lock.  Same traversal, same order, same lock.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
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
 * PDE_DATA() was lowercased to pde_data() in 5.17 (commit 359745d78351).
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0)
#define pde_data(inode)   PDE_DATA(inode)
#endif

#endif /* NVKVM_COMPAT_H */
