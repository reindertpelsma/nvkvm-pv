/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_uvm_va.h — the U-6 UVM gate: command schema + VA-range ownership table.
 *
 * Lifted out of nvkvm_isolate_handlers.c unchanged (2026-08-24) for one reason:
 * the gate's decision is policy, and policy that cannot be unit-tested is
 * policy nobody re-checks.  tests/unit/test_uvm_va_gate.c includes this header
 * and asserts the decisions directly, the same way test_ctrl_gate.c does for
 * the RM-control allowlist.  Exactly one QEMU translation unit includes it
 * (nvkvm_isolate_handlers.c), so the tables live here as file-scope statics.
 *
 * ── Why UVM_VALIDATE_VA_RANGE (72) is a QUERY and not a USE ────────────────
 *
 * Measured on Turing / Ampere / Ada / Blackwell: managed-memory traffic
 * produced `DENY UVM cmd=0x48 ... not owned by handle N (U-6)` lines that no
 * test noticed, because libcuda carries on regardless.  The refusal is CORRECT
 * — what was wrong was calling it a DENY.  The argument, from the driver's own
 * source rather than from the log:
 *
 *   1. `uvm_api_validate_va_range()` (ogkm kernel-open/nvidia-uvm/
 *      uvm_va_range.c:762-777) answers NV_OK only when ONE va_range matches the
 *      requested extent EXACTLY:
 *
 *          va_range = uvm_va_range_find(va_space, params->base);
 *          if (va_range && va_range->node.start == params->base &&
 *              va_range->node.end + 1 == params->base + params->length)
 *
 *      Anything else — a sub-extent, a span across two ranges, an address with
 *      no range at all — is NV_ERR_INVALID_ADDRESS.  72 is a *question* ("is
 *      this exact extent a UVM range?") and "no" is a routine answer that
 *      libcuda uses as information, not an error.  That is why it tolerates it.
 *
 *   2. uvm_va_covers() below is CONTAINMENT, which is strictly WEAKER than the
 *      driver's exact match.  Every va_range that can exist in a guest's
 *      va_space comes from a range-creating ioctl this table records with the
 *      identical base/length (73 CREATE_EXTERNAL_RANGE, 68, 65, 27) —
 *      including managed allocations, which the guest module downgrades to
 *      external ranges (451b788), so no range kind is created by a bare mmap
 *      any more.  Therefore U-6 cannot refuse a 72 the driver would have
 *      answered NV_OK to: a range U-6 does not cover is a range the driver
 *      does not have.
 *
 * So the gate stays exactly as strict as it was — the range is still refused,
 * still with NV_ERR_INVALID_ADDRESS, and the guest still sees the same errno.
 * What changes is the CHANNEL: a query answered "no" is not an attack probe,
 * and writing it to the DENY log — the early-warning signal for guest-
 * originated probes — trains its readers to ignore the one line that matters.
 * NVKVM_UVM_VA_QUERY marks the commands whose refusal is an expected answer;
 * nvkvm_uvm_refusal_is_security() is what the caller logs on.
 *
 * This is NOT an allowlist for 72: the ownership check still runs, the ioctl is
 * still never forwarded for an unowned range, and a guest that names a QEMU
 * address gets exactly the refusal it got before.  Only the log line differs.
 *
 * The other direction is deliberately untouched: 51/42/43/46/47/44 are USE
 * commands whose refusal blocks a real operation, so their refusals stay on the
 * DENY channel where an operator should see them.
 */
#ifndef NVKVM_UVM_VA_H
#define NVKVM_UVM_VA_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

/* ── Phase 3: UVM ioctl field schema + default-deny ──────────────────────────
 *
 * UVM ioctls execute in QEMU's (privileged) process, so we MUST NOT blindly
 * forward an arbitrary cmd the guest names.  This table is the allowlist: each
 * UVM cmd we will forward is listed with (a) a minimum param_size and (b) the
 * offsets of any embedded *frontend fd* fields (RMCtrlFD / UvmFD) that the
 * guest sanitizer rewrote into a handle_id and that the kernel will dereference
 * as an fd — those are translated to QEMU's local fd and restored on response.
 *
 * Field offsets/sizes are taken from the open kernel module / gVisor nvproxy
 * (pkg/abi/nvgpu/uvm.go): NvUUID is 16 bytes, Handle/NvU32 is 4 bytes, the
 * frontend fd is an NvS32 at a fixed offset in the struct header (before any
 * variable-length PerGPUAttributes array, so the offset is version-stable).
 *
 * DEFAULT-DENY: any UVM cmd absent from this table is refused, never forwarded.
 * Notably this denies UVM_TOOLS_READ/WRITE_PROCESS_MEMORY (62/63) — a
 * cross-process memory peek/poke primitive that has no place in our model.
 * min_size uses the base (pre-V550) struct size as a conservative floor; the
 * fd-field read is additionally guarded against the actual param_size.
 */
enum { NVKVM_UVM_FD_FIELD = 1 };

/*
 * ── U-6 (docs/internal/audit-guest-pointers.md): guest-supplied UVM VA ranges
 *
 * 15 of the rows below carry a (base, length) or (requestedBase, length) pair.
 * These are virtual addresses in the CALLING TASK'S mm — and the calling task
 * is QEMU (see the UVM branch of nvkvm_req_ioctl_on_isolate: UVM ioctls run in
 * QEMU's own process, not the isolate).  Nothing used to look at them.
 *
 * What an attacker could otherwise do: name any address in QEMU's address
 * space — the process that holds the KVM fd, every memslot, every isolate's
 * socket and the per-VM handle table.  The sharpest case is UVM_MIGRATE: when
 * UVM finds NO va_range covering the named range it falls through to the
 * pageable-memory path and migrates the CALLER'S OWN anonymous pages (i.e.
 * QEMU's heap, the 128 GiB sparse window that backs guest RAM-visible GPU
 * mappings), and UVM_MIGRATE_PARAMS.semaphoreAddress is an address the driver
 * WRITES semaphorePayload to on async completion.  This is the one finding in
 * that audit the Phase 0 isolate does not contain.
 *
 * The control: a host-side ownership table.  A UVM VA range is only usable if
 * it is one nvkvm itself established for THAT UVM handle — recorded when a
 * range-CREATING command (73 CREATE_EXTERNAL_RANGE, 68 ALLOC_SEMAPHORE_POOL,
 * 65 MAP_DYNAMIC_PARALLELISM_REGION, 27 REGISTER_CHANNEL) is accepted BY THE
 * DRIVER.  Every range-USING command must be fully contained in a recorded
 * range for the same handle; anything unresolvable is rejected with
 * NV_ERR_INVALID_ADDRESS and never reaches the driver.  Default-deny: a row
 * with no va_mode gets no VA treatment, and a cmd that is not in the table at
 * all was already refused above.
 *
 * Per-HANDLE, deliberately: each guest UVM fd gets its own QEMU-side
 * /dev/nvidia-uvm fd and therefore its own uvm_va_space, and two guest
 * processes legitimately pick the SAME base (measured: isolates 8 and 10 both
 * create 0x200000000).  A per-VM or process-wide VA reservation would break
 * that; range ownership is only meaningful inside one va_space.
 */
enum {
	NVKVM_UVM_VA_NONE   = 0, /* no guest VA range in this struct        */
	NVKVM_UVM_VA_CREATE = 1, /* establishes a range; record on success  */
	NVKVM_UVM_VA_USE    = 2, /* must be contained in a recorded range   */
	NVKVM_UVM_VA_FREE   = 3, /* must be contained; drops it on success  */
	NVKVM_UVM_VA_QUERY  = 4, /* USE's checks, but the command only ASKS
				  * about the range: a refusal is the answer,
				  * not a blocked operation.  See the header
				  * comment above for why 72 is one of these. */
};

struct nvkvm_uvm_desc {
	uint32_t cmd;
	uint16_t min_size;
	uint16_t fd_off[2];   /* frontend-fd field byte offsets; 0xffff = none */
	uint16_t va_off;      /* byte offset of the u64 base (length at +8);
			       * 0xffff = this cmd carries no VA range      */
	uint8_t  va_mode;     /* NVKVM_UVM_VA_*                             */
};
/*
 * min_size is the EXACT struct size from our ABI (src/abi/uvm.h, driver
 * 575.51.03) — verified by sizeof, NOT copied from gVisor's newer layouts
 * (several differ: e.g. REGISTER_GPU is 32B here, not gVisor's 40B-with-NUMA;
 * REGISTER_CHANNEL 48 not 56; MIGRATE 48 not 56).  The guest always sends
 * exactly this size, so "param_size < min_size" rejects only malformed calls.
 * fd-field translation is limited to the two cmds the prior code translated
 * (MM_INITIALIZE@0, REGISTER_GPU_VASPACE@16); every other cmd forwarded with
 * its fd field untouched, exactly as before — generalizing it was speculative.
 */
static const struct nvkvm_uvm_desc nvkvm_uvm_schema[] = {
	/* The full UVM command set (open kernel module / gVisor nvproxy
	 * uvm.go).  min_size: cmds whose struct is defined in our ABI
	 * (src/abi/uvm.h, driver 575.51.03) carry the exact sizeof, verified
	 * by measurement — these are the layouts the guest actually sends, so
	 * "param_size < min" rejects only malformed calls.  The five cmds NOT
	 * in our ABI (44/45/53/65/66) carry min_size 0 (allow any size): we
	 * have no driver-verified layout for them and an over-strict guess
	 * already mis-denied REGISTER_GPU once; the kernel validates its own
	 * struct against the fixed shm slot regardless.  fd-field translation
	 * stays limited to the two cmds the pre-schema code translated. */
	{ 0x30000001 /* UVM_INITIALIZE          */,  16, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 0x30000002 /* UVM_DEINITIALIZE        */,   8, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 23 /* UVM_CREATE_RANGE_GROUP          */,  16, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 24 /* UVM_DESTROY_RANGE_GROUP         */,  16, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 25 /* UVM_REGISTER_GPU_VASPACE        */,  32, { 16, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 26 /* UVM_UNREGISTER_GPU_VASPACE      */,  20, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 27 /* UVM_REGISTER_CHANNEL            */,  48, { 0xffff, 0xffff }, 32, NVKVM_UVM_VA_CREATE },
	{ 28 /* UVM_UNREGISTER_CHANNEL          */,  28, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 29 /* UVM_ENABLE_PEER_ACCESS          */,  40, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 30 /* UVM_DISABLE_PEER_ACCESS         */,  40, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 31 /* UVM_SET_RANGE_GROUP             */,  32, { 0xffff, 0xffff }, 8, NVKVM_UVM_VA_USE },
	{ 33 /* UVM_MAP_EXTERNAL_ALLOCATION     */, 9264, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 34 /* UVM_FREE                        */,  24, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_FREE },
	{ 37 /* UVM_REGISTER_GPU                */,  32, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 38 /* UVM_UNREGISTER_GPU              */,  24, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 39 /* UVM_PAGEABLE_MEM_ACCESS         */,   8, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 42 /* UVM_SET_PREFERRED_LOCATION      */,  40, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 43 /* UVM_UNSET_PREFERRED_LOCATION    */,  24, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 44 /* UVM_ENABLE_READ_DUPLICATION     */,   0, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 45 /* UVM_DISABLE_READ_DUPLICATION    */,   0, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 46 /* UVM_SET_ACCESSED_BY             */,  40, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 47 /* UVM_UNSET_ACCESSED_BY           */,  40, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 51 /* UVM_MIGRATE                     */,  48, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 53 /* UVM_MIGRATE_RANGE_GROUP         */,   0, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 65 /* UVM_MAP_DYNAMIC_PARALLELISM_REGION */, 0, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_CREATE },
	{ 66 /* UVM_UNMAP_EXTERNAL              */,   0, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_USE },
	{ 68 /* UVM_ALLOC_SEMAPHORE_POOL        */, 9248, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_CREATE },
	{ 70 /* UVM_PAGEABLE_MEM_ACCESS_ON_GPU  */,  24, { 0xffff, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	{ 72 /* UVM_VALIDATE_VA_RANGE           */,  24, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_QUERY },
	{ 73 /* UVM_CREATE_EXTERNAL_RANGE       */,  24, { 0xffff, 0xffff }, 0, NVKVM_UVM_VA_CREATE },
	{ 75 /* UVM_MM_INITIALIZE               */,   8, { 0, 0xffff }, 0xffff, NVKVM_UVM_VA_NONE },
	/* Default-denied by omission: UVM_TOOLS_READ_PROCESS_MEMORY (62) and
	 * UVM_TOOLS_WRITE_PROCESS_MEMORY (63) — a cross-process memory
	 * peek/poke primitive with no place in our isolation model — plus any
	 * unknown/garbage cmd a malicious guest might name. */
};

static const struct nvkvm_uvm_desc *nvkvm_uvm_lookup(uint32_t cmd)
{
	for (size_t i = 0;
	     i < sizeof(nvkvm_uvm_schema) / sizeof(nvkvm_uvm_schema[0]); i++) {
		if (nvkvm_uvm_schema[i].cmd == cmd)
			return &nvkvm_uvm_schema[i];
	}
	return NULL;
}

/* ── U-6: the UVM VA-range ownership table ──────────────────────────────────
 *
 * One flat array, keyed by the UVM handle_id (== one QEMU-side /dev/nvidia-uvm
 * fd == one uvm_va_space).  Entries are added ONLY after the driver itself has
 * accepted a range-creating ioctl, so the table can never contain a range the
 * driver would have refused; it is a *narrowing* of the driver's own state,
 * never a widening.  Bounded and fail-closed: if the table is full a CREATE is
 * refused rather than silently untracked.
 */
#define NVKVM_UVM_VA_MAX 16384

/*
 * U-6 — floor for the bounce buffer the UVM ioctl actually runs on.  Must be
 * >= the largest UVM_*_PARAMS the driver copies: 9264 bytes
 * (UVM_MAP_EXTERNAL_ALLOCATION_PARAMS, V550 256-entry perGpuAttributes).
 * 16 KiB leaves headroom for a future driver growing a struct, on a path that
 * runs a few hundred times per process.
 */
#define NVKVM_UVM_BOUNCE_MIN 16384u

struct nvkvm_uvm_va_ent {
	uint32_t handle_id;   /* 0 = free slot */
	uint64_t base;
	uint64_t length;
};

static struct nvkvm_uvm_va_ent uvm_va_tbl[NVKVM_UVM_VA_MAX];
static uint32_t                uvm_va_used;
static pthread_mutex_t         uvm_va_lock = PTHREAD_MUTEX_INITIALIZER;

/* [base, base+length) with no wrap.  length == 0 is never a valid range. */
static bool uvm_va_sane(uint64_t base, uint64_t length)
{
	if (length == 0)
		return false;
	if (base + length < base)          /* u64 overflow */
		return false;
	if ((base | length) & 0xfffULL)    /* UVM works in pages */
		return false;
	return true;
}

/* Is there room to record one more range?  Checked BEFORE a range-creating
 * ioctl runs, so we never leave the driver holding a range the table cannot
 * describe (which would fail closed on every later use of it). */
static bool uvm_va_have_room(void)
{
	bool room;
	pthread_mutex_lock(&uvm_va_lock);
	room = uvm_va_used < NVKVM_UVM_VA_MAX;
	pthread_mutex_unlock(&uvm_va_lock);
	return room;
}

static bool uvm_va_add(uint32_t handle_id, uint64_t base, uint64_t length)
{
	bool ok = false;
	pthread_mutex_lock(&uvm_va_lock);
	/* Idempotent: the guest may legitimately re-create an identical range
	 * after freeing it, and the driver arbitrates that. */
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id == handle_id &&
		    uvm_va_tbl[i].base == base &&
		    uvm_va_tbl[i].length == length) {
			pthread_mutex_unlock(&uvm_va_lock);
			return true;
		}
	}
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id == 0) {
			uvm_va_tbl[i].handle_id = handle_id;
			uvm_va_tbl[i].base      = base;
			uvm_va_tbl[i].length    = length;
			uvm_va_used++;
			ok = true;
			break;
		}
	}
	pthread_mutex_unlock(&uvm_va_lock);
	return ok;
}

/* True iff [base, base+length) is fully inside ONE range recorded for this
 * handle.  Deliberately not a union-of-ranges test: UVM operations act on a
 * single va_range, and stitching adjacent entries together would let a guest
 * address across a boundary it never actually owns as one object.  The driver
 * agrees for the one command where it is checkable — uvm_api_validate_va_range()
 * wants ONE va_range matching exactly — so unioning would forward calls the
 * driver refuses anyway.  (A-1's iso_mmap_covers() DOES walk contiguous entries,
 * because there the chunks are one host-installed registration the host itself
 * split; here they are distinct driver objects.  The two are not the same case.)
 *
 * A zero or wrapping extent is not a range: it can never be "inside" anything.
 * The caller already refuses those via uvm_va_sane(), but a helper that answers
 * "yes, covered" for length 0 is a trap for the next caller. */
static bool uvm_va_covers(uint32_t handle_id, uint64_t base, uint64_t length)
{
	bool found = false;
	if (length == 0 || base + length < base)
		return false;
	pthread_mutex_lock(&uvm_va_lock);
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id != handle_id)
			continue;
		if (base >= uvm_va_tbl[i].base &&
		    base + length <= uvm_va_tbl[i].base + uvm_va_tbl[i].length) {
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&uvm_va_lock);
	return found;
}

/*
 * Length of the range this handle recorded starting EXACTLY at `base`, or 0 if
 * it has no such range.
 *
 * UVM_FREE names a range by its base and leaves length 0 -- the driver looks
 * the range up by start address -- so there is no length in the ioctl to
 * validate against.  Recovering our own recorded length lets the ownership
 * check and the drop below run unchanged, and keeps the U-6 property: a base
 * we never recorded for this handle returns 0 and is refused exactly as before.
 */
static uint64_t uvm_va_len_at(uint32_t handle_id, uint64_t base)
{
	uint64_t len = 0;
	pthread_mutex_lock(&uvm_va_lock);
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id == handle_id &&
		    uvm_va_tbl[i].base == base) {
			len = uvm_va_tbl[i].length;
			break;
		}
	}
	pthread_mutex_unlock(&uvm_va_lock);
	return len;
}

/* Drop every entry of this handle fully inside the freed range. */
static void uvm_va_drop(uint32_t handle_id, uint64_t base, uint64_t length)
{
	pthread_mutex_lock(&uvm_va_lock);
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id != handle_id)
			continue;
		if (uvm_va_tbl[i].base >= base &&
		    uvm_va_tbl[i].base + uvm_va_tbl[i].length <= base + length) {
			uvm_va_tbl[i].handle_id = 0;
			if (uvm_va_used)
				uvm_va_used--;
		}
	}
	pthread_mutex_unlock(&uvm_va_lock);
}

/* Forget everything about a handle: its va_space is gone (UVM_INITIALIZE on a
 * recycled handle_id, UVM_DEINITIALIZE, or the fd being closed).  Also called
 * from the close-handle handlers so a long-lived VM cannot leak the table.
 *
 * The exported nvkvm_uvm_va_purge_handle() (virtio_nvgpu.h) is a one-line
 * wrapper in nvkvm_isolate_handlers.c: the table itself is file-scope static
 * here, so the linkage stays where the rest of the handlers are. */
static void uvm_va_purge_handle(uint32_t handle_id)
{
	if (handle_id == 0)
		return;
	pthread_mutex_lock(&uvm_va_lock);
	for (uint32_t i = 0; i < NVKVM_UVM_VA_MAX; i++) {
		if (uvm_va_tbl[i].handle_id == handle_id) {
			uvm_va_tbl[i].handle_id = 0;
			if (uvm_va_used)
				uvm_va_used--;
		}
	}
	pthread_mutex_unlock(&uvm_va_lock);
}

/*
 * Is a U-6 VA-ownership refusal of this command a security event?
 *
 * USE and FREE refusals block an operation the guest asked for: either the
 * guest is confused or it is probing, and both are worth an operator's
 * attention.  A QUERY refusal is the answer to a question — the driver's own
 * answer for the same range would be NV_ERR_INVALID_ADDRESS — so it belongs in
 * a diagnostic line, not in the DENY channel that exists to make probes
 * visible.  CREATE/NONE never reach the ownership check.
 */
static bool nvkvm_uvm_refusal_is_security(uint8_t va_mode)
{
	return va_mode != NVKVM_UVM_VA_QUERY;
}

/* Does this command's VA range have to be owned before it is forwarded?
 * Every mode that names a range it did not create: USE, FREE and QUERY. */
static bool nvkvm_uvm_mode_needs_ownership(uint8_t va_mode)
{
	return va_mode == NVKVM_UVM_VA_USE ||
	       va_mode == NVKVM_UVM_VA_FREE ||
	       va_mode == NVKVM_UVM_VA_QUERY;
}

#endif /* NVKVM_UVM_VA_H */
