#include "Testing/Testing.h"

#include "Math/DeterministicMath.h"
#include "ScriptTestRun.h"
#include "ScriptBackend/ScriptTarget.h"
#include "ScriptVM/ScriptDomain.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
template <typename T> String Fault(const ScriptResult<T> &result)
{
    return result ? String("no fault") : result.GetFault().ToString();
}

/// Runs frames of 1/60 s until *task* is no longer pending, or 600 frames pass.
ScriptTaskStatus RunUntilSettled(ScriptDomain &domain, ScriptObject *task, ScriptFaultInfo *fault = nullptr)
{
    const uint32 handle = domain.CreateHandle(task);
    ScriptTaskStatus status = domain.GetTaskStatus(domain.GetHandle(handle), fault);
    for (uint32 frame = 0; frame < 600 && status == ScriptTaskStatus::Pending; ++frame)
    {
        domain.AdvanceTime(1.0 / 60.0);
        domain.BeginFrame();
        domain.RunReady();
        status = domain.GetTaskStatus(domain.GetHandle(handle), fault);
    }
    domain.DestroyHandle(handle);
    return status;
}
} // namespace

MARLIN_TEST(ScriptVM, RunsArithmeticLoopsAndCalls)
{
    ScriptRun run(R"(
int Fib(int n) => n < 2 ? n : Fib(n - 1) + Fib(n - 2);

int Sum(int count)
{
    int total = 0;
    for (int i = 1; i <= count; i++)
    {
        total += i;
    }
    return total;
}

long Factorial(int n)
{
    long result = 1;
    for (int i = 2; i <= n; i++)
    {
        result *= i;
    }
    return result;
}

float Average(float a, float b) => (a + b) * 0.5f;
double Hypot(double x, double y) => Sqrt(x * x + y * y);
int Divide(int a, int b) => a / b;
int Overflow(int x) => checked(x * 2);
uint Mix(uint value) => (value << 3) ^ (value >> 2);
float Length3(float x, float y, float z) => Length(float3(x, y, z));
float Dot4(float a, float b) => Dot(float4(a, b, a, b), float4(1.0f, 2.0f, 3.0f, 4.0f));
ushort Wrap(ushort value) => (ushort)(value + 65500);

int Pick(int value)
{
    switch (value)
    {
    case 1:
        return 10;
    case 2:
        return 20;
    default:
        return -1;
    }
}
)");
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Fib(int)")(20).GetValue() == 6765);
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Sum(int)")(100).GetValue() == 5050);
    MARLIN_EXPECT(domain.FindFunction<int64(int32)>("Factorial(int)")(20).GetValue() == 2432902008176640000ll);
    MARLIN_EXPECT(domain.FindFunction<float32(float32, float32)>("Average(float, float)")(1.0f, 2.0f).GetValue() ==
                  1.5f);
    MARLIN_EXPECT(domain.FindFunction<float64(float64, float64)>("Hypot(double, double)")(3.0, 4.0).GetValue() == 5.0);
    MARLIN_EXPECT(domain.FindFunction<int32(int32, int32)>("Divide(int, int)")(7, 2).GetValue() == 3);
    MARLIN_EXPECT(domain.FindFunction<int32(int32, int32)>("Divide(int, int)")(1, 0).GetFault().Kind ==
                  ScriptFault::DivideByZero);
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Overflow(int)")(0x40000000).GetFault().Kind ==
                  ScriptFault::Overflow);
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Overflow(int)")(21).GetValue() == 42);
    MARLIN_EXPECT(domain.FindFunction<uint32(uint32)>("Mix(uint)")(0x80000001u).GetValue() ==
                  ((0x80000001u << 3) ^ (0x80000001u >> 2)));
    MARLIN_EXPECT(domain.FindFunction<float32(float32, float32, float32)>("Length3(float, float, float)")(2, 3, 6)
                      .GetValue() == 7.0f);
    MARLIN_EXPECT(domain.FindFunction<float32(float32, float32)>("Dot4(float, float)")(1, 2).GetValue() == 16.0f);
    MARLIN_EXPECT(domain.FindFunction<uint16(uint16)>("Wrap(ushort)")(100).GetValue() == 64);
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Pick(int)")(2).GetValue() == 20);
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Pick(int)")(7).GetValue() == -1);
    // A signature that does not match finds nothing.
    MARLIN_EXPECT(!domain.FindFunction<float32(int32)>("Fib(int)"));
    MARLIN_EXPECT(!domain.FindFunction<int32(int32)>("Missing(int)"));
}

MARLIN_TEST(ScriptVM, RunsLoopsImmediatesAndVectorInstructions)
{
    ScriptRun run(R"(
int Mix(int count)
{
    int state = 1;
    int total = 0;
    for (int i = 0; i < count; i++)
    {
        state = state * 1103515245 + 12345;
        total += (state >> 16) & 0x7FFF;
    }
    return total;
}

int Shifts(int x, uint u) => (x >> 3) + (int)(u >> 3) + (x << 4) + (int)(u & 0xF0F0u);

float Damp(float x, int count)
{
    for (int i = 0; i < count; i++)
    {
        x = x * 0.5f - 0.25f;
    }
    return x;
}

float Turn(int count)
{
    float3 v = float3(1.0f, 0.5f, 0.25f);
    float3 axis = float3(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < count; i++)
    {
        v = Normalize(v + Cross(v, axis) * 0.01f);
    }
    return Dot(v, float3(1.0f, 10.0f, 100.0f));
}

class Empty
{
}

class Derived : Empty
{
}

static Empty? Last;

int Construct(int count)
{
    int made = 0;
    for (int i = 0; i < count; i++)
    {
        Last = new Derived();
        made++;
    }
    return made;
}
)");
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;
    uint32 state = 1;
    int32 total = 0;
    for (int32 i = 0; i < 1000; ++i)
    {
        state = state * 1103515245u + 12345u;
        total += (static_cast<int32>(state) >> 16) & 0x7FFF;
    }
    const ScriptResult<int32> mix = domain.FindFunction<int32(int32)>("Mix(int)")(1000);
    MARLIN_EXPECTF(mix && mix.GetValue() == total, "{0}", Fault(mix).AsView());
    const int32 x = -1234567;
    const uint32 u = 0xF1234567u;
    const int32 shifts = (x >> 3) + static_cast<int32>(u >> 3) + static_cast<int32>(static_cast<uint32>(x) << 4) +
                         static_cast<int32>(u & 0xF0F0u);
    MARLIN_EXPECT(domain.FindFunction<int32(int32, uint32)>("Shifts(int, uint)")(x, u).GetValue() == shifts);
    float32 damped = 3.0f;
    for (int32 i = 0; i < 5; ++i)
    {
        damped = damped * 0.5f - 0.25f;
    }
    MARLIN_EXPECT(domain.FindFunction<float32(float32, int32)>("Damp(float, int)")(3.0f, 5).GetValue() == damped);
    float32 v[3] = {1.0f, 0.5f, 0.25f};
    for (int32 i = 0; i < 100; ++i)
    {
        const float32 cross[3] = {v[1] * 0.0f - v[2] * 1.0f, v[2] * 0.0f - v[0] * 0.0f, v[0] * 1.0f - v[1] * 0.0f};
        float32 sum[3] = {v[0] + cross[0] * 0.01f, v[1] + cross[1] * 0.01f, v[2] + cross[2] * 0.01f};
        const float32 length = __builtin_sqrtf(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
        v[0] = sum[0] / length;
        v[1] = sum[1] / length;
        v[2] = sum[2] / length;
    }
    const float32 turned = v[0] + v[1] * 10.0f + v[2] * 100.0f;
    const ScriptResult<float32> turn = domain.FindFunction<float32(int32)>("Turn(int)")(100);
    MARLIN_EXPECTF(turn && turn.GetValue() > turned - 1e-3f && turn.GetValue() < turned + 1e-3f, "{0} vs {1}",
                   turn.GetValue(), turned);
    // The constructors do nothing, so their calls are skipped; the objects are still made.
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Construct(int)")(50).GetValue() == 50);
}

MARLIN_TEST(ScriptVM, RunsObjectsDelegatesAndStrings)
{
    ScriptRun run(R"(
class Counter
{
    public int Value;
    public virtual int Step() => 1;
    public void Add() { Value += Step(); }
}

class Doubler : Counter
{
    public override int Step() => 2;
}

interface IShape
{
    float Area();
}

class Circle : IShape
{
    public float Radius;
    public Circle(float radius) { Radius = radius; }
    public float Area() => 3.0f * Radius * Radius;
}

class Rectangle : IShape
{
    public float Width;
    public float Height;
    public Rectangle(float width, float height) { Width = width; Height = height; }
    public float Area() => Width * Height;
}

int Objects()
{
    var counter = new Counter();
    counter.Add();
    Counter doubler = new Doubler();
    doubler.Add();
    doubler.Add();
    return counter.Value * 100 + doubler.Value;
}

float Shapes()
{
    var shapes = new IShape[] { new Circle(2.0f), new Rectangle(2.0f, 3.0f), new Circle(1.0f) };
    float total = 0.0f;
    foreach (IShape shape in shapes)
    {
        total += shape.Area();
    }
    return total;
}

int Delegates()
{
    int total = 0;
    Action<int> add = x => total += x;
    Action<int> twice = add + add;
    twice(5);
    Action<int> once = twice - add;
    once(1);
    Func<int, int> square = x => x * x;
    Func<Counter, int> step = counter => counter.Step();
    return total + square(3) + step(new Doubler());
}

string Greet(string name) => "hello " + name + "!";
int Length(string text) => text.Length;
string Describe(int value, float ratio) => $"{value:X4} {ratio:F2} {value > 0}";
bool Boxes(int value)
{
    object boxed = value;
    object same = value;
    return boxed is int && (int)boxed == value && boxed.Equals(same) && !(boxed is string);
}

int Arrays()
{
    var values = new int[10];
    for (int i = 0; i < values.Length; i++)
    {
        values[i] = i * i;
    }
    int sum = 0;
    foreach (int value in values)
    {
        sum += value;
    }
    return sum;
}
)");
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;
    const ScriptResult<int32> objects = domain.FindFunction<int32()>("Objects()")();
    MARLIN_EXPECTF(objects && objects.GetValue() == 104, "{0}", Fault(objects).AsView());
    const ScriptResult<float32> shapes = domain.FindFunction<float32()>("Shapes()")();
    MARLIN_EXPECTF(shapes && shapes.GetValue() == 21.0f, "{0}", Fault(shapes).AsView());
    const ScriptResult<int32> delegates = domain.FindFunction<int32()>("Delegates()")();
    MARLIN_EXPECTF(delegates && delegates.GetValue() == 10 + 1 + 9 + 2, "{0}", Fault(delegates).AsView());
    const ScriptResult<String> greeting = domain.FindFunction<String(StringView)>("Greet(string)")("world");
    MARLIN_EXPECTF(greeting && greeting.GetValue() == "hello world!", "{0}", Fault(greeting).AsView());
    MARLIN_EXPECT(domain.FindFunction<int32(StringView)>("Length(string)")("h\xC3\xA9llo").GetValue() == 6);
    const ScriptResult<String> described =
        domain.FindFunction<String(int32, float32)>("Describe(int, float)")(42, 1.5f);
    MARLIN_EXPECTF(described && described.GetValue() == "002A 1.50 True", "{0}",
                   described ? described.GetValue().AsView() : Fault(described).AsView());
    MARLIN_EXPECT(domain.FindFunction<bool(int32)>("Boxes(int)")(41).GetValue());
    MARLIN_EXPECT(domain.FindFunction<int32()>("Arrays()")().GetValue() == 285);
}

MARLIN_TEST(ScriptVM, FaultsEndTheEntryAndReportTheStack)
{
    ScriptRun run(R"(
int Inner(int[] values, int index) => values[index];

int Outer(int index)
{
    int[] values = new int[3];
    return Inner(values, index);
}

void Broken()
{
    Panic("broken");
}

int Divide(int a, int b) => a / b;

int Cast(object value) => ((string)value).Length;
)");
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;
    const ScriptResult<int32> bad = domain.FindFunction<int32(int32)>("Outer(int)")(5);
    MARLIN_REQUIRE(!bad);
    MARLIN_EXPECT(bad.GetFault().Kind == ScriptFault::IndexOutOfRange);
    MARLIN_REQUIRE(bad.GetFault().Stack.Count() >= 2);
    MARLIN_EXPECTF(bad.GetFault().Stack[0].Function == "Inner(int[], int)" && bad.GetFault().Stack[0].Line == 2 &&
                       bad.GetFault().Stack[1].Function == "Outer(int)" && bad.GetFault().Stack[1].Line == 7,
                   "{0}", Fault(bad).AsView());
    MARLIN_EXPECT(Fault(bad).AsView().Contains("at Inner(int[], int)"));
    // A fault ends only its entry: the domain keeps working.
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Outer(int)")(1).GetValue() == 0);
    const ScriptResult<void> panic = domain.FindFunction<void()>("Broken()")();
    MARLIN_EXPECT(!panic && panic.GetFault().Kind == ScriptFault::Panic && panic.GetFault().Message == "broken");
    MARLIN_EXPECT(domain.FindFunction<int32(int32, int32)>("Divide(int, int)")(1, 0).GetFault().Kind ==
                  ScriptFault::DivideByZero);
    const ScriptResult<int32> cast = domain.FindFunction<int32(ScriptObject *)>("Cast(object)")(nullptr);
    MARLIN_EXPECTF(!cast && cast.GetFault().Kind == ScriptFault::NullReference, "{0}", Fault(cast).AsView());
}

MARLIN_TEST(ScriptVM, CollectsGarbageAndKeepsLiveObjects)
{
    ScriptDomainOptions options;
    options.NurseryBytes = usize{256} << 10;
    options.SurvivorBytes = usize{64} << 10;
    ScriptRun run(R"(
class Node
{
    public int Value;
    public Node? Next;
    public string Label;

    public Node(int value, Node? next)
    {
        Value = value;
        Next = next;
        Label = "node " + value.ToString();
    }
}

static Node? Kept;

int Build(int count)
{
    Node? list = null;
    for (int i = 0; i < count; i++)
    {
        list = new Node(i, list);
    }
    Kept = list;
    return count;
}

int Churn(int rounds)
{
    int total = 0;
    for (int i = 0; i < rounds; i++)
    {
        var garbage = new int[64];
        garbage[63] = i;
        total += garbage[63] & 1;
        string text = "x" + i.ToString();
        total += text.Length > 0 ? 0 : 1;
    }
    return total;
}

int SumKept()
{
    int total = 0;
    Node? node = Kept;
    while (node != null)
    {
        total += node.Value + (node.Label.Length > 4 ? 0 : 1000000);
        node = node.Next;
    }
    return total;
}

Node MakeNode(int value) => new Node(value, null);
int ValueOf(Node node) => node.Value;
void Drop() { Kept = null; }
)",
                  options);
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Build(int)")(1000).GetValue() == 1000);
    ScriptObject *node = domain.FindFunction<ScriptObject *(int32)>("MakeNode(int)")(77).GetValue();
    MARLIN_REQUIRE(node != nullptr);
    const uint32 handle = domain.CreateHandle(node);
    const ScriptResult<int32> churn = domain.FindFunction<int32(int32)>("Churn(int)")(20000);
    MARLIN_EXPECTF(churn && churn.GetValue() == 10000, "{0}", Fault(churn).AsView());
    MARLIN_EXPECT(domain.GetCollectionCount() > 2);
    const ScriptResult<int32> kept = domain.FindFunction<int32()>("SumKept()")();
    MARLIN_EXPECTF(kept && kept.GetValue() == 499500, "{0}", Fault(kept).AsView());
    MARLIN_EXPECT(domain.FindFunction<int32(ScriptObject *)>("ValueOf(Node)")(domain.GetHandle(handle)).GetValue() ==
                  77);
    domain.Collect(true);
    const usize before = domain.GetHeapBytes();
    MARLIN_EXPECT(domain.FindFunction<int32()>("SumKept()")().GetValue() == 499500);
    MARLIN_REQUIRE(domain.FindFunction<void()>("Drop()")());
    domain.Collect(true);
    MARLIN_EXPECT(domain.GetHeapBytes() <= before);
    MARLIN_EXPECT(domain.FindFunction<int32()>("SumKept()")().GetValue() == 0);
    MARLIN_EXPECT(domain.FindFunction<int32(ScriptObject *)>("ValueOf(Node)")(domain.GetHandle(handle)).GetValue() ==
                  77);
    domain.DestroyHandle(handle);
    // Incremental collection at frame boundaries keeps running scripts correct.
    for (uint32 frame = 0; frame < 20; ++frame)
    {
        MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Build(int)")(500).GetValue() == 500);
        domain.BeginFrame();
    }
    MARLIN_EXPECT(domain.FindFunction<int32()>("SumKept()")().GetValue() == 124750);
}

MARLIN_TEST(ScriptVM, WeakRefsFollowAndForgetTheirTargets)
{
    ScriptRun run(R"(
class Payload
{
    public int Value;
}

static WeakRef<Payload>? Weak;
static Payload? Strong;

void Make()
{
    var payload = new Payload();
    payload.Value = 7;
    Strong = payload;
    Weak = new WeakRef<Payload>(payload);
}

int Read()
{
    Payload? target = Weak!.Target;
    return target == null ? -1 : target.Value;
}

bool Alive() => Weak!.IsAlive;
void Forget() { Strong = null; }
void Retarget() { Weak!.SetTarget(Strong); }
)");
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;
    MARLIN_REQUIRE(domain.FindFunction<void()>("Make()")());
    MARLIN_EXPECT(domain.FindFunction<int32()>("Read()")().GetValue() == 7);
    // The target moves out of the nursery and then into the old generation; the weak reference follows it.
    domain.Collect(false);
    MARLIN_EXPECT(domain.FindFunction<int32()>("Read()")().GetValue() == 7);
    domain.Collect(true);
    MARLIN_EXPECT(domain.FindFunction<int32()>("Read()")().GetValue() == 7);
    MARLIN_EXPECT(domain.FindFunction<bool()>("Alive()")().GetValue());
    MARLIN_REQUIRE(domain.FindFunction<void()>("Forget()")());
    domain.Collect(false);
    domain.Collect(true);
    MARLIN_EXPECT(!domain.FindFunction<bool()>("Alive()")().GetValue());
    MARLIN_EXPECT(domain.FindFunction<int32()>("Read()")().GetValue() == -1);
    MARLIN_REQUIRE(domain.FindFunction<void()>("Retarget()")());
    MARLIN_EXPECT(domain.FindFunction<int32()>("Read()")().GetValue() == -1);
}

MARLIN_TEST(ScriptVM, DebugCallsExistOnlyWithDebugOutput)
{
    static constexpr StringView Source = R"(
int Positive(int value)
{
    Debug.Assert(value > 0, "the value is not positive");
    Debug.Print("checked " + value.ToString());
    return value;
}
)";
    ScriptRun debug(Source, {}, true);
    ScriptRun release(Source, {}, false);
    MARLIN_REQUIREF(debug.Ok, "{0}", debug.Report().AsView());
    MARLIN_REQUIREF(release.Ok, "{0}", release.Report().AsView());
    const ScriptResult<int32> checked = debug.Domain->FindFunction<int32(int32)>("Positive(int)")(-1);
    MARLIN_EXPECTF(!checked && checked.GetFault().Kind == ScriptFault::AssertFailed &&
                       checked.GetFault().Message == "the value is not positive",
                   "{0}", Fault(checked).AsView());
    MARLIN_EXPECT(debug.Domain->FindFunction<int32(int32)>("Positive(int)")(3).GetValue() == 3);
    const ScriptResult<int32> unchecked = release.Domain->FindFunction<int32(int32)>("Positive(int)")(-1);
    MARLIN_EXPECTF(unchecked && unchecked.GetValue() == -1, "{0}", Fault(unchecked).AsView());
}

MARLIN_TEST(ScriptVM, RunsTasksAndIterators)
{
    ScriptRun run(R"(
static int Result;

IEnumerable<int> Evens(int count)
{
    for (int i = 0; i < count; i++)
    {
        yield return i * 2;
    }
}

int SumEvens(int count)
{
    int total = 0;
    foreach (int value in Evens(count))
    {
        total += value;
    }
    return total;
}

int FirstEven()
{
    foreach (int value in Evens(100))
    {
        if (value > 6)
        {
            return value;
        }
    }
    return -1;
}

async Task<int> Later(int value)
{
    await Frame.Next();
    await Frame.Next();
    return value + 1;
}

async Task Chain()
{
    int first = await Later(1);
    int second = await Later(first);
    Result = second;
}

async Task Forever()
{
    while (true)
    {
        await Frame.Next();
    }
}

async Task Failing()
{
    await Frame.Next();
    Panic("the task broke");
}

async Task<int> Waits()
{
    await Frame.Delay(0.1f);
    Task<int> a = Later(10);
    Task<int> b = Later(20);
    await Task.WhenAll(new Task[] { a, b });
    return a.Result + b.Result;
}

Task Owned(object owner) => Task.Run(Forever, owner);
object NewOwner() => new object();
int GetResult() => Result;
)");
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("SumEvens(int)")(5).GetValue() == 20);
    MARLIN_EXPECT(domain.FindFunction<int32()>("FirstEven()")().GetValue() == 8);
    ScriptObject *chain = domain.FindFunction<ScriptObject *()>("Chain()")().GetValue();
    MARLIN_REQUIRE(chain != nullptr);
    ScriptFaultInfo fault;
    ScriptTaskStatus status = RunUntilSettled(domain, chain, &fault);
    MARLIN_EXPECTF(status != ScriptTaskStatus::Pending, "the chain did not finish");
    MARLIN_EXPECTF(status != ScriptTaskStatus::Faulted, "{0}", fault.ToString().AsView());
    MARLIN_EXPECT(status == ScriptTaskStatus::Completed && domain.FindFunction<int32()>("GetResult()")().GetValue() == 3);
    ScriptObject *waits = domain.FindFunction<ScriptObject *()>("Waits()")().GetValue();
    status = RunUntilSettled(domain, waits, &fault);
    MARLIN_EXPECTF(status == ScriptTaskStatus::Completed, "{0}", fault.ToString().AsView());
    ScriptObject *failing = domain.FindFunction<ScriptObject *()>("Failing()")().GetValue();
    status = RunUntilSettled(domain, failing, &fault);
    MARLIN_EXPECT(status == ScriptTaskStatus::Faulted && fault.Kind == ScriptFault::Panic &&
                  fault.Message == "the task broke");
    ScriptObject *owner = domain.FindFunction<ScriptObject *()>("NewOwner()")().GetValue();
    const uint32 ownerHandle = domain.CreateHandle(owner);
    ScriptObject *owned = domain.FindFunction<ScriptObject *(ScriptObject *)>("Owned(object)")(owner).GetValue();
    MARLIN_REQUIRE(owned != nullptr);
    const uint32 ownedHandle = domain.CreateHandle(owned);
    for (uint32 frame = 0; frame < 5; ++frame)
    {
        domain.AdvanceTime(1.0 / 60.0);
        domain.BeginFrame();
        domain.RunReady();
    }
    MARLIN_EXPECT(domain.GetTaskStatus(domain.GetHandle(ownedHandle)) == ScriptTaskStatus::Pending);
    domain.CancelTasksOwnedBy(domain.GetHandle(ownerHandle));
    status = domain.GetTaskStatus(domain.GetHandle(ownedHandle));
    MARLIN_EXPECT(status != ScriptTaskStatus::Pending && status == ScriptTaskStatus::Canceled);
    domain.DestroyHandle(ownedHandle);
    domain.DestroyHandle(ownerHandle);
}

MARLIN_TEST(ScriptVM, RunsCompareAndJumpsAndImmediates)
{
    ScriptRun run(R"(
int Classify(int x)
{
    if (x == -3)
    {
        return 1;
    }
    if (5 < x)
    {
        return 2;
    }
    if (x >= 0)
    {
        return 3;
    }
    return 4;
}

int Unsigned(uint a, uint b) => a < b ? 1 : (a == b ? 2 : 3);
int Longs(long a, long b) => a <= b ? 1 : (a != 7 ? 2 : 3);
int Floats(float a, float b) => a < b ? 1 : (a >= b ? 2 : 3);
int Doubles(double a, double b) => !(a <= b) ? 1 : 2;
int Halves(ushort a) => a > 60000 ? 1 : (a != 7 ? 2 : 3);
int Shorts(short a) => a <= -2 ? 1 : 2;
long Steps(long a) => a - 5 + 3000000000L;
int Scale(int a) => a * 9 - 7;
short Narrow(short a) => (short)(a + 30000);
float Nan() => 0.0f / 0.0f;
float Lower(float a, float b) => Min(a, b);
float Upper(float a, float b) => Max(a, b);
float Magnitude(float a) => Abs(a);
float Down(float a) => Floor(a);
float Root(float a) => Sqrt(a);
float Dot3(float a, float b) => Dot(float3(a, b, a), float3(b, a, 2.0f));

int Many(int x)
{
    switch (x)
    {
    case 40:
        return 1;
    case -7:
        return 2;
    case 3:
        return 3;
    case 1000:
        return 4;
    case 0:
        return 5;
    case 12:
        return 6;
    case 99:
        return 7;
    case -100:
        return 8;
    case 5:
        return 9;
    case 6:
        return 10;
    default:
        return 0;
    }
}
)");
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;
    const auto classify = domain.FindFunction<int32(int32)>("Classify(int)");
    MARLIN_EXPECT(classify(-3).GetValue() == 1 && classify(6).GetValue() == 2 && classify(5).GetValue() == 3 &&
                  classify(0).GetValue() == 3 && classify(-1).GetValue() == 4);
    const auto unsignedCompare = domain.FindFunction<int32(uint32, uint32)>("Unsigned(uint, uint)");
    MARLIN_EXPECT(unsignedCompare(1, 0x80000000u).GetValue() == 1 && unsignedCompare(0x80000000u, 1).GetValue() == 3 &&
                  unsignedCompare(9, 9).GetValue() == 2);
    const auto longs = domain.FindFunction<int32(int64, int64)>("Longs(long, long)");
    MARLIN_EXPECT(longs(-(int64{1} << 40), 1).GetValue() == 1 && longs(int64{1} << 40, 1).GetValue() == 2 &&
                  longs(7, 1).GetValue() == 3);
    const float32 nan = domain.FindFunction<float32()>("Nan()")().GetValue();
    MARLIN_REQUIRE(nan != nan);
    const auto floats = domain.FindFunction<int32(float32, float32)>("Floats(float, float)");
    MARLIN_EXPECT(floats(1, 2).GetValue() == 1 && floats(2, 1).GetValue() == 2 && floats(nan, 1).GetValue() == 3 &&
                  floats(1, nan).GetValue() == 3);
    const auto doubles = domain.FindFunction<int32(float64, float64)>("Doubles(double, double)");
    MARLIN_EXPECT(doubles(2, 1).GetValue() == 1 && doubles(1, 2).GetValue() == 2 &&
                  doubles(static_cast<float64>(nan), 1).GetValue() == 1);
    const auto halves = domain.FindFunction<int32(uint16)>("Halves(ushort)");
    MARLIN_EXPECT(halves(60001).GetValue() == 1 && halves(60000).GetValue() == 2 && halves(7).GetValue() == 3);
    const auto shorts = domain.FindFunction<int32(int16)>("Shorts(short)");
    MARLIN_EXPECT(shorts(-2).GetValue() == 1 && shorts(-1).GetValue() == 2 && shorts(-30000).GetValue() == 1);
    MARLIN_EXPECT(domain.FindFunction<int64(int64)>("Steps(long)")(5).GetValue() == 3000000000ll);
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Scale(int)")(-3).GetValue() == -34);
    MARLIN_EXPECT(domain.FindFunction<int16(int16)>("Narrow(short)")(10000).GetValue() == -25536);
    const auto lower = domain.FindFunction<float32(float32, float32)>("Lower(float, float)");
    const auto upper = domain.FindFunction<float32(float32, float32)>("Upper(float, float)");
    MARLIN_EXPECT(lower(1, 2).GetValue() == 1 && lower(nan, 2).GetValue() == 2 && lower(2, nan).GetValue() == 2);
    MARLIN_EXPECT(upper(1, 2).GetValue() == 2 && upper(nan, 2).GetValue() == 2 && upper(2, nan).GetValue() == 2);
    const float32 magnitude = domain.FindFunction<float32(float32)>("Magnitude(float)")(-0.0f).GetValue();
    MARLIN_EXPECT(magnitude == 0.0f && !__builtin_signbit(magnitude));
    MARLIN_EXPECT(domain.FindFunction<float32(float32)>("Magnitude(float)")(-3.5f).GetValue() == 3.5f);
    MARLIN_EXPECT(domain.FindFunction<float32(float32)>("Down(float)")(-1.5f).GetValue() == -2.0f);
    MARLIN_EXPECT(domain.FindFunction<float32(float32)>("Root(float)")(9.0f).GetValue() == 3.0f);
    MARLIN_EXPECT(domain.FindFunction<float32(float32, float32)>("Dot3(float, float)")(2, 3).GetValue() == 16.0f);
    const auto many = domain.FindFunction<int32(int32)>("Many(int)");
    const int32 values[] = {40, -7, 3, 1000, 0, 12, 99, -100, 5, 6};
    for (int32 i = 0; i < 10; ++i)
    {
        MARLIN_EXPECT(many(values[i]).GetValue() == i + 1);
    }
    MARLIN_EXPECT(many(4).GetValue() == 0 && many(-101).GetValue() == 0 && many(1001).GetValue() == 0);
}

MARLIN_TEST(ScriptVM, KeepsReferencesInSharedRegistersAcrossCollections)
{
    ScriptDomainOptions options;
    options.NurseryBytes = usize{64} << 10;
    options.SurvivorBytes = usize{16} << 10;
    ScriptRun run(R"(
class Cell
{
    public int Value;
    public Cell? Next;
    public Cell(int value, Cell? next)
    {
        Value = value;
        Next = next;
    }
}

struct Pair
{
    public Cell First;
    public int Count;
}

int Weave(int rounds)
{
    int checksum = 0;
    for (int i = 0; i < rounds; i++)
    {
        Cell a = new Cell(i, null);
        int x = i * 3;
        Cell b = new Cell(x, a);
        int[] noise = new int[16];
        noise[i & 15] = x;
        Cell c = new Cell(noise[i & 15] + 1, b);
        Pair pair;
        pair.First = c;
        pair.Count = 3;
        object boxed = pair;
        Pair back = (Pair)boxed;
        checksum += back.First.Value + back.First.Next.Value + back.First.Next.Next.Value + back.Count;
    }
    return checksum;
}

void Bump(ref int value, int rounds)
{
    for (int i = 0; i < rounds; i++)
    {
        var garbage = new int[32];
        garbage[0] = i;
        value += garbage[0] & 1;
    }
}

int Pointed(int rounds)
{
    Cell cell = new Cell(0, null);
    int[] values = new int[8];
    Bump(ref cell.Value, rounds);
    Bump(ref values[3], rounds);
    return cell.Value + values[3];
}
)",
                  options);
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;
    int64 expected = 0;
    for (int64 i = 0; i < 5000; ++i)
    {
        expected += (i * 3 + 1) + i * 3 + i + 3;
    }
    const ScriptResult<int32> woven = domain.FindFunction<int32(int32)>("Weave(int)")(5000);
    MARLIN_EXPECTF(woven && woven.GetValue() == static_cast<int32>(expected), "{0}", Fault(woven).AsView());
    MARLIN_EXPECT(domain.GetCollectionCount() > 2);
    // Managed pointers into young objects follow them when they move.
    const ScriptResult<int32> pointed = domain.FindFunction<int32(int32)>("Pointed(int)")(4000);
    MARLIN_EXPECTF(pointed && pointed.GetValue() == 4000, "{0}", Fault(pointed).AsView());
}

MARLIN_TEST(ScriptVM, WakesDelaysInTimeThenCreationOrder)
{
    ScriptRun run(R"(
static int Log;

async Task Sleep(float seconds, int id)
{
    await Frame.Delay(seconds);
    Log = Log * 10 + id;
}

async Task All()
{
    Task canceled = Frame.Delay(0.2f);
    Task a = Sleep(0.5f, 1);
    Task b = Sleep(0.1f, 2);
    Task c = Sleep(0.3f, 3);
    Task d = Sleep(0.1f, 4);
    canceled.Cancel();
    await Task.WhenAll(new Task[] { a, b, c, d });
    Log = canceled.IsCanceled ? Log : -1;
}

int GetLog() => Log;
)");
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;
    ScriptObject *all = domain.FindFunction<ScriptObject *()>("All()")().GetValue();
    MARLIN_REQUIRE(all != nullptr);
    ScriptFaultInfo fault;
    const ScriptTaskStatus status = RunUntilSettled(domain, all, &fault);
    MARLIN_EXPECTF(status == ScriptTaskStatus::Completed, "{0}", fault.ToString().AsView());
    MARLIN_EXPECT(domain.FindFunction<int32()>("GetLog()")().GetValue() == 2431);
}

MARLIN_TEST(ScriptVM, SharesRegistersAcrossPhisAndWideFrames)
{
    // Phis that swap, and more values live at once than 8-bit register operands reach.
    StringBuilder<16384> source;
    source.Append(R"(
int Fibonacci(int n)
{
    int a = 0;
    int b = 1;
    for (int i = 0; i < n; i++)
    {
        int t = a + b;
        a = b;
        b = t;
    }
    return a;
}

int Swap(int n)
{
    int a = 1;
    int b = 2;
    for (int i = 0; i < n; i++)
    {
        int t = a;
        a = b;
        b = t;
    }
    return a * 10 + b;
}

int Wide(int x)
{
)");
    for (uint32 i = 0; i < 300; ++i)
    {
        FormatTo(source, "    int v{0} = x * {1} + {0};\n", i, i + 2);
    }
    source.Append("    int total = 0;\n    for (int i = 0; i < v299; i++)\n    {\n        total += v7;\n    }\n");
    source.Append("    return total");
    for (uint32 i = 0; i < 300; ++i)
    {
        FormatTo(source, " + v{0}", i);
    }
    source.Append(";\n}\n");
    ScriptRun run(source.AsView());
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report().AsView());
    ScriptDomain &domain = *run.Domain;
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Fibonacci(int)")(20).GetValue() == 6765);
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Swap(int)")(3).GetValue() == 21);
    MARLIN_EXPECT(domain.FindFunction<int32(int32)>("Swap(int)")(4).GetValue() == 12);
    int64 expected = 0;
    const int64 x = 3;
    for (int64 i = 0; i < 300; ++i)
    {
        expected += x * (i + 2) + i;
    }
    expected += (x * 301 + 299) * (x * 9 + 7);
    const ScriptFunctionInfo *wideInfo = domain.FindFunction("Wide(int)");
    MARLIN_REQUIRE(wideInfo != nullptr && wideInfo->FrameSlots > 256);
    const ScriptResult<int32> wide = domain.FindFunction<int32(int32)>("Wide(int)")(3);
    MARLIN_EXPECTF(wide && wide.GetValue() == static_cast<int32>(expected), "{0}", Fault(wide).AsView());
}
} // namespace Marlin::Tests
