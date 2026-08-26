/*
 * Production relay wiring, without the QEMU UI dependency graph.
 *
 * The source fragments below contain the real fd-handler registration,
 * unregister/close/reset sequence, and clipboard EAGAIN routing.  The stubs
 * record those external effects.  This complements the pure helper tests: a
 * queue cursor that works in isolation is not useful unless production arms
 * POLLOUT, and a reset helper is not useful unless descriptor teardown calls
 * it before the next connection generation.
 */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../src/common/nvkvm_broker_proto.h"

#define RELAY_CLIP_MAX_CMDS \
    ((NVKVM_BROKER_CLIP_MAX_BYTES + NVKVM_BROKER_CLIP_CMD_BYTES - 1u) / \
     NVKVM_BROKER_CLIP_CMD_BYTES)

typedef enum RelayConnState {
    RELAY_CONN_DOWN,
    RELAY_CONN_CONNECTING,
    RELAY_CONN_HELLO,
    RELAY_CONN_SYNC_CAPS,
    RELAY_CONN_SYNC_WINDOW,
    RELAY_CONN_SYNC_ATTACH,
    RELAY_CONN_SYNC_COMMIT,
    RELAY_CONN_ACTIVE,
} RelayConnState;

typedef struct QEMUTimer {
    unsigned deleted;
    unsigned modified;
    int64_t expires;
} QEMUTimer;

typedef struct Error {
    int unused;
} Error;

#define QEMU_CLOCK_REALTIME 0

typedef struct NvkvmRelay {
    int sock;
    RelayConnState conn_state;
    bool enabled;
    uint32_t caps;
    uint32_t last_w, last_h;
    uint8_t rxbuf[NVKVM_BROKER_PKT_SIZE];
    size_t rxlen;
    char clip_in[NVKVM_BROKER_CLIP_MAX_BYTES + 1];
    unsigned clip_in_len;
    unsigned clip_in_chunks;
    bool clip_in_bad;
    struct nvkvm_broker_clip_cmd clip_out[RELAY_CLIP_MAX_CMDS];
    size_t clip_out_count;
    size_t clip_out_next;
    int last_fd;
    uint32_t last_bw, last_bh, last_stride, last_fourcc;
    uint64_t last_modifier;
    QEMUTimer *handshake_deadline;
} NvkvmRelay;

typedef void IOHandler(void *opaque);

static void relay_readable(void *opaque);
static void relay_writable(void *opaque);
static int relay_send(NvkvmRelay *r, const struct nvkvm_broker_cmd *cmd,
                      int fd);
static void relay_drop(NvkvmRelay *r, const char *why);
static void relay_attempt_failed(NvkvmRelay *r, const char *why);
static void relay_sync_flush(NvkvmRelay *r);

static int watched_fd = -2;
static IOHandler *watched_read;
static IOHandler *watched_write;
static void *watched_opaque;
static unsigned watch_calls;
static unsigned drop_calls;
static unsigned send_calls;
static unsigned accepted_calls;
static unsigned block_call = (unsigned)-1;
static bool blocked_once;
static int fatal_send_rc;

static bool bql_locked(void)
{
    return true;
}

static void qemu_set_fd_handler(int fd, IOHandler *read, IOHandler *write,
                                void *opaque)
{
    watched_fd = fd;
    watched_read = read;
    watched_write = write;
    watched_opaque = opaque;
    watch_calls++;
}

static void timer_del(QEMUTimer *timer)
{
    timer->deleted++;
}

static int64_t qemu_clock_get_ms(int clock)
{
    (void)clock;
    return 1000;
}

static void timer_mod(QEMUTimer *timer, int64_t expires)
{
    timer->modified++;
    timer->expires = expires;
}

static void error_setg(Error **errp, const char *fmt, ...)
{
    static Error error;

    (void)fmt;
    *errp = &error;
}

static void error_setg_errno(Error **errp, int error_no, const char *fmt, ...)
{
    (void)error_no;
    error_setg(errp, fmt);
}

#include "relay_state_helpers.inc"
#include "relay_clip_batch.inc"
#include "relay_fd_ownership.inc"
#include "relay_clip_wiring.inc"
#include "relay_writable_wiring.inc"
#include "relay_connect_wiring.inc"

static void relay_readable(void *opaque)
{
    (void)opaque;
}

static int relay_send(NvkvmRelay *r, const struct nvkvm_broker_cmd *cmd,
                      int fd)
{
    (void)r;
    (void)cmd;
    (void)fd;
    if (fatal_send_rc) {
        return fatal_send_rc;
    }
    if (!blocked_once && send_calls == block_call) {
        blocked_once = true;
        send_calls++;
        return -EAGAIN;
    }
    send_calls++;
    accepted_calls++;
    return 0;
}

static void relay_drop(NvkvmRelay *r, const char *why)
{
    (void)r;
    (void)why;
    drop_calls++;
}

static void relay_attempt_failed(NvkvmRelay *r, const char *why)
{
    (void)r;
    (void)why;
    assert(false);
}

static void relay_sync_flush(NvkvmRelay *r)
{
    (void)r;
    assert(false);
}

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
    QEMUTimer deadline = { 0 };
    NvkvmRelay r = {
        .sock = 91,
        .conn_state = RELAY_CONN_ACTIVE,
        .enabled = true,
        .last_fd = -1,
        .handshake_deadline = &deadline,
    };
    unsigned char text[NVKVM_BROKER_CLIP_CMD_BYTES + 1];
    QEMUTimer connect_deadline = { 0 };
    NvkvmRelay connecting = {
        .sock = -1,
        .conn_state = RELAY_CONN_DOWN,
        .enabled = true,
        .last_fd = -1,
        .handshake_deadline = &connect_deadline,
    };
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    char socket_path[sizeof(sa.sun_path)];
    Error *connect_error = NULL;
    int listener = -1, accepted = -1;
    int p[2] = { -1, -1 };
    int closing;
    size_t built;

    relay_set_fd_handlers(&r, true);
    chk("POLLOUT registration keeps the production read callback",
        watched_fd == r.sock && watched_read == relay_readable);
    chk("POLLOUT registration installs the production write callback",
        watched_write == relay_writable && watched_opaque == &r);

    snprintf(socket_path, sizeof(socket_path),
             "/tmp/nvkvm-relay-wiring-%ld.sock", (long)getpid());
    strncpy(sa.sun_path, socket_path, sizeof(sa.sun_path) - 1);
    unlink(socket_path);
    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    chk("nonblocking-handshake listener opens", listener >= 0);
    chk("nonblocking-handshake listener binds and listens",
        listener >= 0 &&
        bind(listener, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
        listen(listener, 1) == 0);
    chk("production connect starts without waiting for HELLO",
        relay_start_connect(&connecting, socket_path, &connect_error) == 0 &&
        connect_error == NULL);
    chk("production connect socket is O_NONBLOCK",
        connecting.sock >= 0 &&
        (fcntl(connecting.sock, F_GETFL) & O_NONBLOCK) != 0);
    chk("production connect registers readiness on its live descriptor",
        watched_fd == connecting.sock && watched_opaque == &connecting &&
        (watched_read == relay_readable || watched_read == relay_writable));
    chk("production connect arms the bounded handshake deadline",
        connect_deadline.modified == 1 && connect_deadline.expires == 3000);
    accepted = listener >= 0 ? accept(listener, NULL, NULL) : -1;
    chk("nonblocking connection reaches the listening peer", accepted >= 0);
    if (connecting.conn_state == RELAY_CONN_CONNECTING) {
        relay_writable(&connecting);
    }
    chk("connect readiness advances to nonblocking HELLO receive",
        connecting.conn_state == RELAY_CONN_HELLO &&
        watched_read == relay_readable && watched_write == NULL);
    relay_close_connection(&connecting);
    if (accepted >= 0) {
        close(accepted);
    }
    if (listener >= 0) {
        close(listener);
    }
    unlink(socket_path);

    memset(text, 0x5a, sizeof(text));
    built = relay_clip_batch_build(r.clip_out, RELAY_CLIP_MAX_CMDS,
                                   (const char *)text, sizeof(text));
    r.clip_out_count = built;
    r.clip_out_next = 0;
    block_call = 1;
    blocked_once = false;
    send_calls = accepted_calls = 0;
    relay_clip_flush(&r);
    chk("clipboard EAGAIN preserves the first unsent production cursor",
        built == 2 && r.clip_out_next == 1 && accepted_calls == 1);
    chk("clipboard EAGAIN arms POLLOUT on the live fd",
        watched_fd == r.sock && watched_read == relay_readable &&
        watched_write == relay_writable && watched_opaque == &r);

    block_call = (unsigned)-1;
    relay_writable(&r);
    chk("writable retry drains and clears the production queue",
        r.clip_out_count == 0 && r.clip_out_next == 0 &&
        accepted_calls == built);
    chk("a drained queue removes POLLOUT but keeps POLLIN",
        watched_read == relay_readable && watched_write == NULL);

    built = relay_clip_batch_build(r.clip_out, RELAY_CLIP_MAX_CMDS,
                                   (const char *)text, 1);
    r.clip_out_count = built;
    r.clip_out_next = 0;
    fatal_send_rc = -EPIPE;
    relay_clip_flush(&r);
    chk("fatal clipboard send reaches the connection drop owner",
        drop_calls == 1);
    fatal_send_rc = 0;

    chk("teardown test pipe opens", pipe(p) == 0);
    closing = p[0];
    r.sock = closing;
    r.conn_state = RELAY_CONN_ACTIVE;
    r.caps = 0xffffffffu;
    r.rxlen = 7;
    r.clip_in_len = 9;
    r.clip_in_chunks = 2;
    r.clip_in_bad = true;
    r.clip_out_count = 2;
    r.clip_out_next = 1;
    relay_close_connection(&r);
    chk("teardown unregisters the exact descriptor before reuse",
        watched_fd == closing && watched_read == NULL &&
        watched_write == NULL && watched_opaque == NULL);
    chk("teardown closes the owned descriptor",
        !fd_open(closing) && errno == EBADF);
    chk("teardown publishes DOWN with no live descriptor",
        r.sock == -1 && r.conn_state == RELAY_CONN_DOWN);
    chk("teardown resets receive and both clipboard generations",
        r.caps == 0 && r.rxlen == 0 && r.clip_in_len == 0 &&
        r.clip_in_chunks == 0 && !r.clip_in_bad &&
        r.clip_out_count == 0 && r.clip_out_next == 0);
    chk("teardown cancels the handshake deadline", deadline.deleted == 1);
    close(p[1]);

    chk("the production wiring exercised fd-handler updates",
        watch_calls >= 6);

    printf("%d/%d tests passed\n", passed, run);
    return passed == run ? 0 : 1;
}
