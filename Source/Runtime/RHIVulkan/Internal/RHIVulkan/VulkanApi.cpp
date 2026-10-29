#include "RHIVulkan/VulkanApi.h"

#include "HAL/PlatformLibrary.h"
#include "VulkanPlatform.h"

namespace Marlin
{
VulkanApi Vk;

bool VulkanApi::LoadGlobal(const char *path)
{
    const LibraryHandle library = PlatformLibrary::Load(path != nullptr ? path : VulkanPlatform::GetLoaderName());
    if (!library.IsValid())
    {
        return false;
    }
    Library = library.Value;
    vkGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(PlatformLibrary::FindSymbol(library, "vkGetInstanceProcAddr"));
    if (vkGetInstanceProcAddr == nullptr)
    {
        Unload();
        return false;
    }
#define MARLIN_VULKAN_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
    MARLIN_VULKAN_GLOBAL_FUNCTIONS(MARLIN_VULKAN_LOAD)
#undef MARLIN_VULKAN_LOAD
    return vkCreateInstance != nullptr && vkEnumerateInstanceVersion != nullptr;
}

void VulkanApi::LoadInstance(VkInstance instance)
{
#define MARLIN_VULKAN_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
    MARLIN_VULKAN_INSTANCE_FUNCTIONS(MARLIN_VULKAN_LOAD)
#undef MARLIN_VULKAN_LOAD
}

void VulkanApi::LoadDevice(VkDevice device)
{
#define MARLIN_VULKAN_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    MARLIN_VULKAN_DEVICE_FUNCTIONS(MARLIN_VULKAN_LOAD)
#undef MARLIN_VULKAN_LOAD
}

void VulkanApi::Unload()
{
    if (Library != nullptr)
    {
        PlatformLibrary::Unload({Library});
    }
    *this = VulkanApi{};
}
} // namespace Marlin
