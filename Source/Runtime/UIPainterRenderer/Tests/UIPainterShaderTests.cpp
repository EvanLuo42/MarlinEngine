#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirTarget.h"
#include "Math/Float16.h"
#include "Math/MathUtility.h"
#include "RHI/RhiTestDevice.h"
#include "Reflection/TypeInfo.h"
#include "ShaderBackendSpirv/SpirvTarget.h"
#include "UIPainter/UIPainter.h"
#include "UIPainter/UIPath.h"
#include "UIPainterRenderer/UIPainterShaders.h"
#include "spirv-tools/libspirv.h"

namespace Marlin::Tests
{
namespace
{
/// A global shader entry point of the mounted engine's source set, compiled the way the shader pipeline compiles it
/// and validated for Vulkan 1.4, with the [Uniform] parameter block its arguments match.
struct CompiledShader
{
    struct ArgumentField
    {
        String Name;
        uint32 Offset = 0;
        uint32 Size = 0;
    };

    explicit CompiledShader(StringView entryPoint)
    {
        static constexpr StringView Roots[] = {"/Engine"};
        VfsSourceProvider sources{Roots};
        LanguageCompileOptions options;
        LanguageCompilation compilation(sources, options);
        MirModule module;
        if (!compilation.Load() || !compilation.CompileEntryPoint(entryPoint, module))
        {
            Errors = compilation.GetDiagnostics().ToString();
            return;
        }
        for (const MirEntryPoint *entry : module.GetEntryPoints())
        {
            for (const MirGlobal *global : entry->Interface)
            {
                if (global->GetAddressSpace() != MirAddressSpace::Uniform)
                {
                    continue;
                }
                // An indirect block is a Ptr to the struct C++ fills (MarlinLanguage.md §9.3).
                const MirType *block = entry->ArgumentsIndirect ? global->GetValueType()->GetElement() : global->GetValueType();
                ArgumentsSize = block->GetSize();
                for (const MirStructField &field : block->GetFields())
                {
                    Arguments.Add({String(field.Name), field.Offset, field.Type->GetSize()});
                }
            }
        }
        MirTargetRegistry targets;
        RegisterSpirvTarget(targets);
        const MirTarget *target = targets.Find(SpirvTargetName);
        const MirPassRegistry registry = MirPassRegistry::CreateDefault();
        MirModulePassManager passes({.VerifyEach = true});
        String error;
        registry.ParsePipeline(MirShaderPipeline, passes, error, {.VerifyEach = true});
        List<String> errors;
        target->AddLegalizationPasses(passes, errors);
        MirModuleAnalysisManager analyses;
        passes.Run(module, analyses);
        if (!passes.GetErrors().IsEmpty() || !errors.IsEmpty())
        {
            Errors = String(!errors.IsEmpty() ? errors[0].AsView() : passes.GetErrors()[0].AsView());
            return;
        }
        if (!target->Emit(module, {}, Code, errors))
        {
            Errors = String(errors.IsEmpty() ? StringView("emission failed") : errors[0].AsView());
            return;
        }
        spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_4);
        spv_diagnostic diagnostic = nullptr;
        spv_const_binary_t binary{reinterpret_cast<const uint32 *>(Code.Data()), Code.Count() / sizeof(uint32)};
        if (spvValidate(context, &binary, &diagnostic) != SPV_SUCCESS)
        {
            Errors = diagnostic != nullptr && diagnostic->error != nullptr ? String(diagnostic->error)
                                                                           : String("spirv-val failed");
        }
        spvDiagnosticDestroy(diagnostic);
        spvContextDestroy(context);
    }

    /// What ShaderLibrary checks before it dispatches: the C++ arguments struct and the entry point's [Uniform] parameter block
    /// agree field by field in name, offset and size. Empty if they do.
    [[nodiscard]] String CheckArguments(const ShaderDeclaration &shader) const
    {
        const TypeInfo &type = shader.Arguments();
        StringBuilder<512> problems;
        if (ArgumentsSize != shader.ArgumentsSize)
        {
            FormatTo(problems, "{0} bytes against {1}; ", ArgumentsSize, shader.ArgumentsSize);
        }
        usize count = 0;
        for (const FieldInfo *field : type.GetFields())
        {
            if (field->IsStatic())
            {
                continue;
            }
            ++count;
            const ArgumentField *match = nullptr;
            for (const ArgumentField &constant : Arguments)
            {
                match = constant.Name.AsView() == field->GetName() ? &constant : match;
            }
            if (match == nullptr || match->Offset != field->GetOffset() || match->Size != field->GetType().GetSize())
            {
                FormatTo(problems, "{0}; ", field->GetName());
            }
        }
        if (count != Arguments.Count())
        {
            FormatTo(problems, "{0} fields against {1}", count, Arguments.Count());
        }
        return String(problems.AsView());
    }

    String Errors;
    List<uint8> Code;
    List<ArgumentField> Arguments;
    uint32 ArgumentsSize = 0;
};

const ShaderDeclaration *const UiShaders[] = {&UiCountShader,         &UiScanShader,        &UiScatterShader,
                                              &UiRasterSurfaceShader, &UiRasterLayerShader, &UiGlyphAtlasShader,
                                              &UiBlurShader};

template <typename T> Span<const uint8> Bytes(Span<T> values)
{
    return {reinterpret_cast<const uint8 *>(values.Data()), values.Count() * sizeof(T)};
}
} // namespace

MARLIN_TEST(UIPainterRenderer, PassShadersCompileAndMatchTheirArguments)
{
    for (const ShaderDeclaration *shader : UiShaders)
    {
        const CompiledShader compiled(shader->EntryPoint);
        MARLIN_EXPECTF(compiled.Errors.IsEmpty(), "{0}:\n{1}", shader->EntryPoint, compiled.Errors);
        if (compiled.Errors.IsEmpty())
        {
            const String problems = compiled.CheckArguments(*shader);
            MARLIN_EXPECTF(problems.IsEmpty(), "{0}: {1}", shader->EntryPoint, problems);
        }
    }
}

namespace
{
/// Runs UIPainter's binning and layer raster passes over one recorded canvas on the fixture's device, as
/// UIPainterSubsystem does for a layer, and reads back the premultiplied RGBA16F result.
struct RasterRun
{
    RasterRun(RhiDeviceFixture &fixture, const UICommandList &list, uint32 width, uint32 height)
        : Fixture(fixture), Device(*fixture.Device), Width(width), Height(height)
    {
        const UICanvas &canvas = list.Canvases[0];
        const uint32 tilesX = (width + 15) / 16;
        const uint32 tilesY = (height + 15) / 16;
        const uint32 tileCount = tilesX * tilesY;
        List<uint32> rects;
        List<uint32> starts;
        uint32 pairs = 0;
        for (const UIGpuCommand &command : canvas.Commands)
        {
            starts.Add(pairs);
            const auto clampTiles = [](float32 value, uint32 limit) {
                return static_cast<uint32>(Math::Clamp(value, 0.0f, static_cast<float32>(limit)));
            };
            uint32 tx0 = clampTiles(Math::Floor(command.Bounds[0] / 16), tilesX);
            uint32 ty0 = clampTiles(Math::Floor(command.Bounds[1] / 16), tilesY);
            uint32 tx1 = clampTiles(Math::Ceil(command.Bounds[2] / 16), tilesX);
            uint32 ty1 = clampTiles(Math::Ceil(command.Bounds[3] / 16), tilesY);
            if (tx1 <= tx0 || ty1 <= ty0)
            {
                tx0 = tx1 = ty0 = ty1 = 0;
            }
            rects.AddRange({tx0, ty0, tx1, ty1});
            pairs += (tx1 - tx0) * (ty1 - ty0);
        }
        starts.Add(pairs);
        Pairs = pairs;

        const ShaderSpan commands = Structured(Bytes(canvas.Commands.AsSpan()), sizeof(UIGpuCommand));
        const ShaderSpan tileRects = Structured(Bytes(rects.AsSpan()), 16);
        const ShaderSpan pairStarts = Structured(Bytes(starts.AsSpan()), 4);
        const ShaderSpan brushes = Structured(Bytes(list.Brushes.AsSpan()), sizeof(UIGpuBrush));
        const ShaderSpan stops = Structured(Bytes(list.Stops.AsSpan()), sizeof(UIGpuGradientStop));
        const ShaderSpan transforms = Structured(Bytes(list.Transforms.AsSpan()), sizeof(UIGpuTransform));
        const ShaderSpan clips = Structured(Bytes(list.Clips.AsSpan()), sizeof(UIGpuClip));
        const ShaderSpan curves = Structured(Bytes(list.Curves.AsSpan()), sizeof(UIGpuCurve));
        const ShaderSpan words = Structured(Bytes(list.OutlineWords.AsSpan()), 4);
        const RhiTexture dummy = Texture(RhiFormat::RGBA16Float, 1, 1, RhiTextureUsage::ShaderResource);
        const RhiTexture atlas = Texture(RhiFormat::R8Unorm, 1, 1, RhiTextureUsage::ShaderResource);
        const uint32 dummyView = TextureView(dummy, RhiViewAccess::ShaderResource);
        const uint32 images[] = {dummyView};
        const ShaderSpan imageTable = Structured(Bytes(Span<const uint32>(images)), 4);
        const RhiBuffer counts = Scratch(tileCount * 4);
        const RhiBuffer offsets = Scratch(tileCount * 4);
        const RhiBuffer cursors = Scratch(tileCount * 4);
        const RhiBuffer tileCommands = Scratch(Max(pairs, 1u) * 4);
        Output = Texture(RhiFormat::RGBA16Float, width, height,
                         RhiTextureUsage::UnorderedAccess | RhiTextureUsage::CopySource);
        Readback =
            Fixture.CreateBuffer(uint64{width} * height * 8, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
        RhiSamplerDesc linear;
        RhiSamplerDesc point;
        point.MinFilter = point.MagFilter = point.MipFilter = RhiFilter::Nearest;
        const RhiSampler linearSampler = Device.CreateSampler(linear).Value();
        const RhiSampler pointSampler = Device.CreateSampler(point).Value();
        Samplers.AddRange({linearSampler, pointSampler});

        for (const ShaderDeclaration *shader : {static_cast<const ShaderDeclaration *>(&UiCountShader),
                                                static_cast<const ShaderDeclaration *>(&UiScanShader),
                                                static_cast<const ShaderDeclaration *>(&UiScatterShader),
                                                static_cast<const ShaderDeclaration *>(&UiRasterLayerShader)})
        {
            const CompiledShader compiled(shader->EntryPoint);
            if (!compiled.Errors.IsEmpty())
            {
                Error = compiled.Errors;
                return;
            }
            RhiComputePipelineDesc desc;
            desc.Shader.Code = compiled.Code.AsSpan();
            desc.Shader.EntryPoint = shader->EntryPoint;
            desc.Shader.Stage = RhiShaderStage::Compute;
            desc.DebugName = shader->EntryPoint;
            const RhiPipeline pipeline = Device.CreatePipeline(desc);
            if (Device.WaitForPipeline(pipeline) != RhiPipelineState::Ready)
            {
                Error = Format("{0} did not make a pipeline", shader->EntryPoint);
                return;
            }
            Pipelines.Add(pipeline);
        }

        RhiCommandList *recording = Device.BeginCommandList(RhiQueueType::Graphics);
        RhiCommandList &cmd = *recording;
        for (const RhiBuffer buffer : {counts, offsets, cursors, tileCommands})
        {
            cmd.FillBuffer(buffer, 0, Device.GetNativeBuffer(buffer).Size, 0);
        }
        TextureBarrier(cmd, dummy, RhiLayout::Undefined, RhiLayout::ShaderResource);
        TextureBarrier(cmd, atlas, RhiLayout::Undefined, RhiLayout::ShaderResource);
        TextureBarrier(cmd, Output, RhiLayout::Undefined, RhiLayout::UnorderedAccess);
        GlobalBarrier(cmd, RhiSync::Copy, RhiAccess::CopyDest);

        UiBinArguments bin;
        bin.TileRects = tileRects;
        bin.PairStarts = pairStarts;
        bin.TileCounts = Words(counts);
        bin.TileCursors = Words(cursors);
        bin.TileCommands = Words(tileCommands);
        bin.CommandCount = static_cast<uint32>(canvas.Commands.Count());
        bin.PairCount = pairs;
        bin.TilesX = tilesX;
        cmd.SetPipeline(Pipelines[0]);
        cmd.SetShaderArguments(bin);
        cmd.Dispatch((pairs + 63) / 64, 1, 1);
        GlobalBarrier(cmd, RhiSync::ComputeShading, RhiAccess::UnorderedAccess);

        UiScanArguments scan;
        scan.TileCounts = Words(counts);
        scan.TileOffsets = Words(offsets);
        scan.TileCursors = bin.TileCursors;
        scan.TileCount = tileCount;
        cmd.SetPipeline(Pipelines[1]);
        cmd.SetShaderArguments(scan);
        cmd.Dispatch(1, 1, 1);
        GlobalBarrier(cmd, RhiSync::ComputeShading, RhiAccess::UnorderedAccess);

        cmd.SetPipeline(Pipelines[2]);
        cmd.SetShaderArguments(bin);
        cmd.Dispatch((pairs + 63) / 64, 1, 1);
        GlobalBarrier(cmd, RhiSync::ComputeShading, RhiAccess::UnorderedAccess);

        UiRasterArguments raster;
        raster.Inputs.Clear = LinearColor(0, 0, 0, 0);
        raster.Inputs.Commands = commands;
        raster.Inputs.TileOffsets = Words(offsets);
        raster.Inputs.TileCounts = scan.TileCounts;
        raster.Inputs.TileCommands = Words(tileCommands);
        raster.Inputs.Brushes = brushes;
        raster.Inputs.Stops = stops;
        raster.Inputs.Transforms = transforms;
        raster.Inputs.Clips = clips;
        raster.Inputs.Curves = curves;
        raster.Inputs.OutlineWords = words;
        raster.Inputs.GlyphCurves = curves;
        raster.Inputs.GlyphWords = words;
        raster.Inputs.Images = imageTable;
        raster.Inputs.Atlas = TextureView(atlas, RhiViewAccess::ShaderResource);
        raster.Inputs.Background = dummyView;
        raster.Inputs.LinearSampler = linearSampler.Index;
        raster.Inputs.PointSampler = pointSampler.Index;
        raster.Inputs.TilesX = tilesX;
        raster.Inputs.FirstCommand = 0;
        raster.Inputs.EndCommand = static_cast<uint32>(canvas.Commands.Count());
        raster.Inputs.WhiteNits = 80;
        raster.Output = TextureView(Output, RhiViewAccess::UnorderedAccess);
        // 288 bytes, more than the arguments passed directly: the shader reads them through their address (§9.3).
        const RhiBuffer rasterArguments = Fixture.CreateBuffer(sizeof(raster), RhiBufferUsage::ShaderResource,
                                                               RhiMemoryLocation::Upload);
        Buffers.Add(rasterArguments);
        MemCopy(Device.GetMappedData(rasterArguments), &raster, sizeof(raster));
        cmd.SetPipeline(Pipelines[3]);
        cmd.SetShaderArguments(Device.GetBufferAddress(rasterArguments));
        cmd.Dispatch(tilesX, tilesY, 1);

        TextureBarrier(cmd, Output, RhiLayout::UnorderedAccess, RhiLayout::CopySource);
        RhiBufferTextureLayout layout;
        layout.RowPitch = width * 8;
        cmd.CopyTextureToBuffer(Readback, layout, Output, RhiTextureRegion{});
        BinReadback = Fixture.CreateBuffer(uint64{tileCount} * 8 + Max(pairs, 1u) * 4, RhiBufferUsage::CopyDest,
                                           RhiMemoryLocation::Readback);
        Buffers.Add(BinReadback);
        {
            RhiGlobalBarrier toCopy;
            toCopy.SyncBefore = RhiSync::ComputeShading | RhiSync::Copy;
            toCopy.AccessBefore = RhiAccess::UnorderedAccess | RhiAccess::CopyDest;
            toCopy.SyncAfter = RhiSync::Copy;
            toCopy.AccessAfter = RhiAccess::CopySource;
            RhiBarriers copyBarriers;
            copyBarriers.Global = Span<const RhiGlobalBarrier>(&toCopy, 1);
            cmd.Barrier(copyBarriers);
        }
        cmd.CopyBuffer(BinReadback, 0, counts, 0, uint64{tileCount} * 4);
        cmd.CopyBuffer(BinReadback, uint64{tileCount} * 4, offsets, 0, uint64{tileCount} * 4);
        cmd.CopyBuffer(BinReadback, uint64{tileCount} * 8, tileCommands, 0, uint64{Max(pairs, 1u)} * 4);
        TileCount = tileCount;
        RhiGlobalBarrier host;
        host.SyncBefore = RhiSync::Copy;
        host.AccessBefore = RhiAccess::CopyDest;
        host.SyncAfter = RhiSync::Host;
        host.AccessAfter = RhiAccess::HostRead;
        RhiBarriers barriers;
        barriers.Global = Span<const RhiGlobalBarrier>(&host, 1);
        cmd.Barrier(barriers);
        Fixture.SubmitAndWait(recording);
        Textures.AddRange({dummy, atlas});
    }

    ~RasterRun()
    {
        Device.WaitIdle();
        for (const RhiResourceView view : Views)
        {
            Device.Destroy(view);
        }
        for (const RhiBuffer buffer : Buffers)
        {
            Device.Destroy(buffer);
        }
        for (const RhiTexture texture : Textures)
        {
            Device.Destroy(texture);
        }
        for (const RhiPipeline pipeline : Pipelines)
        {
            Device.Destroy(pipeline);
        }
        for (const RhiSampler sampler : Samplers)
        {
            Device.Destroy(sampler);
        }
        if (Output.IsValid())
        {
            Device.Destroy(Output);
        }
        if (Readback.IsValid())
        {
            Device.Destroy(Readback);
        }
    }

    /// The premultiplied color at pixel (x, y).
    [[nodiscard]] LinearColor At(uint32 x, uint32 y) const
    {
        const auto *halves = reinterpret_cast<const uint16 *>(Device.GetMappedData(Readback)) + (y * Width + x) * 4;
        const auto channel = [&](uint32 index) { return BitCast<Float16>(halves[index]).ToFloat(); };
        return LinearColor(channel(0), channel(1), channel(2), channel(3));
    }

    /// *bytes* in GPU memory as a Span of *stride*-byte elements (MarlinLanguage.md §4.5).
    ShaderSpan Structured(Span<const uint8> bytes, uint32 stride)
    {
        const uint64 size = Max<uint64>((bytes.Count() + 15) & ~uint64{15}, 16);
        const RhiBuffer buffer = Fixture.CreateBuffer(size, RhiBufferUsage::ShaderResource, RhiMemoryLocation::Upload);
        Buffers.Add(buffer);
        MemCopy(Device.GetMappedData(buffer), bytes.Data(), bytes.Count());
        return {.Pointer = Device.GetBufferAddress(buffer), .Length = static_cast<uint32>(bytes.Count() / stride)};
    }

    RhiBuffer Scratch(uint64 size)
    {
        const RhiBuffer buffer = Fixture.CreateBuffer(Max<uint64>(size, 16),
                                                      RhiBufferUsage::ShaderResource | RhiBufferUsage::UnorderedAccess |
                                                          RhiBufferUsage::CopySource | RhiBufferUsage::CopyDest,
                                                      RhiMemoryLocation::Device);
        Buffers.Add(buffer);
        return buffer;
    }

    /// The 32-bit words of *buffer* as a Span.
    ShaderSpan Words(RhiBuffer buffer)
    {
        return {.Pointer = Device.GetBufferAddress(buffer),
                .Length = static_cast<uint32>(Device.GetNativeBuffer(buffer).Size / 4)};
    }

    RhiTexture Texture(RhiFormat format, uint32 width, uint32 height, RhiTextureUsage usage)
    {
        RhiTextureDesc desc;
        desc.Format = format;
        desc.Width = width;
        desc.Height = height;
        desc.Usage = usage;
        desc.DebugName = "UiTest";
        return Device.CreateTexture(desc).Value();
    }

    uint32 TextureView(RhiTexture texture, RhiViewAccess access)
    {
        RhiTextureViewDesc desc;
        desc.Texture = texture;
        desc.Access = access;
        const RhiResourceView view = Device.CreateView(desc).Value();
        Views.Add(view);
        return view.Index;
    }

    static void TextureBarrier(RhiCommandList &list, RhiTexture texture, RhiLayout before, RhiLayout after)
    {
        RhiTextureBarrier barrier;
        barrier.Texture = texture;
        barrier.SyncBefore = before == RhiLayout::Undefined ? RhiSync::None : RhiSync::All;
        barrier.AccessBefore = before == RhiLayout::UnorderedAccess ? RhiAccess::UnorderedAccess : RhiAccess::None;
        barrier.SyncAfter = RhiSync::All;
        barrier.AccessAfter = after == RhiLayout::CopySource        ? RhiAccess::CopySource
                              : after == RhiLayout::UnorderedAccess ? RhiAccess::UnorderedAccess
                                                                    : RhiAccess::ShaderResource;
        barrier.LayoutBefore = before;
        barrier.LayoutAfter = after;
        RhiBarriers barriers;
        barriers.Textures = Span<const RhiTextureBarrier>(&barrier, 1);
        list.Barrier(barriers);
    }

    static void GlobalBarrier(RhiCommandList &list, RhiSync before, RhiAccess access)
    {
        RhiGlobalBarrier barrier;
        barrier.SyncBefore = before;
        barrier.AccessBefore = access;
        barrier.SyncAfter = RhiSync::ComputeShading;
        barrier.AccessAfter = RhiAccess::ShaderResource | RhiAccess::UnorderedAccess;
        RhiBarriers barriers;
        barriers.Global = Span<const RhiGlobalBarrier>(&barrier, 1);
        list.Barrier(barriers);
    }

    RhiDeviceFixture &Fixture;
    RhiDevice &Device;
    uint32 Width;
    uint32 Height;
    uint32 Pairs = 0;
    uint32 TileCount = 0;
    RhiBuffer BinReadback;
    String Error;
    RhiTexture Output;
    RhiBuffer Readback;
    List<RhiPipeline> Pipelines;
    List<RhiResourceView> Views;
    List<RhiBuffer> Buffers;
    List<RhiTexture> Textures;
    List<RhiSampler> Samplers;
};
} // namespace

MARLIN_TEST_F(RhiDeviceFixture, UIPainterRenderer, RastersShapesAndJoinedStrokes, Gpu)
{
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 64, 32, 1);
    painter.FillRect(UIRect(4.5f, 4, 20, 20), UIBrush::Solid(LinearColor(1, 0, 0, 1)));
    // Two stroke pieces that meet at x = 48.5, a pixel center: free ends would leave that column half covered.
    UIPath path;
    path.MoveTo(Vector2(32.5f, 16));
    path.LineTo(Vector2(48.5f, 16));
    path.LineTo(Vector2(60.5f, 16.5f));
    UIStrokeStyle style;
    style.Width = 6;
    style.Join = UILineJoin::Bevel;
    painter.StrokePath(path, style, UIBrush::Solid(LinearColor(0, 0, 1, 1)));
    painter.End();
    MARLIN_REQUIRE(list.Canvases.Count() == 1 && list.Canvases[0].Commands.Count() >= 3);

    const RasterRun run(*this, list, 64, 32);
    MARLIN_REQUIRE(run.Error.IsEmpty());
    MARLIN_EXPECT(run.Pairs > 0);
    // Binning: every pair landed in a tile, and the offsets are the exclusive prefix of the counts.
    const auto *bins = reinterpret_cast<const uint32 *>(run.Device.GetMappedData(run.BinReadback));
    uint32 total = 0;
    bool prefix = true;
    for (uint32 tile = 0; tile < run.TileCount; ++tile)
    {
        prefix = prefix && bins[run.TileCount + tile] == total;
        total += bins[tile];
    }
    MARLIN_EXPECTF(total == run.Pairs && prefix, "{0} of {1} pairs binned", total, run.Pairs);
    const LinearColor inside = run.At(12, 12);
    MARLIN_EXPECTF(inside.R > 0.99f && inside.A > 0.99f && inside.B < 0.01f, "rect {0} {1} {2} {3}", inside.R, inside.G,
                   inside.B, inside.A);
    const LinearColor outside = run.At(2, 28);
    MARLIN_EXPECTF(outside.A < 0.01f, "empty {0}", outside.A);
    const LinearColor edge = run.At(24, 12);
    MARLIN_EXPECTF(edge.A > 0.4f && edge.A < 0.6f, "the rect's right edge is half covered: {0}", edge.A);
    const LinearColor stroke = run.At(40, 16);
    MARLIN_EXPECTF(stroke.B > 0.99f && stroke.A > 0.99f, "stroke {0} {1}", stroke.B, stroke.A);
    const LinearColor joint = run.At(48, 16);
    MARLIN_EXPECTF(joint.A > 0.99f, "the joint between the pieces has no seam: {0}", joint.A);
}
} // namespace Marlin::Tests
