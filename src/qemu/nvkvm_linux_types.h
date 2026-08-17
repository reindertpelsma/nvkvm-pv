/* nvkvm_linux_types.h — map Linux kernel integer types to stdint equivalents.
 *
 * Used only in the QEMU (user-space) build to avoid including <linux/types.h>
 * directly in files that don't use it explicitly.
 *
 * If <linux/types.h> was already included (e.g., via qemu/osdep.h pulling in
 * <sys/stat.h> -> <linux/stat.h> -> <linux/types.h>), all the types we need
 * are already defined and this file is a no-op.
 *
 * If <linux/types.h> was not yet included, we define the minimum set of types
 * needed by nvgpu.h, uvm.h, and nvkvm_proto.h in terms of stdint.h types.
 */
#ifndef _LINUX_TYPES_H
/* <linux/types.h> not yet processed — provide minimal definitions */
#include <stdint.h>

typedef uint8_t         __u8;
typedef uint16_t        __u16;
typedef uint32_t        __u32;
/* Use unsigned long long to match <linux/types.h> on LP64 targets */
typedef unsigned long long __u64;
typedef int8_t          __s8;
typedef int16_t         __s16;
typedef int32_t         __s32;
typedef long long       __s64;

/* Endianness-annotated aliases — sparse annotation stripped for user-space */
typedef __u16           __le16;
typedef __u32           __le32;
typedef __u64           __le64;
typedef __u16           __be16;
typedef __u32           __be32;
typedef __u64           __be64;
#endif /* _LINUX_TYPES_H */
