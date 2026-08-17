/* gbmflip.c — deterministic NVIDIA-scanout-bo flip on the nvkvm virtual head.
 * Allocates a SCANOUT gbm_bo (NVIDIA backend) via gbm_bo_create (NOT
 * gbm_surface_create), CPU-fills it (LINEAR), AddFB + SetCrtc + PageFlip on
 * card0. Exercises the exact path the present path needs: NVIDIA bo -> our head
 * flip -> pipe_update proxy/stub_handle. No compositor, no EGL.
 *   usage: gbmflip [/dev/dri/card0] [nframes]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static int flips;
static void page_flip_handler(int fd, unsigned seq, unsigned s, unsigned us, void *d){ (void)fd;(void)seq;(void)s;(void)us;(void)d; flips++; }

int main(int argc, char**argv){
    const char *node = argc>1?argv[1]:"/dev/dri/card0";
    int nframes = argc>2?atoi(argv[2]):10;
    int fd = open(node, O_RDWR|O_CLOEXEC);
    if(fd<0){perror("open");return 1;}
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    drmModeRes *res = drmModeGetResources(fd);
    if(!res){perror("getres");return 2;}
    drmModeConnector *conn=NULL;
    for(int i=0;i<res->count_connectors;i++){
        drmModeConnector *c=drmModeGetConnector(fd,res->connectors[i]);
        if(c && c->connection==DRM_MODE_CONNECTED && c->count_modes>0){conn=c;break;}
        if(c)drmModeFreeConnector(c);
    }
    if(!conn){fprintf(stderr,"no connected connector\n");return 3;}
    drmModeModeInfo mode = conn->modes[0];
    printf("connector %u mode %s %ux%u\n", conn->connector_id, mode.name, mode.hdisplay, mode.vdisplay);
    drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[0]);
    uint32_t crtc_id = enc ? enc->crtc_id : 0;
    if(!crtc_id) crtc_id = res->crtcs[0];
    printf("crtc %u\n", crtc_id);

    struct gbm_device *gbm = gbm_create_device(fd);
    if(!gbm){fprintf(stderr,"gbm_create_device failed\n");return 4;}
    printf("gbm backend: %s\n", gbm_device_get_backend_name(gbm));

    /* Negotiate against the head's advertised modifiers (LINEAR + NVIDIA
     * 16Bx2 block-linear family) so we allocate a buffer the head accepts. */
    uint64_t want_mods[] = {
        DRM_FORMAT_MOD_LINEAR,
        DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(0), DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(1),
        DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(2), DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(3),
        DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(4), DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(5),
    };
    struct gbm_bo *bo = gbm_bo_create_with_modifiers(gbm, mode.hdisplay, mode.vdisplay,
        GBM_FORMAT_XRGB8888, want_mods, sizeof(want_mods)/sizeof(want_mods[0]));
    if(!bo){
        fprintf(stderr,"gbm_bo_create_with_modifiers failed, trying plain SCANOUT\n");
        bo = gbm_bo_create(gbm, mode.hdisplay, mode.vdisplay,
            GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
    }
    if(!bo){fprintf(stderr,"gbm_bo_create failed\n");return 5;}
    uint64_t modifier = gbm_bo_get_modifier(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    /* Handle for AddFB2: gbm_bo_get_handle can be 0 for NVIDIA bos; fall back to
     * exporting a dma-buf and PRIME-importing it into card0's GEM namespace. */
    uint32_t handle = gbm_bo_get_handle_for_plane(bo, 0).u32;
    if(!handle) handle = gbm_bo_get_handle(bo).u32;
    if(!handle){
        int dfd = gbm_bo_get_fd(bo);
        if(dfd>=0 && drmPrimeFDToHandle(fd, dfd, &handle)==0)
            printf("PRIME-imported dma-buf fd=%d -> handle=0x%x\n", dfd, handle);
        if(dfd>=0) close(dfd);
    }
    printf("scanout bo: handle=0x%x stride=%u modifier=0x%llx\n",
           handle, stride, (unsigned long long)modifier);

    /* CPU-fill (LINEAR) with a recognizable gradient. */
    void *md=NULL; uint32_t mstride=0;
    void *ptr = gbm_bo_map(bo, 0,0, mode.hdisplay, mode.vdisplay, GBM_BO_TRANSFER_WRITE, &mstride, &md);
    if(ptr){
        for(uint32_t y=0;y<mode.vdisplay;y++){
            uint32_t *row=(uint32_t*)((char*)ptr + y*mstride);
            for(uint32_t x=0;x<mode.hdisplay;x++)
                row[x] = (((x*255)/mode.hdisplay)<<16)|(((y*255)/mode.vdisplay)<<8)|0x40;
        }
        gbm_bo_unmap(bo, md);
        printf("filled bo via gbm_bo_map (linear, CPU-mappable)\n");
    } else printf("gbm_bo_map returned NULL (not CPU-mappable) — flipping uninitialized\n");

    uint32_t fb_id=0;
    uint32_t handles[4]={handle,0,0,0}, strides[4]={stride,0,0,0}, offsets[4]={0,0,0,0};
    uint64_t mods[4]={modifier,0,0,0};
    int r;
    if(modifier!=DRM_FORMAT_MOD_LINEAR && modifier!=DRM_FORMAT_MOD_INVALID)
        r = drmModeAddFB2WithModifiers(fd, mode.hdisplay, mode.vdisplay, GBM_FORMAT_XRGB8888,
                handles, strides, offsets, mods, &fb_id, DRM_MODE_FB_MODIFIERS);
    else
        r = drmModeAddFB2(fd, mode.hdisplay, mode.vdisplay, GBM_FORMAT_XRGB8888,
                handles, strides, offsets, &fb_id, 0);
    if(r){fprintf(stderr,"AddFB2 failed: %d (%s)\n", r, strerror(errno));return 6;}
    printf("AddFB2 ok fb_id=%u (modifier 0x%llx)\n", fb_id, (unsigned long long)modifier);

    r = drmModeSetCrtc(fd, crtc_id, fb_id, 0,0, &conn->connector_id, 1, &mode);
    if(r){fprintf(stderr,"drmModeSetCrtc failed: %d (%s)\n", r, strerror(errno));return 7;}
    printf("SetCrtc ok — modeset done\n");

    struct pollfd pfd = { .fd=fd, .events=POLLIN };
    drmEventContext ev = { .version=2, .page_flip_handler=page_flip_handler };
    for(int i=0;i<nframes;i++){
        r = drmModePageFlip(fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
        if(r){fprintf(stderr,"PageFlip %d failed: %d (%s)\n", i, r, strerror(errno));break;}
        if(poll(&pfd, 1, 1000)>0) drmHandleEvent(fd, &ev);
    }
    printf("RESULT flips_completed=%d (requested %d)\n", flips, nframes);
    return flips>0?0:8;
}
