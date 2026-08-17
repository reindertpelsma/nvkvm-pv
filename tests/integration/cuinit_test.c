/* SPDX-License-Identifier: Apache-2.0 */
/*
 * cuinit_test.c — minimal cuInit + cuDeviceGetCount regression test.
 *
 * Build inside the guest after libcuda.so.<host-version> is installed:
 *   gcc -O0 -g -o cuinit_test cuinit_test.c -ldl
 *
 * Exit codes:
 *   0  — cuInit OK and device count > 0  (success)
 *   2  — cuInit OK but device count == 0
 *   1  — cuInit failed OR dlopen/dlsym failed
 */

#include <stdio.h>
#include <dlfcn.h>

int main(void)
{
    void *h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    int (*cuInit)(unsigned int)              = dlsym(h, "cuInit");
    int (*cuDriverGetVersion)(int*)          = dlsym(h, "cuDriverGetVersion");
    int (*cuDeviceGetCount)(int*)            = dlsym(h, "cuDeviceGetCount");
    int (*cuGetErrorString)(int, const char**) = dlsym(h, "cuGetErrorString");

    if (!cuInit || !cuDeviceGetCount) {
        printf("dlsym failed\n");
        return 1;
    }

    int ver = -1;
    if (cuDriverGetVersion) cuDriverGetVersion(&ver);
    printf("driver version: %d\n", ver);

    int rc = cuInit(0);
    if (rc != 0) {
        const char *msg = NULL;
        if (cuGetErrorString) cuGetErrorString(rc, &msg);
        printf("cuInit FAILED: %d (%s)\n", rc, msg ? msg : "<no string>");
        return 1;
    }
    printf("cuInit OK\n");

    int count = -1;
    rc = cuDeviceGetCount(&count);
    if (rc != 0) {
        const char *msg = NULL;
        if (cuGetErrorString) cuGetErrorString(rc, &msg);
        printf("cuDeviceGetCount FAILED: %d (%s)\n", rc, msg ? msg : "<no string>");
        return 1;
    }
    printf("device count: %d\n", count);
    if (count <= 0) {
        printf("PARTIAL: cuInit OK but no devices visible\n");
        return 2;
    }

    printf("PASS: cuInit OK with %d device(s)\n", count);
    return 0;
}
