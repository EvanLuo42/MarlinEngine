#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"

namespace Marlin::Tests
{
namespace
{
/// Checks *source*, at /Project/Language/Scripts/Test.mscript, as the Project package of a Script compilation against the
/// mounted engine's core library.
struct CheckedScript
{
    explicit CheckedScript(StringView source)
    {
        Memory.Add("/Project/Language/Scripts/Test.mscript", source);
        ISourceProvider *providers[] = {&Engine, &Memory};
        Sources = MakeUnique<CompositeSourceProvider>(Span<ISourceProvider *const>(providers));
        Options.Language = LanguageDialect::Script;
        Compilation = MakeUnique<LanguageCompilation>(*Sources, Options);
        Loaded = Compilation->Load();
        Ok = Loaded && Compilation->CheckScriptPackage("Project");
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
    bool Loaded = false;
    bool Ok = false;
};
} // namespace

MARLIN_TEST(LanguageScript, ClassesInterfacesAndProperties)
{
    const CheckedScript checked(R"(
namespace Game;

public interface IDamageable
{
    float Health { get; }
    void Hit(float amount);
}

public abstract class Actor : IDamageable
{
    public string Name { get; init; } = "actor";
    public float Health { get; protected set; } = 100;
    protected int _hits;
    public static int Count;

    protected Actor(string name) { Name = name; Count += 1; }

    public virtual void Hit(float amount)
    {
        Health -= amount;
        _hits++;
    }

    public abstract string Describe();
}

public sealed class Enemy : Actor
{
    public float Armor = 0.5f;

    public Enemy() : this("enemy") { }
    public Enemy(string name) : base(name) { }

    public override void Hit(float amount)
    {
        base.Hit(amount * Armor);
    }

    public override string Describe() => Name + " has " + Health + " health after " + _hits + " hits";
}

float Total(IDamageable[] targets)
{
    float total = 0;
    for (int i = 0; i < targets.Length; i++)
    {
        total += targets[i].Health;
    }
    return total;
}

string Run()
{
    var enemy = new Enemy("orc") { Armor = 0.25f };
    enemy.Hit(10);
    IDamageable target = enemy;
    target.Hit(5);
    IDamageable[] all = new IDamageable[] { enemy, new Enemy() };
    object boxed = 42;
    int unboxed = (int)boxed;
    Actor? maybe = target as Actor;
    Enemy cast = (Enemy)target;
    bool same = maybe == cast && cast != null;
    return enemy.Describe() + Total(all) + unboxed + same;
}
)");
    MARLIN_REQUIREF(checked.Ok, "{0}", checked.Errors().AsView());
}

MARLIN_TEST(LanguageScript, ReportsClassErrors)
{
    {
        const CheckedScript checked(R"(
abstract class Shape { public abstract float Area(); }
class Square : Shape { }
)");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::MissingInterfaceMember));
    }
    {
        const CheckedScript checked(R"(
class Base { public void Run() { } }
class Derived : Base { public override void Run() { } }
)");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::NoMatchingOverload));
    }
    {
        const CheckedScript checked(R"(
sealed class Leaf { }
class Branch : Leaf { }
)");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::NotAType));
    }
    {
        const CheckedScript checked(R"(
class Node { }
void Run() { Node node = null; }
)");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::NoConversion));
    }
    {
        const CheckedScript checked(R"(
interface IRun { void Run(); }
class Runner : IRun { }
)");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::MissingInterfaceMember));
    }
    {
        const CheckedScript checked(R"(
class Base { }
class Derived : Base { }
void Run() { Base b = new Derived(); Derived d = b; }
)");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::NoConversion));
    }
    {
        const CheckedScript checked(R"(
class Point { public int X { get; init; } }
void Run() { var p = new Point { X = 1 }; p.X = 2; }
)");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::NotAssignable));
    }
}

MARLIN_TEST(LanguageScript, NullabilityIsProvenByFlow)
{
    const CheckedScript checked(R"(
class Node
{
    public Node? Next;
    public int Value;
}

int Sum(Node? first)
{
    int total = 0;
    Node? node = first;
    while (node != null)
    {
        total += node.Value;
        node = node.Next;
    }
    return total;
}

int Head(Node? node)
{
    if (node == null)
    {
        return 0;
    }
    return node.Value;
}

int Guarded(Node? node) => node != null && node.Value > 0 ? node.Value : -1;

int Tested(object? value)
{
    if (value is Node node)
    {
        return node.Value;
    }
    return value is not null ? 1 : 0;
}

int Asserted(Node? node) => node!.Value + (node?.Value ?? 0);

string Named(string? name)
{
    string? shown = name;
    shown ??= "nobody";
    return shown;
}
)");
    MARLIN_REQUIREF(checked.Ok, "{0}", checked.Errors().AsView());
}

MARLIN_TEST(LanguageScript, ReportsPossiblyNullUses)
{
    {
        const CheckedScript checked(R"(
class Node { public int Value; }
int Read(Node? node) => node.Value;
)");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::PossiblyNull));
    }
    {
        const CheckedScript checked(R"(
class Node { public int Value; }
int Read(Node? node)
{
    if (node != null)
    {
        node = null;
    }
    return node.Value;
}
)");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::PossiblyNull));
    }
    {
        const CheckedScript checked(R"(
int Length(string? text) => text.Length;
)");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::PossiblyNull));
    }
}
} // namespace Marlin::Tests

namespace Marlin::Tests
{
MARLIN_TEST(LanguageScript, PatternsAndSwitchExpressions)
{
    const CheckedScript checked(R"(
public enum Shape { Circle, Square, Triangle }

class Item
{
    public string Name = "";
    public int Count;
}

string Describe(object value) => value switch
{
    int n when n < 0 => "negative",
    int n => "number " + n,
    string s => "text " + s,
    Item { Count: > 10 } item => "many " + item.Name,
    Item { Count: 0 } => "none",
    null => "nothing",
    _ => "something",
};

int Sides(Shape shape) => shape switch
{
    Shape.Circle => 0,
    Shape.Square => 4,
    Shape.Triangle => 3,
};

int Classify(int value)
{
    switch (value)
    {
    case 0:
        return 0;
    case < 10 and > 0:
        return 1;
    case var other when other >= 10:
        return 2;
    default:
        return -1;
    }
}

bool IsSmall(int value) => value is >= 0 and < 8 or 100;
)");
    MARLIN_REQUIREF(checked.Ok, "{0}", checked.Errors().AsView());
}

MARLIN_TEST(LanguageScript, ReportsIncompleteSwitches)
{
    const CheckedScript checked(R"(
public enum Shape { Circle, Square, Triangle }
int Sides(Shape shape) => shape switch { Shape.Circle => 0, Shape.Square => 4 };
)");
    MARLIN_EXPECT(checked.HasError(DiagnosticCode::InvalidStatement));
}
MARLIN_TEST(LanguageScript, DelegatesEventsAndLambdas)
{
    const CheckedScript checked(R"(
public delegate float Curve(float t);

public class Health
{
    public event Action<float> Changed;
    float _value = 100;

    public void Apply(float delta)
    {
        _value += delta;
        Changed(_value);
    }
}

class Bar
{
    public float Shown;
    public void Set(float value) { Shown = value; }
}

float Ease(float t) => t * t * (3 - 2 * t);

float Run(Health health, Bar bar)
{
    float total = 0;
    health.Changed += v => total += v;
    health.Changed += bar.Set;
    Curve curve = Ease;
    Curve doubled = t => curve(t) * 2;
    Func<int, int, int> add = (int a, int b) => a + b;
    Action log = static () => { };
    health.Apply(-10);
    log();
    return doubled(0.5f) + add(1, 2) + total;
}
)");
    MARLIN_REQUIREF(checked.Ok, "{0}", checked.Errors().AsView());
}

MARLIN_TEST(LanguageScript, NullOperatorsAndInterpolation)
{
    const CheckedScript checked(R"(
class Player
{
    public string? Nickname;
    public Player? Target;
    public int Score;
}

string Describe(Player? player, int? bonus)
{
    string name = player?.Nickname ?? "nobody";
    int? targetScore = player?.Target?.Score;
    int extra = bonus.GetValueOrDefault() + (bonus.HasValue ? bonus.Value : 0);
    string? nickname = null;
    nickname ??= "x";
    int sum = checked(extra + 1);
    return $"{name} aims at {targetScore ?? 0} with {extra:D3} ({sum / 2.0f:F2}) {nickname}";
}
)");
    MARLIN_REQUIREF(checked.Ok, "{0}", checked.Errors().AsView());
}

MARLIN_TEST(LanguageScript, TasksIteratorsAndForeach)
{
    const CheckedScript checked(R"(
async Task<int> Load(int id)
{
    await Frame.Delay(0.5f);
    return id * 2;
}

async Task Patrol(int[] points)
{
    foreach (int point in points)
    {
        int loaded = await Load(point);
        await Frame.Next();
    }
}

IEnumerable<int> Evens(int count)
{
    for (int i = 0; i < count; i++)
    {
        if (i % 2 == 0)
        {
            yield return i;
        }
    }
}

int Sum(IEnumerable<int> values)
{
    int total = 0;
    foreach (var value in values)
    {
        total += value;
    }
    return total;
}

int Run() => Sum(Evens(10));
)");
    MARLIN_REQUIREF(checked.Ok, "{0}", checked.Errors().AsView());
}

MARLIN_TEST(LanguageScript, ReportsTaskAndLambdaMistakes)
{
    {
        const CheckedScript checked("int Wait() { await Frame.Next(); return 1; }");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::InvalidStatement));
    }
    {
        const CheckedScript checked("int Run() { int x = 1; Func<int> f = static () => x; return f(); }");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::InvalidStatement));
    }
    {
        const CheckedScript checked("void Run() { Func<int, int> f = (a, b) => a; }");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::WrongArgumentCount));
    }
    {
        const CheckedScript checked("class H { public event Action Fired; } void Run(H h) { h.Fired(); }");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::NotAssignable));
    }
}

MARLIN_TEST(LanguageScript, ConstructorsAssignReferenceFields)
{
    {
        const CheckedScript checked(R"(
class Owner
{
    public string Name;
    public Owner? Parent;
    public string Title = "none";
    public Owner(string name) { Name = name; }
    public Owner() : this("anonymous") { }
}
)");
        MARLIN_REQUIREF(checked.Ok, "{0}", checked.Errors().AsView());
    }
    {
        const CheckedScript checked(R"(
class Owner
{
    public string Name;
    public Owner(bool named) { if (named) { Name = "x"; } }
}
)");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::UnassignedVariable));
    }
    {
        const CheckedScript checked("class Owner { public string Name; }");
        MARLIN_EXPECT(checked.HasError(DiagnosticCode::UnassignedVariable));
    }
}
} // namespace Marlin::Tests
