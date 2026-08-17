/*
 * nvkvm_proc_shim.c — LD_PRELOAD that fakes the three nvidia driver
 * status paths libcuda probes:
 *
 *   /proc/driver/nvidia/params
 *   /sys/module/nvidia/initstate
 *   /sys/module/nvidia_uvm/initstate
 *
 * Used for diagnosis only — confirms whether libcuda's 999 in the guest
 * is caused by these files being missing.  If so, the real fix is for
 * nvkvm-guest to register them as proc entries.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/mman.h>

static const char PARAMS_PATH[] = "/proc/driver/nvidia/params";
static const char NV_INITSTATE[] = "/sys/module/nvidia/initstate";
static const char UVM_INITSTATE[] = "/sys/module/nvidia_uvm/initstate";

static const char PARAMS_BODY[] =
"ResmanDebugLevel: 4294967295\n"
"RmLogonRC: 1\n"
"ModifyDeviceFiles: 1\n"
"DeviceFileUID: 0\n"
"DeviceFileGID: 0\n"
"DeviceFileMode: 438\n"
"InitializeSystemMemoryAllocations: 1\n"
"UsePageAttributeTable: 4294967295\n"
"EnableMSI: 1\n"
"EnablePCIeGen3: 0\n"
"MemoryPoolSize: 0\n"
"KMallocHeapMaxSize: 0\n"
"VMallocHeapMaxSize: 0\n"
"IgnoreMMIOCheck: 0\n"
"EnableStreamMemOPs: 0\n"
"EnableUserNUMAManagement: 1\n"
"NvLinkDisable: 0\n"
"RmProfilingAdminOnly: 1\n"
"PreserveVideoMemoryAllocations: 1\n"
"EnableS0ixPowerManagement: 1\n"
"S0ixPowerManagementVideoMemoryThreshold: 256\n"
"DynamicPowerManagement: 3\n"
"DynamicPowerManagementVideoMemoryThreshold: 200\n"
"RegisterPCIDriver: 1\n"
"EnablePCIERelaxedOrderingMode: 0\n"
"EnableResizableBar: 0\n"
"EnableGpuFirmware: 18\n"
"EnableGpuFirmwareLogs: 2\n"
"RmNvlinkBandwidthLinkCount: 0\n"
"EnableDbgBreakpoint: 0\n"
"OpenRmEnableUnsupportedGpus: 1\n"
"DmaRemapPeerMmio: 1\n"
"ImexChannelCount: 2048\n"
"CreateImexChannel0: 0\n"
"GrdmaPciTopoCheckOverride: 0\n"
"RegistryDwords: \"\"\n"
"RegistryDwordsPerDevice: \"\"\n"
"RmMsg: \"\"\n"
"GpuBlacklist: \"\"\n"
"TemporaryFilePath: \"/var/tmp\"\n"
"ExcludedGpus: \"\"\n";

static const char LIVE_BODY[] = "live\n";

static int memfd_for(const char *body, size_t len)
{
	int fd = syscall(__NR_memfd_create, "nvkvm_shim", 0);
	if (fd < 0) return -1;
	ssize_t n = write(fd, body, len);
	if (n != (ssize_t)len) { close(fd); return -1; }
	lseek(fd, 0, SEEK_SET);
	return fd;
}

static int (*real_open)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_access)(const char *, int);

static const char *match(const char *path)
{
	if (!path) return NULL;
	if (strcmp(path, PARAMS_PATH) == 0)  return PARAMS_BODY;
	if (strcmp(path, NV_INITSTATE) == 0) return LIVE_BODY;
	if (strcmp(path, UVM_INITSTATE) == 0) return LIVE_BODY;
	return NULL;
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
	if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
	const char *body = match(pathname);
	if (body) {
		int fd = memfd_for(body, strlen(body));
		if (fd >= 0) {
			fprintf(stderr, "nvkvm_shim: openat(%s) -> memfd %d\n",
				pathname, fd);
			return fd;
		}
	}
	mode_t m = 0;
	if (flags & O_CREAT) {
		va_list ap; va_start(ap, flags); m = va_arg(ap, mode_t); va_end(ap);
	}
	return real_openat(dirfd, pathname, flags, m);
}

int open(const char *pathname, int flags, ...)
{
	if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
	const char *body = match(pathname);
	if (body) {
		int fd = memfd_for(body, strlen(body));
		if (fd >= 0) {
			fprintf(stderr, "nvkvm_shim: open(%s) -> memfd %d\n",
				pathname, fd);
			return fd;
		}
	}
	mode_t m = 0;
	if (flags & O_CREAT) {
		va_list ap; va_start(ap, flags); m = va_arg(ap, mode_t); va_end(ap);
	}
	return real_open(pathname, flags, m);
}

int access(const char *pathname, int mode)
{
	if (!real_access) real_access = dlsym(RTLD_NEXT, "access");
	if (match(pathname)) {
		fprintf(stderr, "nvkvm_shim: access(%s) -> OK\n", pathname);
		return 0;
	}
	return real_access(pathname, mode);
}
