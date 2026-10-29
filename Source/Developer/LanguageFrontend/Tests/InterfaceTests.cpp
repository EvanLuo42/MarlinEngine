#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"
#include "LanguageFrontend/Parser.h"
#include "LanguageIR/MirPrinter.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
/// Compiles *entryPoint* of *source*, at /Project/Language/Shaders/Test.mshader, with *common* at
/// /Project/Language/Common/Test.mcommon when given, against the mounted engine's core library.
struct CompiledShader
{
    explicit CompiledShader(StringView source, StringView entryPoint = "Test.Main", StringView common = {})
    {
        Memory.Add("/Project/Language/Shaders/Test.mshader", source);
        if (!common.IsEmpty())
        {
            Memory.Add("/Project/Language/Common/Test.mcommon", common);
        }
        ISourceProvider *providers[] = {&Engine, &Memory};
        Sources = MakeUnique<CompositeSourceProvider>(Span<ISourceProvider *const>(providers));
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

/// Checks *source*, at /Project/Language/Scripts/Test.mscript, as the Project package of a Script compilation.
struct CheckedPackage
{
    explicit CheckedPackage(StringView source)
    {
        Memory.Add("/Project/Language/Scripts/Test.mscript", source);
        ISourceProvider *providers[] = {&Engine, &Memory};
        Sources = MakeUnique<CompositeSourceProvider>(Span<ISourceProvider *const>(providers));
        Options.Language = LanguageDialect::Script;
        Compilation = MakeUnique<LanguageCompilation>(*Sources, Options);
        Ok = Compilation->Load() && Compilation->CompileScriptPackage("Project", Module);
    }

    [[nodiscard]] String Errors() const
    {
        return Compilation->GetDiagnostics().ToString();
    }

    static constexpr StringView EngineRoots[] = {"/Engine"};
    VfsSourceProvider Engine{EngineRoots};
    MemorySourceProvider Memory;
    UniquePtr<CompositeSourceProvider> Sources;
    LanguageCompileOptions Options;
    UniquePtr<LanguageCompilation> Compilation;
    MirModule Module;
    bool Ok = false;
};

/// The light interface of §3.5: an associated type, static requirements, a `this`-typed operator, and a struct
/// implementing them.
constexpr StringView LightSource = R"(
public interface ILightSample
{
    float3 Radiance();
}

public struct SpotSample : ILightSample
{
    public float3 Value;
    public float3 Radiance() => Value;
}

public interface ILight
{
    type Sample : ILightSample;
    static abstract this Default { get; }
    static abstract this Create(float intensity);
    Sample SampleAt(float3 position, float2 u);
    static abstract this operator +(this a, this b);
}

public struct SpotLight : ILight
{
    public float Intensity;
    public type Sample = SpotSample;
    public static SpotLight Default => new SpotLight { Intensity = 1 };
    public static SpotLight Create(float intensity) => new SpotLight { Intensity = intensity };
    public Sample SampleAt(float3 position, float2 u) => new SpotSample { Value = position * Intensity + float3(u, 0) };
    public static SpotLight operator +(SpotLight a, SpotLight b) => new SpotLight { Intensity = a.Intensity + b.Intensity };
}

float3 Accumulate<T>(T light, float3 p) where T : ILight
{
    T.Sample sample = light.SampleAt(p, float2(0.5, 0.5));
    T total = T.Default + T.Create(2) + light;
    return sample.Radiance() + total.SampleAt(p, float2(0, 0)).Radiance();
}
)";
} // namespace

MARLIN_TEST(LanguageParser, InterfaceRequirementsAndExtensions)
{
    SourceFile file(1, "/Project/Language/Shaders/Test.mshader", String(R"(
public interface ILight
{
    type Sample : ILightSample, IOther;
    static abstract this Default { get; }
    static abstract this operator +(this a, this b);
}
public struct Spot : ILight { public type Sample = SpotSample; }
public extension<T> Texture2D<T> : IFoo where T : IFloatingPoint
{
    public this Twice() { this result = this; return result; }
}
public extension Make(float x) => x;
)"),
                    LanguageDialect::Shader);
    Arena memory;
    LanguageDiagnostics diagnostics;
    diagnostics.AddFile(file);
    AstFile *tree = ParseFile(file, memory, diagnostics, {});
    MARLIN_REQUIREF(!diagnostics.HasErrors(), "{0}", diagnostics.ToString().AsView());
    MARLIN_REQUIRE(tree->Members.Count() == 4);

    const auto *light = tree->Members[0]->As<AstInterfaceDecl>();
    MARLIN_REQUIRE(light->Members.Count() == 3);
    const auto *sample = light->Members[0]->As<AstTypeDecl>();
    MARLIN_EXPECT(sample->Name == "Sample" && sample->Bounds.Count() == 2 && sample->Target == nullptr);
    const auto *defaultValue = light->Members[1]->As<AstPropertyDecl>();
    MARLIN_EXPECT(defaultValue->Type->Kind == AstKind::SelfType &&
                  HasAnyFlags(defaultValue->Modifiers, AstModifiers::Static | AstModifiers::Abstract));
    const auto *plus = light->Members[2]->As<AstOperatorDecl>();
    MARLIN_REQUIRE(plus->Parameters.Count() == 2);
    MARLIN_EXPECT(plus->ReturnType->Kind == AstKind::SelfType && plus->Parameters[0]->Type->Kind == AstKind::SelfType &&
                  !HasAnyFlags(plus->Parameters[0]->Modifiers, AstModifiers::This));

    const auto *binding = tree->Members[1]->As<AstStructDecl>()->Members[0]->As<AstTypeDecl>();
    MARLIN_EXPECT(binding->Target != nullptr && binding->Bounds.IsEmpty());

    const auto *extension = tree->Members[2]->As<AstExtensionDecl>();
    MARLIN_EXPECT(extension->Name == "Texture2D<T>" && extension->GenericParameters.Count() == 1 &&
                  extension->Interfaces.Count() == 1 && extension->Constraints.Count() == 1);
    MARLIN_EXPECT(extension->ExtendedType->Kind == AstKind::NamedType && extension->Members.Count() == 1);
    const auto *twice = extension->Members[0]->As<AstFunctionDecl>();
    MARLIN_EXPECT(twice->ReturnType->Kind == AstKind::SelfType);
    const auto *local = twice->Body.Block->Statements[0]->As<AstVarStmt>();
    MARLIN_EXPECT(local->Type->Kind == AstKind::SelfType);

    // `extension` stays an identifier where it does not start an extension: here, the return type of `Make`.
    MARLIN_EXPECT(tree->Members[3]->As<AstFunctionDecl>()->Name == "Make");
}

MARLIN_TEST(LanguageSema, StaticRequirementsAndAssociatedTypes)
{
    StringBuilder<4096> source;
    source.Append("namespace Test;\n").Append(LightSource).Append(R"(
// The core library's extension gives every numeric type `T.Zero` and `T.One`.
T Affine<T>(T a) where T : INumeric => a * T.One + T.Zero;

public struct Args { public Span<float4, Access.ReadWrite> Output; }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    args.Output[id.x] = float4(Accumulate(SpotLight.Create(3), float3(1, 2, 3)) + Affine(float3(1, 2, 3)), (float)Affine(2));
}
)");
    const CompiledShader compiled(source.AsView());
    MARLIN_REQUIREF(compiled.Ok, "{0}", compiled.Errors().AsView());
    StringBuilder<16384> text;
    PrintMirModule(compiled.Module, text);
    // Monomorphization calls SpotLight's own static members and operator.
    MARLIN_EXPECTF(text.AsView().Contains("Test.SpotLight.Create") &&
                       text.AsView().Contains("Test.SpotLight.Default") &&
                       text.AsView().Contains("Test.SpotLight.operator"),
                   "{0}", text.AsView());
}

MARLIN_TEST(LanguageSema, ExtensionsAddMembersAndConformances)
{
    const CompiledShader compiled(R"(
namespace Test;

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

public struct Tint { public half3 Color; }

public extension Tint : ILuminance
{
    public type Channel = half;
    public half Luminance() => this.Color.Luminance();
    public static this Zero => new Tint { Color = half3.Zero };
    public this Scaled(half k) { this result = this; result.Color = result.Color * k; return result; }
    public static Tint operator *(Tint a, half k) => a.Scaled(k);
}

public extension<T> Texture2D<T> where T : IFloatingPoint
{
    public T SampleTwice(Sampler s, float2 uv) => this.Sample(s, uv) + this.Sample(s, uv + float2(0.5, 0));
}

half Brightest<T>(T a, T b) where T : ILuminance
{
    T zero = T.Zero;
    static if (T.Implements(nameof(ILuminance.Luminance)))
    {
        return Max(a.Luminance(), Max(b.Luminance(), zero.Luminance()));
    }
    return 0;
}

public struct Args { public Texture2D<half4> Image; public Sampler Linear; }
public struct Varyings { [Position] public float4 Position; public float2 Uv; }

[Pixel] [return: Target(0)]
half4 Main(in Varyings v, [Uniform] in Args args)
{
    half4 texel = args.Image.SampleTwice(args.Linear, v.Uv);
    Tint tint = new Tint { Color = texel.rgb } * 0.5h;
    Tint.Channel luma = Brightest(texel.rgb, half3(0.5, 0.5, 0.5)) + Brightest(tint, Tint.Zero);
    return half4(texel.rgb * luma, 1);
}
)");
    MARLIN_REQUIREF(compiled.Ok, "{0}", compiled.Errors().AsView());
    StringBuilder<16384> text;
    PrintMirModule(compiled.Module, text);
    MARLIN_EXPECTF(text.AsView().Contains("Luminance") && text.AsView().Contains("SampleTwice"), "{0}", text.AsView());
}

MARLIN_TEST(LanguageSema, CommonSubsetDeclaresInterfacesAndExtensions)
{
    const CompiledShader compiled(R"(
namespace Test;
using Shared;
public struct Args { public Span<float, Access.ReadWrite> Output; }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    args.Output[id.x] = Total(float3(1, 2, 3)) + Total(new Weight { Value = 2 });
}
)",
                                  "Test.Main", R"(
namespace Shared;
public interface ISummable
{
    type Scalar;
    static abstract this Identity { get; }
    float Sum();
}
public struct Weight { public float Value; }
public extension Weight : ISummable
{
    public type Scalar = float;
    public static Weight Identity => new Weight { Value = 0 };
    public float Sum() => this.Value;
}
public extension float3 : ISummable
{
    public type Scalar = float;
    public static float3 Identity => float3(0, 0, 0);
    public float Sum() => this.x + this.y + this.z;
}
public float Total<T>(T value) where T : ISummable
{
    T.Scalar identity = T.Identity.Sum();
    return value.Sum() + identity;
}
)");
    MARLIN_REQUIREF(compiled.Ok, "{0}", compiled.Errors().AsView());
}

MARLIN_TEST(LanguageSema, ReportsUnmetRequirements)
{
    const CompiledShader missingStatic(R"(
namespace Test;
public interface IMake { static abstract this Make(); float Value(); }
public struct Thing : IMake { public float Value() => 1; }
float Use<T>() where T : IMake => T.Make().Value();
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { float x = Use<Thing>(); }
)");
    MARLIN_EXPECT(!missingStatic.Ok);
    MARLIN_EXPECTF(missingStatic.HasError(DiagnosticCode::MissingInterfaceMember), "{0}",
                   missingStatic.Errors().AsView());
    MARLIN_EXPECTF(missingStatic.Errors().AsView().Contains("does not implement `IMake.Make`"), "{0}",
                   missingStatic.Errors().AsView());

    const CompiledShader missingOperator(R"(
namespace Test;
public interface IAdd { static abstract this operator +(this a, this b); }
public struct Thing : IAdd { public float Value; }
T Twice<T>(T x) where T : IAdd => x + x;
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { Thing t = Twice(new Thing()); }
)");
    MARLIN_EXPECTF(
        missingOperator.Errors().AsView().Contains("error[M3021]: `Thing` does not implement `IAdd.operator +`"), "{0}",
        missingOperator.Errors().AsView());

    const CompiledShader missingBinding(R"(
namespace Test;
public interface IHasSample { type Sample; }
public struct Thing : IHasSample { }
void Use<T>() where T : IHasSample { }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { Use<Thing>(); }
)");
    MARLIN_EXPECTF(missingBinding.HasError(DiagnosticCode::MissingAssociatedType), "{0}",
                   missingBinding.Errors().AsView());

    const CompiledShader badBinding(R"(
namespace Test;
public interface ISample { float3 Radiance(); }
public interface IHasSample { type Sample : ISample; }
public struct Thing : IHasSample { public type Sample = float; }
void Use<T>() where T : IHasSample { }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { Use<Thing>(); }
)");
    MARLIN_EXPECTF(badBinding.HasError(DiagnosticCode::ConstraintNotSatisfied), "{0}", badBinding.Errors().AsView());
    MARLIN_EXPECTF(badBinding.Errors().AsView().Contains("`float` does not satisfy `ISample`"), "{0}",
                   badBinding.Errors().AsView());

    const CompiledShader unconstrained(R"(
namespace Test;
public interface ILuma { half Luma(); }
public struct Plain { }
half Use<T>(T value) where T : ILuma => value.Luma();
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { half x = Use(new Plain()); }
)");
    MARLIN_EXPECTF(unconstrained.HasError(DiagnosticCode::NoMatchingOverload) ||
                       unconstrained.HasError(DiagnosticCode::ConstraintNotSatisfied),
                   "{0}", unconstrained.Errors().AsView());
}

MARLIN_TEST(LanguageSema, ReportsInvalidExtensions)
{
    const CompiledShader twice(R"(
namespace Test;
public interface ILuma { half Luma(); }
public struct Color : ILuma { public half3 Value; public half Luma() => Value.x; }
public extension Color : ILuma { }
public extension half3 : ILuma { public half Luma() => this.x; }
public extension Vector<half, 3> : ILuma { }
half Use<T>(T value) where T : ILuma => value.Luma();
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { half x = Use(new Color()) + Use(half3(1, 2, 3)); }
)");
    MARLIN_EXPECT(!twice.Ok);
    uint32 duplicates = 0;
    for (const LanguageDiagnostic &diagnostic : twice.Compilation->GetDiagnostics().GetAll())
    {
        duplicates += diagnostic.Code == DiagnosticCode::DuplicateConformance ? 1 : 0;
    }
    MARLIN_EXPECTF(duplicates == 2, "{0}", twice.Errors().AsView());

    const CompiledShader field(R"(
namespace Test;
public extension half3 { public float Extra; public static readonly half Scale = 2; }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { }
)");
    MARLIN_EXPECT(!field.Loaded);
    MARLIN_EXPECTF(field.HasError(DiagnosticCode::InvalidExtensionMember), "{0}", field.Errors().AsView());
    MARLIN_EXPECTF(field.Errors().AsView().Contains("an extension cannot add fields"), "{0}", field.Errors().AsView());

    const CompiledShader selfOutside(R"(
namespace Test;
public struct Thing { public this Copy() => new Thing(); }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { Thing t = new Thing().Copy(); }
)");
    MARLIN_EXPECTF(selfOutside.HasError(DiagnosticCode::SelfTypeOutsideInterface), "{0}",
                   selfOutside.Errors().AsView());

    const CompiledShader unimplemented(R"(
namespace Test;
public interface ILuma { half Luma(); }
public extension half3 : ILuma { }
half Use<T>(T value) where T : ILuma => value.Luma();
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { half x = Use(half3(1, 2, 3)); }
)");
    // An extension that leaves a requirement out does not make the type conform.
    MARLIN_EXPECTF(unimplemented.HasError(DiagnosticCode::MissingInterfaceMember) &&
                       unimplemented.HasError(DiagnosticCode::NoMatchingOverload),
                   "{0}", unimplemented.Errors().AsView());

    const CompiledShader invisible(R"(
namespace Test;
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id) { half x = half3(1, 2, 3).Luma(); }
)");
    MARLIN_EXPECTF(invisible.HasError(DiagnosticCode::UndefinedName), "{0}", invisible.Errors().AsView());
}

MARLIN_TEST(LanguageScript, StaticRequirementsAndExtensions)
{
    const CheckedPackage checked(R"(
namespace Game;

public interface IScore
{
    type Points;
    static abstract this Empty { get; }
    static abstract this operator +(this a, this b);
    Points Total();
}

public struct Score : IScore
{
    public int Value;
    public type Points = int;
    public static Score Empty => new Score();
    public static Score operator +(Score a, Score b) => new Score { Value = a.Value + b.Value };
    public int Total() => Value;
}

public struct Medal { public int Rank; }

public extension Medal : IScore
{
    public type Points = int;
    public static Medal Empty => new Medal { Rank = 0 };
    public static Medal operator +(Medal a, Medal b) => new Medal { Rank = a.Rank + b.Rank };
    public int Total() => 10 - this.Rank;
}

// Members naming the implementing type are reached through type parameters; the others keep their virtual slots.
public interface IShape
{
    float Area();
    static abstract this Unit { get; }
    this Grow(float k);
}

public class Square : IShape
{
    public float Side;
    public float Area() => Side * Side;
    public static Square Unit => new Square { Side = 1 };
    public Square Grow(float k) => new Square { Side = Side * k };
}

public static class Scoring
{
    public static float Measure(IShape shape) => shape.Area();
    public static float Doubled<T>() where T : IShape => T.Unit.Grow(2).Area();
    public static float Shapes() => Measure(Square.Unit.Grow(3)) + Doubled<Square>();
    public static T.Points Sum<T>(T a, T b) where T : IScore => (T.Empty + a + b).Total();
    public static int Run() => Sum(new Score { Value = 2 }, new Score { Value = 3 }) + Sum(new Medal { Rank = 1 }, new Medal());
}
)");
    MARLIN_EXPECTF(checked.Ok, "{0}", checked.Errors().AsView());
}
} // namespace Marlin::Tests
