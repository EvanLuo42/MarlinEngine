#include "Renderer/RendererSubsystem.h"

#include "Application/PlatformWindow.h"
#include "Concurrency/ScopeLock.h"
#include "Diagnostics/AssertFormat.h"
#include "Diagnostics/Log.h"
#include "RHI/RhiSubsystem.h"
#include "Reflection/Reflection.h"
#include "RenderGraph/RenderGraphSubsystem.h"

MARLIN_DEFINE_LOG_CATEGORY(LogRenderer);

namespace Marlin
{
bool RendererSubsystem::Hdr = true;
float32 RendererSubsystem::UiWhiteNits = 0;

namespace
{
constexpr StringView PresentModeName(RhiPresentMode mode)
{
    return mode == RhiPresentMode::Immediate ? "immediate" : mode == RhiPresentMode::Mailbox ? "mailbox" : "fifo";
}

bool IsFrameStage(const TypeInfo &type)
{
    return type.GetKind() == TypeKind::Struct && type.GetMetadata().Has("RenderFrameStage");
}

void ParseNames(Optional<StringView> value, List<StringView> &names)
{
    if (!value.HasValue())
    {
        return;
    }
    StringView text = value.Value().Trim();
    if (text.StartsWith('(') && text.EndsWith(')'))
    {
        text = text.Substring(1, text.Length() - 2);
    }
    for (const StringView part : text.Split(','))
    {
        const StringView name = part.Trim();
        if (!name.IsEmpty())
        {
            names.Add(name);
        }
    }
}

const TypeInfo &ResolveStage(const TypeInfo &owner, StringView key, StringView name)
{
    const TypeInfo *type = Reflection::FindType(name);
    if (type == nullptr || !IsFrameStage(*type))
    {
        MARLIN_FATALF("{0}: {1}={2} does not name a render frame stage", owner.GetName(), key, name);
    }
    return *type;
}
} // namespace

const RenderFrameSurface *RenderFrame::FindSurface(const RenderSurface &surface) const
{
    for (const RenderFrameSurface &entry : _surfaces)
    {
        if (entry.Surface == &surface)
        {
            return &entry;
        }
    }
    return nullptr;
}

void RenderFrame::BindImage(uint32 slot, const RenderFrameImage &image)
{
    _images.FindOrAdd(slot) = image;
}

const RenderFrameImage *RenderFrame::FindImage(uint32 slot) const
{
    return _images.Find(slot);
}

RendererSubsystem::RendererSubsystem() = default;
RendererSubsystem::~RendererSubsystem() = default;

void RendererSubsystem::Initialize(SubsystemContext &context)
{
    _rhi = context.Collection->Get<RhiSubsystem>();
    _graphs = context.Collection->Get<RenderGraphSubsystem>();
    _device = _graphs != nullptr ? _graphs->GetDevice() : nullptr;
    _features.Build();
    DiscoverStages();
    MARLIN_LOG(LogRenderer, Debug, "Renderer: {0} frame stages, {1} features", _stages.Count(), _features.Count());
    if (_rhi != nullptr && _device != nullptr)
    {
        _rhi->ClaimFrameSubmission();
    }
}

void RendererSubsystem::DiscoverStages()
{
    List<const TypeInfo *> stages;
    Reflection::ForEachType([&](const TypeInfo &type) {
        if (IsFrameStage(type))
        {
            stages.Add(&type);
        }
    });
    stages.Sort([](const TypeInfo *a, const TypeInfo *b) { return a->GetName().CompareTo(b->GetName()) < 0; });
    List<List<uint32>> successors;
    List<uint32> predecessors;
    successors.Resize(stages.Count());
    predecessors.Resize(stages.Count());
    const auto indexOf = [&](const TypeInfo &owner, StringView key, StringView name) {
        return static_cast<uint32>(stages.IndexOf(&ResolveStage(owner, key, name)));
    };
    for (uint32 index = 0; index < stages.Count(); ++index)
    {
        List<StringView> names;
        ParseNames(stages[index]->GetMetadata().Get("After"), names);
        for (const StringView name : names)
        {
            successors[indexOf(*stages[index], "After", name)].Add(index);
            ++predecessors[index];
        }
        names.Clear();
        ParseNames(stages[index]->GetMetadata().Get("Before"), names);
        for (const StringView name : names)
        {
            const uint32 next = indexOf(*stages[index], "Before", name);
            successors[index].Add(next);
            ++predecessors[next];
        }
    }
    List<uint32> ready;
    for (uint32 index = 0; index < stages.Count(); ++index)
    {
        if (predecessors[index] == 0)
        {
            ready.Add(index);
        }
    }
    while (!ready.IsEmpty())
    {
        const uint32 index = ready[0];
        ready.RemoveAt(0);
        _stages.AddDefaulted().Type = stages[index];
        for (const uint32 next : successors[index])
        {
            if (--predecessors[next] == 0)
            {
                ready.Add(next);
            }
        }
    }
    if (_stages.Count() != stages.Count())
    {
        MARLIN_FATAL("The render frame stages' After and Before form a cycle");
    }
    for (RenderFrameFeature *feature : _features)
    {
        const TypeInfo &type = _features.GetType(*feature);
        const Optional<StringView> stageName = type.GetMetadata().Get("Stage");
        if (!stageName.HasValue())
        {
            MARLIN_FATALF("{0} names no Stage", type.GetName());
        }
        const TypeInfo &stage = ResolveStage(type, "Stage", stageName.Value().Trim());
        for (Stage &entry : _stages)
        {
            if (entry.Type == &stage)
            {
                entry.Features.Add(feature);
            }
        }
    }
}

void RendererSubsystem::Deinitialize()
{
    WaitForRender();
    if (_device != nullptr)
    {
        _device->WaitIdle();
    }
    for (UniquePtr<RenderSurface> &surface : _surfaces)
    {
        if (surface->_swapchain.IsValid())
        {
            _rhi->DestroySwapchain(surface->_swapchain);
        }
    }
    _surfaces.Clear();
    _stages.Clear();
}

RenderSurface *RendererSubsystem::CreateSurface(const RenderSurfaceDesc &desc)
{
    if (_device == nullptr || _rhi == nullptr)
    {
        MARLIN_LOG(LogRenderer, Error, "No RHI device; the window cannot have a surface");
        return nullptr;
    }
    if (!desc.Window.IsValid())
    {
        MARLIN_LOG(LogRenderer, Warning, "Cannot create a surface for an invalid window");
        return nullptr;
    }
    UniquePtr<RenderSurface> surface = MakeUnique<RenderSurface>();
    surface->_desc = desc;
    if (!CreateSwapchain(*surface))
    {
        return nullptr;
    }
    _surfaces.Add(Move(surface));
    return _surfaces.Last().Get();
}

void RendererSubsystem::DestroySurface(RenderSurface *surface)
{
    WaitForRender();
    for (usize index = 0; index < _surfaces.Count(); ++index)
    {
        if (_surfaces[index].Get() == surface)
        {
            if (surface->_swapchain.IsValid())
            {
                _device->WaitIdle();
                _rhi->DestroySwapchain(surface->_swapchain);
            }
            _surfaces.RemoveAt(index);
            return;
        }
    }
}

bool RendererSubsystem::CreateSwapchain(RenderSurface &surface)
{
    const PlatformRect rect = PlatformWindow::GetClientRect(surface._desc.Window);
    surface._width = static_cast<uint32>(rect.Width > 0 ? rect.Width : 0);
    surface._height = static_cast<uint32>(rect.Height > 0 ? rect.Height : 0);
    surface._dpiScale = PlatformWindow::GetDpiScale(surface._desc.Window);
    RhiSwapchainDesc swapchain;
    swapchain.NativeWindow = PlatformWindow::GetNativeHandle(surface._desc.Window);
    swapchain.Width = surface._width > 0 ? surface._width : 1;
    swapchain.Height = surface._height > 0 ? surface._height : 1;
    swapchain.Format = RhiFormat::RGB10A2Unorm;
    swapchain.ColorSpace = RhiColorSpace::Srgb;
    swapchain.PresentMode = surface._desc.PresentMode;
    swapchain.Usage = RhiTextureUsage::UnorderedAccess | RhiTextureUsage::RenderTarget | RhiTextureUsage::CopyDest;
    swapchain.DebugName = "Window";
    Result<RhiSwapchain, RhiError> created = _rhi->CreateSwapchain(swapchain);
    if (created.IsError())
    {
        MARLIN_LOG(LogRenderer, Error, "The window's swapchain could not be created: {0}",
                   GetErrorName(created.Error()));
        return false;
    }
    surface._swapchain = created.Value();
    surface._format = RhiFormat::RGB10A2Unorm;
    surface._colorSpace = RhiColorSpace::Srgb;
    surface._lost = false;
    const DisplayColorInfo display = PlatformWindow::GetDisplayColor(surface._desc.Window);
    surface._displayHdr = display.Hdr;
    surface._uiWhiteNits = UiWhiteNits > 0 ? UiWhiteNits : display.SdrWhiteNits;
    const bool wantsHdr = Hdr && surface._desc.AllowHdr && display.Hdr;
    if (wantsHdr)
    {
        for (const RhiColorSpace space : _device->GetSupportedColorSpaces(surface._swapchain))
        {
            if (space == RhiColorSpace::Hdr10 &&
                _device->SetColorSpace(surface._swapchain, RhiColorSpace::Hdr10, RhiFormat::RGB10A2Unorm).IsOk())
            {
                surface._format = RhiFormat::RGB10A2Unorm;
                surface._colorSpace = RhiColorSpace::Hdr10;
                break;
            }
        }
        if (!surface.IsHdr())
        {
            MARLIN_LOG(LogRenderer, Warning,
                       "The display is in HDR but the surface cannot present HDR10; presenting SDR at {0} nits",
                       surface._uiWhiteNits);
        }
    }
    MARLIN_LOG(LogRenderer, Info, "Surface {0}x{1} at {2:F2} DPI, {3}, {4}", surface._width, surface._height,
               surface._dpiScale, surface.IsHdr() ? "HDR10" : "SDR", PresentModeName(surface._desc.PresentMode));
    return true;
}

void RendererSubsystem::UpdateSurface(RenderSurface &surface)
{
    surface._minimized = PlatformWindow::IsMinimized(surface._desc.Window);
    if (surface._minimized)
    {
        return;
    }
    const PlatformRect rect = PlatformWindow::GetClientRect(surface._desc.Window);
    const uint32 width = static_cast<uint32>(rect.Width > 0 ? rect.Width : 0);
    const uint32 height = static_cast<uint32>(rect.Height > 0 ? rect.Height : 0);
    surface._dpiScale = PlatformWindow::GetDpiScale(surface._desc.Window);
    if (++surface._colorCheck >= 60)
    {
        surface._colorCheck = 0;
        const DisplayColorInfo display = PlatformWindow::GetDisplayColor(surface._desc.Window);
        surface._lost = surface._lost || display.Hdr != surface._displayHdr;
        surface._uiWhiteNits = UiWhiteNits > 0 ? UiWhiteNits : display.SdrWhiteNits;
    }
    if (surface._lost)
    {
        _device->WaitIdle();
        _rhi->DestroySwapchain(surface._swapchain);
        surface._swapchain = {};
        (void)CreateSwapchain(surface);
        return;
    }
    if ((width != surface._width || height != surface._height) && width != 0 && height != 0)
    {
        // ResizeSwapchain waits for the device to be idle itself.
        const Result<void, RhiError> resized = _device->ResizeSwapchain(surface._swapchain, width, height);
        if (resized.IsError())
        {
            MARLIN_LOG(LogRenderer, Warning, "Surface could not resize from {0}x{1} to {2}x{3}: {4}", surface._width,
                       surface._height, width, height, GetErrorName(resized.Error()));
            surface._lost = true;
            return;
        }
    }
    surface._width = width;
    surface._height = height;
}

uint32 RendererSubsystem::AllocateImageSlot()
{
    const ScopeLock lock(_slotLock);
    if (!_freeSlots.IsEmpty())
    {
        return _freeSlots.Pop();
    }
    return _nextSlot++;
}

void RendererSubsystem::FreeImageSlot(uint32 slot)
{
    const ScopeLock lock(_slotLock);
    _freeSlots.Add(slot);
}

void RendererSubsystem::WaitForRender()
{
    Jobs::Wait(_job);
}

void RendererSubsystem::EndFrame(const FrameTime &time)
{
    if (_device == nullptr)
    {
        return;
    }
    WaitForRender();
    for (UniquePtr<RenderSurface> &surface : _surfaces)
    {
        UpdateSurface(*surface);
    }
    RenderFrameExtract extract(time.Frame);
    for (Stage &stage : _stages)
    {
        for (RenderFrameFeature *feature : stage.Features)
        {
            feature->Extract(extract);
        }
    }
    _renderFrame = time.Frame;
    JobDecl job;
    job.Entry = &RenderJob;
    job.Argument = this;
    job.Priority = JobPriority::High;
    job.LargeStack = true;
    Jobs::Run(job, &_job);
}

void RendererSubsystem::RenderJob(void *argument)
{
    static_cast<RendererSubsystem *>(argument)->Render();
}

void RendererSubsystem::Render()
{
    _rhi->BeginSubmitFrame(_renderFrame);
    RenderGraphBuilder graph(*_graphs);
    RenderFrame frame(graph, _renderFrame);
    for (UniquePtr<RenderSurface> &surface : _surfaces)
    {
        if (!surface->IsPresentable())
        {
            continue;
        }
        _rhi->WaitForFrame(surface->_swapchain, _renderFrame);
        Result<RhiTexture, RhiError> image = _device->AcquireNextImage(surface->_swapchain);
        if (image.IsError())
        {
            surface->_lost = image.Error() == RhiError::SurfaceLost;
            if (!surface->_lost && !surface->_reportedFailure)
            {
                MARLIN_LOG(LogRenderer, Warning, "Frame {0}: no swapchain image for the window: {1}", frame.Index,
                           GetErrorName(image.Error()));
                surface->_reportedFailure = true;
            }
            continue;
        }
        surface->_image.Texture = image.Value();
        surface->_image.Layout = RhiLayout::Undefined;
        surface->_image.FinalLayout = RhiLayout::Present;
        RenderFrameSurface &entry = frame._surfaces.AddDefaulted();
        entry.Surface = surface.Get();
        entry.Target = graph.ImportTexture("Surface", surface->_image);
    }
    for (Stage &stage : _stages)
    {
        for (RenderFrameFeature *feature : stage.Features)
        {
            feature->AddPasses(frame);
        }
    }
    const Result<void, RhiError> executed = graph.Execute();
    if (executed.IsError())
    {
        MARLIN_LOG(LogRenderer, Error, "Frame {0} did not render: {1}", frame.Index, GetErrorName(executed.Error()));
    }
    for (const RenderFrameSurface &entry : frame.GetSurfaces())
    {
        const Result<void, RhiError> presented = _rhi->Present(entry.Surface->_swapchain, frame.Index);
        if (presented.IsError())
        {
            entry.Surface->_lost = presented.Error() == RhiError::SurfaceLost;
            if (!entry.Surface->_lost && !entry.Surface->_reportedFailure)
            {
                MARLIN_LOG(LogRenderer, Warning, "Frame {0}: presenting the window failed: {1}", frame.Index,
                           GetErrorName(presented.Error()));
                entry.Surface->_reportedFailure = true;
            }
        }
        else
        {
            entry.Surface->_reportedFailure = false;
            PlatformWindow::NotifyPresented(entry.Surface->_desc.Window, static_cast<int32>(entry.Surface->_width),
                                            static_cast<int32>(entry.Surface->_height));
        }
    }
}
} // namespace Marlin
