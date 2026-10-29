#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirTarget.h"
#include "Memory/UniquePtr.h"
#include "RHI/RhiTestDevice.h"
#include "ShaderBackendSpirv/SpirvTarget.h"
#include "RenderGraph/RenderGraphExport.h"
#include "RenderGraph/RenderGraphSubsystem.h"
#include "Templates/Function.h"

// The compiler, pool, executor and export builder are private headers of RenderGraph, exported for these tests.
#include "RenderGraph/RenderGraphCompiler.h"
#include "RenderGraph/RenderGraphExecutor.h"
#include "RenderGraph/RenderGraphFrame.h"
#include "RenderGraph/RenderGraphTransientPool.h"

namespace Marlin::Tests
{
namespace
{
using PassBody = Function<void(RenderGraphContext &)>;

/// A frame filled the way RenderGraphBuilder fills it, around a subsystem without a device of its own: the tests
/// compile, place and run it against the fixture's device.
class TestGraph
{
public:
    explicit TestGraph(RhiDevice &device) : Frame(Subsystem, device), Pool(device)
    {
    }

    ~TestGraph()
    {
        Frame.Device.WaitIdle();
        Frame.Device.ProcessCompletions();
    }

    uint32 Buffer(StringView name, uint64 size)
    {
        RenderGraphResource &resource = AddResource(name, RenderGraphResourceKind::Buffer);
        resource.BufferDesc.Size = size;
        resource.BufferDesc.DebugName = name;
        return static_cast<uint32>(Frame.Resources.Count() - 1);
    }

    uint32 Import(StringView name, RenderGraphExternalBuffer &buffer, uint64 size)
    {
        RenderGraphResource &resource = AddResource(name, RenderGraphResourceKind::Buffer);
        resource.ExternalBuffer = &buffer;
        resource.Buffer = buffer.Buffer;
        resource.BufferDesc.Size = size;
        return static_cast<uint32>(Frame.Resources.Count() - 1);
    }

    uint32 Texture(StringView name, const RhiTextureDesc &desc)
    {
        RenderGraphResource &resource = AddResource(name, RenderGraphResourceKind::Texture);
        resource.TextureDesc = desc;
        resource.TextureDesc.DebugName = name;
        return static_cast<uint32>(Frame.Resources.Count() - 1);
    }

    uint32 Pass(StringView name, RhiQueueType queue, PassBody body = {})
    {
        const uint32 index = static_cast<uint32>(Frame.Passes.Count());
        RenderGraphPass &pass = Frame.Passes.AddDefaulted();
        pass.PassName = Name(name);
        pass.DeclaredQueue = queue;
        pass.Queue = queue;
        pass.FirstAccess = static_cast<uint32>(Frame.Accesses.Count());
        pass.FirstView = static_cast<uint32>(Frame.Views.Count());
        if (body)
        {
            UniquePtr<PassBody> &stored = _bodies.Add(MakeUnique<PassBody>(Move(body)));
            pass.Function.Body = stored.Get();
            pass.Function.Invoke = [](void *function, void * /*data*/, RenderGraphContext &context) {
                (*static_cast<PassBody *>(function))(context);
            };
        }
        return index;
    }

    /// Accesses must follow their pass, as setup functions declare them.
    void Access(uint32 pass, uint32 resource, RhiAccess access, bool reads, bool writes,
                const RhiSubresourceRange &range = {})
    {
        RenderGraphAccess &entry = Frame.Accesses.AddDefaulted();
        entry.Resource = resource;
        entry.Access = access;
        entry.Range = range;
        entry.ReadsContents = reads;
        entry.WritesContents = writes;
        ++Frame.Passes[pass].AccessCount;
    }

    void Compile(bool asyncQueues = true, bool culling = true)
    {
        RenderGraphCompileOptions options;
        options.AsyncCompute = asyncQueues;
        options.AsyncCopy = asyncQueues;
        options.Culling = culling;
        RenderGraphCompiler::Compile(Frame, options);
    }

    /// Places, records and submits the compiled frame, then waits for the GPU.
    bool Run(bool aliasing = true)
    {
        Result<void, RhiError> result = Pool.Allocate(Frame, 0, aliasing, uint64{4} << 20);
        if (result.IsOk())
        {
            RenderGraphCompiler::ResolveBarrierHandles(Frame);
            result = RenderGraphExecutor::Run(Frame, Subsystem);
        }
        Frame.Device.WaitIdle();
        Frame.Device.ProcessCompletions();
        Pool.Release(Frame, 0, 1);
        return result.IsOk();
    }

    [[nodiscard]] const RenderGraphPass &Get(uint32 pass) const
    {
        return Frame.Passes[pass];
    }

    [[nodiscard]] RhiBuffer GetBuffer(RenderGraphContext &context, uint32 resource) const
    {
        return context.GetBuffer(RenderGraphBuffer{resource});
    }

    RenderGraphSubsystem Subsystem;
    RenderGraphFrame Frame;
    RenderGraphTransientPool Pool;

private:
    RenderGraphResource &AddResource(StringView name, RenderGraphResourceKind kind)
    {
        RenderGraphResource &resource = Frame.Resources.AddDefaulted();
        resource.Name = name;
        resource.Kind = kind;
        return resource;
    }

    List<UniquePtr<PassBody>> _bodies;
};

/// The segment that holds *pass*, which must be kept.
const RenderGraphSegment &SegmentOf(const TestGraph &graph, uint32 pass)
{
    return graph.Frame.Segments[graph.Get(pass).Segment];
}

usize SubmitPosition(const TestGraph &graph, uint32 segment)
{
    return graph.Frame.SubmitOrder.IndexOf(segment);
}
} // namespace

MARLIN_TEST_F(RhiDeviceFixture, RenderGraph, CullsPassesNothingUses, Gpu)
{
    RenderGraphExternalBuffer output{CreateBuffer(256, RhiBufferUsage::CopyDest, RhiMemoryLocation::Device), false};
    TestGraph graph(*Device);
    const uint32 unused = graph.Buffer("Unused", 256);
    const uint32 temporary = graph.Buffer("Temporary", 256);
    const uint32 imported = graph.Import("Output", output, 256);
    const uint32 dead = graph.Pass("Dead", RhiQueueType::Graphics);
    graph.Access(dead, unused, RhiAccess::CopyDest, false, true);
    const uint32 produce = graph.Pass("Produce", RhiQueueType::Graphics);
    graph.Access(produce, temporary, RhiAccess::CopyDest, false, true);
    const uint32 consume = graph.Pass("Consume", RhiQueueType::Graphics);
    graph.Access(consume, temporary, RhiAccess::CopySource, true, false);
    graph.Access(consume, imported, RhiAccess::CopyDest, false, true);
    graph.Compile();

    MARLIN_EXPECT(!graph.Get(dead).Kept && graph.Get(produce).Kept && graph.Get(consume).Kept);
    // Prologue, Produce, Consume, Epilogue, all on Graphics.
    MARLIN_REQUIRE(graph.Frame.Kept.Count() == 4);
    MARLIN_EXPECT(graph.Frame.GetKept(0).Role == RenderGraphPassRole::Prologue);
    MARLIN_EXPECT(graph.Frame.Kept[1] == produce && graph.Frame.Kept[2] == consume);
    MARLIN_EXPECT(graph.Frame.GetKept(3).Role == RenderGraphPassRole::Epilogue);
    MARLIN_EXPECT(graph.Frame.GetKept(3).PassName == Name("RenderGraph.Epilogue.Graphics"));
    MARLIN_EXPECT(!graph.Frame.Resources[unused].Used && graph.Frame.Resources[temporary].Used);
    // Buffers take a global barrier between the copy into the temporary and the read from it.
    const RenderGraphPass &reader = graph.Get(consume);
    MARLIN_REQUIRE(reader.Before.GlobalCount == 1);
    const RhiGlobalBarrier &barrier = graph.Frame.GlobalBarriers[reader.Before.FirstGlobal];
    MARLIN_EXPECT(barrier.AccessBefore == RhiAccess::CopyDest && barrier.AccessAfter == RhiAccess::CopySource);
    MARLIN_EXPECT(barrier.SyncBefore == RhiSync::Copy && barrier.SyncAfter == RhiSync::Copy);

    TestGraph uncull(*Device);
    const uint32 lone = uncull.Buffer("Lone", 256);
    const uint32 kept = uncull.Pass("Kept", RhiQueueType::Graphics);
    uncull.Access(kept, lone, RhiAccess::CopyDest, false, true);
    uncull.Compile(true, false);
    MARLIN_EXPECT(uncull.Get(kept).Kept);
    Device->Destroy(output.Buffer);
}

MARLIN_TEST_F(RhiDeviceFixture, RenderGraph, OrdersQueuesWithWaitsAndTransfers, Gpu)
{
    TestGraph graph(*Device);
    const uint32 shared = graph.Buffer("Shared", 1024);
    const uint32 produce = graph.Pass("Produce", RhiQueueType::Compute);
    graph.Access(produce, shared, RhiAccess::CopyDest, false, true);
    const uint32 consume = graph.Pass("Consume", RhiQueueType::Graphics);
    graph.Access(consume, shared, RhiAccess::CopySource, true, false);
    graph.Frame.Passes[consume].NeverCull = true;
    graph.Compile();

    const RenderGraphPass &producer = graph.Get(produce);
    const RenderGraphPass &consumer = graph.Get(consume);
    MARLIN_EXPECT(producer.Queue == RhiQueueType::Compute && consumer.Queue == RhiQueueType::Graphics);
    MARLIN_EXPECT(consumer.Waits[static_cast<uint32>(RhiQueueType::Compute)] == producer.Order);
    MARLIN_EXPECT(producer.Signals);
    MARLIN_EXPECT(consumer.Done[static_cast<uint32>(RhiQueueType::Compute)] == producer.Order);
    // The compute queue releases the buffer after the producer and Graphics acquires it before the consumer.
    MARLIN_REQUIRE(producer.After.BufferCount == 1 && consumer.Before.BufferCount == 1);
    const RhiBufferBarrier &release = graph.Frame.BufferBarriers[producer.After.FirstBuffer];
    const RhiBufferBarrier &acquire = graph.Frame.BufferBarriers[consumer.Before.FirstBuffer];
    MARLIN_EXPECT(release.TransferQueue && release.Transfer.From == RhiQueueType::Compute &&
                  release.Transfer.To == RhiQueueType::Graphics);
    MARLIN_EXPECT(acquire.TransferQueue && acquire.AccessAfter == RhiAccess::CopySource);
    MARLIN_EXPECT(release.AccessBefore == RhiAccess::CopyDest && release.AccessAfter == RhiAccess::None);
    // The producer ends its submission, which is submitted before the one that waits for it.
    const RenderGraphSegment &signaling = SegmentOf(graph, produce);
    const RenderGraphSegment &waiting = SegmentOf(graph, consume);
    MARLIN_EXPECT(signaling.LastOrder == producer.Order && waiting.FirstPass < graph.Frame.SegmentPasses.Count());
    MARLIN_EXPECT(graph.Frame.SegmentPasses[waiting.FirstPass] == consumer.Order);
    MARLIN_EXPECT(SubmitPosition(graph, producer.Segment) < SubmitPosition(graph, consumer.Segment));
    // Graphics closes the graph after every other queue's epilogue.
    const RenderGraphPass &last = graph.Frame.GetKept(static_cast<uint32>(graph.Frame.Kept.Count() - 1));
    MARLIN_EXPECT(last.Queue == RhiQueueType::Graphics && last.Role == RenderGraphPassRole::Epilogue);
    MARLIN_EXPECT(last.Waits[static_cast<uint32>(RhiQueueType::Compute)] != RenderGraphNone);

    RenderGraphExport exported;
    BuildRenderGraphExport(graph.Frame, 7, exported);
    MARLIN_EXPECT(exported.Graph == 7 && exported.Passes.Count() == graph.Frame.Passes.Count());
    MARLIN_REQUIRE(exported.Passes[consume].Waits.Count() == 1);
    MARLIN_EXPECT(exported.Passes[consume].Waits[0] == produce);
    MARLIN_REQUIRE(exported.Passes[produce].BarriersAfter.Count() == 1);
    MARLIN_EXPECT(exported.Passes[produce].BarriersAfter[0].Transfer == "Compute->Graphics");
    MARLIN_EXPECT(exported.Passes[produce].Accesses[0].Access == "CopyDest");

    TestGraph folded(*Device);
    const uint32 buffer = folded.Buffer("Shared", 1024);
    const uint32 first = folded.Pass("Produce", RhiQueueType::Compute);
    folded.Access(first, buffer, RhiAccess::CopyDest, false, true);
    const uint32 second = folded.Pass("Consume", RhiQueueType::Graphics);
    folded.Access(second, buffer, RhiAccess::CopySource, true, false);
    folded.Frame.Passes[second].NeverCull = true;
    folded.Compile(false);
    MARLIN_EXPECT(folded.Get(first).Queue == RhiQueueType::Graphics &&
                  folded.Get(first).DeclaredQueue == RhiQueueType::Compute);
    MARLIN_EXPECT(folded.Get(second).Waits[static_cast<uint32>(RhiQueueType::Compute)] == RenderGraphNone);
    MARLIN_EXPECT(folded.Frame.Segments.Count() == 2);
}

MARLIN_TEST_F(RhiDeviceFixture, RenderGraph, CopiesAcrossThreeQueues, Gpu)
{
    constexpr uint64 Size = 4096;
    RenderGraphExternalBuffer readback{CreateBuffer(Size, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback),
                                       false};
    TestGraph graph(*Device);
    const uint32 filled = graph.Buffer("Filled", Size);
    const uint32 copied = graph.Buffer("Copied", Size);
    const uint32 output = graph.Import("Readback", readback, Size);
    const uint32 fill = graph.Pass("Fill", RhiQueueType::Copy, [&](RenderGraphContext &context) {
        context.List.FillBuffer(graph.GetBuffer(context, filled), 0, Size, 0xA5A55A5Au);
    });
    graph.Access(fill, filled, RhiAccess::CopyDest, false, true);
    const uint32 copy = graph.Pass("Copy", RhiQueueType::Compute, [&](RenderGraphContext &context) {
        context.List.CopyBuffer(graph.GetBuffer(context, copied), 0, graph.GetBuffer(context, filled), 0, Size);
    });
    graph.Access(copy, filled, RhiAccess::CopySource, true, false);
    graph.Access(copy, copied, RhiAccess::CopyDest, false, true);
    const uint32 read = graph.Pass("Read", RhiQueueType::Graphics, [&](RenderGraphContext &context) {
        context.List.CopyBuffer(graph.GetBuffer(context, output), 0, graph.GetBuffer(context, copied), 0, Size);
    });
    graph.Access(read, copied, RhiAccess::CopySource, true, false);
    graph.Access(read, output, RhiAccess::CopyDest, false, true);
    const uint32 host = graph.Pass("Host", RhiQueueType::Graphics);
    graph.Access(host, output, RhiAccess::HostRead, true, false);
    graph.Compile();
    MARLIN_EXPECT(graph.Get(fill).Queue == RhiQueueType::Copy && graph.Get(copy).Queue == RhiQueueType::Compute);
    MARLIN_REQUIRE(graph.Run());

    const auto *words = reinterpret_cast<const uint32 *>(Device->GetMappedData(readback.Buffer));
    MARLIN_REQUIRE(words != nullptr);
    uint32 wrong = 0;
    for (uint64 i = 0; i < Size / 4; ++i)
    {
        wrong += words[i] != 0xA5A55A5Au ? 1 : 0;
    }
    MARLIN_EXPECTF(wrong == 0, "{0} of {1} words differ", wrong, Size / 4);
    Device->Destroy(readback.Buffer);
}

MARLIN_TEST_F(RhiDeviceFixture, RenderGraph, TransitionsTextureMips, Gpu)
{
    RhiTextureDesc desc;
    desc.Format = RhiFormat::RGBA8Unorm;
    desc.Width = 64;
    desc.Height = 64;
    desc.MipCount = 3;
    const uint32 pitch = Max<uint32>(32 * 4, Device->GetLimits().CopyRowPitchAlignment);
    const uint64 size = uint64{pitch} * 32;
    RenderGraphExternalBuffer readback{CreateBuffer(size, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback),
                                       false};
    TestGraph graph(*Device);
    const uint32 texture = graph.Texture("Mips", desc);
    const uint32 output = graph.Import("Readback", readback, size);
    const uint32 clear = graph.Pass("Clear", RhiQueueType::Graphics, [&](RenderGraphContext &context) {
        RhiClearValue value;
        value.Color[0] = 1;
        value.Color[3] = 1;
        context.List.ClearTexture(context.GetTexture(RenderGraphTexture{texture}), {0, 3, 0, 1}, value);
    });
    graph.Access(clear, texture, RhiAccess::CopyDest, false, true);
    const uint32 copy = graph.Pass("CopyMip1", RhiQueueType::Graphics, [&](RenderGraphContext &context) {
        RhiTextureRegion region;
        region.Mip = 1;
        RhiBufferTextureLayout layout;
        layout.RowPitch = pitch;
        context.List.CopyTextureToBuffer(graph.GetBuffer(context, output), layout,
                                         context.GetTexture(RenderGraphTexture{texture}), region);
    });
    graph.Access(copy, texture, RhiAccess::CopySource, true, false, {1, 1, 0, 1});
    graph.Access(copy, output, RhiAccess::CopyDest, false, true);
    const uint32 host = graph.Pass("Host", RhiQueueType::Graphics);
    graph.Access(host, output, RhiAccess::HostRead, true, false);
    graph.Compile();

    // The first use discards all three mips in one barrier; the copy moves only mip 1.
    const RenderGraphPass &clearing = graph.Get(clear);
    MARLIN_REQUIRE(clearing.Before.TextureCount == 1);
    const RhiTextureBarrier &first = graph.Frame.TextureBarriers[clearing.Before.FirstTexture];
    MARLIN_EXPECT(first.Discard && first.LayoutAfter == RhiLayout::CopyDest);
    MARLIN_EXPECT(first.Range.FirstMip == 0 && first.Range.MipCount == 3 && first.Range.SliceCount == 1);
    const RenderGraphPass &copying = graph.Get(copy);
    MARLIN_REQUIRE(copying.Before.TextureCount == 1);
    const RhiTextureBarrier &second = graph.Frame.TextureBarriers[copying.Before.FirstTexture];
    MARLIN_EXPECT(second.LayoutBefore == RhiLayout::CopyDest && second.LayoutAfter == RhiLayout::CopySource);
    MARLIN_EXPECT(second.Range.FirstMip == 1 && second.Range.MipCount == 1);
    MARLIN_EXPECT(HasAnyFlags(graph.Frame.Resources[texture].TextureDesc.Usage, RhiTextureUsage::CopySource));
    MARLIN_REQUIRE(graph.Run());

    const uint8 *bytes = Device->GetMappedData(readback.Buffer);
    MARLIN_REQUIRE(bytes != nullptr);
    uint32 wrong = 0;
    for (uint32 y = 0; y < 32; ++y)
    {
        for (uint32 x = 0; x < 32; ++x)
        {
            const uint8 *texel = bytes + uint64{y} * pitch + x * 4;
            wrong += texel[0] == 255 && texel[1] == 0 && texel[2] == 0 && texel[3] == 255 ? 0 : 1;
        }
    }
    MARLIN_EXPECTF(wrong == 0, "{0} of 1024 texels differ", wrong);
    Device->Destroy(readback.Buffer);
}

MARLIN_TEST_F(RhiDeviceFixture, RenderGraph, AliasesTransientMemory, Gpu)
{
    constexpr uint64 Size = 64 * 1024;
    RenderGraphExternalBuffer first{CreateBuffer(Size, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback), false};
    RenderGraphExternalBuffer second{CreateBuffer(Size, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback), false};
    for (const bool aliasing : {true, false})
    {
        TestGraph graph(*Device);
        const uint32 a = graph.Buffer("A", Size);
        const uint32 b = graph.Buffer("B", Size);
        const uint32 outA = graph.Import("ReadA", first, Size);
        const uint32 outB = graph.Import("ReadB", second, Size);
        const auto addFill = [&](uint32 buffer, uint32 value) {
            const uint32 pass =
                graph.Pass("Fill", RhiQueueType::Graphics, [&graph, buffer, value](RenderGraphContext &c) {
                    c.List.FillBuffer(graph.GetBuffer(c, buffer), 0, Size, value);
                });
            graph.Access(pass, buffer, RhiAccess::CopyDest, false, true);
        };
        const auto addCopy = [&](uint32 source, uint32 target) {
            const uint32 pass =
                graph.Pass("Copy", RhiQueueType::Graphics, [&graph, source, target](RenderGraphContext &c) {
                    c.List.CopyBuffer(graph.GetBuffer(c, target), 0, graph.GetBuffer(c, source), 0, Size);
                });
            graph.Access(pass, source, RhiAccess::CopySource, true, false);
            graph.Access(pass, target, RhiAccess::CopyDest, false, true);
        };
        addFill(a, 1);
        addCopy(a, outA);
        addFill(b, 2);
        addCopy(b, outB);
        const uint32 host = graph.Pass("Host", RhiQueueType::Graphics);
        graph.Access(host, outA, RhiAccess::HostRead, true, false);
        graph.Access(host, outB, RhiAccess::HostRead, true, false);
        graph.Compile();
        MARLIN_REQUIRE(graph.Run(aliasing));

        const RenderGraphResource &resourceA = graph.Frame.Resources[a];
        const RenderGraphResource &resourceB = graph.Frame.Resources[b];
        MARLIN_REQUIRE(resourceA.Heap != RenderGraphNone && resourceB.Heap != RenderGraphNone);
        const bool shared = resourceA.Heap == resourceB.Heap && resourceA.Offset == resourceB.Offset;
        MARLIN_EXPECTF(shared == aliasing, "aliasing {0}: A at {1}+{2}, B at {3}+{4}", aliasing, resourceA.Heap,
                       resourceA.Offset, resourceB.Heap, resourceB.Offset);
        MARLIN_EXPECT(graph.Frame.UnaliasedBytes >= 2 * Size);
        const auto *wordsA = reinterpret_cast<const uint32 *>(Device->GetMappedData(first.Buffer));
        const auto *wordsB = reinterpret_cast<const uint32 *>(Device->GetMappedData(second.Buffer));
        uint32 wrong = 0;
        for (uint64 i = 0; i < Size / 4; ++i)
        {
            wrong += wordsA[i] != 1 || wordsB[i] != 2 ? 1 : 0;
        }
        MARLIN_EXPECTF(wrong == 0, "aliasing {0}: {1} words differ", aliasing, wrong);
    }
    Device->Destroy(first.Buffer);
    Device->Destroy(second.Buffer);
}

namespace
{
/// An entry point of *source* compiled the way the shader pipeline compiles global shaders.
List<uint8> CompileTestShader(StringView source, StringView entryPoint)
{
    static constexpr StringView Roots[] = {"/Engine"};
    VfsSourceProvider engine{Roots};
    MemorySourceProvider memory;
    memory.Add("/Project/Language/Shaders/Test.mshader", source);
    ISourceProvider *providers[] = {&engine, &memory};
    CompositeSourceProvider sources{Span<ISourceProvider *const>(providers)};
    LanguageCompileOptions options;
    LanguageCompilation compilation(sources, options);
    MirModule module;
    List<uint8> code;
    if (!compilation.Load() || !compilation.CompileEntryPoint(entryPoint, module))
    {
        Testing::Fail("the shader compiles", __FILE__, __LINE__, compilation.GetDiagnostics().ToString().AsView());
        return code;
    }
    MirTargetRegistry targets;
    RegisterSpirvTarget(targets);
    const MirTarget *target = targets.Find(SpirvTargetName);
    const MirPassRegistry registry = MirPassRegistry::CreateDefault();
    MirModulePassManager passes;
    String error;
    registry.ParsePipeline(MirShaderPipeline, passes, error);
    List<String> errors;
    target->AddLegalizationPasses(passes, errors);
    MirModuleAnalysisManager analyses;
    passes.Run(module, analyses);
    if (!errors.IsEmpty() || !passes.GetErrors().IsEmpty() || !target->Emit(module, {}, code, errors))
    {
        Testing::Fail("the shader compiles", __FILE__, __LINE__, entryPoint);
        code.Clear();
    }
    return code;
}

constexpr StringView RasterSource = R"(
namespace Test;

public struct Varyings { [Position] public float4 Position; public float4 Color; }
public struct VisibilityArgs { public Ptr<ulong, Access.ReadWrite> Visibility; public uint Width; }

/// A triangle that covers the whole render area at depth 0.5.
[Mesh(1, 1, 1, MaxVertices = 3, Topology = MeshTopology.Triangle)]
void Cover(out Vertices<Varyings, 3> verts, out Indices<uint3, 1> tris)
{
    SetMeshOutputCounts(3, 1);
    for (uint i = 0; i < 3; ++i)
    {
        float2 uv = float2((float)((i << 1) & 2), (float)(i & 2));
        Varyings v;
        v.Position = float4(uv * 2 - 1, 0.5, 1);
        v.Color = float4(1, 0, 0, 1);
        verts[i] = v;
    }
    tris[0] = uint3(0, 1, 2);
}

[Pixel] [return: Target(0)]
float4 Shade(in Varyings v)
{
    return v.Color;
}

/// Depth in the high word, the pixel in the low one: what a visibility buffer keeps of the nearest surface.
[Pixel]
void Visibility(in Varyings v, [Uniform] in VisibilityArgs args)
{
    uint2 pixel = uint2(v.Position.xy);
    ulong packed = ((ulong)AsUInt(v.Position.z) << 32) | (ulong)(pixel.y * 256 + pixel.x);
    Atomic.Max(ref args.Visibility[pixel.y * args.Width + pixel.x], packed);
}
)";
} // namespace

MARLIN_TEST_F(RhiDeviceFixture, RenderGraph, RastersMeshShadersIntoTargetsAndAtomics, Gpu)
{
    constexpr uint32 Width = 64;
    constexpr uint32 Height = 32;
    const List<uint8> cover = CompileTestShader(RasterSource, "Test.Cover");
    const List<uint8> shade = CompileTestShader(RasterSource, "Test.Shade");
    const List<uint8> visibility = CompileTestShader(RasterSource, "Test.Visibility");
    MARLIN_REQUIRE(!cover.IsEmpty() && !shade.IsEmpty() && !visibility.IsEmpty());

    const auto makePipeline = [&](const List<uint8> &pixel, StringView pixelEntry, Span<const RhiFormat> formats) {
        const RhiShader shaders[] = {{cover.AsSpan(), "Test.Cover", RhiShaderStage::Mesh, {}},
                                     {pixel.AsSpan(), pixelEntry, RhiShaderStage::Pixel, {}}};
        RhiGraphicsPipelineDesc desc;
        desc.Shaders = shaders;
        desc.Raster.Cull = RhiCullMode::None;
        desc.RenderTargetFormats = formats;
        desc.DebugName = pixelEntry;
        const RhiPipeline pipeline = Device->CreatePipeline(desc);
        MARLIN_REQUIRE(Device->WaitForPipeline(pipeline) == RhiPipelineState::Ready);
        return pipeline;
    };
    const RhiFormat colorFormat = RhiFormat::RGBA8Unorm;
    const RhiPipeline shadePipeline = makePipeline(shade, "Test.Shade", Span<const RhiFormat>(&colorFormat, 1));
    const RhiPipeline visibilityPipeline = makePipeline(visibility, "Test.Visibility", {});

    constexpr uint64 VisibilitySize = uint64{Width} * Height * sizeof(uint64);
    RenderGraphExternalBuffer visibilityBuffer{
        CreateBuffer(VisibilitySize, RhiBufferUsage::UnorderedAccess | RhiBufferUsage::CopySource | RhiBufferUsage::CopyDest,
                     RhiMemoryLocation::Device),
        false};
    const uint32 pitch = Max<uint32>(Width * 4, Device->GetLimits().CopyRowPitchAlignment);
    const uint64 colorSize = uint64{pitch} * Height;
    RenderGraphExternalBuffer colorReadback{
        CreateBuffer(colorSize, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback), false};
    RenderGraphExternalBuffer visibilityReadback{
        CreateBuffer(VisibilitySize, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback), false};

    TestGraph graph(*Device);
    RhiTextureDesc colorDesc;
    colorDesc.Format = colorFormat;
    colorDesc.Width = Width;
    colorDesc.Height = Height;
    const uint32 color = graph.Texture("Color", colorDesc);
    const uint32 visible = graph.Import("Visibility", visibilityBuffer, VisibilitySize);
    const uint32 colorOut = graph.Import("ColorReadback", colorReadback, colorSize);
    const uint32 visibleOut = graph.Import("VisibilityReadback", visibilityReadback, VisibilitySize);

    // A pass with a color target: the executor begins rendering and sets the viewport to the target.
    const uint32 shadePass = graph.Pass("Shade", RhiQueueType::Graphics, [&](RenderGraphContext &context) {
        context.List.SetPipeline(shadePipeline);
        context.List.DrawMesh(1, 1, 1);
    });
    RenderGraphRenderTarget target;
    target.Texture = RenderGraphTexture{color};
    target.Load = RhiLoadOp::Clear;
    target.Clear = RhiClearValue{};
    graph.Frame.Passes[shadePass].FirstTarget = static_cast<uint32>(graph.Frame.Targets.Count());
    graph.Frame.Passes[shadePass].TargetCount = 1;
    graph.Frame.Targets.Add(target);
    graph.Access(shadePass, color, RhiAccess::RenderTarget, false, true);

    const uint32 clearPass = graph.Pass("ClearVisibility", RhiQueueType::Graphics, [&](RenderGraphContext &context) {
        context.List.FillBuffer(graph.GetBuffer(context, visible), 0, VisibilitySize, 0);
    });
    graph.Access(clearPass, visible, RhiAccess::CopyDest, false, true);
    // A pass with only a render area: it renders without attachments and writes through atomics.
    const uint32 rasterPass = graph.Pass("Visibility", RhiQueueType::Graphics, [&](RenderGraphContext &context) {
        context.List.SetPipeline(visibilityPipeline);
        // The visibility buffer as a `Ptr<ulong>` (MarlinLanguage.md §4.5), then the width.
        const uint64 arguments[] = {context.GetBufferAddress(RenderGraphBuffer{visible}), Width};
        context.List.SetShaderArguments(Span<const uint8>(reinterpret_cast<const uint8 *>(arguments), sizeof(arguments)));
        context.List.DrawMesh(1, 1, 1);
    });
    graph.Frame.Passes[rasterPass].Width = Width;
    graph.Frame.Passes[rasterPass].Height = Height;
    graph.Access(rasterPass, visible, RhiAccess::UnorderedAccess, true, true);

    const uint32 copyPass = graph.Pass("Copy", RhiQueueType::Graphics, [&](RenderGraphContext &context) {
        RhiBufferTextureLayout layout;
        layout.RowPitch = pitch;
        context.List.CopyTextureToBuffer(graph.GetBuffer(context, colorOut), layout,
                                         context.GetTexture(RenderGraphTexture{color}), RhiTextureRegion{});
        context.List.CopyBuffer(graph.GetBuffer(context, visibleOut), 0, graph.GetBuffer(context, visible), 0,
                                VisibilitySize);
    });
    graph.Access(copyPass, color, RhiAccess::CopySource, true, false);
    graph.Access(copyPass, visible, RhiAccess::CopySource, true, false);
    graph.Access(copyPass, colorOut, RhiAccess::CopyDest, false, true);
    graph.Access(copyPass, visibleOut, RhiAccess::CopyDest, false, true);
    const uint32 host = graph.Pass("Host", RhiQueueType::Graphics);
    graph.Access(host, colorOut, RhiAccess::HostRead, true, false);
    graph.Access(host, visibleOut, RhiAccess::HostRead, true, false);
    graph.Compile();
    MARLIN_EXPECT(graph.Get(rasterPass).Renders());
    MARLIN_REQUIRE(graph.Run());

    const uint8 *texels = Device->GetMappedData(colorReadback.Buffer);
    const auto *packed = reinterpret_cast<const uint64 *>(Device->GetMappedData(visibilityReadback.Buffer));
    MARLIN_REQUIRE(texels != nullptr && packed != nullptr);
    uint32 wrongColor = 0;
    uint32 wrongVisibility = 0;
    for (uint32 y = 0; y < Height; ++y)
    {
        for (uint32 x = 0; x < Width; ++x)
        {
            const uint8 *texel = texels + uint64{y} * pitch + x * 4;
            wrongColor += texel[0] == 255 && texel[1] == 0 && texel[2] == 0 && texel[3] == 255 ? 0 : 1;
            const uint64 expected = (uint64{0x3F000000u} << 32) | (y * 256 + x);
            wrongVisibility += packed[y * Width + x] == expected ? 0 : 1;
        }
    }
    MARLIN_EXPECTF(wrongColor == 0, "{0} of {1} texels are not the triangle's color", wrongColor, Width * Height);
    MARLIN_EXPECTF(wrongVisibility == 0, "{0} of {1} visibility values differ (first {2:x})", wrongVisibility,
                   Width * Height, packed[0]);
    Device->Destroy(shadePipeline);
    Device->Destroy(visibilityPipeline);
    Device->Destroy(visibilityBuffer.Buffer);
    Device->Destroy(colorReadback.Buffer);
    Device->Destroy(visibilityReadback.Buffer);
}
} // namespace Marlin::Tests
