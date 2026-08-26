/* NVKVM_RELAY_STATE_HELPERS_BEGIN -- extracted by test_relay_state. */
/*
 * Everything below is knowledge held about ONE broker connection.  A stream
 * disconnect is both the packet-framing boundary and the clipboard transaction
 * boundary, so none of it may survive into a new broker process.  The retained
 * frame is deliberately absent: it belongs to the VM and is what the next
 * connection must replay.
 */
static void relay_connection_state_reset(NvkvmRelay *r)
{
    r->caps = 0;
    r->last_w = 0;
    r->last_h = 0;
    r->rxlen = 0;
    r->clip_in_len = 0;
    r->clip_in_chunks = 0;
    r->clip_in_bad = false;
    r->clip_out_count = 0;
    r->clip_out_next = 0;
}

/*
 * Consume and retain a frame whenever broker mode is enabled, independently
 * of whether a broker is connected at this instant.  False leaves ownership
 * with the caller; true transfers ownership of dmabuf_fd to r.  Like every
 * connection-state operation, this runs under the BQL; the relay has no
 * worker-side socket owner.
 */
static bool relay_frame_retain(NvkvmRelay *r, int dmabuf_fd,
                               uint32_t width, uint32_t height,
                               uint32_t stride, uint32_t fourcc,
                               uint64_t modifier)
{
    if (!r || !r->enabled || dmabuf_fd < 0) {
        return false;
    }
    if (r->last_fd >= 0) {
        close(r->last_fd);
    }
    r->last_fd       = dmabuf_fd;
    r->last_bw       = width;
    r->last_bh       = height;
    r->last_stride   = stride;
    r->last_fourcc   = fourcc;
    r->last_modifier = modifier;
    return true;
}
/* NVKVM_RELAY_STATE_HELPERS_END */
