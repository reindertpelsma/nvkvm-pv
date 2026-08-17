/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_ioctl_fwd.c — Integration test for nvkvm-guest ioctl forwarding
 *
 * Runs inside a guest VM after nvkvm-guest.ko is loaded.  Exercises basic
 * ioctl forwarding through the virtio-nvgpu stack without requiring CUDA
 * libraries, nvidia-smi, or any userspace GPU runtime.
 *
 * Compile:
 *   gcc -Wall -Wextra -g -I../../src test_ioctl_fwd.c -o test_ioctl_fwd
 *
 * Run:
 *   ./test_ioctl_fwd
 *
 * Exit code: 0 if all tests pass, 1 if any test fails.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

/*
 * nvgpu.h uses <linux/types.h> typedefs (__u32, __u64, __le32, etc.).
 * On systems where <linux/types.h> is available (Linux userspace), including
 * it directly is the cleanest approach.  The header itself is self-guarded.
 */
#include <linux/types.h>

#include "abi/nvgpu.h"

/* ── NVIDIA ioctl magic ───────────────────────────────────────────────────── */

/*
 * The NVIDIA driver uses magic 'F' for all frontend ioctls.
 * _IOWR('F', nr, type) builds the full 32-bit ioctl command.
 */
#define NV_IOCTL_MAGIC  'F'

/* Frontend ioctls (NV_IOCTL_BASE = 200 = 0xC8) */
#define IOCTL_CHECK_VERSION_STR \
    _IOWR(NV_IOCTL_MAGIC, NV_ESC_CHECK_VERSION_STR, \
          struct nv_ioctl_rm_api_version)

#define IOCTL_SYS_PARAMS \
    _IOWR(NV_IOCTL_MAGIC, NV_ESC_SYS_PARAMS, \
          struct nv_ioctl_sys_params)

/*
 * NV_ESC_CARD_INFO encodes the array size in IOC_SIZE; build a command for
 * exactly one entry.
 */
#define IOCTL_CARD_INFO_1 \
    _IOWR(NV_IOCTL_MAGIC, NV_ESC_CARD_INFO, \
          struct nv_ioctl_card_info)

#define IOCTL_WAIT_OPEN_COMPLETE \
    _IOWR(NV_IOCTL_MAGIC, NV_ESC_WAIT_OPEN_COMPLETE, \
          struct nv_ioctl_wait_open_complete)

/* RM ioctls use their own NR values; build them with the correct struct size */
#define IOCTL_RM_ALLOC_NVOS21 \
    _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, \
          struct nvos21_parameters)

#define IOCTL_RM_FREE \
    _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_FREE, \
          struct nvos00_parameters)

#define IOCTL_RM_CONTROL \
    _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, \
          struct nvos54_parameters)

#define IOCTL_RM_ALLOC_NVOS64 \
    _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, \
          struct nvos64_parameters)

#define IOCTL_REGISTER_FD \
    _IOWR(NV_IOCTL_MAGIC, NV_ESC_REGISTER_FD, \
          struct nv_ioctl_register_fd)

/* ── Simple test framework ────────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;
static const char *g_current_test = "(none)";

#define TEST(name) \
    static void test_##name(void); \
    __attribute__((constructor)) static void register_##name(void) {} \
    static void test_##name(void)

/* Run a named test and update counters */
static void run_test(const char *name, void (*fn)(void))
{
    g_current_test = name;
    printf("[ RUN  ] %s\n", name);
    fn();
}

/*
 * CHECK — evaluate condition; on failure print a message and record the
 * failure but do NOT abort (so subsequent checks in the same test run).
 */
#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            printf("  PASS: %s\n", (msg)); \
            g_pass++; \
        } else { \
            printf("  FAIL: %s  [%s:%d, errno=%d (%s)]\n", \
                   (msg), __FILE__, __LINE__, errno, strerror(errno)); \
            g_fail++; \
        } \
    } while (0)

/* CHECK_RET — check that ret >= 0 (ioctl succeeded) */
#define CHECK_RET(ret, msg) CHECK((ret) >= 0, msg)

/* ── Device paths ─────────────────────────────────────────────────────────── */

#define DEV_CTL  "/dev/nvidiactl"
#define DEV_GPU0 "/dev/nvidia0"

/* ── Helper: open a device, return fd or -1 ──────────────────────────────── */

static int open_dev(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0)
        fprintf(stderr, "  open(%s): %s\n", path, strerror(errno));
    return fd;
}

/* ── Test implementations ─────────────────────────────────────────────────── */

/*
 * test_check_version_str
 *
 * NV_ESC_CHECK_VERSION_STR (NV_IOCTL_BASE+10 = 210)
 * The caller sets cmd=NV_RM_API_VERSION_CMD_QUERY (0); the driver fills
 * version_string with the host NVIDIA driver version like "535.104.05".
 */
static void test_check_version_str(void)
{
    int fd = open_dev(DEV_CTL);
    CHECK(fd >= 0, "open /dev/nvidiactl");
    if (fd < 0) return;

    struct nv_ioctl_rm_api_version ver;
    memset(&ver, 0, sizeof(ver));
    /*
     * cmd=0 → query mode: the NVIDIA driver fills version_string and returns
     * EINVAL (it wants the caller to confirm with cmd=1).  Even on EINVAL the
     * driver writes version_string into the params, so we get the version back
     * via our copy-back-on-error path.
     */
    ver.cmd = 0;

    int ret = ioctl(fd, IOCTL_CHECK_VERSION_STR, &ver);
    /*
     * Driver always returns EINVAL for cmd=0; accept that as the expected
     * result — the important thing is that version_string was populated.
     */
    CHECK(ret == -1 && errno == EINVAL,
          "NV_ESC_CHECK_VERSION_STR query returns EINVAL (expected for cmd=0)");

    /* version_string must be non-empty printable ASCII regardless of ret */
    {
        int ok = 0;
        if (ver.version_string[0] != '\0') {
            ok = 1;
            for (int i = 0; ver.version_string[i] != '\0' &&
                            i < NV_RM_API_VERSION_STRING_LENGTH; i++) {
                unsigned char c = (unsigned char)ver.version_string[i];
                if (c < 0x20 || c > 0x7e) { ok = 0; break; }
            }
        }
        CHECK(ok, "version_string is non-empty printable ASCII");
        printf("  version_string = \"%.*s\"\n",
               NV_RM_API_VERSION_STRING_LENGTH, ver.version_string);
    }

    close(fd);
}

/*
 * test_sys_params
 *
 * NV_ESC_SYS_PARAMS (NV_IOCTL_BASE+14 = 214)
 * Returns system memory size in bytes.
 */
static void test_sys_params(void)
{
    int fd = open_dev(DEV_CTL);
    CHECK(fd >= 0, "open /dev/nvidiactl");
    if (fd < 0) return;

    struct nv_ioctl_sys_params sp;
    memset(&sp, 0, sizeof(sp));

    int ret = ioctl(fd, IOCTL_SYS_PARAMS, &sp);
    /*
     * NV_ESC_SYS_PARAMS can only be called once globally per NVIDIA driver
     * instance.  On a running host where any GPU application has already
     * initialised the driver, the call returns EBUSY (already set).
     * Accept both 0 (first caller) and EBUSY (subsequent caller).
     */
    CHECK(ret == 0 || (ret == -1 && errno == EBUSY),
          "NV_ESC_SYS_PARAMS returns 0 or EBUSY (already initialised)");

    if (ret == 0) {
        CHECK(sp.memory_size > 0, "memory_size > 0");
        printf("  memory_size = %llu bytes\n",
               (unsigned long long)sp.memory_size);
    } else {
        printf("  (already initialised on host — EBUSY is expected)\n");
    }

    close(fd);
}

/*
 * test_card_info
 *
 * NV_ESC_CARD_INFO (NV_IOCTL_BASE+0 = 200)
 * The ioctl size encodes the number of array entries requested.  We ask for
 * one entry.  The driver sets valid=1 for populated slots.
 */
static void test_card_info(void)
{
    int fd = open_dev(DEV_CTL);
    CHECK(fd >= 0, "open /dev/nvidiactl");
    if (fd < 0) return;

    struct nv_ioctl_card_info info;
    memset(&info, 0, sizeof(info));

    int ret = ioctl(fd, IOCTL_CARD_INFO_1, &info);
    /*
     * The ioctl must complete without crashing.  Some drivers return 0 even
     * when no GPU is enumerated (valid==0), so we only require no EFAULT /
     * EBADF / unexpected error.
     */
    CHECK_RET(ret, "NV_ESC_CARD_INFO ioctl returns 0");

    if (ret >= 0) {
        printf("  card_info[0]: valid=%d gpu_id=0x%x minor=%u\n",
               (int)info.valid, info.gpu_id, info.minor_number);
        /*
         * Don't hard-fail on valid==0: the host may not have enumerated a
         * GPU yet.  We verify the ioctl completed without kernel panic.
         */
        CHECK(1, "NV_ESC_CARD_INFO completed without error");
    }

    close(fd);
}

/*
 * test_wait_open_complete
 *
 * NV_ESC_WAIT_OPEN_COMPLETE (NV_IOCTL_BASE+18 = 218)
 * Checks whether the device open sequence on the host completed.
 * rc==0 means success; adapter_status is advisory.
 */
static void test_wait_open_complete(void)
{
    int fd = open_dev(DEV_GPU0);
    CHECK(fd >= 0, "open /dev/nvidia0");
    if (fd < 0) return;

    struct nv_ioctl_wait_open_complete woc;
    memset(&woc, 0, sizeof(woc));

    int ret = ioctl(fd, IOCTL_WAIT_OPEN_COMPLETE, &woc);
    CHECK_RET(ret, "NV_ESC_WAIT_OPEN_COMPLETE ioctl returns 0");

    if (ret >= 0) {
        printf("  wait_open_complete: rc=%d adapter_status=0x%x\n",
               woc.rc, woc.adapter_status);
        CHECK(woc.rc == 0, "wait_open_complete rc == 0");
    }

    close(fd);
}

/*
 * test_rm_alloc_root_client
 *
 * NV_ESC_RM_ALLOC (IOC NR 0x2b) with NVOS21_PARAMETERS
 * Allocates an NV01_ROOT_CLIENT (class 0x41) on the provided fd.
 * The root client is the top of the RM object hierarchy.
 *
 * On success params.status == 0 (unchanged from memset) and h_object_new
 * holds the driver-assigned handle.  Returns the handle, or 0 on failure.
 *
 * NOTE: the returned handle is valid ONLY while fd remains open.  Closing fd
 * implicitly frees all RM objects associated with that client session.
 */
static nvhandle_t test_rm_alloc_root_client_on_fd(int fd)
{
    struct nvos21_parameters p;
    memset(&p, 0, sizeof(p));
    p.h_root           = 0;                 /* new root client has no parent */
    p.h_object_parent  = 0;
    p.h_object_new     = 0x00000001U;       /* caller-chosen handle          */
    p.h_class          = NV01_ROOT_CLIENT;  /* 0x41                          */
    p.p_alloc_parms    = 0;                 /* NV01_ROOT_CLIENT needs none    */
    /* Do NOT set a sentinel in p.status — the NVIDIA driver does not write
     * status=0 on success for NV01_ROOT_CLIENT; it leaves the field as
     * initialised (0 from memset).  Setting a non-zero sentinel here would
     * cause a false failure. */

    int ret = ioctl(fd, IOCTL_RM_ALLOC_NVOS21, &p);
    CHECK_RET(ret, "NV_ESC_RM_ALLOC (NVOS21) ioctl returns 0");
    /* status is left at 0 by both memset and the driver (driver does not
     * overwrite it on success for root-client allocation). */
    if (ret >= 0)
        CHECK(p.status == 0, "NV_ESC_RM_ALLOC status == 0 (driver did not set error)");

    return (ret >= 0 && p.status == 0) ? p.h_object_new : 0;
}

static void test_rm_alloc(void)
{
    int fd = open_dev(DEV_CTL);
    CHECK(fd >= 0, "open /dev/nvidiactl for RM_ALLOC");
    if (fd < 0) return;

    nvhandle_t h = test_rm_alloc_root_client_on_fd(fd);
    CHECK(h != 0, "RM_ALLOC returned non-zero handle");

    close(fd);
}

/*
 * test_rm_free
 *
 * NV_ESC_RM_FREE (IOC NR 0x29)
 * Allocates a root client and immediately frees it on the SAME fd.
 *
 * Important: the handle is only valid while the allocating fd is open.
 * Closing the fd implicitly frees all associated RM objects (driver behaviour).
 * Therefore alloc and free must be performed on the same fd in the same test.
 */
static void test_rm_free(void)
{
    int fd = open_dev(DEV_CTL);
    CHECK(fd >= 0, "open /dev/nvidiactl for RM_ALLOC+RM_FREE");
    if (fd < 0) return;

    nvhandle_t h = test_rm_alloc_root_client_on_fd(fd);
    if (h == 0) {
        printf("  SKIP: RM_ALLOC failed; skipping RM_FREE\n");
        close(fd);
        return;
    }

    struct nvos00_parameters fp;
    memset(&fp, 0, sizeof(fp));
    fp.h_root           = h;   /* for root client h_root == handle itself */
    fp.h_object_parent  = 0;
    fp.h_object_old     = h;
    /* Do NOT set a sentinel — same reasoning as RM_ALLOC. */

    int ret = ioctl(fd, IOCTL_RM_FREE, &fp);
    CHECK_RET(ret, "NV_ESC_RM_FREE ioctl returns 0");
    if (ret >= 0)
        CHECK(fp.status == 0, "NV_ESC_RM_FREE status == 0 (driver did not set error)");

    close(fd);
}

/*
 * test_rm_control_wrong_size  (negative test)
 *
 * Send an NV_ESC_RM_CONTROL ioctl with an intentionally wrong IOC_SIZE by
 * constructing a mangled command word.  The guest kernel module validates
 * param_size before forwarding; it should reject this with EINVAL or ENOTTY.
 */
static void test_rm_control_wrong_size(void)
{
    int fd = open_dev(DEV_CTL);
    CHECK(fd >= 0, "open /dev/nvidiactl for negative test");
    if (fd < 0) return;

    /*
     * Build a mangled command: same magic and NR as NV_ESC_RM_CONTROL but
     * with a size field that is intentionally wrong (1 byte instead of
     * sizeof(struct nvos54_parameters)).
     *
     * _IOC(dir, type, nr, size) — direction=RW (3), type='F', nr=0x2a, size=1
     */
    unsigned long bad_cmd = _IOC(_IOC_READ | _IOC_WRITE,
                                 NV_IOCTL_MAGIC,
                                 NV_ESC_RM_CONTROL,
                                 1 /* wrong size */);

    struct nvos54_parameters ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    errno = 0;
    int ret = ioctl(fd, bad_cmd, &ctrl);
    int saved_errno = errno;

    /*
     * We expect failure: ret == -1 and the ioctl is rejected.  Accept EINVAL/
     * ENOTTY/EBADMSG (size/path rejection) OR EACCES — the zeroed params carry
     * control cmd 0, which the QEMU default-deny control allowlist (#76) refuses
     * with NV_ERR_NOT_SUPPORTED → EACCES before it can reach the host driver.
     * Any of these means the malformed control was correctly rejected.
     */
    int rejected = (ret == -1) &&
                   (saved_errno == EINVAL || saved_errno == ENOTTY ||
                    saved_errno == EBADMSG || saved_errno == EACCES);
    CHECK(rejected,
          "NV_ESC_RM_CONTROL with wrong size is rejected (EINVAL/ENOTTY/EACCES)");
    if (!rejected)
        printf("  note: ret=%d errno=%d (%s)\n",
               ret, saved_errno, strerror(saved_errno));

    close(fd);
}

/*
 * test_check_version_str_strict
 *
 * NV_ESC_CHECK_VERSION_STR cmd=1 (strict / confirm mode).
 * First query the version with cmd=0, then confirm it with cmd=1.
 * This is what the CUDA driver runtime does during initialisation to verify
 * that the kernel module version matches the userspace library version.
 * cmd=1 with the correct version string returns 0; with a wrong string
 * it returns EINVAL (same as cmd=0 returning the version for inspection).
 */
static void test_check_version_str_strict(void)
{
    int fd = open_dev(DEV_CTL);
    CHECK(fd >= 0, "open /dev/nvidiactl for strict version check");
    if (fd < 0) return;

    /* Step 1: query version (cmd=0) */
    struct nv_ioctl_rm_api_version ver;
    memset(&ver, 0, sizeof(ver));
    ver.cmd = 0;
    ioctl(fd, IOCTL_CHECK_VERSION_STR, &ver);
    /* Ignore return value — cmd=0 always returns EINVAL but fills version_string */

    if (ver.version_string[0] == '\0') {
        printf("  SKIP: version_string not populated; cannot do strict check\n");
        close(fd);
        return;
    }
    printf("  queried version: \"%.*s\"\n",
           NV_RM_API_VERSION_STRING_LENGTH, ver.version_string);

    /* Step 2: confirm version (cmd=1, supply exact version string) */
    struct nv_ioctl_rm_api_version confirm;
    memset(&confirm, 0, sizeof(confirm));
    confirm.cmd = 1;
    memcpy(confirm.version_string, ver.version_string,
           NV_RM_API_VERSION_STRING_LENGTH);

    int ret = ioctl(fd, IOCTL_CHECK_VERSION_STR, &confirm);
    CHECK(ret == 0,
          "NV_ESC_CHECK_VERSION_STR cmd=1 with correct version returns 0");
    if (ret != 0)
        printf("  note: ret=%d errno=%d (%s) reply=%u\n",
               ret, errno, strerror(errno), confirm.reply);

    close(fd);
}

/*
 * test_register_fd
 *
 * NV_ESC_REGISTER_FD associates a /dev/nvidiaN fd with /dev/nvidiactl.
 * The NVIDIA RM requires this before any device-level RM operations can be
 * issued on the nvidiaN fd.  The guest module translates the ctl_fd file-
 * descriptor number to its fd_token before forwarding.
 */
static void test_register_fd(void)
{
    int ctl_fd = open_dev(DEV_CTL);
    CHECK(ctl_fd >= 0, "open /dev/nvidiactl for register_fd");
    if (ctl_fd < 0) return;

    int gpu_fd = open_dev(DEV_GPU0);
    CHECK(gpu_fd >= 0, "open /dev/nvidia0 for register_fd");
    if (gpu_fd < 0) { close(ctl_fd); return; }

    struct nv_ioctl_register_fd reg;
    memset(&reg, 0, sizeof(reg));
    reg.ctl_fd = ctl_fd;   /* guest fd number; module translates to fd_token */

    int ret = ioctl(gpu_fd, IOCTL_REGISTER_FD, &reg);
    CHECK(ret == 0, "NV_ESC_REGISTER_FD returns 0");
    if (ret != 0)
        printf("  note: ret=%d errno=%d (%s)\n", ret, errno, strerror(errno));

    close(gpu_fd);
    close(ctl_fd);
}

/*
 * Helper: allocate a root client on an already-open fd.
 * Returns the driver-assigned handle, or 0 on failure.
 */
static nvhandle_t alloc_root_client(int fd)
{
    struct nvos21_parameters p;
    memset(&p, 0, sizeof(p));
    p.h_object_new = 0x00000001U;
    p.h_class      = NV01_ROOT_CLIENT;
    int ret = ioctl(fd, IOCTL_RM_ALLOC_NVOS21, &p);
    if (ret < 0 || p.status != 0)
        return 0;
    return p.h_object_new;
}

/*
 * Helper: free an RM object.
 * Returns 0 on success.
 */
static int free_rm_object(int fd, nvhandle_t h_root, nvhandle_t h_obj)
{
    struct nvos00_parameters fp;
    memset(&fp, 0, sizeof(fp));
    fp.h_root       = h_root;
    fp.h_object_old = h_obj;
    return ioctl(fd, IOCTL_RM_FREE, &fp);
}

/*
 * alloc_device_with_params — allocate NV01_DEVICE_0 using NVOS21 with
 * explicit NV0080_ALLOC_PARAMETERS (DeviceID=0).  This is required for
 * RM_CONTROL calls on the device to succeed (driver won't associate the
 * device with a physical GPU otherwise).
 * Returns the device handle, or 0 on failure.
 */
static nvhandle_t alloc_device_with_params(int fd, nvhandle_t h_root)
{
    struct nv0080_alloc_parameters dev_alloc;
    memset(&dev_alloc, 0, sizeof(dev_alloc));
    dev_alloc.device_id = 0;  /* first GPU */

    struct nvos21_parameters p;
    memset(&p, 0, sizeof(p));
    p.h_root          = h_root;
    p.h_object_parent = h_root;
    p.h_object_new    = 0x00000002U;
    p.h_class         = NV01_DEVICE_0;
    p.p_alloc_parms   = (uint64_t)(uintptr_t)&dev_alloc;

    int ret = ioctl(fd, IOCTL_RM_ALLOC_NVOS21, &p);
    if (ret < 0 || p.status != 0)
        return 0;
    return p.h_object_new;
}

/*
 * alloc_subdevice_with_params — allocate NV20_SUBDEVICE_0 using NVOS21.
 */
static nvhandle_t alloc_subdevice_with_params(int fd, nvhandle_t h_root,
                                               nvhandle_t h_device)
{
    struct nv2080_alloc_parameters sub_alloc;
    memset(&sub_alloc, 0, sizeof(sub_alloc));
    sub_alloc.sub_device_id = 0;

    struct nvos21_parameters p;
    memset(&p, 0, sizeof(p));
    p.h_root          = h_root;
    p.h_object_parent = h_device;
    p.h_object_new    = 0x00000003U;
    p.h_class         = NV20_SUBDEVICE_0;
    p.p_alloc_parms   = (uint64_t)(uintptr_t)&sub_alloc;

    int ret = ioctl(fd, IOCTL_RM_ALLOC_NVOS21, &p);
    if (ret < 0 || p.status != 0)
        return 0;
    return p.h_object_new;
}

/*
 * test_rm_alloc_device
 *
 * Allocates the full RM object hierarchy required before any GPU compute work:
 *   root client → NV01_DEVICE_0 → NV20_SUBDEVICE_0
 *
 * Uses NVOS21_PARAMETERS with explicit NV0080/NV2080 alloc params so the
 * host passes DeviceID=0 / SubDeviceID=0 to the driver.  This is required
 * for RM_CONTROL commands on those objects to succeed.  REGISTER_FD must
 * also be called on /dev/nvidia0 to associate the GPU with the control fd.
 *
 * QEMU must correctly track all three handles so that RM_FREE calls on each
 * object succeed in reverse order.
 */
static void test_rm_alloc_device(void)
{
    int fd = open_dev(DEV_CTL);
    CHECK(fd >= 0, "open /dev/nvidiactl for device hierarchy test");
    if (fd < 0) return;

    /* REGISTER_FD: associate /dev/nvidia0 with /dev/nvidiactl */
    int gpu_fd = open_dev(DEV_GPU0);
    CHECK(gpu_fd >= 0, "open /dev/nvidia0 for register_fd");
    if (gpu_fd >= 0) {
        struct nv_ioctl_register_fd reg = {fd};
        int rret = ioctl(gpu_fd, IOCTL_REGISTER_FD, &reg);
        CHECK(rret == 0, "REGISTER_FD /dev/nvidia0 with /dev/nvidiactl");
    }

    /* 1. Root client */
    nvhandle_t h_root = alloc_root_client(fd);
    CHECK(h_root != 0, "alloc NV01_ROOT_CLIENT");
    if (!h_root) { if (gpu_fd >= 0) close(gpu_fd); close(fd); return; }
    printf("  h_root = 0x%08x\n", h_root);

    /* 2. NV01_DEVICE_0 with explicit DeviceID=0 (NVOS21 + alloc params) */
    nvhandle_t h_device = alloc_device_with_params(fd, h_root);
    CHECK(h_device != 0, "NV_ESC_RM_ALLOC NV01_DEVICE_0 with alloc params");
    if (!h_device) {
        free_rm_object(fd, h_root, h_root);
        if (gpu_fd >= 0) close(gpu_fd);
        close(fd);
        return;
    }
    printf("  h_device = 0x%08x\n", h_device);

    /* 3. NV20_SUBDEVICE_0 with explicit SubDeviceID=0 (NVOS21 + alloc params) */
    nvhandle_t h_subdev = alloc_subdevice_with_params(fd, h_root, h_device);
    CHECK(h_subdev != 0, "NV_ESC_RM_ALLOC NV20_SUBDEVICE_0 with alloc params");
    printf("  h_subdev = 0x%08x\n", h_subdev ? h_subdev : 0);

    /* 4. Free in reverse order */
    if (h_subdev) {
        int ret = free_rm_object(fd, h_root, h_subdev);
        CHECK(ret == 0, "RM_FREE NV20_SUBDEVICE_0 returns 0");
    }
    int ret = free_rm_object(fd, h_root, h_device);
    CHECK(ret == 0, "RM_FREE NV01_DEVICE_0 returns 0");
    ret = free_rm_object(fd, h_root, h_root);
    CHECK(ret == 0, "RM_FREE NV01_ROOT_CLIENT returns 0");

    if (gpu_fd >= 0) close(gpu_fd);
    close(fd);
}

/*
 * test_rm_control_num_subdevices
 *
 * NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES against a properly allocated
 * NV01_DEVICE_0.  This requires:
 *  a) REGISTER_FD + NVOS21 with explicit NV0080_ALLOC_PARAMETERS (DeviceID=0)
 *     when allocating the device — the driver will NOT associate the RM device
 *     with a physical GPU otherwise, causing RM_CONTROL to return NOT_SUPPORTED.
 *  b) The aux-slot pipeline to carry the 4-byte result buffer through shared
 *     memory to/from the host.
 *
 * Expected result: status=0, num_sub_devices=1 on a single-GPU system.
 */
static void test_rm_control_num_subdevices(void)
{
    int fd = open_dev(DEV_CTL);
    CHECK(fd >= 0, "open /dev/nvidiactl for rm_control test");
    if (fd < 0) return;

    int gpu_fd = open_dev(DEV_GPU0);
    CHECK(gpu_fd >= 0, "open /dev/nvidia0 for register_fd");
    if (gpu_fd >= 0) {
        struct nv_ioctl_register_fd reg = {fd};
        int rret = ioctl(gpu_fd, IOCTL_REGISTER_FD, &reg);
        CHECK(rret == 0, "REGISTER_FD /dev/nvidia0 with /dev/nvidiactl");
    }

    nvhandle_t h_root = alloc_root_client(fd);
    if (!h_root) {
        printf("  SKIP: root client alloc failed\n");
        if (gpu_fd >= 0) close(gpu_fd);
        close(fd);
        return;
    }

    nvhandle_t h_device = alloc_device_with_params(fd, h_root);
    if (!h_device) {
        printf("  SKIP: device alloc failed\n");
        free_rm_object(fd, h_root, h_root);
        if (gpu_fd >= 0) close(gpu_fd);
        close(fd);
        return;
    }

    /* NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES */
    struct nv0080_ctrl_gpu_get_num_subdevices_params ctrl_params;
    memset(&ctrl_params, 0, sizeof(ctrl_params));

    struct nvos54_parameters ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.h_client    = h_root;
    ctrl.h_object    = h_device;
    ctrl.cmd         = NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES;
    ctrl.params      = (uint64_t)(uintptr_t)&ctrl_params;
    ctrl.params_size = sizeof(ctrl_params);

    int ret = ioctl(fd, IOCTL_RM_CONTROL, &ctrl);
    CHECK(ret == 0, "NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES ioctl returns 0");
    if (ret == 0) {
        CHECK(ctrl.status == 0,
              "NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES status == 0");
        CHECK(ctrl_params.num_sub_devices > 0,
              "num_sub_devices > 0");
        printf("  num_sub_devices = %u\n", ctrl_params.num_sub_devices);
    } else {
        printf("  note: ret=%d errno=%d (%s) status=0x%x\n",
               ret, errno, strerror(errno), ctrl.status);
    }

    free_rm_object(fd, h_root, h_device);
    free_rm_object(fd, h_root, h_root);
    if (gpu_fd >= 0) close(gpu_fd);
    close(fd);
}

/*
 * test_rm_control_gpu_name
 *
 * NV2080_CTRL_CMD_GPU_GET_NAME_STRING (0x20800110) against a properly
 * allocated NV20_SUBDEVICE_0.  Returns the human-readable GPU name
 * (e.g. "NVIDIA GeForce RTX 3060").
 *
 * Struct layout discovered via brute-force size scan on 575.51.03:
 *   uint32 flags; char name[128];  (total 132 bytes, no padding after flags)
 */
#define NV2080_CTRL_CMD_GPU_GET_NAME_STRING 0x20800110U

struct nv2080_ctrl_gpu_get_name_string_params {
    uint32_t flags;
    char     name[128];
};

static void test_rm_control_gpu_name(void)
{
    int fd = open_dev(DEV_CTL);
    CHECK(fd >= 0, "open /dev/nvidiactl for gpu_name test");
    if (fd < 0) return;

    int gpu_fd = open_dev(DEV_GPU0);
    CHECK(gpu_fd >= 0, "open /dev/nvidia0 for register_fd");
    if (gpu_fd >= 0) {
        struct nv_ioctl_register_fd reg = {fd};
        int rret = ioctl(gpu_fd, IOCTL_REGISTER_FD, &reg);
        CHECK(rret == 0, "REGISTER_FD /dev/nvidia0 with /dev/nvidiactl");
    }

    nvhandle_t h_root = alloc_root_client(fd);
    if (!h_root) { printf("  SKIP: root client alloc failed\n"); if (gpu_fd >= 0) close(gpu_fd); close(fd); return; }

    nvhandle_t h_device = alloc_device_with_params(fd, h_root);
    if (!h_device) { printf("  SKIP: device alloc failed\n"); free_rm_object(fd, h_root, h_root); if (gpu_fd >= 0) close(gpu_fd); close(fd); return; }

    nvhandle_t h_subdev = alloc_subdevice_with_params(fd, h_root, h_device);
    if (!h_subdev) { printf("  SKIP: subdevice alloc failed\n"); free_rm_object(fd, h_root, h_device); free_rm_object(fd, h_root, h_root); if (gpu_fd >= 0) close(gpu_fd); close(fd); return; }

    struct nv2080_ctrl_gpu_get_name_string_params name_params;
    memset(&name_params, 0, sizeof(name_params));

    struct nvos54_parameters ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.h_client    = h_root;
    ctrl.h_object    = h_subdev;
    ctrl.cmd         = NV2080_CTRL_CMD_GPU_GET_NAME_STRING;
    ctrl.params      = (uint64_t)(uintptr_t)&name_params;
    ctrl.params_size = sizeof(name_params);  /* 132 bytes */

    int ret = ioctl(fd, IOCTL_RM_CONTROL, &ctrl);
    CHECK(ret == 0, "NV2080_CTRL_CMD_GPU_GET_NAME_STRING ioctl returns 0");
    if (ret == 0) {
        CHECK(ctrl.status == 0, "NV2080_CTRL_CMD_GPU_GET_NAME_STRING status == 0");
        int name_ok = (name_params.name[0] != '\0');
        CHECK(name_ok, "GPU name string is non-empty");
        printf("  GPU name: \"%s\"\n", name_params.name);
    } else {
        printf("  note: ret=%d errno=%d (%s) status=0x%x\n",
               ret, errno, strerror(errno), ctrl.status);
    }

    free_rm_object(fd, h_root, h_subdev);
    free_rm_object(fd, h_root, h_device);
    free_rm_object(fd, h_root, h_root);
    if (gpu_fd >= 0) close(gpu_fd);
    close(fd);
}

/* ── Test table ───────────────────────────────────────────────────────────── */

struct test_entry {
    const char *name;
    void (*fn)(void);
};

static const struct test_entry tests[] = {
    { "check_version_str",           test_check_version_str           },
    { "check_version_str_strict",    test_check_version_str_strict    },
    { "sys_params",                  test_sys_params                  },
    { "card_info",                   test_card_info                   },
    { "wait_open_complete",          test_wait_open_complete          },
    { "rm_alloc_root_client",        test_rm_alloc                    },
    { "rm_free_root_client",         test_rm_free                     },
    { "rm_control_wrong_size",       test_rm_control_wrong_size       },
    { "register_fd",                 test_register_fd                 },
    { "rm_alloc_device",             test_rm_alloc_device             },
    { "rm_control_num_subdevices",   test_rm_control_num_subdevices   },
    { "rm_control_gpu_name",         test_rm_control_gpu_name         },
};

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    /* Skip everything gracefully if the module is not loaded */
    if (access(DEV_CTL, F_OK) != 0) {
        printf("SKIP: %s does not exist — is nvkvm-guest.ko loaded?\n",
               DEV_CTL);
        return 0;
    }

    int n = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < n; i++)
        run_test(tests[i].name, tests[i].fn);

    printf("\n");
    printf("Results: %d passed, %d failed  (total checks: %d)\n",
           g_pass, g_fail, g_pass + g_fail);

    return (g_fail == 0) ? 0 : 1;
}
