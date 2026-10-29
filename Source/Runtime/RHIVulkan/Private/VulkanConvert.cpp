#include "VulkanConvert.h"

#include "Diagnostics/Assert.h"

namespace Marlin::VulkanConvert
{
VkFormat Format(RhiFormat format)
{
    switch (format)
    {
    case RhiFormat::Unknown:
        return VK_FORMAT_UNDEFINED;
    case RhiFormat::R8Unorm:
        return VK_FORMAT_R8_UNORM;
    case RhiFormat::R8Snorm:
        return VK_FORMAT_R8_SNORM;
    case RhiFormat::R8Uint:
        return VK_FORMAT_R8_UINT;
    case RhiFormat::R8Sint:
        return VK_FORMAT_R8_SINT;
    case RhiFormat::RG8Unorm:
        return VK_FORMAT_R8G8_UNORM;
    case RhiFormat::RG8Snorm:
        return VK_FORMAT_R8G8_SNORM;
    case RhiFormat::RG8Uint:
        return VK_FORMAT_R8G8_UINT;
    case RhiFormat::RG8Sint:
        return VK_FORMAT_R8G8_SINT;
    case RhiFormat::RGBA8Unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case RhiFormat::RGBA8UnormSrgb:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case RhiFormat::RGBA8Snorm:
        return VK_FORMAT_R8G8B8A8_SNORM;
    case RhiFormat::RGBA8Uint:
        return VK_FORMAT_R8G8B8A8_UINT;
    case RhiFormat::RGBA8Sint:
        return VK_FORMAT_R8G8B8A8_SINT;
    case RhiFormat::BGRA8Unorm:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case RhiFormat::BGRA8UnormSrgb:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case RhiFormat::R16Unorm:
        return VK_FORMAT_R16_UNORM;
    case RhiFormat::R16Snorm:
        return VK_FORMAT_R16_SNORM;
    case RhiFormat::R16Uint:
        return VK_FORMAT_R16_UINT;
    case RhiFormat::R16Sint:
        return VK_FORMAT_R16_SINT;
    case RhiFormat::R16Float:
        return VK_FORMAT_R16_SFLOAT;
    case RhiFormat::RG16Unorm:
        return VK_FORMAT_R16G16_UNORM;
    case RhiFormat::RG16Snorm:
        return VK_FORMAT_R16G16_SNORM;
    case RhiFormat::RG16Uint:
        return VK_FORMAT_R16G16_UINT;
    case RhiFormat::RG16Sint:
        return VK_FORMAT_R16G16_SINT;
    case RhiFormat::RG16Float:
        return VK_FORMAT_R16G16_SFLOAT;
    case RhiFormat::RGBA16Unorm:
        return VK_FORMAT_R16G16B16A16_UNORM;
    case RhiFormat::RGBA16Snorm:
        return VK_FORMAT_R16G16B16A16_SNORM;
    case RhiFormat::RGBA16Uint:
        return VK_FORMAT_R16G16B16A16_UINT;
    case RhiFormat::RGBA16Sint:
        return VK_FORMAT_R16G16B16A16_SINT;
    case RhiFormat::RGBA16Float:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RhiFormat::R32Uint:
        return VK_FORMAT_R32_UINT;
    case RhiFormat::R32Sint:
        return VK_FORMAT_R32_SINT;
    case RhiFormat::R32Float:
        return VK_FORMAT_R32_SFLOAT;
    case RhiFormat::RG32Uint:
        return VK_FORMAT_R32G32_UINT;
    case RhiFormat::RG32Sint:
        return VK_FORMAT_R32G32_SINT;
    case RhiFormat::RG32Float:
        return VK_FORMAT_R32G32_SFLOAT;
    case RhiFormat::RGB32Uint:
        return VK_FORMAT_R32G32B32_UINT;
    case RhiFormat::RGB32Sint:
        return VK_FORMAT_R32G32B32_SINT;
    case RhiFormat::RGB32Float:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case RhiFormat::RGBA32Uint:
        return VK_FORMAT_R32G32B32A32_UINT;
    case RhiFormat::RGBA32Sint:
        return VK_FORMAT_R32G32B32A32_SINT;
    case RhiFormat::RGBA32Float:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RhiFormat::R64Uint:
        return VK_FORMAT_R64_UINT;
    case RhiFormat::RGB10A2Unorm:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case RhiFormat::RGB10A2Uint:
        return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case RhiFormat::RG11B10Float:
        return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case RhiFormat::RGB9E5Float:
        return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case RhiFormat::D16Unorm:
        return VK_FORMAT_D16_UNORM;
    case RhiFormat::D32Float:
        return VK_FORMAT_D32_SFLOAT;
    case RhiFormat::D32FloatS8Uint:
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case RhiFormat::BC1Unorm:
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case RhiFormat::BC1UnormSrgb:
        return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case RhiFormat::BC2Unorm:
        return VK_FORMAT_BC2_UNORM_BLOCK;
    case RhiFormat::BC2UnormSrgb:
        return VK_FORMAT_BC2_SRGB_BLOCK;
    case RhiFormat::BC3Unorm:
        return VK_FORMAT_BC3_UNORM_BLOCK;
    case RhiFormat::BC3UnormSrgb:
        return VK_FORMAT_BC3_SRGB_BLOCK;
    case RhiFormat::BC4Unorm:
        return VK_FORMAT_BC4_UNORM_BLOCK;
    case RhiFormat::BC4Snorm:
        return VK_FORMAT_BC4_SNORM_BLOCK;
    case RhiFormat::BC5Unorm:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case RhiFormat::BC5Snorm:
        return VK_FORMAT_BC5_SNORM_BLOCK;
    case RhiFormat::BC6HUfloat:
        return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case RhiFormat::BC6HSfloat:
        return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case RhiFormat::BC7Unorm:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case RhiFormat::BC7UnormSrgb:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    case RhiFormat::Astc4x4Unorm:
        return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case RhiFormat::Astc4x4UnormSrgb:
        return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case RhiFormat::Astc5x5Unorm:
        return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
    case RhiFormat::Astc5x5UnormSrgb:
        return VK_FORMAT_ASTC_5x5_SRGB_BLOCK;
    case RhiFormat::Astc6x6Unorm:
        return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    case RhiFormat::Astc6x6UnormSrgb:
        return VK_FORMAT_ASTC_6x6_SRGB_BLOCK;
    case RhiFormat::Astc8x8Unorm:
        return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
    case RhiFormat::Astc8x8UnormSrgb:
        return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
    case RhiFormat::Count:
        break;
    }
    return VK_FORMAT_UNDEFINED;
}

VkImageAspectFlags Aspect(RhiFormat format, RhiTextureAspect aspect)
{
    const RhiFormatInfo &info = GetFormatInfo(format);
    if (!info.HasDepth)
    {
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
    switch (aspect)
    {
    case RhiTextureAspect::Depth:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case RhiTextureAspect::Stencil:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case RhiTextureAspect::All:
        break;
    }
    return info.HasStencil ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
}

VkImageLayout Layout(RhiLayout layout)
{
    switch (layout)
    {
    case RhiLayout::Undefined:
        return VK_IMAGE_LAYOUT_UNDEFINED;
    case RhiLayout::General:
    case RhiLayout::UnorderedAccess:
        return VK_IMAGE_LAYOUT_GENERAL;
    case RhiLayout::Present:
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case RhiLayout::RenderTarget:
    case RhiLayout::DepthStencilWrite:
        return VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    case RhiLayout::DepthStencilRead:
    case RhiLayout::ShaderResource:
        return VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    case RhiLayout::CopySource:
    case RhiLayout::ResolveSource:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case RhiLayout::CopyDest:
    case RhiLayout::ResolveDest:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case RhiLayout::ShadingRateSource:
        return VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    }
    return VK_IMAGE_LAYOUT_GENERAL;
}

VkPipelineStageFlags2 Stages(RhiSync sync)
{
    if (sync == RhiSync::All)
    {
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    struct Entry
    {
        RhiSync Sync;
        VkPipelineStageFlags2 Stages;
    };
    static constexpr Entry Table[] = {
        {RhiSync::IndexInput, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT},
        {RhiSync::VertexShading, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT},
        {RhiSync::TaskShading, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT},
        {RhiSync::MeshShading, VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT},
        {RhiSync::PixelShading, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT},
        {RhiSync::DepthStencil,
         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT},
        {RhiSync::RenderTarget, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
        {RhiSync::ComputeShading, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT},
        {RhiSync::RayTracing, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR},
        {RhiSync::Copy, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT},
        {RhiSync::Resolve, VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
        {RhiSync::IndirectArguments, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT},
        {RhiSync::AccelerationStructureBuild, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR},
        {RhiSync::AccelerationStructureCopy, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR},
        {RhiSync::ShadingRate, VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR},
        {RhiSync::IndirectPreprocess, VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_EXT},
        {RhiSync::Host, VK_PIPELINE_STAGE_2_HOST_BIT},
    };
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    for (const Entry &entry : Table)
    {
        if (HasAnyFlags(sync, entry.Sync))
        {
            stages |= entry.Stages;
        }
    }
    return stages;
}

VkAccessFlags2 Access(RhiAccess access)
{
    struct Entry
    {
        RhiAccess Access;
        VkAccessFlags2 Flags;
    };
    static constexpr Entry Table[] = {
        {RhiAccess::IndexBuffer, VK_ACCESS_2_INDEX_READ_BIT},
        {RhiAccess::ShaderResource, VK_ACCESS_2_SHADER_READ_BIT},
        {RhiAccess::UnorderedAccess, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT},
        {RhiAccess::RenderTarget, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
        {RhiAccess::DepthStencilRead, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT},
        {RhiAccess::DepthStencilWrite,
         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
        {RhiAccess::CopySource, VK_ACCESS_2_TRANSFER_READ_BIT},
        {RhiAccess::CopyDest, VK_ACCESS_2_TRANSFER_WRITE_BIT},
        {RhiAccess::ResolveSource, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT},
        {RhiAccess::ResolveDest, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
        {RhiAccess::IndirectArguments, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT},
        {RhiAccess::AccelerationStructureRead, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR},
        {RhiAccess::AccelerationStructureWrite, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR},
        {RhiAccess::ShadingRateSource, VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR},
        {RhiAccess::IndirectPreprocessRead, VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_EXT},
        {RhiAccess::IndirectPreprocessWrite, VK_ACCESS_2_COMMAND_PREPROCESS_WRITE_BIT_EXT},
        {RhiAccess::HostRead, VK_ACCESS_2_HOST_READ_BIT},
        {RhiAccess::HostWrite, VK_ACCESS_2_HOST_WRITE_BIT},
        {RhiAccess::MarkerWrite, VK_ACCESS_2_TRANSFER_WRITE_BIT},
    };
    VkAccessFlags2 flags = VK_ACCESS_2_NONE;
    for (const Entry &entry : Table)
    {
        if (HasAnyFlags(access, entry.Access))
        {
            flags |= entry.Flags;
        }
    }
    return flags;
}

VkCompareOp Compare(RhiCompareOp op)
{
    return static_cast<VkCompareOp>(op);
}

VkFilter Filter(RhiFilter filter)
{
    return filter == RhiFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

VkSamplerMipmapMode MipFilter(RhiFilter filter)
{
    return filter == RhiFilter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

VkSamplerAddressMode Address(RhiAddressMode mode)
{
    switch (mode)
    {
    case RhiAddressMode::Wrap:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case RhiAddressMode::Mirror:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case RhiAddressMode::Clamp:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case RhiAddressMode::Border:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case RhiAddressMode::MirrorOnce:
        return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkBorderColor Border(RhiBorderColor color)
{
    switch (color)
    {
    case RhiBorderColor::TransparentBlack:
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    case RhiBorderColor::OpaqueBlack:
        return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    case RhiBorderColor::OpaqueWhite:
        return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}

VkShaderStageFlagBits ShaderStage(RhiShaderStage stage)
{
    switch (stage)
    {
    case RhiShaderStage::Vertex:
        return VK_SHADER_STAGE_VERTEX_BIT;
    case RhiShaderStage::Pixel:
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    case RhiShaderStage::Task:
        return VK_SHADER_STAGE_TASK_BIT_EXT;
    case RhiShaderStage::Mesh:
        return VK_SHADER_STAGE_MESH_BIT_EXT;
    case RhiShaderStage::Compute:
        return VK_SHADER_STAGE_COMPUTE_BIT;
    case RhiShaderStage::RayGeneration:
        return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    case RhiShaderStage::Miss:
        return VK_SHADER_STAGE_MISS_BIT_KHR;
    case RhiShaderStage::ClosestHit:
        return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    case RhiShaderStage::AnyHit:
        return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    case RhiShaderStage::Intersection:
        return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    case RhiShaderStage::Callable:
        return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    default:
        break;
    }
    MARLIN_ASSERT(false);
    return VK_SHADER_STAGE_ALL;
}

VkPrimitiveTopology Topology(RhiPrimitiveTopology topology)
{
    switch (topology)
    {
    case RhiPrimitiveTopology::TriangleList:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case RhiPrimitiveTopology::TriangleStrip:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case RhiPrimitiveTopology::LineList:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case RhiPrimitiveTopology::LineStrip:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case RhiPrimitiveTopology::PointList:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCullModeFlags Cull(RhiCullMode mode)
{
    switch (mode)
    {
    case RhiCullMode::None:
        return VK_CULL_MODE_NONE;
    case RhiCullMode::Front:
        return VK_CULL_MODE_FRONT_BIT;
    case RhiCullMode::Back:
        return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}

VkStencilOp Stencil(RhiStencilOp op)
{
    switch (op)
    {
    case RhiStencilOp::Keep:
        return VK_STENCIL_OP_KEEP;
    case RhiStencilOp::Zero:
        return VK_STENCIL_OP_ZERO;
    case RhiStencilOp::Replace:
        return VK_STENCIL_OP_REPLACE;
    case RhiStencilOp::IncrementClamp:
        return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case RhiStencilOp::DecrementClamp:
        return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case RhiStencilOp::Invert:
        return VK_STENCIL_OP_INVERT;
    case RhiStencilOp::IncrementWrap:
        return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case RhiStencilOp::DecrementWrap:
        return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

VkBlendFactor Blend(RhiBlendFactor factor)
{
    switch (factor)
    {
    case RhiBlendFactor::Zero:
        return VK_BLEND_FACTOR_ZERO;
    case RhiBlendFactor::One:
        return VK_BLEND_FACTOR_ONE;
    case RhiBlendFactor::SourceColor:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case RhiBlendFactor::InverseSourceColor:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case RhiBlendFactor::SourceAlpha:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case RhiBlendFactor::InverseSourceAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case RhiBlendFactor::DestColor:
        return VK_BLEND_FACTOR_DST_COLOR;
    case RhiBlendFactor::InverseDestColor:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case RhiBlendFactor::DestAlpha:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case RhiBlendFactor::InverseDestAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case RhiBlendFactor::SourceAlphaSaturate:
        return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case RhiBlendFactor::Constant:
        return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case RhiBlendFactor::InverseConstant:
        return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case RhiBlendFactor::Source1Color:
        return VK_BLEND_FACTOR_SRC1_COLOR;
    case RhiBlendFactor::InverseSource1Color:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case RhiBlendFactor::Source1Alpha:
        return VK_BLEND_FACTOR_SRC1_ALPHA;
    case RhiBlendFactor::InverseSource1Alpha:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp BlendOperation(RhiBlendOp op)
{
    switch (op)
    {
    case RhiBlendOp::Add:
        return VK_BLEND_OP_ADD;
    case RhiBlendOp::Subtract:
        return VK_BLEND_OP_SUBTRACT;
    case RhiBlendOp::ReverseSubtract:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case RhiBlendOp::Minimum:
        return VK_BLEND_OP_MIN;
    case RhiBlendOp::Maximum:
        return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkSampleCountFlagBits Samples(uint32 count)
{
    switch (count)
    {
    case 2:
        return VK_SAMPLE_COUNT_2_BIT;
    case 4:
        return VK_SAMPLE_COUNT_4_BIT;
    case 8:
        return VK_SAMPLE_COUNT_8_BIT;
    case 16:
        return VK_SAMPLE_COUNT_16_BIT;
    default:
        return VK_SAMPLE_COUNT_1_BIT;
    }
}

VkAttachmentLoadOp Load(RhiLoadOp op)
{
    switch (op)
    {
    case RhiLoadOp::Load:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case RhiLoadOp::Clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case RhiLoadOp::Discard:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}

VkAttachmentStoreOp Store(RhiStoreOp op)
{
    return op == RhiStoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

VkExtent2D ShadingRate(RhiShadingRate rate)
{
    switch (rate)
    {
    case RhiShadingRate::Rate1x1:
        return {1, 1};
    case RhiShadingRate::Rate1x2:
        return {1, 2};
    case RhiShadingRate::Rate2x1:
        return {2, 1};
    case RhiShadingRate::Rate2x2:
        return {2, 2};
    case RhiShadingRate::Rate2x4:
        return {2, 4};
    case RhiShadingRate::Rate4x2:
        return {4, 2};
    case RhiShadingRate::Rate4x4:
        return {4, 4};
    }
    return {1, 1};
}

VkFragmentShadingRateCombinerOpKHR Combiner(RhiShadingRateCombiner combiner)
{
    switch (combiner)
    {
    case RhiShadingRateCombiner::Passthrough:
        return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    case RhiShadingRateCombiner::Override:
        return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
    case RhiShadingRateCombiner::Minimum:
        return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
    case RhiShadingRateCombiner::Maximum:
        return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR;
    case RhiShadingRateCombiner::Sum:
        return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR;
    }
    return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
}

VkBuildAccelerationStructureFlagsKHR BuildFlags(RhiBuildFlags flags)
{
    VkBuildAccelerationStructureFlagsKHR result = 0;
    if (HasAnyFlags(flags, RhiBuildFlags::AllowUpdate))
    {
        result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    if (HasAnyFlags(flags, RhiBuildFlags::AllowCompaction))
    {
        result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    }
    if (HasAnyFlags(flags, RhiBuildFlags::PreferFastTrace))
    {
        result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    }
    if (HasAnyFlags(flags, RhiBuildFlags::PreferFastBuild))
    {
        result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    }
    if (HasAnyFlags(flags, RhiBuildFlags::MinimizeMemory))
    {
        result |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
    }
    return result;
}

VkGeometryFlagsKHR GeometryFlags(RhiGeometryFlags flags)
{
    VkGeometryFlagsKHR result = 0;
    if (HasAnyFlags(flags, RhiGeometryFlags::Opaque))
    {
        result |= VK_GEOMETRY_OPAQUE_BIT_KHR;
    }
    if (HasAnyFlags(flags, RhiGeometryFlags::NoDuplicateAnyHit))
    {
        result |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
    }
    return result;
}

VkImageViewType ViewType(RhiTextureViewDimension dimension)
{
    switch (dimension)
    {
    case RhiTextureViewDimension::Texture1D:
        return VK_IMAGE_VIEW_TYPE_1D;
    case RhiTextureViewDimension::Texture1DArray:
        return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case RhiTextureViewDimension::Texture2D:
    case RhiTextureViewDimension::Texture2DMultisample:
        return VK_IMAGE_VIEW_TYPE_2D;
    case RhiTextureViewDimension::Texture2DArray:
    case RhiTextureViewDimension::Texture2DMultisampleArray:
        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case RhiTextureViewDimension::Texture3D:
        return VK_IMAGE_VIEW_TYPE_3D;
    case RhiTextureViewDimension::TextureCube:
        return VK_IMAGE_VIEW_TYPE_CUBE;
    case RhiTextureViewDimension::TextureCubeArray:
        return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

VkSamplerCreateInfo Sampler(const RhiSamplerDesc &desc, VkSamplerReductionModeCreateInfo &reduction)
{
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = Filter(desc.MagFilter);
    info.minFilter = Filter(desc.MinFilter);
    info.mipmapMode = MipFilter(desc.MipFilter);
    info.addressModeU = Address(desc.AddressU);
    info.addressModeV = Address(desc.AddressV);
    info.addressModeW = Address(desc.AddressW);
    info.mipLodBias = desc.MipLodBias;
    info.anisotropyEnable = desc.MaxAnisotropy > 1 ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = static_cast<float32>(desc.MaxAnisotropy);
    info.compareEnable = desc.Reduction == RhiSamplerReduction::Comparison ? VK_TRUE : VK_FALSE;
    info.compareOp = Compare(desc.Compare);
    info.minLod = desc.MinLod;
    info.maxLod = desc.MaxLod;
    info.borderColor = Border(desc.Border);
    reduction = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    if (desc.Reduction == RhiSamplerReduction::Minimum || desc.Reduction == RhiSamplerReduction::Maximum)
    {
        reduction.reductionMode = desc.Reduction == RhiSamplerReduction::Minimum ? VK_SAMPLER_REDUCTION_MODE_MIN
                                                                                  : VK_SAMPLER_REDUCTION_MODE_MAX;
        info.pNext = &reduction;
    }
    return info;
}

void Geometry(const RhiAccelerationStructureInputs &inputs, AccelerationStructureGeometry &geometry)
{
    VkAccelerationStructureBuildGeometryInfoKHR &info = geometry.Info;
    info.flags = BuildFlags(inputs.Flags);
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    if (inputs.Kind == RhiAccelerationStructureKind::TopLevel)
    {
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VkAccelerationStructureGeometryKHR &instances = geometry.Geometries.AddDefaulted();
        instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        instances.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        instances.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        instances.geometry.instances.data.deviceAddress = inputs.InstanceAddress;
        geometry.Ranges.Add({inputs.InstanceCount, 0, 0, 0});
        geometry.PrimitiveCounts.Add(inputs.InstanceCount);
    }
    else
    {
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        for (const RhiGeometry &source : inputs.Geometries)
        {
            VkAccelerationStructureGeometryKHR &entry = geometry.Geometries.AddDefaulted();
            entry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            entry.flags = GeometryFlags(source.Flags);
            uint32 primitives = 0;
            if (source.Kind == RhiGeometryKind::Triangles)
            {
                entry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                VkAccelerationStructureGeometryTrianglesDataKHR &triangles = entry.geometry.triangles;
                triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
                triangles.vertexFormat = Format(source.VertexFormat);
                triangles.vertexData.deviceAddress = source.VertexAddress;
                triangles.vertexStride = source.VertexStride;
                triangles.maxVertex = source.VertexCount > 0 ? source.VertexCount - 1 : 0;
                triangles.indexType = source.IndexAddress == 0 ? VK_INDEX_TYPE_NONE_KHR
                                      : source.IndexBits == 16 ? VK_INDEX_TYPE_UINT16
                                                               : VK_INDEX_TYPE_UINT32;
                triangles.indexData.deviceAddress = source.IndexAddress;
                triangles.transformData.deviceAddress = source.TransformAddress;
                primitives = (source.IndexAddress != 0 ? source.IndexCount : source.VertexCount) / 3;
            }
            else
            {
                entry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
                entry.geometry.aabbs = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
                entry.geometry.aabbs.data.deviceAddress = source.BoxAddress;
                entry.geometry.aabbs.stride = source.BoxStride;
                primitives = source.BoxCount;
            }
            geometry.Ranges.Add({primitives, 0, 0, 0});
            geometry.PrimitiveCounts.Add(primitives);
        }
    }
    info.geometryCount = static_cast<uint32>(geometry.Geometries.Count());
    info.pGeometries = geometry.Geometries.Data();
}

RhiError Error(VkResult result)
{
    switch (result)
    {
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return RhiError::OutOfHostMemory;
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_TOO_MANY_OBJECTS:
    case VK_ERROR_FRAGMENTATION:
        return RhiError::OutOfDeviceMemory;
    case VK_ERROR_DEVICE_LOST:
        return RhiError::DeviceLost;
    case VK_ERROR_SURFACE_LOST_KHR:
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return RhiError::SurfaceLost;
    case VK_ERROR_INVALID_SHADER_NV:
        return RhiError::PipelineCompilationFailed;
    default:
        return RhiError::Unsupported;
    }
}
} // namespace Marlin::VulkanConvert
