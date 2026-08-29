/* NVKVM_RELAY_WRITABLE_WIRING_BEGIN -- extracted by test_relay_wiring. */
static void relay_writable(void *opaque)
{
    NvkvmRelay *r = opaque;
    int err = 0;
    socklen_t errlen = sizeof(err);

    assert(bql_locked());
    if (r->sock < 0) {
        return;
    }
    if (r->conn_state == RELAY_CONN_CONNECTING) {
        if (getsockopt(r->sock, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
            err = errno;
        }
        if (err != 0) {
            relay_attempt_failed(r, strerror(err));
            return;
        }
        r->conn_state = RELAY_CONN_HELLO;
        relay_set_fd_handlers(r, false);
        /* HELLO may already be queued; consume it without another poll turn. */
        relay_readable(r);
        return;
    }
    if (r->conn_state >= RELAY_CONN_SYNC_CAPS &&
        r->conn_state <= RELAY_CONN_SYNC_COMMIT) {
        relay_sync_flush(r);
        return;
    }
    if (r->conn_state == RELAY_CONN_ACTIVE) {
        /* The frame first: it is on a deadline and the clipboard is not. */
        relay_frame_flush(r);
        relay_clip_flush(r);
    }
}
/* NVKVM_RELAY_WRITABLE_WIRING_END */
