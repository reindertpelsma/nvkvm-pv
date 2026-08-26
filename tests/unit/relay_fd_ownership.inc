/* NVKVM_RELAY_FD_OWNERSHIP_BEGIN -- extracted by test_relay_wiring. */
static void relay_set_fd_handlers(NvkvmRelay *r, bool writable)
{
    assert(bql_locked());
    assert(r->sock >= 0);
    qemu_set_fd_handler(r->sock, relay_readable,
                        writable ? relay_writable : NULL, r);
}

/*
 * The main loop owns registration and closure.  Removing the handler before
 * close means there is no deferred raw descriptor that can be reused between
 * scheduling and teardown.  aio_set_fd_handler(), which backs this API, is
 * explicitly safe when called while the handler list is being walked.
 */
static void relay_close_connection(NvkvmRelay *r)
{
    int fd;

    assert(bql_locked());
    if (r->handshake_deadline) {
        timer_del(r->handshake_deadline);
    }
    if (r->sock < 0) {
        r->conn_state = RELAY_CONN_DOWN;
        relay_connection_state_reset(r);
        return;
    }
    fd = r->sock;
    r->sock = -1;
    r->conn_state = RELAY_CONN_DOWN;
    qemu_set_fd_handler(fd, NULL, NULL, NULL);
    close(fd);
    relay_connection_state_reset(r);
}
/* NVKVM_RELAY_FD_OWNERSHIP_END */
