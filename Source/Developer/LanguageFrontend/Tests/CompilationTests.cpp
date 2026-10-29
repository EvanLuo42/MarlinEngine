#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirPrinter.h"
#include "LanguageIR/MirVerifier.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
/// Compiles *source*, at /Project/Language/Shaders/Test.mshader, against the mounted engine's core library.
struct Compiled
{
    explicit Compiled(StringView source, StringView entryPoint = "Test.Main", List<String> features = {})
    {
        Memory.Add("/Project/Language/Shaders/Test.mshader", source);
        ISourceProvider *providers[] = {&Engine, &Memory};
        Sources = MakeUnique<CompositeSourceProvider>(Span<ISourceProvider *const>(providers));
        Options.Features = Move(features);
        Compilation = MakeUnique<LanguageCompilation>(*Sources, Options);
        Loaded = Compilation->Load();
        if (Loaded && !entryPoint.IsEmpty())
        {
            Ok = Compilation->CompileEntryPoint(entryPoint, Module);
        }
    }

    [[nodiscard]] String Errors() const
    {
        return Compilation->GetDiagnostics().ToString();
    }

    [[nodiscard]] bool HasError(DiagnosticCode code) const
    {
        for (const LanguageDiagnostic &diagnostic : Compilation->GetDiagnostics().GetAll())
        {
            if (diagnostic.Code == code)
            {
                return true;
            }
        }
        return false;
    }

    static constexpr StringView EngineRoots[] = {"/Engine"};
    VfsSourceProvider Engine{EngineRoots};
    MemorySourceProvider Memory;
    UniquePtr<CompositeSourceProvider> Sources;
    LanguageCompileOptions Options;
    UniquePtr<LanguageCompilation> Compilation;
    MirModule Module;
    bool Loaded = false;
    bool Ok = false;
};
} // namespace

MARLIN_TEST(LanguageSema, ComputeShaderLowersToMir)
{
    const Compiled compiled(R"(
namespace Test;

public struct Args
{
    public Span<float, Access.ReadWrite> Output;
    public uint Count;
    public float Scale;
}

float Shape(float x) => Sqrt(x) * 2.0 + Abs(-x);

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    if (id.x >= args.Count)
    {
        return;
    }
    float sum = 0;
    for (uint i = 0; i < 4 && i <= id.x; ++i)
    {
        sum += Shape(float(id.x - i));
    }
    args.Output[id.x] = sum * args.Scale;
}
)");
    MARLIN_REQUIREF(compiled.Ok, "{0}", compiled.Errors().AsView());
    MARLIN_REQUIRE(compiled.Module.GetEntryPoints().Count() == 1);
    const MirEntryPoint &entry = *compiled.Module.GetEntryPoints()[0];
    MARLIN_EXPECT(entry.Stage == MirStage::Compute && entry.GroupSize[0] == 64);
    MARLIN_EXPECT(entry.Interface.Count() == 2);
    StringBuilder<4096> text;
    PrintMirModule(compiled.Module, text);
    MARLIN_EXPECTF(text.AsView().Contains("Sqrt f32") && text.AsView().Contains("BufferPointer") &&
                       text.AsView().Contains("loop merge"),
                   "{0}", text.AsView());
}

MARLIN_TEST(LanguageSema, GenericsInferAroundConstants)
{
    const Compiled compiled(R"(
namespace Test;

public const int Log2(int n) => n <= 1 ? 0 : 1 + Log2(n / 2);

public struct Args { public Span<half4, Access.ReadWrite> Output; }

[Compute(Log2(256), 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    half3 color = half3(0.5h, -1, 2);
    half3 clamped = Max(color, 0.0);
    half luma = Dot(clamped, half3(0.2126, 0.7152, 0.0722));
    static if (Device.Supports(Features.CooperativeVector))
    {
        luma = 0;
    }
    args.Output[id.x] = half4(clamped, luma);
}
)");
    MARLIN_REQUIREF(compiled.Ok, "{0}", compiled.Errors().AsView());
    MARLIN_EXPECT(compiled.Module.GetEntryPoints()[0]->GroupSize[0] == 8);
}

MARLIN_TEST(LanguageSema, ReportsQueriedFeatures)
{
    constexpr StringView Source = R"(
namespace Test;
public struct Args { public Span<uint, Access.ReadWrite> Output; }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    uint value = 0;
    static if (Device.Supports(Features.ShaderExecutionReorder))
    {
        value = 1;
    }
    else static if (Device.Supports(Features.CooperativeVector))
    {
        value = 2;
    }
    args.Output[id.x] = value;
}
)";
    const Compiled without(Source);
    MARLIN_REQUIREF(without.Ok, "{0}", without.Errors().AsView());
    List<String> queried;
    without.Compilation->GetQueriedFeatures(queried);
    MARLIN_REQUIRE(queried.Count() == 2);
    MARLIN_EXPECT(queried[0] == "CooperativeVector" && queried[1] == "ShaderExecutionReorder");

    const Compiled with(Source, "Test.Main", {String("ShaderExecutionReorder")});
    MARLIN_REQUIREF(with.Ok, "{0}", with.Errors().AsView());
    queried.Clear();
    with.Compilation->GetQueriedFeatures(queried);
    MARLIN_REQUIRE(queried.Count() == 1);
    MARLIN_EXPECT(queried[0] == "ShaderExecutionReorder");
}

MARLIN_TEST(LanguageSema, ReportsNamesAndOverloads)
{
    const Compiled compiled(R"(
namespace Test;
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id)
{
    float3 v = float3(1, 2);
    float x = Missing + 1;
    float y = Dot(v, float2(1, 2));
}
)");
    MARLIN_EXPECT(!compiled.Ok);
    MARLIN_EXPECTF(compiled.HasError(DiagnosticCode::WrongArgumentCount), "{0}", compiled.Errors().AsView());
    MARLIN_EXPECTF(compiled.HasError(DiagnosticCode::UndefinedName), "{0}", compiled.Errors().AsView());
    MARLIN_EXPECTF(compiled.HasError(DiagnosticCode::NoMatchingOverload), "{0}", compiled.Errors().AsView());
    MARLIN_EXPECT(compiled.Errors().AsView().Contains("error[M3001]: cannot find `Missing` in this scope\n"));
}

MARLIN_TEST(LanguageSema, ChecksStagesFeaturesAndRecursion)
{
    const Compiled stages(R"(
namespace Test;
float Slope(float x) => Derivative.X(x);
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { float s = Slope(id.x); }
)");
    MARLIN_EXPECTF(stages.HasError(DiagnosticCode::NotInStage), "{0}", stages.Errors().AsView());

    const Compiled features(R"(
namespace Test;
[Requires(Features.ShaderExecutionReorder)] void Reorder() {}
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { Reorder(); }
)");
    MARLIN_EXPECTF(features.HasError(DiagnosticCode::FeatureRequired), "{0}", features.Errors().AsView());

    const Compiled guarded(R"(
namespace Test;
[Requires(Features.ShaderExecutionReorder)] void Reorder() {}
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id)
{
    static if (Device.Supports(Features.ShaderExecutionReorder)) { Reorder(); }
}
)", "Test.Main", {String("ShaderExecutionReorder")});
    MARLIN_EXPECTF(guarded.Ok, "{0}", guarded.Errors().AsView());

    // A non-public function infers the features it uses; its guarded caller compiles, an unguarded entry point does
    // not, and a public function must declare them (§9.7).
    const Compiled inferred(R"(
namespace Test;
[Requires(Features.ShaderExecutionReorder)] void Reorder() {}
void Helper() { Reorder(); }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id)
{
    static if (Device.Supports(Features.ShaderExecutionReorder)) { Helper(); }
}
)", "Test.Main", {String("ShaderExecutionReorder")});
    MARLIN_EXPECTF(inferred.Ok, "{0}", inferred.Errors().AsView());

    const Compiled unguarded(R"(
namespace Test;
[Requires(Features.ShaderExecutionReorder)] void Reorder() {}
void Helper() { Reorder(); }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { Helper(); }
)");
    MARLIN_EXPECTF(unguarded.Errors().AsView().Contains("`Helper` needs the feature `ShaderExecutionReorder`"), "{0}",
                   unguarded.Errors().AsView());

    const Compiled undeclared(R"(
namespace Test;
[Requires(Features.ShaderExecutionReorder)] void Reorder() {}
public void Helper() { Reorder(); }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id)
{
    static if (Device.Supports(Features.ShaderExecutionReorder)) { Helper(); }
}
)", "Test.Main", {String("ShaderExecutionReorder")});
    MARLIN_EXPECTF(undeclared.Errors().AsView().Contains("`Reorder` needs the feature `ShaderExecutionReorder`"), "{0}",
                   undeclared.Errors().AsView());

    const Compiled recursion(R"(
namespace Test;
int Down(int n) => n == 0 ? 0 : Down(n - 1);
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { int r = Down((int)id.x); }
)");
    MARLIN_EXPECTF(recursion.HasError(DiagnosticCode::RecursionNotAllowed), "{0}", recursion.Errors().AsView());
}

MARLIN_TEST(LanguageSema, ChecksFlow)
{
    const Compiled compiled(R"(
namespace Test;
float Pick(bool c) { if (c) { return 1; } }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id)
{
    float x;
    float y = x + Pick(id.x == 0);
}
)");
    MARLIN_EXPECTF(compiled.HasError(DiagnosticCode::MissingReturn), "{0}", compiled.Errors().AsView());
    MARLIN_EXPECTF(compiled.HasError(DiagnosticCode::UnassignedVariable), "{0}", compiled.Errors().AsView());
}

MARLIN_TEST(LanguageSema, VertexAndPixelStagesLinkByStruct)
{
    const Compiled vertex(R"(
namespace Test;
public struct Varyings
{
    [Position] public float4 Position;
    public float2 Uv;
    [NoInterpolation] public uint Layer;
}
[Vertex]
Varyings Main([VertexId] uint id)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return new Varyings { Position = float4(uv * 2 - 1, 0, 1), Uv = uv, Layer = 0 };
}
[Pixel] [return: Target(0)]
half4 Shade(in Varyings v) => half4(half2(v.Uv), 0, 1);
)");
    MARLIN_REQUIREF(vertex.Ok, "{0}", vertex.Errors().AsView());
    MirModule pixelModule;
    MARLIN_REQUIREF(vertex.Compilation->CompileEntryPoint("Test.Shade", pixelModule), "{0}",
                    vertex.Errors().AsView());
    MARLIN_EXPECT(pixelModule.GetEntryPoints()[0]->Stage == MirStage::Pixel);
}
MARLIN_TEST(LanguageSema, ShaderPipelineKeepsLoweredCodeValid)
{
    Compiled compiled(R"(
namespace Test;
public struct Args { public Span<float4, Access.ReadWrite> Output; public uint Count; }
public struct Light { public float3 Direction; public float Intensity; }

float Shade(in Light light, float3 normal)
{
    float lambert = Dot(normal, -light.Direction);
    if (lambert <= 0)
    {
        return 0;
    }
    return lambert * light.Intensity;
}

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    Light light = new Light { Direction = Normalize(float3(1, -1, 0)), Intensity = 2 };
    float total = 0;
    for (uint i = 0; i < 3; ++i)
    {
        total += Shade(light, float3(0, 1, 0)) * (i == 1 ? 0.5 : 1.0);
    }
    args.Output[id.x] = float4(total, total * 2, 0, 1);
}
)");
    MARLIN_REQUIREF(compiled.Ok, "{0}", compiled.Errors().AsView());
    const MirPassRegistry registry = MirPassRegistry::CreateDefault();
    MirModulePassManager passes({.VerifyEach = true});
    String error;
    MARLIN_REQUIRE(registry.ParsePipeline(MirShaderPipeline, passes, error, {.VerifyEach = true}));
    MirModuleAnalysisManager analyses;
    passes.Run(compiled.Module, analyses);
    StringBuilder<8192> text;
    PrintMirModule(compiled.Module, text);
    List<String> problems;
    MARLIN_REQUIREF(VerifyMirModule(compiled.Module, problems), "{0}\n{1}",
                    problems.IsEmpty() ? StringView() : problems[0].AsView(), text.AsView());
    MARLIN_EXPECTF(compiled.Module.GetFunctions().Count() == 1, "{0}", text.AsView());
    MARLIN_EXPECTF(!text.AsView().Contains("Variable"), "{0}", text.AsView());
}

MARLIN_TEST(LanguageSema, SourceSetsShareTheCommonFolder)
{
    MARLIN_EXPECT(IsCoreLibraryPath("/Engine/Language/Common/Core/Marlin.mcommon"));
    MARLIN_EXPECT(IsCoreLibraryPath("/Engine/Language/Scripts/Core/Marlin.mscript"));
    MARLIN_EXPECT(!IsCoreLibraryPath("/Engine/Language/Shaders/Passes/Core/Blit.mshader"));
    MARLIN_EXPECT(!IsCoreLibraryPath("/Project/Language/Shaders/Core/Marlin.mshader"));

    for (const LanguageDialect language : {LanguageDialect::Shader, LanguageDialect::Script})
    {
        static constexpr StringView EngineRoots[] = {"/Engine"};
        VfsSourceProvider engine{EngineRoots};
        MemorySourceProvider memory;
        memory.Add("/Project/Language/Common/Shared.mcommon", "namespace Shared;\npublic float Twice(float x) => x * 2;\n");
        ISourceProvider *providers[] = {&engine, &memory};
        CompositeSourceProvider sources{Span<ISourceProvider *const>(providers)};
        LanguageCompileOptions options;
        options.Language = language;
        LanguageCompilation compilation(sources, options);
        MARLIN_EXPECTF(compilation.Load(), "{0}", compilation.GetDiagnostics().ToString().AsView());
    }

    static constexpr StringView EngineRoots[] = {"/Engine"};
    VfsSourceProvider engine{EngineRoots};
    MemorySourceProvider memory;
    memory.Add("/Project/Language/Shaders/Shared.mcommon", "namespace Shared;\n");
    ISourceProvider *providers[] = {&engine, &memory};
    CompositeSourceProvider sources{Span<ISourceProvider *const>(providers)};
    LanguageCompilation compilation(sources, LanguageCompileOptions{});
    MARLIN_EXPECT(!compilation.Load());
    const String errors = compilation.GetDiagnostics().ToString();
    MARLIN_EXPECTF(errors.AsView().Contains("Language/Common/"), "{0}", errors.AsView());
}

MARLIN_TEST(LanguageSema, NameDependenciesSeeShadowingFiles)
{
    const Compiled compiled(R"(
namespace Test;

public struct Args { public Span<float, Access.ReadWrite> Output; }

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    args.Output[id.x] = Sqrt(float(id.x));
}
)");
    MARLIN_REQUIREF(compiled.Ok, "{0}", compiled.Errors().AsView());
    LanguageNameDependencies names;
    compiled.Compilation->GetNameDependencies(names);
    MARLIN_REQUIRE(!names.Names.IsEmpty());

    const auto affects = [&](StringView text) {
        LanguageDeclaredNames declared;
        GetDeclaredNames("/Project/Language/Shaders/New.mshader", String(text), declared);
        return AffectsNames(declared, names);
    };
    // `Sqrt` was found in Marlin; one declared in Test is nearer and would be found instead.
    MARLIN_EXPECT(affects("namespace Test; public float Sqrt(float x) => x;"));
    MARLIN_EXPECT(affects("namespace Test { public struct Args { } }"));
    MARLIN_EXPECT(!affects("namespace Other; public float Sqrt(float x) => x;"));
    MARLIN_EXPECT(!affects("namespace Test; public struct Unrelated { public float Value; }"));
    // Test already exists, so another declaration of it only adds to it.
    MARLIN_EXPECT(!affects("namespace Test.Inner; public struct Unrelated { }"));
}
} // namespace Marlin::Tests
