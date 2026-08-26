/* NVKVM_RELAY_CONNECT_WIRING_BEGIN -- extracted by test_relay_wiring. */
static int relay_start_connect(NvkvmRelay *r, const char *path, Error **errp)
{
    struct sockaddr_un sa;
    bool connecting = false;
    int fd;

    assert(bql_locked());
    assert(r->sock < 0 && r->conn_state == RELAY_CONN_DOWN);
    if (strlen(path) >= sizeof(sa.sun_path)) {
        error_setg(errp, "nvkvm-broker: socket path too long");
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        error_setg_errno(errp, errno, "nvkvm-broker: socket");
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (errno == EINPROGRESS || errno == EALREADY || errno == EAGAIN) {
            connecting = true;
        } else {
            error_setg_errno(errp, errno,
                             "nvkvm-broker: cannot connect to %s. Is the "
                             "display broker running, and is the socket "
                             "bind-mounted into this container?", path);
            close(fd);
            return -1;
        }
    }

    /*
     * No connect(), HELLO read, CAPS send, or replay operation may wait under
     * the BQL.  Readiness advances this state machine; the timer bounds a peer
     * that accepts and then stops at any handshake phase.
     */
    relay_connection_state_reset(r);
    r->sock = fd;
    r->conn_state = connecting ? RELAY_CONN_CONNECTING : RELAY_CONN_HELLO;
    qemu_set_fd_handler(fd,
                        connecting ? relay_writable : relay_readable,
                        connecting ? relay_writable : NULL, r);
    timer_mod(r->handshake_deadline,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
    return 0;
}
/* NVKVM_RELAY_CONNECT_WIRING_END */
