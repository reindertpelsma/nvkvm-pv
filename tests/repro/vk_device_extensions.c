/* vk_device_extensions.c -- does vkCreateDevice succeed with a given set of
 * device extensions enabled?
 *
 * THIS IS THE RDR2 REGRESSION TEST. Red Dead Redemption 2 dies at ERR_GFX_INIT
 * in an nvkvm guest, and the cause is here, not in the game: seven advertised
 * device extensions make vkCreateDevice return VK_ERROR_INITIALIZATION_FAILED,
 * each independently. The launcher asks for D3D12, vkd3d-proton enables the
 * ray-tracing extensions to expose DXR, device creation fails, the game stops.
 * DXVK survives the same box only by falling back to a reduced "safe mode" set.
 *
 * MEASURED 2026-09-01 on an RTX 4070 / driver 595.84, with the control that
 * makes it a finding: the identical binary on bare metal succeeds for all seven
 * individually AND for the full 275-extension advertised set. Three runs each
 * side, deterministic both ways. Largest set that succeeds in the guest: 268.
 *
 *     VK_KHR_acceleration_structure   VK_KHR_ray_query
 *     VK_KHR_ray_tracing_pipeline     VK_NV_ray_tracing
 *     VK_NV_optical_flow              VK_NV_cuda_kernel_launch
 *     VK_NVX_binary_import
 *
 * All seven are ADVERTISED as supported -- vulkaninfo is happy, because
 * enumerating a physical device is cheap and creating a logical one is what
 * breaks. An application cannot tell in advance except by trying.
 *
 * dlopen()s libvulkan rather than linking it, so it builds anywhere a compiler
 * exists and needs no Vulkan headers or loader at build time.
 *
 * Root cause OPEN as of this commit. This file is the acceptance criterion: when
 * it exits 0 with the seven extensions above, the bug is fixed.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *vkresult_str(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        default: return "VK_ERROR_<other>";
    }
}

#define LOAD_INSTANCE_PROC(inst, name) \
    PFN_##name name = (PFN_##name)vkGetInstanceProcAddr(inst, #name); \
    if (!name) { fprintf(stderr, "FATAL: could not resolve %s\n", #name); exit(2); }

int main(int argc, char **argv) {
    int gpu_index = 0;
    int list_mode = 0;
    int argi = 1;

    /* crude arg parse: --gpu N and --list can appear before the extension list */
    while (argi < argc) {
        if (strcmp(argv[argi], "--gpu") == 0 && argi + 1 < argc) {
            gpu_index = atoi(argv[argi + 1]);
            argi += 2;
        } else if (strcmp(argv[argi], "--list") == 0) {
            list_mode = 1;
            argi += 1;
        } else {
            break;
        }
    }
    const char **want_exts = (const char **)&argv[argi];
    int want_ext_count = argc - argi;

    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "FATAL: dlopen(libvulkan.so.1) failed: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) {
        fprintf(stderr, "FATAL: dlsym(vkGetInstanceProcAddr) failed: %s\n", dlerror());
        return 2;
    }

    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    if (!vkCreateInstance) {
        fprintf(stderr, "FATAL: could not resolve vkCreateInstance\n");
        return 2;
    }

    uint32_t api_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) {
        vkEnumerateInstanceVersion(&api_version);
    }
    printf("loader instance API version: %u.%u.%u\n",
           VK_API_VERSION_MAJOR(api_version), VK_API_VERSION_MINOR(api_version),
           VK_API_VERSION_PATCH(api_version));

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vkdevprobe",
        .applicationVersion = 1,
        .pEngineName = "vkdevprobe",
        .engineVersion = 1,
        .apiVersion = api_version >= VK_API_VERSION_1_1 ? api_version : VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo inst_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };

    VkInstance instance;
    VkResult r = vkCreateInstance(&inst_info, NULL, &instance);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "FATAL: vkCreateInstance failed: %d (%s)\n", r, vkresult_str(r));
        return 2;
    }

    LOAD_INSTANCE_PROC(instance, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE_PROC(instance, vkGetPhysicalDeviceProperties);
    LOAD_INSTANCE_PROC(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE_PROC(instance, vkEnumerateDeviceExtensionProperties);
    LOAD_INSTANCE_PROC(instance, vkCreateDevice);
    LOAD_INSTANCE_PROC(instance, vkDestroyDevice);
    LOAD_INSTANCE_PROC(instance, vkDestroyInstance);

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, NULL);
    if (gpu_count == 0) {
        fprintf(stderr, "FATAL: zero physical devices enumerated\n");
        return 2;
    }
    VkPhysicalDevice *gpus = calloc(gpu_count, sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus);
    if (gpu_index < 0 || (uint32_t)gpu_index >= gpu_count) {
        fprintf(stderr, "FATAL: --gpu %d out of range (0..%u)\n", gpu_index, gpu_count - 1);
        return 2;
    }
    VkPhysicalDevice phys = gpus[gpu_index];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("physical device [%d/%u]: %s (deviceType=%d, driverVersion=0x%x, apiVersion=%u.%u.%u)\n",
           gpu_index, gpu_count, props.deviceName, props.deviceType, props.driverVersion,
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
           VK_API_VERSION_PATCH(props.apiVersion));

    uint32_t avail_count = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &avail_count, NULL);
    VkExtensionProperties *avail = calloc(avail_count, sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &avail_count, avail);

    if (list_mode) {
        printf("device extensions advertised (%u):\n", avail_count);
        for (uint32_t i = 0; i < avail_count; i++) {
            printf("%s\n", avail[i].extensionName);
        }
        vkDestroyInstance(instance, NULL);
        return 0;
    }

    /* find a queue family with GRAPHICS (RDR2/vkd3d/dxvk all need a graphics queue) */
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, NULL);
    VkQueueFamilyProperties *qfs = calloc(qf_count, sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, qfs);
    int gfx_family = -1;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx_family = (int)i; break; }
    }
    if (gfx_family < 0) {
        fprintf(stderr, "FATAL: no graphics-capable queue family\n");
        return 2;
    }

    printf("requested %d device extension(s):\n", want_ext_count);
    for (int i = 0; i < want_ext_count; i++) printf("  %s\n", want_exts[i]);

    /* verify every requested extension is actually in the advertised set,
     * so a failure can't be misread as a typo'd extension name */
    for (int i = 0; i < want_ext_count; i++) {
        int found = 0;
        for (uint32_t j = 0; j < avail_count; j++) {
            if (strcmp(want_exts[i], avail[j].extensionName) == 0) { found = 1; break; }
        }
        if (!found) {
            fprintf(stderr, "FATAL: requested extension %s not in advertised set\n", want_exts[i]);
            return 2;
        }
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = (uint32_t)gfx_family,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = (uint32_t)want_ext_count,
        .ppEnabledExtensionNames = want_exts,
    };

    VkDevice device = VK_NULL_HANDLE;
    VkResult dr = vkCreateDevice(phys, &dci, NULL, &device);
    printf("vkCreateDevice -> %d (%s)\n", dr, vkresult_str(dr));
    if (dr == VK_SUCCESS) {
        vkDestroyDevice(device, NULL);
    }

    vkDestroyInstance(instance, NULL);

    printf("RESULT: %d %s\n", dr, vkresult_str(dr));
    return dr == VK_SUCCESS ? 0 : 1;
}
