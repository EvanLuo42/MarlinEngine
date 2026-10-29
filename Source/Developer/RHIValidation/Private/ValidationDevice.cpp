#include "ValidationDevice.h"

#include "Concurrency/ScopeLock.h"
#include "HAL/PlatformDebug.h"
#include "HAL/PlatformThread.h"
#include "Misc/CommandLine.h"
#include "ValidationCommandList.h"
#include "ValidationLayer.h"

MARLIN_DEFINE_LOG_CATEGORY(LogRhiValidation);

namespace Marlin
{
namespace Private
{
void BreakOnValidationError()
{
    if (PlatformDebug::IsDebuggerAttached())
    {
        __builtin_debugtrap();
    }
}
} // namespace Private

namespace
{
template <typename TValue, typename THandle>
bool Remove(ReadWriteLock &lock, HashMap<uint64, TValue> &map, THandle handle, const char *kind)
{
    if (!handle.IsValid())
    {
        return false;
    }
    const ScopeLock scope(lock);
    if (!map.Remove(HandleKey(handle)))
    {
        MARLIN_RHI_VALIDATION_ERROR("Destroy of a {0} that is stale or was destroyed already (slot {1})", kind,
                                    handle.Index);
        return false;
    }
    return true;
}

template <typename TValue, typename THandle>
bool IsTracked(ReadWriteLock &lock, const HashMap<uint64, TValue> &map, THandle handle)
{
    const SharedScopeLock scope(lock);
    return map.Contains(HandleKey(handle));
}

uint32 MipCount(const RhiTextureDesc &desc)
{
    uint32 size = desc.Width > desc.Height ? desc.Width : desc.Height;
    if (desc.Dimension == RhiTextureDimension::Texture3D && desc.DepthOrArraySize > size)
    {
        size = desc.DepthOrArraySize;
    }
    uint32 count = 1;
    while (size > 1)
    {
        size >>= 1;
        ++count;
    }
    return count;
}
} // namespace

Name RhiValidationLayer::GetName() const
{
    return Name("RHIValidation");
}

int32 RhiValidationLayer::GetPriority() const
{
    return 0;
}

bool RhiValidationLayer::IsEnabled() const
{
    return CommandLine::HasSwitch("RHIValidation");
}

RhiDevice *RhiValidationLayer::Wrap(RhiDevice &inner)
{
    return new ValidationDevice(inner);
}

void RhiValidationLayer::Unwrap(RhiDevice *outer)
{
    delete static_cast<ValidationDevice *>(outer);
}

ValidationDevice::ValidationDevice(RhiDevice &inner) : _inner(inner)
{
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        _fences.Add(HandleKey(inner.GetQueueFence(static_cast<RhiQueueType>(queue))), 0);
    }
    // The built-in signatures of the plain commands are the device's, alive as long as it is; ExecuteIndirect takes
    // them like the ones CreateCommandSignature made.
    for (const RhiIndirectCommandKind kind : {RhiIndirectCommandKind::Draw, RhiIndirectCommandKind::DrawIndexed,
                                              RhiIndirectCommandKind::DrawMesh, RhiIndirectCommandKind::Dispatch,
                                              RhiIndirectCommandKind::DispatchRays})
    {
        const RhiCommandSignature signature = inner.GetCommandSignature(kind);
        if (signature.IsValid())
        {
            _commandSignatures.Add(HandleKey(signature), kind);
            _builtInSignatures.Add(HandleKey(signature));
        }
    }
}

ValidationDevice::~ValidationDevice()
{
    if (_liveLists.Load() != 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} command lists were begun and never submitted or discarded", _liveLists.Load());
    }
}

ValidationDevice::SubmitScope::SubmitScope(ValidationDevice &device, const char *call) : _device(device)
{
    const uint32 thread = PlatformThread::CurrentId();
    uint32 expected = 0;
    _owner = device._submitThread.CompareExchange(expected, thread);
    if (!_owner && expected != thread)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} on thread {1} while thread {2} submits; submissions must not overlap", call,
                                    thread, expected);
    }
}

ValidationDevice::SubmitScope::~SubmitScope()
{
    if (_owner)
    {
        _device._submitThread.Store(0);
    }
}

bool ValidationDevice::CheckTextureDesc(const RhiTextureDesc &desc) const
{
    const RhiDeviceLimits &limits = _inner.GetLimits();
    bool ok = true;
    if (desc.Format == RhiFormat::Unknown || desc.Width == 0 || desc.Height == 0 || desc.DepthOrArraySize == 0 ||
        desc.MipCount == 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("Texture {0}: format, size, depth or array size, or mip count is zero",
                                    desc.DebugName);
        ok = false;
    }
    if (desc.MipCount > MipCount(desc))
    {
        MARLIN_RHI_VALIDATION_ERROR("Texture {0}: {1} mips, but a {2}x{3} texture has {4}", desc.DebugName,
                                    desc.MipCount, desc.Width, desc.Height, MipCount(desc));
        ok = false;
    }
    if (desc.SampleCount > 1 && (desc.MipCount > 1 || desc.Dimension != RhiTextureDimension::Texture2D))
    {
        MARLIN_RHI_VALIDATION_ERROR("Texture {0}: multisampled textures are 2D with one mip", desc.DebugName);
        ok = false;
    }
    if (desc.Width > limits.MaxTextureSize2D || desc.Height > limits.MaxTextureSize2D)
    {
        MARLIN_RHI_VALIDATION_ERROR("Texture {0}: {1}x{2} is larger than the device allows ({3})", desc.DebugName,
                                    desc.Width, desc.Height, limits.MaxTextureSize2D);
        ok = false;
    }
    if (HasAnyFlags(desc.Usage, RhiTextureUsage::Cube) && (desc.Width != desc.Height || desc.DepthOrArraySize % 6 != 0))
    {
        MARLIN_RHI_VALIDATION_ERROR("Texture {0}: cube textures are square with a multiple of six slices",
                                    desc.DebugName);
        ok = false;
    }
    if (HasAnyFlags(desc.Usage, RhiTextureUsage::DepthStencil) && !IsDepthFormat(desc.Format))
    {
        MARLIN_RHI_VALIDATION_ERROR("Texture {0}: DepthStencil usage needs a depth format", desc.DebugName);
        ok = false;
    }
    if (HasAnyFlags(desc.Usage, RhiTextureUsage::RenderTarget) && IsDepthFormat(desc.Format))
    {
        MARLIN_RHI_VALIDATION_ERROR("Texture {0}: a depth format cannot be a render target", desc.DebugName);
        ok = false;
    }
    for (const RhiFormat format : desc.ViewFormats)
    {
        if (GetFormatInfo(format).BlockBytes != GetFormatInfo(desc.Format).BlockBytes)
        {
            MARLIN_RHI_VALIDATION_ERROR("Texture {0}: view format {1} has another block size than {2}", desc.DebugName,
                                        GetFormatInfo(format).Name, GetFormatInfo(desc.Format).Name);
            ok = false;
        }
    }
    return ok;
}

Name ValidationDevice::GetBackendName() const
{
    return _inner.GetBackendName();
}

const RhiAdapterInfo &ValidationDevice::GetAdapterInfo() const
{
    return _inner.GetAdapterInfo();
}

const RhiDeviceLimits &ValidationDevice::GetLimits() const
{
    return _inner.GetLimits();
}

Span<const Name> ValidationDevice::GetFeatures() const
{
    return _inner.GetFeatures();
}

RhiNativeHandles ValidationDevice::GetNativeHandles() const
{
    return _inner.GetNativeHandles();
}

RhiMemoryBudget ValidationDevice::GetMemoryBudget(RhiMemoryLocation location) const
{
    return _inner.GetMemoryBudget(location);
}

void ValidationDevice::GetMemoryStats(List<RhiMemoryPoolStats> &pools) const
{
    _inner.GetMemoryStats(pools);
}

Result<RhiHeap, RhiError> ValidationDevice::CreateHeap(const RhiHeapDesc &desc)
{
    if (desc.Size == 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("Heap {0} has size zero", desc.DebugName);
    }
    Result<RhiHeap, RhiError> heap = _inner.CreateHeap(desc);
    if (heap.IsOk())
    {
        const ScopeLock scope(_lock);
        _heaps.Add(HandleKey(heap.Value()), 0);
    }
    return heap;
}

void ValidationDevice::Destroy(RhiHeap heap)
{
    if (Remove(_lock, _heaps, heap, "heap"))
    {
        _inner.Destroy(heap);
    }
}

RhiAllocationInfo ValidationDevice::GetAllocationInfo(const RhiBufferDesc &desc) const
{
    return _inner.GetAllocationInfo(desc);
}

RhiAllocationInfo ValidationDevice::GetAllocationInfo(const RhiTextureDesc &desc) const
{
    return _inner.GetAllocationInfo(desc);
}

Result<RhiBuffer, RhiError> ValidationDevice::CreateBuffer(const RhiBufferDesc &desc)
{
    if (desc.Size == 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("Buffer {0} has size zero", desc.DebugName);
    }
    if (HasAnyFlags(desc.Flags, RhiResourceFlags::Reserved) && desc.Location != RhiMemoryLocation::Device)
    {
        MARLIN_RHI_VALIDATION_ERROR("Buffer {0}: reserved buffers live in Device memory", desc.DebugName);
    }
    if (HasAnyFlags(desc.Flags, RhiResourceFlags::Transient))
    {
        MARLIN_RHI_VALIDATION_ERROR("Buffer {0}: only render targets can be Transient", desc.DebugName);
    }
    if (desc.Placement.Heap.IsValid() && !IsTracked(_lock, _heaps, desc.Placement.Heap))
    {
        MARLIN_RHI_VALIDATION_ERROR("Buffer {0} is placed in a stale heap", desc.DebugName);
    }
    Result<RhiBuffer, RhiError> buffer = _inner.CreateBuffer(desc);
    if (buffer.IsOk())
    {
        RhiBufferDesc tracked = desc;
        tracked.DebugName = {};
        const ScopeLock scope(_lock);
        _buffers.Add(HandleKey(buffer.Value()), tracked);
    }
    return buffer;
}

void ValidationDevice::Destroy(RhiBuffer buffer)
{
    if (Remove(_lock, _buffers, buffer, "buffer"))
    {
        _inner.Destroy(buffer);
    }
}

uint8 *ValidationDevice::GetMappedData(RhiBuffer buffer) const
{
    const Optional<RhiBufferDesc> desc = FindBuffer(buffer);
    if (!desc.HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("GetMappedData of a stale buffer (slot {0})", buffer.Index);
        return nullptr;
    }
    if (desc->Location == RhiMemoryLocation::Device)
    {
        MARLIN_RHI_VALIDATION_ERROR("GetMappedData of a Device buffer, which is not mapped");
    }
    return _inner.GetMappedData(buffer);
}

uint64 ValidationDevice::GetBufferAddress(RhiBuffer buffer) const
{
    if (!FindBuffer(buffer).HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("GetBufferAddress of a stale buffer (slot {0})", buffer.Index);
    }
    return _inner.GetBufferAddress(buffer);
}

Result<RhiTexture, RhiError> ValidationDevice::CreateTexture(const RhiTextureDesc &desc)
{
    CheckTextureDesc(desc);
    if (HasAnyFlags(desc.Usage, RhiTextureUsage::Present))
    {
        MARLIN_RHI_VALIDATION_ERROR("Texture {0}: only swapchains make Present textures", desc.DebugName);
    }
    Result<RhiTexture, RhiError> texture = _inner.CreateTexture(desc);
    if (texture.IsOk())
    {
        RhiTextureDesc tracked = desc;
        tracked.DebugName = {};
        tracked.ViewFormats = {};
        const ScopeLock scope(_lock);
        _textures.Add(HandleKey(texture.Value()), tracked);
    }
    return texture;
}

void ValidationDevice::Destroy(RhiTexture texture)
{
    {
        const SharedScopeLock scope(_lock);
        if (_swapchainImages.Contains(HandleKey(texture)))
        {
            MARLIN_RHI_VALIDATION_ERROR("Destroy of a swapchain image; destroy the swapchain instead");
            return;
        }
    }
    if (Remove(_lock, _textures, texture, "texture"))
    {
        _inner.Destroy(texture);
    }
}

const RhiTextureDesc &ValidationDevice::GetTextureDesc(RhiTexture texture) const
{
    if (!FindTexture(texture).HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("GetTextureDesc of a stale texture (slot {0})", texture.Index);
    }
    return _inner.GetTextureDesc(texture);
}

RhiNativeTexture ValidationDevice::GetNativeTexture(RhiTexture texture, const RhiSubresourceRange &range,
                                                    RhiFormat viewFormat)
{
    const Optional<RhiTextureDesc> desc = FindTexture(texture);
    if (!desc.HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("GetNativeTexture of a stale texture (slot {0})", texture.Index);
        return {};
    }
    if (range.FirstMip >= desc->MipCount)
    {
        MARLIN_RHI_VALIDATION_ERROR("GetNativeTexture: mip {0} is outside the texture", range.FirstMip);
    }
    return _inner.GetNativeTexture(texture, range, viewFormat);
}

RhiNativeBuffer ValidationDevice::GetNativeBuffer(RhiBuffer buffer) const
{
    if (!FindBuffer(buffer).HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("GetNativeBuffer of a stale buffer (slot {0})", buffer.Index);
        return {};
    }
    return _inner.GetNativeBuffer(buffer);
}

RhiTileInfo ValidationDevice::GetTileInfo(RhiTexture texture) const
{
    const Optional<RhiTextureDesc> desc = FindTexture(texture);
    if (!desc.HasValue() || !HasAnyFlags(desc->Flags, RhiResourceFlags::Reserved))
    {
        MARLIN_RHI_VALIDATION_ERROR("GetTileInfo of a texture that is stale or not reserved");
    }
    return _inner.GetTileInfo(texture);
}

Result<RhiResourceView, RhiError> ValidationDevice::CreateView(const RhiTextureViewDesc &desc)
{
    const Optional<RhiTextureDesc> texture = FindTexture(desc.Texture);
    if (!texture.HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0} of a stale texture", desc.DebugName);
        return Failure(RhiError::Unsupported);
    }
    const RhiTextureUsage needed = desc.Access == RhiViewAccess::UnorderedAccess ? RhiTextureUsage::UnorderedAccess
                                                                                 : RhiTextureUsage::ShaderResource;
    if (!HasAnyFlags(texture->Usage, needed))
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0}: the texture lacks the {1} usage", desc.DebugName,
                                    needed == RhiTextureUsage::UnorderedAccess ? "UnorderedAccess" : "ShaderResource");
    }
    const uint32 slices = texture->Dimension == RhiTextureDimension::Texture3D ? 1 : texture->DepthOrArraySize;
    const uint32 mipCount = desc.Range.MipCount == 0 ? texture->MipCount - desc.Range.FirstMip : desc.Range.MipCount;
    const uint32 sliceCount = desc.Range.SliceCount == 0 ? slices - desc.Range.FirstSlice : desc.Range.SliceCount;
    if (desc.Range.FirstMip + mipCount > texture->MipCount || desc.Range.FirstSlice + sliceCount > slices)
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0}: mips {1}+{2} or slices {3}+{4} are outside the texture", desc.DebugName,
                                    desc.Range.FirstMip, mipCount, desc.Range.FirstSlice, sliceCount);
    }
    if (desc.Access == RhiViewAccess::UnorderedAccess && mipCount != 1)
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0}: an UnorderedAccess view covers one mip", desc.DebugName);
    }
    const bool cube = desc.Dimension == RhiTextureViewDimension::TextureCube ||
                      desc.Dimension == RhiTextureViewDimension::TextureCubeArray;
    if (cube && (!HasAnyFlags(texture->Usage, RhiTextureUsage::Cube) || sliceCount % 6 != 0))
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0}: cube views need a Cube texture and a multiple of six slices",
                                    desc.DebugName);
    }
    Result<RhiResourceView, RhiError> view = _inner.CreateView(desc);
    if (view.IsOk())
    {
        const ScopeLock scope(_lock);
        _views.Add(HandleKey(view.Value()), 0);
    }
    return view;
}

Result<RhiResourceView, RhiError> ValidationDevice::CreateView(const RhiBufferViewDesc &desc)
{
    const Optional<RhiBufferDesc> buffer = FindBuffer(desc.Buffer);
    if (!buffer.HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0} of a stale buffer", desc.DebugName);
        return Failure(RhiError::Unsupported);
    }
    const RhiBufferUsage needed = desc.Access == RhiViewAccess::UnorderedAccess ? RhiBufferUsage::UnorderedAccess
                                                                                : RhiBufferUsage::ShaderResource;
    if (!HasAnyFlags(buffer->Usage, needed))
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0}: the buffer lacks the {1} usage", desc.DebugName,
                                    needed == RhiBufferUsage::UnorderedAccess ? "UnorderedAccess" : "ShaderResource");
    }
    const uint64 size = desc.Size == 0 ? buffer->Size - desc.Offset : desc.Size;
    if (desc.Offset + size > buffer->Size)
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0}: bytes {1}+{2} are outside the {3}-byte buffer", desc.DebugName,
                                    desc.Offset, size, buffer->Size);
    }
    if (desc.Kind == RhiBufferViewKind::Structured && desc.Stride == 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0}: a structured view needs a stride", desc.DebugName);
    }
    if (desc.Kind == RhiBufferViewKind::Typed && desc.Format == RhiFormat::Unknown)
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0}: a typed view needs a format", desc.DebugName);
    }
    if (desc.Kind == RhiBufferViewKind::Raw && (desc.Offset % 4 != 0 || size % 4 != 0))
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0}: raw views are in 4-byte words", desc.DebugName);
    }
    if (desc.Offset % _inner.GetLimits().StructuredBufferOffsetAlignment != 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("View {0}: offset {1} is not aligned to {2}", desc.DebugName, desc.Offset,
                                    _inner.GetLimits().StructuredBufferOffsetAlignment);
    }
    Result<RhiResourceView, RhiError> view = _inner.CreateView(desc);
    if (view.IsOk())
    {
        const ScopeLock scope(_lock);
        _views.Add(HandleKey(view.Value()), 0);
    }
    return view;
}

Result<RhiResourceView, RhiError> ValidationDevice::CreateView(RhiAccelerationStructure structure)
{
    if (!IsLive(structure))
    {
        MARLIN_RHI_VALIDATION_ERROR("View of a stale acceleration structure");
        return Failure(RhiError::Unsupported);
    }
    Result<RhiResourceView, RhiError> view = _inner.CreateView(structure);
    if (view.IsOk())
    {
        const ScopeLock scope(_lock);
        _views.Add(HandleKey(view.Value()), 0);
    }
    return view;
}

void ValidationDevice::Destroy(RhiResourceView view)
{
    if (Remove(_lock, _views, view, "view"))
    {
        _inner.Destroy(view);
    }
}

Result<RhiSampler, RhiError> ValidationDevice::CreateSampler(const RhiSamplerDesc &desc)
{
    if (desc.MaxAnisotropy == 0 || desc.MaxAnisotropy > 16 || desc.MinLod > desc.MaxLod)
    {
        MARLIN_RHI_VALIDATION_ERROR("Sampler: anisotropy {0} is outside 1..16, or MinLod is above MaxLod",
                                    desc.MaxAnisotropy);
    }
    Result<RhiSampler, RhiError> sampler = _inner.CreateSampler(desc);
    if (sampler.IsOk())
    {
        const ScopeLock scope(_lock);
        ++_samplers.FindOrAdd(HandleKey(sampler.Value()));
    }
    return sampler;
}

void ValidationDevice::Destroy(RhiSampler sampler)
{
    {
        const ScopeLock scope(_lock);
        uint32 *references = _samplers.Find(HandleKey(sampler));
        if (references == nullptr)
        {
            MARLIN_RHI_VALIDATION_ERROR("Destroy of a sampler that is stale or was destroyed as often as created");
            return;
        }
        if (--*references == 0)
        {
            _samplers.Remove(HandleKey(sampler));
        }
    }
    _inner.Destroy(sampler);
}

RhiAccelerationStructureSizes ValidationDevice::GetAccelerationStructureSizes(
    const RhiAccelerationStructureInputs &inputs) const
{
    return _inner.GetAccelerationStructureSizes(inputs);
}

Result<RhiAccelerationStructure, RhiError> ValidationDevice::CreateAccelerationStructure(
    const RhiAccelerationStructureDesc &desc)
{
    const Optional<RhiBufferDesc> buffer = FindBuffer(desc.Buffer);
    if (!buffer.HasValue() || !HasAnyFlags(buffer->Usage, RhiBufferUsage::AccelerationStructure))
    {
        MARLIN_RHI_VALIDATION_ERROR("Acceleration structure {0}: the buffer is stale or lacks the "
                                    "AccelerationStructure usage",
                                    desc.DebugName);
    }
    else if (desc.Offset % 256 != 0 || desc.Offset + desc.Size > buffer->Size)
    {
        MARLIN_RHI_VALIDATION_ERROR("Acceleration structure {0}: bytes {1}+{2} are unaligned or outside the buffer",
                                    desc.DebugName, desc.Offset, desc.Size);
    }
    Result<RhiAccelerationStructure, RhiError> structure = _inner.CreateAccelerationStructure(desc);
    if (structure.IsOk())
    {
        const ScopeLock scope(_lock);
        _accelerationStructures.Add(HandleKey(structure.Value()), 0);
    }
    return structure;
}

void ValidationDevice::Destroy(RhiAccelerationStructure structure)
{
    if (Remove(_lock, _accelerationStructures, structure, "acceleration structure"))
    {
        _inner.Destroy(structure);
    }
}

uint64 ValidationDevice::GetAccelerationStructureAddress(RhiAccelerationStructure structure) const
{
    if (!IsLive(structure))
    {
        MARLIN_RHI_VALIDATION_ERROR("GetAccelerationStructureAddress of a stale structure");
    }
    return _inner.GetAccelerationStructureAddress(structure);
}

RhiPipeline ValidationDevice::CreatePipeline(const RhiGraphicsPipelineDesc &desc)
{
    RhiShaderStage stages = RhiShaderStage::None;
    for (const RhiShader &shader : desc.Shaders)
    {
        if (HasAnyFlags(stages, shader.Stage) || shader.Code.IsEmpty())
        {
            MARLIN_RHI_VALIDATION_ERROR("Pipeline {0}: a stage appears twice or has no code", desc.DebugName);
        }
        stages |= shader.Stage;
    }
    const bool vertex = HasAnyFlags(stages, RhiShaderStage::Vertex);
    const bool mesh = HasAnyFlags(stages, RhiShaderStage::Mesh);
    if (vertex == mesh || (HasAnyFlags(stages, RhiShaderStage::Task) && !mesh) ||
        HasAnyFlags(stages,
                    ~(RhiShaderStage::Vertex | RhiShaderStage::Task | RhiShaderStage::Mesh | RhiShaderStage::Pixel)))
    {
        MARLIN_RHI_VALIDATION_ERROR("Pipeline {0}: graphics pipelines are Vertex, or Task and Mesh, then Pixel",
                                    desc.DebugName);
    }
    if (desc.RenderTargetFormats.Count() > RhiMaxRenderTargets)
    {
        MARLIN_RHI_VALIDATION_ERROR("Pipeline {0}: more than {1} render targets", desc.DebugName, RhiMaxRenderTargets);
    }
    for (const RhiFormat format : desc.RenderTargetFormats)
    {
        if (format == RhiFormat::Unknown || IsDepthFormat(format))
        {
            MARLIN_RHI_VALIDATION_ERROR("Pipeline {0}: render target format {1} is not a color format", desc.DebugName,
                                        GetFormatInfo(format).Name);
        }
    }
    if (desc.DepthStencilFormat != RhiFormat::Unknown && !IsDepthFormat(desc.DepthStencilFormat))
    {
        MARLIN_RHI_VALIDATION_ERROR("Pipeline {0}: {1} is not a depth format", desc.DebugName,
                                    GetFormatInfo(desc.DepthStencilFormat).Name);
    }
    if ((desc.DepthStencil.DepthTest || desc.DepthStencil.StencilTest) && desc.DepthStencilFormat == RhiFormat::Unknown)
    {
        MARLIN_RHI_VALIDATION_ERROR("Pipeline {0}: depth or stencil testing without a depth format", desc.DebugName);
    }
    const RhiPipeline pipeline = _inner.CreatePipeline(desc);
    if (pipeline.IsValid())
    {
        const ScopeLock scope(_lock);
        _pipelines.Add(HandleKey(pipeline), RhiPipelineKind::Graphics);
    }
    return pipeline;
}

RhiPipeline ValidationDevice::CreatePipeline(const RhiComputePipelineDesc &desc)
{
    if (desc.Shader.Stage != RhiShaderStage::Compute || desc.Shader.Code.IsEmpty())
    {
        MARLIN_RHI_VALIDATION_ERROR("Pipeline {0}: a compute pipeline needs Compute code", desc.DebugName);
    }
    const RhiPipeline pipeline = _inner.CreatePipeline(desc);
    if (pipeline.IsValid())
    {
        const ScopeLock scope(_lock);
        _pipelines.Add(HandleKey(pipeline), RhiPipelineKind::Compute);
    }
    return pipeline;
}

RhiPipeline ValidationDevice::CreatePipeline(const RhiRayTracingPipelineDesc &desc)
{
    const auto checkShader = [&](uint32 index, RhiShaderStage stage) {
        if (index != ~0u && (index >= desc.Shaders.Count() || desc.Shaders[index].Stage != stage))
        {
            MARLIN_RHI_VALIDATION_ERROR("Pipeline {0}: hit group shader {1} is missing or of the wrong stage",
                                        desc.DebugName, index);
        }
    };
    bool rayGeneration = false;
    for (const RhiShader &shader : desc.Shaders)
    {
        rayGeneration = rayGeneration || shader.Stage == RhiShaderStage::RayGeneration;
    }
    if (!rayGeneration)
    {
        MARLIN_RHI_VALIDATION_ERROR("Pipeline {0}: no ray generation shader", desc.DebugName);
    }
    for (const RhiHitGroup &group : desc.HitGroups)
    {
        checkShader(group.ClosestHit, RhiShaderStage::ClosestHit);
        checkShader(group.AnyHit, RhiShaderStage::AnyHit);
        checkShader(group.Intersection, RhiShaderStage::Intersection);
        if ((group.Kind == RhiHitGroupKind::Procedural) != (group.Intersection != ~0u))
        {
            MARLIN_RHI_VALIDATION_ERROR("Pipeline {0}: procedural hit groups, and only they, have an intersection",
                                        desc.DebugName);
        }
    }
    if (desc.MaxRecursionDepth > _inner.GetLimits().MaxRayRecursionDepth)
    {
        MARLIN_RHI_VALIDATION_ERROR("Pipeline {0}: recursion depth {1} is above the device's {2}", desc.DebugName,
                                    desc.MaxRecursionDepth, _inner.GetLimits().MaxRayRecursionDepth);
    }
    const RhiPipeline pipeline = _inner.CreatePipeline(desc);
    if (pipeline.IsValid())
    {
        const ScopeLock scope(_lock);
        _pipelines.Add(HandleKey(pipeline), RhiPipelineKind::RayTracing);
    }
    return pipeline;
}

void ValidationDevice::Destroy(RhiPipeline pipeline)
{
    if (Remove(_lock, _pipelines, pipeline, "pipeline"))
    {
        _inner.Destroy(pipeline);
    }
}

RhiPipelineState ValidationDevice::GetPipelineState(RhiPipeline pipeline) const
{
    if (!IsLive(pipeline))
    {
        MARLIN_RHI_VALIDATION_ERROR("GetPipelineState of a stale pipeline");
    }
    return _inner.GetPipelineState(pipeline);
}

RhiPipelineState ValidationDevice::WaitForPipeline(RhiPipeline pipeline)
{
    if (!IsLive(pipeline))
    {
        MARLIN_RHI_VALIDATION_ERROR("WaitForPipeline of a stale pipeline");
    }
    return _inner.WaitForPipeline(pipeline);
}

void ValidationDevice::GetShaderGroupHandles(RhiPipeline pipeline, uint32 first, uint32 count, Span<uint8> output) const
{
    const Optional<RhiPipelineKind> kind = FindPipelineKind(pipeline);
    if (!kind.HasValue() || kind.Value() != RhiPipelineKind::RayTracing ||
        _inner.GetPipelineState(pipeline) != RhiPipelineState::Ready)
    {
        MARLIN_RHI_VALIDATION_ERROR("GetShaderGroupHandles of a pipeline that is not a ready ray tracing pipeline");
    }
    if (output.Count() < uint64{count} * _inner.GetLimits().ShaderGroupHandleSize)
    {
        MARLIN_RHI_VALIDATION_ERROR("GetShaderGroupHandles: {0} bytes hold fewer than {1} handles", output.Count(),
                                    count);
    }
    _inner.GetShaderGroupHandles(pipeline, first, count, output);
}

List<uint8> ValidationDevice::GetPipelineCacheData() const
{
    return _inner.GetPipelineCacheData();
}

Result<RhiIndirectPipelineSet, RhiError> ValidationDevice::CreateIndirectPipelineSet(
    const RhiIndirectPipelineSetDesc &desc)
{
    for (const RhiPipeline pipeline : desc.Pipelines)
    {
        if (!IsLive(pipeline) || _inner.GetPipelineState(pipeline) != RhiPipelineState::Ready)
        {
            MARLIN_RHI_VALIDATION_ERROR("Indirect pipeline set {0}: a pipeline is stale or not ready", desc.DebugName);
        }
    }
    return _inner.CreateIndirectPipelineSet(desc);
}

void ValidationDevice::Destroy(RhiIndirectPipelineSet set)
{
    _inner.Destroy(set);
}

Result<RhiCommandSignature, RhiError> ValidationDevice::CreateCommandSignature(const RhiCommandSignatureDesc &desc)
{
    if (desc.Commands.IsEmpty() || desc.Commands.Last().Kind < RhiIndirectCommandKind::Draw)
    {
        MARLIN_RHI_VALIDATION_ERROR("Command signature {0}: the last command must draw, dispatch or trace",
                                    desc.DebugName);
        return Failure(RhiError::Unsupported);
    }
    for (usize i = 0; i + 1 < desc.Commands.Count(); ++i)
    {
        const RhiIndirectCommand &command = desc.Commands[i];
        if (command.Kind >= RhiIndirectCommandKind::Draw)
        {
            MARLIN_RHI_VALIDATION_ERROR("Command signature {0}: only the last command draws, dispatches or traces",
                                        desc.DebugName);
        }
        if (command.Kind == RhiIndirectCommandKind::ShaderArguments &&
            (command.Offset + command.Size > RhiShaderArgumentsSize || command.Offset % 4 != 0))
        {
            MARLIN_RHI_VALIDATION_ERROR("Command signature {0}: shader arguments {1}+{2} are unaligned or past {3}",
                                        desc.DebugName, command.Offset, command.Size, RhiShaderArgumentsSize);
        }
        if (command.Kind == RhiIndirectCommandKind::SetPipeline && !desc.PipelineSet.IsValid())
        {
            MARLIN_RHI_VALIDATION_ERROR("Command signature {0}: SetPipeline without a pipeline set", desc.DebugName);
        }
    }
    Result<RhiCommandSignature, RhiError> signature = _inner.CreateCommandSignature(desc);
    if (signature.IsOk())
    {
        const ScopeLock scope(_lock);
        _commandSignatures.Add(HandleKey(signature.Value()), desc.Commands.Last().Kind);
    }
    return signature;
}

void ValidationDevice::Destroy(RhiCommandSignature signature)
{
    if (_builtInSignatures.Contains(HandleKey(signature)))
    {
        MARLIN_RHI_VALIDATION_ERROR("Destroy: a built-in command signature belongs to the device");
        return;
    }
    if (Remove(_lock, _commandSignatures, signature, "command signature"))
    {
        _inner.Destroy(signature);
    }
}

RhiCommandSignature ValidationDevice::GetCommandSignature(RhiIndirectCommandKind kind) const
{
    const RhiCommandSignature signature = _inner.GetCommandSignature(kind);
    if (!signature.IsValid())
    {
        MARLIN_RHI_VALIDATION_ERROR("GetCommandSignature: only plain commands have a built-in signature");
    }
    return signature;
}

Result<RhiQueryPool, RhiError> ValidationDevice::CreateQueryPool(const RhiQueryPoolDesc &desc)
{
    if (desc.Count == 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("Query pool {0} holds no queries", desc.DebugName);
    }
    Result<RhiQueryPool, RhiError> pool = _inner.CreateQueryPool(desc);
    if (pool.IsOk())
    {
        RhiQueryPoolDesc tracked = desc;
        tracked.DebugName = {};
        const ScopeLock scope(_lock);
        _queryPools.Add(HandleKey(pool.Value()), tracked);
    }
    return pool;
}

void ValidationDevice::Destroy(RhiQueryPool pool)
{
    if (Remove(_lock, _queryPools, pool, "query pool"))
    {
        _inner.Destroy(pool);
    }
}

bool ValidationDevice::ReadQueries(RhiQueryPool pool, uint32 first, uint32 count, Span<uint64> output)
{
    const Optional<RhiQueryPoolDesc> desc = FindQueryPool(pool);
    if (!desc || first + count > desc->Count)
    {
        MARLIN_RHI_VALIDATION_ERROR("ReadQueries: the pool is stale, or queries {0}+{1} are outside it", first, count);
        return false;
    }
    const uint32 values = desc->Kind == RhiQueryKind::PipelineStatistics ? 9 : 1;
    if (output.Count() < usize{count} * values)
    {
        MARLIN_RHI_VALIDATION_ERROR("ReadQueries: {0} results need {1} values; the output holds {2}", count,
                                    count * values, output.Count());
        return false;
    }
    return _inner.ReadQueries(pool, first, count, output);
}

void ValidationDevice::GetCalibratedTimestamps(RhiQueueType queue, uint64 &cpuTicks, uint64 &gpuTicks) const
{
    _inner.GetCalibratedTimestamps(queue, cpuTicks, gpuTicks);
}

Result<RhiFence, RhiError> ValidationDevice::CreateFence(uint64 initialValue, StringView debugName)
{
    Result<RhiFence, RhiError> fence = _inner.CreateFence(initialValue, debugName);
    if (fence.IsOk())
    {
        const ScopeLock scope(_lock);
        _fences.Add(HandleKey(fence.Value()), initialValue);
    }
    return fence;
}

void ValidationDevice::Destroy(RhiFence fence)
{
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        if (fence == _inner.GetQueueFence(static_cast<RhiQueueType>(queue)))
        {
            MARLIN_RHI_VALIDATION_ERROR("Destroy of a queue's own fence");
            return;
        }
    }
    if (Remove(_lock, _fences, fence, "fence"))
    {
        _inner.Destroy(fence);
    }
}

uint64 ValidationDevice::GetCompletedValue(RhiFence fence) const
{
    if (!IsTracked(_lock, _fences, fence))
    {
        MARLIN_RHI_VALIDATION_ERROR("GetCompletedValue of a stale fence");
    }
    return _inner.GetCompletedValue(fence);
}

bool ValidationDevice::Wait(RhiFence fence, uint64 value, uint32 timeout)
{
    {
        const SharedScopeLock scope(_lock);
        const uint64 *signaled = _fences.Find(HandleKey(fence));
        if (signaled == nullptr)
        {
            MARLIN_RHI_VALIDATION_ERROR("Wait on a stale fence");
        }
        else if (timeout == 0xFFFFFFFFu && value > *signaled)
        {
            MARLIN_RHI_VALIDATION_ERROR("Wait forever for value {0}, but nothing will signal past {1} yet", value,
                                        *signaled);
        }
    }
    return _inner.Wait(fence, value, timeout);
}

void ValidationDevice::Signal(RhiFence fence, uint64 value)
{
    {
        const ScopeLock scope(_lock);
        uint64 *signaled = _fences.Find(HandleKey(fence));
        if (signaled == nullptr)
        {
            MARLIN_RHI_VALIDATION_ERROR("Signal of a stale fence");
            return;
        }
        if (value <= *signaled)
        {
            MARLIN_RHI_VALIDATION_ERROR("Signal of value {0}; the fence already reaches {1}", value, *signaled);
            return;
        }
        *signaled = value;
    }
    _inner.Signal(fence, value);
}

RhiFence ValidationDevice::GetQueueFence(RhiQueueType queue) const
{
    return _inner.GetQueueFence(queue);
}

RhiQueueValues ValidationDevice::GetSubmittedValues() const
{
    return _inner.GetSubmittedValues();
}

RhiQueueValues ValidationDevice::GetCompletedValues() const
{
    return _inner.GetCompletedValues();
}

RhiCommandList *ValidationDevice::BeginCommandList(RhiQueueType queue)
{
    RhiCommandList *inner = _inner.BeginCommandList(queue);
    if (inner == nullptr)
    {
        return nullptr;
    }
    _liveLists.FetchAdd(1);
    return new ValidationCommandList(*this, *inner);
}

RhiCommandList *ValidationDevice::Unwrap(RhiCommandList *list, RhiQueueType queue, const char *call)
{
    auto *validation = static_cast<ValidationCommandList *>(list);
    if (validation->GetQueueType() != queue)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0}: a command list for another queue type", call);
    }
    validation->PrepareSubmit(call);
    return &validation->GetInner();
}

void ValidationDevice::DiscardCommandList(RhiCommandList *list)
{
    RhiCommandList *inner = Unwrap(list, list->GetQueueType(), "DiscardCommandList");
    delete static_cast<ValidationCommandList *>(list);
    _liveLists.FetchSub(1);
    _inner.DiscardCommandList(inner);
}

Result<uint64, RhiError> ValidationDevice::Submit(RhiQueueType queue, const RhiSubmitDesc &desc)
{
    const SubmitScope submitting(*this, "Submit");
    List<RhiCommandList *> lists;
    for (RhiCommandList *list : desc.CommandLists)
    {
        if (lists.Contains(list))
        {
            MARLIN_RHI_VALIDATION_ERROR("Submit: the same command list twice");
        }
        lists.Add(list);
    }
    List<RhiCommandList *> inner;
    for (RhiCommandList *list : lists)
    {
        inner.Add(Unwrap(list, queue, "Submit"));
    }
    {
        const ScopeLock scope(_lock);
        for (const RhiFenceWait &wait : desc.Waits)
        {
            if (!_fences.Contains(HandleKey(wait.Fence)))
            {
                MARLIN_RHI_VALIDATION_ERROR("Submit: a wait on a stale fence");
            }
        }
        for (const RhiFenceSignal &signal : desc.Signals)
        {
            uint64 *signaled = _fences.Find(HandleKey(signal.Fence));
            if (signaled == nullptr)
            {
                MARLIN_RHI_VALIDATION_ERROR("Submit: a signal of a stale fence");
                continue;
            }
            if (signal.Value <= *signaled)
            {
                MARLIN_RHI_VALIDATION_ERROR("Submit: signal of value {0}; the fence already reaches {1}", signal.Value,
                                            *signaled);
            }
            *signaled = signal.Value;
        }
    }
    RhiSubmitDesc forwarded = desc;
    forwarded.CommandLists = inner;
    Result<uint64, RhiError> result = _inner.Submit(queue, forwarded);
    if (result.IsOk())
    {
        const ScopeLock scope(_lock);
        _fences.Set(HandleKey(_inner.GetQueueFence(queue)), result.Value());
    }
    for (RhiCommandList *list : lists)
    {
        delete static_cast<ValidationCommandList *>(list);
        _liveLists.FetchSub(1);
    }
    return result;
}

Result<void, RhiError> ValidationDevice::UpdateTileMappings(RhiQueueType queue, Span<const RhiTileMapping> mappings)
{
    const SubmitScope submitting(*this, "UpdateTileMappings");
    for (const RhiTileMapping &mapping : mappings)
    {
        const Optional<RhiTextureDesc> texture = FindTexture(mapping.Texture);
        const Optional<RhiBufferDesc> buffer = FindBuffer(mapping.Buffer);
        const bool reserved = (texture.HasValue() && HasAnyFlags(texture->Flags, RhiResourceFlags::Reserved)) ||
                              (buffer.HasValue() && HasAnyFlags(buffer->Flags, RhiResourceFlags::Reserved));
        if (!reserved)
        {
            MARLIN_RHI_VALIDATION_ERROR("UpdateTileMappings of a resource that is stale or not reserved");
        }
        if (mapping.Heap.IsValid() && !IsTracked(_lock, _heaps, mapping.Heap))
        {
            MARLIN_RHI_VALIDATION_ERROR("UpdateTileMappings into a stale heap");
        }
    }
    return _inner.UpdateTileMappings(queue, mappings);
}

void ValidationDevice::WaitIdle()
{
    _inner.WaitIdle();
}

void ValidationDevice::ProcessCompletions()
{
    const SubmitScope submitting(*this, "ProcessCompletions");
    _inner.ProcessCompletions();
}

Result<RhiSwapchain, RhiError> ValidationDevice::CreateSwapchain(const RhiSwapchainDesc &desc)
{
    if (desc.NativeWindow == nullptr || desc.ImageCount < 2 || desc.MaxFrameLatency == 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("Swapchain {0}: no window, fewer than two images or no frame latency",
                                    desc.DebugName);
    }
    Result<RhiSwapchain, RhiError> swapchain = _inner.CreateSwapchain(desc);
    if (swapchain.IsOk())
    {
        const ScopeLock scope(_lock);
        _swapchains.Add(HandleKey(swapchain.Value()), 0);
    }
    return swapchain;
}

void ValidationDevice::Destroy(RhiSwapchain swapchain)
{
    if (Remove(_lock, _swapchains, swapchain, "swapchain"))
    {
        {
            const ScopeLock scope(_lock);
            for (const auto &image : _swapchainImages)
            {
                _textures.Remove(image.Key);
            }
            _swapchainImages.Clear();
        }
        _inner.Destroy(swapchain);
    }
}

Result<void, RhiError> ValidationDevice::ResizeSwapchain(RhiSwapchain swapchain, uint32 width, uint32 height)
{
    if (!IsTracked(_lock, _swapchains, swapchain))
    {
        MARLIN_RHI_VALIDATION_ERROR("ResizeSwapchain of a stale swapchain");
    }
    {
        const ScopeLock scope(_lock);
        for (const auto &image : _swapchainImages)
        {
            _textures.Remove(image.Key);
        }
        _swapchainImages.Clear();
    }
    return _inner.ResizeSwapchain(swapchain, width, height);
}

Result<void, RhiError> ValidationDevice::SetPresentMode(RhiSwapchain swapchain, RhiPresentMode mode)
{
    {
        const ScopeLock scope(_lock);
        for (const auto &image : _swapchainImages)
        {
            _textures.Remove(image.Key);
        }
        _swapchainImages.Clear();
    }
    return _inner.SetPresentMode(swapchain, mode);
}

Span<const RhiColorSpace> ValidationDevice::GetSupportedColorSpaces(RhiSwapchain swapchain) const
{
    return _inner.GetSupportedColorSpaces(swapchain);
}

Result<void, RhiError> ValidationDevice::SetColorSpace(RhiSwapchain swapchain, RhiColorSpace colorSpace,
                                                       RhiFormat format)
{
    bool supported = false;
    for (const RhiColorSpace candidate : _inner.GetSupportedColorSpaces(swapchain))
    {
        supported = supported || candidate == colorSpace;
    }
    if (!supported)
    {
        MARLIN_RHI_VALIDATION_ERROR("SetColorSpace to a color space the swapchain does not support");
    }
    {
        const ScopeLock scope(_lock);
        for (const auto &image : _swapchainImages)
        {
            _textures.Remove(image.Key);
        }
        _swapchainImages.Clear();
    }
    return _inner.SetColorSpace(swapchain, colorSpace, format);
}

Result<void, RhiError> ValidationDevice::SetHdrMetadata(RhiSwapchain swapchain, const RhiHdrMetadata &metadata)
{
    if (!IsTracked(_lock, _swapchains, swapchain))
    {
        MARLIN_RHI_VALIDATION_ERROR("SetHdrMetadata of a stale swapchain");
    }
    if (metadata.MinLuminance >= metadata.MaxLuminance ||
        metadata.MaxFrameAverageLightLevel > metadata.MaxContentLightLevel)
    {
        MARLIN_RHI_VALIDATION_ERROR("SetHdrMetadata: minimum luminance above the maximum, or frame average above the "
                                    "content maximum");
    }
    return _inner.SetHdrMetadata(swapchain, metadata);
}

void ValidationDevice::WaitForPresent(RhiSwapchain swapchain)
{
    const SubmitScope submitting(*this, "WaitForPresent");
    _inner.WaitForPresent(swapchain);
}

Result<RhiTexture, RhiError> ValidationDevice::AcquireNextImage(RhiSwapchain swapchain)
{
    const SubmitScope submitting(*this, "AcquireNextImage");
    if (!IsTracked(_lock, _swapchains, swapchain))
    {
        MARLIN_RHI_VALIDATION_ERROR("AcquireNextImage of a stale swapchain");
    }
    Result<RhiTexture, RhiError> image = _inner.AcquireNextImage(swapchain);
    if (image.IsOk())
    {
        const uint64 key = HandleKey(image.Value());
        const ScopeLock scope(_lock);
        if (_swapchainImages.TryAdd(key, 0))
        {
            RhiTextureDesc desc = _inner.GetTextureDesc(image.Value());
            desc.DebugName = {};
            desc.ViewFormats = {};
            _textures.Set(key, desc);
        }
    }
    return image;
}

Result<void, RhiError> ValidationDevice::Present(RhiSwapchain swapchain, RhiQueueType queue)
{
    const SubmitScope submitting(*this, "Present");
    if (!IsTracked(_lock, _swapchains, swapchain))
    {
        MARLIN_RHI_VALIDATION_ERROR("Present of a stale swapchain");
    }
    return _inner.Present(swapchain, queue);
}

void ValidationDevice::WriteCrashReport(StringWriter &output) const
{
    _inner.WriteCrashReport(output);
}

void ValidationDevice::SetDebugName(RhiBuffer buffer, StringView name)
{
    if (!FindBuffer(buffer).HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("SetDebugName of a stale buffer");
    }
    _inner.SetDebugName(buffer, name);
}

void ValidationDevice::SetDebugName(RhiTexture texture, StringView name)
{
    if (!FindTexture(texture).HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("SetDebugName of a stale texture");
    }
    _inner.SetDebugName(texture, name);
}

Optional<RhiBufferDesc> ValidationDevice::FindBuffer(RhiBuffer buffer) const
{
    const SharedScopeLock scope(_lock);
    const RhiBufferDesc *desc = _buffers.Find(HandleKey(buffer));
    return desc != nullptr ? Optional<RhiBufferDesc>(*desc) : Optional<RhiBufferDesc>();
}

Optional<RhiTextureDesc> ValidationDevice::FindTexture(RhiTexture texture) const
{
    const SharedScopeLock scope(_lock);
    const RhiTextureDesc *desc = _textures.Find(HandleKey(texture));
    return desc != nullptr ? Optional<RhiTextureDesc>(*desc) : Optional<RhiTextureDesc>();
}

bool ValidationDevice::IsLive(RhiPipeline pipeline) const
{
    return IsTracked(_lock, _pipelines, pipeline);
}

bool ValidationDevice::IsLive(RhiAccelerationStructure structure) const
{
    return IsTracked(_lock, _accelerationStructures, structure);
}

Optional<RhiQueryPoolDesc> ValidationDevice::FindQueryPool(RhiQueryPool pool) const
{
    const SharedScopeLock scope(_lock);
    const RhiQueryPoolDesc *desc = _queryPools.Find(HandleKey(pool));
    return desc != nullptr ? Optional<RhiQueryPoolDesc>(*desc) : Optional<RhiQueryPoolDesc>();
}

Optional<RhiIndirectCommandKind> ValidationDevice::FindCommandSignature(RhiCommandSignature signature) const
{
    const SharedScopeLock scope(_lock);
    const RhiIndirectCommandKind *kind = _commandSignatures.Find(HandleKey(signature));
    return kind != nullptr ? Optional<RhiIndirectCommandKind>(*kind) : Optional<RhiIndirectCommandKind>();
}

Optional<RhiPipelineKind> ValidationDevice::FindPipelineKind(RhiPipeline pipeline) const
{
    const SharedScopeLock scope(_lock);
    const RhiPipelineKind *kind = _pipelines.Find(HandleKey(pipeline));
    return kind != nullptr ? Optional<RhiPipelineKind>(*kind) : Optional<RhiPipelineKind>();
}
} // namespace Marlin
