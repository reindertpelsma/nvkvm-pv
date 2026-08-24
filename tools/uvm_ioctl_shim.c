/*
 * uvmshim.c — LD_PRELOAD tracer + fault injector for /dev/nvidia-uvm ioctls.
 *
 * TRACE:  logs every UVM ioctl in order, with base/length where the cmd has
 *         them, and the rmStatus the driver returned.
 * INJECT: UVM_FAIL_CMDS="51,42,46,44,45" overwrites rmStatus AFTER the real
 *         call with NV_ERR_INVALID_ADDRESS -- exactly what an external range
 *         would return for those commands.  This is the M1 decision experiment:
 *         does libcuda degrade, or hard-fail?
 *
 * Every struct offset and status code is taken by offsetof()/enum from the ogkm
 * headers compiled in, never transcribed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>

#include "uvm_ioctl.h"
#include "nvstatuscodes.h"
#include "uvm_linux_ioctl.h"

static int  (*real_ioctl)(int, unsigned long, ...);
static FILE *logf;
static int   fail_cmd[64], n_fail;

__attribute__((constructor)) static void init(void)
{
    const char *p;
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    p = getenv("UVM_LOG");
    logf = p ? fopen(p, "w") : stderr;
    if (!logf) logf = stderr;
    setvbuf(logf, NULL, _IOLBF, 0);
    p = getenv("UVM_FAIL_CMDS");
    if (p) {
        char buf[256], *tok, *sp = NULL;
        snprintf(buf, sizeof buf, "%s", p);
        for (tok = strtok_r(buf, ",", &sp); tok && n_fail < 64; tok = strtok_r(NULL, ",", &sp))
            fail_cmd[n_fail++] = atoi(tok);
    }
}

/* Is this fd /dev/nvidia-uvm?  Resolved per call via /proc/self/fd; the path is
 * short and this is a tracing build, so correctness beats speed. */
static int is_uvm_fd(int fd)
{
    char lnk[64], tgt[256];
    ssize_t n;
    snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", fd);
    n = readlink(lnk, tgt, sizeof tgt - 1);
    if (n < 0) return 0;
    tgt[n] = 0;
    return strcmp(tgt, "/dev/nvidia-uvm") == 0;
}

/* rmStatus offset per cmd, derived from the ogkm structs. 0 = unknown. */
static size_t rmstatus_off(unsigned long cmd, const char **name)
{
    switch (cmd) {
    case UVM_MIGRATE:                 *name="MIGRATE";                 return offsetof(UVM_MIGRATE_PARAMS, rmStatus);
    case UVM_VALIDATE_VA_RANGE:       *name="VALIDATE_VA_RANGE";       return offsetof(UVM_VALIDATE_VA_RANGE_PARAMS, rmStatus);
    case UVM_FREE:                    *name="FREE";                    return offsetof(UVM_FREE_PARAMS, rmStatus);
    case UVM_SET_PREFERRED_LOCATION:  *name="SET_PREFERRED_LOCATION";  return offsetof(UVM_SET_PREFERRED_LOCATION_PARAMS, rmStatus);
    case UVM_UNSET_PREFERRED_LOCATION:*name="UNSET_PREFERRED_LOCATION";return offsetof(UVM_UNSET_PREFERRED_LOCATION_PARAMS, rmStatus);
    case UVM_SET_ACCESSED_BY:         *name="SET_ACCESSED_BY";         return offsetof(UVM_SET_ACCESSED_BY_PARAMS, rmStatus);
    case UVM_UNSET_ACCESSED_BY:       *name="UNSET_ACCESSED_BY";       return offsetof(UVM_UNSET_ACCESSED_BY_PARAMS, rmStatus);
    case UVM_ENABLE_READ_DUPLICATION: *name="ENABLE_READ_DUPLICATION"; return offsetof(UVM_ENABLE_READ_DUPLICATION_PARAMS, rmStatus);
    case UVM_DISABLE_READ_DUPLICATION:*name="DISABLE_READ_DUPLICATION";return offsetof(UVM_DISABLE_READ_DUPLICATION_PARAMS, rmStatus);
    case UVM_CREATE_EXTERNAL_RANGE:   *name="CREATE_EXTERNAL_RANGE";   return offsetof(UVM_CREATE_EXTERNAL_RANGE_PARAMS, rmStatus);
    case UVM_MAP_EXTERNAL_ALLOCATION: *name="MAP_EXTERNAL_ALLOCATION"; return offsetof(UVM_MAP_EXTERNAL_ALLOCATION_PARAMS, rmStatus);
    case UVM_UNMAP_EXTERNAL:          *name="UNMAP_EXTERNAL";          return offsetof(UVM_UNMAP_EXTERNAL_PARAMS, rmStatus);
    case UVM_ALLOC_SEMAPHORE_POOL:    *name="ALLOC_SEMAPHORE_POOL";    return offsetof(UVM_ALLOC_SEMAPHORE_POOL_PARAMS, rmStatus);
    case UVM_CREATE_RANGE_GROUP:      *name="CREATE_RANGE_GROUP";      return offsetof(UVM_CREATE_RANGE_GROUP_PARAMS, rmStatus);
    case UVM_DESTROY_RANGE_GROUP:     *name="DESTROY_RANGE_GROUP";     return offsetof(UVM_DESTROY_RANGE_GROUP_PARAMS, rmStatus);
    /* SET_RANGE_GROUP is the one behind cuStreamAttachMemAsync, and its VA pair
     * does NOT start at offset 0 -- rangeGroupId comes first.  See base_off(). */
    case UVM_SET_RANGE_GROUP:         *name="SET_RANGE_GROUP";         return offsetof(UVM_SET_RANGE_GROUP_PARAMS, rmStatus);
    case UVM_REGISTER_CHANNEL:        *name="REGISTER_CHANNEL";        return offsetof(UVM_REGISTER_CHANNEL_PARAMS, rmStatus);
    case UVM_REGISTER_GPU_VASPACE:    *name="REGISTER_GPU_VASPACE";    return offsetof(UVM_REGISTER_GPU_VASPACE_PARAMS, rmStatus);
    case UVM_REGISTER_GPU:            *name="REGISTER_GPU";            return offsetof(UVM_REGISTER_GPU_PARAMS, rmStatus);
    case UVM_INITIALIZE:              *name="INITIALIZE";              return offsetof(UVM_INITIALIZE_PARAMS, rmStatus);
    case UVM_MM_INITIALIZE:           *name="MM_INITIALIZE";           return offsetof(UVM_MM_INITIALIZE_PARAMS, rmStatus);
    case UVM_PAGEABLE_MEM_ACCESS:     *name="PAGEABLE_MEM_ACCESS";     return 0;
    default:                          *name="?";                       return 0;
    }
}

/* Byte offset of the {base,length} pair, or -1 if the cmd carries none.  Almost
 * every VA-carrying UVM cmd starts with it; UVM_SET_RANGE_GROUP does not, which
 * is why this returns an offset rather than a bool. */
static long base_off(unsigned long cmd)
{
    if (cmd == UVM_SET_RANGE_GROUP)
        return (long)offsetof(UVM_SET_RANGE_GROUP_PARAMS, requestedBase);
    switch (cmd) {
    case UVM_MIGRATE: case UVM_VALIDATE_VA_RANGE: case UVM_FREE:
    case UVM_SET_PREFERRED_LOCATION: case UVM_UNSET_PREFERRED_LOCATION:
    case UVM_SET_ACCESSED_BY: case UVM_UNSET_ACCESSED_BY:
    case UVM_ENABLE_READ_DUPLICATION: case UVM_DISABLE_READ_DUPLICATION:
    case UVM_CREATE_EXTERNAL_RANGE: case UVM_MAP_EXTERNAL_ALLOCATION:
    case UVM_UNMAP_EXTERNAL: case UVM_ALLOC_SEMAPHORE_POOL:
        return 0;
    default: return -1;
    }
}

static int has_base_len_unused(unsigned long cmd)
{
    switch (cmd) {
    case UVM_MIGRATE: case UVM_VALIDATE_VA_RANGE: case UVM_FREE:
    case UVM_SET_PREFERRED_LOCATION: case UVM_UNSET_PREFERRED_LOCATION:
    case UVM_SET_ACCESSED_BY: case UVM_UNSET_ACCESSED_BY:
    case UVM_ENABLE_READ_DUPLICATION: case UVM_DISABLE_READ_DUPLICATION:
    case UVM_CREATE_EXTERNAL_RANGE: case UVM_MAP_EXTERNAL_ALLOCATION:
    case UVM_UNMAP_EXTERNAL: case UVM_ALLOC_SEMAPHORE_POOL:
        return 1;
    default: return 0;
    }
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; void *arg; int rc, i, inject = 0;
    const char *name = "?";
    size_t off;

    va_start(ap, req); arg = va_arg(ap, void *); va_end(ap);

    if (!is_uvm_fd(fd))
        return real_ioctl(fd, req, arg);

    rc  = real_ioctl(fd, req, arg);
    off = rmstatus_off(req, &name);

    for (i = 0; i < n_fail; i++)
        if ((unsigned long)fail_cmd[i] == req) inject = 1;

    if (inject && off && arg) {
        NV_STATUS bad = NV_ERR_INVALID_ADDRESS;
        memcpy((char *)arg + off, &bad, sizeof bad);
    }

    {
        unsigned long long base = 0, len = 0;
        unsigned st = 0;
        long bo = base_off(req);
        if (arg && bo >= 0) {
            memcpy(&base, (char *)arg + bo, 8);
            memcpy(&len,  (char *)arg + bo + 8, 8);
        }
        if (arg && off) memcpy(&st, (char *)arg + off, 4);
        fprintf(logf, "UVM cmd=%-4lu %-24s rc=%d rmStatus=0x%x base=0x%llx len=0x%llx%s\n",
                req, name, rc, st, base, len, inject ? "   <<INJECTED" : "");
    }
    return rc;
}
