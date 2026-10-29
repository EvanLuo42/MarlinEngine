#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPrinter.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
/// Compiles `Test.Main` of *source*, at /Project/Language/Shaders/Test.mshader, against the mounted engine's core
/// library, without running MIR passes: what automatic differentiation (MarlinLanguage.md §9.12) checks before them.
struct Differentiated
{
    explicit Differentiated(StringView source)
    {
        Memory.Add("/Project/Language/Shaders/Test.mshader", source);
        ISourceProvider *providers[] = {&Engine, &Memory};
        Sources = MakeUnique<CompositeSourceProvider>(Span<ISourceProvider *const>(providers));
        Compilation = MakeUnique<LanguageCompilation>(*Sources, Options);
        Ok = Compilation->Load() && Compilation->CompileEntryPoint("Test.Main", Module);
    }

    [[nodiscard]] String Errors() const
    {
        return Compilation->GetDiagnostics().ToString();
    }

    /// Whether the compilation failed with *code* and *message* in its diagnostics.
    [[nodiscard]] bool Fails(DiagnosticCode code, StringView message) const
    {
        bool found = false;
        for (const LanguageDiagnostic &diagnostic : Compilation->GetDiagnostics().GetAll())
        {
            found = found || diagnostic.Code == code;
        }
        return !Ok && found && Errors().AsView().Contains(message);
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

/// *body* as the body of a compute entry point `Test.Main` writing `float result` out, after *declarations*.
String Shader(StringView declarations, StringView body)
{
    StringBuilder<4096> source;
    source.Append("namespace Test;\n")
        .Append(declarations)
        .Append("\npublic struct Args { public Span<float, Access.ReadWrite> Output; public float X; }\n"
                "[Compute(1, 1, 1)]\nvoid Main([Uniform] in Args args)\n{\n    float result = 0;\n")
        .Append(body)
        .Append("\n    args.Output[0] = result;\n}\n");
    return source.ToString();
}
} // namespace

MARLIN_TEST(LanguageSema, DifferentiableStructsConformToIDifferentiable)
{
    // Structs whose fields are differentiable or [NoDiff] conform without saying so, and may say so; generic code
    // reaches the requirements through `T`.
    const Differentiated compiled(Shader(R"(
public struct Ray : IDifferentiable { public float3 Origin; [NoDiff] public uint Bounces; }
public struct Sample { public float2 Uv; public float[2] Weights; [NoDiff] public int Index; }

T.Differential Twice<T>(T.Differential d) where T : IDifferentiable => T.DifferentialAdd(d, T.DifferentialScale(d, 1));

[Differentiable]
float Length2(Ray ray) => Dot(ray.Origin, ray.Origin);
)",
                                         R"(
    Ray.Differential zero = Ray.DifferentialZero;
    Sample.Differential sample = Twice<Sample>(new Sample { Uv = float2(1, 2) });
    float3x3.Differential m = Twice<float3x3>(float3x3.DifferentialZero);
    var origin = new Ray { Origin = float3(args.X, 0, 0) };
    var ray = new DifferentialPair<Ray>(origin, new Ray { Origin = float3(1, 0, 0) });
    DifferentialPair<float> length = Autodiff.Forward(Length2, ray);
    Autodiff.Backward(Length2, ref ray, 1);
    result = length.Differential + ray.Differential.Origin.x + sample.Uv.y + zero.Origin.x + m[0].x;)"));
    MARLIN_REQUIREF(compiled.Ok, "{0}", compiled.Errors().AsView());
}

MARLIN_TEST(LanguageSema, ReportsTypesWithoutDerivatives)
{
    const Differentiated declared(
        Shader("public struct Counter : IDifferentiable { public float Value; public uint Count; }",
               "    var c = new DifferentialPair<Counter>(new Counter(), new Counter());\n"
               "    result = c.Primal.Value;"));
    MARLIN_EXPECTF(declared.Fails(DiagnosticCode::NotDifferentiable, "`Counter` cannot implement `IDifferentiable`") &&
                       declared.Errors().AsView().Contains("`uint` has no derivative"),
                   "{0}", declared.Errors().AsView());

    const Differentiated pair(
        Shader("", "    var p = new DifferentialPair<int>(1, 0);\n    result = (float)p.Primal;"));
    MARLIN_EXPECTF(pair.Fails(DiagnosticCode::ConstraintNotSatisfied, "IDifferentiable"), "{0}",
                   pair.Errors().AsView());

    const Differentiated detached(Shader("", "    result = (float)Autodiff.Detach(3u);"));
    MARLIN_EXPECTF(detached.Fails(DiagnosticCode::NotDifferentiable,
                                  "`Autodiff.Detach` takes a value with a derivative, not a `uint`"),
                   "{0}", detached.Errors().AsView());
}

MARLIN_TEST(LanguageSema, ReportsCallsWithoutDerivatives)
{
    // A plain function called from differentiable code with a value that has a derivative.
    constexpr StringView Declarations = R"(
float Cube(float x) => x * x * x;
float Scale(uint n) => (float)n;
[Differentiable]
float Shade(float x) => Cube(x) + Scale(3u) + Cube(2) + Autodiff.Detach(Cube(x));
)";
    const Differentiated plain(Shader(
        Declarations, "    result = Autodiff.Forward(Shade, new DifferentialPair<float>(args.X, 1)).Differential;"));
    MARLIN_EXPECTF(plain.Fails(DiagnosticCode::NotDifferentiable, "`Cube` is not differentiable"), "{0}",
                   plain.Errors().AsView());
    // Only the first call is reported: the others take no value with a derivative, or are detached.
    usize reports = 0;
    for (const LanguageDiagnostic &diagnostic : plain.Compilation->GetDiagnostics().GetAll())
    {
        reports += diagnostic.Code == DiagnosticCode::NotDifferentiable ? 1 : 0;
    }
    MARLIN_EXPECTF(reports == 1, "{0}", plain.Errors().AsView());
}

MARLIN_TEST(LanguageSema, ReportsPairsForNoDiffParameters)
{
    const Differentiated compiled(Shader(R"(
[Differentiable]
float Shade(float x, [NoDiff] float exposure) => x * exposure;
)",
                                         R"(
    var x = new DifferentialPair<float>(args.X, 1);
    result = Autodiff.Forward(Shade, x, new DifferentialPair<float>(2, 1)).Differential;)"));
    MARLIN_EXPECTF(compiled.Fails(DiagnosticCode::PairForNoDiffParameter,
                                  "the parameter `exposure` is `[NoDiff]`, so it takes a `float`, not a pair"),
                   "{0}", compiled.Errors().AsView());
}

MARLIN_TEST(LanguageSema, ChecksCustomDerivatives)
{
    // The backward derivative must take the pair by `ref`, then dResult.
    const Differentiated signature(Shader(R"(
[ForwardDerivative(nameof(SquareForward)), BackwardDerivative(nameof(SquareBackward))]
float Square(float x, int unused) => x * x;
DifferentialPair<float> SquareForward(DifferentialPair<float> x, int unused)
    => new DifferentialPair<float>(x.Primal * x.Primal, 2 * x.Primal * x.Differential);
void SquareBackward(DifferentialPair<float> x, int unused, float dResult) { }
[Differentiable]
float Shade(float x) => Square(x, 0);
)",
                                          "    result = Autodiff.Forward(Shade, new DifferentialPair<float>(args.X, "
                                          "1)).Differential;"));
    MARLIN_EXPECTF(
        signature.Fails(DiagnosticCode::InvalidCustomDerivative,
                        "`SquareBackward` does not have the signature of a backward derivative of `Square`") &&
            signature.Errors().AsView().Contains(
                "expected `void SquareBackward(ref DifferentialPair<float> x, int unused, float dResult)`"),
        "{0}", signature.Errors().AsView());

    const Differentiated missing(Shader(R"(
[ForwardDerivative(nameof(Shade))]
float Square(float x) => x * x;
[ForwardDerivative("Nothing")]
float Cube(float x) => x * x * x;
float Shade(float x) => Square(x) + Cube(x);
)",
                                        "    result = Shade(args.X);"));
    MARLIN_EXPECTF(missing.Fails(DiagnosticCode::InvalidCustomDerivative,
                                 "`Shade` does not have the signature of a forward derivative of `Square`") &&
                       missing.Errors().AsView().Contains("`Nothing` is not a function in scope"),
                   "{0}", missing.Errors().AsView());
}

MARLIN_TEST(LanguageSema, ReadsMaxIterations)
{
    const Differentiated compiled(Shader("", "    [MaxIterations(4)] for (int i = 0; i < 2; i++) { result += 1; }\n"
                                             "    [MaxIterations(0)] for (int i = 0; i < 2; i++) { result += 1; }"));
    MARLIN_EXPECTF(
        compiled.Fails(DiagnosticCode::InvalidAttribute, "`[MaxIterations(n)]` takes a constant from 1 to 65536"),
        "{0}", compiled.Errors().AsView());
    usize reports = 0;
    for (const LanguageDiagnostic &diagnostic : compiled.Compilation->GetDiagnostics().GetAll())
    {
        reports += diagnostic.Code == DiagnosticCode::InvalidAttribute ? 1 : 0;
    }
    MARLIN_EXPECT(reports == 1);
}
} // namespace Marlin::Tests
