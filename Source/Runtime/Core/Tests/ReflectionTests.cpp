#include "Testing/Testing.h"

#include "Console/Console.h"
#include "Reflection/Reflection.h"
#include "ReflectionTestTypes.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
float32 TestBase::Speed = 3.0f;

MARLIN_ENUM_FLAGS(TestFlags)

namespace
{
String Show(const TypeInfo &type, const void *value)
{
    StringBuilder<256> text;
    Reflection::FormatValue(text, type, value);
    return text.ToString();
}
} // namespace

MARLIN_TEST(Reflection, StructFields)
{
    const TypeInfo &point = TypeOf<TestPoint>();
    MARLIN_EXPECT(point.IsRegistered() && point.GetKind() == TypeKind::Struct);
    MARLIN_EXPECT(point.GetName() == "TestPoint" && point.GetFullName() == "Marlin::Tests::TestPoint");
    MARLIN_EXPECT(point.GetModule() == "CoreTests" && point.GetMetadata().Has("Tuple"));
    MARLIN_REQUIRE(point.GetFields().Count() == 2);
    const FieldInfo *x = point.FindField("X");
    MARLIN_REQUIRE(x != nullptr);
    MARLIN_EXPECT(x->GetMetadata().GetOr("Range", "") == "(0, 10)" && x->GetMetadata().GetOr("Unit", "") == "Meter");
    MARLIN_EXPECT(&x->GetOwner() == &point && &x->GetType() == &TypeOf<float32>());
    const FieldInfo *y = point.FindField("Y");
    MARLIN_EXPECT(&y->GetType() == &TypeOf<float32[2]>() && y->GetMetadata().GetOr("Tooltip", "") == "Up.");
    MARLIN_EXPECT(Reflection::FindType("TestPoint") == &point && Reflection::FindType(point.GetId()) == &point);
}

MARLIN_TEST(Reflection, Enums)
{
    const TypeInfo &motion = TypeOf<TestMotion>();
    MARLIN_EXPECT(motion.GetKind() == TypeKind::Enum && motion.GetEnumerators().Count() == 2 && !motion.IsFlags());
    MARLIN_EXPECT(motion.GetMetadata().GetOr("Tooltip", "") == "How a body moves.");
    MARLIN_EXPECT(motion.FindEnumerator("Static")->Meta.GetOr("Tooltip", "") == "Never moves.");
    MARLIN_EXPECT(motion.FindEnumerator("Dynamic")->Meta.GetOr("Tooltip", "") ==
                  "Moved by forces, as TestPoint and the docs say, a * b.");
    TestMotion value = TestMotion::Dynamic;
    MARLIN_EXPECT(Show(motion, &value) == "Dynamic");
    MARLIN_EXPECT(Reflection::ParseValue("Static", motion, &value) && value == TestMotion::Static);
    MARLIN_EXPECT(Reflection::ParseValue("4", motion, &value) && value == TestMotion::Dynamic);
    MARLIN_EXPECT(!Reflection::ParseValue("Sideways", motion, &value));

    const TypeInfo &flags = TypeOf<TestFlags>();
    TestFlags colors = TestFlags::Red | TestFlags::Blue;
    MARLIN_EXPECT(flags.IsFlags() && Show(flags, &colors) == "Red|Blue");
    MARLIN_EXPECT(Reflection::ParseValue("Green | Blue", flags, &colors) &&
                  colors == (TestFlags::Green | TestFlags::Blue));
}

MARLIN_TEST(Reflection, ClassHierarchy)
{
    const TypeInfo &base = TypeOf<TestBase>();
    const TypeInfo &derived = TypeOf<TestDerived>();
    const TypeInfo &counter = TypeOf<ITestCounter>();
    MARLIN_EXPECT(base.GetVersion() == 3 && derived.GetVersion() == 2 && counter.GetKind() == TypeKind::Interface);
    MARLIN_EXPECT(derived.GetParent() == &base && base.GetParent() == nullptr);
    MARLIN_EXPECT(derived.IsA(base) && derived.IsA(counter) && !base.IsA(derived));
    MARLIN_EXPECT(derived.GetFields().Count() == 6 && derived.GetOwnFields().Count() == 5);
    MARLIN_EXPECT(derived.FindField("Speed") != nullptr && derived.FindField("Speed")->IsStatic());
    MARLIN_EXPECT(base.IsAbstract() == false && counter.IsAbstract());

    List<const TypeInfo *> types;
    Reflection::GetDerivedTypes(base, types);
    MARLIN_EXPECT(types.Count() == 1 && types[0] == &derived);

    TestDerived object;
    TestBase *asBase = &object;
    MARLIN_EXPECT(&asBase->GetType() == &derived && &base.GetDynamicType(asBase) == &derived);
    MARLIN_EXPECT(Cast<TestDerived>(asBase) == &object);
    MARLIN_EXPECT(Cast<ITestCounter>(asBase) == static_cast<ITestCounter *>(&object));
    TestBase plain;
    MARLIN_EXPECT(Cast<TestDerived>(&plain) == nullptr);
    void *created = derived.New();
    MARLIN_REQUIRE(created != nullptr);
    MARLIN_EXPECT(&static_cast<TestBase *>(created)->GetType() == &derived);
    derived.GetOperations().Delete(created);
}

MARLIN_TEST(Reflection, Functions)
{
    const TypeInfo &derived = TypeOf<TestDerived>();
    const FunctionInfo *move = derived.FindFunction("Move");
    MARLIN_REQUIRE(move != nullptr && move->GetParameters().Count() == 2);
    MARLIN_EXPECT(move->GetParameters()[0].Name == "to" &&
                  move->GetParameters()[0].Flags == (ParameterFlags::Reference | ParameterFlags::Const));
    MARLIN_EXPECT(move->GetParameters()[1].DefaultValue == "1.5f" && move->GetReturnType() == &TypeOf<float32>());
    MARLIN_EXPECT(!move->IsStatic() && !move->IsConst());

    TestDerived object;
    TestPoint to;
    to.X = 1;
    float32 speed = 4;
    void *arguments[] = {&to, &speed};
    float32 result = 0;
    move->Invoke(&object, arguments, &result);
    MARLIN_EXPECT(result == 8 && object.Count() == 1);

    const FunctionInfo *count = TypeOf<ITestCounter>().FindFunction("Count");
    MARLIN_REQUIRE(count != nullptr && count->IsConst());
    int32 counted = 0;
    count->Invoke(static_cast<ITestCounter *>(&object), nullptr, &counted);
    MARLIN_EXPECT(counted == 1);
    const FunctionInfo *twice = derived.FindFunction("Twice");
    MARLIN_EXPECT(twice != nullptr && twice->IsStatic() &&
                  twice->GetMetadata().GetOr("Tooltip", "") == "Doubles a number.");
}

MARLIN_TEST(Reflection, EqualsCopyAndFormat)
{
    const TypeInfo &derived = TypeOf<TestDerived>();
    TestDerived object;
    TestPoint point;
    point.X = 1;
    point.Y[0] = 2;
    point.Y[1] = 3;
    object.Move(point);
    object.Maybe = 7;
    object.Weights.Add(Name("a"), 0.5f);
    object.Child = MakeUnique<TestDerived>();
    MARLIN_EXPECT(Show(derived, &object) ==
                  "{_points = [{X = 1, Y = [2, 3]}], Maybe = 7, Child = TestDerived{_points = [], Maybe = None, "
                  "Child = null, Weights = {}, Motion = Dynamic, Colors = None}, Weights = {a = 0.5}, Motion = "
                  "Dynamic, Colors = None}");

    TestDerived copy;
    MARLIN_EXPECT(!Reflection::Equals(derived, &copy, &object));
    MARLIN_EXPECT(Reflection::Copy(derived, &copy, &object));
    MARLIN_EXPECT(Reflection::Equals(derived, &copy, &object));
    MARLIN_EXPECT(copy.Child.Get() != object.Child.Get() && &copy.Child->GetType() == &derived);
    static_cast<TestDerived *>(copy.Child.Get())->Maybe = 1;
    MARLIN_EXPECT(!Reflection::Equals(derived, &copy, &object));
}

MARLIN_TEST(Reflection, Containers)
{
    const TypeInfo &list = TypeOf<List<TestPoint>>();
    MARLIN_EXPECT(list.GetName() == "List<TestPoint>" && &list == &TypeOf<List<TestPoint>>());
    MARLIN_EXPECT(list.GetKind() == TypeKind::List && list.GetElementType() == &TypeOf<TestPoint>());
    MARLIN_EXPECT((TypeOf<HashMap<Name, float32>>().GetName() == "Map<Name, float32>"));
    MARLIN_EXPECT(TypeOf<int32[4]>().GetName() == "Array<int32, 4>" && TypeOf<int32[4]>().GetFixedCount() == 4);
    MARLIN_EXPECT(TypeOf<Optional<String>>().GetName() == "Optional<String>");
    MARLIN_EXPECT(Reflection::FindType("List<TestPoint>") == &list);

    List<int32> values = {1, 2, 3};
    const ListOperations &operations = TypeOf<List<int32>>().GetListOperations();
    MARLIN_EXPECT(operations.Count(&values) == 3 && *static_cast<int32 *>(operations.At(&values, 2)) == 3);
    operations.RemoveAt(&values, 0);
    MARLIN_EXPECT(values.Count() == 2 && values[0] == 2);
}

MARLIN_TEST(Reflection, Scalars)
{
    int8 small = 0;
    MARLIN_EXPECT(Reflection::ParseValue("-128", TypeOf<int8>(), &small) && small == -128);
    MARLIN_EXPECT(!Reflection::ParseValue("128", TypeOf<int8>(), &small));
    uint32 hex = 0;
    MARLIN_EXPECT(Reflection::ParseValue("0x1F", TypeOf<uint32>(), &hex) && hex == 31);
    MARLIN_EXPECT(!Reflection::ParseValue("-1", TypeOf<uint32>(), &hex));
    bool flag = false;
    MARLIN_EXPECT(Reflection::ParseValue("TRUE", TypeOf<bool>(), &flag) && flag);
    float64 number = 0;
    MARLIN_EXPECT(Reflection::ParseValue("2.5e3", TypeOf<float64>(), &number) && number == 2500);
    Name name;
    MARLIN_EXPECT(Reflection::ParseValue("Hello", TypeOf<Name>(), &name) && name == Name("Hello"));
}

MARLIN_TEST(Console, VariablesAndCommands)
{
    ConsoleVariable *variable = Console::FindVariable("TEST.SPEED");
    MARLIN_REQUIRE(variable != nullptr);
    MARLIN_EXPECT(variable->HasFlag("Cheat") && variable->HasFlag("Scalability") && !variable->HasFlag("ReadOnly"));
    MARLIN_EXPECT(variable->ToString() == "3");

    StringBuilder<128> output;
    MARLIN_EXPECT(Console::Execute("test.Speed", output) && output.AsView() == "test.Speed = 3");
    output.Clear();
    const bool set = Console::Execute("test.Speed 9.5", output);
    MARLIN_EXPECT(set != static_cast<bool>(MARLIN_BUILD_SHIPPING));
    MARLIN_EXPECT(TestBase::Speed == (MARLIN_BUILD_SHIPPING ? 3.0f : 9.5f));
    MARLIN_EXPECT(variable->GetSetBy() == (MARLIN_BUILD_SHIPPING ? ConsoleSetBy::Default : ConsoleSetBy::Console));
    MARLIN_EXPECT(MARLIN_BUILD_SHIPPING || !variable->Set("2", ConsoleSetBy::Code));
    variable->Reset();
    MARLIN_EXPECT(variable->Set("2", ConsoleSetBy::Code) && TestBase::Speed == 2);
    MARLIN_EXPECT(!variable->Set("1", ConsoleSetBy::Config) && TestBase::Speed == 2);
    MARLIN_EXPECT(!variable->Set("fast", ConsoleSetBy::Console));
    int32 changes = 0;
    variable->AddChangedHandler([&](const ConsoleVariable &) { ++changes; });
    variable->Reset();
    MARLIN_EXPECT(TestBase::Speed == 3.0f && variable->GetSetBy() == ConsoleSetBy::Default && changes == 1);

    output.Clear();
    MARLIN_EXPECT(Console::Execute("test.Twice 21", output) && output.AsView() == "42");
    output.Clear();
    MARLIN_EXPECT(!Console::Execute("test.Twice nope", output));
    output.Clear();
    MARLIN_EXPECT(!Console::Execute("test.Twice", output));
    output.Clear();
    MARLIN_EXPECT(!Console::Execute("test.Missing", output));
    MARLIN_EXPECT(Console::FindCommand("Test.Twice")->GetHelp() == "Doubles a number.");
}
} // namespace Marlin::Tests
