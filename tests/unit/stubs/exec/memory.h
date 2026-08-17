/* QEMU exec/memory.h stub for unit tests */
#ifndef QEMU_EXEC_MEMORY_STUB_H
#define QEMU_EXEC_MEMORY_STUB_H

#include <stdint.h>
#include <stddef.h>

typedef struct MemoryRegion { int _dummy; } MemoryRegion;
typedef struct MemoryRegion AddressSpace;

static inline void memory_region_init_ram_ptr(MemoryRegion *mr, void *owner,
					      const char *name, uint64_t size,
					      void *ptr) { (void)mr; (void)owner; (void)name; (void)size; (void)ptr; }
static inline void memory_region_add_subregion(MemoryRegion *mr, uint64_t offset,
					       MemoryRegion *sub) { (void)mr; (void)offset; (void)sub; }
static inline void memory_region_del_subregion(MemoryRegion *mr,
					       MemoryRegion *sub) { (void)mr; (void)sub; }
static inline MemoryRegion *get_system_memory(void) { return NULL; }

#define OBJECT(o) ((void *)(o))

#endif /* QEMU_EXEC_MEMORY_STUB_H */
