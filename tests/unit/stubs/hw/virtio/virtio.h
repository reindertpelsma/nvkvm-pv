/* QEMU virtio stub for unit tests — minimal type definitions */
#ifndef QEMU_VIRTIO_STUB_H
#define QEMU_VIRTIO_STUB_H

#include <stdint.h>
#include <stdlib.h>

/* exec/memory.h stub — MemoryRegion is needed in VirtIONvgpu struct */
typedef struct MemoryRegion { int _dummy; } MemoryRegion;
static inline void memory_region_init_ram_ptr(MemoryRegion *m, void *o,
					      const char *n, uint64_t s, void *p)
{ (void)m;(void)o;(void)n;(void)s;(void)p; }
static inline void memory_region_add_subregion(MemoryRegion *c, uint64_t off,
					       MemoryRegion *s)
{ (void)c;(void)off;(void)s; }
static inline void memory_region_del_subregion(MemoryRegion *c, MemoryRegion *s)
{ (void)c;(void)s; }
static inline MemoryRegion *get_system_memory(void) { return NULL; }

typedef struct VirtIODevice    { int _dummy; } VirtIODevice;
typedef struct VirtQueue       { int _dummy; } VirtQueue;
typedef struct VirtQueueElement { int _dummy; } VirtQueueElement;

#define OBJECT_CHECK(t, o, n) ((t *)(o))

/* GLib memory stubs */
#define g_new0(T, n)      ((T *)calloc((size_t)(n), sizeof(T)))
#define g_free(p)         free(p)
#define g_realloc(p, s)   realloc((p), (size_t)(s))
#define g_malloc(s)       malloc(s)

#endif /* QEMU_VIRTIO_STUB_H */
