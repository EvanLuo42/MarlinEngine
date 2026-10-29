#include "VulkanPlatform.h"

#include "HAL/MicrosoftHeaders.h"

#include <vulkan/vulkan_win32.h>

namespace Marlin::VulkanPlatform
{
const char *GetLoaderName()
{
    return "vulkan-1.dll";
}

const char *GetSurfaceExtension()
{
    return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
}

VkResult CreateSurface(VkInstance instance, void *nativeWindow, VkSurfaceKHR &surface)
{
    const auto create =
        reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(Vk.vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    if (create == nullptr)
    {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = static_cast<HWND>(nativeWindow);
    return create(instance, &info, nullptr, &surface);
}

VkTimeDomainKHR GetHostTimeDomain()
{
    return VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
}
} // namespace Marlin::VulkanPlatform
