/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_log.h — verbose tracing gate for the QEMU virtio-nvgpu device.
 *
 * Per-operation traces (every open/ioctl/alloc/mmap) are useful during
 * development but pure noise in production, where they also leak guest
 * activity into the host log.  Route them through NVKVM_DBG(), which is
 * silent unless the NVKVM_DEBUG environment variable is set at device
 * realize time.  Genuine errors and security DENY decisions keep using
 * fprintf(stderr, ...) directly so they are always visible.
 */
#ifndef NVKVM_LOG_H
#define NVKVM_LOG_H

#include <stdio.h>

extern int nvkvm_debug_enabled;  /* set once from getenv("NVKVM_DEBUG") */

#define NVKVM_DBG(...) \
	do { if (nvkvm_debug_enabled) fprintf(stderr, __VA_ARGS__); } while (0)

#endif /* NVKVM_LOG_H */
