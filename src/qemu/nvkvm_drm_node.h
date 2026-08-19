/*
 * nvkvm_drm_node.h — find the host's NVIDIA DRM render node.
 *
 * The render-node minor is NOT stable at 128.  DRM hands out minors in probe
 * order, so on any machine where another GPU enumerates first — an Intel or AMD
 * iGPU on a hybrid-graphics laptop, which is the common case, or a second card
 * in a workstation — the NVIDIA node is renderD129 or higher.  Assuming 128
 * makes every DRM-dependent path fail with ENOENT on exactly the hardware most
 * people have, while CUDA and headless EGL keep working, so nothing notices.
 *
 * Measured on an RTX 3050 laptop container: the only node present is
 * renderD129 (driver "nvidia"); renderD128 belongs to the Intel iGPU and is not
 * even passed into the container.
 *
 * Ask sysfs which driver owns each node instead.
 */
#ifndef NVKVM_DRM_NODE_H
#define NVKVM_DRM_NODE_H

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Highest render minor worth scanning.  DRM allocates render minors from 128
 * upward; 64 slots is far more GPUs than any host we target. */
#define NVKVM_DRM_RENDER_MIN 128
#define NVKVM_DRM_RENDER_MAX 192

/*
 * The minor of the k-th NVIDIA render node on this host, or -1 if there is no
 * k-th one.  k is an index over NVIDIA nodes only, so k=0 is "the first NVIDIA
 * GPU" whatever minor it landed on.
 *
 * Falls back to 128+k when sysfs cannot be read at all (an unusual container
 * without /sys), which is the historical behaviour and no worse than it.
 */
static inline int nvkvm_nvidia_render_minor(unsigned k)
{
    bool sysfs_seen = false;
    unsigned found = 0;

    for (int n = NVKVM_DRM_RENDER_MIN; n < NVKVM_DRM_RENDER_MAX; n++) {
        char path[96], link[256], *base;
        ssize_t r;

        snprintf(path, sizeof(path),
                 "/sys/class/drm/renderD%d/device/driver", n);
        r = readlink(path, link, sizeof(link) - 1);
        if (r <= 0) {
            continue;
        }
        sysfs_seen = true;
        link[r] = '\0';
        base = strrchr(link, '/');
        base = base ? base + 1 : link;
        if (strcmp(base, "nvidia") != 0) {
            continue;
        }
        if (found++ == k) {
            return n;
        }
    }
    return sysfs_seen ? -1 : (int)(NVKVM_DRM_RENDER_MIN + k);
}

/* "/dev/dri/renderD<minor>" for the k-th NVIDIA node, or NULL if none. */
static inline const char *nvkvm_nvidia_render_path(unsigned k, char *buf,
                                                   size_t buflen)
{
    int minor = nvkvm_nvidia_render_minor(k);

    if (minor < 0) {
        return NULL;
    }
    snprintf(buf, buflen, "/dev/dri/renderD%d", minor);
    return buf;
}

/*
 * Make the k-th NVIDIA node reachable under the name the stub asks for.
 *
 * The stub resolves NVKVM_DEV_DRM_RD(k) to the fixed name "dri/renderD(128+k)".
 * In namespace mode the sandbox bind-mounts the right node onto that name.  In
 * uid+chroot mode — the fallback when the container lacks CAP_SYS_ADMIN, i.e.
 * the DEFAULT unprivileged-container deployment — there is no mount namespace
 * to rewrite, and the stub resolves the name against the real /dev.  So make
 * the name exist there: a relative symlink, which resolves correctly both
 * outside and inside the chroot.
 *
 * Only ever CREATES a name that is absent.  If renderD(128+k) already exists it
 * is left alone — on a bare-metal host that node is likely another vendor's GPU
 * and replacing it would be both wrong and destructive.  That case is reported
 * by the return value so the caller can say so rather than fail silently.
 *
 * Returns 1 if a link was created, 0 if nothing was needed, -1 if the name is
 * taken by something that is not our node.
 */
static inline int nvkvm_drm_node_alias(unsigned k)
{
    char want[64], target[64], link[256];
    int minor = nvkvm_nvidia_render_minor(k);
    ssize_t r;

    if (minor < 0) {
        return 0;                       /* no k-th NVIDIA GPU; nothing to do */
    }
    if (minor == (int)(NVKVM_DRM_RENDER_MIN + k)) {
        return 0;                       /* already at the expected name */
    }
    snprintf(want, sizeof(want), "/dev/dri/renderD%u", NVKVM_DRM_RENDER_MIN + k);
    snprintf(target, sizeof(target), "renderD%d", minor);

    /* Already the alias we would have made (e.g. a previous run)? */
    r = readlink(want, link, sizeof(link) - 1);
    if (r > 0) {
        link[r] = '\0';
        return strcmp(link, target) == 0 ? 0 : -1;
    }
    if (access(want, F_OK) == 0) {
        return -1;                      /* a real node, not ours — hands off */
    }
    return symlink(target, want) == 0 ? 1 : -1;
}

#endif /* NVKVM_DRM_NODE_H */
