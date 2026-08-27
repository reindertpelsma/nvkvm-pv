/*
 * test_relay_bufmap.c — the release path's one translation.
 *
 * The broker names a scanout buffer by its host dma-buf INODE; the guest names
 * the same buffer by (isolate, stub GEM handle).  Present backpressure exists
 * only because a release can be carried from one name to the other, so the
 * table that does it is worth pinning on its own: a lookup that returns the
 * WRONG pair would tell the guest a buffer is free while the host is still
 * reading it, which is precisely the corruption the feature is meant to stop.
 *
 * The production helpers are extracted from nvkvm_display_relay.c between the
 * NVKVM_RELAY_BUFMAP markers.  Losing the markers makes this fail to compile
 * rather than silently test a copy.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "relay_bufmap.inc"

static int run, passed;

static void chk(const char *name, bool good)
{
    run++;
    if (good) {
        passed++;
        printf("[ PASS ] %s\n", name);
    } else {
        printf("[ FAIL ] %s\n", name);
    }
}

int main(void)
{
    RelayBufMap m;
    uint32_t iso = 0xdead, gem = 0xbeef;
    unsigned i;

    relay_bufmap_reset(&m);

    chk("an empty table translates nothing",
        !relay_bufmap_lookup(&m, 1234, &iso, &gem));

    relay_bufmap_note(&m, 1234, 7, 0x11);
    iso = gem = 0;
    chk("a noted buffer translates back to the pair it was noted with",
        relay_bufmap_lookup(&m, 1234, &iso, &gem) && iso == 7 && gem == 0x11);

    iso = gem = 0;
    chk("an id that was never noted still translates to nothing",
        !relay_bufmap_lookup(&m, 4321, &iso, &gem));

    /*
     * A lookup must NOT consume the entry.  The same two or three bos are
     * presented and released over and over; forgetting one on first use would
     * lose the translation every other frame.
     */
    iso = gem = 0;
    chk("a translation survives being used",
        relay_bufmap_lookup(&m, 1234, &iso, &gem) && iso == 7 && gem == 0x11);

    /*
     * INODE REUSE.  The host kernel may hand the same inode to a different
     * dma-buf once the first is gone.  Every present passes through note()
     * before its ATTACH, so re-noting must REPLACE, never add a second entry
     * that an unlucky scan order could return instead.
     */
    relay_bufmap_note(&m, 1234, 9, 0x22);
    iso = gem = 0;
    chk("re-noting an id replaces the pair rather than shadowing it",
        relay_bufmap_lookup(&m, 1234, &iso, &gem) && iso == 9 && gem == 0x22);

    /* Nothing nameable on one side is not a translation worth storing. */
    relay_bufmap_note(&m, 0, 1, 0x33);
    relay_bufmap_note(&m, 555, 1, 0);
    iso = gem = 0;
    chk("a zero buffer id is not recorded",
        !relay_bufmap_lookup(&m, 0, &iso, &gem));
    chk("a zero GEM handle is not recorded",
        !relay_bufmap_lookup(&m, 555, &iso, &gem));

    /*
     * OVERFLOW IS LEAST-RECENTLY-NOTED, and that is the property that matters:
     * the buffers being cycled right now are re-noted every frame, so they are
     * exactly the ones that must survive an eviction storm.
     */
    relay_bufmap_reset(&m);
    for (i = 0; i < RELAY_BUFMAP_SLOTS; i++) {
        relay_bufmap_note(&m, 1000 + i, 1, 0x100 + i);
    }
    chk("a full table holds every id it was given",
        relay_bufmap_lookup(&m, 1000, &iso, &gem) &&
        relay_bufmap_lookup(&m, 1000 + RELAY_BUFMAP_SLOTS - 1, &iso, &gem));

    /* Touch the oldest so it is no longer the oldest, then overflow by one. */
    relay_bufmap_note(&m, 1000, 1, 0x100);
    relay_bufmap_note(&m, 2000, 2, 0x200);
    iso = gem = 0;
    chk("the new id is recorded",
        relay_bufmap_lookup(&m, 2000, &iso, &gem) && iso == 2 && gem == 0x200);
    chk("a re-noted id is not the one evicted",
        relay_bufmap_lookup(&m, 1000, &iso, &gem) && gem == 0x100);
    chk("the least recently noted id is the one that goes",
        !relay_bufmap_lookup(&m, 1001, &iso, &gem));

    /* Reset must leave nothing behind: it runs at relay construction. */
    relay_bufmap_reset(&m);
    chk("reset forgets every translation",
        !relay_bufmap_lookup(&m, 1000, &iso, &gem) &&
        !relay_bufmap_lookup(&m, 2000, &iso, &gem));

    printf("%d/%d tests passed\n", passed, run);
    return passed == run ? 0 : 1;
}
