/* QEMU notifier stub for unit tests: virtio_nvgpu.h embeds a
 * NotifierWithReturn (the live-migration setup notifier), so the struct
 * needs a complete type. Nothing here calls it. */
#ifndef QEMU_NOTIFY_H
#define QEMU_NOTIFY_H
typedef struct NotifierWithReturn NotifierWithReturn;
struct NotifierWithReturn {
    void *notify;
    struct { NotifierWithReturn *le_next; NotifierWithReturn **le_prev; } node;
};
#endif
