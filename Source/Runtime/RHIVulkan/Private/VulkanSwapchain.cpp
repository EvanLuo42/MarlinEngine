#include "Diagnostics/Log.h"
#include "RHI/RhiLog.h"
#include "VulkanConvert.h"
#include "VulkanDevice.h"
#include "VulkanPlatform.h"

namespace Marlin
{
namespace
{
constexpr uint64 PresentWaitTimeout = 100'000'000;

VkColorSpaceKHR ColorSpace(RhiColorSpace colorSpace)
{
    switch (colorSpace)
    {
    case RhiColorSpace::Srgb:
        return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    case RhiColorSpace::Hdr10:
        return VK_COLOR_SPACE_HDR10_ST2084_EXT;
    case RhiColorSpace::ScRgb:
        return VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    }
    return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
}

VkPresentModeKHR PresentMode(RhiPresentMode mode)
{
    switch (mode)
    {
    case RhiPresentMode::Fifo:
        return VK_PRESENT_MODE_FIFO_KHR;
    case RhiPresentMode::Immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case RhiPresentMode::Mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkImageUsageFlags SwapchainUsage(RhiTextureUsage usage)
{
    VkImageUsageFlags flags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (HasAnyFlags(usage, RhiTextureUsage::UnorderedAccess))
    {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (HasAnyFlags(usage, RhiTextureUsage::ShaderResource))
    {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (HasAnyFlags(usage, RhiTextureUsage::CopyDest))
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (HasAnyFlags(usage, RhiTextureUsage::CopySource))
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    return flags;
}

VkSemaphore CreateBinarySemaphore(VkDevice device)
{
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    Vk.vkCreateSemaphore(device, &info, nullptr, &semaphore);
    return semaphore;
}
} // namespace

Result<RhiSwapchain, RhiError> VulkanDevice::CreateSwapchain(const RhiSwapchainDesc &desc)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult result = VulkanPlatform::CreateSurface(_backend.GetInstance(), desc.NativeWindow, surface);
    if (result != VK_SUCCESS)
    {
        return Failure(VulkanConvert::Error(result));
    }
    for (VulkanQueue &queue : _queues)
    {
        VkBool32 supported = VK_FALSE;
        Vk.vkGetPhysicalDeviceSurfaceSupportKHR(_adapter.PhysicalDevice, queue.Family, surface, &supported);
        queue.Present = supported == VK_TRUE;
    }
    if (!_queues[0].Present)
    {
        Vk.vkDestroySurfaceKHR(_backend.GetInstance(), surface, nullptr);
        return Failure(RhiError::Unsupported);
    }
    const RhiSwapchain handle = _swapchains.Allocate();
    if (!handle.IsValid())
    {
        Vk.vkDestroySurfaceKHR(_backend.GetInstance(), surface, nullptr);
        return Failure(RhiError::OutOfHostMemory);
    }
    VulkanSwapchain &swapchain = _swapchains.Get(handle);
    swapchain.Desc = desc;
    swapchain.Desc.DebugName = {};
    swapchain.Surface = surface;

    uint32 count = 0;
    Vk.vkGetPhysicalDeviceSurfaceFormatsKHR(_adapter.PhysicalDevice, surface, &count, nullptr);
    List<VkSurfaceFormatKHR> formats;
    formats.Resize(count);
    Vk.vkGetPhysicalDeviceSurfaceFormatsKHR(_adapter.PhysicalDevice, surface, &count, formats.Data());
    for (const RhiColorSpace colorSpace : {RhiColorSpace::Srgb, RhiColorSpace::Hdr10, RhiColorSpace::ScRgb})
    {
        const VkColorSpaceKHR vkColorSpace = ColorSpace(colorSpace);
        if (formats.Find([&](const VkSurfaceFormatKHR &format) { return format.colorSpace == vkColorSpace; }))
        {
            swapchain.ColorSpaces.Add(colorSpace);
        }
    }

    Result<void, RhiError> images = CreateSwapchainImages(swapchain, desc.Width, desc.Height, VK_NULL_HANDLE);
    if (images.IsError())
    {
        Vk.vkDestroySurfaceKHR(_backend.GetInstance(), surface, nullptr);
        _swapchains.Free(handle);
        return Failure(images.Error());
    }
    for (usize i = 0; i <= swapchain.Images.Count(); ++i)
    {
        swapchain.AcquireSemaphores.Add(CreateBinarySemaphore(_device));
        swapchain.AcquireSemaphoreValues.Add(0);
    }
    return handle;
}

Result<void, RhiError> VulkanDevice::CreateSwapchainImages(VulkanSwapchain &swapchain, uint32 width, uint32 height,
                                                           VkSwapchainKHR oldSwapchain)
{
    VkSurfaceCapabilitiesKHR capabilities{};
    Vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_adapter.PhysicalDevice, swapchain.Surface, &capabilities);
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0xFFFFFFFFu)
    {
        extent.width = width < capabilities.minImageExtent.width    ? capabilities.minImageExtent.width
                       : width > capabilities.maxImageExtent.width ? capabilities.maxImageExtent.width
                                                                   : width;
        extent.height = height < capabilities.minImageExtent.height    ? capabilities.minImageExtent.height
                        : height > capabilities.maxImageExtent.height ? capabilities.maxImageExtent.height
                                                                      : height;
    }
    if (extent.width == 0 || extent.height == 0)
    {
        return Failure(RhiError::SurfaceLost);
    }
    uint32 imageCount = swapchain.Desc.ImageCount < capabilities.minImageCount ? capabilities.minImageCount
                                                                               : swapchain.Desc.ImageCount;
    if (capabilities.maxImageCount != 0 && imageCount > capabilities.maxImageCount)
    {
        imageCount = capabilities.maxImageCount;
    }

    uint32 count = 0;
    Vk.vkGetPhysicalDeviceSurfaceFormatsKHR(_adapter.PhysicalDevice, swapchain.Surface, &count, nullptr);
    List<VkSurfaceFormatKHR> formats;
    formats.Resize(count);
    Vk.vkGetPhysicalDeviceSurfaceFormatsKHR(_adapter.PhysicalDevice, swapchain.Surface, &count, formats.Data());
    const VkFormat wanted = VulkanConvert::Format(swapchain.Desc.Format);
    const VkColorSpaceKHR wantedColorSpace = ColorSpace(swapchain.Desc.ColorSpace);
    const VkSurfaceFormatKHR *chosen = formats.Find([&](const VkSurfaceFormatKHR &format) {
        return format.format == wanted && format.colorSpace == wantedColorSpace;
    });
    if (chosen == nullptr)
    {
        MARLIN_LOG(LogRhi, Warning, "The surface has no {0} in the asked color space; using its first format",
                   GetFormatInfo(swapchain.Desc.Format).Name);
        chosen = &formats[0];
    }
    swapchain.Format = chosen->format;
    swapchain.ColorSpace = chosen->colorSpace;

    Vk.vkGetPhysicalDeviceSurfacePresentModesKHR(_adapter.PhysicalDevice, swapchain.Surface, &count, nullptr);
    List<VkPresentModeKHR> modes;
    modes.Resize(count);
    Vk.vkGetPhysicalDeviceSurfacePresentModesKHR(_adapter.PhysicalDevice, swapchain.Surface, &count, modes.Data());
    VkPresentModeKHR presentMode = PresentMode(swapchain.Desc.PresentMode);
    if (!modes.Contains(presentMode))
    {
        presentMode = VK_PRESENT_MODE_FIFO_KHR;
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = swapchain.Surface;
    info.minImageCount = imageCount;
    info.imageFormat = swapchain.Format;
    info.imageColorSpace = swapchain.ColorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = SwapchainUsage(swapchain.Desc.Usage) & capabilities.supportedUsageFlags;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    const VkResult result = Vk.vkCreateSwapchainKHR(_device, &info, nullptr, &created);
    if (result != VK_SUCCESS)
    {
        return Failure(VulkanConvert::Error(result));
    }
    swapchain.Swapchain = created;
    Vk.vkGetSwapchainImagesKHR(_device, created, &count, nullptr);
    List<VkImage> images;
    images.Resize(count);
    Vk.vkGetSwapchainImagesKHR(_device, created, &count, images.Data());
    for (const VkImage image : images)
    {
        const RhiTexture handle = _textures.Allocate();
        VulkanTexture &texture = _textures.Get(handle);
        texture.Image = image;
        texture.OwnsImage = false;
        texture.Format = swapchain.Format;
        texture.Aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        texture.Desc.Format = swapchain.Desc.Format;
        texture.Desc.Width = extent.width;
        texture.Desc.Height = extent.height;
        texture.Desc.Usage = swapchain.Desc.Usage | RhiTextureUsage::Present;
        swapchain.Images.Add(handle);
        swapchain.PresentSemaphores.Add(CreateBinarySemaphore(_device));
    }
    swapchain.Desc.Width = extent.width;
    swapchain.Desc.Height = extent.height;
    swapchain.CurrentImage = ~0u;
    ApplyHdrMetadata(swapchain);
    return {};
}

void VulkanDevice::ApplyHdrMetadata(const VulkanSwapchain &swapchain) const
{
    if (!swapchain.HasHdrMetadata || Vk.vkSetHdrMetadataEXT == nullptr)
    {
        return;
    }
    const RhiHdrMetadata &source = swapchain.HdrMetadata;
    VkHdrMetadataEXT metadata{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
    metadata.displayPrimaryRed = {source.RedPrimary[0], source.RedPrimary[1]};
    metadata.displayPrimaryGreen = {source.GreenPrimary[0], source.GreenPrimary[1]};
    metadata.displayPrimaryBlue = {source.BluePrimary[0], source.BluePrimary[1]};
    metadata.whitePoint = {source.WhitePoint[0], source.WhitePoint[1]};
    metadata.maxLuminance = source.MaxLuminance;
    metadata.minLuminance = source.MinLuminance;
    metadata.maxContentLightLevel = source.MaxContentLightLevel;
    metadata.maxFrameAverageLightLevel = source.MaxFrameAverageLightLevel;
    Vk.vkSetHdrMetadataEXT(_device, 1, &swapchain.Swapchain, &metadata);
}

Result<void, RhiError> VulkanDevice::SetHdrMetadata(RhiSwapchain swapchain, const RhiHdrMetadata &metadata)
{
    VulkanSwapchain *entry = _swapchains.Find(swapchain);
    if (entry == nullptr || Vk.vkSetHdrMetadataEXT == nullptr)
    {
        return Failure(RhiError::Unsupported);
    }
    entry->HdrMetadata = metadata;
    entry->HasHdrMetadata = true;
    ApplyHdrMetadata(*entry);
    return {};
}

void VulkanDevice::DestroySwapchainImages(VulkanSwapchain &swapchain)
{
    for (const RhiTexture handle : swapchain.Images)
    {
        if (VulkanTexture *texture = _textures.Find(handle))
        {
            for (const VulkanAttachmentView &view : texture->AttachmentViews)
            {
                Vk.vkDestroyImageView(_device, view.View, nullptr);
            }
            _textures.Free(handle);
        }
    }
    swapchain.Images.Clear();
    for (const VkSemaphore semaphore : swapchain.PresentSemaphores)
    {
        Vk.vkDestroySemaphore(_device, semaphore, nullptr);
    }
    swapchain.PresentSemaphores.Clear();
}

void VulkanDevice::Destroy(RhiSwapchain swapchain)
{
    VulkanSwapchain *entry = _swapchains.Find(swapchain);
    if (entry == nullptr)
    {
        return;
    }
    // An acquire semaphore must not be destroyed with its signal still unwaited.
    (void)FlushPendingAcquires();
    Vk.vkDeviceWaitIdle(_device);
    DestroySwapchainImages(*entry);
    for (const VkSemaphore semaphore : entry->AcquireSemaphores)
    {
        Vk.vkDestroySemaphore(_device, semaphore, nullptr);
    }
    Vk.vkDestroySwapchainKHR(_device, entry->Swapchain, nullptr);
    Vk.vkDestroySurfaceKHR(_backend.GetInstance(), entry->Surface, nullptr);
    _swapchains.Free(swapchain);
}

Result<void, RhiError> VulkanDevice::ResizeSwapchain(RhiSwapchain swapchain, uint32 width, uint32 height)
{
    VulkanSwapchain *entry = _swapchains.Find(swapchain);
    if (entry == nullptr)
    {
        return Failure(RhiError::Unsupported);
    }
    (void)FlushPendingAcquires();
    Vk.vkDeviceWaitIdle(_device);
    DestroySwapchainImages(*entry);
    const VkSwapchainKHR old = entry->Swapchain;
    Result<void, RhiError> result = CreateSwapchainImages(*entry, width, height, old);
    if (result.IsOk())
    {
        Vk.vkDestroySwapchainKHR(_device, old, nullptr);
    }
    return result;
}

Result<void, RhiError> VulkanDevice::SetPresentMode(RhiSwapchain swapchain, RhiPresentMode mode)
{
    VulkanSwapchain *entry = _swapchains.Find(swapchain);
    if (entry == nullptr)
    {
        return Failure(RhiError::Unsupported);
    }
    entry->Desc.PresentMode = mode;
    return ResizeSwapchain(swapchain, entry->Desc.Width, entry->Desc.Height);
}

Span<const RhiColorSpace> VulkanDevice::GetSupportedColorSpaces(RhiSwapchain swapchain) const
{
    const VulkanSwapchain *entry = _swapchains.Find(swapchain);
    return entry != nullptr ? Span<const RhiColorSpace>(entry->ColorSpaces) : Span<const RhiColorSpace>();
}

Result<void, RhiError> VulkanDevice::SetColorSpace(RhiSwapchain swapchain, RhiColorSpace colorSpace,
                                                   RhiFormat format)
{
    VulkanSwapchain *entry = _swapchains.Find(swapchain);
    if (entry == nullptr || !entry->ColorSpaces.Contains(colorSpace))
    {
        return Failure(RhiError::Unsupported);
    }
    entry->Desc.ColorSpace = colorSpace;
    entry->Desc.Format = format;
    return ResizeSwapchain(swapchain, entry->Desc.Width, entry->Desc.Height);
}

void VulkanDevice::WaitForPresent(RhiSwapchain swapchain)
{
    VulkanSwapchain *entry = _swapchains.Find(swapchain);
    if (entry == nullptr || entry->PresentCount <= entry->Desc.MaxFrameLatency)
    {
        return;
    }
    const uint64 target = entry->PresentCount - entry->Desc.MaxFrameLatency;
    if (_presentWait)
    {
        CheckLost(Vk.vkWaitForPresentKHR(_device, entry->Swapchain, target, PresentWaitTimeout));
        return;
    }
    const usize index = static_cast<usize>(target - 1) % entry->PresentValues.Count();
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &_queues[entry->PresentQueue].Timeline;
    info.pValues = &entry->PresentValues[index];
    CheckLost(Vk.vkWaitSemaphores(_device, &info, ~uint64{0}));
}

Result<RhiTexture, RhiError> VulkanDevice::AcquireNextImage(RhiSwapchain swapchain)
{
    VulkanSwapchain *entry = _swapchains.Find(swapchain);
    if (entry == nullptr)
    {
        return Failure(RhiError::Unsupported);
    }
    VulkanQueue &graphics = _queues[static_cast<uint32>(RhiQueueType::Graphics)];
    const uint32 slot = entry->NextAcquireSemaphore;
    entry->NextAcquireSemaphore = (slot + 1) % static_cast<uint32>(entry->AcquireSemaphores.Count());
    if (entry->AcquireSemaphoreValues[slot] == VulkanAcquirePending)
    {
        // Acquired again before any graphics submission waited for it.
        Result<void, RhiError> flushed = FlushPendingAcquires();
        if (flushed.IsError())
        {
            return Failure(flushed.Error());
        }
    }
    if (entry->AcquireSemaphoreValues[slot] != 0)
    {
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.semaphoreCount = 1;
        wait.pSemaphores = &graphics.Timeline;
        wait.pValues = &entry->AcquireSemaphoreValues[slot];
        CheckLost(Vk.vkWaitSemaphores(_device, &wait, ~uint64{0}));
    }
    const VkSemaphore semaphore = entry->AcquireSemaphores[slot];
    uint32 image = 0;
    const VkResult result =
        Vk.vkAcquireNextImageKHR(_device, entry->Swapchain, ~uint64{0}, semaphore, VK_NULL_HANDLE, &image);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        CheckLost(result);
        return Failure(VulkanConvert::Error(result));
    }
    // The next graphics submission waits for the image, at the stages that first write it; work on other queues
    // reaches the image through a wait for graphics.
    entry->AcquireSemaphoreValues[slot] = VulkanAcquirePending;
    graphics.PendingAcquires.Add({semaphore, swapchain, slot});
    entry->CurrentAcquireSemaphore = slot;
    entry->CurrentImage = image;
    return entry->Images[image];
}

Result<void, RhiError> VulkanDevice::Present(RhiSwapchain swapchain, RhiQueueType queueType)
{
    VulkanSwapchain *entry = _swapchains.Find(swapchain);
    if (entry == nullptr || entry->CurrentImage == ~0u)
    {
        return Failure(RhiError::Unsupported);
    }
    uint32 queueIndex = static_cast<uint32>(queueType);
    if (!_queues[queueIndex].Present)
    {
        queueIndex = static_cast<uint32>(RhiQueueType::Graphics);
    }
    VulkanQueue &queue = _queues[queueIndex];
    if (queueIndex != static_cast<uint32>(RhiQueueType::Graphics))
    {
        // Presenting from another queue: the image must still be available first.
        Result<void, RhiError> flushed = FlushPendingAcquires();
        if (flushed.IsError())
        {
            return flushed;
        }
        AddPendingWait(queue, _queues[static_cast<uint32>(RhiQueueType::Graphics)].Timeline,
                       entry->AcquireSemaphoreValues[entry->CurrentAcquireSemaphore],
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    }
    const VkSemaphore semaphore = entry->PresentSemaphores[entry->CurrentImage];
    uint64 value = 0;
    Result<void, RhiError> submitted = SubmitEmpty(queue, semaphore, value);
    if (submitted.IsError())
    {
        return submitted;
    }
    ++entry->PresentCount;
    VkPresentIdKHR presentId{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    presentId.swapchainCount = 1;
    presentId.pPresentIds = &entry->PresentCount;
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.pNext = _presentWait ? &presentId : nullptr;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &entry->Swapchain;
    info.pImageIndices = &entry->CurrentImage;
    const VkResult result = Vk.vkQueuePresentKHR(queue.Queue, &info);
    entry->CurrentImage = ~0u;
    entry->PresentQueue = queueIndex;
    const usize latency = entry->Desc.MaxFrameLatency + 1;
    if (entry->PresentValues.Count() < latency)
    {
        entry->PresentValues.Add(value);
    }
    else
    {
        entry->PresentValues[static_cast<usize>(entry->PresentCount - 1) % latency] = value;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        CheckLost(result);
        return Failure(VulkanConvert::Error(result));
    }
    return {};
}
} // namespace Marlin
