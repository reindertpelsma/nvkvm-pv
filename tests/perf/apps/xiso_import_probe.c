/* xiso_import_probe.c — CROSS-PROCESS dma-buf import probe (#110 cross-isolate).
 *
 * Two roles in two separate processes (= two nvkvm isolates), mirroring the
 * weston-capture case the single-process dmabuf_import_probe cannot reproduce:
 *
 *   exporter: allocate a gbm bo on renderD128, gbm_bo_get_fd() (PRIME export),
 *             send the dma-buf fd to the importer over a unix socket (SCM_RIGHTS),
 *             then idle (keeps the bo alive).
 *   importer: receive the dma-buf fd, eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) —
 *             the exact op weston does on a client buffer.  Prints RESULT.
 *
 * Run under rmdump.so on the IMPORTER to capture the cross-process import ioctl
 * sequence (frontend EXPORT/IMPORT_OBJECT_FROM_FD 0x5c/0x5d, EXPORT_TO_DMABUF_FD
 * 0xd4, RM_CONTROL 0x3d05/0x3d06).
 *
 *   usage: xiso_import_probe <sockpath>           # spawns both roles
 *          xiso_import_probe <sockpath> exporter  # exporter only
 *          xiso_import_probe <sockpath> importer  # importer only (trace this)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define W 256
#define H 256

/* meta[0] = stride, meta[1] = modifier — fixed 16-byte layout, no padding. */
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

static int run_exporter(const char *path)
{
	int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	struct gbm_device *gbm = gbm_create_device(fd);
	const uint64_t lin = DRM_FORMAT_MOD_LINEAR;
	struct gbm_bo *bo = gbm_bo_create_with_modifiers(gbm, W, H, GBM_FORMAT_XRGB8888, &lin, 1);
	if (!bo) { fprintf(stderr, "exporter: gbm_bo_create\n"); return 2; }
	uint32_t stride = gbm_bo_get_stride(bo);
	uint64_t mod = gbm_bo_get_modifier(bo);
	int dbuf = gbm_bo_get_fd(bo);
	fprintf(stderr, "exporter: bo stride=%u mod=0x%llx dmabuf=%d\n",
		stride, (unsigned long long)mod, dbuf);

	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
	unlink(path);
	bind(s, (struct sockaddr *)&a, sizeof(a));
	listen(s, 1);
	int c = accept(s, NULL, NULL);
	if (c < 0) { perror("accept"); return 3; }
	send_fd(c, dbuf, stride, mod);
	fprintf(stderr, "exporter: sent dmabuf fd, idling\n");
	char x; read(c, &x, 1);   /* block until importer done */
	return 0;
}

static int run_importer(const char *path)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
	for (int i = 0; i < 100 && connect(s, (struct sockaddr *)&a, sizeof(a)) < 0; i++)
		usleep(50000);
	uint32_t stride = 0; uint64_t mod = 0;
	int dbuf = recv_fd(s, &stride, &mod);
	if (dbuf < 0) { fprintf(stderr, "importer: recv_fd\n"); return 4; }
	fprintf(stderr, "importer: got dmabuf=%d stride=%u mod=0x%llx\n",
		dbuf, stride, (unsigned long long)mod);

	int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	struct gbm_device *gbm = gbm_create_device(fd);
	EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
	EGLint maj, min;
	eglInitialize(dpy, &maj, &min);
	fprintf(stderr, "importer: EGL %d.%d vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));
	eglBindAPI(EGL_OPENGL_ES_API);
	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");

	EGLint attrs[] = {
		EGL_WIDTH, W, EGL_HEIGHT, H,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, dbuf,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32),
		EGL_NONE
	};
	fprintf(stderr, ">>> eglCreateImageKHR(LINUX_DMA_BUF) BEGIN\n");
	EGLImageKHR img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
	fprintf(stderr, "<<< eglCreateImageKHR END img=%p err=0x%x\n", img, eglGetError());
	fprintf(stderr, "RESULT import=%s\n", img != EGL_NO_IMAGE_KHR ? "OK" : "FAILED");
	char x = 0; write(s, &x, 1);   /* release exporter */
	return img != EGL_NO_IMAGE_KHR ? 0 : 5;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s <sockpath> [exporter|importer]\n", argv[0]); return 1; }
	const char *path = argv[1];
	if (argc >= 3 && !strcmp(argv[2], "exporter")) return run_exporter(path);
	if (argc >= 3 && !strcmp(argv[2], "importer")) return run_importer(path);

	/* No role → spawn both as separate processes (separate isolates). */
	pid_t pid = fork();
	if (pid == 0) { execl("/proc/self/exe", argv[0], path, "exporter", (char *)NULL); _exit(127); }
	usleep(200000);
	int rc = run_importer(path);
	int st; waitpid(pid, &st, 0);
	return rc;
}
