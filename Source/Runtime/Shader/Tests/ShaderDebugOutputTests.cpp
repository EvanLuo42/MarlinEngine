#include "Testing/Testing.h"

#include <initializer_list>

#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirReflection.h"
#include "LanguageIR/MirTarget.h"
#include "Math/Float16.h"
#include "Memory/MemoryOps.h"
#include "String/StringBuilder.h"
#include "Memory/MemoryOps.h"
#include "RHI/RhiTestDevice.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderDebugOutput.h"
#include "ShaderBackendSpirv/SpirvTarget.h"

namespace Marlin::Tests
{
namespace
{
/// A debug output buffer built by hand: records appended after the two header words.
struct DebugBuffer
{
    explicit DebugBuffer(uint32 capacity)
    {
        Words.Add(0);
        Words.Add(capacity);
    }

    void Record(StringView format, std::initializer_list<uint32> arguments)
    {
        Words.Add(static_cast<uint32>(2 + arguments.size()));
        Words.Add(GetShaderDebugFormatHash(format));
        for (const uint32 word : arguments)
        {
            Words.Add(word);
        }
        Words[ShaderDebugCount] = static_cast<uint32>(Words.Count() - ShaderDebugHeaderWords);
    }

    List<uint32> Words;
};

constexpr uint32 Descriptor(uint32 kind, uint32 bits, uint32 components)
{
    return kind | (bits << 8) | (components << 16);
}

constexpr uint32 FloatBits(float32 value)
{
    return __builtin_bit_cast(uint32, value);
}
} // namespace

MARLIN_TEST(Shader, ReadsDebugOutputRecords)
{
    constexpr StringView Print = "thread {0}: mask {1:x}, at {2}, {3} {{ok}}";
    constexpr StringView Assert = "/Engine/Test.mshader:7: assertion failed: past the {{limit}}";
    HashMap<uint32, String> formats;
    formats.Add(GetShaderDebugFormatHash(Print), String(Print));
    formats.Add(GetShaderDebugFormatHash(Assert), String(Assert));

    DebugBuffer buffer(64);
    const uint64 negative = static_cast<uint64>(-5);
    buffer.Record(Print, {Descriptor(0, 32, 1), 3, Descriptor(0, 32, 1), 255, Descriptor(2, 32, 3), FloatBits(1.5f),
                          FloatBits(-2), FloatBits(0.25f), Descriptor(1, 64, 1), static_cast<uint32>(negative),
                          static_cast<uint32>(negative >> 32)});
    buffer.Record(Assert, {});
    buffer.Record("not a format the shaders have", {});
    List<ShaderDebugMessage> messages;
    MARLIN_EXPECT(ReadShaderDebugOutput(buffer.Words.AsSpan(), formats, messages) == 0);
    MARLIN_REQUIREF(messages.Count() == 3, "{0}", messages.Count());
    MARLIN_EXPECTF(messages[0].Text == "thread 3: mask ff, at (1.5, -2, 0.25), -5 {ok}", "{0}", messages[0].Text);
    MARLIN_EXPECT(!messages[0].Assertion);
    MARLIN_EXPECTF(messages[1].Text == "/Engine/Test.mshader:7: assertion failed: past the {limit}", "{0}",
                   messages[1].Text);
    MARLIN_EXPECT(messages[1].Assertion);
    MARLIN_EXPECT(messages[2].Text.AsView().StartsWith("debug output of an unknown format"));
}

MARLIN_TEST(Shader, CountsDebugOutputThatDidNotFit)
{
    constexpr StringView Print = "{0}";
    HashMap<uint32, String> formats;
    formats.Add(GetShaderDebugFormatHash(Print), String(Print));
    // Room for one record of four words; the GPU counted three, the last two of which went to the scratch words.
    DebugBuffer buffer(4);
    buffer.Record(Print, {Descriptor(0, 32, 1), 1});
    buffer.Words[ShaderDebugCount] = 12;
    List<ShaderDebugMessage> messages;
    MARLIN_EXPECT(ReadShaderDebugOutput(buffer.Words.AsSpan(), formats, messages) == 8);
    MARLIN_REQUIRE(messages.Count() == 1);
    MARLIN_EXPECT(messages[0].Text == "1");
}

namespace
{
/// What compiling *source*'s `Test.Main` through the shader pipeline reports: the front end's diagnostics, or the
/// errors of the MIR passes; empty when it compiles.
String CompileErrors(StringView source)
{
    static constexpr StringView Roots[] = {"/Engine"};
    VfsSourceProvider engine{Roots};
    MemorySourceProvider memory;
    memory.Add("/Project/Language/Shaders/Test.mshader", source);
    ISourceProvider *providers[] = {&engine, &memory};
    CompositeSourceProvider sources{Span<ISourceProvider *const>(providers)};
    LanguageCompilation compilation(sources, {});
    MirModule module;
    if (!compilation.Load() || !compilation.CompileEntryPoint("Test.Main", module))
    {
        return compilation.GetDiagnostics().ToString();
    }
    const MirPassRegistry registry = MirPassRegistry::CreateDefault();
    MirModulePassManager passes({.VerifyEach = true});
    String error;
    registry.ParsePipeline(MirShaderPipeline, passes, error, {.VerifyEach = true});
    MirModuleAnalysisManager analyses;
    passes.Run(module, analyses);
    StringBuilder<1024> errors;
    for (const String &reason : passes.GetErrors())
    {
        errors.Append(reason.AsView()).Append('\n');
    }
    return String(errors.AsView());
}

/// *source*'s `Test.Main`, compiled with debug output as MarlinConverter compiles global shaders for development
/// builds, run once on one group of *fixture*'s device with *arguments* (and *constants* as its specialization
/// constants by id), and its debug output read back.
struct DebugRun
{
    DebugRun(RhiDeviceFixture &fixture, StringView source, Span<const uint8> arguments,
             Span<const uint32> constants = {})
    {
        static constexpr StringView Roots[] = {"/Engine"};
        VfsSourceProvider engine{Roots};
        MemorySourceProvider memory;
        memory.Add("/Project/Language/Shaders/Test.mshader", source);
        ISourceProvider *providers[] = {&engine, &memory};
        CompositeSourceProvider sources{Span<ISourceProvider *const>(providers)};
        LanguageCompileOptions options;
        options.DebugOutput = true;
        options.CheckSpans = true;
        LanguageCompilation compilation(sources, options);
        MirModule module;
        MARLIN_REQUIREF(compilation.Load() && compilation.CompileEntryPoint("Test.Main", module), "{0}",
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
        const bool emitted = passes.GetErrors().IsEmpty() && errors.IsEmpty() && target->Emit(module, {}, code, errors);
        StringBuilder<1024> reasons;
        for (const String &reason : passes.GetErrors())
        {
            reasons.Append(reason.AsView()).Append('\n');
        }
        for (const String &reason : errors)
        {
            reasons.Append(reason.AsView()).Append('\n');
        }
        MARLIN_REQUIREF(emitted, "{0}", reasons.AsView());
        MirReflection reflection;
        ReflectMirEntryPoint(module, *module.GetEntryPoints()[0], reflection);
        MARLIN_REQUIRE(reflection.DebugBufferOffset != ~0u);
        HashMap<uint32, String> formats;
        for (const StringView format : reflection.DebugFormats)
        {
            formats.Add(GetShaderDebugFormatHash(format), String(format));
        }

        RhiDevice &device = *fixture.Device;
        RhiComputePipelineDesc pipelineDesc;
        pipelineDesc.Shader = {code.AsSpan(), "Test.Main", RhiShaderStage::Compute, constants};
        // The reflection's names view the module, which goes away with this scope.
        for (const MirReflectedSpecialization &specialization : reflection.Specializations)
        {
            Specializations.Add({String(specialization.Name), specialization.Id});
        }
        const RhiPipeline pipeline = device.CreatePipeline(pipelineDesc);
        MARLIN_REQUIRE(device.WaitForPipeline(pipeline) == RhiPipelineState::Ready);
        // As ShaderLibrary keeps it: in Readback memory, which the GPU writes and the CPU reads without a copy.
        constexpr uint32 Capacity = 256;
        const RhiBuffer buffer = fixture.CreateBuffer(
            (ShaderDebugHeaderWords + Capacity + ShaderDebugScratchWords) * sizeof(uint32),
            RhiBufferUsage::ShaderResource | RhiBufferUsage::UnorderedAccess, RhiMemoryLocation::Readback);
        auto *words = reinterpret_cast<uint32 *>(device.GetMappedData(buffer));
        words[ShaderDebugCount] = 0;
        words[ShaderDebugCapacity] = Capacity;

        // As ShaderLibrary passes them: the arguments, then the buffer's address, in one piece.
        List<uint8> direct;
        direct.Resize(reflection.DebugBufferOffset + sizeof(uint64));
        MemCopy(direct.Data(), arguments.Data(), arguments.Count());
        const uint64 address = device.GetBufferAddress(buffer);
        MemCopy(direct.Data() + reflection.DebugBufferOffset, &address, sizeof(address));
        RhiCommandList *list = device.BeginCommandList(RhiQueueType::Graphics);
        list->SetPipeline(pipeline);
        list->SetShaderArguments(Span<const uint8>(direct.Data(), direct.Count()));
        list->Dispatch(1, 1, 1);
        fixture.SubmitAndWait(list);
        Dropped = ReadShaderDebugOutput(Span<const uint32>(words, ShaderDebugHeaderWords + Capacity), formats,
                                        Messages);
        device.Destroy(pipeline);
        device.Destroy(buffer);
    }

    List<ShaderDebugMessage> Messages;
    struct Specialization
    {
        String Name;
        uint32 Id = 0;
    };
    List<Specialization> Specializations;
    uint32 Dropped = 0;
};
} // namespace

MARLIN_TEST_F(RhiDeviceFixture, Shader, DebugOutputReachesTheCpu, Gpu)
{
    const uint32 limit[] = {3};
    const DebugRun run(*this, R"(
namespace Test;
public struct Args { public uint Limit; }
[Compute(4, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    Debug.Print("thread {0}: {1} {2} {3}", id.x, half2(0.5, -1), (long)id.x - 2, id.x < 2);
    Debug.Assert(id.x < args.Limit, "past the limit");
}
)",
                       Span<const uint8>(reinterpret_cast<const uint8 *>(limit), sizeof(limit)));
    MARLIN_EXPECT(run.Dropped == 0);
    MARLIN_REQUIREF(run.Messages.Count() == 5, "{0}", run.Messages.Count());
    uint32 prints = 0;
    for (const ShaderDebugMessage &message : run.Messages)
    {
        if (message.Assertion)
        {
            MARLIN_EXPECTF(message.Text.AsView().EndsWith("Test.mshader:8: assertion failed: past the limit"), "{0}",
                           message.Text);
            continue;
        }
        for (uint32 thread = 0; thread < 4; ++thread)
        {
            StringBuilder<128> expected;
            FormatTo(expected, "thread {0}: (0.5, -1) {1} {2}", thread, static_cast<int64>(thread) - 2,
                     thread < 2 ? "true" : "false");
            prints += message.Text.AsView() == expected.AsView() ? 1 : 0;
        }
    }
    MARLIN_EXPECTF(prints == 4, "{0}", prints);
}

MARLIN_TEST_F(RhiDeviceFixture, Shader, SpanAccessesOutOfRangeAreReported, Gpu)
{
    // A read past the end of a Span of two and one of an empty Span: both are clamped and reported. (A null
    // Span is reported too, but its read faults the GPU before the report can be read back.)
    const RhiBuffer data = CreateBuffer(sizeof(uint32) * 2, RhiBufferUsage::ShaderResource, RhiMemoryLocation::Upload);
    // Args as the language lays it out: a Span is a Ptr and a uint, 16 bytes with its padding.
    struct
    {
        uint64 ValuesPointer = 0;
        uint32 ValuesLength = 2;
        uint32 ValuesPad = 0;
        uint64 EmptyPointer = 0;
        uint32 EmptyLength = 0;
        uint32 EmptyPad = 0;
        uint32 Index = 5;
    } arguments;
    arguments.ValuesPointer = Device->GetBufferAddress(data);
    arguments.EmptyPointer = arguments.ValuesPointer;
    const DebugRun run(*this, R"(
namespace Test;
public struct Args { public Span<uint> Values; public Span<uint> Empty; public uint Index; }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    uint sum = args.Values[args.Index] + args.Values[1];
    if (sum == 12345)
    {
        Debug.Print("{0}", args.Empty[0]);
    }
    else
    {
        Debug.Print("{0}", sum + args.Empty[id.x]);
    }
}
)",
                       Span<const uint8>(reinterpret_cast<const uint8 *>(&arguments), sizeof(arguments)));
    Device->Destroy(data);
    bool pastTheEnd = false;
    bool empty = false;
    constexpr StringView PastTheEnd = "`args.Values[args.Index]` reads index 5 of a Span of 2 at";
    constexpr StringView Empty = "`args.Empty[id.x]` reads index 0 of a Span of 0 at ";
    for (const ShaderDebugMessage &message : run.Messages)
    {
        pastTheEnd = pastTheEnd || (message.Assertion && message.Text.AsView().Contains(PastTheEnd));
        empty = empty || (message.Assertion && message.Text.AsView().Contains(Empty));
    }
    MARLIN_EXPECTF(pastTheEnd && empty, "{0}: {1}", run.Messages.Count(),
                   run.Messages.IsEmpty() ? StringView() : run.Messages[0].Text.AsView());
}

MARLIN_TEST_F(RhiDeviceFixture, Shader, TwoDirectionForwardModeCarriesBothTangents, Gpu)
{
    // t = uv * 4 + (1, 5) = (3, 6) with tangents (4, 0) along x and (0, 4) along y.
    // t.x * t.y = 18, with derivatives 24 and 12.
    // Both: its [ForwardDerivative2] sees both directions at once and returns 1000 * dx.x + dy.y = 4004, with
    // derivatives dx.x = 4 and dy.y = 4.
    // Triple: only a one-direction derivative, called per direction; it scales the tangent by 3 (not its true 2) to
    // show it is the one used: 12, with derivatives 0 and 12.
    const uint32 unused[] = {0};
    const DebugRun run(*this, R"(
namespace Test;
public struct Args { public uint Unused; }

[ForwardDerivative2(nameof(BothDerivative))]
float Both(float2 uv) => uv.x + uv.y;
DifferentialPair2<float> BothDerivative(DifferentialPair2<float2> uv)
    => new DifferentialPair2<float>(uv.Dx.x * 1000 + uv.Dy.y, uv.Dx.x, uv.Dy.y);

[ForwardDerivative(nameof(TripleDerivative))]
float Triple(float x) => 2 * x;
DifferentialPair<float> TripleDerivative(DifferentialPair<float> x)
    => new DifferentialPair<float>(2 * x.Primal, 3 * x.Differential);

[Differentiable]
float Shade(float2 uv)
{
    float2 t = uv * 4 + float2(1, 5);
    return t.x * t.y + Both(t) + Triple(t.y);
}

[Compute(1, 1, 1)]
void Main([Uniform] in Args args)
{
    DifferentialPair2<float> r =
        Autodiff.Forward(Shade, new DifferentialPair2<float2>(float2(0.5, 0.25), float2(1, 0), float2(0, 1)));
    Debug.Print("{0} {1} {2}", r.Primal, r.Dx, r.Dy);
}
)",
                       Span<const uint8>(reinterpret_cast<const uint8 *>(unused), sizeof(unused)));
    MARLIN_REQUIREF(run.Messages.Count() == 1, "{0}", run.Messages.Count());
    MARLIN_EXPECTF(run.Messages[0].Text.AsView() == "4034 28 28", "{0}", run.Messages[0].Text);
}

MARLIN_TEST_F(RhiDeviceFixture, Shader, InstanceMethodsDifferentiateThroughTheirObject, Gpu)
{
    // As a material pass differentiates a material: a generic function names the method through its object, and
    // the method is differentiable because the interface member it implements is. Evaluate(uv) = uv.x * 5 + uv.y²
    // at (1, 3) has derivatives 5 along x and 6 along y.
    const uint32 unused[] = {0};
    const DebugRun run(*this, R"(
namespace Test;
public struct Args { public uint Unused; }

public interface IShade
{
    [Differentiable] float Evaluate(float2 uv);
}

public struct Stripes : IShade
{
    public float Scale;
    public float Evaluate(float2 uv) => uv.x * Scale + uv.y * uv.y;
}

float Run<T>(T shade) where T : IShade
{
    DifferentialPair2<float> r =
        Autodiff.Forward(shade.Evaluate, new DifferentialPair2<float2>(float2(1, 3), float2(1, 0), float2(0, 1)));
    return r.Dx * 100 + r.Dy;
}

[Compute(1, 1, 1)]
void Main([Uniform] in Args args)
{
    Stripes stripes;
    stripes.Scale = 5;
    Debug.Print("{0}", Run(stripes));
}
)",
                       Span<const uint8>(reinterpret_cast<const uint8 *>(unused), sizeof(unused)));
    MARLIN_REQUIREF(run.Messages.Count() == 1, "{0}", run.Messages.Count());
    MARLIN_EXPECTF(run.Messages[0].Text.AsView() == "506", "{0}", run.Messages[0].Text);
}

MARLIN_TEST_F(RhiDeviceFixture, Shader, SurfacesFlattenToClosures, Gpu)
{
    // Moss a quarter unit thick over rock, mixed with a Toon BSDF covering a quarter: the layered pair keeps three
    // quarters, so the weights are 0.75, 0.75 and 0.25. The moss lets 0.5^0.25 = 0.841 through, which the rock keeps
    // as its coat, its opacity changing by -ln(0.5) 0.841 = 0.583 per unit of u along x; the moss's diffuse keeps
    // what it does not let through, 0.4 (1 - 0.841) = 0.064 of green, and its surface reflects 4%. The Toon closure
    // carries its type ID and its fields.
    const uint32 unused[] = {0};
    const DebugRun run(*this, R"(
namespace Test;
using Marlin.Shading;
public struct Args { public uint Unused; }

public struct Toon : IBsdf
{
    public half3 Color;
    public half Steps;
    public half3 EvaluateDirect(in BsdfContext ctx, in LightSample light)
        => Color * Floor(Saturate((half)Dot(ctx.N, light.L)) * Steps) / Steps;
}

public struct Mossy : ISurfaceMaterial
{
    public half Roughness;
    public Surface Evaluate(in SurfaceInputs input)
    {
        Slab rock = Slab.Default with { Roughness = Roughness };
        Slab moss = Slab.Default with { Albedo = half3(0.2h, 0.4h, 0.1h), Fuzz = 0.5h,
                                        TransmittanceColor = half3(0.5h, 0.5h, 0.5h) };
        Surface layered = Surface.VerticalLayer(moss, rock, (half)input.Uv0.x);
        return Surface.HorizontalMix(layered, Surface.Custom(new Toon { Color = half3(1.0h, 0.5h, 0.25h), Steps = 3.0h }),
                                     0.25h);
    }
}

[Compute(1, 1, 1)]
void Main([Uniform] in Args args)
{
    Mossy material;
    material.Roughness = 0.7h;
    SurfaceInputs inputs = default;
    inputs.Uv0 = float2(0.25, 0);
    SurfaceInputs dx = default;
    dx.Uv0 = float2(1, 0);
    SurfaceInputs dy = default;
    dy.Uv0 = float2(0, 1);
    DifferentialPair2<Surface> s =
        Autodiff.Forward(material.Evaluate, new DifferentialPair2<SurfaceInputs>(inputs, dx, dy));
    Surface surface = s.Primal;
    Toon toon = Compiler.FromWords<Toon>(surface.Closures[2].Payload);
    Debug.Print("{0} {1} {2} {3}", surface.Count, (float)surface.Closures[0].Weight, (float)surface.Closures[1].Weight,
                (float)surface.Closures[2].Weight);
    Debug.Print("{0} {1}", surface.Closures[2].Type == Compiler.TypeId<Toon>(), (float)toon.Color.y);
    Debug.Print("{0} {1} {2}", (uint)((float)surface.Closures[1].CoatTransmittance.x * 1000 + 0.5),
                (uint)((float)s.Dx.Closures[1].CoatOpacity.x * 1000 + 0.5),
                (uint)((float)surface.Closures[0].Slab.Albedo.y * 1000 + 0.5));
    Debug.Print("{0} {1}", (uint)((float)surface.Closures[1].CoatSpecular * 1000 + 0.5), surface.Closures[0].IsCoated);
}
)",
                       Span<const uint8>(reinterpret_cast<const uint8 *>(unused), sizeof(unused)));
    MARLIN_REQUIREF(run.Messages.Count() == 4, "{0}", run.Messages.Count());
    MARLIN_EXPECTF(run.Messages[0].Text.AsView() == "3 0.75 0.75 0.25", "{0}", run.Messages[0].Text);
    MARLIN_EXPECTF(run.Messages[1].Text.AsView() == "true 0.5", "{0}", run.Messages[1].Text);
    MARLIN_EXPECTF(run.Messages[2].Text.AsView() == "841 583 64", "{0}", run.Messages[2].Text);
    MARLIN_EXPECTF(run.Messages[3].Text.AsView() == "40 false", "{0}", run.Messages[3].Text);
}

MARLIN_TEST(Shader, ShaderKeysCarrySpecializationValues)
{
    const ShaderSpecializationValue values[] = {{"Steps", 8}, {"Fog", 1}};
    const String material = MakeMaterialShaderKey("Test.Resolve", "Test.Rock", {});
    const String key = MakeSpecializedShaderKey(material, values);
    MARLIN_EXPECTF(key.AsView() == "Test.Resolve|Test.Rock|#Fog=1;Steps=8;", "{0}", key);
    MARLIN_EXPECT(GetShaderKeyAsset(key) == material.AsView());
    MARLIN_EXPECT(GetShaderKeyEntryPoint(key) == "Test.Resolve");
    MARLIN_EXPECT(GetShaderKeyEntryPoint(MakeSpecializedShaderKey("Test.Main", values)) == "Test.Main");
    StringView parsedMaterial;
    List<ShaderVariantValue> variant;
    MARLIN_EXPECT(ParseShaderKey(key, parsedMaterial, variant) && parsedMaterial == "Test.Rock");
    List<ShaderSpecializationValue> parsed;
    MARLIN_REQUIRE(ParseShaderKeySpecializations(key, parsed) && parsed.Count() == 2);
    MARLIN_EXPECT(parsed[0].Name.AsView() == "Fog" && parsed[0].Bits == 1);
    MARLIN_EXPECT(parsed[1].Name.AsView() == "Steps" && parsed[1].Bits == 8);
    MARLIN_EXPECT(ParseShaderKeySpecializations("Test.Main", parsed) && parsed.IsEmpty());
    MARLIN_EXPECT(!ParseShaderKeySpecializations("Test.Main#Steps=x;", parsed));
    // Re-specializing replaces the values rather than appending to them.
    const ShaderSpecializationValue other[] = {{"Steps", 2}};
    MARLIN_EXPECT(MakeSpecializedShaderKey(key, other).AsView() == "Test.Resolve|Test.Rock|#Steps=2;");
}

MARLIN_TEST_F(RhiDeviceFixture, Shader, SpecializationConstantsTakeThePipelinesValues, Gpu)
{
    static constexpr StringView Source = R"(
namespace Test;
[Specialization] static uint Steps = 4;
[Specialization] static float Scale = 0.5;
[Specialization] static bool Fog = false;
[Compute(1, 1, 1)]
void Main()
{
    Debug.Print("{0} {1} {2}", Steps, Scale, Fog);
}
)";
    const DebugRun defaults(*this, Source, {});
    MARLIN_REQUIRE(defaults.Messages.Count() == 1);
    MARLIN_EXPECTF(defaults.Messages[0].Text.AsView() == "4 0.5 false", "{0}", defaults.Messages[0].Text);
    // Dense by id, as ShaderLibrary fills them from the reflected constants.
    MARLIN_REQUIRE(defaults.Specializations.Count() == 3);
    uint32 constants[3] = {};
    for (const auto &[name, id] : defaults.Specializations)
    {
        MARLIN_REQUIRE(id < 3);
        constants[id] = name.AsView() == "Test.Steps" ? 9u : name.AsView() == "Test.Scale" ? FloatBits(2.0f) : 1u;
    }
    const DebugRun specialized(*this, Source, {}, constants);
    MARLIN_REQUIRE(specialized.Messages.Count() == 1);
    MARLIN_EXPECTF(specialized.Messages[0].Text.AsView() == "9 2 true", "{0}", specialized.Messages[0].Text);
}

MARLIN_TEST(Shader, MaterialsReflectTheirParameters)
{
    static constexpr StringView Roots[] = {"/Engine"};
    VfsSourceProvider engine{Roots};
    MemorySourceProvider memory;
    memory.Add("/Project/Language/Shaders/Materials/Rock.mshader", R"(
namespace Test;
using Marlin.Shading;

[Material, Id("9a0b0000-0000-0000-0000-000000000001")]
public struct Rock<bool UseMoss = true> : ISurfaceMaterial
{
    public MaterialTexture2D<half4> BaseColor = DefaultTextures.White;
    [Range(0, 1)] public half Roughness = 0.25h;
    [Color] public half3 Tint = half3(1.0h, 0.5h, 0.25h);

    public Surface Evaluate(in SurfaceInputs input)
    {
        half4 albedo = BaseColor.Sample(input.Uv0);
        return Surface.FromSlab(Slab.Default with { Albedo = albedo.xyz * Tint, Roughness = Roughness });
    }
}
)");
    ISourceProvider *providers[] = {&engine, &memory};
    CompositeSourceProvider sources{Span<ISourceProvider *const>(providers)};
    LanguageCompileOptions options;
    options.Material = "Test.Rock";
    LanguageCompilation compilation(sources, options);
    LanguageMaterialReflection reflection;
    MARLIN_REQUIREF(compilation.Load() && compilation.ReflectMaterial(reflection), "{0}",
                    compilation.GetDiagnostics().ToString());
    List<String> materials;
    compilation.GetMaterials("/Language/Shaders/", materials);
    MARLIN_EXPECT(materials.Contains(String("Test.Rock")) && materials.Contains(String("Marlin.Materials.Default")));
    MARLIN_EXPECT(reflection.Name == "Test.Rock" && reflection.Id == "9a0b0000-0000-0000-0000-000000000001");
    MARLIN_REQUIRE(reflection.Switches.Count() == 1);
    MARLIN_EXPECT(reflection.Switches[0].Name == "UseMoss" && reflection.Switches[0].Default == 1 &&
                  reflection.Switches[0].ValueCount == 2);
    MARLIN_REQUIRE(reflection.Parameters.Count() == 3);
    const LanguageMaterialParameter &baseColor = reflection.Parameters[0];
    const LanguageMaterialParameter &roughness = reflection.Parameters[1];
    const LanguageMaterialParameter &tint = reflection.Parameters[2];
    // The texture's handle and sampler, its default (White = 1), then at 16 its streaming feedback pointer, which
    // SceneMaterialRegistry writes there.
    MARLIN_EXPECT(baseColor.IsTexture && baseColor.DefaultTexture == 1 && baseColor.Offset == 0 && baseColor.Size == 24);
    MARLIN_EXPECTF(roughness.Offset == 24, "{0}", roughness.Offset);
    MARLIN_EXPECT(roughness.HasRange && roughness.RangeMin == 0 && roughness.RangeMax == 1 && roughness.Size == 2);
    MARLIN_EXPECT(tint.IsColor && tint.Size == 6);
    MARLIN_REQUIRE(reflection.Defaults.Count() == reflection.Size && reflection.Size >= tint.Offset + tint.Size);
    uint16 roughnessBits = 0;
    MemCopy(&roughnessBits, reflection.Defaults.Data() + roughness.Offset, sizeof(roughnessBits));
    MARLIN_EXPECTF(roughnessBits == Float16(0.25f).Bits, "{0}", roughnessBits);
    uint16 tintBits[3] = {};
    MemCopy(tintBits, reflection.Defaults.Data() + tint.Offset, sizeof(tintBits));
    MARLIN_EXPECT(tintBits[0] == Float16(1.0f).Bits && tintBits[1] == Float16(0.5f).Bits &&
                  tintBits[2] == Float16(0.25f).Bits);
    uint32 defaultTexture = 0;
    MemCopy(&defaultTexture, reflection.Defaults.Data() + baseColor.Offset + 8, sizeof(defaultTexture));
    MARLIN_EXPECT(defaultTexture == 1);
}

MARLIN_TEST(Shader, UnrolledLoopsInlineFunctionsThatReturnEarly)
{
    // An [Unroll] loop whose body inlines a function with an early return, as the lighting passes have.
    const String errors = CompileErrors(R"(
namespace Test;
public struct Args { public Span<float, Access.ReadWrite> Out; public uint Count; }

float Shade(float x)
{
    if (x <= 0)
    {
        return 0;
    }
    return x * x;
}

[Compute(1, 1, 1)]
void Main([Uniform] in Args args)
{
    float sum = 0;
    [Unroll]
    for (uint i = 0; i < 4; ++i)
    {
        if (i < args.Count)
        {
            sum += Shade((float)i - 1);
        }
    }
    args.Out[0] = sum;
}
)");
    MARLIN_EXPECTF(errors.IsEmpty(), "{0}", errors);
}

MARLIN_TEST(Shader, SurfaceShapesMustBeKnownAtCompileTime)
{
    // One slab or two depending on a runtime value: the shape is not a constant, which is an error (§9.4).
    const String errors = CompileErrors(R"(
namespace Test;
using Marlin.Shading;
public struct Args { public uint Layers; }

[Compute(1, 1, 1)]
void Main([Uniform] in Args args)
{
    Surface surface = Surface.FromSlab(Slab.Default);
    if (args.Layers > 1)
    {
        surface = Surface.VerticalLayer(Slab.Default, Slab.Default, 0.5h);
    }
    surface = Surface.HorizontalMix(surface, Surface.FromSlab(Slab.Default), 0.5h);
    if (surface.Closures[0].Weight > 2.0h)
    {
        Debug.Print("{0}", surface.Count);
    }
}
)");
    MARLIN_EXPECTF(errors.AsView().Contains("its shape must be known at compile time (it depends on a value only known "
                                            "at run time)"),
                   "{0}", errors);
}
} // namespace Marlin::Tests
