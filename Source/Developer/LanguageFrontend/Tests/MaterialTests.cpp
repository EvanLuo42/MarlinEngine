#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"
#include "LanguageFrontend/Parser.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
/// Compiles the material pass *entryPoint* of *source*, at /Project/Language/Shaders/Rock.mshader, against the mounted
/// engine's core library.
struct MaterialCompiled
{
    explicit MaterialCompiled(StringView source, StringView entryPoint, LanguageCompileOptions options = {})
        : Options(Move(options))
    {
        Memory.Add("/Project/Language/Shaders/Rock.mshader", source);
        ISourceProvider *providers[] = {&Engine, &Memory};
        Sources = MakeUnique<CompositeSourceProvider>(Span<ISourceProvider *const>(providers));
        Compilation = MakeUnique<LanguageCompilation>(*Sources, Options);
        Loaded = Compilation->Load();
        if (Loaded)
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

/// Options that compile a material pass with the material *material* and the variant values *variant*.
LanguageCompileOptions WithMaterial(StringView material, List<LanguageVariantValue> variant = {})
{
    LanguageCompileOptions options;
    options.Material = String(material);
    options.Variant = Move(variant);
    return options;
}

/// A source with a material interface, a material declared by *header* with *body*, and a material pass that
/// shades with it.
String MaterialSource(StringView body, StringView header = "public struct Rock<bool UseTint = true, Look Mode = Look.Flat>")
{
    StringBuilder<4096> text;
    text.Append(R"(namespace Test;

public enum Look { Flat, Shiny, Wet }

public interface ISurfaceMaterial
{
    float3 Shade(float3 normal);
}

public struct ResolveArgs { public Span<float4, Access.ReadWrite> Output; }

[Compute(64, 1, 1)]
void Resolve<TMaterial>([DispatchThreadId] uint3 id, [Uniform] in ResolveArgs args) where TMaterial : ISurfaceMaterial
{
    TMaterial material = default;
    args.Output[id.x] = float4(material.Shade(float3(0, 1, 0)), 1);
}

[Material]
)");
    text.Append(header);
    text.Append(" : ISurfaceMaterial\n{\n");
    text.Append(body);
    text.Append("}\n");
    return text.ToString();
}

constexpr StringView RockBody = R"(    public float3 Tint;
    public float Gloss;
    public Texture2D<float4> Detail;

    public float3 Shade(float3 normal)
    {
        float3 color = normal;
        static if (UseTint) { color = color * Tint; }
        static if (Mode == Look.Shiny) { color = color + Gloss; }
        return color;
    }
)";
} // namespace

MARLIN_TEST(LanguageParser, GenericParameterDefaults)
{
    SourceFile file(1, "/Project/Language/Shaders/Grid.mshader",
                    String("public struct Grid<T, int N = 4, int Cells = 2 * 8> { }\n"
                           "public struct Bad<int N = 4, T> { }\n"),
                    LanguageDialect::Shader);
    Arena memory;
    LanguageDiagnostics diagnostics;
    diagnostics.AddFile(file);
    AstFile *tree = ParseFile(file, memory, diagnostics, {});
    const auto *grid = tree->Members[0]->As<AstStructDecl>();
    MARLIN_REQUIRE(grid->GenericParameters.Count() == 3);
    MARLIN_EXPECT(grid->GenericParameters[0]->Default == nullptr);
    MARLIN_EXPECT(grid->GenericParameters[1]->Default->Kind == AstKind::LiteralExpr);
    MARLIN_EXPECT(grid->GenericParameters[2]->Default->Kind == AstKind::BinaryExpr);
    MARLIN_EXPECTF(diagnostics.ToString().AsView().Contains(
                       "a generic parameter after one with a default value needs a default value too"),
                   "{0}", diagnostics.ToString().AsView());
}

MARLIN_TEST(LanguageSema, GenericDefaultsFillOmittedArguments)
{
    const MaterialCompiled compiled(R"(namespace Test;
public struct Grid<int N = 4>
{
    public float[N] Cells;
    public int Count() => N;
}
public struct Args { public Span<float, Access.ReadWrite> Output; }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    Grid small = default;
    Grid<8> large = default;
    args.Output[id.x] = small.Cells[3] + large.Cells[7] + small.Count() + large.Count();
}
)",
                                    "Test.Main");
    MARLIN_REQUIREF(compiled.Ok, "{0}", compiled.Errors().AsView());
}

MARLIN_TEST(LanguageSema, DeclaredAttributesAreChecked)
{
    constexpr StringView Attributes = R"(namespace Test;
[Attribute(AttributeTargets.Field)]
public struct SliderAttribute
{
    public float Min;
    public float Max = 1;
}
public struct Args { public Span<float, Access.ReadWrite> Output; }
)";
    {
        StringBuilder<2048> source;
        source.Append(Attributes).Append(R"(
public struct Settings { [Slider(0, 2)] public float Exposure; [Slider(Max = 4)] public float Gamma; }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    Settings settings = default;
    args.Output[id.x] = settings.Exposure + settings.Gamma;
}
)");
        const MaterialCompiled compiled(source.AsView(), "Test.Main");
        MARLIN_REQUIREF(compiled.Ok, "{0}", compiled.Errors().AsView());
    }
    {
        StringBuilder<2048> source;
        source.Append(Attributes).Append(R"(
[Slider] float Twice(float x) => x * 2;
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args) { args.Output[id.x] = Twice(1); }
)");
        const MaterialCompiled compiled(source.AsView(), "Test.Main");
        MARLIN_EXPECTF(compiled.HasError(DiagnosticCode::InvalidAttribute), "{0}", compiled.Errors().AsView());
        MARLIN_EXPECTF(compiled.Errors().AsView().Contains("cannot be put on a function"), "{0}",
                       compiled.Errors().AsView());
    }
    {
        StringBuilder<2048> source;
        source.Append(Attributes).Append(R"(
public struct Settings { [Slider(0, 1, 2)] public float A; [Slider(Step = 1)] public float B; [Slider(0, Min = 1)] public float C; }
[Compute(1, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    Settings settings = default;
    args.Output[id.x] = settings.A;
}
)");
        const MaterialCompiled compiled(source.AsView(), "Test.Main");
        MARLIN_EXPECT(compiled.HasError(DiagnosticCode::WrongArgumentCount));
        MARLIN_EXPECT(compiled.HasError(DiagnosticCode::UndefinedName));
        MARLIN_EXPECTF(compiled.Errors().AsView().Contains("`Min` is given twice"), "{0}", compiled.Errors().AsView());
    }
}

MARLIN_TEST(LanguageSema, MaterialPassInstantiatesPerMaterialAndVariant)
{
    const String source = MaterialSource(RockBody);
    const MaterialCompiled defaults(source.AsView(), "Test.Resolve", WithMaterial("Test.Rock"));
    MARLIN_REQUIREF(defaults.Ok, "{0}", defaults.Errors().AsView());
    MARLIN_REQUIRE(defaults.Module.GetEntryPoints().Count() == 1);

    List<LanguageVariantValue> variant;
    variant.Add({String("UseTint"), 0});
    variant.Add({String("Mode"), 1});
    const MaterialCompiled shiny(source.AsView(), "Test.Resolve", WithMaterial("Test.Rock", Move(variant)));
    MARLIN_REQUIREF(shiny.Ok, "{0}", shiny.Errors().AsView());

    List<LanguageVariantValue> outOfRange;
    outOfRange.Add({String("UseTint"), 2});
    const MaterialCompiled wrong(source.AsView(), "Test.Resolve", WithMaterial("Test.Rock", Move(outOfRange)));
    MARLIN_EXPECTF(wrong.HasError(DiagnosticCode::InvalidMaterial), "{0}", wrong.Errors().AsView());
}

MARLIN_TEST(LanguageSema, MaterialsAreChecked)
{
    const String source = MaterialSource(RockBody);
    {
        const MaterialCompiled compiled(source.AsView(), "Test.Resolve");
        MARLIN_EXPECTF(compiled.Errors().AsView().Contains("is a material pass and needs a material"), "{0}",
                       compiled.Errors().AsView());
    }
    {
        const MaterialCompiled compiled(source.AsView(), "Test.Resolve", WithMaterial("Test.Marble"));
        MARLIN_EXPECTF(compiled.Errors().AsView().Contains("cannot find the material `Test.Marble`"), "{0}",
                       compiled.Errors().AsView());
    }
    {
        List<LanguageVariantValue> variant;
        variant.Add({String("UseMoss"), 1});
        const MaterialCompiled compiled(source.AsView(), "Test.Resolve", WithMaterial("Test.Rock", Move(variant)));
        MARLIN_EXPECTF(compiled.Errors().AsView().Contains("has no variant switch `UseMoss`"), "{0}",
                       compiled.Errors().AsView());
    }
    {
        const String writes = MaterialSource("    public Texture2D<float4, Access.ReadWrite> Target;\n"
                                             "    public float3 Shade(float3 normal) => normal;\n");
        const MaterialCompiled compiled(writes.AsView(), "Test.Resolve", WithMaterial("Test.Rock"));
        MARLIN_EXPECTF(compiled.Errors().AsView().Contains("material code cannot write to resources"), "{0}",
                       compiled.Errors().AsView());
    }
    {
        const String switches = MaterialSource("    public float3 Shade(float3 normal) => normal;\n",
                                               "public struct Rock<float Scale = 1>");
        const MaterialCompiled compiled(switches.AsView(), "Test.Resolve", WithMaterial("Test.Rock"));
        MARLIN_EXPECTF(compiled.Errors().AsView().Contains("a variant switch is a `bool` or an enum, not `float`"),
                       "{0}", compiled.Errors().AsView());
    }
}
} // namespace Marlin::Tests
