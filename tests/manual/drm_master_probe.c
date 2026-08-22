/* Does anything but occupancy gate the guest's real display?
 * Distinguishes: (a) auto-master on first open (no cap check in drm core),
 * (b) explicit SET_MASTER, (c) GETFB capture, (d) actual scanout takeover. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static const char *E(void){ return strerror(errno); }

int main(int argc, char **argv)
{
    const char *node = argc > 1 ? argv[1] : "/dev/dri/card0";
    int drive = (argc > 2 && !strcmp(argv[2], "--drive"));
    printf("uid=%d euid=%d node=%s\n", getuid(), geteuid(), node);

    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) { printf("open: FAILED (%s)\n", E()); return 1; }
    printf("open: OK\n");

    struct drm_version v; char nm[64]={0};
    memset(&v,0,sizeof v); v.name=nm; v.name_len=sizeof nm-1;
    if (!ioctl(fd, DRM_IOCTL_VERSION, &v)) printf("driver: %s\n", nm);

    /* (a) Did the mere act of opening make us master?  drm core's
     * drm_master_open() grants it to the first opener with NO capability
     * check.  Probe with a DRM_MASTER-gated ioctl that changes nothing. */
    struct drm_mode_card_res res; memset(&res,0,sizeof res);
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        printf("GETRESOURCES: FAILED (%s)\n", E()); close(fd); return 1;
    }
    uint32_t crtcs[16], fbs[16], conns[16];
    if (res.count_crtcs > 16) res.count_crtcs = 16;
    if (res.count_connectors > 16) res.count_connectors = 16;
    res.crtc_id_ptr=(uintptr_t)crtcs; res.fb_id_ptr=(uintptr_t)fbs;
    res.connector_id_ptr=(uintptr_t)conns; res.count_fbs=0; res.count_encoders=0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        printf("GETRESOURCES(2): FAILED (%s)\n", E()); close(fd); return 1;
    }
    printf("GETRESOURCES: OK  crtcs=%u connectors=%u\n",
           res.count_crtcs, res.count_connectors);

    /* implicit-master probe: DROP_MASTER succeeds only if we ARE master. */
    int implicit = (ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) == 0);
    printf("implicit master on open: %s\n",
           implicit ? "YES  <-- became master with no capability check"
                    : "no");
    if (implicit) ioctl(fd, DRM_IOCTL_SET_MASTER, 0); /* take it back */

    /* (b) explicit */
    if (!ioctl(fd, DRM_IOCTL_SET_MASTER, 0)) printf("SET_MASTER: OK\n");
    else printf("SET_MASTER: %s (%s)\n", E(),
                errno==EACCES ? "no CAP_SYS_ADMIN" :
                errno==EBUSY  ? "someone else holds it" : "?");

    /* (c) capture the live scanout */
    int captured = 0;
    for (unsigned i = 0; i < res.count_crtcs; i++) {
        struct drm_mode_crtc c; memset(&c,0,sizeof c); c.crtc_id = crtcs[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c)) continue;
        if (!c.fb_id) { printf("  crtc %u: idle\n", crtcs[i]); continue; }
        printf("  crtc %u: ACTIVE fb=%u\n", crtcs[i], c.fb_id);
        struct drm_mode_fb_cmd f; memset(&f,0,sizeof f); f.fb_id = c.fb_id;
        if (ioctl(fd, DRM_IOCTL_MODE_GETFB, &f)) {
            printf("    GETFB: FAILED (%s)\n", E()); continue;
        }
        printf("    GETFB -> handle=%u %ux%u pitch=%u bpp=%u\n",
               f.handle, f.width, f.height, f.pitch, f.bpp);
        if (!f.handle) { printf("    handle=0 -> kernel WITHHELD pixels\n"); continue; }
        struct drm_prime_handle p; memset(&p,0,sizeof p);
        p.handle=f.handle; p.flags=DRM_CLOEXEC|DRM_RDWR;
        if (!ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p)) {
            printf("    PRIME export -> fd=%d  <-- SCREEN CONTENTS READABLE\n", p.fd);
            captured = 1;
        } else printf("    PRIME export: FAILED (%s)\n", E());
    }
    printf("RESULT capture: %s\n", captured ? "POSSIBLE" : "refused");

    /* (d) can we actually drive it? */
    if (drive) {
        struct drm_mode_create_dumb d; memset(&d,0,sizeof d);
        d.width=64; d.height=64; d.bpp=32;
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d)) {
            printf("CREATE_DUMB: FAILED (%s)\n", E());
        } else {
            struct drm_mode_fb_cmd fb; memset(&fb,0,sizeof fb);
            fb.width=64; fb.height=64; fb.pitch=d.pitch; fb.bpp=32;
            fb.depth=24; fb.handle=d.handle;
            if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb))
                printf("ADDFB: FAILED (%s)\n", E());
            else printf("ADDFB: OK fb=%u  <-- can create scanout fbs\n", fb.fb_id);
        }
    }
    close(fd);
    return 0;
}
