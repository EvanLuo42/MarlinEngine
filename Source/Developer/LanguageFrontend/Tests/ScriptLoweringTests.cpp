#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPassManager.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirPrinter.h"
#include "LanguageIR/MirVerifier.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
/// Compiles *source*, at /Project/Language/Scripts/Test.mscript, as the Project package, then runs the script pipeline
/// with the verifier after each pass.
struct LoweredScript
{
    explicit LoweredScript(StringView source)
    {
        Memory.Add("/Project/Language/Scripts/Test.mscript", source);
        ISourceProvider *providers[] = {&Engine, &Memory};
        Sources = MakeUnique<CompositeSourceProvider>(Span<ISourceProvider *const>(providers));
        Options.Language = LanguageDialect::Script;
        Compilation = MakeUnique<LanguageCompilation>(*Sources, Options);
        Ok = Compilation->Load() && Compilation->CompileScriptPackage("Project", Module);
        if (!Ok)
        {
            return;
        }
        const MirPassRegistry registry = MirPassRegistry::CreateDefault();
        MirModulePassManager passes({.VerifyEach = true});
        String error;
        Ok = registry.ParsePipeline(MirScriptPipeline, passes, error, {.VerifyEach = true});
        MirModuleAnalysisManager analyses;
        passes.Run(Module, analyses);
        for (const String &problem : Module.TakeErrors())
        {
            Problems.Add(problem);
        }
        List<String> invalid;
        Ok = Ok && VerifyMirModule(Module, invalid) && Problems.IsEmpty();
        Problems.AddRange(invalid.AsSpan());
        StringBuilder<16384> text;
        PrintMirModule(Module, text);
        Text = text.ToString();
    }

    [[nodiscard]] String Report() const
    {
        StringBuilder<16384> report;
        report.Append(Compilation->GetDiagnostics().ToString().AsView());
        for (const String &problem : Problems)
        {
            report.Append(problem.AsView()).Append('\n');
        }
        report.Append(Text.AsView());
        return report.ToString();
    }

    [[nodiscard]] bool Has(StringView text) const
    {
        return Text.AsView().Contains(text);
    }

    static constexpr StringView EngineRoots[] = {"/Engine"};
    VfsSourceProvider Engine{EngineRoots};
    MemorySourceProvider Memory;
    UniquePtr<CompositeSourceProvider> Sources;
    LanguageCompileOptions Options;
    UniquePtr<LanguageCompilation> Compilation;
    MirModule Module;
    List<String> Problems;
    String Text;
    bool Ok = false;
};
} // namespace

MARLIN_TEST(LanguageScriptLowering, ClassesArraysAndDispatch)
{
    const LoweredScript lowered(R"(
namespace Game;

public interface IShape { float Area(); }

public abstract class Shape : IShape
{
    public string Name { get; init; } = "shape";
    public static int Count;
    public abstract float Area();
    public virtual string Describe() => Name + " " + Area();
}

public sealed class Circle : Shape
{
    public float Radius;
    public Circle(float radius) { Radius = radius; Count += 1; }
    public override float Area() => 3.14159f * Radius * Radius;
}

float Total(IShape[] shapes)
{
    float total = 0;
    foreach (IShape shape in shapes)
    {
        total += shape.Area();
    }
    return total;
}

string Run()
{
    var shapes = new IShape[] { new Circle(1), new Circle(2) };
    Shape first = (Shape)shapes[0];
    object boxed = 3;
    int unboxed = (int)boxed;
    Circle? circle = first as Circle;
    bool isCircle = first is Circle;
    return first.Describe() + Total(shapes) + unboxed + (circle != null) + isCircle;
}
)");
    MARLIN_REQUIREF(lowered.Ok, "{0}", lowered.Report().AsView());
    MARLIN_EXPECTF(lowered.Has("type #") && lowered.Has("class Game.Circle"), "{0}", lowered.Text.AsView());
    for (const StringView op : {StringView("New "), StringView("NewArray"), StringView("FieldAddress"),
                                StringView("ElementAddress"), StringView("CallVirtual"), StringView("CallInterface"),
                                StringView("Box"), StringView("Unbox"), StringView("IsInstance"), StringView("Cast"),
                                StringView("StringConstant")})
    {
        MARLIN_EXPECTF(lowered.Has(op), "{0} is missing\n{1}", op, lowered.Text.AsView());
    }
    MARLIN_EXPECTF(lowered.Has("native(\"String.Concat\")"), "{0}", lowered.Text.AsView());
    MARLIN_EXPECTF(lowered.Has("global @Game.Shape.Count: ptr<static"), "{0}", lowered.Text.AsView());
}

MARLIN_TEST(LanguageScriptLowering, ClosuresDelegatesAndTasks)
{
    const LoweredScript lowered(R"(
class Counter
{
    public event Action<int> Changed;
    int _count;
    public void Add(int amount)
    {
        _count += amount;
        Changed(_count);
    }
}

int Run(Counter counter)
{
    int total = 0;
    counter.Changed += value => total += value;
    Func<int, int> twice = x => x * 2 + total;
    counter.Add(twice(3));
    return total;
}

async Task<int> Load(int id)
{
    await Frame.Delay(1.0f);
    return id;
}

IEnumerable<int> Count(int n)
{
    for (int i = 0; i < n; i++)
    {
        yield return i;
    }
}

string? Name(Counter? counter) => counter?.ToString() ?? null;
)");
    MARLIN_REQUIREF(lowered.Ok, "{0}", lowered.Report().AsView());
    for (const StringView op : {StringView("MakeDelegate"), StringView("CallDelegate"), StringView("CombineDelegates"),
                                StringView("$closure"), StringView("Await"), StringView("Yield"), StringView(" async"),
                                StringView(" iterator")})
    {
        MARLIN_EXPECTF(lowered.Has(op), "{0} is missing\n{1}", op, lowered.Text.AsView());
    }
    MARLIN_EXPECTF(!lowered.Has("NullCheck"), "`?.` and `??` test for null already\n{0}", lowered.Text.AsView());
}

MARLIN_TEST(LanguageScriptLowering, LoopIterationsGetTheirOwnClosures)
{
    const LoweredScript lowered(R"(
Func<int>[] Make(int[] values)
{
    var result = new Func<int>[values.Length];
    int shared = 0;
    for (int i = 0; i < values.Length; i++)
    {
        int copy = i;
        result[i] = () => copy + shared;
    }
    foreach (int value in values)
    {
        result[0] = () => value + shared;
    }
    return result;
}
)");
    MARLIN_REQUIREF(lowered.Ok, "{0}", lowered.Report().AsView());
    for (const StringView type : {StringView("$closure"), StringView("$iteration0"), StringView("$iteration1")})
    {
        MARLIN_EXPECTF(lowered.Has(type), "{0} is missing\n{1}", type, lowered.Text.AsView());
    }
    MARLIN_EXPECTF(!lowered.Has("$iteration2"), "{0}", lowered.Text.AsView());
}

MARLIN_TEST(LanguageScriptLowering, StaticInitializersAndImports)
{
    const LoweredScript lowered(R"(
static class Settings
{
    public static int Limit = 10;
    public static string Title = "game";
}

int Twice() => Settings.Limit * 2;
)");
    MARLIN_REQUIREF(lowered.Ok, "{0}", lowered.Report().AsView());
    MARLIN_EXPECT(lowered.Module.GetStaticInitializers().Count() == 1);
    MARLIN_EXPECTF(lowered.Has("Test.mscript$static"), "{0}", lowered.Text.AsView());
}
} // namespace Marlin::Tests
