#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPassManager.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirVerifier.h"
#include "ScriptBackend/ScriptDisassembler.h"
#include "ScriptBackend/ScriptTarget.h"
#include "ScriptVM/ScriptBytecode.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
/// Compiles *source* as the `Project` script package against the mounted engine, with the script pipeline and the
/// script target's legalization, writes the module, reads it back and disassembles it (MarlinScript §6, §14).
// RECOVERY-DECISION S15: the backend tests are written to the outline and the recovered assert texts; their script
// sources are new.
struct CompiledScript
{
    CompiledScript(StringView source, StringView other = {})
    {
        Memory.Add("/Project/Language/Scripts/Test.mscript", source);
        if (!other.IsEmpty())
        {
            Memory.Add("/Project/Language/Scripts/Other.mscript", other);
        }
        ISourceProvider *providers[] = {&Engine, &Memory};
        Sources = MakeUnique<CompositeSourceProvider>(Span<ISourceProvider *const>(providers));
        Options.Language = LanguageDialect::Script;
        Compilation = MakeUnique<LanguageCompilation>(*Sources, Options);
        if (!Compilation->Load() || !Compilation->CompileScriptPackage("Project", Module))
        {
            Errors.Add(Compilation->GetDiagnostics().ToString());
            return;
        }
        MirTargetRegistry targets;
        RegisterScriptTarget(targets);
        const MirTarget *target = targets.Find(ScriptTargetName);
        const MirPassRegistry registry = MirPassRegistry::CreateDefault();
        MirModulePassManager passes({.VerifyEach = true});
        String error;
        if (!registry.ParsePipeline(MirScriptPipeline, passes, error, {.VerifyEach = true}))
        {
            Errors.Add(Move(error));
            return;
        }
        target->AddLegalizationPasses(passes, Errors);
        MirModuleAnalysisManager analyses;
        passes.Run(Module, analyses);
        for (const String &problem : passes.GetErrors())
        {
            Errors.Add(problem);
        }
        if (!Errors.IsEmpty() || !target->Emit(Module, {}, Bytes, Errors))
        {
            return;
        }
        String readError;
        if (!ReadScriptModule(Bytes.AsSpan(), Data, readError))
        {
            Errors.Add(Move(readError));
            return;
        }
        StringBuilder<4096> text;
        DisassembleScriptModule(Data, text);
        Text = text.ToString();
        Ok = true;
    }

    [[nodiscard]] String Report() const
    {
        StringBuilder<1024> report;
        for (const String &error : Errors)
        {
            report.Append(error.AsView()).Append('\n');
        }
        report.Append(Text.AsView());
        return report.ToString();
    }

    [[nodiscard]] bool Has(StringView text) const
    {
        return Text.AsView().Contains(text);
    }

    [[nodiscard]] const ScriptFunctionRecord *Function(StringView name) const
    {
        for (const ScriptFunctionRecord &function : Data.Functions)
        {
            if (Data.Strings[function.Name].AsView() == name)
            {
                return &function;
            }
        }
        return nullptr;
    }

    /// What is wrong with the code of any function: an instruction that does not decode, a jump, switch or safepoint
    /// off an instruction, a call or type past its table, a map of the wrong size or a line table that does not
    /// decode; empty when nothing is.
    [[nodiscard]] String CodeProblem() const
    {
        for (const ScriptFunctionRecord &function : Data.Functions)
        {
            const StringView name = Data.Strings[function.Name].AsView();
            const Span<const uint32> code = function.Code.AsSpan();
            List<bool> starts;
            starts.Resize(code.Count() + 1, false);
            ScriptDecodedInstruction instruction;
            for (uint32 pc = 0; pc < code.Count(); pc += instruction.Words)
            {
                if (!DecodeScriptInstruction(code, pc, instruction))
                {
                    return Format("{0}: the instruction at {1} does not decode", name, pc);
                }
                starts[pc] = true;
            }
            for (uint32 pc = 0; pc < code.Count(); pc += instruction.Words)
            {
                (void)DecodeScriptInstruction(code, pc, instruction);
                const ScriptOperands operands = GetScriptOpInfo(instruction.Op).Operands;
                if (operands != ScriptOperands::None && operands != ScriptOperands::sAx &&
                    instruction.Op != ScriptOp::Fault && instruction.A >= function.FrameSlots)
                {
                    return Format("{0}: the instruction at {1} writes past the frame", name, pc);
                }
                if (int64 target = 0; GetScriptJumpTarget(instruction, pc, target))
                {
                    if (target < 0 || target >= static_cast<int64>(code.Count()) || !starts[static_cast<usize>(target)])
                    {
                        return Format("{0}: the jump at {1} leaves the instructions", name, pc);
                    }
                }
                if (instruction.Op == ScriptOp::Call && instruction.Bx >= Data.Functions.Count())
                {
                    return Format("{0}: the call at {1} names no function", name, pc);
                }
                if (instruction.Op == ScriptOp::New && instruction.Bx >= Data.Types.Count())
                {
                    return Format("{0}: the instruction at {1} names no type", name, pc);
                }
            }
            for (const ScriptSwitchTable &table : function.Switches)
            {
                if (!starts[table.Default])
                {
                    return Format("{0}: a switch target is not an instruction", name);
                }
            }
            for (const ScriptSafepoint &safepoint : function.Safepoints)
            {
                if (safepoint.Pc >= code.Count() || !starts[safepoint.Pc] ||
                    (usize{safepoint.Map} + 1) * function.MapBytes * 2 > function.Maps.Count())
                {
                    return Format("{0}: the safepoint at {1} is malformed", name, safepoint.Pc);
                }
            }
            List<ScriptLineEntry> lines;
            if (!DecodeScriptLineTable(function.LineTable.AsSpan(), lines))
            {
                return Format("{0}: the line table does not decode", name);
            }
        }
        return {};
    }

    static constexpr StringView EngineRoots[] = {"/Engine"};
    VfsSourceProvider Engine{EngineRoots};
    MemorySourceProvider Memory;
    UniquePtr<CompositeSourceProvider> Sources;
    LanguageCompileOptions Options;
    UniquePtr<LanguageCompilation> Compilation;
    MirModule Module;
    List<String> Errors;
    List<uint8> Bytes;
    ScriptModuleData Data;
    String Text;
    bool Ok = false;
};

/// The line the code at *pc* of *function* comes from, or 0.
uint32 LineAt(const ScriptFunctionRecord &function, uint32 pc)
{
    List<ScriptLineEntry> lines;
    if (!DecodeScriptLineTable(function.LineTable.AsSpan(), lines))
    {
        return 0;
    }
    uint32 line = 0;
    for (const ScriptLineEntry &entry : lines)
    {
        if (entry.Pc <= pc)
        {
            line = entry.Line;
        }
    }
    return line;
}
} // namespace

MARLIN_TEST(ScriptBackend, ArithmeticLoopsAndCalls)
{
    const CompiledScript script(R"(
int Sum(int count)
{
    int total = 0;
    for (int i = 0; i < count; i++)
    {
        total += i * 3;
    }
    return total;
}

float Blend(float a, float b, float t) => a + (b - a) * t;

double Area(double radius) => 3.0 * radius * radius;

long Wide(long a, uint b) => a * b + (a >> 3);

int Pick(int value)
{
    switch (value)
    {
    case 1:
        return 10;
    case 2:
        return 20;
    default:
        return Sum(value);
    }
}

float Length2(float x, float y) => Length(float2(x, y));
)");
    MARLIN_REQUIREF(script.Ok, "{0}", script.Report().AsView());
    MARLIN_EXPECTF(script.CodeProblem().IsEmpty(), "{0}", script.CodeProblem().AsView());
    // `i < count` is a compare-and-jump: the loop leaves when count <= i.
    for (const StringView op : {StringView("AddI32"), StringView("MulI32"), StringView("JumpLeI32"), StringView("Loop"),
                                StringView("SubF32"), StringView("MulF64"), StringView("ShrI64"), StringView("Switch"),
                                StringView("Call"), StringView("Return"), StringView("Math")})
    {
        MARLIN_EXPECTF(script.Has(op), "no {0} in\n{1}", op, script.Text.AsView());
    }
    const ScriptFunctionRecord *sum = script.Function("Sum(int)");
    MARLIN_REQUIRE(sum != nullptr);
    MARLIN_EXPECT(sum->Kind == ScriptFunctionKind::Normal && sum->ParameterSlots == 1 && sum->ReturnSlots == 1);
    MARLIN_EXPECT(sum->FrameSlots >= sum->ParameterSlots && !sum->Code.IsEmpty());
    MARLIN_EXPECT(!sum->Blocks.IsEmpty() && sum->Blocks[0] == 0);
    const ScriptFunctionRecord *pick = script.Function("Pick(int)");
    MARLIN_REQUIRE(pick != nullptr);
    MARLIN_EXPECT(pick->Switches.Count() == 1 || script.Has("JumpIf"));
    MARLIN_EXPECT(script.Data.Exports.Count() >= 5);
}

MARLIN_TEST(ScriptBackend, ObjectsDelegatesAndStackMaps)
{
    const CompiledScript script(R"(
class Node
{
    public int Value;
    public Node? Next;

    public Node(int value, Node? next)
    {
        Value = value;
        Next = next;
    }

    public virtual int Weight() => Value;
}

class Heavy : Node
{
    public Heavy(int value) : base(value, null) {}
    public override int Weight() => Value * 10;
}

int Chain(int count)
{
    Node? list = null;
    for (int i = 0; i < count; i++)
    {
        list = new Node(i, list);
    }
    int total = 0;
    while (list != null)
    {
        total += list.Weight();
        list = list.Next;
    }
    return total;
}

int Apply(Func<int, int> function, int value) => function(value);

int Delegates(int offset)
{
    Func<int, int> add = x => x + offset;
    return Apply(add, 1);
}

bool Boxes(int value)
{
    object boxed = value;
    return boxed is int && (int)boxed == value;
}
)");
    MARLIN_REQUIREF(script.Ok, "{0}", script.Report().AsView());
    MARLIN_EXPECTF(script.CodeProblem().IsEmpty(), "{0}", script.CodeProblem().AsView());
    for (const StringView op : {StringView("New"), StringView("StoreField"), StringView("LoadField"),
                                StringView("CallVirtual"), StringView("MakeDelegate"), StringView("CallDelegate"),
                                StringView("Box"), StringView("Unbox"), StringView("IsInstance")})
    {
        MARLIN_EXPECTF(script.Has(op), "no {0} in\n{1}", op, script.Text.AsView());
    }
    MARLIN_EXPECT(script.Has("class Node") && script.Has("extends Node") && script.Has("; gc"));
    const ScriptFunctionRecord *chain = script.Function("Chain(int)");
    MARLIN_REQUIRE(chain != nullptr);
    MARLIN_EXPECT(!chain->Safepoints.IsEmpty() && chain->MapBytes != 0);
    // Some map names a register holding a reference: the list.
    bool marked = false;
    for (usize i = 0; i < chain->Maps.Count(); ++i)
    {
        marked = marked || ((i / chain->MapBytes) % 2 == 0 && chain->Maps[i] != 0);
    }
    MARLIN_EXPECT(marked);
}

MARLIN_TEST(ScriptBackend, TasksIteratorsAndStatics)
{
    const CompiledScript script(R"(
static int Calls = 3;
static string Name = "counter";

IEnumerable<int> Evens(int count)
{
    for (int i = 0; i < count; i++)
    {
        yield return i * 2;
    }
}

async Task<int> Later(int value)
{
    await Frame.Next();
    Calls += 1;
    return value + Calls;
}

int Total()
{
    int total = 0;
    foreach (int value in Evens(4))
    {
        total += value;
    }
    return total + Name.Length;
}
)");
    MARLIN_REQUIREF(script.Ok, "{0}", script.Report().AsView());
    MARLIN_EXPECTF(script.CodeProblem().IsEmpty(), "{0}", script.CodeProblem().AsView());
    const ScriptFunctionRecord *evens = script.Function("Evens(int)");
    const ScriptFunctionRecord *later = script.Function("Later(int)");
    MARLIN_REQUIRE(evens != nullptr && later != nullptr);
    MARLIN_EXPECT(evens->Kind == ScriptFunctionKind::Iterator && evens->ResultShape != ~0u);
    MARLIN_EXPECT(later->Kind == ScriptFunctionKind::Async && later->ResultShape != ~0u);
    MARLIN_EXPECT(script.Has("Yield") && script.Has("Await") && script.Has("RefStatic") && script.Has("CallInterface"));
    MARLIN_EXPECT(script.Data.Statics.Count() == 2 && script.Data.StaticSize >= 12);
    MARLIN_REQUIRE(script.Data.StaticInitializers.Count() == 1);
    const ScriptFunctionRecord &initializer = script.Data.Functions[script.Data.StaticInitializers[0]];
    MARLIN_EXPECT((initializer.Flags & ScriptFunctionFlags::StaticInitializer) != 0);
    MARLIN_EXPECT(script.Has("initializer") && script.Has("statics"));
    bool imports = false;
    for (const ScriptImport &import : script.Data.Imports)
    {
        imports = imports || (import.Kind == ScriptImportKind::Native &&
                              script.Data.Strings[import.Name].AsView() == "Frame.Next");
    }
    MARLIN_EXPECT(imports);
}

MARLIN_TEST(ScriptBackend, CallsKeepTheirCallersLocation)
{
    const CompiledScript script(R"(
int Inner(int value)
{
    return value * 2;
}

int Outer(int value)
{
    int first = value + 1;
    int second = Inner(first);
    return second + Inner(second);
}
)");
    MARLIN_REQUIREF(script.Ok, "{0}", script.Report().AsView());
    const ScriptFunctionRecord *outer = script.Function("Outer(int)");
    MARLIN_REQUIRE(outer != nullptr && !outer->LineTable.IsEmpty());
    MARLIN_EXPECT((outer->Flags & ScriptFunctionFlags::Debuggable) != 0);
    List<uint32> calls;
    ScriptDecodedInstruction instruction;
    for (uint32 pc = 0; pc < outer->Code.Count(); pc += instruction.Words)
    {
        MARLIN_REQUIRE(DecodeScriptInstruction(outer->Code.AsSpan(), pc, instruction));
        if (instruction.Op == ScriptOp::Call)
        {
            calls.Add(pc);
        }
    }
    MARLIN_REQUIRE(calls.Count() == 2);
    MARLIN_EXPECTF(LineAt(*outer, calls[0]) == 10, "the first call is on line {0}", LineAt(*outer, calls[0]));
    MARLIN_EXPECTF(LineAt(*outer, calls[1]) == 11, "the second call is on line {0}", LineAt(*outer, calls[1]));
    // The safepoint of each call is the call itself, so a stack walk finds the caller's line.
    for (const uint32 call : calls)
    {
        bool found = false;
        for (const ScriptSafepoint &safepoint : outer->Safepoints)
        {
            found = found || safepoint.Pc == call;
        }
        MARLIN_EXPECT(found);
    }
}

MARLIN_TEST(ScriptBackend, SharesRegistersAndFusesBranches)
{
    const CompiledScript script(R"(
int Sum(int count)
{
    int total = 0;
    for (int i = 0; i < count; i++)
    {
        total += i;
    }
    return total;
}

int Chain(int a)
{
    int b = a * 3 + 1;
    int c = b * 5 - 2;
    int d = c * 7 + 3;
    int e = d * 11 + a;
    int f = e * 13 + b;
    return f;
}

float Clamp01(float x)
{
    if (x < 0.0f)
    {
        return 0.0f;
    }
    if (x > 1.0f)
    {
        return 1.0f;
    }
    return x;
}

class Box
{
    public int Value;
    public Box? Next;
}

float Span3(float x, float y, float z) => Length(float3(x, y, z)) + Min(x, y);

int Walk(Box? box)
{
    int total = 0;
    while (box != null)
    {
        total += box.Value;
        box = box.Next;
    }
    return total;
}
)");
    MARLIN_REQUIREF(script.Ok, "{0}", script.Report().AsView());
    MARLIN_EXPECTF(script.CodeProblem().IsEmpty(), "{0}", script.CodeProblem().AsView());
    const auto count = [&](StringView name, ScriptOp op) {
        const ScriptFunctionRecord *function = script.Function(name);
        uint32 found = 0;
        ScriptDecodedInstruction instruction;
        for (uint32 pc = 0; function != nullptr && pc < function->Code.Count(); pc += instruction.Words)
        {
            if (!DecodeScriptInstruction(function->Code.AsSpan(), pc, instruction))
            {
                return ~0u;
            }
            found += instruction.Op == op ? 1 : 0;
        }
        return found;
    };
    const ScriptFunctionRecord *sum = script.Function("Sum(int)");
    const ScriptFunctionRecord *chain = script.Function("Chain(int)");
    MARLIN_REQUIRE(sum != nullptr && chain != nullptr);
    // The loop's phis share their registers with the values flowing into them: no moves, and three registers.
    MARLIN_EXPECTF(count("Sum(int)", ScriptOp::Move) == 0 && sum->FrameSlots == 3, "{0}", script.Text.AsView());
    MARLIN_EXPECTF(count("Sum(int)", ScriptOp::JumpLeI32) == 1 && count("Sum(int)", ScriptOp::AddI32K) == 1 &&
                       count("Sum(int)", ScriptOp::LtI32) == 0 && count("Sum(int)", ScriptOp::JumpIfNot) == 0,
                   "{0}", script.Text.AsView());
    // Values that do not overlap share registers, and constants are immediates.
    MARLIN_EXPECTF(chain->FrameSlots <= 3 && count("Chain(int)", ScriptOp::LoadI) == 0, "{0}", script.Text.AsView());
    MARLIN_EXPECT(sum->ZeroSlots == 0 && chain->ZeroSlots == 0);
    // NaN fails both comparisons, so the float branches jump when theirs fails.
    MARLIN_EXPECTF(count("Clamp01(float)", ScriptOp::JumpNotLtF32) == 2, "{0}", script.Text.AsView());
    MARLIN_EXPECTF(count("Span3(float, float, float)", ScriptOp::LengthF32x3) == 1 &&
                       count("Span3(float, float, float)", ScriptOp::MinF32) == 1 &&
                       count("Span3(float, float, float)", ScriptOp::Math) == 0,
                   "{0}", script.Text.AsView());
    // The null test compares with an immediate, and the fields load by their width.
    MARLIN_EXPECTF(count("Walk(Box?)", ScriptOp::JumpEqI64K) == 1 && count("Walk(Box?)", ScriptOp::LoadField32) == 1 &&
                       count("Walk(Box?)", ScriptOp::LoadField64) == 1 && count("Walk(Box?)", ScriptOp::Zero) == 0,
                   "{0}", script.Text.AsView());
}

MARLIN_TEST(ScriptBackend, JumpsBackThroughLoopsAndKeepsVectorsWhole)
{
    const CompiledScript script(R"(
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

float Damp(float x, int count)
{
    for (int i = 0; i < count; i++)
    {
        x = x * 0.5f - 0.25f;
    }
    return x;
}

int Fib(int n) => n < 2 ? n : Fib(n - 1) + Fib(n - 2);

float3 Turn(float3 v, float3 axis, int count)
{
    for (int i = 0; i < count; i++)
    {
        v = Normalize(v + Cross(v, axis) * 0.01f);
    }
    return v;
}

class Stepper
{
    public virtual int Step(int x) => x + 1;
}

int Steps(Stepper[] steppers, int count)
{
    int total = 0;
    for (int i = 0; i < count; i++)
    {
        total = steppers[i & 1].Step(total);
    }
    return total;
}
)");
    MARLIN_REQUIREF(script.Ok, "{0}", script.Report().AsView());
    MARLIN_EXPECTF(script.CodeProblem().IsEmpty(), "{0}", script.CodeProblem().AsView());
    const auto count = [&](StringView name, ScriptOp op) {
        const ScriptFunctionRecord *function = script.Function(name);
        uint32 found = 0;
        ScriptDecodedInstruction instruction;
        for (uint32 pc = 0; function != nullptr && pc < function->Code.Count(); pc += instruction.Words)
        {
            if (!DecodeScriptInstruction(function->Code.AsSpan(), pc, instruction))
            {
                return ~0u;
            }
            found += instruction.Op == op ? 1 : 0;
        }
        return found;
    };
    // The back edge is a Loop of its own, so a loop has the header's and no Jump.
    MARLIN_EXPECTF(count("Mix(int)", ScriptOp::Loop) == 2 && count("Mix(int)", ScriptOp::Jump) == 0, "{0}",
                   script.Text.AsView());
    // Shifts and masks by constants, and float arithmetic with constants, are K forms.
    MARLIN_EXPECTF(count("Mix(int)", ScriptOp::ShrI32K) == 1 && count("Mix(int)", ScriptOp::AndI32K) == 1 &&
                       count("Mix(int)", ScriptOp::ShrI32) == 0 && count("Mix(int)", ScriptOp::And) == 0,
                   "{0}", script.Text.AsView());
    MARLIN_EXPECTF(count("Damp(float, int)", ScriptOp::MulF32K) == 1 &&
                       count("Damp(float, int)", ScriptOp::AddF32K) == 1 &&
                       count("Damp(float, int)", ScriptOp::LoadK) == 0,
                   "{0}", script.Text.AsView());
    // The edge into the phi whose value is already in place is the branch itself.
    MARLIN_EXPECTF(count("Fib(int)", ScriptOp::Jump) == 0, "{0}", script.Text.AsView());
    // Each call's window starts above what is live across it, and the values passed and returned live in it: n - 1
    // and n - 2 are computed into their windows and the results stay there.
    MARLIN_EXPECTF(count("Fib(int)", ScriptOp::Move) <= 1, "{0}", script.Text.AsView());
    // The receiver is loaded into the window and the running total lives in it; only the result moves back.
    MARLIN_EXPECTF(count("Steps(Stepper[], int)", ScriptOp::Move) <= 1, "{0}", script.Text.AsView());
    // A float3 variable stays in one register, and Cross and Normalize have instructions of their own.
    MARLIN_EXPECTF(count("Turn(float3, float3, int)", ScriptOp::Insert) == 0 &&
                       count("Turn(float3, float3, int)", ScriptOp::Extract) == 0 &&
                       count("Turn(float3, float3, int)", ScriptOp::CrossF32x3) == 1 &&
                       count("Turn(float3, float3, int)", ScriptOp::NormalizeF32x3) == 1 &&
                       count("Turn(float3, float3, int)", ScriptOp::Math) == 0,
                   "{0}", script.Text.AsView());
}
} // namespace Marlin::Tests
