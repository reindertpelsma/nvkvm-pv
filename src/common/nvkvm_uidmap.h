/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_uidmap.h — what uids this process may actually become.
 *
 * Both the broker and the isolate hand themselves to an unprivileged uid, and
 * both used to pick one from a HARDCODED offset: 500000..504095 for the
 * isolate window, a random 600000+ for the broker.  That is correct on an
 * ordinary host and wrong inside a user namespace.
 *
 * With `dockerd --userns-remap`, or under sysbox-runc, the container is given a
 * mapped range -- commonly 65536 uids, so 0..65535 inside.  A setuid() to a uid
 * outside the map fails with EINVAL: not a permission problem, an UNMAPPED uid.
 * Hardcoded offsets therefore fail exactly on the deployments that went to the
 * trouble of enabling user namespaces.
 *
 * The kernel already publishes the answer, so ask it rather than guess.
 * /proc/self/uid_map is one line per range:
 *
 *     inside   outside      count
 *     0        0            4294967295     <- identity: not in a userns
 *     0        100000       65536          <- remapped: only 0..65535 usable
 *
 * We care only about the INSIDE numbers: those are the uids setuid() accepts.
 */
#ifndef NVKVM_UIDMAP_H
#define NVKVM_UIDMAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct nvkvm_uidmap {
    uint32_t lo;         /* lowest usable uid inside this namespace  */
    uint32_t hi;         /* highest usable uid, inclusive            */
    bool     identity;   /* true when not in a user namespace at all */
};

/*
 * Read the map.  Falls back to the identity range when /proc is unreadable --
 * the pre-existing behaviour, which is right for a host with no userns and no
 * worse than what the hardcoded offsets did.
 *
 * Only the LARGEST range is reported.  A multi-range map is legal but rare, and
 * picking the largest keeps a single contiguous window, which is what both
 * callers actually want; a caller needing more can parse it itself.
 */
static inline void nvkvm_uidmap_get(struct nvkvm_uidmap *out)
{
    FILE *f;
    unsigned long inside, outside, count;
    unsigned long best_lo = 0, best_count = 0;
    int n_ranges = 0;

    /*
     * Capped at 2^31-2, not at (uid_t)-1.
     *
     * uids at or above 2^31 are legal and almost never exercised: plenty of
     * userspace still moves a uid through an int somewhere, and 0xffffffff is
     * the "invalid uid" sentinel besides.  Nothing here needs a uid that high,
     * and a two-billion-wide range is not a meaningful constraint, so stay in
     * the half that is actually tested.
     */
    out->lo = 0;
    out->hi = 0x7ffffffeu;
    out->identity = true;

    f = fopen("/proc/self/uid_map", "re");
    if (!f) {
        return;
    }
    while (fscanf(f, "%lu %lu %lu", &inside, &outside, &count) == 3) {
        n_ranges++;
        if (count > best_count) {
            best_count = count;
            best_lo    = inside;
        }
    }
    fclose(f);

    if (best_count == 0) {
        return;                 /* unparseable: keep the identity assumption */
    }
    /*
     * The identity map is `0 0 4294967295`.  Anything else means a namespace
     * with a real bound, even when it happens to start at 0.
     */
    out->identity = (n_ranges == 1 && best_lo == 0 &&
                     best_count >= 0xfffffffeu);
    out->lo = (uint32_t)best_lo;
    {
        uint64_t hi = best_lo + best_count - 1;

        out->hi = hi > 0x7ffffffeu ? 0x7ffffffeu : (uint32_t)hi;
    }
}

/* Does [base, base+slots) fit inside the usable range, avoiding uid 0? */
static inline bool nvkvm_uidmap_fits(const struct nvkvm_uidmap *m,
                                     uint32_t base, uint32_t slots)
{
    if (slots == 0 || base == 0) {
        return false;
    }
    if (base < m->lo || base > m->hi) {
        return false;
    }
    return (uint64_t)base + slots - 1 <= (uint64_t)m->hi;
}

/*
 * Where to put a `slots`-wide window that fits.  Prefers the TOP of the usable
 * range: low uids are where real accounts, system users and subuid allocations
 * live, so the top is the least likely to collide with something that matters.
 * Returns 0 when it cannot fit, which no caller should treat as a uid.
 */
static inline uint32_t nvkvm_uidmap_place(const struct nvkvm_uidmap *m,
                                          uint32_t slots)
{
    uint64_t base;

    if (slots == 0 || (uint64_t)m->hi - m->lo + 1 < slots) {
        return 0;
    }
    base = (uint64_t)m->hi - slots + 1;
    if (base <= m->lo) {
        base = m->lo ? m->lo : 1;   /* never hand back uid 0 */
    }
    return (uint32_t)base;
}

#endif /* NVKVM_UIDMAP_H */
