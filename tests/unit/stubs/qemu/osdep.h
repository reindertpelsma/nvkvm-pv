/* qemu/osdep.h stub for unit tests */
#ifndef QEMU_OSDEP_STUB_H
#define QEMU_OSDEP_STUB_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>

/* Real qemu/osdep.h supplies endian helpers.  Keep the syntax-check stub
 * honest for protocol structs instead of making production code read little-
 * endian fields as native integers.  CI runs on little-endian x86_64. */
#define le32_to_cpu(v) ((uint32_t)(v))

#endif /* QEMU_OSDEP_STUB_H */
