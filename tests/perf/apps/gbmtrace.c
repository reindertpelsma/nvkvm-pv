/* gbmtrace.c — isolate the NVIDIA gbm backend's allocation + handle path on a
 * DRM node, with NO connector / modeset dependency (so it runs on a headless
 * host card0). Strace this to capture the exact ioctl sequence the NVIDIA gbm
 * backend issues during gbm_bo_create + gbm_bo_get_handle / get_handle_for_plane
 * / gbm_bo_get_fd. Compare host (card0, real nvidia-drm) vs guest (nvkvm head).
 *   usage: gbmtrace [/dev/dri/card0] [W] [H]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

static const char *bn(struct gbm_device *g){ return gbm_device_get_backend_name(g); }

int main(int argc, char**argv){
    const char *node = argc>1?argv[1]:"/dev/dri/card0";
    uint32_t W = argc>2?atoi(argv[2]):256;
    uint32_t H = argc>3?atoi(argv[3]):256;
    int fd = open(node, O_RDWR|O_CLOEXEC);
    if(fd<0){perror("open");return 1;}
    fprintf(stderr,"### gbmtrace node=%s fd=%d %ux%u\n", node, fd, W, H);

    struct gbm_device *gbm = gbm_create_device(fd);
    if(!gbm){fprintf(stderr,"gbm_create_device failed\n");return 4;}
    fprintf(stderr,"### gbm backend: %s\n", bn(gbm));

    uint64_t want_mods[] = {
        DRM_FORMAT_MOD_LINEAR,
        DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(0), DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(1),
        DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(2), DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(3),
        DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(4), DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(5),
    };

    fprintf(stderr,"### >>> gbm_bo_create_with_modifiers\n");
    struct gbm_bo *bo = gbm_bo_create_with_modifiers(gbm, W, H,
        GBM_FORMAT_XRGB8888, want_mods, sizeof(want_mods)/sizeof(want_mods[0]));
    if(!bo){
        fprintf(stderr,"### with_modifiers failed, trying plain SCANOUT|RENDERING\n");
        bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,
            GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
    }
    if(!bo){
        fprintf(stderr,"### plain failed, trying RENDERING only\n");
        bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    }
    if(!bo){fprintf(stderr,"### gbm_bo_create FAILED entirely\n");return 5;}
    fprintf(stderr,"### <<< bo created\n");

    uint64_t modifier = gbm_bo_get_modifier(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    int nplanes = gbm_bo_get_plane_count(bo);
    fprintf(stderr,"### bo: %ux%u stride=%u modifier=0x%llx planes=%d\n",
            W,H,stride,(unsigned long long)modifier,nplanes);

    fprintf(stderr,"### >>> gbm_bo_get_handle\n");
    uint32_t h_simple = gbm_bo_get_handle(bo).u32;
    fprintf(stderr,"### <<< gbm_bo_get_handle = 0x%x\n", h_simple);

    fprintf(stderr,"### >>> gbm_bo_get_handle_for_plane(0)\n");
    uint32_t h_plane = gbm_bo_get_handle_for_plane(bo, 0).u32;
    fprintf(stderr,"### <<< gbm_bo_get_handle_for_plane = 0x%x\n", h_plane);

    fprintf(stderr,"### >>> gbm_bo_get_fd (dma-buf export)\n");
    int dfd = gbm_bo_get_fd(bo);
    fprintf(stderr,"### <<< gbm_bo_get_fd = %d\n", dfd);

    if(dfd>=0){
        uint32_t prime_h=0;
        fprintf(stderr,"### >>> drmPrimeFDToHandle(card_fd, dfd=%d)\n", dfd);
        int pr = drmPrimeFDToHandle(fd, dfd, &prime_h);
        fprintf(stderr,"### <<< drmPrimeFDToHandle ret=%d handle=0x%x (errno=%s)\n",
                pr, prime_h, pr?strerror(errno):"ok");
        close(dfd);
    }

    fprintf(stderr,"### done\n");
    return 0;
}
