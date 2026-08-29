/* NVKVM_RELAY_CLIP_WIRING_BEGIN -- extracted by test_relay_wiring. */
static int relay_clip_send_one(void *opaque,
                               const struct nvkvm_broker_clip_cmd *cmd)
{
    NvkvmRelay *r = opaque;

    return relay_send(r, (const struct nvkvm_broker_cmd *)cmd, -1);
}

static void relay_clip_flush(NvkvmRelay *r)
{
    int rc;

    assert(bql_locked());
    if (r->conn_state != RELAY_CONN_ACTIVE) {
        return;
    }
    if (r->clip_out_next >= r->clip_out_count) {
        r->clip_out_count = 0;
        r->clip_out_next = 0;
        relay_set_fd_handlers(r, relay_want_writable(r));
        return;
    }
    rc = relay_clip_batch_flush(r->clip_out, r->clip_out_count,
                                &r->clip_out_next, relay_clip_send_one, r);
    if (rc == 0) {
        r->clip_out_count = 0;
        r->clip_out_next = 0;
        relay_set_fd_handlers(r, relay_want_writable(r));
    } else if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
        relay_set_fd_handlers(r, true);
    } else {
        relay_drop(r, rc == -EPROTO
                          ? "short write in a clipboard command"
                          : "the broker socket failed during clipboard send");
    }
}
/* NVKVM_RELAY_CLIP_WIRING_END */
