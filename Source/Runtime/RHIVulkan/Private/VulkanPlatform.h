#pragma once

#include "RHIVulkan/VulkanApi.h"

/// What differs between the platforms Vulkan runs on; each platform layer implements it.
namespace Marlin::VulkanPlatform
{
/// The loader library, such as vulkan-1.dll.
const char *GetLoaderName();
/// The instance extension for window surfaces, such as VK_KHR_win32_surface.
const char *GetSurfaceExtension();
/// A surface for an OS window, such as an HWND.
VkResult CreateSurface(VkInstance instance, void *nativeWindow, VkSurfaceKHR &surface);
/// The time domain of PlatformTime::Ticks, for calibrated timestamps.
VkTimeDomainKHR GetHostTimeDomain();
} // namespace Marlin::VulkanPlatform
