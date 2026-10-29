#include "Testing/Testing.h"

#include "LanguageIR/MirBuilder.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirPrinter.h"
#include "LanguageIR/MirVerifier.h"
#include "Memory/UniquePtr.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
/// The derivative pipeline of MirShaderPipeline, with verification after every pass; returns the first error.
String Differentiate(MirModule &module)
{
    const MirPassRegistry registry = MirPassRegistry::CreateDefault();
    MirModulePassManager passes({.VerifyEach = true});
    String error;
    MARLIN_REQUIREF(registry.ParsePipeline("autodiff,inline,function(sroa,mem2reg,constfold,deadbranch,simplifycfg,cse,"
                                           "constfold,cse,constfold,dce)",
                                           passes, error, {.VerifyEach = true}),
                    "{0}", error.AsView());
    MirModuleAnalysisManager analyses;
    passes.Run(module, analyses);
    return passes.GetErrors().IsEmpty() ? String() : passes.GetErrors()[0];
}

/// `f = x > 1 ? x * x : 3 * x + y; return f + f * f;`, as the frontend lowers it after mem2reg: a selection with a
/// phi at its merge.
MirFunction *BuildPiecewise(MirModule &module)
{
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *parameters[] = {f32, f32};
    MirFunction *function = module.AddFunction(types.GetFunction(f32, parameters), "Piecewise");
    MirBlock *entry = function->AddBlock("entry");
    MirBlock *then = function->AddBlock("then");
    MirBlock *otherwise = function->AddBlock("else");
    MirBlock *merge = function->AddBlock("merge");
    MirValue *x = function->GetArguments()[0];
    MirValue *y = function->GetArguments()[1];
    MirBuilder builder(module);
    builder.SetInsertPoint(entry);
    entry->Merge = {MirMergeKind::Selection, merge};
    builder.CondBranch(builder.Compare(MirOp::Greater, x, module.GetFloat(f32, 1.0)), then, otherwise);
    builder.SetInsertPoint(then);
    MirValue *square = builder.Binary(MirOp::Mul, x, x);
    builder.Branch(merge);
    builder.SetInsertPoint(otherwise);
    MirValue *line = builder.Binary(MirOp::Add, builder.Binary(MirOp::Mul, module.GetFloat(f32, 3.0), x), y);
    builder.Branch(merge);
    builder.SetInsertPoint(merge);
    MirInstruction *phi = builder.Phi(f32);
    phi->AddOperand(square);
    phi->AddTarget(then);
    phi->AddOperand(line);
    phi->AddTarget(otherwise);
    builder.Return(builder.Binary(MirOp::Add, phi, builder.Binary(MirOp::Mul, phi, phi)));
    return function;
}

const MirType *PairOf(MirModule &module, const MirType *type)
{
    const MirStructField fields[] = {{"Primal", type, MirTypeContext::AutoOffset},
                                     {"Differential", type, MirTypeContext::AutoOffset}};
    return module.GetTypes().CreateStruct("DifferentialPair", fields);
}

/// A compute entry point that stores *value*, built by *body*, into a private global, so the result survives.
template <typename TBody> MirGlobal *BuildEntry(MirModule &module, TBody body)
{
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    MirGlobal *result = module.AddGlobal(f32, MirAddressSpace::Private, "Result");
    MirFunction *main = module.AddFunction(types.GetFunction(types.GetVoid(), {}), "Main");
    MirBuilder builder(module);
    builder.SetInsertPoint(main->AddBlock("entry"));
    builder.Store(result, body(builder));
    builder.Return();
    module.AddEntryPoint("Main", MirStage::Compute, main).Interface.Add(result);
    return result;
}

/// The constant the entry point stores, or NaN when it is not constant.
double StoredConstant(const MirGlobal *global)
{
    for (const MirUse &use : global->GetUses())
    {
        const MirValue *value = use.User->GetOperand(1);
        if (use.User->GetOp() == MirOp::Store && value->GetValueKind() == MirValueKind::Constant)
        {
            return static_cast<const MirConstant *>(value)->GetFloat();
        }
    }
    return __builtin_nan("");
}

String Print(const MirModule &module)
{
    StringBuilder<4096> text;
    PrintMirModule(module, text);
    return text.ToString();
}

bool Near(double value, double expected)
{
    return __builtin_fabs(value - expected) < 1e-4;
}

/// A value the interpreter computes: a number, or the parts of a vector, matrix (its rows), array or struct.
struct Computed
{
    double Number = 0;
    List<Computed> Parts;
};

/// Runs a function without parameters, the MIR the derivative tests leave after the pipeline has inlined
/// everything into their entry point: arithmetic, composites, local and global variables, phis and structured
/// branches. Enough to check that derivatives through loops come out right, which constant folding cannot show.
class Interpreter
{
public:
    /// Runs *function*; false, with the reason in *error*, on what it cannot run.
    bool Run(const MirFunction &function, String &error)
    {
        const MirBlock *previous = nullptr;
        const MirBlock *block = function.GetEntry();
        for (uint32 steps = 0; steps < 100000; ++steps)
        {
            List<const MirInstruction *> phis;
            List<Computed> incoming;
            for (const MirInstruction *phi : *block)
            {
                if (phi->GetOp() != MirOp::Phi)
                {
                    break;
                }
                for (usize i = 0; i < phi->GetTargets().Count(); ++i)
                {
                    if (phi->GetTargets()[i] == previous)
                    {
                        phis.Add(phi);
                        incoming.Add(Get(phi->GetOperand(i)));
                    }
                }
            }
            for (usize i = 0; i < phis.Count(); ++i)
            {
                _values.Set(phis[i], incoming[i]);
            }
            const MirBlock *next = nullptr;
            for (const MirInstruction *instruction : *block)
            {
                switch (instruction->GetOp())
                {
                case MirOp::Phi:
                    continue;
                case MirOp::Branch:
                    next = instruction->GetTargets()[0];
                    break;
                case MirOp::CondBranch:
                    next = instruction->GetTargets()[Get(instruction->GetOperand(0)).Number != 0 ? 0 : 1];
                    break;
                case MirOp::Return:
                    return true;
                default:
                    if (!Execute(*instruction, error))
                    {
                        return false;
                    }
                    continue;
                }
                break;
            }
            previous = block;
            block = next;
        }
        error = "it runs too long";
        return false;
    }

    /// The scalar a private global holds.
    double Read(const MirGlobal *global)
    {
        return Pointer(global)->Number;
    }

private:
    static Computed Zero(const MirType *type)
    {
        Computed value;
        switch (type->GetKind())
        {
        case MirTypeKind::Vector:
        case MirTypeKind::Array:
            for (uint32 i = 0; i < type->GetCount(); ++i)
            {
                value.Parts.Add(Zero(type->GetElement()));
            }
            break;
        case MirTypeKind::Matrix:
            for (uint32 i = 0; i < type->GetRows(); ++i)
            {
                value.Parts.Add(Zero(type->GetElement()));
            }
            break;
        case MirTypeKind::Struct:
            for (const MirStructField &field : type->GetFields())
            {
                value.Parts.Add(Zero(field.Type));
            }
            break;
        default:
            break;
        }
        return value;
    }

    static Computed FromConstant(const MirConstant *constant)
    {
        switch (constant->GetConstantKind())
        {
        case MirConstantKind::Scalar:
        {
            Computed value;
            value.Number = constant->GetType()->Is(MirTypeKind::Float) ? constant->GetFloat()
                                                                       : static_cast<double>(constant->GetInt());
            return value;
        }
        case MirConstantKind::Composite:
        {
            Computed value;
            for (const MirConstant *element : constant->GetElements())
            {
                value.Parts.Add(FromConstant(element));
            }
            return value;
        }
        default:
            return Zero(constant->GetType());
        }
    }

    Computed Get(const MirValue *value)
    {
        if (value->GetValueKind() == MirValueKind::Constant)
        {
            return FromConstant(static_cast<const MirConstant *>(value));
        }
        Computed *found = _values.Find(value);
        return found != nullptr ? *found : Computed();
    }

    Computed *Pointer(const MirValue *value)
    {
        if (Computed **found = _pointers.Find(value))
        {
            return *found;
        }
        // A global, made on first use.
        _storage.Add(MakeUnique<Computed>(Zero(value->GetType()->GetElement())));
        _pointers.Set(value, _storage.Last().Get());
        return _storage.Last().Get();
    }

    template <typename TOperation> static Computed Map(const Computed &a, const Computed &b, TOperation operation)
    {
        Computed result;
        if (a.Parts.IsEmpty() && b.Parts.IsEmpty())
        {
            result.Number = operation(a.Number, b.Number);
            return result;
        }
        const usize count = a.Parts.IsEmpty() ? b.Parts.Count() : a.Parts.Count();
        for (usize i = 0; i < count; ++i)
        {
            result.Parts.Add(Map(a.Parts.IsEmpty() ? a : a.Parts[i], b.Parts.IsEmpty() ? b : b.Parts[i], operation));
        }
        return result;
    }

    static void Flatten(const Computed &value, List<Computed> &scalars)
    {
        if (value.Parts.IsEmpty())
        {
            scalars.Add(value);
            return;
        }
        for (const Computed &part : value.Parts)
        {
            Flatten(part, scalars);
        }
    }

    bool Execute(const MirInstruction &instruction, String &error)
    {
        auto operand = [&](usize index) { return Get(instruction.GetOperand(index)); };
        auto set = [&](Computed value) { _values.Set(&instruction, Move(value)); };
        auto path = [&](Computed *value) {
            for (const int64 index : instruction.GetLiterals())
            {
                value = &value->Parts[static_cast<usize>(index)];
            }
            return value;
        };
        const bool isInt = instruction.GetType()->Is(MirTypeKind::Int) ||
                           ((instruction.GetType()->Is(MirTypeKind::Vector)) &&
                            instruction.GetType()->GetElement()->Is(MirTypeKind::Int));
        switch (instruction.GetOp())
        {
        case MirOp::Variable:
            _storage.Add(MakeUnique<Computed>(Zero(instruction.GetType()->GetElement())));
            _pointers.Set(&instruction, _storage.Last().Get());
            return true;
        case MirOp::Load:
            set(*Pointer(instruction.GetOperand(0)));
            return true;
        case MirOp::Store:
            *Pointer(instruction.GetOperand(0)) = operand(1);
            return true;
        case MirOp::AccessChain:
        {
            Computed *pointer = Pointer(instruction.GetOperand(0));
            for (usize i = 1; i < instruction.GetOperands().Count(); ++i)
            {
                const auto index = static_cast<usize>(operand(i).Number);
                if (index >= pointer->Parts.Count())
                {
                    error = Format("index {0} is out of bounds", index);
                    return false;
                }
                pointer = &pointer->Parts[index];
            }
            _pointers.Set(&instruction, pointer);
            return true;
        }
        case MirOp::Add:
            set(Map(operand(0), operand(1), [](double a, double b) { return a + b; }));
            return true;
        case MirOp::Sub:
            set(Map(operand(0), operand(1), [](double a, double b) { return a - b; }));
            return true;
        case MirOp::Mul:
        case MirOp::VectorTimesScalar:
        case MirOp::MatrixTimesScalar:
            set(Map(operand(0), operand(1), [](double a, double b) { return a * b; }));
            return true;
        case MirOp::Div:
            set(Map(operand(0), operand(1), [&](double a, double b) {
                return isInt ? static_cast<double>(static_cast<int64>(a / b)) : a / b;
            }));
            return true;
        case MirOp::Negate:
            set(Map(operand(0), operand(0), [](double a, double) { return -a; }));
            return true;
        case MirOp::Less:
            set(Map(operand(0), operand(1), [](double a, double b) { return a < b ? 1.0 : 0.0; }));
            return true;
        case MirOp::LessEqual:
            set(Map(operand(0), operand(1), [](double a, double b) { return a <= b ? 1.0 : 0.0; }));
            return true;
        case MirOp::Greater:
            set(Map(operand(0), operand(1), [](double a, double b) { return a > b ? 1.0 : 0.0; }));
            return true;
        case MirOp::GreaterEqual:
            set(Map(operand(0), operand(1), [](double a, double b) { return a >= b ? 1.0 : 0.0; }));
            return true;
        case MirOp::Equal:
            set(Map(operand(0), operand(1), [](double a, double b) { return a == b ? 1.0 : 0.0; }));
            return true;
        case MirOp::NotEqual:
            set(Map(operand(0), operand(1), [](double a, double b) { return a != b ? 1.0 : 0.0; }));
            return true;
        case MirOp::Convert:
            set(Map(operand(0), operand(0),
                    [&](double a, double) { return isInt ? static_cast<double>(static_cast<int64>(a)) : a; }));
            return true;
        case MirOp::Select:
        {
            const Computed condition = operand(0);
            if (condition.Parts.IsEmpty())
            {
                set(condition.Number != 0 ? operand(1) : operand(2));
                return true;
            }
            Computed result = operand(1);
            const Computed otherwise = operand(2);
            for (usize i = 0; i < condition.Parts.Count(); ++i)
            {
                if (condition.Parts[i].Number == 0)
                {
                    result.Parts[i] = otherwise.Parts[i];
                }
            }
            set(Move(result));
            return true;
        }
        case MirOp::CompositeConstruct:
        {
            Computed result;
            for (usize i = 0; i < instruction.GetOperands().Count(); ++i)
            {
                if (instruction.GetType()->Is(MirTypeKind::Vector))
                {
                    Flatten(operand(i), result.Parts);
                }
                else
                {
                    result.Parts.Add(operand(i));
                }
            }
            set(Move(result));
            return true;
        }
        case MirOp::CompositeExtract:
        {
            Computed composite = operand(0);
            set(*path(&composite));
            return true;
        }
        case MirOp::CompositeInsert:
        {
            Computed composite = operand(0);
            *path(&composite) = operand(1);
            set(Move(composite));
            return true;
        }
        case MirOp::Intrinsic:
            switch (instruction.GetIntrinsic())
            {
            case MirIntrinsic::Min:
                set(Map(operand(0), operand(1), [](double a, double b) { return a < b ? a : b; }));
                return true;
            case MirIntrinsic::Max:
                set(Map(operand(0), operand(1), [](double a, double b) { return a > b ? a : b; }));
                return true;
            default:
                break;
            }
            error = Format("cannot run the intrinsic {0}", GetMirIntrinsicName(instruction.GetIntrinsic()));
            return false;
        default:
            error = Format("cannot run {0}", GetMirOpName(instruction.GetOp()));
            return false;
        }
    }

    HashMap<const MirValue *, Computed> _values;
    HashMap<const MirValue *, Computed *> _pointers;
    List<UniquePtr<Computed>> _storage;
};

/// A compute entry point that stores each value *body* adds to its list into a private global of its own.
template <typename TBody> List<MirGlobal *> BuildResults(MirModule &module, TBody body)
{
    MirTypeContext &types = module.GetTypes();
    MirFunction *main = module.AddFunction(types.GetFunction(types.GetVoid(), {}), "Main");
    MirBuilder builder(module);
    builder.SetInsertPoint(main->AddBlock("entry"));
    List<MirValue *> values;
    body(builder, values);
    MirEntryPoint &entry = module.AddEntryPoint("Main", MirStage::Compute, main);
    List<MirGlobal *> globals;
    for (MirValue *value : values)
    {
        globals.Add(module.AddGlobal(value->GetType(), MirAddressSpace::Private, "Result"));
        builder.Store(globals.Last(), value);
        entry.Interface.Add(globals.Last());
    }
    builder.Return();
    return globals;
}

/// Adds the forward derivative of *function* at *x* with the derivative 1, then the gradient Backward writes for
/// *x* with dResult 1, to *values*; *others* are the arguments after x.
void AddDerivatives(MirModule &module, MirBuilder &builder, MirFunction *function, double x,
                    Span<MirValue *const> others, List<MirValue *> &values)
{
    const MirType *f32 = module.GetTypes().GetFloat(32);
    const MirType *pair = PairOf(module, f32);
    const uint32 differential[] = {1};
    MirConstant *forwardPair[] = {module.GetFloat(f32, x), module.GetFloat(f32, 1.0)};
    List<MirValue *> forward;
    forward.Add(function);
    forward.Add(module.GetComposite(pair, forwardPair));
    forward.AddRange(others);
    const int64 pairs[] = {1};
    values.Add(builder.Extract(builder.Intrinsic(MirIntrinsic::ForwardDerivative, pair, forward.AsSpan(), pairs),
                               differential));
    MirInstruction *variable = builder.Variable(pair, "x");
    MirConstant *backwardPair[] = {module.GetFloat(f32, x), module.GetFloat(f32, 0.0)};
    builder.Store(variable, module.GetComposite(pair, backwardPair));
    List<MirValue *> backward;
    backward.Add(function);
    backward.Add(variable);
    backward.AddRange(others);
    backward.Add(module.GetFloat(f32, 1.0));
    const int64 literals[] = {1, 1};
    builder.Intrinsic(MirIntrinsic::BackwardDerivative, module.GetTypes().GetVoid(), backward.AsSpan(), literals);
    values.Add(builder.Extract(builder.Load(variable), differential));
}

/// Differentiates *module* and runs its entry point; the values of *results*.
List<double> RunDerivatives(MirModule &module, Span<MirGlobal *const> results)
{
    List<double> values;
    const String error = Differentiate(module);
    MARLIN_EXPECTF(error.IsEmpty(), "{0}\n{1}", error.AsView(), Print(module).AsView());
    if (!error.IsEmpty())
    {
        return values;
    }
    Interpreter interpreter;
    String problem;
    const bool ran = interpreter.Run(*module.GetEntryPoints()[0]->Function, problem);
    MARLIN_EXPECTF(ran, "{0}\n{1}", problem.AsView(), Print(module).AsView());
    for (const MirGlobal *result : results)
    {
        values.Add(ran ? interpreter.Read(result) : __builtin_nan(""));
    }
    return values;
}

/// A loop as the frontend lowers it after mem2reg: *header* starts it, with phis; *check* tests the condition
/// before the body; *latch* steps it.
struct LoopBlocks
{
    MirBlock *Header = nullptr;
    MirBlock *Check = nullptr;
    MirBlock *Body = nullptr;
    MirBlock *Latch = nullptr;
    MirBlock *Merge = nullptr;
};

LoopBlocks AddLoop(MirFunction *function, uint32 maxIterations, bool testFirst = true)
{
    LoopBlocks loop;
    loop.Header = function->AddBlock("loop.header");
    loop.Check = testFirst ? function->AddBlock("loop.check") : nullptr;
    loop.Body = function->AddBlock("loop.body");
    loop.Latch = function->AddBlock("loop.continue");
    loop.Merge = function->AddBlock("loop.merge");
    loop.Header->Merge = {MirMergeKind::Loop, loop.Merge, loop.Latch, MirLoopControl::None, 0, maxIterations};
    return loop;
}

/// Gives *phi* its value *first* from *from* and *back* from the latch *latch*.
void Complete(MirInstruction *phi, MirValue *first, MirBlock *from, MirValue *back, MirBlock *latch)
{
    phi->AddOperand(first);
    phi->AddTarget(from);
    phi->AddOperand(back);
    phi->AddTarget(latch);
}

/// `p = 1; for (int i = 0; i < count; i++) { p = p * (x + i); } return p;` with `[MaxIterations(maxIterations)]`.
MirFunction *BuildProduct(MirModule &module, uint32 maxIterations)
{
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *i32 = types.GetInt(32, true);
    const MirType *parameters[] = {f32, i32};
    MirFunction *function = module.AddFunction(types.GetFunction(f32, parameters), "Product");
    MirBlock *entry = function->AddBlock("entry");
    const LoopBlocks loop = AddLoop(function, maxIterations);
    MirValue *x = function->GetArguments()[0];
    MirValue *count = function->GetArguments()[1];
    MirBuilder builder(module);
    builder.SetInsertPoint(entry);
    builder.Branch(loop.Header);
    builder.SetInsertPoint(loop.Header);
    MirInstruction *i = builder.Phi(i32);
    MirInstruction *p = builder.Phi(f32);
    builder.Branch(loop.Check);
    builder.SetInsertPoint(loop.Check);
    builder.CondBranch(builder.Compare(MirOp::Less, i, count), loop.Body, loop.Merge);
    builder.SetInsertPoint(loop.Body);
    MirValue *next = builder.Binary(MirOp::Mul, p, builder.Binary(MirOp::Add, x, builder.Convert(i, f32)));
    builder.Branch(loop.Latch);
    builder.SetInsertPoint(loop.Latch);
    MirValue *step = builder.Binary(MirOp::Add, i, module.GetInt(i32, 1));
    builder.Branch(loop.Header);
    builder.SetInsertPoint(loop.Merge);
    builder.Return(p);
    Complete(i, module.GetInt(i32, 0), entry, step, loop.Latch);
    Complete(p, module.GetFloat(f32, 1.0), entry, next, loop.Latch);
    return function;
}
} // namespace

MARLIN_TEST(MirAutodiff, ForwardModeFollowsTheTakenBranch)
{
    MirModule module;
    MirFunction *piecewise = BuildPiecewise(module);
    const MirType *f32 = module.GetTypes().GetFloat(32);
    const MirType *pair = PairOf(module, f32);
    MirGlobal *result = BuildEntry(module, [&](MirBuilder &builder) {
        MirConstant *x[] = {module.GetFloat(f32, 0.5), module.GetFloat(f32, 1.0)};
        MirValue *operands[] = {piecewise, module.GetComposite(pair, x), module.GetFloat(f32, 2.0)};
        const int64 pairs[] = {1};
        MirValue *derivative = builder.Intrinsic(MirIntrinsic::ForwardDerivative, pair, operands, pairs);
        const uint32 index[] = {1};
        return builder.Extract(derivative, index);
    });
    const String error = Differentiate(module);
    MARLIN_REQUIREF(error.IsEmpty(), "{0}", error.AsView());
    const String text = Print(module);
    MARLIN_EXPECTF(text.AsView().IndexOf("ForwardDerivative") == StringView::NotFound, "{0}", text.AsView());
    // x = 0.5 takes the else branch: f = 3x + y = 3.5, and the derivative is 3 (1 + 2f).
    const double expected = 24.0;
    const double stored = StoredConstant(result);
    MARLIN_EXPECTF(__builtin_fabs(stored - expected) < 1e-4, "{0} != {1}\n{2}", stored, expected, text.AsView());
}

MARLIN_TEST(MirAutodiff, ReverseModeWritesTheGradient)
{
    MirModule module;
    MirFunction *piecewise = BuildPiecewise(module);
    const MirType *f32 = module.GetTypes().GetFloat(32);
    const MirType *pair = PairOf(module, f32);
    MirGlobal *result = BuildEntry(module, [&](MirBuilder &builder) {
        MirInstruction *x = builder.Variable(pair, "x");
        MirConstant *initial[] = {module.GetFloat(f32, 2.0), module.GetFloat(f32, 0.0)};
        builder.Store(x, module.GetComposite(pair, initial));
        MirValue *operands[] = {piecewise, x, module.GetFloat(f32, 5.0), module.GetFloat(f32, 1.0)};
        const int64 literals[] = {1, 1};
        builder.Intrinsic(MirIntrinsic::BackwardDerivative, module.GetTypes().GetVoid(), operands, literals);
        const uint32 index[] = {1};
        return builder.Extract(builder.Load(x), index);
    });
    const String error = Differentiate(module);
    MARLIN_REQUIREF(error.IsEmpty(), "{0}", error.AsView());
    const String text = Print(module);
    // x = 2 takes the then branch: f = x * x = 4, and the derivative is 2x (1 + 2f).
    const double expected = 36.0;
    const double stored = StoredConstant(result);
    MARLIN_EXPECTF(__builtin_fabs(stored - expected) < 1e-4, "{0} != {1}\n{2}", stored, expected, text.AsView());
}

MARLIN_TEST(MirAutodiff, VectorCodeOnUnknownValuesVerifies)
{
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *vector = types.GetVector(f32, 3);
    const MirType *matrix = types.GetMatrix(f32, 3, 3);
    const MirType *parameters[] = {vector, matrix};
    MirFunction *shade = module.AddFunction(types.GetFunction(f32, parameters), "Shade");
    {
        MirBlock *entry = shade->AddBlock("entry");
        MirBlock *then = shade->AddBlock("then");
        MirBlock *merge = shade->AddBlock("merge");
        MirValue *v = shade->GetArguments()[0];
        MirValue *m = shade->GetArguments()[1];
        MirBuilder builder(module);
        builder.SetInsertPoint(entry);
        MirValue *transformed = builder.Binary(MirOp::MatrixTimesVector, m, v);
        MirValue *normalizeOperands[] = {transformed};
        MirValue *n = builder.Intrinsic(MirIntrinsic::Normalize, vector, normalizeOperands);
        MirValue *dotOperands[] = {n, v};
        MirValue *lit = builder.Intrinsic(MirIntrinsic::Dot, f32, dotOperands);
        entry->Merge = {MirMergeKind::Selection, merge};
        builder.CondBranch(builder.Compare(MirOp::Less, lit, module.GetFloat(f32, 0.0)), then, merge);
        builder.SetInsertPoint(then);
        MirValue *crossOperands[] = {v, builder.Binary(MirOp::VectorTimesScalar, v, lit)};
        MirValue *lengthOperands[] = {builder.Intrinsic(MirIntrinsic::Cross, vector, crossOperands)};
        MirValue *edge = builder.Intrinsic(MirIntrinsic::Length, f32, lengthOperands);
        builder.Branch(merge);
        builder.SetInsertPoint(merge);
        MirInstruction *phi = builder.Phi(f32);
        phi->AddOperand(lit);
        phi->AddTarget(entry);
        phi->AddOperand(edge);
        phi->AddTarget(then);
        MirValue *clampOperands[] = {phi, module.GetFloat(f32, 0.0), module.GetFloat(f32, 1.0)};
        builder.Return(builder.Intrinsic(MirIntrinsic::Clamp, f32, clampOperands));
    }
    const MirType *vectorPair = PairOf(module, vector);
    const MirType *matrixPair = PairOf(module, matrix);
    MirGlobal *input = module.AddGlobal(vector, MirAddressSpace::Private, "Input");
    MirGlobal *transform = module.AddGlobal(matrix, MirAddressSpace::Private, "Transform");
    BuildEntry(module, [&](MirBuilder &builder) {
        MirValue *v = builder.Load(input);
        MirValue *m = builder.Load(transform);
        MirValue *vParts[] = {v, module.GetSplat(vector, module.GetFloat(f32, 1.0))};
        MirValue *mParts[] = {m, module.GetNull(matrix)};
        MirValue *forward[] = {shade, builder.Construct(vectorPair, vParts), builder.Construct(matrixPair, mParts)};
        const int64 pairs[] = {3};
        const MirType *f32Pair = PairOf(module, f32);
        const uint32 derivative[] = {1};
        MirValue *tangent =
            builder.Extract(builder.Intrinsic(MirIntrinsic::ForwardDerivative, f32Pair, forward, pairs), derivative);
        MirInstruction *vPair = builder.Variable(vectorPair, "v");
        MirInstruction *mPair = builder.Variable(matrixPair, "m");
        builder.Store(vPair, builder.Construct(vectorPair, vParts));
        builder.Store(mPair, builder.Construct(matrixPair, mParts));
        MirValue *backward[] = {shade, vPair, mPair, tangent};
        const int64 literals[] = {3, 3};
        builder.Intrinsic(MirIntrinsic::BackwardDerivative, module.GetTypes().GetVoid(), backward, literals);
        const uint32 x[] = {1, 0};
        return builder.Extract(builder.Load(vPair), x);
    });
    const String error = Differentiate(module);
    MARLIN_EXPECTF(error.IsEmpty(), "{0}\n{1}", error.AsView(), Print(module).AsView());
}

MARLIN_TEST(MirAutodiff, ReportsWhatCannotBeDifferentiated)
{
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *parameters[] = {f32};
    MirFunction *rounded = module.AddFunction(types.GetFunction(f32, parameters), "Bits");
    MirBuilder builder(module);
    builder.SetInsertPoint(rounded->AddBlock("entry"));
    MirValue *operands[] = {rounded->GetArguments()[0], rounded->GetArguments()[0]};
    builder.Return(builder.Intrinsic(MirIntrinsic::Reflect, f32, operands));
    const MirType *pair = PairOf(module, f32);
    BuildEntry(module, [&](MirBuilder &entry) {
        MirConstant *x[] = {module.GetFloat(f32, 1.0), module.GetFloat(f32, 1.0)};
        MirValue *call[] = {rounded, module.GetComposite(pair, x)};
        const int64 pairs[] = {1};
        const uint32 index[] = {1};
        return entry.Extract(entry.Intrinsic(MirIntrinsic::ForwardDerivative, pair, call, pairs), index);
    });
    const String error = Differentiate(module);
    const StringView expected = "cannot differentiate `Bits` (forward mode): Reflect has no derivative rule";
    MARLIN_EXPECTF(error.AsView().Contains(expected), "{0}", error.AsView());
}

MARLIN_TEST(MirAutodiff, ReverseModeKeepsEachIterationOfALoop)
{
    // p = x (x + 1) (x + 2) at x = 2; its derivative is (x + 1)(x + 2) + x (x + 2) + x (x + 1) = 12 + 8 + 6, which
    // reverse mode gets from the factors each iteration kept.
    MirModule module;
    MirFunction *product = BuildProduct(module, 8);
    const MirType *i32 = module.GetTypes().GetInt(32, true);
    const List<MirGlobal *> results = BuildResults(module, [&](MirBuilder &builder, List<MirValue *> &values) {
        MirValue *count[] = {module.GetInt(i32, 3)};
        AddDerivatives(module, builder, product, 2.0, count, values);
    });
    const List<double> values = RunDerivatives(module, results.AsSpan());
    MARLIN_REQUIRE(values.Count() == 2);
    MARLIN_EXPECTF(Near(values[0], 26.0) && Near(values[1], 26.0), "forward {0}, reverse {1}\n{2}", values[0],
                   values[1], Print(module).AsView());
}

MARLIN_TEST(MirAutodiff, ReverseModeRunsNestedAndDoWhileLoopsBackwards)
{
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *i32 = types.GetInt(32, true);
    MirConstant *zero = module.GetInt(i32, 0);
    MirConstant *one = module.GetInt(i32, 1);
    // s = 0; for (i = 0; i < n; i++) { t = x; for (j = 0; j < m; j++) { t = t * x; } s = s + t * (i + 1); }
    // With n = 2 and m = 3, s = 3x^4: the inner loop's trips and t of every inner trip are kept per outer trip.
    const MirType *nestedParameters[] = {f32, i32, i32};
    MirFunction *nested = module.AddFunction(types.GetFunction(f32, nestedParameters), "Nested");
    {
        MirValue *x = nested->GetArguments()[0];
        MirBlock *entry = nested->AddBlock("entry");
        const LoopBlocks outer = AddLoop(nested, 4);
        const LoopBlocks inner = AddLoop(nested, 4);
        MirBuilder builder(module);
        builder.SetInsertPoint(entry);
        builder.Branch(outer.Header);
        builder.SetInsertPoint(outer.Header);
        MirInstruction *i = builder.Phi(i32);
        MirInstruction *s = builder.Phi(f32);
        builder.Branch(outer.Check);
        builder.SetInsertPoint(outer.Check);
        builder.CondBranch(builder.Compare(MirOp::Less, i, nested->GetArguments()[1]), outer.Body, outer.Merge);
        builder.SetInsertPoint(outer.Body);
        builder.Branch(inner.Header);
        builder.SetInsertPoint(inner.Header);
        MirInstruction *j = builder.Phi(i32);
        MirInstruction *t = builder.Phi(f32);
        builder.Branch(inner.Check);
        builder.SetInsertPoint(inner.Check);
        builder.CondBranch(builder.Compare(MirOp::Less, j, nested->GetArguments()[2]), inner.Body, inner.Merge);
        builder.SetInsertPoint(inner.Body);
        MirValue *product = builder.Binary(MirOp::Mul, t, x);
        builder.Branch(inner.Latch);
        builder.SetInsertPoint(inner.Latch);
        MirValue *nextJ = builder.Binary(MirOp::Add, j, one);
        builder.Branch(inner.Header);
        builder.SetInsertPoint(inner.Merge);
        MirValue *weight = builder.Convert(builder.Binary(MirOp::Add, i, one), f32);
        MirValue *sum = builder.Binary(MirOp::Add, s, builder.Binary(MirOp::Mul, t, weight));
        builder.Branch(outer.Latch);
        builder.SetInsertPoint(outer.Latch);
        MirValue *nextI = builder.Binary(MirOp::Add, i, one);
        builder.Branch(outer.Header);
        builder.SetInsertPoint(outer.Merge);
        builder.Return(s);
        Complete(i, zero, entry, nextI, outer.Latch);
        Complete(s, module.GetFloat(f32, 0.0), entry, sum, outer.Latch);
        Complete(j, zero, outer.Body, nextJ, inner.Latch);
        Complete(t, x, outer.Body, product, inner.Latch);
    }
    // p = x; k = 0; do { p = p * x; k++; } while (k < count); with count = 2, p = x^3.
    const MirType *repeatParameters[] = {f32, i32};
    MirFunction *repeat = module.AddFunction(types.GetFunction(f32, repeatParameters), "Repeat");
    {
        MirValue *x = repeat->GetArguments()[0];
        MirBlock *entry = repeat->AddBlock("entry");
        const LoopBlocks loop = AddLoop(repeat, 8, false);
        MirBuilder builder(module);
        builder.SetInsertPoint(entry);
        builder.Branch(loop.Header);
        builder.SetInsertPoint(loop.Header);
        MirInstruction *k = builder.Phi(i32);
        MirInstruction *p = builder.Phi(f32);
        builder.Branch(loop.Body);
        builder.SetInsertPoint(loop.Body);
        MirValue *product = builder.Binary(MirOp::Mul, p, x);
        builder.Branch(loop.Latch);
        builder.SetInsertPoint(loop.Latch);
        MirValue *nextK = builder.Binary(MirOp::Add, k, one);
        builder.CondBranch(builder.Compare(MirOp::Less, nextK, repeat->GetArguments()[1]), loop.Header, loop.Merge);
        builder.SetInsertPoint(loop.Merge);
        builder.Return(product);
        Complete(k, zero, entry, nextK, loop.Latch);
        Complete(p, x, entry, product, loop.Latch);
    }
    const List<MirGlobal *> results = BuildResults(module, [&](MirBuilder &builder, List<MirValue *> &values) {
        MirValue *counts[] = {module.GetInt(i32, 2), module.GetInt(i32, 3)};
        AddDerivatives(module, builder, nested, 2.0, counts, values);
        MirValue *count[] = {module.GetInt(i32, 2)};
        AddDerivatives(module, builder, repeat, 2.0, count, values);
    });
    const List<double> values = RunDerivatives(module, results.AsSpan());
    MARLIN_REQUIRE(values.Count() == 4);
    // d(3x^4) = 12x^3 = 96 and d(x^3) = 3x^2 = 12 at x = 2.
    MARLIN_EXPECTF(Near(values[0], 96.0) && Near(values[1], 96.0) && Near(values[2], 12.0) && Near(values[3], 12.0),
                   "{0} {1} {2} {3}\n{4}", values[0], values[1], values[2], values[3], Print(module).AsView());
}

MARLIN_TEST(MirAutodiff, ReverseModeNeedsABoundOnLoops)
{
    // Forward mode needs no bound; reverse mode keeps values per iteration, so a loop with a trip count known only
    // at run time needs [MaxIterations(n)].
    MirModule forwardModule;
    MirFunction *forward = BuildProduct(forwardModule, 0);
    const MirType *i32 = forwardModule.GetTypes().GetInt(32, true);
    const MirType *f32 = forwardModule.GetTypes().GetFloat(32);
    const List<MirGlobal *> results = BuildResults(forwardModule, [&](MirBuilder &builder, List<MirValue *> &values) {
        MirConstant *x[] = {forwardModule.GetFloat(f32, 2.0), forwardModule.GetFloat(f32, 1.0)};
        MirValue *operands[] = {forward, forwardModule.GetComposite(PairOf(forwardModule, f32), x),
                                forwardModule.GetInt(i32, 3)};
        const int64 pairs[] = {1};
        const uint32 differential[] = {1};
        values.Add(builder.Extract(
            builder.Intrinsic(MirIntrinsic::ForwardDerivative, PairOf(forwardModule, f32), operands, pairs),
            differential));
    });
    const List<double> values = RunDerivatives(forwardModule, results.AsSpan());
    MARLIN_EXPECT(values.Count() == 1 && Near(values[0], 26.0));

    MirModule module;
    MirFunction *product = BuildProduct(module, 0);
    BuildResults(module, [&](MirBuilder &builder, List<MirValue *> &values) {
        MirValue *count[] = {module.GetInt(module.GetTypes().GetInt(32, true), 3)};
        AddDerivatives(module, builder, product, 2.0, count, values);
    });
    const String error = Differentiate(module);
    MARLIN_EXPECTF(error.AsView().Contains("cannot differentiate `Product` (reverse mode): the loop needs "
                                           "`[MaxIterations(n)]`"),
                   "{0}", error.AsView());
}

MARLIN_TEST(MirAutodiff, DetachStopsTheDerivative)
{
    // x * Detach(x * x): the derivative is x * x = 4 at x = 2, not 3x^2.
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *parameters[] = {f32};
    MirFunction *function = module.AddFunction(types.GetFunction(f32, parameters), "Detached");
    MirBuilder builder(module);
    builder.SetInsertPoint(function->AddBlock("entry"));
    MirValue *x = function->GetArguments()[0];
    MirValue *square[] = {builder.Binary(MirOp::Mul, x, x)};
    builder.Return(builder.Binary(MirOp::Mul, x, builder.Intrinsic(MirIntrinsic::Detach, f32, square)));
    const List<MirGlobal *> results = BuildResults(module, [&](MirBuilder &entry, List<MirValue *> &values) {
        AddDerivatives(module, entry, function, 2.0, {}, values);
    });
    const List<double> values = RunDerivatives(module, results.AsSpan());
    MARLIN_REQUIRE(values.Count() == 2);
    MARLIN_EXPECTF(Near(values[0], 4.0) && Near(values[1], 4.0), "{0} {1}", values[0], values[1]);
    MARLIN_EXPECT(!Print(module).AsView().Contains("Detach"));
}

MARLIN_TEST(MirAutodiff, CustomDerivativesReplaceTheCallee)
{
    // Square(x) is x * x, but its custom derivatives say 10 x': differentiating 3 * Square(x) calls them instead of
    // differentiating the body, in both modes.
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *pair = PairOf(module, f32);
    const uint32 primal[] = {0};
    const uint32 differential[] = {1};
    MirBuilder builder(module);
    const MirType *squareParameters[] = {f32};
    MirFunction *square = module.AddFunction(types.GetFunction(f32, squareParameters), "Square");
    builder.SetInsertPoint(square->AddBlock("entry"));
    builder.Return(builder.Binary(MirOp::Mul, square->GetArguments()[0], square->GetArguments()[0]));
    const MirType *forwardParameters[] = {pair};
    MirFunction *forward = module.AddFunction(types.GetFunction(pair, forwardParameters), "SquareForward");
    builder.SetInsertPoint(forward->AddBlock("entry"));
    {
        MirValue *x = forward->GetArguments()[0];
        MirValue *arguments[] = {builder.Extract(x, primal)};
        MirValue *parts[] = {builder.Call(square, arguments),
                             builder.Binary(MirOp::Mul, module.GetFloat(f32, 10.0), builder.Extract(x, differential))};
        builder.Return(builder.Construct(pair, parts));
    }
    const MirType *backwardParameters[] = {types.GetPointer(pair, MirAddressSpace::Function), f32};
    MirFunction *backward =
        module.AddFunction(types.GetFunction(types.GetVoid(), backwardParameters), "SquareBackward");
    builder.SetInsertPoint(backward->AddBlock("entry"));
    {
        MirValue *x = backward->GetArguments()[0];
        MirValue *gradient = builder.Binary(MirOp::Mul, module.GetFloat(f32, 10.0), backward->GetArguments()[1]);
        builder.Store(x, builder.Insert(builder.Load(x), gradient, differential));
        builder.Return();
    }
    square->ForwardDerivative = forward;
    square->BackwardDerivative = backward;
    square->DerivativeArguments = 1;
    MirFunction *scaled = module.AddFunction(types.GetFunction(f32, squareParameters), "Scaled");
    builder.SetInsertPoint(scaled->AddBlock("entry"));
    MirValue *arguments[] = {scaled->GetArguments()[0]};
    builder.Return(builder.Binary(MirOp::Mul, module.GetFloat(f32, 3.0), builder.Call(square, arguments)));
    const List<MirGlobal *> results = BuildResults(module, [&](MirBuilder &entry, List<MirValue *> &values) {
        AddDerivatives(module, entry, scaled, 2.0, {}, values);
    });
    const List<double> values = RunDerivatives(module, results.AsSpan());
    MARLIN_REQUIRE(values.Count() == 2);
    MARLIN_EXPECTF(Near(values[0], 30.0) && Near(values[1], 30.0), "{0} {1}\n{2}", values[0], values[1],
                   Print(module).AsView());
}

MARLIN_TEST(MirAutodiff, ReportsAMissingCustomDerivative)
{
    // A callee with only a forward derivative cannot be differentiated in reverse mode.
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *pair = PairOf(module, f32);
    MirBuilder builder(module);
    const MirType *parameters[] = {f32};
    MirFunction *callee = module.AddFunction(types.GetFunction(f32, parameters), "Opaque");
    builder.SetInsertPoint(callee->AddBlock("entry"));
    builder.Return(callee->GetArguments()[0]);
    const MirType *forwardParameters[] = {pair};
    MirFunction *forward = module.AddFunction(types.GetFunction(pair, forwardParameters), "OpaqueForward");
    builder.SetInsertPoint(forward->AddBlock("entry"));
    builder.Return(forward->GetArguments()[0]);
    callee->ForwardDerivative = forward;
    callee->DerivativeArguments = 1;
    MirFunction *caller = module.AddFunction(types.GetFunction(f32, parameters), "Caller");
    builder.SetInsertPoint(caller->AddBlock("entry"));
    MirValue *arguments[] = {caller->GetArguments()[0]};
    builder.Return(builder.Call(callee, arguments));
    BuildResults(module, [&](MirBuilder &entry, List<MirValue *> &values) {
        AddDerivatives(module, entry, caller, 2.0, {}, values);
    });
    const String error = Differentiate(module);
    MARLIN_EXPECTF(
        error.AsView().Contains(
            "cannot differentiate `Caller` (reverse mode): `Opaque` has a custom derivative only for forward "
            "mode"),
        "{0}", error.AsView());
}

MARLIN_TEST(MirAutodiff, NoDiffFieldsHaveNoDerivative)
{
    // s.a * s.b + s.b with s.b [NoDiff]: the derivative along {1, 1} is s.b = 5, and the gradient is {5, 0}.
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    MirStructField fields[] = {{"A", f32, MirTypeContext::AutoOffset}, {"B", f32, MirTypeContext::AutoOffset, true}};
    const MirType *weighted = types.CreateStruct("Weighted", fields);
    const MirType *pair = PairOf(module, weighted);
    const MirType *parameters[] = {weighted};
    MirFunction *function = module.AddFunction(types.GetFunction(f32, parameters), "Weigh");
    MirBuilder builder(module);
    builder.SetInsertPoint(function->AddBlock("entry"));
    const uint32 a[] = {0};
    const uint32 b[] = {1};
    MirValue *s = function->GetArguments()[0];
    MirValue *weight = builder.Extract(s, b);
    builder.Return(builder.Binary(MirOp::Add, builder.Binary(MirOp::Mul, builder.Extract(s, a), weight), weight));
    const List<MirGlobal *> results = BuildResults(module, [&](MirBuilder &entry, List<MirValue *> &values) {
        MirConstant *value[] = {module.GetFloat(f32, 2.0), module.GetFloat(f32, 5.0)};
        MirConstant *direction[] = {module.GetFloat(f32, 1.0), module.GetFloat(f32, 1.0)};
        MirConstant *forwardPair[] = {module.GetComposite(weighted, value), module.GetComposite(weighted, direction)};
        MirValue *forward[] = {function, module.GetComposite(pair, forwardPair)};
        const int64 pairs[] = {1};
        const uint32 differential[] = {1};
        values.Add(entry.Extract(entry.Intrinsic(MirIntrinsic::ForwardDerivative, PairOf(module, f32), forward, pairs),
                                 differential));
        MirInstruction *variable = entry.Variable(pair, "s");
        MirConstant *backwardPair[] = {module.GetComposite(weighted, value), module.GetComposite(weighted, direction)};
        entry.Store(variable, module.GetComposite(pair, backwardPair));
        MirValue *backward[] = {function, variable, module.GetFloat(f32, 1.0)};
        const int64 literals[] = {1, 1};
        entry.Intrinsic(MirIntrinsic::BackwardDerivative, types.GetVoid(), backward, literals);
        const uint32 gradientA[] = {1, 0};
        const uint32 gradientB[] = {1, 1};
        values.Add(entry.Extract(entry.Load(variable), gradientA));
        values.Add(entry.Extract(entry.Load(variable), gradientB));
    });
    const List<double> values = RunDerivatives(module, results.AsSpan());
    MARLIN_REQUIRE(values.Count() == 3);
    MARLIN_EXPECTF(Near(values[0], 5.0) && Near(values[1], 5.0) && Near(values[2], 0.0), "{0} {1} {2}\n{3}",
                   values[0], values[1], values[2], Print(module).AsView());
}

MARLIN_TEST(MirAutodiff, ReverseModeRejectsBreakingOutOfLoops)
{
    // for (…) { if (p > 10) break; p = p * x; }: the loop leaves other than through its condition.
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *i32 = types.GetInt(32, true);
    const MirType *parameters[] = {f32};
    MirFunction *function = module.AddFunction(types.GetFunction(f32, parameters), "Breaks");
    MirBlock *entry = function->AddBlock("entry");
    const LoopBlocks loop = AddLoop(function, 8);
    MirBlock *leave = function->AddBlock("leave");
    MirBlock *stay = function->AddBlock("stay");
    MirValue *x = function->GetArguments()[0];
    MirBuilder builder(module);
    builder.SetInsertPoint(entry);
    builder.Branch(loop.Header);
    builder.SetInsertPoint(loop.Header);
    MirInstruction *i = builder.Phi(i32);
    MirInstruction *p = builder.Phi(f32);
    builder.Branch(loop.Check);
    builder.SetInsertPoint(loop.Check);
    builder.CondBranch(builder.Compare(MirOp::Less, i, module.GetInt(i32, 4)), loop.Body, loop.Merge);
    builder.SetInsertPoint(loop.Body);
    loop.Body->Merge = {MirMergeKind::Selection, stay};
    builder.CondBranch(builder.Compare(MirOp::Greater, p, module.GetFloat(f32, 10.0)), leave, stay);
    builder.SetInsertPoint(leave);
    builder.Branch(loop.Merge);
    builder.SetInsertPoint(stay);
    MirValue *next = builder.Binary(MirOp::Mul, p, x);
    builder.Branch(loop.Latch);
    builder.SetInsertPoint(loop.Latch);
    MirValue *step = builder.Binary(MirOp::Add, i, module.GetInt(i32, 1));
    builder.Branch(loop.Header);
    builder.SetInsertPoint(loop.Merge);
    MirInstruction *result = builder.Phi(f32);
    result->AddOperand(p);
    result->AddTarget(loop.Check);
    result->AddOperand(p);
    result->AddTarget(leave);
    builder.Return(result);
    Complete(i, module.GetInt(i32, 0), entry, step, loop.Latch);
    Complete(p, x, entry, next, loop.Latch);
    BuildResults(module, [&](MirBuilder &main, List<MirValue *> &values) {
        AddDerivatives(module, main, function, 2.0, {}, values);
    });
    const String error = Differentiate(module);
    MARLIN_EXPECTF(
        error.AsView().Contains("cannot differentiate `Breaks` (reverse mode): it leaves a loop with break or "
                                "continue"),
        "{0}", error.AsView());
}
} // namespace Marlin::Tests
