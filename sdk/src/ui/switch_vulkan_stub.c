/*
 * futari-nx (2026-10-20): the Switch build without Mesa's NVK (REXGLUE_SWITCH_LINK_NVK OFF; app/CMakeLists.txt
 * FUTARI_SWITCH_NVK, the user's request: deko3d only). These are the symbols the code still referenced from Mesa's
 * libvulkan.a once it was out of the link (the linker's list): the Vulkan entry points of the SDK's Vulkan code and of
 * plume's Vulkan back end, answered as a system without a Vulkan driver answers (vkCreateInstance:
 * VK_ERROR_INCOMPATIBLE_DRIVER; no extensions, no layers; every other function unknown), and posix_memalign, which
 * Mesa's archive happened to supply to libavutil (newlib has memalign).
 */
#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <string.h>

#include <vulkan/vulkan.h>

static VKAPI_ATTR VkResult VKAPI_CALL StubCreateInstance(const VkInstanceCreateInfo *info,
                                                         const VkAllocationCallbacks *allocator,
                                                         VkInstance *instance) {
  (void)info;
  (void)allocator;
  if (instance)
    *instance = VK_NULL_HANDLE;
  return VK_ERROR_INCOMPATIBLE_DRIVER;
}

static VKAPI_ATTR VkResult VKAPI_CALL StubEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                               VkExtensionProperties *properties) {
  (void)layer;
  (void)properties;
  if (count)
    *count = 0;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL StubEnumerateInstanceLayerProperties(uint32_t *count,
                                                                           VkLayerProperties *properties) {
  (void)properties;
  if (count)
    *count = 0;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL StubEnumerateInstanceVersion(uint32_t *version) {
  if (version)
    *version = VK_API_VERSION_1_0;
  return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  (void)instance;
  if (!name)
    return NULL;
  if (strcmp(name, "vkCreateInstance") == 0)
    return (PFN_vkVoidFunction)StubCreateInstance;
  if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
    return (PFN_vkVoidFunction)StubEnumerateInstanceExtensionProperties;
  if (strcmp(name, "vkEnumerateInstanceLayerProperties") == 0)
    return (PFN_vkVoidFunction)StubEnumerateInstanceLayerProperties;
  if (strcmp(name, "vkEnumerateInstanceVersion") == 0)
    return (PFN_vkVoidFunction)StubEnumerateInstanceVersion;
  if (strcmp(name, "vkGetInstanceProcAddr") == 0)
    return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
  return NULL;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
  (void)instance;
  (void)allocator;
}

/* The loader-ICD interface plume's Vulkan back end talks to on the Switch. */
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version) {
  (void)version;
  return VK_ERROR_INCOMPATIBLE_DRIVER;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
  return vkGetInstanceProcAddr(instance, name);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
  if (!memptr || alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
    return EINVAL;
  void *p = memalign(alignment, size ? size : 1);
  if (!p)
    return ENOMEM;
  *memptr = p;
  return 0;
}
