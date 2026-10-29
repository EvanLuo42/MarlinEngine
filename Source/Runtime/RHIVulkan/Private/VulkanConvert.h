#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "RHI/RhiBarriers.h"
#include "RHI/RhiCommandList.h"
#include "RHI/RhiFormat.h"
#include "RHI/RhiPipeline.h"
#include "RHI/RhiRayTracing.h"
#include "RHI/RhiResources.h"
#include "RHIVulkan/VulkanApi.h"

/// The RHI's enums in Vulkan terms.
namespace Marlin::VulkanConvert
{
VkFormat Format(RhiFormat format);
VkImageAspectFlags Aspect(RhiFormat format, RhiTextureAspect aspect = RhiTextureAspect::All);
VkImageLayout Layout(RhiLayout layout);
VkPipelineStageFlags2 Stages(RhiSync sync);
VkAccessFlags2 Access(RhiAccess access);
VkCompareOp Compare(RhiCompareOp op);
VkFilter Filter(RhiFilter filter);
VkSamplerMipmapMode MipFilter(RhiFilter filter);
VkSamplerAddressMode Address(RhiAddressMode mode);
VkBorderColor Border(RhiBorderColor color);
VkShaderStageFlagBits ShaderStage(RhiShaderStage stage);
VkPrimitiveTopology Topology(RhiPrimitiveTopology topology);
VkCullModeFlags Cull(RhiCullMode mode);
VkStencilOp Stencil(RhiStencilOp op);
VkBlendFactor Blend(RhiBlendFactor factor);
VkBlendOp BlendOperation(RhiBlendOp op);
VkSampleCountFlagBits Samples(uint32 count);
VkAttachmentLoadOp Load(RhiLoadOp op);
VkAttachmentStoreOp Store(RhiStoreOp op);
VkExtent2D ShadingRate(RhiShadingRate rate);
VkFragmentShadingRateCombinerOpKHR Combiner(RhiShadingRateCombiner combiner);
VkBuildAccelerationStructureFlagsKHR BuildFlags(RhiBuildFlags flags);
VkGeometryFlagsKHR GeometryFlags(RhiGeometryFlags flags);
VkImageViewType ViewType(RhiTextureViewDimension dimension);
VkSamplerCreateInfo Sampler(const RhiSamplerDesc &desc, VkSamplerReductionModeCreateInfo &reduction);
RhiError Error(VkResult result);

/// What a build or a size query of *inputs* passes to Vulkan; the lists keep the structures alive.
struct AccelerationStructureGeometry
{
    List<VkAccelerationStructureGeometryKHR> Geometries;
    List<VkAccelerationStructureBuildRangeInfoKHR> Ranges;
    List<uint32> PrimitiveCounts;
    VkAccelerationStructureBuildGeometryInfoKHR Info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
};
void Geometry(const RhiAccelerationStructureInputs &inputs, AccelerationStructureGeometry &geometry);
} // namespace Marlin::VulkanConvert
