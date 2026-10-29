#include "UIPainterRenderer/UIPainterSubsystem.h"

#include "Application/ApplicationSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/StartupProgress.h"
#include "Memory/MemoryOps.h"
#include "RHI/RhiDevice.h"
#include "RenderGraph/RenderGraphBuilder.h"
#include "Renderer/RendererSubsystem.h"
#include "Shader/ShaderLibrary.h"
#include "Templates/Bit.h"
#include "UIText/UITextSubsystem.h"

namespace Marlin
{
bool UIPainterSubsystem::MainWindow = true;

namespace
{
constexpr uint32 TileSize = 16;
constexpr uint32 EncodeLinear = 0;
constexpr uint32 EncodeSrgb = 1;
constexpr uint32 EncodePq = 2;

template <typename T> Span<const uint8> AsBytes(Span<const T> values)
{
    return {reinterpret_cast<const uint8 *>(values.Data()), values.Count() * sizeof(T)};
}

/// A copy from upload memory into a graph buffer, or a zero fill when there is nothing to copy.
struct UploadCopy
{
    RenderGraphBuffer Target;
    RhiBuffer Source;
    uint64 SourceOffset = 0;
    uint64 TargetOffset = 0;
    uint64 Size = 0;
};

struct UploadData
{
    List<UploadCopy> Copies;
};

/// What a raster pass's image table holds at one index.
struct ImageEntry
{
    enum class Kind : uint8
    {
        Descriptor,
        Graph,
        CanvasOutput,
        CanvasBackdrop,
    };

    Kind Type = Kind::Descriptor;
    uint32 Value = 0;
    RenderGraphTexture Texture;
};

} // namespace

struct UIPainterSubsystem::FrameState
{
    RenderGraphBuffer GlyphCurves;
    RenderGraphBuffer GlyphWords;
    RenderGraphTexture Atlas;
    List<UploadCopy> Copies;
    RhiUploadAllocator *Uploads = nullptr;

    RenderGraphBuffer Upload(RenderGraphBuilder &graph, StringView name, Span<const uint8> bytes)
    {
        RhiBufferDesc desc;
        desc.Size = Max<uint64>((bytes.Count() + 15) & ~uint64{15}, 16);
        desc.DebugName = name;
        const RenderGraphBuffer buffer = graph.CreateBuffer(name, desc);
        UploadCopy &copy = Copies.AddDefaulted();
        copy.Target = buffer;
        copy.Size = desc.Size;
        if (!bytes.IsEmpty())
        {
            const RhiUploadAllocation allocation = Uploads->Allocate(desc.Size);
            if (allocation.Data != nullptr)
            {
                MemCopy(allocation.Data, bytes.Data(), bytes.Count());
                copy.Source = allocation.Buffer;
                copy.SourceOffset = allocation.Offset;
            }
        }
        return buffer;
    }

    void AddUploadPass(RenderGraphBuilder &graph)
    {
        if (Copies.IsEmpty())
        {
            return;
        }
        graph.AddPass<UploadData>(
            "UI Upload", RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass, UploadData &data) {
                for (const UploadCopy &copy : Copies)
                {
                    bool seen = false;
                    for (const UploadCopy &earlier : data.Copies)
                    {
                        seen = seen || earlier.Target == copy.Target;
                    }
                    if (!seen)
                    {
                        (void)pass.Buffer(copy.Target, RhiAccess::CopyDest);
                    }
                    data.Copies.Add(copy);
                }
            },
            [](const UploadData &data, RenderGraphContext &context) {
                for (const UploadCopy &copy : data.Copies)
                {
                    const RhiBuffer target = context.GetBuffer(copy.Target);
                    if (copy.Source.IsValid())
                    {
                        context.List.CopyBuffer(target, copy.TargetOffset, copy.Source, copy.SourceOffset, copy.Size);
                    }
                    else
                    {
                        context.List.FillBuffer(target, copy.TargetOffset, copy.Size, 0);
                    }
                }
            });
        Copies.Clear();
    }
};

UIPainterSubsystem::UIPainterSubsystem() = default;
UIPainterSubsystem::~UIPainterSubsystem() = default;

void UIPainterSubsystem::Initialize(SubsystemContext &context)
{
    _renderer = context.Collection->Get<RendererSubsystem>();
    _shaders = context.Collection->Get<ShaderLibrary>();
    _text = context.Collection->Get<UITextSubsystem>();
    _device = _renderer != nullptr ? _renderer->GetDevice() : nullptr;
    if (_device == nullptr || _shaders == nullptr || _shaders->GetProfile() == nullptr)
    {
        return;
    }
    const ShaderDeclaration *shaders[] = {&UiCountShader,         &UiScanShader,        &UiScatterShader,
                                          &UiRasterSurfaceShader, &UiRasterLayerShader, &UiGlyphAtlasShader,
                                          &UiBlurShader};
    for (const ShaderDeclaration *shader : shaders)
    {
        _shaders->Preload(*shader);
    }
    _readyCheck = StartupProgress::AddReadyCheck("UI shaders", [library = _shaders] {
        const ShaderDeclaration *needed[] = {&UiCountShader,         &UiScanShader,        &UiScatterShader,
                                             &UiRasterSurfaceShader, &UiRasterLayerShader, &UiGlyphAtlasShader,
                                             &UiBlurShader};
        bool ready = true;
        for (const ShaderDeclaration *shader : needed)
        {
            ready = library->IsReady(*shader) && ready;
        }
        return ready;
    });
    _uploads = MakeUnique<RhiUploadAllocator>(*_device);

    RhiTextureDesc atlas;
    atlas.Format = RhiFormat::R8Unorm;
    atlas.Width = UIGlyphCache::AtlasSize;
    atlas.Height = UIGlyphCache::AtlasSize;
    atlas.Usage = RhiTextureUsage::ShaderResource | RhiTextureUsage::UnorderedAccess | RhiTextureUsage::CopyDest;
    atlas.DebugName = "UI Glyph Atlas";
    Result<RhiTexture, RhiError> created = _device->CreateTexture(atlas);
    if (created.IsOk())
    {
        _atlas = created.Value();
        _atlasState.Texture = _atlas;
    }
    RhiSamplerDesc linear;
    linear.AddressU = RhiAddressMode::Clamp;
    linear.AddressV = RhiAddressMode::Clamp;
    linear.AddressW = RhiAddressMode::Clamp;
    linear.MipFilter = RhiFilter::Nearest;
    if (Result<RhiSampler, RhiError> sampler = _device->CreateSampler(linear); sampler.IsOk())
    {
        _linearSampler = sampler.Value();
    }
    RhiSamplerDesc point = linear;
    point.MinFilter = RhiFilter::Nearest;
    point.MagFilter = RhiFilter::Nearest;
    if (Result<RhiSampler, RhiError> sampler = _device->CreateSampler(point); sampler.IsOk())
    {
        _pointSampler = sampler.Value();
    }

    const ApplicationSubsystem *application = context.Collection->Get<ApplicationSubsystem>();
    if (MainWindow && application != nullptr && application->GetMainWindow().IsValid())
    {
        _mainCanvas = CreateWindowCanvas(application->GetMainWindow());
    }
}

void UIPainterSubsystem::Deinitialize()
{
    StartupProgress::RemoveReadyCheck(_readyCheck);
    if (_renderer != nullptr)
    {
        _renderer->WaitForRender();
    }
    while (!_canvases.IsEmpty())
    {
        DestroyCanvas(_canvases.Last().Get());
    }
    _mainCanvas = nullptr;
    if (_device != nullptr)
    {
        _device->WaitIdle();
        if (_atlas.IsValid())
        {
            _device->Destroy(_atlas);
        }
        if (_glyphCurves.IsValid())
        {
            _device->Destroy(_glyphCurves);
        }
        if (_glyphWords.IsValid())
        {
            _device->Destroy(_glyphWords);
        }
    }
    _atlas = {};
    _glyphCurves = {};
    _glyphWords = {};
    _uploads.Reset();
}

UIWindowCanvas *UIPainterSubsystem::CreateCanvas(RenderSurface &surface)
{
    UniquePtr<UIWindowCanvas> canvas = MakeUnique<UIWindowCanvas>();
    canvas->_surface = &surface;
    _canvases.Add(Move(canvas));
    return _canvases.Last().Get();
}

UIWindowCanvas *UIPainterSubsystem::CreateWindowCanvas(WindowHandle window)
{
    RenderSurfaceDesc desc;
    desc.Window = window;
    RenderSurface *surface = _renderer != nullptr ? _renderer->CreateSurface(desc) : nullptr;
    if (surface == nullptr)
    {
        return nullptr;
    }
    UIWindowCanvas *canvas = CreateCanvas(*surface);
    canvas->_ownsSurface = true;
    return canvas;
}

void UIPainterSubsystem::DestroyCanvas(UIWindowCanvas *canvas)
{
    _renderer->WaitForRender();
    for (usize index = 0; index < _canvases.Count(); ++index)
    {
        if (_canvases[index].Get() == canvas)
        {
            if (canvas->_ownsSurface)
            {
                _renderer->DestroySurface(canvas->_surface);
            }
            if (_mainCanvas == canvas)
            {
                _mainCanvas = nullptr;
            }
            _canvases.RemoveAt(index);
            return;
        }
    }
}

void UIPainterSubsystem::BeginFrame(const FrameTime &time)
{
    for (UniquePtr<UIWindowCanvas> &canvas : _canvases)
    {
        const RenderSurface &surface = canvas->GetSurface();
        canvas->_painter.Begin(canvas->_lists[canvas->_recording], surface.GetWidth(), surface.GetHeight(),
                               surface.GetDpiScale());
        canvas->_overlay.Begin(canvas->_overlayList, surface.GetWidth(), surface.GetHeight(), surface.GetDpiScale());
    }
}

void UIPainterSubsystem::Extract()
{
    for (UniquePtr<UIWindowCanvas> &canvas : _canvases)
    {
        canvas->_rendering = nullptr;
        if (!canvas->_painter.IsRecording())
        {
            continue;
        }
        canvas->_painter.End();
        canvas->_overlay.End();
        UICommandList &list = canvas->_lists[canvas->_recording];
        list.Append(canvas->_overlayList);
        canvas->_recording ^= 1;
        if (_text != nullptr)
        {
            _glyphs.Resolve(list, *_text);
        }
        canvas->_rendering = &list;
    }
    List<UiAtlasGlyph> glyphs;
    bool clear = false;
    _glyphs.TakeUploads(glyphs, clear);
    if (clear)
    {
        _atlasGlyphs.Clear();
        _clearAtlas = true;
    }
    _atlasGlyphs.AddRange(glyphs.AsSpan());
    // RECOVERY-DECISION D1: the render job of the frame before has finished (Renderer waits for it before Extract).
    _stats = _renderStats;
}

void UIPainterSubsystem::EnsureGlyphBuffers(uint64 curveBytes, uint64 wordBytes, bool &reupload)
{
    const auto grow = [&](RhiBuffer &buffer, uint64 &capacity, RenderGraphExternalBuffer &state, uint64 needed,
                          StringView name) {
        if (needed <= capacity && buffer.IsValid())
        {
            return;
        }
        uint64 size = Max<uint64>(capacity * 2, 64 * 1024);
        while (size < needed)
        {
            size *= 2;
        }
        if (buffer.IsValid())
        {
            _device->Destroy(buffer);
        }
        RhiBufferDesc desc;
        desc.Size = size;
        desc.Usage = RhiBufferUsage::ShaderResource | RhiBufferUsage::CopyDest;
        desc.DebugName = name;
        Result<RhiBuffer, RhiError> created = _device->CreateBuffer(desc);
        buffer = created.IsOk() ? created.Value() : RhiBuffer();
        capacity = created.IsOk() ? size : 0;
        state.Buffer = buffer;
        reupload = true;
    };
    grow(_glyphCurves, _glyphCurveCapacity, _glyphCurveState, curveBytes, "UI Glyph Curves");
    grow(_glyphWords, _glyphWordCapacity, _glyphWordState, wordBytes, "UI Glyph Words");
}

void UIPainterSubsystem::AddPasses(RenderFrame &frame)
{
    if (_device == nullptr || !_atlas.IsValid() || _uploads == nullptr)
    {
        return;
    }
    _uploads->EndFrame();
    _renderStats = {};
    RenderGraphBuilder &graph = frame.Graph;
    const ShaderDeclaration *shaders[] = {&UiCountShader,         &UiScanShader,        &UiScatterShader,
                                          &UiRasterSurfaceShader, &UiRasterLayerShader, &UiGlyphAtlasShader,
                                          &UiBlurShader};
    bool ready = true;
    for (const ShaderDeclaration *shader : shaders)
    {
        ready = _shaders->IsReady(*shader) && ready;
    }
    if (!ready)
    {
        for (UniquePtr<UIWindowCanvas> &canvas : _canvases)
        {
            const RenderFrameSurface *surface = frame.FindSurface(canvas->GetSurface());
            if (surface == nullptr)
            {
                continue;
            }
            const RenderGraphTexture target = surface->Target;
            LinearColor clear = canvas->ClearColor;
            graph.AddPass(
                "UI Clear", RhiQueueType::Graphics,
                [target](RenderGraphPassBuilder &pass) { (void)pass.Texture(target, RhiAccess::CopyDest); },
                [target, clear](RenderGraphContext &context) {
                    RhiClearValue value;
                    value.Color[0] = clear.R;
                    value.Color[1] = clear.G;
                    value.Color[2] = clear.B;
                    value.Color[3] = 1;
                    context.List.ClearTexture(context.GetTexture(target), {0, 1, 0, 1}, value);
                });
        }
        return;
    }

    FrameState state;
    state.Uploads = _uploads.Get();
    const Span<const UIGpuCurve> glyphCurves = _glyphs.GetCurves();
    const Span<const uint32> glyphWords = _glyphs.GetWords();
    bool reupload = false;
    EnsureGlyphBuffers(Max<uint64>(glyphCurves.Count() * sizeof(UIGpuCurve), 16),
                       Max<uint64>(glyphWords.Count() * sizeof(uint32), 16), reupload);
    state.GlyphCurves = graph.ImportBuffer("UI Glyph Curves", _glyphCurveState);
    state.GlyphWords = graph.ImportBuffer("UI Glyph Words", _glyphWordState);
    const uint32 firstCurve = reupload ? 0 : _uploadedGlyphCurves;
    const uint32 firstWord = reupload ? 0 : _uploadedGlyphWords;
    _uploadedGlyphCurves = static_cast<uint32>(glyphCurves.Count());
    _uploadedGlyphWords = static_cast<uint32>(glyphWords.Count());
    const auto uploadInto = [&](RenderGraphBuffer target, Span<const uint8> bytes, uint64 offset) {
        if (bytes.IsEmpty())
        {
            return;
        }
        const RhiUploadAllocation allocation = _uploads->Allocate(bytes.Count());
        if (allocation.Data == nullptr)
        {
            return;
        }
        MemCopy(allocation.Data, bytes.Data(), bytes.Count());
        UploadCopy &copy = state.Copies.AddDefaulted();
        copy.Target = target;
        copy.Source = allocation.Buffer;
        copy.SourceOffset = allocation.Offset;
        copy.TargetOffset = offset;
        copy.Size = bytes.Count();
    };
    uploadInto(state.GlyphCurves, AsBytes(glyphCurves.Slice(firstCurve, glyphCurves.Count() - firstCurve)),
               firstCurve * sizeof(UIGpuCurve));
    uploadInto(state.GlyphWords, AsBytes(glyphWords.Slice(firstWord, glyphWords.Count() - firstWord)),
               firstWord * sizeof(uint32));
    if (reupload && glyphCurves.IsEmpty())
    {
        UploadCopy &copy = state.Copies.AddDefaulted();
        copy.Target = state.GlyphCurves;
        copy.Size = 16;
    }
    if (reupload && glyphWords.IsEmpty())
    {
        UploadCopy &copy = state.Copies.AddDefaulted();
        copy.Target = state.GlyphWords;
        copy.Size = 16;
    }
    state.Atlas = graph.ImportTexture("UI Glyph Atlas", _atlasState);
    const RenderGraphBuffer atlasJobs = _atlasGlyphs.IsEmpty()
                                            ? RenderGraphBuffer()
                                            : state.Upload(graph, "UI Atlas Glyphs", AsBytes(_atlasGlyphs.AsSpan()));

    struct CanvasWork
    {
        UIWindowCanvas *Canvas = nullptr;
        RenderGraphTexture Target;
    };
    List<CanvasWork> work;
    for (UniquePtr<UIWindowCanvas> &canvas : _canvases)
    {
        const RenderFrameSurface *surface = frame.FindSurface(canvas->GetSurface());
        if (surface != nullptr && canvas->_rendering != nullptr)
        {
            work.Add({canvas.Get(), surface->Target});
        }
    }
    state.AddUploadPass(graph);

    if (_clearAtlas)
    {
        const RenderGraphTexture atlas = state.Atlas;
        graph.AddPass(
            "UI Atlas Clear", RhiQueueType::Graphics,
            [atlas](RenderGraphPassBuilder &pass) { (void)pass.Texture(atlas, RhiAccess::CopyDest); },
            [atlas](RenderGraphContext &context) {
                context.List.ClearTexture(context.GetTexture(atlas), {0, 1, 0, 1}, RhiClearValue{});
            });
        _clearAtlas = false;
    }
    if (!_atlasGlyphs.IsEmpty())
    {
        struct AtlasData
        {
            RenderGraphView Glyphs;
            RenderGraphView Curves;
            RenderGraphView Words;
            RenderGraphView Atlas;
            uint32 Count = 0;
            ShaderLibrary *Shaders = nullptr;
        };
        const auto count = static_cast<uint32>(_atlasGlyphs.Count());
        _renderStats.AtlasGlyphs = count;
        graph.AddPass<AtlasData>(
            "UI Glyph Atlas", RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass, AtlasData &data) {
                data.Glyphs = pass.ReadBuffer(atlasJobs, {0, 0, sizeof(UiAtlasGlyph)});
                data.Curves =
                    pass.ReadBuffer(state.GlyphCurves, {0, 0, sizeof(UIGpuCurve)});
                data.Words = pass.ReadBuffer(state.GlyphWords, {0, 0, 4});
                data.Atlas = pass.WriteTexture(state.Atlas);
                data.Count = count;
                data.Shaders = _shaders;
            },
            [](const AtlasData &data, RenderGraphContext &context) {
                UiGlyphAtlasArguments arguments;
                arguments.Glyphs = context.GetSpan(data.Glyphs);
                arguments.GlyphCurves = context.GetSpan(data.Curves);
                arguments.GlyphWords = context.GetSpan(data.Words);
                arguments.Atlas = context.GetDescriptorIndex(data.Atlas);
                arguments.GlyphCount = data.Count;
                (void)data.Shaders->Dispatch(context.List, UiGlyphAtlasShader, arguments, data.Count);
            });
        _atlasGlyphs.Clear();
    }

    _renderStats.Canvases = static_cast<uint32>(work.Count());
    for (const CanvasWork &entry : work)
    {
        AddCanvasPasses(frame, state, *entry.Canvas, entry.Target);
    }
}

void UIPainterSubsystem::AddCanvasPasses(RenderFrame &frame, FrameState &state, UIWindowCanvas &window,
                                         RenderGraphTexture target)
{
    RenderGraphBuilder &graph = frame.Graph;
    UICommandList &list = *window._rendering;
    const RenderSurface &surface = window.GetSurface();

    List<ImageEntry> images;
    const auto addImage = [&](ImageEntry entry) {
        for (usize index = 0; index < images.Count(); ++index)
        {
            const ImageEntry &other = images[index];
            if (other.Type == entry.Type && other.Value == entry.Value && other.Texture == entry.Texture)
            {
                return static_cast<uint32>(index);
            }
        }
        images.Add(entry);
        return static_cast<uint32>(images.Count() - 1);
    };
    const auto drop = [](UIGpuCommand &command) {
        command.Bounds[0] = command.Bounds[1] = command.Bounds[2] = command.Bounds[3] = 0;
    };
    for (UICanvas &canvas : list.Canvases)
    {
        for (UIGpuCommand &command : canvas.Commands)
        {
            const UICommandKind kind = command.GetKind();
            if (kind == UICommandKind::Image)
            {
                const uint32 value = BitCast<uint32>(command.Data[9]);
                if ((command.GetFlags() & UICommandFlags::FrameSlot) != UICommandFlags::None)
                {
                    const RenderFrameImage *bound = frame.FindImage(value);
                    if (bound == nullptr)
                    {
                        drop(command);
                        continue;
                    }
                    command.Data[9] = BitCast<float32>(addImage({ImageEntry::Kind::Graph, value, bound->Texture}));
                    command.Data[10] = BitCast<float32>(static_cast<uint32>(bound->Range));
                }
                else
                {
                    command.Data[9] = BitCast<float32>(addImage({ImageEntry::Kind::Descriptor, value, {}}));
                }
            }
            else if (kind == UICommandKind::Canvas || kind == UICommandKind::Backdrop)
            {
                const uint32 child = BitCast<uint32>(command.Data[4]);
                command.Data[5] = BitCast<float32>(addImage(
                    {kind == UICommandKind::Canvas ? ImageEntry::Kind::CanvasOutput : ImageEntry::Kind::CanvasBackdrop,
                     child,
                     {}}));
                command.Data[6] = BitCast<float32>(addImage({ImageEntry::Kind::CanvasOutput, child, {}}));
            }
        }
    }

    const RenderGraphBuffer brushes = state.Upload(graph, "UI Brushes", AsBytes(list.Brushes.AsSpan()));
    const RenderGraphBuffer stops = state.Upload(graph, "UI Gradient Stops", AsBytes(list.Stops.AsSpan()));
    const RenderGraphBuffer transforms = state.Upload(graph, "UI Transforms", AsBytes(list.Transforms.AsSpan()));
    const RenderGraphBuffer clips = state.Upload(graph, "UI Clips", AsBytes(list.Clips.AsSpan()));
    const RenderGraphBuffer curves = state.Upload(graph, "UI Curves", AsBytes(list.Curves.AsSpan()));
    const RenderGraphBuffer words = state.Upload(graph, "UI Outline Words", AsBytes(list.OutlineWords.AsSpan()));

    struct CanvasBuffers
    {
        RenderGraphBuffer Commands;
        RenderGraphBuffer TileRects;
        RenderGraphBuffer PairStarts;
        uint32 PairCount = 0;
        uint32 TilesX = 0;
        uint32 TilesY = 0;
        UIRect Area;
    };
    List<CanvasBuffers> buffers;
    buffers.Resize(list.Canvases.Count());
    for (usize index = 0; index < list.Canvases.Count(); ++index)
    {
        UICanvas &canvas = list.Canvases[index];
        CanvasBuffers &canvasBuffers = buffers[index];
        canvasBuffers.Area = index == 0 ? UIRect(0, 0, static_cast<float32>(surface.GetWidth()),
                                                 static_cast<float32>(surface.GetHeight()))
                                        : canvas.Bounds;
        if (canvasBuffers.Area.IsEmpty())
        {
            continue;
        }
        canvasBuffers.TilesX = (static_cast<uint32>(canvasBuffers.Area.Width) + TileSize - 1) / TileSize;
        canvasBuffers.TilesY = (static_cast<uint32>(canvasBuffers.Area.Height) + TileSize - 1) / TileSize;
        List<uint32> rects;
        List<uint32> starts;
        rects.Reserve(canvas.Commands.Count() * 4);
        starts.Reserve(canvas.Commands.Count() + 1);
        uint32 pairs = 0;
        for (const UIGpuCommand &command : canvas.Commands)
        {
            starts.Add(pairs);
            const float32 x0 = (command.Bounds[0] - canvasBuffers.Area.X) / static_cast<float32>(TileSize);
            const float32 y0 = (command.Bounds[1] - canvasBuffers.Area.Y) / static_cast<float32>(TileSize);
            const float32 x1 = (command.Bounds[2] - canvasBuffers.Area.X) / static_cast<float32>(TileSize);
            const float32 y1 = (command.Bounds[3] - canvasBuffers.Area.Y) / static_cast<float32>(TileSize);
            const auto clampTiles = [](float32 value, uint32 limit) {
                return static_cast<uint32>(Math::Clamp(value, 0.0f, static_cast<float32>(limit)));
            };
            uint32 tx0 = clampTiles(Math::Floor(x0), canvasBuffers.TilesX);
            uint32 ty0 = clampTiles(Math::Floor(y0), canvasBuffers.TilesY);
            uint32 tx1 = clampTiles(Math::Ceil(x1), canvasBuffers.TilesX);
            uint32 ty1 = clampTiles(Math::Ceil(y1), canvasBuffers.TilesY);
            if (command.Bounds[2] <= command.Bounds[0] || command.Bounds[3] <= command.Bounds[1] || tx1 <= tx0 ||
                ty1 <= ty0)
            {
                tx0 = tx1 = ty0 = ty1 = 0;
            }
            rects.Add(tx0);
            rects.Add(ty0);
            rects.Add(tx1);
            rects.Add(ty1);
            pairs += (tx1 - tx0) * (ty1 - ty0);
        }
        starts.Add(pairs);
        canvasBuffers.PairCount = pairs;
        canvasBuffers.Commands = state.Upload(graph, "UI Commands", AsBytes(canvas.Commands.AsSpan()));
        canvasBuffers.TileRects = state.Upload(graph, "UI Tile Rects", AsBytes(rects.AsSpan()));
        canvasBuffers.PairStarts = state.Upload(graph, "UI Pair Starts", AsBytes(starts.AsSpan()));
    }
    state.AddUploadPass(graph);

    List<RenderGraphTexture> outputs;
    List<RenderGraphTexture> backdrops;
    outputs.Resize(list.Canvases.Count());
    backdrops.Resize(list.Canvases.Count());
    const bool hdr = surface.IsHdr();
    const float32 whiteNits = surface.GetUiWhiteNits();
    ShaderLibrary *shaders = _shaders;
    const uint32 linearSampler = _linearSampler.Index;
    const uint32 pointSampler = _pointSampler.Index;

    const auto createTexture = [&](StringView name, const UIRect &area) {
        RhiTextureDesc desc;
        desc.Format = RhiFormat::RGBA16Float;
        desc.Width = Max(static_cast<uint32>(area.Width), 1u);
        desc.Height = Max(static_cast<uint32>(area.Height), 1u);
        desc.DebugName = name;
        return graph.CreateTexture(name, desc);
    };
    const auto addBlur = [&](RenderGraphTexture source, const UIRect &sourceArea, const UIRect &area, float32 sigma) {
        RenderGraphTexture current = source;
        UIRect currentArea = sourceArea;
        for (uint32 horizontal = 1; horizontal != ~0u; horizontal = horizontal == 1 ? 0 : ~0u)
        {
            const RenderGraphTexture blurred = createTexture("UI Blur", area);
            struct BlurData
            {
                RenderGraphView Source;
                RenderGraphView Target;
                UiBlurArguments Arguments;
                uint32 Width = 0;
                uint32 Height = 0;
            };
            const RenderGraphTexture from = current;
            const UIRect fromArea = currentArea;
            graph.AddPass<BlurData>(
                "UI Blur", RhiQueueType::Graphics,
                [&](RenderGraphPassBuilder &pass, BlurData &data) {
                    data.Source = pass.ReadTexture(from);
                    data.Target = pass.WriteTexture(blurred);
                    data.Arguments.Horizontal = horizontal;
                    data.Arguments.OffsetX = static_cast<int32>(area.X - fromArea.X);
                    data.Arguments.OffsetY = static_cast<int32>(area.Y - fromArea.Y);
                    data.Arguments.Sigma = sigma;
                    data.Width = static_cast<uint32>(area.Width);
                    data.Height = static_cast<uint32>(area.Height);
                },
                [shaders](const BlurData &data, RenderGraphContext &context) {
                    UiBlurArguments arguments = data.Arguments;
                    arguments.Source = context.GetDescriptorIndex(data.Source);
                    arguments.Target = context.GetDescriptorIndex(data.Target);
                    (void)shaders->DispatchThreads(context.List, UiBlurShader, arguments, data.Width, data.Height);
                });
            current = blurred;
            currentArea = area;
        }
        return current;
    };

    for (usize reverse = list.Canvases.Count(); reverse-- > 0;)
    {
        const UICanvas &canvas = list.Canvases[reverse];
        const CanvasBuffers &canvasBuffers = buffers[reverse];
        if (canvasBuffers.Area.IsEmpty())
        {
            continue;
        }
        const UIRect area = canvasBuffers.Area;
        const uint32 tileCount = canvasBuffers.TilesX * canvasBuffers.TilesY;
        RhiBufferDesc tileDesc;
        tileDesc.Size = Max<uint64>(tileCount * sizeof(uint32), 16);
        const RenderGraphBuffer tileCounts = graph.CreateBuffer("UI Tile Counts", tileDesc);
        const RenderGraphBuffer tileOffsets = graph.CreateBuffer("UI Tile Offsets", tileDesc);
        const RenderGraphBuffer tileCursors = graph.CreateBuffer("UI Tile Cursors", tileDesc);
        RhiBufferDesc commandsDesc;
        commandsDesc.Size = Max<uint64>(canvasBuffers.PairCount * sizeof(uint32), 16);
        const RenderGraphBuffer tileCommands = graph.CreateBuffer("UI Tile Commands", commandsDesc);
        const auto commandCount = static_cast<uint32>(canvas.Commands.Count());
        _renderStats.Commands += commandCount;
        _renderStats.TilePairs += canvasBuffers.PairCount;
        _renderStats.Tiles += tileCount;

        // Only the counts accumulate: Scan writes every tile's offset and cursor, and Scatter one tile command per
        // pair, every slot the raster pass reads.
        graph.AddPass(
            "UI Bin Clear", RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass) { (void)pass.Buffer(tileCounts, RhiAccess::CopyDest); },
            [tileCounts, countsSize = tileDesc.Size](RenderGraphContext &context) {
                context.List.FillBuffer(context.GetBuffer(tileCounts), 0, countsSize, 0);
            });
        struct BinData
        {
            RenderGraphView Rects;
            RenderGraphView Starts;
            RenderGraphView Counts;
            RenderGraphView Cursors;
            RenderGraphView Commands;
            UiBinArguments Arguments;
        };
        const RenderGraphBufferViewDesc words4{0, 0, 4};
        const RenderGraphBufferViewDesc rects16{0, 0, 16};
        UiBinArguments binArguments;
        binArguments.CommandCount = commandCount;
        binArguments.PairCount = canvasBuffers.PairCount;
        binArguments.TilesX = canvasBuffers.TilesX;
        graph.AddPass<BinData>(
            "UI Bin Count", RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass, BinData &data) {
                data.Rects = pass.ReadBuffer(canvasBuffers.TileRects, rects16);
                data.Starts = pass.ReadBuffer(canvasBuffers.PairStarts, words4);
                data.Counts = pass.WriteBuffer(tileCounts, words4);
                data.Arguments = binArguments;
            },
            [shaders](const BinData &data, RenderGraphContext &context) {
                UiBinArguments arguments = data.Arguments;
                arguments.TileRects = context.GetSpan(data.Rects);
                arguments.PairStarts = context.GetSpan(data.Starts);
                arguments.TileCounts = context.GetSpan(data.Counts);
                (void)shaders->DispatchThreads(context.List, UiCountShader, arguments, arguments.PairCount);
            });
        struct ScanData
        {
            RenderGraphView Counts;
            RenderGraphView Offsets;
            RenderGraphView Cursors;
            uint32 TileCount = 0;
        };
        graph.AddPass<ScanData>(
            "UI Bin Scan", RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass, ScanData &data) {
                data.Counts = pass.ReadBuffer(tileCounts, words4);
                // The scan writes every tile's offset and cursor.
                data.Offsets = pass.OverwriteBuffer(tileOffsets, words4);
                data.Cursors = pass.OverwriteBuffer(tileCursors, words4);
                data.TileCount = tileCount;
            },
            [shaders](const ScanData &data, RenderGraphContext &context) {
                UiScanArguments arguments;
                arguments.TileCounts = context.GetSpan(data.Counts);
                arguments.TileOffsets = context.GetSpan(data.Offsets);
                arguments.TileCursors = context.GetSpan(data.Cursors);
                arguments.TileCount = data.TileCount;
                (void)shaders->Dispatch(context.List, UiScanShader, arguments, 1);
            });
        graph.AddPass<BinData>(
            "UI Bin Scatter", RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass, BinData &data) {
                data.Rects = pass.ReadBuffer(canvasBuffers.TileRects, rects16);
                data.Starts = pass.ReadBuffer(canvasBuffers.PairStarts, words4);
                data.Cursors = pass.WriteBuffer(tileCursors, words4);
                // Fills each tile's range, which the raster reads up to the tile's count.
                data.Commands = pass.OverwriteBuffer(tileCommands, words4);
                data.Arguments = binArguments;
            },
            [shaders](const BinData &data, RenderGraphContext &context) {
                UiBinArguments arguments = data.Arguments;
                arguments.TileRects = context.GetSpan(data.Rects);
                arguments.PairStarts = context.GetSpan(data.Starts);
                arguments.TileCursors = context.GetSpan(data.Cursors);
                arguments.TileCommands = context.GetSpan(data.Commands);
                (void)shaders->DispatchThreads(context.List, UiScatterShader, arguments, arguments.PairCount);
            });

        List<uint32> splits;
        for (uint32 index = 0; index < commandCount; ++index)
        {
            const UIGpuCommand &command = canvas.Commands[index];
            if (command.GetKind() == UICommandKind::Backdrop && command.Bounds[2] > command.Bounds[0] &&
                command.Bounds[3] > command.Bounds[1])
            {
                splits.Add(index);
            }
        }
        splits.Add(commandCount);
        RenderGraphTexture background;
        uint32 first = 0;
        for (usize segment = 0; segment < splits.Count(); ++segment)
        {
            const bool last = segment + 1 == splits.Count();
            const uint32 end = splits[segment];
            const bool surfaceOutput = last && reverse == 0;
            const RenderGraphTexture output =
                surfaceOutput ? target : createTexture(last ? StringView("UI Layer") : StringView("UI Segment"), area);

            RhiBufferDesc tableDesc;
            tableDesc.Size = Max<uint64>(images.Count() * sizeof(uint32), 16);
            const RenderGraphBuffer table = graph.CreateBuffer("UI Images", tableDesc);
            List<RenderGraphTexture> available;
            for (usize index = 0; index < images.Count(); ++index)
            {
                const ImageEntry &entry = images[index];
                RenderGraphTexture texture;
                if (entry.Type == ImageEntry::Kind::Graph)
                {
                    texture = entry.Texture;
                }
                else if (entry.Type == ImageEntry::Kind::CanvasOutput)
                {
                    texture = outputs[entry.Value];
                }
                else if (entry.Type == ImageEntry::Kind::CanvasBackdrop)
                {
                    texture = backdrops[entry.Value];
                }
                available.Add(texture);
            }
            struct TableData
            {
                List<RenderGraphView> Views;
                List<uint32> Descriptors;
                RenderGraphBuffer Table;
                uint64 Size = 0;
                /// Upload memory the descriptors are written into when the pass runs, then copied in one go.
                RhiUploadAllocation Staging;
            };
            const RhiUploadAllocation staging = _uploads->Allocate(tableDesc.Size);
            graph.AddPass<TableData>(
                "UI Images", RhiQueueType::Graphics,
                [&](RenderGraphPassBuilder &pass, TableData &data) {
                    data.Table = table;
                    data.Size = tableDesc.Size;
                    data.Staging = staging;
                    (void)pass.Buffer(table, RhiAccess::CopyDest);
                    for (usize index = 0; index < images.Count(); ++index)
                    {
                        data.Descriptors.Add(images[index].Type == ImageEntry::Kind::Descriptor ? images[index].Value
                                                                                                : 0u);
                        data.Views.Add(available[index].IsValid() ? pass.ReadTexture(available[index])
                                                                  : RenderGraphView());
                    }
                },
                [](const TableData &data, RenderGraphContext &context) {
                    const RhiBuffer buffer = context.GetBuffer(data.Table);
                    if (data.Staging.Data != nullptr)
                    {
                        auto *values = reinterpret_cast<uint32 *>(data.Staging.Data);
                        MemZero(values, data.Size);
                        for (usize index = 0; index < data.Descriptors.Count(); ++index)
                        {
                            values[index] = data.Views[index].IsValid() ? context.GetDescriptorIndex(data.Views[index])
                                                                        : data.Descriptors[index];
                        }
                        context.List.CopyBuffer(buffer, 0, data.Staging.Buffer, data.Staging.Offset, data.Size);
                        return;
                    }
                    context.List.FillBuffer(buffer, 0, data.Size, 0);
                    for (usize index = 0; index < data.Descriptors.Count(); ++index)
                    {
                        const uint32 value = data.Views[index].IsValid() ? context.GetDescriptorIndex(data.Views[index])
                                                                         : data.Descriptors[index];
                        if (value != 0)
                        {
                            context.List.FillBuffer(buffer, index * sizeof(uint32), sizeof(uint32), value);
                        }
                    }
                });

            struct RasterData
            {
                RenderGraphView Commands;
                RenderGraphView Offsets;
                RenderGraphView Counts;
                RenderGraphView TileCommands;
                RenderGraphView Brushes;
                RenderGraphView Stops;
                RenderGraphView Transforms;
                RenderGraphView Clips;
                RenderGraphView Curves;
                RenderGraphView Words;
                RenderGraphView GlyphCurves;
                RenderGraphView GlyphWords;
                RenderGraphView Images;
                RenderGraphView Atlas;
                RenderGraphView Background;
                RenderGraphView Output;
                UiRasterInputs Inputs;
                bool Surface = false;
                uint32 TilesY = 0;
            };
            UiRasterInputs inputs;
            if (reverse == 0)
            {
                const LinearColor &clear = window.ClearColor;
                inputs.Clear = LinearColor(clear.R * clear.A, clear.G * clear.A, clear.B * clear.A, clear.A);
            }
            inputs.TilesX = canvasBuffers.TilesX;
            inputs.FirstCommand = first;
            inputs.EndCommand = end;
            inputs.Flags =
                (surfaceOutput ? (hdr ? EncodePq : EncodeSrgb) : EncodeLinear) | (background.IsValid() ? 0x100u : 0u);
            inputs.OriginX = area.X;
            inputs.OriginY = area.Y;
            inputs.WhiteNits = whiteNits;
            inputs.LinearSampler = linearSampler;
            inputs.PointSampler = pointSampler;
            const RenderGraphTexture backgroundTexture = background;
            graph.AddPass<RasterData>(
                surfaceOutput ? StringView("UI Raster") : StringView("UI Raster Layer"), RhiQueueType::Graphics,
                [&](RenderGraphPassBuilder &pass, RasterData &data) {
                    data.Commands = pass.ReadBuffer(canvasBuffers.Commands,
                                                    {0, 0, sizeof(UIGpuCommand)});
                    data.Offsets = pass.ReadBuffer(tileOffsets, words4);
                    data.Counts = pass.ReadBuffer(tileCounts, words4);
                    data.TileCommands = pass.ReadBuffer(tileCommands, words4);
                    data.Brushes = pass.ReadBuffer(brushes, {0, 0, sizeof(UIGpuBrush)});
                    data.Stops =
                        pass.ReadBuffer(stops, {0, 0, sizeof(UIGpuGradientStop)});
                    data.Transforms =
                        pass.ReadBuffer(transforms, {0, 0, sizeof(UIGpuTransform)});
                    data.Clips = pass.ReadBuffer(clips, {0, 0, sizeof(UIGpuClip)});
                    data.Curves = pass.ReadBuffer(curves, {0, 0, sizeof(UIGpuCurve)});
                    data.Words = pass.ReadBuffer(words, words4);
                    data.GlyphCurves =
                        pass.ReadBuffer(state.GlyphCurves, {0, 0, sizeof(UIGpuCurve)});
                    data.GlyphWords = pass.ReadBuffer(state.GlyphWords, words4);
                    data.Images = pass.ReadBuffer(table, words4);
                    data.Atlas = pass.ReadTexture(state.Atlas);
                    for (const RenderGraphTexture &image : available)
                    {
                        if (image.IsValid())
                        {
                            (void)pass.ReadTexture(image);
                        }
                    }
                    if (backgroundTexture.IsValid())
                    {
                        data.Background = pass.ReadTexture(backgroundTexture);
                    }
                    // RECOVERY-DECISION D6: each tile is written once, from Clear or Background, never read back.
                    data.Output = pass.OverwriteTexture(output);
                    data.Inputs = inputs;
                    data.Surface = surfaceOutput;
                    data.TilesY = canvasBuffers.TilesY;
                },
                [shaders](const RasterData &data, RenderGraphContext &context) {
                    UiRasterArguments arguments;
                    arguments.Inputs = data.Inputs;
                    UiRasterInputs &inputs = arguments.Inputs;
                    inputs.Commands = context.GetSpan(data.Commands);
                    inputs.TileOffsets = context.GetSpan(data.Offsets);
                    inputs.TileCounts = context.GetSpan(data.Counts);
                    inputs.TileCommands = context.GetSpan(data.TileCommands);
                    inputs.Brushes = context.GetSpan(data.Brushes);
                    inputs.Stops = context.GetSpan(data.Stops);
                    inputs.Transforms = context.GetSpan(data.Transforms);
                    inputs.Clips = context.GetSpan(data.Clips);
                    inputs.Curves = context.GetSpan(data.Curves);
                    inputs.OutlineWords = context.GetSpan(data.Words);
                    inputs.GlyphCurves = context.GetSpan(data.GlyphCurves);
                    inputs.GlyphWords = context.GetSpan(data.GlyphWords);
                    inputs.Images = context.GetSpan(data.Images);
                    inputs.Atlas = context.GetDescriptorIndex(data.Atlas);
                    inputs.Background = data.Background.IsValid() ? context.GetDescriptorIndex(data.Background) : 0;
                    arguments.Output = context.GetDescriptorIndex(data.Output);
                    (void)shaders->Dispatch(context.List, data.Surface ? UiRasterSurfaceShader : UiRasterLayerShader,
                                            arguments, inputs.TilesX, data.TilesY);
                });

            if (!last)
            {
                const UIGpuCommand &split = canvas.Commands[end];
                const uint32 child = BitCast<uint32>(split.Data[4]);
                const UICanvas &layer = list.Canvases[child];
                const UIRect layerArea = buffers[child].Area;
                if (!layerArea.IsEmpty())
                {
                    backdrops[child] = addBlur(output, area, layerArea, layer.BackdropBlur);
                }
            }
            background = output;
            first = end;
        }
        RenderGraphTexture result = background;
        if (reverse != 0 && canvas.Blur > 0)
        {
            result = addBlur(result, area, area, canvas.Blur);
        }
        outputs[reverse] = result;
    }
}

void UIPainterFrameFeature::Extract(RenderFrameExtract & /*frame*/)
{
    if (UIPainterSubsystem *painter = Engine::GetSubsystem<UIPainterSubsystem>())
    {
        painter->Extract();
    }
}

void UIPainterFrameFeature::AddPasses(RenderFrame &frame)
{
    if (UIPainterSubsystem *painter = Engine::GetSubsystem<UIPainterSubsystem>())
    {
        painter->AddPasses(frame);
    }
}
} // namespace Marlin
