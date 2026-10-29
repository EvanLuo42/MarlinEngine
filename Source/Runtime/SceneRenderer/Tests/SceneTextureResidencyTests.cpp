#include "Testing/Testing.h"

#include <initializer_list>

#include "Asset/AssetBlock.h"
#include "Asset/Assets.h"
#include "Asset/BulkStreamer.h"
#include "Containers/HashMap.h"
#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirReflection.h"
#include "LanguageIR/MirTarget.h"
#include "Memory/MemoryOps.h"
#include "RHI/RhiShaderTypes.h"
#include "RHI/RhiTestDevice.h"
#include "SceneRenderer/SceneTextureResidency.h"
#include "ShaderBackendSpirv/SpirvTarget.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Texture/Texture.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
namespace
{
constexpr StringView TextureTestShader = R"(
namespace Test;

public struct DefaultsArgs
{
    public Span<float4, Access.ReadWrite> Output;
    public Texture2D<float4> White;
    public Texture2D<float4> Black;
    public Texture2D<float4> FlatNormal;
}

[Compute(1, 1, 1)]
void Defaults([DispatchThreadId] uint3 id, [Uniform] in DefaultsArgs args)
{
    args.Output[0] = args.White.Load(int2(0, 0), 0);
    args.Output[1] = args.Black.Load(int2(0, 0), 0);
    args.Output[2] = args.FlatNormal.Load(int2(0, 0), 0);
}

public struct MipsArgs
{
    public Span<float4, Access.ReadWrite> Output;
    public Texture2D<float4> Linear;
    public Texture2D<float4> Srgb;
    public Sampler Sampler;
}

[Compute(4, 4, 1)]
void Mips([DispatchThreadId] uint3 id, [Uniform] in MipsArgs args)
{
    int2 texel = int2((int)id.x, (int)id.y);
    args.Output[id.y * 4 + id.x] = args.Linear.Load(texel, 0);
    if (id.x < 2 && id.y < 2)
    {
        args.Output[16 + id.y * 2 + id.x] = args.Linear.Load(texel, 1);
    }
    if (id.x == 0 && id.y == 0)
    {
        // Texel centers: mip 0's (1, 2) and mip 1's (0, 0), which filter to exactly those texels.
        args.Output[20] = args.Linear.SampleLevel(args.Sampler, float2(0.375, 0.625), 0.0);
        args.Output[21] = args.Linear.SampleLevel(args.Sampler, float2(0.25, 0.25), 1.0);
        args.Output[22] = args.Srgb.Load(int2(1, 2), 0);
        args.Output[23] = float4((float)args.Linear.MipCount, 0.0, 0.0, 0.0);
    }
}

public struct ProbeArgs
{
    public Span<float4, Access.ReadWrite> Output;
    public Texture2D<float4> Texture;
}

/// Texel (0, 0) of each of the texture's mips, then its mip count.
[Compute(1, 1, 1)]
void Probe([DispatchThreadId] uint3 id, [Uniform] in ProbeArgs args)
{
    uint count = args.Texture.MipCount;
    for (uint mip = 0; mip < 9; ++mip)
    {
        args.Output[mip] = mip < count ? args.Texture.Load(int2(0, 0), (int)mip) : float4(-1.0, -1.0, -1.0, -1.0);
    }
    args.Output[9] = float4((float)count, 0.0, 0.0, 0.0);
}
)";

/// An entry point of TextureTestShader compiled for the fixture's device, run once on *groups* groups with
/// *arguments*, and the float4s it wrote to its Output read back.
struct TextureKernelRun
{
    template <typename TArguments>
    TextureKernelRun(RhiDeviceFixture &fixture, StringView entryPoint, TArguments arguments, uint32 outputs)
    {
        static constexpr StringView Roots[] = {"/Engine"};
        VfsSourceProvider engine{Roots};
        MemorySourceProvider memory;
        memory.Add("/Project/Language/Shaders/Test.mshader", TextureTestShader);
        ISourceProvider *providers[] = {&engine, &memory};
        CompositeSourceProvider sources{Span<ISourceProvider *const>(providers)};
        const LanguageCompileOptions options;
        LanguageCompilation compilation(sources, options);
        MirModule module;
        MARLIN_REQUIREF(compilation.Load() && compilation.CompileEntryPoint(entryPoint, module), "{0}",
                        compilation.GetDiagnostics().ToString());
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
        List<uint8> code;
        MARLIN_REQUIRE(passes.GetErrors().IsEmpty() && errors.IsEmpty() && target->Emit(module, {}, code, errors));
        MirReflection reflection;
        ReflectMirEntryPoint(module, *module.GetEntryPoints()[0], reflection);
        MARLIN_REQUIREF(reflection.ArgumentsSize <= sizeof(TArguments), "{0} bytes of arguments",
                        reflection.ArgumentsSize);

        RhiDevice &device = *fixture.Device;
        RhiComputePipelineDesc pipelineDesc;
        pipelineDesc.Shader = {code.AsSpan(), entryPoint, RhiShaderStage::Compute, {}};
        const RhiPipeline pipeline = device.CreatePipeline(pipelineDesc);
        MARLIN_REQUIRE(device.WaitForPipeline(pipeline) == RhiPipelineState::Ready);
        const RhiBuffer output =
            fixture.CreateBuffer(uint64{outputs} * 16, RhiBufferUsage::ShaderResource | RhiBufferUsage::UnorderedAccess,
                                 RhiMemoryLocation::Readback);
        arguments.Output = {.Pointer = device.GetBufferAddress(output), .Length = outputs};
        RhiCommandList *list = device.BeginCommandList(RhiQueueType::Graphics);
        list->SetPipeline(pipeline);
        list->SetShaderArguments(
            Span<const uint8>(reinterpret_cast<const uint8 *>(&arguments), reflection.ArgumentsSize));
        list->Dispatch(1, 1, 1);
        fixture.SubmitAndWait(list);
        const auto *values = reinterpret_cast<const float32 *>(device.GetMappedData(output));
        Values.AddRange(Span<const float32>(values, uint64{outputs} * 4));
        device.Destroy(pipeline);
        device.Destroy(output);
    }

    /// True if output *index* is (r, g, b, a) within a unorm8 step.
    [[nodiscard]] bool Near(uint32 index, float32 r, float32 g, float32 b, float32 a) const
    {
        const float32 expected[] = {r, g, b, a};
        bool near = true;
        for (uint32 channel = 0; channel < 4; ++channel)
        {
            const float32 difference = Values[index * 4 + channel] - expected[channel];
            near = near && difference < 0.5f / 255 && difference > -0.5f / 255;
        }
        return near;
    }

    List<float32> Values;
};

struct DefaultsArguments
{
    ShaderSpan Output;
    uint32 White = 0;
    uint32 Black = 0;
    uint32 FlatNormal = 0;
};

struct ProbeArguments
{
    ShaderSpan Output;
    uint32 Texture = 0;
};

struct MipsArguments
{
    ShaderSpan Output;
    uint32 Linear = 0;
    uint32 Srgb = 0;
    uint32 Sampler = 0;
};

/// Records what Extract handed over into one Graphics list and waits for it, as the frame's upload pass would.
void UploadAndWait(RhiDeviceFixture &fixture, SceneTextureResidency &residency)
{
    RhiCommandList *list = fixture.Device->BeginCommandList(RhiQueueType::Graphics);
    residency.RecordUploads(*list);
    fixture.SubmitAndWait(list);
}

void Mip0Texel(uint32 x, uint32 y, uint8 (&rgba)[4])
{
    rgba[0] = static_cast<uint8>(x * 60);
    rgba[1] = static_cast<uint8>(y * 60);
    rgba[2] = static_cast<uint8>((x + y) * 20);
    rgba[3] = 255;
}

void Mip1Texel(uint32 x, uint32 y, uint8 (&rgba)[4])
{
    rgba[0] = static_cast<uint8>(200 - x * 50);
    rgba[1] = static_cast<uint8>(10 + y * 100);
    rgba[2] = 77;
    rgba[3] = 128;
}

float32 DecodeSrgb(uint8 value)
{
    const float32 encoded = static_cast<float32>(value) / 255;
    return encoded <= 0.04045f ? encoded / 12.92f : __builtin_powf((encoded + 0.055f) / 1.055f, 2.4f);
}

/// The red of every texel of mip *mip* of the streamed test texture, so a probe tells the mips apart.
uint8 StreamedRed(uint32 mip)
{
    return static_cast<uint8>(20 + mip * 25);
}

/// Serves texture blocks written under /Temp, all with one bulk file.
class TextureTestStore final : public IAssetStore
{
public:
    TextureTestStore(HashMap<Guid, String> blocks, String bulk) : _blocks(Move(blocks)), _bulk(Move(bulk))
    {
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        const String *path = _blocks.Find(guid);
        AssetLocation location;
        const bool found = path != nullptr && Vfs::OpenRegion(path->AsView(), location.Block) &&
                           Vfs::OpenRegion(_bulk.AsView(), location.Bulk);
        done(found ? &location : nullptr);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "Scene texture test";
    }

private:
    HashMap<Guid, String> _blocks;
    String _bulk;
};

/// Two 4x4 RGBA8 textures of two mips sharing their texels, one linear and one sRGB, as converted assets in
/// /Temp/SceneTextureResidencyTests, served to the asset table it starts and shuts down.
class TextureTestContent
{
public:
    static inline const Guid LinearGuid{0x7E87E801u, 0, 0, 1};
    static inline const Guid SrgbGuid{0x7E87E801u, 0, 0, 2};
    /// 256x256 RGBA8 with all nine mips, each texel (StreamedRed(mip), x, y, 255): more mips than the tail.
    static inline const Guid StreamedGuid{0x7E87E801u, 0, 0, 3};

    TextureTestContent()
    {
        Assets::Initialize();
        (void)Vfs::Delete(Directory);
        MARLIN_REQUIRE(Vfs::CreateDirectory(Directory));
        List<uint8> bulk;
        for (uint32 y = 0; y < 4; ++y)
        {
            for (uint32 x = 0; x < 4; ++x)
            {
                uint8 texel[4];
                Mip0Texel(x, y, texel);
                bulk.AddRange(Span<const uint8>(texel, 4));
            }
        }
        for (uint32 y = 0; y < 2; ++y)
        {
            for (uint32 x = 0; x < 2; ++x)
            {
                uint8 texel[4];
                Mip1Texel(x, y, texel);
                bulk.AddRange(Span<const uint8>(texel, 4));
            }
        }
        // The streamed texture's mips follow the small textures' in the one bulk file.
        const uint64 streamedOffset = bulk.Count();
        List<TextureMip> streamedMips;
        for (uint32 mip = 0, size = 256; size > 0; ++mip, size /= 2)
        {
            TextureMip &level = streamedMips.AddDefaulted();
            level.Width = size;
            level.Height = size;
            level.RowPitch = size * 4;
            level.Offset = bulk.Count();
            level.Size = uint64{size} * size * 4;
            for (uint32 y = 0; y < size; ++y)
            {
                for (uint32 x = 0; x < size; ++x)
                {
                    const uint8 texel[4] = {StreamedRed(mip), static_cast<uint8>(x), static_cast<uint8>(y), 255};
                    bulk.AddRange(Span<const uint8>(texel, 4));
                }
            }
        }
        (void)streamedOffset;
        const String bulkPath = Format("{0}/Texels.bulk", Directory);
        MARLIN_REQUIRE(
            Vfs::WriteAll(bulkPath.AsView(), StringView(reinterpret_cast<const char *>(bulk.Data()), bulk.Count())));
        const TextureMip mips[] = {{.Width = 4, .Height = 4, .RowPitch = 16, .Offset = 0, .Size = 64},
                                   {.Width = 2, .Height = 2, .RowPitch = 8, .Offset = 64, .Size = 16}};
        HashMap<Guid, String> blocks;
        for (const Guid &guid : {LinearGuid, SrgbGuid, StreamedGuid})
        {
            const bool streamed = guid == StreamedGuid;
            Texture texture;
            texture.Format = TextureFormat::RGBA8;
            texture.ColorSpace = guid == SrgbGuid ? TextureColorSpace::Srgb : TextureColorSpace::Linear;
            texture.Width = streamed ? 256 : 4;
            texture.Height = streamed ? 256 : 4;
            texture.Mips = streamed ? FrozenArray<TextureMip>(streamedMips.AsSpan())
                                    : FrozenArray<TextureMip>(Span<const TextureMip>(mips));
            List<uint8> block;
            MARLIN_REQUIRE(AssetBlock::Write(texture, guid, block, bulk.Count()));
            StringBuilder<256> path;
            FormatTo(path, "{0}/", Directory);
            guid.FormatTo(path, "N");
            path.Append(".mablk");
            MARLIN_REQUIRE(
                Vfs::WriteAll(path.AsView(), StringView(reinterpret_cast<const char *>(block.Data()), block.Count())));
            blocks.Set(guid, String(path.AsView()));
        }
        _store = Assets::AddStore(MakeUnique<TextureTestStore>(Move(blocks), bulkPath), 0);
    }

    ~TextureTestContent()
    {
        Assets::RemoveStore(_store);
        Assets::Shutdown();
        (void)Vfs::Delete(Directory);
    }

    MARLIN_NONCOPYABLE(TextureTestContent);

private:
    static constexpr StringView Directory = "/Temp/SceneTextureResidencyTests";
    AssetStoreId _store = 0;
};
} // namespace

MARLIN_TEST_F(RhiDeviceFixture, SceneRenderer, DefaultTexturesHoldTheirColors, Gpu)
{
    SceneTextureResidency residency(*Device, nullptr);
    residency.Extract();
    UploadAndWait(*this, residency);
    DefaultsArguments arguments;
    arguments.White = residency.GetDefaultTexture(SceneDefaultTexture::White);
    arguments.Black = residency.GetDefaultTexture(SceneDefaultTexture::Black);
    arguments.FlatNormal = residency.GetDefaultTexture(SceneDefaultTexture::FlatNormal);
    MARLIN_EXPECT(arguments.White != arguments.Black && arguments.Black != arguments.FlatNormal);
    const TextureKernelRun run(*this, "Test.Defaults", arguments, 3);
    MARLIN_EXPECTF(run.Near(0, 1, 1, 1, 1), "white {0} {1} {2} {3}", run.Values[0], run.Values[1], run.Values[2],
                   run.Values[3]);
    MARLIN_EXPECTF(run.Near(1, 0, 0, 0, 1), "black {0} {1} {2} {3}", run.Values[4], run.Values[5], run.Values[6],
                   run.Values[7]);
    MARLIN_EXPECTF(run.Near(2, 128.0f / 255, 128.0f / 255, 1, 1), "flat normal {0} {1} {2} {3}", run.Values[8],
                   run.Values[9], run.Values[10], run.Values[11]);
}

MARLIN_TEST_F(RhiDeviceFixture, SceneRenderer, TextureResidencyUploadsEveryMip, Gpu)
{
    const TextureTestContent content;
    {
        const AssetRef<Texture> linear = Assets::LoadSync<Texture>(TextureTestContent::LinearGuid);
        const AssetRef<Texture> srgb = Assets::LoadSync<Texture>(TextureTestContent::SrgbGuid);
        MARLIN_REQUIRE(linear.IsLoaded() && srgb.IsLoaded());
        BulkStreamer streamer;
        SceneTextureResidency residency(*Device, &streamer);
        const uint32 white = residency.GetDefaultTexture(SceneDefaultTexture::White);

        // White until uploaded: read, created and handed to a frame, then resident from the next.
        MARLIN_EXPECT(residency.AcquireTexture(linear) == white);
        MARLIN_EXPECT(residency.AcquireTexture(srgb) == white);
        residency.Update();
        streamer.Flush();
        MARLIN_EXPECT(residency.GetTextureIndex(linear) == white);
        residency.Extract();
        UploadAndWait(*this, residency);
        MARLIN_EXPECT(residency.GetTextureIndex(linear) == white && residency.GetResidentStamp() == 0);
        residency.Extract();
        MARLIN_EXPECT(residency.GetResidentStamp() == 2);
        MipsArguments arguments;
        arguments.Linear = residency.AcquireTexture(linear);
        arguments.Srgb = residency.GetTextureIndex(srgb);
        arguments.Sampler = residency.GetMaterialSampler();
        MARLIN_REQUIRE(arguments.Linear != white && arguments.Srgb != white && arguments.Linear != arguments.Srgb);
        MARLIN_EXPECT(residency.GetTextureIndex(linear) == arguments.Linear);

        const TextureKernelRun run(*this, "Test.Mips", arguments, 24);
        uint32 wrong = 0;
        for (uint32 y = 0; y < 4; ++y)
        {
            for (uint32 x = 0; x < 4; ++x)
            {
                uint8 texel[4];
                Mip0Texel(x, y, texel);
                wrong += run.Near(y * 4 + x, texel[0] / 255.0f, texel[1] / 255.0f, texel[2] / 255.0f,
                                  texel[3] / 255.0f)
                             ? 0
                             : 1;
            }
        }
        MARLIN_EXPECTF(wrong == 0, "{0} of 16 texels of mip 0 differ", wrong);
        wrong = 0;
        for (uint32 y = 0; y < 2; ++y)
        {
            for (uint32 x = 0; x < 2; ++x)
            {
                uint8 texel[4];
                Mip1Texel(x, y, texel);
                wrong += run.Near(16 + y * 2 + x, texel[0] / 255.0f, texel[1] / 255.0f, texel[2] / 255.0f,
                                  texel[3] / 255.0f)
                             ? 0
                             : 1;
            }
        }
        MARLIN_EXPECTF(wrong == 0, "{0} of 4 texels of mip 1 differ", wrong);
        uint8 texel[4];
        Mip0Texel(1, 2, texel);
        MARLIN_EXPECTF(run.Near(20, texel[0] / 255.0f, texel[1] / 255.0f, texel[2] / 255.0f, 1), "sampled mip 0 {0} {1}",
                       run.Values[80], run.Values[81]);
        MARLIN_EXPECTF(run.Near(22, DecodeSrgb(texel[0]), DecodeSrgb(texel[1]), DecodeSrgb(texel[2]), 1),
                       "sRGB {0} {1} {2}", run.Values[88], run.Values[89], run.Values[90]);
        Mip1Texel(0, 0, texel);
        MARLIN_EXPECTF(run.Near(21, texel[0] / 255.0f, texel[1] / 255.0f, texel[2] / 255.0f, texel[3] / 255.0f),
                       "sampled mip 1 {0} {1}", run.Values[84], run.Values[85]);
        MARLIN_EXPECTF(run.Values[92] == 2, "{0} mips", run.Values[92]);

        // Released by every holder, it samples as white again and its descriptor goes after RetireFrames.
        residency.ReleaseTexture(linear);
        residency.ReleaseTexture(linear);
        residency.ReleaseTexture(srgb);
        residency.Update();
        MARLIN_EXPECT(residency.GetTextureIndex(linear) == white);
        // Asked for again while retiring, it is resident at once.
        MARLIN_EXPECT(residency.AcquireTexture(srgb) == arguments.Srgb);
        residency.ReleaseTexture(srgb);
        for (uint32 frame = 0; frame <= SceneTextureResidency::RetireFrames; ++frame)
        {
            residency.Update();
        }
        MARLIN_EXPECT(residency.GetTextureIndex(linear) == white && residency.GetTextureIndex(srgb) == white);
    }
}
namespace
{
/// One frame of streaming as SceneRendererSubsystem drives it: Update, reads done, the uploads handed over and recorded,
/// and the next Extract making them resident.
void StreamFrame(RhiDeviceFixture &fixture, SceneTextureResidency &residency, BulkStreamer &streamer,
                 const SceneTextureSettings &settings)
{
    residency.Update(settings);
    streamer.Flush();
    residency.Extract();
    UploadAndWait(fixture, residency);
    residency.Extract();
}

/// The resident mips of *texture* as the probe sees them: true if each holds the mip it should, from *first* on.
bool HoldsMips(RhiDeviceFixture &fixture, SceneTextureResidency &residency, const AssetRef<Texture> &texture,
               uint32 first)
{
    ProbeArguments arguments;
    arguments.Texture = residency.GetTextureIndex(texture);
    const TextureKernelRun run(fixture, "Test.Probe", arguments, 10);
    bool holds = run.Values[36] == static_cast<float32>(9 - first);
    for (uint32 mip = first; mip < 9; ++mip)
    {
        holds = holds && run.Near(mip - first, StreamedRed(mip) / 255.0f, 0, 0, 1);
    }
    return holds;
}
} // namespace

MARLIN_TEST_F(RhiDeviceFixture, SceneRenderer, TexturesStreamTheMipsTheirSamplesNeed, Gpu)
{
    const TextureTestContent content;
    {
        const AssetRef<Texture> texture = Assets::LoadSync<Texture>(TextureTestContent::StreamedGuid);
        MARLIN_REQUIRE(texture.IsLoaded());
        BulkStreamer streamer;
        SceneTextureResidency residency(*Device, &streamer);
        SceneTextureSettings settings;
        (void)residency.AcquireTexture(texture);
        MARLIN_EXPECT(residency.GetFeedbackAddress(texture) != 0);

        // The tail first: mips 2 (64x64) to 8.
        StreamFrame(*this, residency, streamer, settings);
        MARLIN_REQUIRE(residency.GetFirstResidentMip(texture) == 2);
        MARLIN_EXPECT(HoldsMips(*this, residency, texture, 2));
        MARLIN_EXPECT(residency.GetResidentBytes() == (64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1) * 4);

        // A sample needed 256 texels across: mips 0 and 1 are read, the tail copied over on the GPU.
        const uint32 stamp = residency.GetResidentStamp();
        residency.NoteNeeded(texture, 8);
        StreamFrame(*this, residency, streamer, settings);
        MARLIN_REQUIRE(residency.GetFirstResidentMip(texture) == 0);
        MARLIN_EXPECT(residency.GetResidentStamp() > stamp);
        MARLIN_EXPECT(HoldsMips(*this, residency, texture, 0));

        // Over the pool, it gives up a mip a frame down to its tail, and no further.
        settings.PoolSize = 1;
        StreamFrame(*this, residency, streamer, settings);
        MARLIN_EXPECT(residency.GetFirstResidentMip(texture) == 1);
        MARLIN_EXPECT(HoldsMips(*this, residency, texture, 1));
        for (uint32 frame = 0; frame < 3; ++frame)
        {
            StreamFrame(*this, residency, streamer, settings);
        }
        MARLIN_EXPECT(residency.GetFirstResidentMip(texture) == 2);
        MARLIN_EXPECT(HoldsMips(*this, residency, texture, 2));

        // Streaming off: whole, whatever the pool.
        settings.Streaming = false;
        StreamFrame(*this, residency, streamer, settings);
        MARLIN_EXPECT(residency.GetFirstResidentMip(texture) == 0);
        MARLIN_EXPECT(HoldsMips(*this, residency, texture, 0));

        // Back on, a sample that needs 64 texels across lets mips 0 and 1 go, once RelaxFrames have passed.
        settings = SceneTextureSettings();
        residency.NoteNeeded(texture, 6);
        StreamFrame(*this, residency, streamer, settings);
        MARLIN_EXPECT(residency.GetFirstResidentMip(texture) == 0);
        for (uint32 frame = 0; frame <= SceneTextureResidency::RelaxFrames; ++frame)
        {
            residency.Update(settings);
        }
        residency.NoteNeeded(texture, 6);
        StreamFrame(*this, residency, streamer, settings);
        MARLIN_EXPECT(residency.GetFirstResidentMip(texture) == 2);
        MARLIN_EXPECT(HoldsMips(*this, residency, texture, 2));

        // A mip bias streams coarser: 256 texels needed, biased by one, wants mip 1.
        settings.MipBias = 1;
        residency.Update(settings);
        residency.NoteNeeded(texture, 8);
        StreamFrame(*this, residency, streamer, settings);
        MARLIN_EXPECT(residency.GetFirstResidentMip(texture) == 1);
        residency.ReleaseTexture(texture);
    }
}
} // namespace Marlin::Tests
