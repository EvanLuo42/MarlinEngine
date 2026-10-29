#pragma once

#include "CoreTypes.h"
#include "RHIVulkan/VulkanApi.h"
#include "String/StringBuilder.h"

namespace Marlin
{
struct VulkanAdapter;

/// The device extensions of the hardware baseline (Documentation/Architecture/07 §1.5).
inline constexpr const char *VulkanRequiredExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
    // RECOVERY-DECISION D4: required, since the SPIR-V backend always pairs untyped pointers with the heap.
    VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
    VK_EXT_MESH_SHADER_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
};

/// Device extensions enabled when present.
inline constexpr const char *VulkanOptionalExtensions[] = {
    VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME,
    VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME,
    VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME,
    VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME,
    VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME,
    VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
    VK_KHR_PRESENT_ID_EXTENSION_NAME,
    VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
    VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
    VK_AMD_BUFFER_MARKER_EXTENSION_NAME,
    VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME,
    VK_EXT_HDR_METADATA_EXTENSION_NAME,
    VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
};

/// The feature structures of every extension the backend may enable, linked for the adapter's extensions.
struct VulkanFeatureChain
{
    VkPhysicalDeviceFeatures2 Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features Vulkan11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features Vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features Vulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features Vulkan14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceDescriptorHeapFeaturesEXT DescriptorHeap{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    VkPhysicalDeviceShaderUntypedPointersFeaturesKHR UntypedPointers{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR};
    VkPhysicalDeviceMeshShaderFeaturesEXT MeshShader{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR AccelerationStructure{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR RayTracingPipeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR RayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR RayTracingMaintenance1{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR ShadingRate{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT ImageAtomic64{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT InvocationReorder{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT};
    VkPhysicalDeviceCooperativeVectorFeaturesNV CooperativeVector{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_VECTOR_FEATURES_NV};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR CooperativeMatrix{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDevicePresentIdFeaturesKHR PresentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR PresentWait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT GeneratedCommands{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT};

    VulkanFeatureChain() = default;
    MARLIN_NONCOPYABLE(VulkanFeatureChain);

    /// Links the structures of the extensions *adapter* has and queries them.
    void Query(const VulkanAdapter &adapter);
    /// Writes the baseline features the adapter lacks; false if any.
    bool CheckBaseline(StringWriter &missing) const;
    /// Turns off what costs speed and is not wanted: robustness and capture replay.
    void DisableUnwanted();
};

struct VulkanPropertyChain
{
    VkPhysicalDeviceProperties2 Properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceVulkan11Properties Vulkan11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties Vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties Vulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceDescriptorHeapPropertiesEXT DescriptorHeap{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    VkPhysicalDeviceMeshShaderPropertiesEXT MeshShader{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR AccelerationStructure{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR RayTracingPipeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR ShadingRate{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
    VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT GeneratedCommands{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_PROPERTIES_EXT};

    VulkanPropertyChain() = default;
    MARLIN_NONCOPYABLE(VulkanPropertyChain);

    void Query(const VulkanAdapter &adapter);
};
} // namespace Marlin
