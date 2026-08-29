/*
 * test_relay_state.c — RR-06/RR-07 relay generation state.
 *
 * The production helpers are extracted from nvkvm_display_relay.c.  A full
 * relay unit would require QEMU's display and clipboard stacks; these cases
 * instead pin the two ownership rules that regressed: a disconnected relay
 * still consumes the newest frame, and receive/clipboard prefixes never cross
 * a socket generation.
 */
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>

/*
 * The relay's frame-retention state, mirrored for the extracted helpers.
 *
 * These fields arrived with the EAGAIN-redelivery work (2026-08-28) and were
 * never added here, so this suite STOPPED BUILDING FROM CLEAN and nobody saw it:
 * relay_state_helpers.inc is a tracked, generated file, so `make` refreshed it on
 * disk while a stale object satisfied the link. A suite that does not build is
 * not a smaller test run, it is a hole -- which is what the Makefile says, and
 * what happened anyway.
 */
typedef enum {
    RELAY_PEND_NONE = 0,
    RELAY_PEND_ATTACH,
    RELAY_PEND_COMMIT,
} RelayPend;

/* QEMU's timer API, reduced to what the extracted helpers touch. The tests do
 * not exercise firing; they exercise that arm/clear happen on the right edges. */
typedef struct { bool armed; int64_t at; } QEMUTimer;
static void timer_del(QEMUTimer *t) { if (t) t->armed = false; }
static void timer_mod(QEMUTimer *t, int64_t when) { if (t) { t->armed = true; t->at = when; } }



typedef struct NvkvmRelay {
    int sock;
    bool enabled;
    uint32_t caps;
    uint32_t last_w, last_h;
    uint8_t rxbuf[24];
    size_t rxlen;
    char clip_in[16385];
    unsigned clip_in_len;
    unsigned clip_in_chunks;
    bool clip_in_bad;
    size_t clip_out_count;
    size_t clip_out_next;
    int last_fd;
    uint32_t last_bw, last_bh, last_stride, last_fourcc;
    uint64_t last_modifier;
    /* frame retention (EAGAIN redelivery) */
    RelayPend pend;
    QEMUTimer *pend_timer;
    bool     pend_counted;
    bool     last_shm;
    int      conn_state;
    uint64_t n_sent;
    uint64_t n_recovered;
} NvkvmRelay;

/*
 * What the retention state machine reaches for, and nothing more.
 *
 * relay_frame_flush() drives the socket, so it was moved BELOW the extraction
 * markers -- it is not a state helper, and sweeping it in is what grew this
 * region from 47 lines to 183 and broke the suite. Only its declaration is
 * needed here, because relay_pend_deadline() calls it.
 */
static void relay_frame_flush(NvkvmRelay *r);

/* The BQL is held by every real caller; the assert exists to keep it that way. */
static bool bql_locked(void) { return true; }

/* Deadline arithmetic only: the tests assert WHEN the timer is armed and
 * cleared, never that it fires. */
#define QEMU_CLOCK_REALTIME 0
static int64_t mock_now_ms = 1000;
static int64_t qemu_clock_get_ms(int clock) { (void)clock; return mock_now_ms; }

/* Mirrors the constant in the relay. Kept in sync by test_relay_wiring, which
 * greps the source for it -- a copy that drifts fails there, not silently here. */
#define RELAY_PEND_DEADLINE_MS 50

/* Toggling POLLOUT is a state transition, so the suite counts it. */
static int mock_fd_handler_updates;
static void relay_set_fd_handlers(NvkvmRelay *r, bool want_write)
{ (void)r; (void)want_write; mock_fd_handler_updates++; }

#include "relay_state_helpers.inc"

/* Satisfies the declaration above; the suite counts transitions, not bytes. */
static int mock_flushes;
static void relay_frame_flush(NvkvmRelay *r) { (void)r; mock_flushes++; }

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

static bool fd_open(int fd)
{
    errno = 0;
    return fcntl(fd, F_GETFD) >= 0;
}

int main(void)
{
    NvkvmRelay r = {
        .sock = -1,
        .enabled = true,
        .last_fd = -1,
    };
    int first[2] = { -1, -1 };
    int second[2] = { -1, -1 };
    int disabled[2] = { -1, -1 };
    int old_fd;

    chk("first retained-frame pipe opens", pipe(first) == 0);
    chk("a disconnected enabled relay consumes a valid frame",
        relay_frame_retain(&r, first[0], 1920, 1080, 7680,
                           0x34325258u, 0x0300000000606014ULL));
    chk("disconnected retention does not invent a connection",
        r.sock == -1);
    chk("retained frame fd stays open", r.last_fd == first[0] &&
        fd_open(r.last_fd));
    chk("retained frame metadata is exact",
        r.last_bw == 1920 && r.last_bh == 1080 &&
        r.last_stride == 7680 && r.last_fourcc == 0x34325258u &&
        r.last_modifier == 0x0300000000606014ULL);
    close(first[1]);

    old_fd = r.last_fd;
    chk("replacement-frame pipe opens", pipe(second) == 0);
    chk("a newer disconnected frame replaces the retained one",
        relay_frame_retain(&r, second[0], 1280, 720, 5120,
                           0x34325241u, 0));
    chk("replacing the retained frame closes the stale fd",
        !fd_open(old_fd) && errno == EBADF);
    chk("the replacement remains owned and open",
        r.last_fd == second[0] && fd_open(r.last_fd));
    close(second[1]);

    chk("disabled-relay pipe opens", pipe(disabled) == 0);
    r.enabled = false;
    chk("a disabled relay rejects a frame",
        !relay_frame_retain(&r, disabled[0], 1, 1, 4, 0, 0));
    chk("a rejected frame remains caller-owned",
        fd_open(disabled[0]));
    close(disabled[0]);
    close(disabled[1]);
    r.enabled = true;

    r.caps = 0xffffffffu;
    r.last_w = 1280;
    r.last_h = 720;
    r.rxlen = 13;
    r.clip_in_len = 41;
    r.clip_in_chunks = 3;
    r.clip_in_bad = true;
    r.clip_out_count = 7;
    r.clip_out_next = 2;
    relay_connection_state_reset(&r);
    chk("disconnect clears broker capabilities and sent geometry",
        r.caps == 0 && r.last_w == 0 && r.last_h == 0);
    chk("disconnect clears a partial event packet",
        r.rxlen == 0);
    chk("disconnect aborts an inbound clipboard transaction",
        r.clip_in_len == 0 && r.clip_in_chunks == 0 && !r.clip_in_bad);
    chk("disconnect aborts an outbound clipboard transaction",
        r.clip_out_count == 0 && r.clip_out_next == 0);
    chk("connection reset preserves the VM's retained frame",
        r.last_fd == second[0] && r.last_bw == 1280 && r.last_bh == 720 &&
        fd_open(r.last_fd));

    close(r.last_fd);
    printf("%d/%d tests passed\n", passed, run);
    return passed == run ? 0 : 1;
}
