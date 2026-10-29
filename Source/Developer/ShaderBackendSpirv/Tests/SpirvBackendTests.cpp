#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirPrinter.h"
#include "LanguageIR/MirReflection.h"
#include "LanguageIR/MirTarget.h"
#include "Memory/MemoryOps.h"
#include "ShaderBackendSpirv/SpirvTarget.h"
#include "String/StringBuilder.h"
#include "spirv-tools/libspirv.h"

namespace Marlin::Tests
{
namespace
{
/// Compiles *entryPoint* of *source* against the mounted core library, optimizes it with the shader pipeline,
/// legalizes it for the SPIR-V target, emits it and validates it for Vulkan 1.4.
struct SpirvCompiled
{
    SpirvCompiled(StringView source, StringView entryPoint, bool debugOutput = false,
                  Span<const StringView> features = {}, bool debugInfo = false)
    {
        Memory.Add("/Project/Language/Shaders/Test.mshader", source);
        ISourceProvider *providers[] = {&Engine, &Memory};
        CompositeSourceProvider sources{Span<ISourceProvider *const>(providers)};
        LanguageCompileOptions options;
        options.DebugOutput = debugOutput;
        options.DebugInfo = debugInfo;
        for (const StringView feature : features)
        {
            options.Features.Add(String(feature));
        }
        LanguageCompilation compilation(sources, options);
        if (!compilation.Load() || !compilation.CompileEntryPoint(entryPoint, Module))
        {
            Errors = compilation.GetDiagnostics().ToString();
            return;
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
        passes.Run(Module, analyses);
        if (!passes.GetErrors().IsEmpty() || !errors.IsEmpty())
        {
            Errors = String(!errors.IsEmpty() ? errors[0].AsView() : passes.GetErrors()[0].AsView());
            return;
        }
        List<uint8> bytes;
        if (!target->Emit(Module, {}, bytes, errors))
        {
            Errors = String(errors.IsEmpty() ? StringView("emission failed") : errors[0].AsView());
            return;
        }
        Words.ResizeUninitialized(bytes.Count() / sizeof(uint32));
        MemCopy(Words.Data(), bytes.Data(), bytes.Count());
        Errors = Validate();
        Ok = Errors.IsEmpty();
    }

    String Validate() const
    {
        spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_4);
        spv_diagnostic diagnostic = nullptr;
        spv_const_binary_t binary{Words.Data(), Words.Count()};
        const spv_result_t result = spvValidate(context, &binary, &diagnostic);
        String message;
        if (result != SPV_SUCCESS)
        {
            message = diagnostic != nullptr && diagnostic->error != nullptr ? String(diagnostic->error)
                                                                          : String("spirv-val failed");
        }
        spvDiagnosticDestroy(diagnostic);
        spvContextDestroy(context);
        return message;
    }

    String Disassemble() const
    {
        spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_4);
        spv_text text = nullptr;
        spv_diagnostic diagnostic = nullptr;
        spvBinaryToText(context, Words.Data(), Words.Count(), SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES, &text,
                        &diagnostic);
        String result = text != nullptr ? String(StringView(text->str, text->length)) : String();
        spvTextDestroy(text);
        spvDiagnosticDestroy(diagnostic);
        spvContextDestroy(context);
        return result;
    }

    static constexpr StringView EngineRoots[] = {"/Engine"};
    VfsSourceProvider Engine{EngineRoots};
    MemorySourceProvider Memory;
    MirModule Module;
    List<uint32> Words;
    String Errors;
    bool Ok = false;
};
} // namespace

MARLIN_TEST(SpirvBackend, PointersAndSpansValidate)
{
    const SpirvCompiled compiled(R"(
namespace Test;

public struct Counter { public uint Count; public float Total; }

public struct Inputs
{
    public Span<float4> Values;
    public Ptr<byte> Raw;
}

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Ptr<Inputs> inputs, [Uniform] in Ptr<Counter, Access.ReadWrite> counter,
          [Uniform] in Span<uint, Access.ReadWrite> output)
{
    Inputs read = inputs.Value;
    if (id.x >= read.Values.Length || counter.IsNull)
    {
        return;
    }
    float4 value = read.Values[id.x] + read.Values.Slice(1, 4)[0];
    uint word = read.Raw.Offset(4).Read<uint>(0);
    Atomic.Add(ref counter.Value.Count, 1u);
    Atomic.Add(ref output[id.x], word);
    (counter + 1u).Value.Total = value.x;
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    const String text = compiled.Disassemble();
    MARLIN_EXPECTF(text.AsView().Contains("PhysicalStorageBuffer64") && !text.AsView().Contains("DescriptorHeap"), "{0}",
                   text.AsView());
}

MARLIN_TEST(SpirvBackend, PointerConversionsLoopsAndAliasingValidate)
{
    const SpirvCompiled compiled(R"(
namespace Test;

public struct Inputs
{
    [NoAlias] public Span<float, Access.ReadWrite> Values;
    public Ptr<float> Scale;
}

float Sum(Span<float> values)
{
    float total = 0;
    foreach (float value in values)
    {
        total += value;
    }
    return total;
}

float First(Ptr<float> pointer) => pointer.Value;

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Inputs inputs, [Uniform, NoAlias] in Ptr<float, Access.ReadWrite> output)
{
    // A writable Span and Ptr convert to read-only ones, and a Span to its Ptr (§4.5).
    float total = Sum(inputs.Values) + First(inputs.Values);
    Ptr<float> read = output;
    if (read == inputs.Scale || inputs.Scale != inputs.Scale + 1u)
    {
        total = total * inputs.Scale.Value;
    }
    output[id.x] = total;
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    const String text = compiled.Disassemble();
    MARLIN_EXPECTF(text.AsView().Contains("RestrictPointer") && text.AsView().Contains("NonWritable"), "{0}",
                   text.AsView());
}

MARLIN_TEST(SpirvBackend, ExtensionOperatorsOnVectorsValidate)
{
    // Where no built-in operator fits, an extension adds one to a vector (§3.5): `^` as the cross product, `~` as
    // the reversed components.
    const SpirvCompiled compiled(R"(
namespace Test;

public extension float3
{
    public static float3 operator ^(float3 a, float3 b) => Cross(a, b);
    public static float3 operator ~(float3 v) => v.zyx;
}

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Span<float3, Access.ReadWrite> output)
{
    float3 a = float3((float)id.x, 1, 0);
    output[id.x] = ~(a ^ float3(0, 0, 1));
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    MARLIN_EXPECTF(compiled.Disassemble().AsView().Contains("Cross"), "{0}", compiled.Disassemble().AsView());
}

MARLIN_TEST(SpirvBackend, SixtyFourBitAtomicLoadsAndStoresValidate)
{
    const SpirvCompiled compiled(R"(
namespace Test;
[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Span<ulong, Access.ReadWrite> queue)
{
    ulong value = Atomic.Load(ref queue[id.x]);
    Atomic.Store(ref queue[id.x + 1], value + 1ul);
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    MARLIN_EXPECT(compiled.Disassemble().AsView().Contains("Int64Atomics"));
}

MARLIN_TEST(SpirvBackend, ComputeShaderValidates)
{
    const SpirvCompiled compiled(R"(
namespace Test;

public struct Particle
{
    public float3 Position;
    public float Mass;
    public float3x4 Transform;
}

public struct Args
{
    public Span<Particle, Access.ReadWrite> Particles;
    public Span<float4> Forces;
    public uint Count;
    public float Step;
}

[GroupShared] static float[64] Scratch;

float3 Integrate(float3 position, float3 force, float mass, float step) => position + force / Max(mass, 0.001) * step;

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [GroupIndex] uint lane, [Uniform] in Args args)
{
    if (id.x >= args.Count)
    {
        return;
    }
    Particle particle = args.Particles[id.x];
    float3 force = args.Forces[id.x].xyz;
    Scratch[lane] = Length(force);
    Barrier.Group();
    float neighbor = Scratch[(lane + 1) % 64];
    particle.Position = Integrate(particle.Position, force, particle.Mass, args.Step) + particle.Transform * float4(0, neighbor, 0, 1);
    args.Particles[id.x] = particle;
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    MARLIN_EXPECT(compiled.Disassemble().AsView().Contains("OpConvertUToPtr"));
}

MARLIN_TEST(SpirvBackend, PixelShaderSamplesThroughTheHeaps)
{
    const SpirvCompiled compiled(R"(
namespace Test;

public struct Material
{
    public Texture2D<half4> BaseColor;
    public Sampler LinearSampler;
    public float Exposure;
}

public struct Varyings
{
    [Position] public float4 Position;
    public float2 Uv;
    [NoInterpolation] public uint MaterialIndex;
}

public struct Args { public Span<Material> Materials; }

[Pixel] [return: Target(0)]
half4 Main(in Varyings v, [Uniform] in Args args)
{
    Material material = args.Materials[v.MaterialIndex];
    half4 color = material.BaseColor.Sample(material.LinearSampler, v.Uv);
    if (color.a < 0.5)
    {
        discard;
    }
    return half4(color.rgb * (half)material.Exposure, 1);
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    const String text = compiled.Disassemble();
    MARLIN_EXPECT(text.AsView().Contains("ResourceHeapEXT") && text.AsView().Contains("SamplerHeapEXT"));
    MARLIN_EXPECT(text.AsView().Contains("NonUniform"));
}
MARLIN_TEST(SpirvBackend, IntegerMatricesBecomeArraysOfRows)
{
    const SpirvCompiled compiled(R"(
namespace Test;
public struct Cell { public int2x2 Weights; public uint Flags; }
public struct Args { public Span<Cell, Access.ReadWrite> Cells; public Span<int2, Access.ReadWrite> Output; }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    Cell cell = args.Cells[id.x];
    int2x2 m = int2x2(1, 2, 3, (int)id.x) + cell.Weights * 2;
    int2x2 product = m * Transpose(cell.Weights);
    args.Output[id.x] = product * int2(1, -1) + int2(m[1][1], Dot(int2(1, 2), int2(3, (int)id.y)));
    cell.Weights = product;
    args.Cells[id.x] = cell;
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    MARLIN_EXPECT(!compiled.Disassemble().AsView().Contains("OpTypeMatrix"));
}

MARLIN_TEST(SpirvBackend, MatrixAndLibraryExpansionsValidate)
{
    const SpirvCompiled compiled(R"(
namespace Test;
public struct Args { public Span<float4, Access.ReadWrite> Output; public float3x3 A; public float3x3 B; public int2 C; }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    float3x3 sum = args.A + args.B * 2 - (half3x3)args.A;
    float scale = Saturate(Ldexp(0.75, 2.0));
    int total = Dot(args.C, int2(3, 4));
    args.Output[id.x] = float4(sum[0] * scale, (float)total);
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    const String text = compiled.Disassemble();
    MARLIN_EXPECT(text.AsView().Contains("FClamp") && text.AsView().Contains("Exp2"));
}
MARLIN_TEST(SpirvBackend, LanguageConveniences)
{
    const SpirvCompiled compiled(R"(
namespace Test;

public static half Luma(this half3 c) => Dot(c, half3(0.2126, 0.7152, 0.0722));
T Largest<T>(T a, T b) where T : IComparable<T> => a.CompareTo(b) >= 0 ? a : b;

public struct Args { public Texture2D<half4> Image; public Sampler Linear; }
public struct Varyings { [Position] public float4 Position; public float2 Uv; }
public struct Output
{
    [Target(0)] public half4 Color;
    [Coverage] public uint Mask;
}

[Pixel]
Output Main(in Varyings v, [Coverage] uint coverage, [Uniform] in Args args)
{
    half4 red = args.Image.Gather(args.Linear, v.Uv);
    half luma = half3(red.xyz).Luma();
    Output output;
    output.Color = half4(Largest(luma, (half)0.5), Largest(1, 2), 0, 1);
    output.Mask = coverage & 0x5;
    return output;
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    const String text = compiled.Disassemble();
    MARLIN_EXPECT(text.AsView().Contains("SampleMask") && text.AsView().Contains("OpImageGather"));
}

MARLIN_TEST(SpirvBackend, FormatsClampingAndPrecision)
{
    const SpirvCompiled compiled(R"(
namespace Test;

public struct Args
{
    public Texture2D<float2, Access.ReadWrite, ImageFormat.Rg16f> Velocity;
    public Texture2D<half4, Access.ReadWrite> Accumulation;
    public Texture2D<float3, Access.ReadWrite> Normals;
    public uint Pick;
}

void Clear(Texture2D<float2, Access.ReadWrite> target, int2 position) => target.Store(position, float2(0, 0));

[Unchecked] float Raw(float[4] values, uint i) => values[i];

[Precise] float Blend(float a, float b, float t) => a * (1 - t) + b * t;

[Compute(8, 8, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    float[4] weights;
    for (int i = 0; i < 4; i++)
    {
        weights[i] = (float)i * 0.25;
    }
    weights[args.Pick] = 1;
    [Precise] float picked = weights[args.Pick] * 3 + Raw(weights, args.Pick + 1);
    float3 normal = args.Normals.Load((int2)id.xy);
    args.Velocity.Store((int2)id.xy, float2(Blend(picked, normal[args.Pick], 0.5), 0));
    args.Accumulation.Store((int2)id.xy, half4(1, 0, 0, 1));
    Clear(args.Velocity, (int2)id.xy + 1);
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    const String text = compiled.Disassemble();
    MARLIN_EXPECT(text.AsView().Contains(" Rg16f") && text.AsView().Contains(" Rgba16f") &&
                  text.AsView().Contains("StorageImageReadWithoutFormat"));
    MARLIN_EXPECT(text.AsView().Contains("UMin") && text.AsView().Contains("NoContraction"));
}

MARLIN_TEST(SpirvBackend, FormatMustHoldTheTexel)
{
    const SpirvCompiled compiled(R"(
namespace Test;
public struct Args { public Texture2D<float, Access.ReadWrite, ImageFormat.R32u> Bad; }
[Compute(1, 1, 1)]
void Main([Uniform] in Args args) { args.Bad.Store(int2(0, 0), 1); }
)", "Test.Main");
    MARLIN_EXPECT(!compiled.Ok);
    MARLIN_EXPECTF(compiled.Errors.AsView().Contains("does not hold `float` texels"), "{0}", compiled.Errors.AsView());
}

// RECOVERY-DECISION L12: float4[8] put the [Uniform] parameter block at 144 bytes, over the 128-byte limit (M4008) that every
// Vulkan device guarantees; the test shrinks to float4[7] rather than raising the limit.
MARLIN_TEST(SpirvBackend, UnrollAndSpecialization)
{
    const SpirvCompiled compiled(R"(
namespace Test;

[Specialization] static bool UseFog = false;
[Specialization] static uint SampleCount = 4;

public struct Args { public Span<float4, Access.ReadWrite> Output; public float4[7] Weights; }

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    float4 sum = float4(0, 0, 0, 0);
    [Unroll]
    for (int i = 0; i < 7; i++)
    {
        sum += args.Weights[i];
    }
    [Unroll(2)]
    for (uint j = 0; j < SampleCount; j++)
    {
        sum *= 0.5;
    }
    if (UseFog)
    {
        sum.w = 1;
    }
    args.Output[id.x] = sum;
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    const String text = compiled.Disassemble();
    MARLIN_EXPECT(text.AsView().Contains("OpSpecConstantFalse") && text.AsView().Contains("SpecId 1"));
    MARLIN_EXPECT(text.AsView().Contains("PartialCount 2") && !text.AsView().Contains("Unroll "));
}

MARLIN_TEST(SpirvBackend, DebugOutput)
{
    constexpr StringView source = R"(
namespace Test;
public struct Args { public Span<uint, Access.ReadWrite> Output; public uint Limit; }
[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    half3 color = half3(1, 0.5, 0);
    Debug.Print("thread {0} of {{group}}: 100% {1}, mask {2:x}, ok {3}", id.x, color, args.Limit, id.x < 4);
    Debug.Assert(id.x < args.Limit, "past the {limit}");
    args.Output[id.x] = id.x;
}
)";
    // Each print appends a record to the debug output buffer whose address follows the arguments (§9.11).
    const SpirvCompiled enabled(source, "Test.Main", true);
    MARLIN_REQUIREF(enabled.Ok, "{0}\n{1}", enabled.Errors.AsView(), enabled.Disassemble().AsView());
    const String text = enabled.Disassemble();
    MARLIN_EXPECT(!text.AsView().Contains("DebugPrintf"));
    MARLIN_EXPECT(text.AsView().Contains("OpAtomicIAdd") && text.AsView().Contains("\"DebugBuffer\""));
    MirReflection reflection;
    ReflectMirEntryPoint(enabled.Module, *enabled.Module.GetEntryPoints()[0], reflection);
    MARLIN_EXPECTF(reflection.DebugBufferOffset == (reflection.ArgumentsSize + 7) / 8 * 8, "{0}",
                   reflection.DebugBufferOffset);
    MARLIN_EXPECTF(reflection.Arguments.Count() == 2u, "{0}", reflection.Arguments.Count());
    MARLIN_REQUIREF(reflection.DebugFormats.Count() == 2u, "{0}", reflection.DebugFormats.Count());
    MARLIN_EXPECT(reflection.DebugFormats[0] == "thread {0} of {{group}}: 100% {1}, mask {2:x}, ok {3}");
    MARLIN_EXPECTF(reflection.DebugFormats[1].EndsWith(": assertion failed: past the {{limit}}"), "{0}",
                   reflection.DebugFormats[1]);

    const SpirvCompiled disabled(source, "Test.Main");
    MARLIN_REQUIREF(disabled.Ok, "{0}", disabled.Errors.AsView());
    MARLIN_EXPECT(!disabled.Disassemble().AsView().Contains("DebugBuffer"));
    MirReflection plain;
    ReflectMirEntryPoint(disabled.Module, *disabled.Module.GetEntryPoints()[0], plain);
    MARLIN_EXPECTF(plain.DebugBufferOffset == ~0u, "{0}", plain.DebugBufferOffset);
    MARLIN_EXPECTF(plain.ArgumentsSize == reflection.ArgumentsSize, "{0}", plain.ArgumentsSize);
}

MARLIN_TEST(SpirvBackend, DebugOutputWithoutArgumentsGetsABlock)
{
    // An entry point with no [Uniform] parameters still gets the debug buffer's address passed to it.
    const SpirvCompiled compiled(R"(
namespace Test;
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { Debug.Print("{0} {1}", id.x, (long)id.y - 3); }
)", "Test.Main", true);
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    MirReflection reflection;
    ReflectMirEntryPoint(compiled.Module, *compiled.Module.GetEntryPoints()[0], reflection);
    MARLIN_EXPECTF(reflection.DebugBufferOffset == 0u, "{0}", reflection.DebugBufferOffset);
    MARLIN_EXPECTF(reflection.ArgumentsSize == 0u, "{0}", reflection.ArgumentsSize);
}

MARLIN_TEST(SpirvBackend, DebugPrintChecksItsFormat)
{
    const SpirvCompiled compiled(R"(
namespace Test;
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { Debug.Print("{0} {2}", id.x, id.y); }
)", "Test.Main", true);
    MARLIN_EXPECT(!compiled.Ok);
    MARLIN_EXPECTF(compiled.Errors.AsView().Contains("references argument 2"), "{0}", compiled.Errors.AsView());
}

MARLIN_TEST(SpirvBackend, RayQueryValidates)
{
    const SpirvCompiled compiled(R"(
namespace Test;
public struct Args { public AccelerationStructure Scene; public Texture2D<float4, Access.ReadWrite> Output; }
[Compute(8, 8, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    RayDesc ray;
    ray.Origin = float3((float)id.x, (float)id.y, 0);
    ray.TMin = 0.001;
    ray.Direction = float3(0, 0, 1);
    ray.TMax = 1000;
    RayQuery<RayFlags.CullBackFacingTriangles> query;
    query.TraceRayInline(args.Scene, RayFlags.None, 0xFF, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CandidateType.Triangle && query.CandidateTriangleRayT() > 0.5)
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    float4 color = float4(0, 0, 0, 1);
    if (query.CommittedStatus() == CommittedStatus.Triangle)
    {
        float3x4 objectToWorld = query.CommittedObjectToWorld();
        color = float4(query.CommittedTriangleBarycentrics(), query.CommittedRayT(), objectToWorld[0][3]);
    }
    args.Output.Store((int2)id.xy, color);
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    MARLIN_EXPECT(compiled.Disassemble().AsView().Contains("OpRayQueryInitializeKHR"));
}

MARLIN_TEST(SpirvBackend, MeshPipelineValidates)
{
    constexpr StringView source = R"(
namespace Test;

public struct Payload { public uint Base; public uint Count; }
public struct Vertex { [Position] public float4 Position; public float2 Uv; }
public struct Primitive { [PrimitiveId] public uint Id; [CullPrimitive] public bool Culled; public uint Material; }
public struct Args { public Span<float4> Positions; public uint MeshletCount; }

[Amplification(32, 1, 1)]
void Task([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    Payload payload;
    payload.Base = id.x * 32;
    payload.Count = Min(args.MeshletCount, 32u);
    DispatchMesh(payload.Count, 1, 1, payload);
}

[Mesh(64, 1, 1, MaxVertices = 64, Topology = MeshTopology.Triangle)]
void Meshlet([GroupThreadId] uint3 thread, [Payload] in Payload payload, [Uniform] in Args args,
             out Vertices<Vertex, 64> verts, out Indices<uint3, 32> tris, out Primitives<Primitive, 32> prims)
{
    SetMeshOutputCounts(64, 32);
    uint i = thread.x;
    Vertex v;
    v.Position = args.Positions[payload.Base + i];
    v.Uv = float2((float)i, 0);
    verts[i] = v;
    verts[i].Uv.y = 1;
    if (i < 32)
    {
        tris[i] = uint3(i, i + 1, i + 2);
        prims[i].Id = i;
        prims[i].Culled = false;
        prims[i].Material = payload.Base;
    }
}

[Pixel] [return: Target(0)]
float4 Shade(in Vertex v, [PerPrimitive] in Primitive prim)
{
    return float4(v.Uv, (float)prim.Material, 1);
}
)";
    for (const StringView entry : {StringView("Test.Task"), StringView("Test.Meshlet"), StringView("Test.Shade")})
    {
        const SpirvCompiled compiled(source, entry);
        MARLIN_REQUIREF(compiled.Ok, "{0}: {1}\n{2}", entry, compiled.Errors.AsView(), compiled.Disassemble().AsView());
    }
}

MARLIN_TEST(SpirvBackend, RayTracingPipelineValidates)
{
    constexpr StringView source = R"(
namespace Test;

[RayPayload] public struct Radiance { public float3 Color; public uint Depth; }
[HitAttributes] public struct Sphere { public float3 Normal; }
public struct Record { public uint Material; }
public struct Args { public AccelerationStructure Scene; public Texture2D<float4, Access.ReadWrite> Output; }

RayDesc CameraRay(uint3 index)
{
    RayDesc ray;
    ray.Origin = float3((float)index.x, (float)index.y, -10);
    ray.TMin = 0.001;
    ray.Direction = float3(0, 0, 1);
    ray.TMax = 1000;
    return ray;
}

[RayGeneration]
void Trace([Uniform] in Args args)
{
    Radiance payload;
    payload.Color = float3(0, 0, 0);
    payload.Depth = 0;
    TraceRay(args.Scene, CameraRay(Launch.Index), ref payload);
    args.Output.Store((int2)Launch.Index.xy, float4(payload.Color, 1));
}

[RayGeneration] [Requires(Features.ShaderExecutionReorder)]
void TraceReordered([Uniform] in Args args)
{
    Radiance payload;
    payload.Color = float3(0, 0, 0);
    payload.Depth = 0;
    HitObject hit = HitObject.TraceRay(args.Scene, CameraRay(Launch.Index), ref payload);
    ReorderThread(hit, hit.IsHit() ? hit.InstanceId() : 0u, 4);
    hit.Invoke(ref payload);
    args.Output.Store((int2)Launch.Index.xy, float4(payload.Color, hit.HitT()));
}

[ClosestHit]
void Shade(ref Radiance payload, in TriangleAttributes attributes, [Uniform] in Record record)
{
    float3x4 objectToWorld = Hit.ObjectToWorld;
    payload.Color = float3(attributes.Barycentrics, (float)record.Material + objectToWorld[0][3] + Hit.T);
}

[AnyHit]
void AlphaTest(ref Radiance payload, in TriangleAttributes attributes)
{
    if (attributes.Barycentrics.x > 0.5)
    {
        IgnoreHit();
    }
}

[Miss]
void Sky(ref Radiance payload) { payload.Color = Hit.WorldRayDirection; }

[Intersection]
void Ball()
{
    Sphere sphere;
    sphere.Normal = Hit.ObjectRayDirection;
    ReportHit(Hit.RayTMin + 1, 0, sphere);
}
)";
    const StringView features[] = {"ShaderExecutionReorder"};
    for (const StringView entry : {StringView("Test.Trace"), StringView("Test.TraceReordered"), StringView("Test.Shade"),
                                   StringView("Test.AlphaTest"), StringView("Test.Sky"), StringView("Test.Ball")})
    {
        const SpirvCompiled compiled(source, entry, false, features);
        MARLIN_REQUIREF(compiled.Ok, "{0}: {1}\n{2}", entry, compiled.Errors.AsView(), compiled.Disassemble().AsView());
    }
}

MARLIN_TEST(SpirvBackend, CooperativeTypesValidate)
{
    constexpr StringView source = R"(
namespace Test;

public struct LayerArgs { public Ptr<byte> Weights; public Ptr<byte, Access.ReadWrite> Output; }

[Requires(Features.CooperativeVector)]
CoopVector<half, 16> Layer(in CoopVector<half, 32> input, Ptr<byte> weights)
{
    var w = new MatrixRef<half, 16, 32>(weights, MatrixLayout.InferencingOptimal);
    var b = new VectorRef<half, 16>(weights.Offset(1024));
    return Max(MatMulAdd(w, input, b), CoopVector<half, 16>(0));
}

[Compute(64, 1, 1)] [Requires(Features.CooperativeVector)]
void Infer([DispatchThreadId] uint3 id, [Uniform] in LayerArgs args)
{
    CoopVector<half, 32> input = CoopVector<half, 32>.Load(args.Weights.Offset(2048 + id.x * 64));
    CoopVector<half, 16> output = Layer(input, args.Weights) * (half)2;
    output.Store(args.Output.Offset(id.x * 32));
}

public struct GemmArgs { public Ptr<byte> A; public Ptr<byte> B; public Ptr<byte, Access.ReadWrite> C; public uint K; }

[Compute(32, 1, 1)] [Requires(Features.CooperativeMatrix)]
void Gemm([Uniform] in GemmArgs args)
{
    var acc = CoopMatrix<float, 16, 16, MatrixUse.Accumulator>(0);
    for (uint k = 0; k < args.K; k += 16)
    {
        var a = CoopMatrix<half, 16, 16, MatrixUse.A>.Load(args.A.Offset(k * 2), args.K * 2, MatrixLayout.RowMajor);
        var b = CoopMatrix<half, 16, 16, MatrixUse.B>.Load(args.B.Offset(k * 32), 32, MatrixLayout.RowMajor);
        acc = MulAdd(a, b, acc);
    }
    acc.Store(args.C, 64, MatrixLayout.RowMajor);
}
)";
    const StringView features[] = {"CooperativeVector", "CooperativeMatrix"};
    for (const StringView entry : {StringView("Test.Infer"), StringView("Test.Gemm")})
    {
        const SpirvCompiled compiled(source, entry, false, features);
        MARLIN_REQUIREF(compiled.Ok, "{0}: {1}\n{2}", entry, compiled.Errors.AsView(), compiled.Disassemble().AsView());
    }
}

MARLIN_TEST(SpirvBackend, SourceDebugInfoAndReflection)
{
    const SpirvCompiled compiled(R"(
namespace Test;
[Specialization] static bool UseFog = false;
public struct Args { public Span<float4, Access.ReadWrite> Output; public float Scale; }
public struct Particle { public float3 Position; public float Mass; }
float Weigh(Particle particle, float scale) { float weight = particle.Mass * scale; return weight; }
[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    Particle particle;
    particle.Position = float3((float)id.x, 0, 0);
    particle.Mass = 2;
    float4 result = float4(particle.Position, Weigh(particle, args.Scale));
    if (UseFog)
    {
        result.w = 0;
    }
    args.Output[id.x] = result;
}
)", "Test.Main", false, {}, true);
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    const String text = compiled.Disassemble();
    MARLIN_EXPECTF(text.AsView().Contains("NonSemantic.Shader.DebugInfo.100") && text.AsView().Contains("DebugDeclare"),
                   "{0}", text.AsView());

    MirReflection reflection;
    ReflectMirEntryPoint(compiled.Module, *compiled.Module.GetEntryPoints()[0], reflection);
    MARLIN_EXPECT(reflection.GroupSize[0] == 64 && reflection.ArgumentsSize == 24);
    const String printed = PrintMirReflection(reflection);
    MARLIN_EXPECTF(printed.AsView().Contains("uniform Scale") && printed.AsView().Contains("specialization Test.UseFog"),
                   "{0}", printed.AsView());
}

MARLIN_TEST(SpirvBackend, AutomaticDifferentiation)
{
    const SpirvCompiled compiled(R"(
namespace Test;

[Differentiable]
float Specular(float roughness, float3 n, float3 h)
{
    float a = roughness * roughness;
    float d = Max(Dot(n, h), 0.0);
    float denominator = d * d * (a * a - 1) + 1;
    if (roughness > 0.5)
    {
        denominator = denominator * 1.5;
    }
    return a * a / (3.14159 * denominator * denominator);
}

public struct Args { public Span<float4, Access.ReadWrite> Output; public float3 Normal; public float3 Half; }

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    float roughness = (float)id.x / 64;
    DifferentialPair<float> forward = Autodiff.Forward(Specular, new DifferentialPair<float>(roughness, 1),
                                                       new DifferentialPair<float3>(args.Normal, float3(0, 0, 0)), args.Half);
    var r = new DifferentialPair<float>(roughness, 0);
    var n = new DifferentialPair<float3>(args.Normal, float3(0, 0, 0));
    Autodiff.Backward(Specular, ref r, ref n, args.Half, 1);
    args.Output[id.x] = float4(forward.Primal, forward.Differential, r.Differential, n.Differential.x);
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
}

MARLIN_TEST(SpirvBackend, ReverseModeLoopsNeedABound)
{
    // The loop runs a number of times known only at run time, and its body needs x * i of every iteration.
    const SpirvCompiled compiled(R"(
namespace Test;
[Differentiable]
float Series(float x, uint count)
{
    float sum = 0;
    for (uint i = 0; i < count; i++) { sum += x * x * (float)i; }
    return sum;
}
public struct Args { public Span<float, Access.ReadWrite> Output; public uint Count; }
[Compute(1, 1, 1)]
void Main([Uniform] in Args args)
{
    var x = new DifferentialPair<float>(1, 0);
    Autodiff.Backward(Series, ref x, args.Count, 1);
    args.Output[0] = x.Differential;
}
)", "Test.Main");
    MARLIN_EXPECT(!compiled.Ok);
    MARLIN_EXPECTF(compiled.Errors.AsView().Contains("Test.mshader:7:5: cannot differentiate `Test.Series` (reverse "
                                                     "mode): the loop needs `[MaxIterations(n)]`"),
                   "{0}", compiled.Errors.AsView());
}

MARLIN_TEST(SpirvBackend, DifferentiableStructsCustomDerivativesAndLoops)
{
    // A differentiable struct with [NoDiff] fields, a [NoDiff] parameter, Autodiff.Detach, a function with custom
    // derivatives that is not differentiable itself, a bounded loop in reverse mode, and generic code over
    // IDifferentiable (§9.12).
    const SpirvCompiled compiled(R"(
namespace Test;

public struct Material
{
    public float3 Albedo;
    public float Roughness;
    [NoDiff] public uint Layer;
    [NoDiff] public float Seed;
}

// The bits of x * x: no derivative of its own, so its derivatives say what it means.
[ForwardDerivative(nameof(SquareForward)), BackwardDerivative(nameof(SquareBackward))]
float Square(float x) => AsFloat(AsUInt(x * x));

DifferentialPair<float> SquareForward(DifferentialPair<float> x)
    => new DifferentialPair<float>(Square(x.Primal), 2 * x.Primal * x.Differential);

void SquareBackward(ref DifferentialPair<float> x, float dResult)
{
    x = new DifferentialPair<float>(x.Primal, 2 * x.Primal * dResult);
}

[Differentiable]
float Shade(Material m, [NoDiff] float exposure, uint count)
{
    float sum = 0;
    [MaxIterations(16)]
    for (uint i = 0; i < count; i++)
    {
        sum = sum * m.Roughness + Square(Dot(m.Albedo, float3(1, 1, 1))) + m.Seed * (float)m.Layer;
    }
    return sum * exposure + Autodiff.Detach(m.Roughness) * m.Roughness;
}

T.Differential Blend<T>(T.Differential a, T.Differential b, float t) where T : IDifferentiable
    => T.DifferentialAdd(T.DifferentialScale(a, 1 - t), T.DifferentialScale(b, t));

public struct Args { public Span<float4, Access.ReadWrite> Output; public float3 Albedo; public uint Count; }

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    var m = new Material { Albedo = args.Albedo, Roughness = (float)id.x / 64, Layer = 3, Seed = 1 };
    Material.Differential direction = new Material { Albedo = float3(1, 0, 0), Roughness = 1 };
    DifferentialPair<float> forward = Autodiff.Forward(Shade, new DifferentialPair<Material>(m, direction), 2.0,
                                                       args.Count);
    var gradient = new DifferentialPair<Material>(m, Material.DifferentialZero);
    Autodiff.Backward(Shade, ref gradient, 2.0, args.Count, 1);
    Material mixed = Blend<Material>(gradient.Differential, direction, 0.5);
    args.Output[id.x] = float4(forward.Primal, forward.Differential, gradient.Differential.Roughness, mixed.Albedo.x);
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    // The loop runs forward, then again in the reverse derivative to keep what each of its at most 16 trips
    // computed (in arrays with a slot more, for the test that ends it), then backwards.
    const String text = compiled.Disassemble();
    usize loops = 0;
    for (usize at = text.AsView().IndexOf("OpLoopMerge"); at != StringView::NotFound;
         at = text.AsView().IndexOf("OpLoopMerge", at + 1))
    {
        ++loops;
    }
    MARLIN_EXPECTF(loops == 3 && text.AsView().Contains("OpTypeArray %float %uint_17"), "{0}", text.AsView());
}

MARLIN_TEST(SpirvBackend, InterfaceRequirementsAndExtensionsValidate)
{
    // Static requirements, `this` in type positions, associated types and extensions of a struct, a vector and a
    // texture (§3.5), monomorphized into one pixel shader.
    const SpirvCompiled compiled(R"(
namespace Test;

public interface ILightSample { half3 Radiance(); }

public struct SpotSample : ILightSample
{
    public half3 Value;
    public half3 Radiance() => Value;
}

public interface ILight
{
    type Sample : ILightSample;
    static abstract this Default { get; }
    static abstract this Create(half intensity);
    static abstract this operator +(this a, this b);
    Sample SampleAt(float2 uv);
}

public struct SpotLight : ILight
{
    public half Intensity;
    public type Sample = SpotSample;
    public static SpotLight Default => SpotLight.Create(1);
    public static SpotLight Create(half intensity) => new SpotLight { Intensity = intensity };
    public static SpotLight operator +(SpotLight a, SpotLight b) => Create(a.Intensity + b.Intensity);
    public Sample SampleAt(float2 uv) => new SpotSample { Value = half3(half2(uv), 1) * Intensity };
}

public interface ILuminance
{
    half Luminance();
    static abstract this Zero { get; }
}

public extension half3 : ILuminance
{
    public half Luminance() => Dot(this, half3(0.2126, 0.7152, 0.0722));
    public static half3 Zero => half3(0, 0, 0);
}

public extension SpotSample : ILuminance
{
    public half Luminance() => this.Value.Luminance();
    public static this Zero => new SpotSample { Value = half3.Zero };
}

public extension<T> Texture2D<T> where T : IFloatingPoint
{
    public T SampleBlurred(Sampler s, float2 uv, float2 step)
    {
        return (this.Sample(s, uv - step) + this.Sample(s, uv + step)) * 0.5;
    }
}

half3 Light<T>(float2 uv) where T : ILight
{
    T light = T.Default + T.Create(0.5);
    T.Sample sample = light.SampleAt(uv);
    return sample.Radiance();
}

half Brightest<T>(T a, T b) where T : ILuminance => Max(Max(a.Luminance(), b.Luminance()), T.Zero.Luminance());

public struct Args { public Texture2D<half4> Image; public Sampler Linear; }
public struct Varyings { [Position] public float4 Position; public float2 Uv; }

[Pixel] [return: Target(0)]
half4 Main(in Varyings v, [Uniform] in Args args)
{
    half4 texel = args.Image.SampleBlurred(args.Linear, v.Uv, float2(0.01, 0));
    half3 lit = Light<SpotLight>(v.Uv);
    SpotSample sample = new SpotSample { Value = lit };
    half luma = Brightest(texel.rgb, lit) + Brightest(sample, SpotSample.Zero);
    return half4(texel.rgb * luma, 1);
}
)", "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}\n{1}", compiled.Errors.AsView(), compiled.Disassemble().AsView());
    MARLIN_EXPECT(compiled.Disassemble().AsView().Contains("OpImageSampleImplicitLod"));
}
} // namespace Marlin::Tests
