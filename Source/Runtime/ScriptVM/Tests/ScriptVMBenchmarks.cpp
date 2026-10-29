#include "Testing/Testing.h"

#include "HAL/PlatformTime.h"
#include "Math/Vector.h"
#include "ScriptTestRun.h"
#include "ScriptVM/ScriptWorld.h"
#include "ScriptWorldRun.h"
#include "String/Format.h"
#include "World/EntityRef.h"

namespace Marlin::Tests
{
namespace
{
template <typename TFunction> float64 BestOf(TFunction function)
{
    float64 best = 1e30;
    for (int32 i = 0; i < 5; ++i)
    {
        const float64 start = PlatformTime::Seconds();
        function();
        const float64 elapsed = PlatformTime::Seconds() - start;
        best = elapsed < best ? elapsed : best;
    }
    return best;
}

void PrintRate(StringView name, float64 seconds, float64 count, StringView unit)
{
    Testing::Print(Format("{0,-22} {1,8:F2} ns/{2}\n", name, seconds * 1e9 / count, unit));
}

/// Each function runs its loop *n* times and returns a checksum, so the work cannot be dropped. The loops read their
/// objects and functions through arrays and fields, so the MIR passes cannot fold or inline them away.
constexpr StringView BenchmarkSource = R"(
class Node
{
    public int Value;
    public Node? Next;
}

class Stepper
{
    public virtual int Step(int x) => x + 1;
}

class DoubleStepper : Stepper
{
    public override int Step(int x) => x + 2;
}

interface IStep
{
    int Step(int x);
}

class AddStep : IStep
{
    public int Step(int x) => x + 1;
}

class XorStep : IStep
{
    public int Step(int x) => x ^ 3;
}

static Node? Sink;

int IntArith(int n)
{
    int state = 1;
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        state = state * 1103515245 + 12345;
        total += (state >> 16) & 0x7FFF;
    }
    return total;
}

int FloatArith(int n)
{
    float x = 0.0f;
    float v = 1.0f;
    for (int i = 0; i < n; i++)
    {
        x = x * 0.999f + v;
        v = v * 0.5f + 0.25f;
    }
    return (int)x;
}

int VectorMath(int n)
{
    float3 position = float3(0.0f, 0.0f, 0.0f);
    float3 velocity = float3(1.0f, 0.5f, 0.25f);
    float3 up = float3(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < n; i++)
    {
        velocity = Normalize(velocity + Cross(velocity, up) * 0.01f);
        position += velocity * 0.016f;
    }
    return (int)Length(position);
}

int Fib(int n) => n < 2 ? n : Fib(n - 1) + Fib(n - 2);

int VirtualCalls(int n)
{
    var steppers = new Stepper[] { new Stepper(), new DoubleStepper() };
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total = steppers[i & 1].Step(total);
    }
    return total;
}

int InterfaceHit(int n)
{
    var steps = new IStep[] { new AddStep(), new XorStep() };
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total = steps[0].Step(total);
    }
    return total;
}

int InterfaceMiss(int n)
{
    var steps = new IStep[] { new AddStep(), new XorStep() };
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total = steps[i & 1].Step(total);
    }
    return total;
}

int Delegates(int n)
{
    Func<int, int> step = x => x + 1;
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total = step(total);
    }
    return total;
}

int NativeCalls(int n)
{
    string text = "benchmark";
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total += text.Length;
    }
    return total;
}

int FieldChase(int n)
{
    var first = new Node();
    Node last = first;
    for (int i = 1; i < 64; i++)
    {
        var node = new Node();
        node.Value = i;
        last.Next = node;
        last = node;
    }
    last.Next = first;
    Node at = first;
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        at.Value += 1;
        total += at.Value;
        at = at.Next!;
    }
    return total;
}

int IntArray(int n)
{
    var values = new int[1024];
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total += values[i & 1023];
        values[(i * 7) & 1023] = total;
    }
    return total;
}

int VectorArray(int n)
{
    var points = new float3[1024];
    float3 step = float3(1.0f, 2.0f, 3.0f);
    for (int i = 0; i < n; i++)
    {
        points[i & 1023] += step;
    }
    return (int)points[5].x;
}

int Allocate(int n)
{
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        var node = new Node();
        node.Value = i;
        Sink = node;
        total += node.Value;
    }
    return total;
}

int Identity(int x) => x;
)";

constexpr StringView EcsSource = R"(
namespace EcsBenchmark;

[Component] public struct Position { public float3 Value; }
[Component] public struct Velocity { public float3 Value; }

[System]
public void Move(ref Position position, in Velocity velocity)
{
    position.Value += velocity.Value * 0.016f;
}
)";
} // namespace

MARLIN_TEST(Benchmark, ScriptVM, Benchmark)
{
    ScriptRun run(BenchmarkSource);
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;

    struct Case
    {
        StringView Name;
        StringView Function;
        int32 Iterations;
    };
    const Case cases[] = {
        {"IntArith", "IntArith(int)", 4000000},         {"FloatArith", "FloatArith(int)", 4000000},
        {"VectorMath", "VectorMath(int)", 1000000},     {"VirtualCalls", "VirtualCalls(int)", 2000000},
        {"InterfaceHit", "InterfaceHit(int)", 2000000}, {"InterfaceMiss", "InterfaceMiss(int)", 2000000},
        {"Delegates", "Delegates(int)", 2000000},       {"NativeCalls", "NativeCalls(int)", 2000000},
        {"FieldChase", "FieldChase(int)", 4000000},     {"IntArray", "IntArray(int)", 4000000},
        {"VectorArray", "VectorArray(int)", 2000000},   {"Allocate", "Allocate(int)", 1000000},
    };
    for (const Case &benchmark : cases)
    {
        const ScriptFunction<int32(int32)> function = domain.FindFunction<int32(int32)>(benchmark.Function);
        MARLIN_REQUIREF(function, "{0} not found", benchmark.Function);
        bool ok = true;
        const float64 seconds = BestOf([&] { ok = ok && function(benchmark.Iterations); });
        MARLIN_EXPECTF(ok, "{0} faulted: {1}", benchmark.Name, function(benchmark.Iterations).GetFault().ToString());
        PrintRate(benchmark.Name, seconds, benchmark.Iterations, "iter");
    }

    // Fib(n) makes 2 Fib(n + 1) - 1 calls: 635621 for n = 27.
    const ScriptFunction<int32(int32)> fib = domain.FindFunction<int32(int32)>("Fib(int)");
    MARLIN_REQUIRE(fib);
    MARLIN_EXPECT(fib(27).GetValue() == 196418);
    PrintRate("Fib(27)", BestOf([&] { (void)fib(27); }), 635621, "call");

    // An entry from C++: what every call into the VM pays before the first instruction.
    const ScriptFunction<int32(int32)> identity = domain.FindFunction<int32(int32)>("Identity(int)");
    MARLIN_REQUIRE(identity);
    constexpr int32 Entries = 1000000;
    const float64 entries = BestOf([&] {
        for (int32 i = 0; i < Entries; ++i)
        {
            (void)identity(i);
        }
    });
    PrintRate("Entry from C++", entries, Entries, "call");
}

MARLIN_TEST(Benchmark, ScriptSystem, Benchmark)
{
    ScriptWorldRun run(EcsSource);
    MARLIN_REQUIREF(run.Run->Ok, "{0}", run.Run->Report());
    const ComponentType *position = ScriptEcs::FindComponent("EcsBenchmark.Position");
    const ComponentType *velocity = ScriptEcs::FindComponent("EcsBenchmark.Velocity");
    MARLIN_REQUIRE(position != nullptr && velocity != nullptr);

    // The frame without entities is the world's own cost, taken off what the script system adds per entity.
    run.RunFrame();
    const float64 empty = BestOf([&] { run.RunFrame(); });
    constexpr uint32 Count = 20000;
    EntityManager &entities = run.TheWorld->GetEntities();
    for (uint32 i = 0; i < Count; ++i)
    {
        const EntityRef entity(*run.TheWorld, entities.CreateEntity());
        const Vector3 start(static_cast<float32>(i), 0.0f, 0.0f);
        const Vector3 speed(1.0f, 2.0f, 3.0f);
        entity.AddComponent(position->GetId(), &start);
        entity.AddComponent(velocity->GetId(), &speed);
    }
    run.RunFrame();
    const float64 full = BestOf([&] { run.RunFrame(); });
    Testing::Print(Format("Empty frame            {0,8:F2} us\n", empty * 1e6));
    PrintRate("Move system", full - empty, Count, "entity");
}
} // namespace Marlin::Tests
