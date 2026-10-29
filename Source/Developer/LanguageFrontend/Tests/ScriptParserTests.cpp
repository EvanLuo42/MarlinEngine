#include "Testing/Testing.h"

#include "LanguageFrontend/Parser.h"

namespace Marlin::Tests
{
namespace
{
struct ParsedScript
{
    explicit ParsedScript(StringView text)
        : File(1, "/Project/Language/Scripts/Test.mscript", String(text.Data(), text.Length()), LanguageDialect::Script)
    {
        Diagnostics.AddFile(File);
        Tree = ParseFile(File, Memory, Diagnostics);
    }

    template <typename T> T *Member(usize index) const
    {
        return Tree->Members[index]->As<T>();
    }

    [[nodiscard]] StringView Text(const AstNode *node) const
    {
        return File.GetText().Substring(node->Range.Begin, node->Range.End - node->Range.Begin);
    }

    /// The expression of statement *index* of the body of function *function*.
    [[nodiscard]] AstNode *Statement(usize function, usize index) const
    {
        return Member<AstFunctionDecl>(function)->Body.Block->Statements[index];
    }

    [[nodiscard]] AstNode *Expression(usize function, usize index) const
    {
        return Statement(function, index)->As<AstExprStmt>()->Expression;
    }

    /// The initializer of the local declared by statement *index*.
    [[nodiscard]] AstNode *Initializer(usize function, usize index) const
    {
        return Statement(function, index)->As<AstVarStmt>()->Declarators[0].Initializer;
    }

    SourceFile File;
    Arena Memory;
    LanguageDiagnostics Diagnostics;
    AstFile *Tree = nullptr;
};
} // namespace

MARLIN_TEST(LanguageLexer, ScriptLiterals)
{
    SourceFile file(1, "/a.mscript", String(R"(@"C:\dir""x" 'a' '\n' '\u00E9' $"hp {hp:F1} {{ok}} {Name("}")}" a ?? b ??= c?.d)"),
                    LanguageDialect::Script);
    LanguageDiagnostics diagnostics;
    diagnostics.AddFile(file);
    List<Token> tokens;
    Tokenize(file, diagnostics, tokens);
    MARLIN_REQUIREF(!diagnostics.HasErrors(), "{0}", diagnostics.ToString().AsView());
    MARLIN_REQUIRE(tokens.Count() == 13);
    MARLIN_EXPECT(tokens[0].Kind == TokenKind::StringLiteral && tokens[0].Verbatim);
    MARLIN_EXPECT(DecodeStringLiteral(file, tokens[0]) == R"(C:\dir"x)");
    MARLIN_EXPECT(tokens[1].Kind == TokenKind::RuneLiteral && tokens[1].Integer == 'a');
    MARLIN_EXPECT(tokens[2].Integer == '\n');
    MARLIN_EXPECT(tokens[3].Integer == 0xE9);
    MARLIN_EXPECT(tokens[4].Kind == TokenKind::InterpolatedStringLiteral);
    MARLIN_EXPECT(tokens[6].Kind == TokenKind::QuestionQuestion);
    MARLIN_EXPECT(tokens[8].Kind == TokenKind::QuestionQuestionAssign);
    MARLIN_EXPECT(tokens[10].Kind == TokenKind::QuestionDot);
    MARLIN_EXPECT(tokens[12].Kind == TokenKind::EndOfFile);
}

MARLIN_TEST(LanguageParser, ScriptDeclarations)
{
    const ParsedScript parsed(R"(
public delegate TResult Func<T, TResult>(T argument);

public abstract class Weapon : Item, IUsable
{
    public string Name { get; init; } = "weapon";
    public float Ammo { get => _ammo; private set { _ammo = value; } }
    public event Action<float> Fired;
    protected float _ammo;
    public Weapon(string name) : base(name) { }
    public abstract void Fire();
    public virtual float this[int slot] { get => 0; }
}

public sealed class Rifle : Weapon
{
    public Rifle() : this("rifle") { }
    public override void Fire() { }
}

T Make<T>() where T : class, new() => new T();
)");
    MARLIN_REQUIREF(!parsed.Diagnostics.HasErrors(), "{0}", parsed.Diagnostics.ToString().AsView());
    MARLIN_REQUIRE(parsed.Tree->Members.Count() == 4);

    auto *delegate = parsed.Member<AstDelegateDecl>(0);
    MARLIN_EXPECT(delegate->Name == "Func" && delegate->GenericParameters.Count() == 2);
    MARLIN_EXPECT(delegate->Parameters.Count() == 1);

    auto *weapon = parsed.Member<AstStructDecl>(1);
    MARLIN_EXPECT(weapon->Modifiers == (AstModifiers::Class | AstModifiers::Abstract | AstModifiers::Public));
    MARLIN_EXPECT(weapon->Interfaces.Count() == 2);
    MARLIN_REQUIRE(weapon->Members.Count() == 7);
    auto *name = weapon->Members[0]->As<AstPropertyDecl>();
    MARLIN_EXPECT(name->HasGetter && name->Getter.IsMissing() && name->Setter.Present && name->SetterIsInit);
    MARLIN_EXPECT(name->Initializer != nullptr);
    auto *ammo = weapon->Members[1]->As<AstPropertyDecl>();
    MARLIN_EXPECT(!ammo->Getter.IsMissing() && ammo->Setter.Present && !ammo->SetterIsInit);
    MARLIN_EXPECT(HasAnyFlags(ammo->Setter.Modifiers, AstModifiers::Private));
    MARLIN_EXPECT(HasAnyFlags(weapon->Members[2]->Modifiers, AstModifiers::Event));
    MARLIN_EXPECT(HasAnyFlags(weapon->Members[3]->Modifiers, AstModifiers::Protected));
    auto *constructor = weapon->Members[4]->As<AstConstructorDecl>();
    MARLIN_EXPECT(constructor->HasInitializer && constructor->InitializerIsBase);
    MARLIN_EXPECT(constructor->InitializerArguments.Count() == 1);
    MARLIN_EXPECT(weapon->Members[5]->As<AstFunctionDecl>()->Body.IsMissing());
    auto *indexer = weapon->Members[6]->As<AstPropertyDecl>();
    MARLIN_EXPECT(indexer->IsIndexer && indexer->Parameters.Count() == 1);

    auto *rifle = parsed.Member<AstStructDecl>(2);
    MARLIN_EXPECT(HasAnyFlags(rifle->Modifiers, AstModifiers::Sealed));
    MARLIN_EXPECT(!rifle->Members[0]->As<AstConstructorDecl>()->InitializerIsBase);

    auto *make = parsed.Member<AstFunctionDecl>(3);
    MARLIN_REQUIRE(make->Constraints.Count() == 1);
    MARLIN_EXPECT(make->Constraints[0].Class && make->Constraints[0].New);
}

MARLIN_TEST(LanguageParser, ScriptExpressions)
{
    const ParsedScript parsed(R"(
async Task Run(Weapon? weapon, int[] values)
{
    Func<int, int> twice = x => x * 2;
    Action<int, float> both = (int a, ref float b) => { b = a; };
    await Frame.Delay(2);
    string text = $"ammo {weapon?.Ammo:F1} of {values.Length}";
    var name = weapon?.Name ?? "none";
    weapon!.Fire();
    var copy = new int[values.Length];
    var squares = new[] { 1, 4, 9 };
    var list = new List<int> { 1, 2 };
    var item = new Item { Name = "x" };
    bool rifle = weapon is Rifle { Ammo: > 0 and < 10 } r;
    var gun = weapon as Rifle;
    float scale = weapon switch { Rifle r when r.Ammo > 0 => 1, null => 0, _ => 2 };
    ref int first = ref values[0];
    name ??= "x";
}

IEnumerable<int> Count(int n)
{
    yield return n;
    yield break;
}
)");
    MARLIN_REQUIREF(!parsed.Diagnostics.HasErrors(), "{0}", parsed.Diagnostics.ToString().AsView());

    auto *twice = parsed.Initializer(0, 0)->As<AstLambdaExpr>();
    MARLIN_EXPECT(twice->Parameters.Count() == 1 && twice->Parameters[0].Type == nullptr);
    auto *both = parsed.Initializer(0, 1)->As<AstLambdaExpr>();
    MARLIN_EXPECT(both->Parameters.Count() == 2 && both->Parameters[1].Modifier == AstModifiers::Ref);
    MARLIN_EXPECT(both->Body->Kind == AstKind::BlockStmt);
    MARLIN_EXPECT(parsed.Expression(0, 2)->Kind == AstKind::AwaitExpr);

    auto *text = parsed.Initializer(0, 3)->As<AstInterpolatedStringExpr>();
    MARLIN_REQUIRE(text->Parts.Count() == 4);
    MARLIN_EXPECT(text->Parts[0].Text == "ammo ");
    MARLIN_EXPECT(text->Parts[1].Expression->Kind == AstKind::ConditionalAccessExpr && text->Parts[1].Format == "F1");
    MARLIN_EXPECT(text->Parts[2].Text == " of ");
    MARLIN_EXPECT(parsed.Text(text->Parts[3].Expression) == "values.Length");

    auto *name = parsed.Initializer(0, 4)->As<AstBinaryExpr>();
    MARLIN_EXPECT(name->Operator == TokenKind::QuestionQuestion);
    auto *access = name->Left->As<AstConditionalAccessExpr>();
    MARLIN_EXPECT(access->Access->As<AstMemberExpr>()->Target->Kind == AstKind::ConditionalReceiverExpr);

    auto *call = parsed.Expression(0, 5)->As<AstCallExpr>();
    MARLIN_EXPECT(call->Callee->As<AstMemberExpr>()->Target->Kind == AstKind::NullForgivingExpr);

    auto *copy = parsed.Initializer(0, 6)->As<AstArrayCreationExpr>();
    MARLIN_EXPECT(copy->Element != nullptr && copy->Length != nullptr && !copy->HasElements);
    auto *squares = parsed.Initializer(0, 7)->As<AstArrayCreationExpr>();
    MARLIN_EXPECT(squares->Element == nullptr && squares->Elements.Count() == 3);
    auto *list = parsed.Initializer(0, 8)->As<AstNewExpr>();
    MARLIN_EXPECT(list->HasCollectionInitializer && list->Elements.Count() == 2);
    MARLIN_EXPECT(parsed.Initializer(0, 9)->As<AstNewExpr>()->HasInitializer);

    auto *is = parsed.Initializer(0, 10)->As<AstIsExpr>();
    auto *property = is->Pattern->As<AstPropertyPattern>();
    MARLIN_EXPECT(property->Type != nullptr && property->Name == "r");
    MARLIN_REQUIRE(property->Subpatterns.Count() == 1);
    MARLIN_EXPECT(property->Subpatterns[0].Pattern->As<AstBinaryPattern>()->IsAnd);
    MARLIN_EXPECT(parsed.Initializer(0, 11)->Kind == AstKind::AsExpr);

    auto *scale = parsed.Initializer(0, 12)->As<AstSwitchExpr>();
    MARLIN_REQUIRE(scale->Arms.Count() == 3);
    MARLIN_EXPECT(scale->Arms[0].Pattern->Kind == AstKind::DeclarationPattern && scale->Arms[0].When != nullptr);
    MARLIN_EXPECT(scale->Arms[1].Pattern->Kind == AstKind::ConstantPattern);
    MARLIN_EXPECT(scale->Arms[2].Pattern->Kind == AstKind::DiscardPattern);

    MARLIN_EXPECT(parsed.Statement(0, 13)->As<AstVarStmt>()->IsRef);
    MARLIN_EXPECT(parsed.Expression(0, 14)->As<AstAssignExpr>()->Operator == TokenKind::QuestionQuestionAssign);

    MARLIN_EXPECT(parsed.Statement(1, 0)->As<AstYieldStmt>()->Value != nullptr);
    MARLIN_EXPECT(parsed.Statement(1, 1)->As<AstYieldStmt>()->IsBreak);
}

MARLIN_TEST(LanguageParser, ScriptSwitchStatementPatterns)
{
    const ParsedScript parsed(R"(
void Classify(object value)
{
    switch (value)
    {
    case int n when n > 0:
    case string:
        break;
    case null:
    default:
        break;
    }
}
)");
    MARLIN_REQUIREF(!parsed.Diagnostics.HasErrors(), "{0}", parsed.Diagnostics.ToString().AsView());
    auto *statement = parsed.Statement(0, 0)->As<AstSwitchStmt>();
    MARLIN_REQUIRE(statement->Sections.Count() == 2);
    const AstSwitchSection &first = statement->Sections[0];
    MARLIN_REQUIRE(first.Labels.Count() == 2);
    MARLIN_EXPECT(first.Labels[0].Pattern->As<AstDeclarationPattern>()->Name == "n" && first.Labels[0].When != nullptr);
    MARLIN_EXPECT(first.Labels[1].Pattern->Kind == AstKind::ConstantPattern);
    MARLIN_EXPECT(statement->Sections[1].HasDefault);
}
} // namespace Marlin::Tests
