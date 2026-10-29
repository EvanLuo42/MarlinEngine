#include "Testing/Testing.h"

#include "LanguageFrontend/Parser.h"

namespace Marlin::Tests
{
namespace
{
struct Parsed
{
    explicit Parsed(StringView text, LanguageDialect dialect = LanguageDialect::Shader,
                    const ParseOptions &options = {})
        : File(1, "/Project/Language/Shaders/Test.mshader", String(text.Data(), text.Length()), dialect)
    {
        Diagnostics.AddFile(File);
        Tree = ParseFile(File, Memory, Diagnostics, options);
    }

    template <typename T> T *Member(usize index) const
    {
        return Tree->Members[index]->As<T>();
    }

    SourceFile File;
    Arena Memory;
    LanguageDiagnostics Diagnostics;
    AstFile *Tree = nullptr;
};

StringView TextOf(const Parsed &parsed, const AstNode *node)
{
    return parsed.File.GetText().Substring(node->Range.Begin, node->Range.End - node->Range.Begin);
}

constexpr StringView RockShader = R"(// Rock.mshader
using Marlin.Shading;

[Material, Id("9a0b")]
public struct Rock<bool UseMoss = true> : ISurfaceMaterial
{
    public MaterialTexture2D<half4> BaseColor = DefaultTextures.White;
    [Range(0, 1)] public half Roughness = 0.5h;

    public Surface Evaluate(in SurfaceInputs input)
    {
        half4 albedo = BaseColor.Sample(input.Uv0);
        Slab rock = Slab.Default with { Albedo = albedo.rgb, Roughness = Roughness };
        Slab moss = Slab.Default with { Albedo = half3(0.2h, 0.4h, 0.1h), Roughness = 0.9h, Fuzz = 0.5h };
        return Surface.VerticalLayer(top: moss, bottom: rock, thickness: input.WorldNormal.y * 0.3h);
    }
}
)";
} // namespace

MARLIN_TEST(LanguageLexer, NumbersAndSuffixes)
{
    SourceFile file(1, "/a.mcommon", String("0x1F 1_000u 2.5 .5h 1e-3f 3d 0b101ul"), LanguageDialect::Common);
    LanguageDiagnostics diagnostics;
    diagnostics.AddFile(file);
    List<Token> tokens;
    Tokenize(file, diagnostics, tokens);
    MARLIN_REQUIREF(!diagnostics.HasErrors(), "{0}", diagnostics.ToString().AsView());
    MARLIN_REQUIRE(tokens.Count() == 8);
    MARLIN_EXPECT(tokens[0].Kind == TokenKind::IntegerLiteral && tokens[0].Integer == 31);
    MARLIN_EXPECT(tokens[1].Integer == 1000 && tokens[1].Suffix == LiteralSuffix::U);
    MARLIN_EXPECT(tokens[2].Kind == TokenKind::FloatLiteral && tokens[2].Float == 2.5);
    MARLIN_EXPECT(tokens[3].Float == 0.5 && tokens[3].Suffix == LiteralSuffix::H);
    MARLIN_EXPECT(tokens[4].Kind == TokenKind::FloatLiteral && tokens[4].Suffix == LiteralSuffix::F);
    MARLIN_EXPECT(tokens[5].Kind == TokenKind::FloatLiteral && tokens[5].Float == 3.0);
    MARLIN_EXPECT(tokens[6].Integer == 5 && tokens[6].Suffix == LiteralSuffix::UL);
    MARLIN_EXPECT(tokens[7].Kind == TokenKind::EndOfFile);
}

MARLIN_TEST(LanguageDiagnostics, RendersLikeRustc)
{
    SourceFile file(1, "/Project/Language/Shaders/Rock.mshader",
                    String("struct Slab\n{\n    half3 Albedo;\n}\n\tSlab s = x;\n"), LanguageDialect::Shader);
    LanguageDiagnostics diagnostics;
    diagnostics.AddFile(file);
    diagnostics.Error(DiagnosticCode::UndefinedName, {1, 44, 45}, "cannot find `{0}` in this scope", StringView("x"))
        .WithLabel("not found in this scope")
        .WithSecondaryLabel({1, 7, 11}, "the struct")
        .WithNote({1, 24, 30}, "a field with a similar role")
        .WithHelp("declare it first");
    constexpr StringView Expected = R"(error[M3001]: cannot find `x` in this scope
 --> /Project/Language/Shaders/Rock.mshader:5:11
  |
1 | struct Slab
  |        ---- the struct
5 |     Slab s = x;
  |              ^ not found in this scope
  |
  = help: declare it first
note: a field with a similar role
 --> /Project/Language/Shaders/Rock.mshader:3:11
  |
3 |     half3 Albedo;
  |           ^^^^^^

)";
    const String text = diagnostics.ToString();
    MARLIN_EXPECTF(text.AsView() == Expected, "{0}", text.AsView());
}

MARLIN_TEST(LanguageLexer, ReportsBadInput)
{
    SourceFile file(1, "/a.mcommon", String("int x = 1z; /* open"), LanguageDialect::Common);
    LanguageDiagnostics diagnostics;
    diagnostics.AddFile(file);
    List<Token> tokens;
    Tokenize(file, diagnostics, tokens);
    MARLIN_EXPECT(diagnostics.GetErrorCount() == 2);
    constexpr StringView Expected = R"(error[M1004]: unknown literal suffix 'z'
 --> /a.mcommon:1:9
  |
1 | int x = 1z; /* open
  |         ^^

error[M1002]: the comment is not closed
 --> /a.mcommon:1:13
  |
1 | int x = 1z; /* open
  |             ^^^^^^^

)";
    const String text = diagnostics.ToString();
    MARLIN_EXPECTF(text.AsView() == Expected, "{0}", text.AsView());
}

MARLIN_TEST(LanguageParser, MaterialExample)
{
    const Parsed parsed(RockShader);
    MARLIN_REQUIREF(!parsed.Diagnostics.HasErrors(), "{0}", parsed.Diagnostics.ToString().AsView());
    MARLIN_REQUIRE(parsed.Tree->Members.Count() == 2);

    const auto *usingDecl = parsed.Member<AstUsingDecl>(0);
    MARLIN_EXPECT(TextOf(parsed, usingDecl->Target) == "Marlin.Shading");

    const auto *rock = parsed.Member<AstStructDecl>(1);
    MARLIN_REQUIRE(rock->Attributes.Count() == 2);
    MARLIN_EXPECT(rock->Attributes[0].Name == "Material" && !rock->Attributes[0].HasArguments);
    MARLIN_EXPECT(rock->Attributes[1].Name == "Id" && rock->Attributes[1].Arguments.Count() == 1);
    MARLIN_REQUIRE(rock->GenericParameters.Count() == 1);
    const auto *useMoss = rock->GenericParameters[0];
    MARLIN_EXPECT(useMoss->Name == "UseMoss" && useMoss->ValueType != nullptr);
    MARLIN_EXPECT(useMoss->Default->As<AstLiteralExpr>()->Literal.Kind == TokenKind::KeywordTrue);
    MARLIN_EXPECT(rock->Interfaces.Count() == 1);
    MARLIN_REQUIRE(rock->Members.Count() == 3);

    const auto *baseColor = rock->Members[0]->As<AstFieldDecl>();
    const auto *textureType = baseColor->Type->As<AstNamedType>();
    MARLIN_EXPECT(textureType->Name == "MaterialTexture2D" && textureType->GenericArguments.Count() == 1);
    MARLIN_EXPECT(baseColor->Initializer->Kind == AstKind::MemberExpr);
    const auto *roughness = rock->Members[1]->As<AstFieldDecl>();
    MARLIN_EXPECT(roughness->Attributes[0].Name == "Range" && roughness->Attributes[0].Arguments.Count() == 2);
    MARLIN_EXPECT(roughness->Initializer->As<AstLiteralExpr>()->Literal.Suffix == LiteralSuffix::H);

    const auto *evaluate = rock->Members[2]->As<AstFunctionDecl>();
    MARLIN_EXPECT(evaluate->Name == "Evaluate" && evaluate->Parameters.Count() == 1);
    MARLIN_EXPECT(HasAnyFlags(evaluate->Parameters[0]->Modifiers, AstModifiers::In));
    MARLIN_REQUIRE(evaluate->Body.Block != nullptr && evaluate->Body.Block->Statements.Count() == 4);
    const auto *rockVar = evaluate->Body.Block->Statements[1]->As<AstVarStmt>();
    const auto *with = rockVar->Declarators[0].Initializer->As<AstWithExpr>();
    MARLIN_EXPECT(with->Initializers.Count() == 2 && with->Initializers[0].Name == "Albedo");
    const auto *call = evaluate->Body.Block->Statements[3]->As<AstReturnStmt>()->Value->As<AstCallExpr>();
    MARLIN_REQUIRE(call->Arguments.Count() == 3);
    MARLIN_EXPECT(call->Arguments[0].Name == "top" && call->Arguments[2].Name == "thickness");
    MARLIN_EXPECT(call->Arguments[2].Value->As<AstBinaryExpr>()->Operator == TokenKind::Star);
}

MARLIN_TEST(LanguageParser, GenericsCastsAndShifts)
{
    const Parsed parsed(R"(
float Test<int N>(float[N] values, Vector<float, 3> v)
{
    var a = Max<float>(values[0], 1.0);
    bool b = x < y && z > w;
    uint c = (uint)a >> 2;
    var d = (a) - 1;
    Vector<Vector<float, 2>, 2> e = default;
    c >>= 1;
    return (float)-c;
}
)");
    MARLIN_REQUIREF(!parsed.Diagnostics.HasErrors(), "{0}", parsed.Diagnostics.ToString().AsView());
    const auto *test = parsed.Member<AstFunctionDecl>(0);
    MARLIN_EXPECT(test->GenericParameters.Count() == 1 && test->GenericParameters[0]->ValueType != nullptr);
    MARLIN_EXPECT(test->Parameters[0]->Type->Kind == AstKind::ArrayType);
    const Span<AstNode *> statements = test->Body.Block->Statements;

    const auto *max = statements[0]->As<AstVarStmt>()->Declarators[0].Initializer->As<AstCallExpr>();
    MARLIN_EXPECT(max->Callee->As<AstNameExpr>()->HasGenericArguments);

    const auto *b = statements[1]->As<AstVarStmt>()->Declarators[0].Initializer->As<AstBinaryExpr>();
    MARLIN_EXPECT(b->Operator == TokenKind::AmpersandAmpersand);

    const auto *c = statements[2]->As<AstVarStmt>()->Declarators[0].Initializer->As<AstBinaryExpr>();
    MARLIN_EXPECT(c->Operator == TokenKind::GreaterGreater && c->Left->Kind == AstKind::CastExpr);

    const auto *d = statements[3]->As<AstVarStmt>()->Declarators[0].Initializer->As<AstBinaryExpr>();
    MARLIN_EXPECT(d->Operator == TokenKind::Minus && d->Left->Kind == AstKind::NameExpr);

    const auto *e = statements[4]->As<AstVarStmt>();
    MARLIN_EXPECT(e->Type->As<AstNamedType>()->GenericArguments[0]->Kind == AstKind::NamedType);

    MARLIN_EXPECT(statements[5]->As<AstExprStmt>()->Expression->As<AstAssignExpr>()->Operator ==
                  TokenKind::GreaterGreaterAssign);
    MARLIN_EXPECT(statements[6]->As<AstReturnStmt>()->Value->Kind == AstKind::CastExpr);
}

MARLIN_TEST(LanguageParser, DeclarationsAndStatements)
{
    const Parsed parsed(R"(
namespace Marlin.Shading;

public interface IBsdf
{
    half3 EvaluateDirect(in BsdfContext ctx, in LightSample light);
    [Optional] BsdfSample Sample(in BsdfContext ctx, float2 u);
}

[Flags] public enum Lobe : uint { Diffuse = 1, Specular = 2, }

public struct Slab : IComparable<Slab>
{
    public half3 Albedo, Emission;
    public half Roughness = 0.5h;
    public static readonly Slab Default = new Slab();
    public half Luma => Dot(Albedo, half3(0.2126h, 0.7152h, 0.0722h));
    public Slab(half3 albedo) { Albedo = albedo; }
    public static Slab operator +(Slab a, Slab b) => a with { Albedo = a.Albedo + b.Albedo };
    public static implicit operator half3(Slab s) => s.Albedo;
}

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    for (uint i = 0, j = 1; i < 4; ++i, j++) { if (i == 2) continue; }
    foreach (var x in Range(0, 4)) { }
    static if (Device.Supports(Features.CooperativeVector)) { } else { }
    switch (id.x) { case 0: case 1: break; default: return; }
    do { } while (false);
    discard;
}
)");
    MARLIN_REQUIREF(!parsed.Diagnostics.HasErrors(), "{0}", parsed.Diagnostics.ToString().AsView());
    const auto *ns = parsed.Member<AstNamespaceDecl>(0);
    MARLIN_EXPECT(ns->FileScoped && ns->Name == "Marlin.Shading");
    MARLIN_REQUIRE(ns->Members.Count() == 4);

    const auto *bsdf = ns->Members[0]->As<AstInterfaceDecl>();
    MARLIN_EXPECT(bsdf->Members.Count() == 2 && bsdf->Members[1]->Attributes[0].Name == "Optional");
    MARLIN_EXPECT(bsdf->Members[0]->As<AstFunctionDecl>()->Body.IsMissing());

    const auto *lobe = ns->Members[1]->As<AstEnumDecl>();
    MARLIN_EXPECT(lobe->Members.Count() == 2 && lobe->UnderlyingType != nullptr);

    const auto *slab = ns->Members[2]->As<AstStructDecl>();
    MARLIN_REQUIRE(slab->Members.Count() == 8);
    MARLIN_EXPECT(slab->Members[0]->Name == "Albedo" && slab->Members[1]->Name == "Emission");
    MARLIN_EXPECT(slab->Members[0]->Parent == slab);
    MARLIN_EXPECT(slab->Members[4]->Kind == AstKind::PropertyDecl);
    MARLIN_EXPECT(slab->Members[5]->Kind == AstKind::ConstructorDecl);
    MARLIN_EXPECT(slab->Members[6]->As<AstOperatorDecl>()->Operator == TokenKind::Plus);
    MARLIN_EXPECT(slab->Members[7]->As<AstOperatorDecl>()->Operator == TokenKind::EndOfFile);

    const auto *main = ns->Members[3]->As<AstFunctionDecl>();
    MARLIN_EXPECT(main->Parameters[0]->Attributes[0].Name == "DispatchThreadId");
    const Span<AstNode *> statements = main->Body.Block->Statements;
    MARLIN_REQUIRE(statements.Count() == 6);
    MARLIN_EXPECT(statements[0]->As<AstForStmt>()->Increments.Count() == 2);
    MARLIN_EXPECT(statements[1]->Kind == AstKind::ForeachStmt);
    MARLIN_EXPECT(statements[2]->As<AstIfStmt>()->IsStatic);
    MARLIN_EXPECT(statements[3]->As<AstSwitchStmt>()->Sections.Count() == 2);
    MARLIN_EXPECT(statements[3]->As<AstSwitchStmt>()->Sections[0].Labels.Count() == 2);
    MARLIN_EXPECT(statements[4]->Kind == AstKind::DoStmt);
    MARLIN_EXPECT(statements[5]->Kind == AstKind::DiscardStmt);
}

MARLIN_TEST(LanguageParser, SkipsBodiesAndParsesThemLater)
{
    Parsed parsed(RockShader, LanguageDialect::Shader, {.SkipBodies = true});
    MARLIN_REQUIRE(!parsed.Diagnostics.HasErrors());
    auto *rock = parsed.Member<AstStructDecl>(1)->Members[2]->As<AstFunctionDecl>();
    MARLIN_EXPECT(!rock->Body.Parsed && rock->Body.Block == nullptr && !rock->Body.IsMissing());
    ParseBody(*parsed.Tree, rock->Body, parsed.Memory, parsed.Diagnostics);
    MARLIN_REQUIRE(rock->Body.Parsed && rock->Body.Block != nullptr);
    MARLIN_EXPECT(rock->Body.Block->Statements.Count() == 4);
}

MARLIN_TEST(LanguageParser, RecoversFromErrors)
{
    const Parsed parsed(R"(
void A() { int x = ; float y = 1; }
struct B { int = 3; float Ok; }
void C() { return 1 }
float D() => 2.0;
)");
    MARLIN_EXPECT(parsed.Diagnostics.GetErrorCount() >= 3);
    MARLIN_REQUIRE(parsed.Tree->Members.Count() == 4);
    MARLIN_EXPECT(parsed.Member<AstFunctionDecl>(0)->Body.Block->Statements.Count() == 2);
    MARLIN_EXPECT(parsed.Member<AstFunctionDecl>(3)->Name == "D");
}
} // namespace Marlin::Tests
