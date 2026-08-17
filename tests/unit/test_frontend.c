/*
 * test_frontend.c — unit tests for nvkvm_frontend.c
 *
 * Regression coverage for the RM_CONTROL dispatch path:
 *
 *  - GET_VIRTUALIZATION_MODE must be forwarded to the host driver unchanged.
 *    (Previously it was intercepted and returned mode=NONE synthetically, which
 *    caused libnvml to take the wrong init path and send zero-filled params for
 *    NV2080 commands, getting NV_ERR_NOT_SUPPORTED back from the host driver.)
 *
 *  - NV_ERR_NOT_SUPPORTED (0x57) from the host propagates to the guest as-is.
 *    (A suppressor that blanket-zeroed 0x57 has been removed.)
 *
 *  - Security gates: unknown client and oversized params_size are rejected
 *    before the host ioctl is issued.
 *
 * Tests use a mock ioctl() so no GPU or running VM is required.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>

/* Pull in QEMU type stubs before any project headers */
#include "../../src/qemu/virtio_nvgpu.h"

/* ── Stubs for nvkvm_frontend.c dependencies not under test ─────────────── */

struct nvkvm_host_fd *nvkvm_fd_lookup(struct nvkvm_session *session,
				      uint32_t fd_token)
{
	(void)session;
	(void)fd_token;
	return NULL;
}

/* ── Mock ioctl ───────────────────────────────────────────────────────────── */

/*
 * We override ioctl() so host_ioctl() in nvkvm_frontend.c calls our mock
 * instead of the real kernel interface.
 *
 * Per-test control:
 *   mock_ioctl_ret    — value to return (0 = success, -1 = error)
 *   mock_ioctl_errno_ — errno to set when ret == -1
 *   mock_ioctl_status — NvStatus value to write into NVOS54.status
 *   mock_ioctl_called — incremented on each call
 */
static int      mock_ioctl_ret    = 0;
static int      mock_ioctl_errno_ = 0;
static uint32_t mock_ioctl_status = 0;
static int      mock_ioctl_called = 0;

int ioctl(int fd, unsigned long request, ...)
{
	(void)fd;
	mock_ioctl_called++;

	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	if (_IOC_NR(request) == NV_ESC_RM_CONTROL && arg) {
		struct nvos54_parameters *p = (struct nvos54_parameters *)arg;
		p->status = mock_ioctl_status;
	}

	if (mock_ioctl_ret == -1)
		errno = mock_ioctl_errno_;
	return mock_ioctl_ret;
}

static void mock_reset(void)
{
	mock_ioctl_ret    = 0;
	mock_ioctl_errno_ = 0;
	mock_ioctl_status = NV_OK;
	mock_ioctl_called = 0;
}

/* ── Minimal test framework ───────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
	static void name(void); \
	__attribute__((constructor)) static void _reg_##name(void) { \
		__test_registry[__test_count++] = (struct _test){ #name, name }; \
	} \
	static void name(void)

#define ASSERT_EQ(a, b) do { \
	if ((a) != (b)) { \
		fprintf(stderr, "  FAIL %s:%d: %s == %s  got %lld  expected %lld\n", \
			__FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b)); \
		__current_test_failed = 1; \
		return; \
	} \
} while (0)

#define ASSERT_NE(a, b) do { \
	if ((a) == (b)) { \
		fprintf(stderr, "  FAIL %s:%d: %s != %s, both %lld\n", \
			__FILE__, __LINE__, #a, #b, (long long)(a)); \
		__current_test_failed = 1; \
		return; \
	} \
} while (0)

#define ASSERT_TRUE(e)    ASSERT_NE((int)(e), 0)
#define ASSERT_FALSE(e)   ASSERT_EQ((int)(e), 0)
#define ASSERT_NULL(p)    ASSERT_EQ((uintptr_t)(p), 0UL)
#define ASSERT_NOTNULL(p) ASSERT_NE((uintptr_t)(p), 0UL)

struct _test { const char *name; void (*fn)(void); };
static struct _test __test_registry[256];
static int __test_count = 0;
static int __current_test_failed = 0;

int main(void)
{
	int i;
	for (i = 0; i < __test_count; i++) {
		mock_reset();
		__current_test_failed = 0;
		tests_run++;
		printf("[ RUN  ] %s\n", __test_registry[i].name);
		__test_registry[i].fn();
		if (__current_test_failed) {
			printf("[ FAIL ] %s\n", __test_registry[i].name);
			tests_failed++;
		} else {
			printf("[ PASS ] %s\n", __test_registry[i].name);
			tests_passed++;
		}
	}
	printf("\n%d/%d tests passed", tests_passed, tests_run);
	if (tests_failed)
		printf(", %d FAILED", tests_failed);
	printf("\n");
	return tests_failed ? 1 : 0;
}

/* ── Test fixture helpers ─────────────────────────────────────────────────── */

static struct nvkvm_session test_session;
static struct nvkvm_host_fd  test_hfd;
static struct nvkvm_client  *test_client;

static void fixture_init(void)
{
	memset(&test_session, 0, sizeof(test_session));
	pthread_mutex_init(&test_session.lock, NULL);
	pthread_mutex_init(&test_session.clients_lock, NULL);
	test_session.id = 1;

	memset(&test_hfd, 0, sizeof(test_hfd));
	test_hfd.fd     = 42;
	test_hfd.dev_id = NVKVM_DEV_CTL;

	test_client = nvkvm_client_alloc(0xc001);
	assert(test_client);
	test_session.clients[0] = test_client;
	test_session.nclients   = 1;
}

static void fixture_fini(void)
{
	nvkvm_client_free(NULL, test_client);
}

static struct nvkvm_req_ctx make_ctx(struct nvos54_parameters *p,
				     void *aux, size_t aux_sz)
{
	struct nvkvm_req_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.session    = &test_session;
	ctx.hfd        = &test_hfd;
	ctx.params_buf = p;
	ctx.param_size = sizeof(*p);
	ctx.aux_buf    = aux;
	ctx.aux_size   = aux_sz;
	return ctx;
}

/* ── Tests: GET_VIRTUALIZATION_MODE is forwarded, not intercepted ─────────── */

/*
 * Regression: GET_VIRT_MODE must NOT be intercepted — it must reach the host
 * driver so the host returns the real virtualization mode (typically mode=1 on
 * a KVM guest).  Intercepting and returning mode=NONE(0) caused libnvml to
 * take the non-VM init path, which sends zero-filled params for NV2080
 * commands, resulting in NV_ERR_NOT_SUPPORTED from the host driver.
 */
TEST(test_get_virt_mode_forwarded_to_host)
{
	fixture_init();

	struct nv0080_ctrl_gpu_get_virtualization_mode_params vp = {
		.virtualization_mode = 0xFF,  /* poison — must not be overwritten */
		.b_is_grid_licensed  = 1,
	};
	struct nvos54_parameters p = {
		.h_client    = 0xc001,
		.h_object    = 0x1234,
		.cmd         = NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE,
		.params_size = sizeof(vp),
		.params      = 0,
	};
	struct nvkvm_req_ctx ctx = make_ctx(&p, &vp, sizeof(vp));

	mock_ioctl_ret    = 0;
	mock_ioctl_status = NV_OK;

	int ret = nvkvm_handle_rm_control(&ctx);

	ASSERT_EQ(ret, 0);
	ASSERT_EQ(mock_ioctl_called, 1);  /* host MUST be called */

	fixture_fini();
}

/*
 * Regression: the params pointer must be restored to the guest-side value
 * after the host ioctl, even for GET_VIRT_MODE.
 */
TEST(test_get_virt_mode_params_ptr_restored_after_forward)
{
	fixture_init();

	struct nv0080_ctrl_gpu_get_virtualization_mode_params vp = { 0 };
	struct nvos54_parameters p = {
		.h_client    = 0xc001,
		.h_object    = 0x1234,
		.cmd         = NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE,
		.params_size = sizeof(vp),
		.params      = 0xDEADBEEFCAFEULL,
	};
	struct nvkvm_req_ctx ctx = make_ctx(&p, &vp, sizeof(vp));
	uint64_t orig_params = p.params;

	nvkvm_handle_rm_control(&ctx);

	ASSERT_EQ(p.params, orig_params);

	fixture_fini();
}

/* ── Tests: 0x57 propagates to the guest unchanged ───────────────────────── */

/*
 * NV_ERR_NOT_SUPPORTED (0x57) from the host must propagate to the guest
 * unchanged.  A previous blanket suppressor was removed because it masked real
 * errors after the GET_VIRT_MODE interception was taken out.
 */
TEST(test_not_supported_propagates_unchanged)
{
	fixture_init();

	uint8_t aux[32];
	memset(aux, 0xAB, sizeof(aux));

	struct nvos54_parameters p = {
		.h_client    = 0xc001,
		.h_object    = 0x5678,
		.cmd         = NV2080_CTRL_CMD_GPU_GET_GID_INFO,
		.params_size = sizeof(aux),
	};
	struct nvkvm_req_ctx ctx = make_ctx(&p, aux, sizeof(aux));

	mock_ioctl_ret    = 0;
	mock_ioctl_status = NV_ERR_NOT_SUPPORTED;

	int ret = nvkvm_handle_rm_control(&ctx);

	ASSERT_EQ(ret, 0);
	ASSERT_EQ(p.status, NV_ERR_NOT_SUPPORTED);  /* must reach the guest */

	/* aux buffer must be intact — the driver's response, however partial */
	ASSERT_EQ(aux[0], 0xAB);

	fixture_fini();
}

/*
 * A hard ioctl error (ret < 0) propagates regardless of the status field.
 */
TEST(test_ioctl_error_propagates)
{
	fixture_init();

	uint8_t aux[8] = { 0 };
	struct nvos54_parameters p = {
		.h_client    = 0xc001,
		.h_object    = 0x5678,
		.cmd         = 0x20800155,
		.params_size = sizeof(aux),
	};
	struct nvkvm_req_ctx ctx = make_ctx(&p, aux, sizeof(aux));

	mock_ioctl_ret    = -1;
	mock_ioctl_errno_ = EINVAL;

	int ret = nvkvm_handle_rm_control(&ctx);

	ASSERT_NE(ret, 0);

	fixture_fini();
}

/*
 * A successful response passes through with the driver-supplied data intact.
 */
TEST(test_success_passthrough)
{
	fixture_init();

	uint8_t aux[8] = { 0 };
	struct nvos54_parameters p = {
		.h_client    = 0xc001,
		.h_object    = 0x5678,
		.cmd         = 0x20800102,   /* NV2080_CTRL_CMD_GPU_GET_INFO_V2 */
		.params_size = sizeof(aux),
	};
	struct nvkvm_req_ctx ctx = make_ctx(&p, aux, sizeof(aux));

	mock_ioctl_ret    = 0;
	mock_ioctl_status = NV_OK;

	int ret = nvkvm_handle_rm_control(&ctx);

	ASSERT_EQ(ret, 0);
	ASSERT_EQ(p.status, NV_OK);
	ASSERT_EQ(mock_ioctl_called, 1);

	fixture_fini();
}

/* ── Tests: security gates ───────────────────────────────────────────────── */

TEST(test_rm_control_unknown_client_rejected)
{
	fixture_init();

	uint8_t aux[8] = { 0 };
	struct nvos54_parameters p = {
		.h_client    = 0xDEAD,   /* not registered */
		.h_object    = 0x1234,
		.cmd         = 0x20800155,
		.params_size = sizeof(aux),
	};
	struct nvkvm_req_ctx ctx = make_ctx(&p, aux, sizeof(aux));

	int ret = nvkvm_handle_rm_control(&ctx);

	ASSERT_NE(ret, 0);
	ASSERT_EQ(mock_ioctl_called, 0);  /* host must not be called */

	fixture_fini();
}

TEST(test_rm_control_params_size_exceeds_aux_size_rejected)
{
	fixture_init();

	uint8_t aux[8] = { 0 };
	struct nvos54_parameters p = {
		.h_client    = 0xc001,
		.h_object    = 0x1234,
		.cmd         = 0x20800155,
		.params_size = 9,   /* 1 byte bigger than aux (8) */
	};
	struct nvkvm_req_ctx ctx = make_ctx(&p, aux, sizeof(aux));

	int ret = nvkvm_handle_rm_control(&ctx);

	ASSERT_NE(ret, 0);
	ASSERT_EQ(mock_ioctl_called, 0);

	fixture_fini();
}

TEST(test_rm_control_params_size_nonzero_without_aux_rejected)
{
	fixture_init();

	struct nvos54_parameters p = {
		.h_client    = 0xc001,
		.h_object    = 0x1234,
		.cmd         = 0x20800155,
		.params_size = 8,   /* non-zero but no aux_buf */
	};
	struct nvkvm_req_ctx ctx = make_ctx(&p, NULL, 0);

	int ret = nvkvm_handle_rm_control(&ctx);

	ASSERT_NE(ret, 0);
	ASSERT_EQ(mock_ioctl_called, 0);

	fixture_fini();
}
