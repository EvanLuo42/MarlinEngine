#include "VulkanFeatures.h"

#include "RHIVulkan/VulkanBackend.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
template <typename T> void Link(void **&tail, T &structure)
{
    structure.pNext = nullptr;
    *tail = &structure;
    tail = &structure.pNext;
}
} // namespace

void VulkanFeatureChain::Query(const VulkanAdapter &adapter)
{
    void **tail = &Features.pNext;
    Link(tail, Vulkan11);
    Link(tail, Vulkan12);
    Link(tail, Vulkan13);
    Link(tail, Vulkan14);
    const auto linkIf = [&](const char *extension, auto &structure) {
        if (adapter.HasExtension(extension))
        {
            Link(tail, structure);
        }
    };
    linkIf(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, DescriptorHeap);
    linkIf(VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME, UntypedPointers);
    linkIf(VK_EXT_MESH_SHADER_EXTENSION_NAME, MeshShader);
    linkIf(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, AccelerationStructure);
    linkIf(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, RayTracingPipeline);
    linkIf(VK_KHR_RAY_QUERY_EXTENSION_NAME, RayQuery);
    linkIf(VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME, RayTracingMaintenance1);
    linkIf(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, ShadingRate);
    linkIf(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME, ImageAtomic64);
    linkIf(VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME, InvocationReorder);
    linkIf(VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME, CooperativeVector);
    linkIf(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, CooperativeMatrix);
    linkIf(VK_KHR_PRESENT_ID_EXTENSION_NAME, PresentId);
    linkIf(VK_KHR_PRESENT_WAIT_EXTENSION_NAME, PresentWait);
    linkIf(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME, GeneratedCommands);
    Vk.vkGetPhysicalDeviceFeatures2(adapter.PhysicalDevice, &Features);
}

bool VulkanFeatureChain::CheckBaseline(StringWriter &missing) const
{
    struct Requirement
    {
        const char *Name;
        VkBool32 Value;
    };
    const VkPhysicalDeviceFeatures &core = Features.features;
    const Requirement requirements[] = {
        {"samplerAnisotropy", core.samplerAnisotropy},
        {"fillModeNonSolid", core.fillModeNonSolid},
        {"independentBlend", core.independentBlend},
        {"dualSrcBlend", core.dualSrcBlend},
        {"depthClamp", core.depthClamp},
        {"multiDrawIndirect", core.multiDrawIndirect},
        {"drawIndirectFirstInstance", core.drawIndirectFirstInstance},
        {"imageCubeArray", core.imageCubeArray},
        {"fragmentStoresAndAtomics", core.fragmentStoresAndAtomics},
        {"shaderStorageImageReadWithoutFormat", core.shaderStorageImageReadWithoutFormat},
        {"shaderStorageImageWriteWithoutFormat", core.shaderStorageImageWriteWithoutFormat},
        {"shaderInt64", core.shaderInt64},
        {"shaderInt16", core.shaderInt16},
        {"pipelineStatisticsQuery", core.pipelineStatisticsQuery},
        {"textureCompressionBC", core.textureCompressionBC},
        {"storageBuffer16BitAccess", Vulkan11.storageBuffer16BitAccess},
        {"multiview", Vulkan11.multiview},
        {"shaderFloat16", Vulkan12.shaderFloat16},
        {"shaderBufferInt64Atomics", Vulkan12.shaderBufferInt64Atomics},
        {"bufferDeviceAddress", Vulkan12.bufferDeviceAddress},
        {"timelineSemaphore", Vulkan12.timelineSemaphore},
        {"scalarBlockLayout", Vulkan12.scalarBlockLayout},
        {"vulkanMemoryModel", Vulkan12.vulkanMemoryModel},
        {"hostQueryReset", Vulkan12.hostQueryReset},
        {"drawIndirectCount", Vulkan12.drawIndirectCount},
        {"samplerFilterMinmax", Vulkan12.samplerFilterMinmax},
        {"shaderOutputLayer", Vulkan12.shaderOutputLayer},
        {"synchronization2", Vulkan13.synchronization2},
        {"dynamicRendering", Vulkan13.dynamicRendering},
        {"maintenance4", Vulkan13.maintenance4},
        {"shaderDemoteToHelperInvocation", Vulkan13.shaderDemoteToHelperInvocation},
        {"subgroupSizeControl", Vulkan13.subgroupSizeControl},
        {"maintenance5", Vulkan14.maintenance5},
        {"maintenance6", Vulkan14.maintenance6},
        {"descriptorHeap", DescriptorHeap.descriptorHeap},
        {"shaderUntypedPointers", UntypedPointers.shaderUntypedPointers},
        {"taskShader", MeshShader.taskShader},
        {"meshShader", MeshShader.meshShader},
        {"accelerationStructure", AccelerationStructure.accelerationStructure},
        {"rayTracingPipeline", RayTracingPipeline.rayTracingPipeline},
        {"rayQuery", RayQuery.rayQuery},
    };
    bool ok = true;
    for (const Requirement &requirement : requirements)
    {
        if (requirement.Value != VK_TRUE)
        {
            FormatTo(missing, "{0}{1}", ok ? "" : ", ", requirement.Name);
            ok = false;
        }
    }
    return ok;
}

void VulkanFeatureChain::DisableUnwanted()
{
    Features.features.robustBufferAccess = VK_FALSE;
    Vulkan12.bufferDeviceAddressCaptureReplay = VK_FALSE;
    Vulkan12.bufferDeviceAddressMultiDevice = VK_FALSE;
    Vulkan13.robustImageAccess = VK_FALSE;
    Vulkan14.pipelineRobustness = VK_FALSE;
    DescriptorHeap.descriptorHeapCaptureReplay = VK_FALSE;
    AccelerationStructure.accelerationStructureCaptureReplay = VK_FALSE;
    RayTracingPipeline.rayTracingPipelineShaderGroupHandleCaptureReplay = VK_FALSE;
    RayTracingPipeline.rayTracingPipelineShaderGroupHandleCaptureReplayMixed = VK_FALSE;
    CooperativeMatrix.cooperativeMatrixRobustBufferAccess = VK_FALSE;
}

void VulkanPropertyChain::Query(const VulkanAdapter &adapter)
{
    void **tail = &Properties.pNext;
    Link(tail, Vulkan11);
    Link(tail, Vulkan12);
    Link(tail, Vulkan13);
    const auto linkIf = [&](const char *extension, auto &structure) {
        if (adapter.HasExtension(extension))
        {
            Link(tail, structure);
        }
    };
    linkIf(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, DescriptorHeap);
    linkIf(VK_EXT_MESH_SHADER_EXTENSION_NAME, MeshShader);
    linkIf(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, AccelerationStructure);
    linkIf(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, RayTracingPipeline);
    linkIf(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, ShadingRate);
    linkIf(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME, GeneratedCommands);
    Vk.vkGetPhysicalDeviceProperties2(adapter.PhysicalDevice, &Properties);
}
} // namespace Marlin
