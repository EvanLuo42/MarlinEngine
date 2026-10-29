#include "Diagnostics/Log.h"
#include "RHI/RhiLog.h"
#include "VulkanConvert.h"
#include "VulkanDevice.h"

namespace Marlin
{
namespace
{
/// The stage create infos of a pipeline and what they point to.
struct StageInfos
{
    List<VkPipelineShaderStageCreateInfo> Stages;
    List<VkShaderModuleCreateInfo> Modules;
    List<VkSpecializationInfo> Specializations;
    List<VkSpecializationMapEntry> Entries;

    List<uint32> Data;

    /// *resourceStride* and *samplerStride* go to every stage as the heap stride specialization constants.
    void Build(Span<const VulkanShaderSource> shaders, uint32 resourceStride, uint32 samplerStride)
    {
        Stages.Reserve(shaders.Count());
        Modules.Reserve(shaders.Count());
        Specializations.Reserve(shaders.Count());
        usize entryCount = 0;
        for (const VulkanShaderSource &shader : shaders)
        {
            entryCount += shader.SpecializationConstants.Count() + 2;
        }
        Entries.Reserve(entryCount);
        Data.Reserve(entryCount);
        for (const VulkanShaderSource &shader : shaders)
        {
            VkShaderModuleCreateInfo &module = Modules.AddDefaulted();
            module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module.codeSize = shader.Code.Count();
            module.pCode = reinterpret_cast<const uint32 *>(shader.Code.Data());
            VkPipelineShaderStageCreateInfo &stage = Stages.AddDefaulted();
            stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.pNext = &module;
            stage.stage = VulkanConvert::ShaderStage(shader.Stage);
            stage.pName = shader.EntryPoint.CStr();
            const usize first = Entries.Count();
            const usize firstData = Data.Count();
            const auto count = static_cast<uint32>(shader.SpecializationConstants.Count());
            for (uint32 i = 0; i < count; ++i)
            {
                Entries.Add({i, i * 4, 4});
                Data.Add(shader.SpecializationConstants[i]);
            }
            Entries.Add({RhiResourceStrideSpecializationId, count * 4, 4});
            Data.Add(resourceStride);
            Entries.Add({RhiSamplerStrideSpecializationId, (count + 1) * 4, 4});
            Data.Add(samplerStride);
            VkSpecializationInfo &specialization = Specializations.AddDefaulted();
            specialization.mapEntryCount = count + 2;
            specialization.pMapEntries = Entries.Data() + first;
            specialization.dataSize = (count + 2) * sizeof(uint32);
            specialization.pData = Data.Data() + firstData;
            stage.pSpecializationInfo = &specialization;
        }
    }
};

void CopyShaders(Span<const RhiShader> shaders, List<VulkanShaderSource> &sources)
{
    for (const RhiShader &shader : shaders)
    {
        VulkanShaderSource &source = sources.AddDefaulted();
        source.Code.AddRange(shader.Code);
        source.EntryPoint = String(shader.EntryPoint);
        source.Stage = shader.Stage;
        source.SpecializationConstants.AddRange(shader.SpecializationConstants);
    }
}

VkPipelineCreateFlags2CreateInfo DescriptorHeapFlags()
{
    VkPipelineCreateFlags2CreateInfo flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};
    flags.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
    return flags;
}
} // namespace

Result<RhiPipeline, RhiError> VulkanDevice::AddPipeline(VulkanPipeline *&pipeline)
{
    const RhiPipeline handle = _pipelines.Allocate();
    if (!handle.IsValid())
    {
        return Failure(RhiError::OutOfHostMemory);
    }
    pipeline = &_pipelines.Get(handle);
    pipeline->Device = this;
    return handle;
}

void VulkanDevice::NotifyPipelineCreated(Span<const RhiShader> shaders, StringView debugName) const
{
    if (_desc.OnPipelineCreated != nullptr)
    {
        _desc.OnPipelineCreated(_desc.PipelineCreatedContext, shaders, debugName);
    }
}

RhiPipeline VulkanDevice::CreatePipeline(const RhiGraphicsPipelineDesc &desc)
{
    VulkanPipeline *pipeline = nullptr;
    Result<RhiPipeline, RhiError> handle = AddPipeline(pipeline);
    if (handle.IsError())
    {
        return {};
    }
    pipeline->Kind = RhiPipelineKind::Graphics;
    pipeline->BindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    CopyShaders(desc.Shaders, pipeline->Shaders);
    pipeline->Graphics = desc;
    pipeline->RenderTargetFormats.AddRange(desc.RenderTargetFormats);
    pipeline->Graphics.Shaders = {};
    pipeline->Graphics.RenderTargetFormats = pipeline->RenderTargetFormats;
    pipeline->DebugName = String(desc.DebugName);
    pipeline->Graphics.DebugName = {};
    pipeline->IndirectBindable = desc.IndirectBindable;
    NotifyPipelineCreated(desc.Shaders, desc.DebugName);
    Jobs::Run(JobDecl{&VulkanDevice::CompilePipeline, pipeline, JobPriority::Normal, true}, &pipeline->Compile);
    return handle.Value();
}

RhiPipeline VulkanDevice::CreatePipeline(const RhiComputePipelineDesc &desc)
{
    VulkanPipeline *pipeline = nullptr;
    Result<RhiPipeline, RhiError> handle = AddPipeline(pipeline);
    if (handle.IsError())
    {
        return {};
    }
    pipeline->Kind = RhiPipelineKind::Compute;
    pipeline->BindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    CopyShaders(Span<const RhiShader>(&desc.Shader, 1), pipeline->Shaders);
    pipeline->DebugName = String(desc.DebugName);
    pipeline->IndirectBindable = desc.IndirectBindable;
    NotifyPipelineCreated(Span<const RhiShader>(&desc.Shader, 1), desc.DebugName);
    Jobs::Run(JobDecl{&VulkanDevice::CompilePipeline, pipeline, JobPriority::Normal, true}, &pipeline->Compile);
    return handle.Value();
}

RhiPipeline VulkanDevice::CreatePipeline(const RhiRayTracingPipelineDesc &desc)
{
    VulkanPipeline *pipeline = nullptr;
    Result<RhiPipeline, RhiError> handle = AddPipeline(pipeline);
    if (handle.IsError())
    {
        return {};
    }
    pipeline->Kind = RhiPipelineKind::RayTracing;
    pipeline->BindPoint = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
    CopyShaders(desc.Shaders, pipeline->Shaders);
    pipeline->HitGroups.AddRange(desc.HitGroups);
    pipeline->MaxRecursionDepth = desc.MaxRecursionDepth;
    pipeline->DebugName = String(desc.DebugName);
    NotifyPipelineCreated(desc.Shaders, desc.DebugName);
    Jobs::Run(JobDecl{&VulkanDevice::CompilePipeline, pipeline, JobPriority::Normal, true}, &pipeline->Compile);
    return handle.Value();
}

void VulkanDevice::CompilePipeline(void *pipeline)
{
    auto *entry = static_cast<VulkanPipeline *>(pipeline);
    entry->Device->Compile(*entry);
}

void VulkanDevice::Compile(VulkanPipeline &pipeline)
{
    VkResult result = VK_SUCCESS;
    switch (pipeline.Kind)
    {
    case RhiPipelineKind::Graphics:
        result = CompileGraphics(pipeline);
        break;
    case RhiPipelineKind::Compute:
        result = CompileCompute(pipeline);
        break;
    case RhiPipelineKind::RayTracing:
        result = CompileRayTracing(pipeline);
        break;
    }
    if (result != VK_SUCCESS)
    {
        MARLIN_LOG(LogRhi, Error, "Pipeline {0} failed to compile ({1})", pipeline.DebugName,
                   static_cast<int32>(result));
        pipeline.State.Store(static_cast<uint8>(RhiPipelineState::Failed), MemoryOrder::Release);
        return;
    }
    SetObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64>(pipeline.Pipeline), pipeline.DebugName);
    pipeline.Shaders.Clear();
    pipeline.State.Store(static_cast<uint8>(RhiPipelineState::Ready), MemoryOrder::Release);
}

VkResult VulkanDevice::CompileGraphics(VulkanPipeline &pipeline)
{
    const RhiGraphicsPipelineDesc &desc = pipeline.Graphics;
    StageInfos stages;
    stages.Build(pipeline.Shaders, _resourceStride, _samplerStride);
    bool mesh = false;
    for (const VulkanShaderSource &shader : pipeline.Shaders)
    {
        mesh = mesh || shader.Stage == RhiShaderStage::Mesh;
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VulkanConvert::Topology(desc.Topology);
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};

    VkPipelineRasterizationConservativeStateCreateInfoEXT conservative{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT};
    conservative.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.depthClampEnable = desc.Raster.DepthClip ? VK_FALSE : VK_TRUE;
    raster.polygonMode = desc.Raster.Fill == RhiFillMode::Wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    raster.cullMode = VulkanConvert::Cull(desc.Raster.Cull);
    raster.frontFace = desc.Raster.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    raster.depthBiasEnable = desc.Raster.DepthBias != 0 || desc.Raster.DepthBiasSlope != 0 ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = desc.Raster.DepthBias;
    raster.depthBiasClamp = desc.Raster.DepthBiasClamp;
    raster.depthBiasSlopeFactor = desc.Raster.DepthBiasSlope;
    raster.lineWidth = 1;
    if (desc.Raster.Conservative && _conservativeRasterization)
    {
        raster.pNext = &conservative;
    }

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VulkanConvert::Samples(desc.SampleCount);
    multisample.alphaToCoverageEnable = desc.AlphaToCoverage ? VK_TRUE : VK_FALSE;

    const auto stencilFace = [&](const RhiStencilFace &face) {
        VkStencilOpState state{};
        state.failOp = VulkanConvert::Stencil(face.Fail);
        state.passOp = VulkanConvert::Stencil(face.Pass);
        state.depthFailOp = VulkanConvert::Stencil(face.DepthFail);
        state.compareOp = VulkanConvert::Compare(face.Compare);
        state.compareMask = desc.DepthStencil.StencilReadMask;
        state.writeMask = desc.DepthStencil.StencilWriteMask;
        return state;
    };
    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = desc.DepthStencil.DepthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.DepthStencil.DepthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VulkanConvert::Compare(desc.DepthStencil.DepthCompare);
    depthStencil.depthBoundsTestEnable = desc.DepthStencil.DepthBoundsTest ? VK_TRUE : VK_FALSE;
    depthStencil.stencilTestEnable = desc.DepthStencil.StencilTest ? VK_TRUE : VK_FALSE;
    depthStencil.front = stencilFace(desc.DepthStencil.Front);
    depthStencil.back = stencilFace(desc.DepthStencil.Back);
    depthStencil.maxDepthBounds = 1;

    VkPipelineColorBlendAttachmentState blends[RhiMaxRenderTargets] = {};
    List<VkFormat> colorFormats;
    for (usize i = 0; i < desc.RenderTargetFormats.Count() && i < RhiMaxRenderTargets; ++i)
    {
        const RhiBlendTarget &target = desc.Blend[i];
        VkPipelineColorBlendAttachmentState &blend = blends[i];
        blend.blendEnable = target.Enabled ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = VulkanConvert::Blend(target.SourceColor);
        blend.dstColorBlendFactor = VulkanConvert::Blend(target.DestColor);
        blend.colorBlendOp = VulkanConvert::BlendOperation(target.ColorOp);
        blend.srcAlphaBlendFactor = VulkanConvert::Blend(target.SourceAlpha);
        blend.dstAlphaBlendFactor = VulkanConvert::Blend(target.DestAlpha);
        blend.alphaBlendOp = VulkanConvert::BlendOperation(target.AlphaOp);
        blend.colorWriteMask = static_cast<VkColorComponentFlags>(target.WriteMask);
        colorFormats.Add(VulkanConvert::Format(desc.RenderTargetFormats[i]));
    }
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = static_cast<uint32>(colorFormats.Count());
    colorBlend.pAttachments = blends;

    List<VkDynamicState> dynamicStates;
    dynamicStates.AddRange({VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_DEPTH_BOUNDS});
    if (_variableRateShading)
    {
        dynamicStates.Add(VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR);
    }
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32>(dynamicStates.Count());
    dynamic.pDynamicStates = dynamicStates.Data();

    const VkFormat depthFormat = VulkanConvert::Format(desc.DepthStencilFormat);
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.viewMask = desc.ViewMask;
    rendering.colorAttachmentCount = static_cast<uint32>(colorFormats.Count());
    rendering.pColorAttachmentFormats = colorFormats.Data();
    rendering.depthAttachmentFormat = depthFormat;
    rendering.stencilAttachmentFormat = IsStencilFormat(desc.DepthStencilFormat) ? depthFormat : VK_FORMAT_UNDEFINED;

    VkPipelineCreateFlags2CreateInfo flags = DescriptorHeapFlags();
    if (_generatedCommands && pipeline.IndirectBindable)
    {
        flags.flags |= VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT;
    }
    if (_variableRateShading)
    {
        flags.flags |= VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
    }
    flags.pNext = &rendering;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &flags;
    info.stageCount = static_cast<uint32>(stages.Stages.Count());
    info.pStages = stages.Stages.Data();
    info.pVertexInputState = mesh ? nullptr : &vertexInput;
    info.pInputAssemblyState = mesh ? nullptr : &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    return Vk.vkCreateGraphicsPipelines(_device, _pipelineCache, 1, &info, nullptr, &pipeline.Pipeline);
}

VkResult VulkanDevice::CompileCompute(VulkanPipeline &pipeline)
{
    StageInfos stages;
    stages.Build(pipeline.Shaders, _resourceStride, _samplerStride);
    VkPipelineCreateFlags2CreateInfo flags = DescriptorHeapFlags();
    if (_generatedCommands && pipeline.IndirectBindable)
    {
        flags.flags |= VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT;
    }
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.pNext = &flags;
    info.stage = stages.Stages[0];
    return Vk.vkCreateComputePipelines(_device, _pipelineCache, 1, &info, nullptr, &pipeline.Pipeline);
}

VkResult VulkanDevice::CompileRayTracing(VulkanPipeline &pipeline)
{
    StageInfos stages;
    stages.Build(pipeline.Shaders, _resourceStride, _samplerStride);
    List<VkRayTracingShaderGroupCreateInfoKHR> groups;
    for (uint32 i = 0; i < pipeline.Shaders.Count(); ++i)
    {
        const RhiShaderStage stage = pipeline.Shaders[i].Stage;
        if (stage != RhiShaderStage::RayGeneration && stage != RhiShaderStage::Miss &&
            stage != RhiShaderStage::Callable)
        {
            continue;
        }
        VkRayTracingShaderGroupCreateInfoKHR &group = groups.AddDefaulted();
        group = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        group.generalShader = i;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    for (const RhiHitGroup &hit : pipeline.HitGroups)
    {
        VkRayTracingShaderGroupCreateInfoKHR &group = groups.AddDefaulted();
        group = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        group.type = hit.Kind == RhiHitGroupKind::Procedural ? VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR
                                                             : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = hit.ClosestHit;
        group.anyHitShader = hit.AnyHit;
        group.intersectionShader = hit.Intersection;
    }
    pipeline.GroupCount = static_cast<uint32>(groups.Count());
    VkPipelineCreateFlags2CreateInfo flags = DescriptorHeapFlags();
    VkRayTracingPipelineCreateInfoKHR info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    info.pNext = &flags;
    info.stageCount = static_cast<uint32>(stages.Stages.Count());
    info.pStages = stages.Stages.Data();
    info.groupCount = pipeline.GroupCount;
    info.pGroups = groups.Data();
    info.maxPipelineRayRecursionDepth = pipeline.MaxRecursionDepth;
    return Vk.vkCreateRayTracingPipelinesKHR(_device, VK_NULL_HANDLE, _pipelineCache, 1, &info, nullptr,
                                             &pipeline.Pipeline);
}

void VulkanDevice::Destroy(RhiPipeline pipeline)
{
    if (VulkanPipeline *entry = _pipelines.Find(pipeline))
    {
        Jobs::Wait(entry->Compile);
    }
    DeferRelease(pipeline, [](void *context, uint64 payload) {
        auto *device = static_cast<VulkanDevice *>(context);
        const RhiPipeline handle{static_cast<uint32>(payload), static_cast<uint32>(payload >> 32)};
        if (const VulkanPipeline *entry = device->_pipelines.Find(handle))
        {
            if (entry->Pipeline != VK_NULL_HANDLE)
            {
                Vk.vkDestroyPipeline(device->_device, entry->Pipeline, nullptr);
            }
            device->_pipelines.Free(handle);
        }
    });
}

RhiPipelineState VulkanDevice::GetPipelineState(RhiPipeline pipeline) const
{
    const VulkanPipeline *entry = _pipelines.Find(pipeline);
    return entry != nullptr ? static_cast<RhiPipelineState>(entry->State.Load(MemoryOrder::Acquire))
                            : RhiPipelineState::Failed;
}

RhiPipelineState VulkanDevice::WaitForPipeline(RhiPipeline pipeline)
{
    VulkanPipeline *entry = _pipelines.Find(pipeline);
    if (entry == nullptr)
    {
        return RhiPipelineState::Failed;
    }
    Jobs::Wait(entry->Compile);
    return static_cast<RhiPipelineState>(entry->State.Load(MemoryOrder::Acquire));
}

void VulkanDevice::GetShaderGroupHandles(RhiPipeline pipeline, uint32 first, uint32 count, Span<uint8> output) const
{
    const VulkanPipeline *entry = _pipelines.Find(pipeline);
    if (entry == nullptr || entry->Pipeline == VK_NULL_HANDLE || entry->Kind != RhiPipelineKind::RayTracing)
    {
        return;
    }
    Vk.vkGetRayTracingShaderGroupHandlesKHR(_device, entry->Pipeline, first, count, output.Count(), output.Data());
}

List<uint8> VulkanDevice::GetPipelineCacheData() const
{
    List<uint8> data;
    usize size = 0;
    if (Vk.vkGetPipelineCacheData(_device, _pipelineCache, &size, nullptr) != VK_SUCCESS)
    {
        return data;
    }
    data.Resize(size);
    if (Vk.vkGetPipelineCacheData(_device, _pipelineCache, &size, data.Data()) != VK_SUCCESS)
    {
        data.Clear();
    }
    return data;
}
} // namespace Marlin
