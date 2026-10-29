#pragma once

#include "CoreTypes.h"

#include <vulkan/vulkan_core.h>

// clang-format off
#define MARLIN_VULKAN_GLOBAL_FUNCTIONS(X)                                                                              \
    X(vkCreateInstance)                                                                                                \
    X(vkEnumerateInstanceVersion)                                                                                      \
    X(vkEnumerateInstanceExtensionProperties)                                                                          \
    X(vkEnumerateInstanceLayerProperties)

#define MARLIN_VULKAN_INSTANCE_FUNCTIONS(X)                                                                            \
    X(vkDestroyInstance)                                                                                               \
    X(vkEnumeratePhysicalDevices)                                                                                      \
    X(vkGetPhysicalDeviceProperties2)                                                                                  \
    X(vkGetPhysicalDeviceFeatures2)                                                                                    \
    X(vkGetPhysicalDeviceQueueFamilyProperties2)                                                                       \
    X(vkGetPhysicalDeviceMemoryProperties2)                                                                            \
    X(vkGetPhysicalDeviceFormatProperties2)                                                                            \
    X(vkGetPhysicalDeviceImageFormatProperties2)                                                                       \
    X(vkEnumerateDeviceExtensionProperties)                                                                            \
    X(vkCreateDevice)                                                                                                  \
    X(vkGetDeviceProcAddr)                                                                                             \
    X(vkCreateDebugUtilsMessengerEXT)                                                                                  \
    X(vkDestroyDebugUtilsMessengerEXT)                                                                                 \
    X(vkDestroySurfaceKHR)                                                                                             \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                                       \
    X(vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)                                                                  \
    X(vkGetPhysicalDeviceDescriptorSizeEXT)

#define MARLIN_VULKAN_DEVICE_FUNCTIONS(X)                                                                              \
    X(vkDestroyDevice)                                                                                                 \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkDeviceWaitIdle)                                                                                                \
    X(vkQueueSubmit2)                                                                                                  \
    X(vkQueueWaitIdle)                                                                                                 \
    X(vkQueueBindSparse)                                                                                               \
    X(vkQueuePresentKHR)                                                                                               \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkMapMemory)                                                                                                     \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkCreateImage)                                                                                                   \
    X(vkDestroyImage)                                                                                                  \
    X(vkBindBufferMemory)                                                                                              \
    X(vkBindImageMemory)                                                                                               \
    X(vkGetDeviceBufferMemoryRequirements)                                                                             \
    X(vkGetDeviceImageMemoryRequirements)                                                                              \
    X(vkGetBufferMemoryRequirements2)                                                                                  \
    X(vkGetImageMemoryRequirements2)                                                                                   \
    X(vkGetImageSparseMemoryRequirements2)                                                                             \
    X(vkGetBufferDeviceAddress)                                                                                        \
    X(vkCreateImageView)                                                                                               \
    X(vkDestroyImageView)                                                                                              \
    X(vkCreateSemaphore)                                                                                               \
    X(vkDestroySemaphore)                                                                                              \
    X(vkWaitSemaphores)                                                                                                \
    X(vkSignalSemaphore)                                                                                               \
    X(vkGetSemaphoreCounterValue)                                                                                      \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkResetCommandPool)                                                                                              \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkCreateQueryPool)                                                                                               \
    X(vkDestroyQueryPool)                                                                                              \
    X(vkResetQueryPool)                                                                                                \
    X(vkGetQueryPoolResults)                                                                                           \
    X(vkCreatePipelineCache)                                                                                           \
    X(vkDestroyPipelineCache)                                                                                          \
    X(vkGetPipelineCacheData)                                                                                          \
    X(vkCreateGraphicsPipelines)                                                                                       \
    X(vkCreateComputePipelines)                                                                                        \
    X(vkCreateRayTracingPipelinesKHR)                                                                                  \
    X(vkDestroyPipeline)                                                                                               \
    X(vkGetRayTracingShaderGroupHandlesKHR)                                                                            \
    X(vkCreateAccelerationStructureKHR)                                                                                \
    X(vkDestroyAccelerationStructureKHR)                                                                               \
    X(vkGetAccelerationStructureBuildSizesKHR)                                                                         \
    X(vkGetAccelerationStructureDeviceAddressKHR)                                                                      \
    X(vkCreateSwapchainKHR)                                                                                            \
    X(vkDestroySwapchainKHR)                                                                                           \
    X(vkGetSwapchainImagesKHR)                                                                                         \
    X(vkAcquireNextImageKHR)                                                                                           \
    X(vkWaitForPresentKHR)                                                                                             \
    X(vkSetHdrMetadataEXT)                                                                                             \
    X(vkGetGeneratedCommandsMemoryRequirementsEXT)                                                                     \
    X(vkCmdExecuteGeneratedCommandsEXT)                                                                                \
    X(vkCreateIndirectCommandsLayoutEXT)                                                                               \
    X(vkDestroyIndirectCommandsLayoutEXT)                                                                              \
    X(vkCreateIndirectExecutionSetEXT)                                                                                 \
    X(vkDestroyIndirectExecutionSetEXT)                                                                                \
    X(vkUpdateIndirectExecutionSetPipelineEXT)                                                                         \
    X(vkSetDebugUtilsObjectNameEXT)                                                                                    \
    X(vkWriteResourceDescriptorsEXT)                                                                                   \
    X(vkWriteSamplerDescriptorsEXT)                                                                                    \
    X(vkGetCalibratedTimestampsKHR)                                                                                    \
    X(vkCmdPipelineBarrier2)                                                                                           \
    X(vkCmdBeginRendering)                                                                                             \
    X(vkCmdEndRendering)                                                                                               \
    X(vkCmdBindPipeline)                                                                                               \
    X(vkCmdPushDataEXT)                                                                                                \
    X(vkCmdBindResourceHeapEXT)                                                                                        \
    X(vkCmdBindSamplerHeapEXT)                                                                                         \
    X(vkCmdSetViewportWithCount)                                                                                       \
    X(vkCmdSetScissorWithCount)                                                                                        \
    X(vkCmdSetStencilReference)                                                                                        \
    X(vkCmdSetBlendConstants)                                                                                          \
    X(vkCmdSetDepthBounds)                                                                                             \
    X(vkCmdSetFragmentShadingRateKHR)                                                                                  \
    X(vkCmdBindIndexBuffer2)                                                                                           \
    X(vkCmdDraw)                                                                                                       \
    X(vkCmdDrawIndexed)                                                                                                \
    X(vkCmdDrawMeshTasksEXT)                                                                                           \
    X(vkCmdDispatch)                                                                                                   \
    X(vkCmdTraceRaysKHR)                                                                                               \
    X(vkCmdTraceRaysIndirect2KHR)                                                                                      \
    X(vkCmdDrawIndirect)                                                                                               \
    X(vkCmdDrawIndexedIndirect)                                                                                        \
    X(vkCmdDrawMeshTasksIndirectEXT)                                                                                   \
    X(vkCmdDrawIndirectCount)                                                                                          \
    X(vkCmdDrawIndexedIndirectCount)                                                                                   \
    X(vkCmdDrawMeshTasksIndirectCountEXT)                                                                              \
    X(vkCmdDispatchIndirect)                                                                                           \
    X(vkCmdCopyBuffer2)                                                                                                \
    X(vkCmdCopyImage2)                                                                                                 \
    X(vkCmdCopyBufferToImage2)                                                                                         \
    X(vkCmdCopyImageToBuffer2)                                                                                         \
    X(vkCmdFillBuffer)                                                                                                 \
    X(vkCmdClearColorImage)                                                                                            \
    X(vkCmdClearDepthStencilImage)                                                                                     \
    X(vkCmdResolveImage2)                                                                                              \
    X(vkCmdBuildAccelerationStructuresKHR)                                                                             \
    X(vkCmdCopyAccelerationStructureKHR)                                                                               \
    X(vkCmdCopyAccelerationStructureToMemoryKHR)                                                                       \
    X(vkCmdCopyMemoryToAccelerationStructureKHR)                                                                       \
    X(vkCmdWriteAccelerationStructuresPropertiesKHR)                                                                   \
    X(vkCmdWriteTimestamp2)                                                                                            \
    X(vkCmdBeginQuery)                                                                                                 \
    X(vkCmdEndQuery)                                                                                                   \
    X(vkCmdCopyQueryPoolResults)                                                                                       \
    X(vkCmdResetQueryPool)                                                                                             \
    X(vkCmdBeginDebugUtilsLabelEXT)                                                                                    \
    X(vkCmdEndDebugUtilsLabelEXT)                                                                                      \
    X(vkCmdWriteBufferMarker2AMD)
// clang-format on

namespace Marlin
{
/// The Vulkan functions the backend calls, loaded at run time; extension functions are null when the extension is
/// not enabled. There is one table per process: one instance and one device.
struct VulkanApi
{
#define MARLIN_VULKAN_DECLARE(name) PFN_##name name = nullptr;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    MARLIN_VULKAN_GLOBAL_FUNCTIONS(MARLIN_VULKAN_DECLARE)
    MARLIN_VULKAN_INSTANCE_FUNCTIONS(MARLIN_VULKAN_DECLARE)
    MARLIN_VULKAN_DEVICE_FUNCTIONS(MARLIN_VULKAN_DECLARE)
#undef MARLIN_VULKAN_DECLARE

    /// Opens the loader, or *library* in its place, such as an SDK's interposer; false if it cannot be opened.
    bool LoadGlobal(const char *library = nullptr);
    void LoadInstance(VkInstance instance);
    void LoadDevice(VkDevice device);
    void Unload();

    void *Library = nullptr;
};

RHIVULKAN_API extern VulkanApi Vk;
} // namespace Marlin
