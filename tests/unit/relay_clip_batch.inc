/* NVKVM_RELAY_CLIP_BATCH_BEGIN -- extracted by test_relay_clip. */
/*
 * Build one complete clipboard transaction in memory before writing any of
 * it.  Separate nonblocking sends can accept a prefix and then return EAGAIN;
 * abandoning that prefix makes the next transfer's LAST commit concatenated
 * text.  The bounded batch and its cursor stay live until POLLOUT drains the
 * remainder, and no newer clipboard transaction begins in between.
 */
static size_t relay_clip_batch_build(struct nvkvm_broker_clip_cmd *out,
                                     size_t out_count,
                                     const char *text, size_t len)
{
    size_t ncmds, off = 0;
    uint32_t chunk = 0;

    if (!out || !text || !len || len > NVKVM_BROKER_CLIP_MAX_BYTES) {
        return 0;
    }
    ncmds = (len + NVKVM_BROKER_CLIP_CMD_BYTES - 1) /
            NVKVM_BROKER_CLIP_CMD_BYTES;
    if (out_count < ncmds) {
        return 0;
    }

    memset(out, 0, ncmds * sizeof(*out));
    while (off < len) {
        struct nvkvm_broker_clip_cmd *cc = &out[chunk];
        size_t n = len - off;

        if (n > NVKVM_BROKER_CLIP_CMD_BYTES) {
            n = NVKVM_BROKER_CLIP_CMD_BYTES;
        }
        cc->type = NVKVM_BROKER_CMD_CLIPBOARD;
        cc->chunk = chunk++;
        cc->info = (uint8_t)n;
        if (off + n == len) {
            cc->info |= NVKVM_BROKER_CLIP_LAST;
        }
        memcpy(cc->data, text + off, n);
        off += n;
    }
    return ncmds;
}

typedef int (*RelayClipSendOne)(void *opaque,
                                const struct nvkvm_broker_clip_cmd *cmd);

/*
 * Drain whole 40-byte commands from an already-built transaction.  EAGAIN
 * leaves `next` at the first unsent chunk, so POLLOUT resumes the SAME
 * transaction; a later clipboard update cannot append or interleave chunks.
 */
static int relay_clip_batch_flush(const struct nvkvm_broker_clip_cmd *cmds,
                                  size_t ncmds, size_t *next,
                                  RelayClipSendOne send_one, void *opaque)
{
    int rc;

    if (!cmds || !next || !send_one || *next > ncmds) {
        return -EINVAL;
    }
    while (*next < ncmds) {
        rc = send_one(opaque, &cmds[*next]);
        if (rc != 0) {
            return rc;
        }
        (*next)++;
    }
    return 0;
}
/* NVKVM_RELAY_CLIP_BATCH_END */
