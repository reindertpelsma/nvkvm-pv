/*
 * test_relay_clip.c — RR-08, build a clipboard transfer before sending it.
 *
 * The old relay issued one nonblocking send per chunk.  If chunk N reached
 * the broker and chunk N+1 hit EAGAIN, the prefix remained live and the next
 * transfer's LAST could commit both strings.  The production batch builder and
 * resumable flush cursor are extracted from nvkvm_display_relay.c at build
 * time, so this suite pins the actual queued transaction path rather than a
 * copy.
 */
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/common/nvkvm_broker_proto.h"
#include "relay_clip_batch.inc"

#define MAX_CMDS ((NVKVM_BROKER_CLIP_MAX_BYTES + \
                   NVKVM_BROKER_CLIP_CMD_BYTES - 1) / \
                  NVKVM_BROKER_CLIP_CMD_BYTES)

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

static unsigned nbytes(const struct nvkvm_broker_clip_cmd *c)
{
    return NVKVM_BROKER_CLIP_NBYTES(c->info);
}

struct fake_sink {
    struct nvkvm_broker_clip_cmd accepted[MAX_CMDS];
    size_t naccepted;
    size_t block_at;
    bool blocked_once;
    int fail_rc;
};

static int fake_send_one(void *opaque,
                         const struct nvkvm_broker_clip_cmd *cmd)
{
    struct fake_sink *s = opaque;

    if (s->fail_rc) {
        return s->fail_rc;
    }
    if (!s->blocked_once && s->naccepted == s->block_at) {
        s->blocked_once = true;
        return -EAGAIN;
    }
    s->accepted[s->naccepted++] = *cmd;
    return 0;
}

int main(void)
{
    struct nvkvm_broker_clip_cmd cmds[MAX_CMDS];
    unsigned char text[NVKVM_BROKER_CLIP_MAX_BYTES];
    unsigned char rebuilt[NVKVM_BROKER_CLIP_MAX_BYTES];
    size_t n, off;
    bool good;

    for (size_t i = 0; i < sizeof(text); i++) {
        text[i] = (unsigned char)((i * 37u + 11u) & 0xffu);
    }

    chk("NULL output is rejected",
        relay_clip_batch_build(NULL, MAX_CMDS, (char *)text, 1) == 0);
    chk("NULL text is rejected",
        relay_clip_batch_build(cmds, MAX_CMDS, NULL, 1) == 0);
    chk("an empty transfer is rejected",
        relay_clip_batch_build(cmds, MAX_CMDS, (char *)text, 0) == 0);
    chk("a transfer over the protocol cap is rejected",
        relay_clip_batch_build(cmds, MAX_CMDS, (char *)text,
                               NVKVM_BROKER_CLIP_MAX_BYTES + 1u) == 0);

    memset(cmds, 0xa5, sizeof(cmds));
    chk("insufficient output capacity is rejected",
        relay_clip_batch_build(cmds, 1, (char *)text,
                               NVKVM_BROKER_CLIP_CMD_BYTES + 1u) == 0);
    chk("capacity rejection writes no prefix", ((unsigned char *)cmds)[0] == 0xa5);

    n = relay_clip_batch_build(cmds, MAX_CMDS, (char *)text, 1);
    chk("one byte builds one command", n == 1);
    chk("one-byte command has the clipboard type and chunk zero",
        cmds[0].type == NVKVM_BROKER_CMD_CLIPBOARD && cmds[0].chunk == 0);
    chk("one-byte command is also LAST",
        nbytes(&cmds[0]) == 1 && (cmds[0].info & NVKVM_BROKER_CLIP_LAST));
    chk("one-byte payload is exact", cmds[0].data[0] == text[0]);

    n = relay_clip_batch_build(cmds, MAX_CMDS, (char *)text,
                               NVKVM_BROKER_CLIP_CMD_BYTES);
    chk("an exact full chunk builds one command", n == 1);
    chk("an exact full chunk carries LAST without an empty extra command",
        nbytes(&cmds[0]) == NVKVM_BROKER_CLIP_CMD_BYTES &&
        (cmds[0].info & NVKVM_BROKER_CLIP_LAST));

    n = relay_clip_batch_build(cmds, MAX_CMDS, (char *)text,
                               NVKVM_BROKER_CLIP_CMD_BYTES + 1u);
    chk("one byte over a chunk builds two commands", n == 2);
    chk("the first command is full, chunk zero, and not LAST",
        cmds[0].chunk == 0 &&
        nbytes(&cmds[0]) == NVKVM_BROKER_CLIP_CMD_BYTES &&
        !(cmds[0].info & NVKVM_BROKER_CLIP_LAST));
    chk("the second command is chunk one with one byte and LAST",
        cmds[1].chunk == 1 && nbytes(&cmds[1]) == 1 &&
        (cmds[1].info & NVKVM_BROKER_CLIP_LAST));
    chk("the split payload remains byte exact",
        memcmp(cmds[0].data, text, NVKVM_BROKER_CLIP_CMD_BYTES) == 0 &&
        cmds[1].data[0] == text[NVKVM_BROKER_CLIP_CMD_BYTES]);

    n = relay_clip_batch_build(cmds, MAX_CMDS, (char *)text,
                               NVKVM_BROKER_CLIP_CMD_BYTES * 2u);
    chk("two exact chunks build two commands, not three", n == 2);
    chk("only the second exact chunk carries LAST",
        !(cmds[0].info & NVKVM_BROKER_CLIP_LAST) &&
        (cmds[1].info & NVKVM_BROKER_CLIP_LAST));

    n = relay_clip_batch_build(cmds, MAX_CMDS, (char *)text, sizeof(text));
    chk("the maximum transfer fits the bounded command array", n == MAX_CMDS);
    good = true;
    off = 0;
    memset(rebuilt, 0, sizeof(rebuilt));
    for (size_t i = 0; i < n; i++) {
        size_t take = nbytes(&cmds[i]);

        if (cmds[i].type != NVKVM_BROKER_CMD_CLIPBOARD ||
            cmds[i].reserved0 != 0 || cmds[i].reserved1 != 0 ||
            cmds[i].chunk != i || !take ||
            take > NVKVM_BROKER_CLIP_CMD_BYTES ||
            off + take > sizeof(rebuilt)) {
            good = false;
            break;
        }
        memcpy(rebuilt + off, cmds[i].data, take);
        off += take;
    }
    chk("maximum-transfer chunks are monotonic and structurally valid", good);
    good = true;
    for (size_t i = 0; i < n; i++) {
        bool last = (cmds[i].info & NVKVM_BROKER_CLIP_LAST) != 0;

        if (last != (i + 1 == n)) {
            good = false;
        }
    }
    chk("only the final maximum-transfer command carries LAST", good);
    chk("maximum-transfer reconstruction has the original length",
        off == sizeof(text));
    chk("maximum-transfer reconstruction is byte exact",
        memcmp(rebuilt, text, sizeof(text)) == 0);
    chk("the maximum transfer's final chunk length is exact",
        nbytes(&cmds[n - 1]) ==
        NVKVM_BROKER_CLIP_MAX_BYTES % NVKVM_BROKER_CLIP_CMD_BYTES);

    {
        struct fake_sink sink = { .block_at = 1 };
        size_t next = 0;
        int rc;

        n = relay_clip_batch_build(cmds, MAX_CMDS, (char *)text,
                                   NVKVM_BROKER_CLIP_CMD_BYTES + 1u);
        chk("flush rejects a NULL command array",
            relay_clip_batch_flush(NULL, n, &next, fake_send_one, &sink) ==
            -EINVAL);
        chk("flush rejects a NULL transaction cursor",
            relay_clip_batch_flush(cmds, n, NULL, fake_send_one, &sink) ==
            -EINVAL);
        chk("flush rejects a NULL sender",
            relay_clip_batch_flush(cmds, n, &next, NULL, &sink) == -EINVAL);

        rc = relay_clip_batch_flush(cmds, n, &next, fake_send_one, &sink);
        chk("EAGAIN leaves the clipboard transaction queued", rc == -EAGAIN);
        chk("only the prefix before EAGAIN was accepted", sink.naccepted == 1);
        chk("the transaction cursor names the first unsent chunk", next == 1);

        rc = relay_clip_batch_flush(cmds, n, &next, fake_send_one, &sink);
        chk("a writable retry completes the same transaction", rc == 0);
        chk("the retry sends every command exactly once",
            sink.naccepted == n && next == n);
        chk("the resumed transaction is byte-identical to the built batch",
            memcmp(sink.accepted, cmds, n * sizeof(cmds[0])) == 0);
    }

    {
        struct fake_sink sink = {
            .block_at = (size_t)-1,
            .fail_rc = -EPIPE,
        };
        size_t next = 0;

        n = relay_clip_batch_build(cmds, MAX_CMDS, (char *)text, 1);
        chk("a fatal command send is returned to the connection owner",
            relay_clip_batch_flush(cmds, n, &next, fake_send_one, &sink) ==
            -EPIPE);
        chk("a fatal send does not skip the unsent command", next == 0);
    }

    printf("%d/%d tests passed\n", passed, run);
    return passed == run ? 0 : 1;
}
