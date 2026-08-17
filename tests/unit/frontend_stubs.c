/*
 * frontend_stubs.c — stub implementations of QEMU backend functions not
 * tested by test_dispatch.c. Allows linking the dispatch layer in isolation.
 */

#include <errno.h>
#include "../../src/qemu/virtio_nvgpu.h"

int nvkvm_handle_rm_alloc(struct nvkvm_req_ctx *ctx)        { (void)ctx; return -ENOSYS; }
int nvkvm_handle_rm_free(struct nvkvm_req_ctx *ctx)         { (void)ctx; return -ENOSYS; }
int nvkvm_handle_rm_control(struct nvkvm_req_ctx *ctx)      { (void)ctx; return -ENOSYS; }
int nvkvm_handle_rm_dup_object(struct nvkvm_req_ctx *ctx)   { (void)ctx; return -ENOSYS; }
int nvkvm_handle_register_fd(struct nvkvm_req_ctx *ctx)     { (void)ctx; return -ENOSYS; }
int nvkvm_handle_alloc_os_event(struct nvkvm_req_ctx *ctx)  { (void)ctx; return -ENOSYS; }
int nvkvm_handle_free_os_event(struct nvkvm_req_ctx *ctx)   { (void)ctx; return -ENOSYS; }
int nvkvm_handle_simple_ioctl(struct nvkvm_req_ctx *ctx,
                               unsigned int cmd)             { (void)ctx; (void)cmd; return -ENOSYS; }

struct nvkvm_host_fd *nvkvm_fd_lookup(struct nvkvm_session *session,
                                       uint32_t fd_token)   { (void)session; (void)fd_token; return NULL; }
