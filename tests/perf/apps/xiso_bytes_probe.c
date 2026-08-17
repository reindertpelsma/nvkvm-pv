/* xiso_bytes_probe.c — CROSS-ISOLATE dma-buf DATA probe (#110).
 *
 * WHY THIS EXISTS.  xiso_import_probe proves that
 * eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) SUCCEEDS across two guest processes.
 * That is necessary but NOT sufficient, and on this architecture the difference
 * is the whole question: nvkvm's proxy GEM has a get_sg_table path that hands
 * out freshly allocated, zeroed GUEST pages (src/guest/nvkvm_drm.c, "ITERATION
 * 1: plain pages to unblock the map").  An import can therefore return a valid
 * EGLImage while the importer is looking at memory that has nothing to do with
 * the exporter's buffer.  A test that stops at EGL_SUCCESS cannot tell the two
 * apart.  This one compares bytes.
 *
 *   exporter (process A = isolate A): allocate a bo on renderD128, bind it as a
 *      GL render target, paint four quadrants in distinct known colours,
 *      glFinish, then PRIME-export the fd over a unix socket and hold the bo.
 *   importer (process B = isolate B): receive the fd, import it as an EGLImage,
 *      bind it as a GL texture, and read it back.  Compare each quadrant.
 *
 * Two processes is the point — that is what makes two isolates, and it is the
 * case the single-process dmabuf_import_probe structurally cannot reach.
 *
 * Colours are flat per quadrant and written with glClearColor+glScissor rather
 * than a computed pattern: v/255.0 round-trips exactly through a unorm8
 * attachment, so PASS means an EXACT match, with no shader-precision slack to
 * hide a near-miss.  All four colours are non-zero and mutually distinct, so a
 * zeroed buffer, a stale buffer, or a buffer showing only one quadrant all
 * fail.  An all-zero readback is reported as ZEROES rather than MISMATCH
 * because all-zero is the exact signature of the plain-guest-pages fallback and
 * should never be read as "the data merely differed".
 *
 *   usage: xiso_bytes_probe <sockpath>            # spawns both roles
 *          xiso_bytes_probe <sockpath> exporter
 *          xiso_bytes_probe <sockpath> importer   # trace this one
 *
 * build: cc -O2 -o xiso_bytes_probe xiso_bytes_probe.c -I/usr/include/libdrm \
 *          $(pkg-config --cflags --libs gbm libdrm egl glesv2)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define W 256
#define H 256

/* Four distinct, non-zero quadrant colours (R,G,B). Order: TL, TR, BL, BR in
 * GL coordinates (origin bottom-left). */
static const uint8_t QCOL[4][3] = {
	{ 0x20, 0x40, 0x60 },
	{ 0xA0, 0x10, 0xF0 },
	{ 0x11, 0xC3, 0x77 },
	{ 0xEE, 0x88, 0x22 },
};

static int quadrant_of(int x, int y)
{
	int qx = (x >= W / 2), qy = (y >= H / 2);
	return qy * 2 + qx;
}

static int send_fd(int sock, int fd, uint32_t stride, uint64_t mod)
{
	uint64_t meta[2] = { stride, mod };
	struct iovec io = { .iov_base = meta, .iov_len = sizeof(meta) };
	char cbuf[CMSG_SPACE(sizeof(int))] = {0};
	struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1,
			      .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &fd, sizeof(int));
	return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

static int recv_fd(int sock, uint32_t *stride, uint64_t *mod)
{
	uint64_t meta[2] = {0, 0};
	struct iovec io = { .iov_base = meta, .iov_len = sizeof(meta) };
	char cbuf[CMSG_SPACE(sizeof(int))] = {0};
	struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1,
			      .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
	if (recvmsg(sock, &msg, 0) < 0) return -1;
	*stride = (uint32_t)meta[0]; *mod = meta[1];
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
		if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
			int fd; memcpy(&fd, CMSG_DATA(cm), sizeof(int));
			return fd;
		}
	return -1;
}

/* ── shared EGL/GL bring-up ─────────────────────────────────────────────── */

struct glctx {
	struct gbm_device *gbm;
	EGLDisplay dpy;
	EGLContext ctx;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC img_tex2d;
};

static int gl_setup(struct glctx *g, const char *who)
{
	int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (fd < 0) { fprintf(stderr, "%s: open renderD128: %s\n", who, strerror(errno)); return -1; }
	g->gbm = gbm_create_device(fd);
	if (!g->gbm) { fprintf(stderr, "%s: gbm_create_device failed\n", who); return -1; }
	fprintf(stderr, "%s: gbm backend=%s\n", who, gbm_device_get_backend_name(g->gbm));

	g->dpy = eglGetDisplay((EGLNativeDisplayType)g->gbm);
	EGLint maj, min;
	if (!eglInitialize(g->dpy, &maj, &min)) {
		fprintf(stderr, "%s: eglInitialize err=0x%x\n", who, eglGetError()); return -1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);

	EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
			      EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLConfig cfg; EGLint ncfg = 0;
	eglChooseConfig(g->dpy, cfg_attr, &cfg, 1, &ncfg);
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	g->ctx = eglCreateContext(g->dpy, ncfg ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
	if (g->ctx == EGL_NO_CONTEXT) {
		fprintf(stderr, "%s: eglCreateContext err=0x%x\n", who, eglGetError()); return -1;
	}
	if (!eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx)) {
		fprintf(stderr, "%s: eglMakeCurrent err=0x%x\n", who, eglGetError()); return -1;
	}
	fprintf(stderr, "%s: EGL %d.%d vendor=%s\n", who, maj, min, eglQueryString(g->dpy, EGL_VENDOR));
	fprintf(stderr, "%s: GL_RENDERER=%s\n", who, (const char *)glGetString(GL_RENDERER));

	g->create_image  = (void *)eglGetProcAddress("eglCreateImageKHR");
	g->destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
	g->img_tex2d     = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!g->create_image || !g->img_tex2d) {
		fprintf(stderr, "%s: missing EGLImage entrypoints\n", who); return -1;
	}
	return 0;
}

static EGLImageKHR import_dmabuf(struct glctx *g, const char *who, int dbuf,
				 uint32_t stride, uint64_t mod)
{
	EGLint attrs[] = {
		EGL_WIDTH, W, EGL_HEIGHT, H,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, dbuf,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32),
		EGL_NONE
	};
	EGLImageKHR img = g->create_image(g->dpy, EGL_NO_CONTEXT,
					  EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
	if (img == EGL_NO_IMAGE_KHR)
		fprintf(stderr, "%s: eglCreateImageKHR err=0x%x\n", who, eglGetError());
	return img;
}

/* Bind an EGLImage as a GL_TEXTURE_2D and attach it to an FBO. Returns 0 on a
 * complete FBO. Used by BOTH roles: the exporter renders into it, the importer
 * reads out of it. Reading through an FBO attachment rather than a sampled
 * quad means the comparison sees the buffer's actual contents with no filtering
 * or colour-conversion in between. */
static int fbo_from_image(struct glctx *g, EGLImageKHR img, GLuint *tex_out, GLuint *fbo_out)
{
	GLuint tex = 0, fbo = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	g->img_tex2d(GL_TEXTURE_2D, img);
	GLenum e = glGetError();
	if (e != GL_NO_ERROR) {
		fprintf(stderr, "  glEGLImageTargetTexture2DOES(TEXTURE_2D) err=0x%x\n", e);
		return -1;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "  FBO incomplete status=0x%x\n", st);
		return -1;
	}
	*tex_out = tex; *fbo_out = fbo;
	return 0;
}

/* ── exporter ───────────────────────────────────────────────────────────── */

static int run_exporter(const char *path)
{
	struct glctx g = {0};
	if (gl_setup(&g, "exporter") < 0) return 2;

	struct gbm_bo *bo = gbm_bo_create(g.gbm, W, H, GBM_FORMAT_ARGB8888,
					  GBM_BO_USE_RENDERING);
	if (!bo) { fprintf(stderr, "exporter: gbm_bo_create failed\n"); return 2; }
	uint32_t stride = gbm_bo_get_stride(bo);
	uint64_t mod = gbm_bo_get_modifier(bo);
	int dbuf = gbm_bo_get_fd(bo);
	if (dbuf < 0) { fprintf(stderr, "exporter: gbm_bo_get_fd failed\n"); return 2; }
	fprintf(stderr, "exporter: bo stride=%u mod=0x%llx dmabuf=%d\n",
		stride, (unsigned long long)mod, dbuf);

	/* Paint through GL: NVIDIA's gbm has no CPU map (gbm_bo_map returns
	 * EINVAL for every creation flag combination on this driver), and a GL
	 * write is what a real client does anyway. */
	EGLImageKHR img = import_dmabuf(&g, "exporter", dbuf, stride, mod);
	if (img == EGL_NO_IMAGE_KHR) {
		printf("RESULT xiso_bytes=FAIL reason=exporter_self_import\n");
		return 2;
	}
	GLuint tex = 0, fbo = 0;
	if (fbo_from_image(&g, img, &tex, &fbo) < 0) {
		printf("RESULT xiso_bytes=FAIL reason=exporter_fbo\n");
		return 2;
	}
	glViewport(0, 0, W, H);
	glEnable(GL_SCISSOR_TEST);
	for (int q = 0; q < 4; q++) {
		int qx = q & 1, qy = q >> 1;
		glScissor(qx * (W / 2), qy * (H / 2), W / 2, H / 2);
		glClearColor(QCOL[q][0] / 255.0f, QCOL[q][1] / 255.0f,
			     QCOL[q][2] / 255.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glDisable(GL_SCISSOR_TEST);
	glFinish();
	GLenum ge = glGetError();
	if (ge != GL_NO_ERROR) fprintf(stderr, "exporter: glGetError=0x%x\n", ge);

	/* Verify locally that the write landed, so a failure on the importer can
	 * be attributed to the cross-isolate path and not to the exporter. */
	{
		uint32_t px = 0;
		glReadPixels(W / 4, H / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &px);
		fprintf(stderr, "exporter: self-readback q0=0x%08x (expect r=%02x g=%02x b=%02x)\n",
			px, QCOL[0][0], QCOL[0][1], QCOL[0][2]);
	}
	fprintf(stderr, "exporter: painted 4 quadrants\n");

	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
	unlink(path);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("exporter: bind"); return 3; }
	listen(s, 1);
	int c = accept(s, NULL, NULL);
	if (c < 0) { perror("exporter: accept"); return 3; }
	send_fd(c, dbuf, stride, mod);
	fprintf(stderr, "exporter: sent dmabuf fd, holding bo alive\n");
	char x; if (read(c, &x, 1) < 0) { /* importer finished or died */ }
	return 0;
}

/* ── importer ───────────────────────────────────────────────────────────── */

static int run_importer(const char *path)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
	for (int i = 0; i < 300 && connect(s, (struct sockaddr *)&a, sizeof(a)) < 0; i++)
		usleep(50000);
	uint32_t stride = 0; uint64_t mod = 0;
	int dbuf = recv_fd(s, &stride, &mod);
	if (dbuf < 0) {
		fprintf(stderr, "importer: recv_fd failed\n");
		printf("RESULT xiso_bytes=FAIL reason=no_fd\n");
		return 4;
	}
	fprintf(stderr, "importer: got dmabuf=%d stride=%u mod=0x%llx\n",
		dbuf, stride, (unsigned long long)mod);

	struct glctx g = {0};
	if (gl_setup(&g, "importer") < 0) {
		printf("RESULT xiso_bytes=FAIL reason=importer_gl_setup\n");
		return 4;
	}

	EGLImageKHR img = import_dmabuf(&g, "importer", dbuf, stride, mod);
	if (img == EGL_NO_IMAGE_KHR) {
		printf("RESULT xiso_bytes=FAIL reason=import_failed\n");
		return 5;
	}
	fprintf(stderr, "importer: EGLImage=%p\n", img);

	GLuint tex = 0, fbo = 0;
	if (fbo_from_image(&g, img, &tex, &fbo) < 0) {
		printf("RESULT xiso_bytes=FAIL reason=importer_fbo\n");
		return 6;
	}

	uint32_t *back = malloc((size_t)W * H * 4);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, back);
	GLenum gerr = glGetError();
	if (gerr != GL_NO_ERROR) fprintf(stderr, "importer: glGetError=0x%x\n", gerr);

	/* Compare every pixel against its quadrant colour. glReadPixels(GL_RGBA,
	 * UNSIGNED_BYTE) yields R,G,B,A in ascending byte order. */
	long mismatch = 0, nonzero = 0, checked = 0;
	int fx = -1, fy = -1; uint32_t fgot = 0; int fq = 0;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint32_t got = back[(size_t)y * W + x];
			uint8_t r = got & 0xff, gg = (got >> 8) & 0xff, b = (got >> 16) & 0xff;
			int q = quadrant_of(x, y);
			checked++;
			if (r || gg || b) nonzero++;
			if (r != QCOL[q][0] || gg != QCOL[q][1] || b != QCOL[q][2]) {
				if (!mismatch) { fx = x; fy = y; fgot = got; fq = q; }
				mismatch++;
			}
		}
	}
	fprintf(stderr, "importer: checked=%ld mismatch=%ld nonzero=%ld\n",
		checked, mismatch, nonzero);

	if (mismatch == 0) {
		printf("RESULT xiso_bytes=PASS checked=%ld quadrants=4\n", checked);
	} else if (nonzero == 0) {
		printf("RESULT xiso_bytes=FAIL reason=ZEROES checked=%ld "
		       "(importer saw an all-zero buffer: the classic "
		       "plain-guest-pages fallback, not the exporter's bo)\n", checked);
	} else {
		printf("RESULT xiso_bytes=FAIL reason=MISMATCH mismatch=%ld/%ld "
		       "first=(%d,%d) q=%d got r=%02x g=%02x b=%02x want r=%02x g=%02x b=%02x\n",
		       mismatch, checked, fx, fy, fq,
		       fgot & 0xff, (fgot >> 8) & 0xff, (fgot >> 16) & 0xff,
		       QCOL[fq][0], QCOL[fq][1], QCOL[fq][2]);
	}
	char x = 0; if (write(s, &x, 1) < 0) { /* exporter may already be gone */ }
	return mismatch == 0 ? 0 : 7;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	if (argc < 2) { fprintf(stderr, "usage: %s <sockpath> [exporter|importer]\n", argv[0]); return 1; }
	const char *path = argv[1];
	if (argc >= 3 && !strcmp(argv[2], "exporter")) return run_exporter(path);
	if (argc >= 3 && !strcmp(argv[2], "importer")) return run_importer(path);

	/* No role → spawn both as separate PROCESSES; that is what makes them
	 * separate nvkvm isolates, which is the entire point of the probe. */
	pid_t pid = fork();
	if (pid == 0) { execl("/proc/self/exe", argv[0], path, "exporter", (char *)NULL); _exit(127); }
	usleep(400000);
	int rc = run_importer(path);
	int st; waitpid(pid, &st, 0);
	return rc;
}
