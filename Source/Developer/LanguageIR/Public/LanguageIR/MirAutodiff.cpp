#include "Containers/HashSet.h"
#include "LanguageIR/MirAnalysis.h"
#include "LanguageIR/MirBuilder.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirPrinter.h"
#include "Math/MathUtility.h"
#include "Memory/UniquePtr.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
/// Whether values of *type* carry a derivative: floating-point scalars, vectors and matrices, and arrays and structs
/// that hold one outside `[NoDiff]` fields. The derivative of such a value has the same type (the Differential of
/// every differentiable type is the type itself, §9.12); its other parts stay zero.
bool HasDerivative(const MirType *type)
{
    switch (type->GetKind())
    {
    case MirTypeKind::Float:
        return true;
    case MirTypeKind::Vector:
    case MirTypeKind::Matrix:
        return type->GetScalar()->Is(MirTypeKind::Float);
    case MirTypeKind::Array:
        return HasDerivative(type->GetElement());
    case MirTypeKind::Struct:
        for (const MirStructField &field : type->GetFields())
        {
            if (!field.NoDiff && HasDerivative(field.Type))
            {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

/// Whether *type* has a `[NoDiff]` field anywhere in it.
bool HasNoDiffField(const MirType *type)
{
    switch (type->GetKind())
    {
    case MirTypeKind::Array:
        return HasNoDiffField(type->GetElement());
    case MirTypeKind::Struct:
        for (const MirStructField &field : type->GetFields())
        {
            if (field.NoDiff || HasNoDiffField(field.Type))
            {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

/// Whether the part of a *type* at *indices* lies in a `[NoDiff]` field, whose derivative is zero.
bool IsNoDiffPath(const MirType *type, Span<const int64> indices)
{
    for (const int64 index : indices)
    {
        if (!type->Is(MirTypeKind::Struct))
        {
            type = type->GetElement();
            continue;
        }
        const MirStructField &field = type->GetFields()[static_cast<usize>(index)];
        if (field.NoDiff)
        {
            return true;
        }
        type = field.Type;
    }
    return false;
}

/// Whether the derivative of operand *index* of *instruction* reaches its result: not when the operand goes into, or
/// the result comes out of, a `[NoDiff]` field.
bool Carries(const MirInstruction &instruction, usize index)
{
    switch (instruction.GetOp())
    {
    case MirOp::CompositeExtract:
        return !IsNoDiffPath(instruction.GetOperand(0)->GetType(), instruction.GetLiterals());
    case MirOp::CompositeInsert:
        return index == 0 || !IsNoDiffPath(instruction.GetOperand(0)->GetType(), instruction.GetLiterals());
    case MirOp::CompositeConstruct:
        return !instruction.GetType()->Is(MirTypeKind::Struct) || !instruction.GetType()->GetFields()[index].NoDiff;
    default:
        return true;
    }
}

bool IsDetach(const MirInstruction &instruction)
{
    return instruction.GetOp() == MirOp::Intrinsic && instruction.GetIntrinsic() == MirIntrinsic::Detach;
}

MirFunction *CalleeOf(const MirInstruction &call)
{
    return static_cast<MirFunction *>(call.GetOperand(0));
}

/// The value *phi* takes when control comes from *block*; null when it does not come from there.
MirValue *PhiIncoming(const MirInstruction *phi, const MirBlock *block)
{
    for (usize i = 0; i < phi->GetTargets().Count(); ++i)
    {
        if (phi->GetTargets()[i] == block)
        {
            return phi->GetOperand(i);
        }
    }
    return nullptr;
}

/// How the derivative of a result depends linearly on the derivative *d* of one operand.
enum class TermKind : uint8
{
    Identity,
    Negate,
    /// Factor * d, componentwise; Factor has the type of d.
    Scale,
    /// d * Factor, where d is a vector or matrix and Factor a scalar.
    ByScalar,
    /// Factor * d, where Factor is a vector or matrix and d a scalar.
    OfScalar,
    /// Op(Factor, d) or Op(d, Factor) for a matrix product, or the cross product when Op is Intrinsic.
    Product,
    Dot,
    Convert,
    Transpose,
    /// The derivative of normalize: (d - Factor * dot(Factor, d)) / Extra, with Factor the result and Extra the
    /// length of the operand.
    Normalize,
};

/// One linear term of a derivative rule: the derivative of the result is the sum of the terms whose operand has a
/// derivative.
struct Term
{
    uint32 Operand = 0;
    TermKind Kind = TermKind::Identity;
    MirValue *Factor = nullptr;
    MirValue *Extra = nullptr;
    MirOp Op = MirOp::Mul;
    bool FactorFirst = false;
};

/// Generates the derivative of one function (MarlinLanguage.md §9.12). *pairs* marks the parameters given a
/// DifferentialPair {Primal, Differential}, *gradients* those of them passed by `ref` to receive a gradient. Forward
/// mode returns the pair of the result; reverse mode replays the primal, then accumulates adjoints over the
/// structured regions from dResult, its last argument, back to the `ref` pairs. Forward mode may carry *tangents*
/// directions at once (two for DifferentialPair2 {Primal, Dx, Dy}): the derivative rules are linear in the tangent, so
/// each rule's factors are built once from the primal values and applied to every direction.
///
/// A call left in the source is to a function with a custom derivative, which is called instead of differentiating
/// the callee. A loop in reverse mode is swept backwards trip by trip: the primal code counts the trips, keeps the
/// values the sweep needs in arrays with a slot per trip (sized by `[MaxIterations(n)]` or a constant trip count),
/// and a reverse loop counts back down to the first trip.
class Differentiator
{
public:
    Differentiator(MirModule &module, MirFunction &source, uint32 pairs, uint32 gradients, bool forward,
                   uint32 tangents = 1)
        : _module(&module), _types(&module.GetTypes()), _builder(module), _source(&source), _pairs(pairs),
          _gradients(gradients), _forward(forward), _tangentCount(forward ? Math::Clamp(tangents, 1u, MaxTangents) : 1)
    {
    }

    MirFunction *Run(Span<MirValue *const> callArguments, const MirType *resultType)
    {
        if (!Check() || !CheckArguments(callArguments, resultType))
        {
            return nullptr;
        }
        List<const MirType *> parameters;
        for (MirValue *argument : callArguments)
        {
            parameters.Add(argument->GetType());
        }
        _result = _module->AddFunction(_types->GetFunction(resultType, parameters.AsSpan()), _source->GetName());
        _result->Location = _source->Location;
        for (const MirBlock *block : _source->GetBlocks())
        {
            _blocks.Add(block, _result->AddBlock(block->GetName()));
        }
        FindVaried();
        Prologue();
        for (const MirBlock *block : _source->GetBlocks())
        {
            CloneBlock(*block);
        }
        CompletePhis();
        if (!_failed && !_forward)
        {
            Reverse();
        }
        if (_failed)
        {
            _module->RemoveFunction(_result);
            return nullptr;
        }
        return _result;
    }

private:
    void Fail(const MirInstruction *at, StringView problem)
    {
        const MirLocation location = at != nullptr ? at->Location : _source->Location;
        const Span<const StringView> files = _module->GetFiles();
        const StringView file =
            location.File != 0 && location.File <= files.Count() ? files[location.File - 1] : _source->GetName();
        if (!_failed)
        {
            _module->ReportError(Format("{0}:{1}:{2}: cannot differentiate `{3}` ({4} mode): {5}", file, location.Line,
                                        location.Column, _source->GetName(),
                                        StringView(_forward ? "forward" : "reverse"), problem));
        }
        _failed = true;
    }

    /// Whether the source is one function of structured selections and loops, calling only functions with a custom
    /// derivative for the mode, as the derivative rules need; earlier passes inline, unroll and promote to registers
    /// to get there. Reports why not.
    bool Check()
    {
        MARLIN_ASSERT(_source->GetEntry() != nullptr);
        _flow = MakeUnique<MirControlFlow>(*_source);
        usize returns = 0;
        for (const MirBlock *block : _source->GetBlocks())
        {
            for (const MirInstruction *instruction : *block)
            {
                switch (instruction->GetOp())
                {
                case MirOp::Return:
                    ++returns;
                    break;
                case MirOp::Switch:
                    Fail(instruction, "switch statements are not supported; use if");
                    return false;
                case MirOp::Call:
                {
                    const MirFunction *callee = CalleeOf(*instruction);
                    if (DerivativeOf(*callee) != nullptr)
                    {
                        break;
                    }
                    if (callee->ForwardDerivative != nullptr || callee->ForwardDerivative2 != nullptr ||
                        callee->BackwardDerivative != nullptr)
                    {
                        Fail(instruction, Format("`{0}` has a custom derivative only for {1} mode", callee->GetName(),
                                                 StringView(_forward ? "reverse" : "forward"))
                                              .AsView());
                        return false;
                    }
                    Fail(instruction, "it still calls a function that could not be inlined");
                    return false;
                }
                case MirOp::CallVirtual:
                case MirOp::CallInterface:
                case MirOp::CallDelegate:
                    Fail(instruction, "it still calls a function that could not be inlined");
                    return false;
                case MirOp::Variable:
                    Fail(instruction, "it indexes local memory; index with constants so the value stays in registers");
                    return false;
                default:
                    break;
                }
            }
        }
        if (returns != 1)
        {
            Fail(nullptr, "it returns early; return once at the end");
            return false;
        }
        if (!_forward)
        {
            Parse(_source->GetEntry(), nullptr, 0, nullptr, _items);
        }
        return !_failed;
    }

    /// The custom derivative of *function* for the mode; null when it has none.
    [[nodiscard]] MirFunction *DerivativeOf(const MirFunction &function) const
    {
        if (!_forward)
        {
            return function.BackwardDerivative;
        }
        // Several directions call the one-direction derivative once per direction when there is no other.
        return _tangentCount > 1 && function.ForwardDerivative2 != nullptr ? function.ForwardDerivative2
                                                                          : function.ForwardDerivative;
    }

    /// Whether the call is shaped as the frontend binds it: the arguments in parameter order, a pair (or a pointer to
    /// one for a `ref` pair) for each active parameter, then dResult for Backward. The frontend has checked this
    /// already, so a mismatch is a compiler bug.
    bool CheckArguments(Span<MirValue *const> arguments, const MirType *resultType)
    {
        const Span<MirArgument *const> parameters = _source->GetArguments();
        bool fits = arguments.Count() == parameters.Count() + (_forward ? 0 : 1) && (!_forward || _gradients == 0);
        for (usize i = 0; fits && i < parameters.Count(); ++i)
        {
            const MirType *type = arguments[i]->GetType();
            if (IsActive(static_cast<uint32>(i)))
            {
                const MirType *pair = IsRef(static_cast<uint32>(i))
                                          ? (type->Is(MirTypeKind::Pointer) ? type->GetElement() : nullptr)
                                          : type;
                fits = pair != nullptr && IsPairOf(pair, parameters[i]->GetType());
            }
            else
            {
                fits = type == parameters[i]->GetType();
            }
        }
        const MirType *returnType = _source->GetReturnType();
        fits = fits && (_forward ? IsPairOf(resultType, returnType)
                                 : resultType->Is(MirTypeKind::Void) && arguments.Last()->GetType() == returnType);
        MARLIN_ASSERT(fits);
        return fits;
    }

    /// Whether *pair* is the pair of *type* for the mode: the primal, then a derivative per direction.
    [[nodiscard]] bool IsPairOf(const MirType *pair, const MirType *type) const
    {
        const uint32 fields = _forward ? 1 + _tangentCount : 2;
        if (!pair->Is(MirTypeKind::Struct) || pair->GetFields().Count() != fields)
        {
            return false;
        }
        for (const MirStructField &field : pair->GetFields())
        {
            if (field.Type != type)
            {
                return false;
            }
        }
        return true;
    }

    /// Whether active parameter *parameter* is a `ref` pair, which receives its gradient in reverse mode.
    [[nodiscard]] bool IsRef(uint32 parameter) const
    {
        return !_forward && parameter < 32 && (_gradients & (1u << parameter)) != 0;
    }

    [[nodiscard]] bool IsActive(uint32 parameter) const
    {
        return parameter < 32 && (_pairs & (1u << parameter)) != 0 &&
               HasDerivative(_source->GetArguments()[parameter]->GetType());
    }

    /// Marks the values that depend on an active parameter through operations that carry derivatives. `Detach`
    /// stops the dependence; a loop's phis depend on what its back edge brings, so this repeats until nothing changes.
    void FindVaried()
    {
        for (const MirArgument *argument : _source->GetArguments())
        {
            if (IsActive(argument->GetIndex()))
            {
                _varied.Add(argument);
            }
        }
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (const MirBlock *block : _flow->GetReversePostOrder())
            {
                for (const MirInstruction *instruction : *block)
                {
                    if (_varied.Contains(instruction) || !HasDerivative(instruction->GetType()) ||
                        instruction->GetOp() == MirOp::Load || instruction->GetOp() == MirOp::Bitcast ||
                        IsDetach(*instruction))
                    {
                        continue;
                    }
                    for (usize i = 0; i < instruction->GetOperands().Count(); ++i)
                    {
                        if (_varied.Contains(instruction->GetOperand(i)) && Carries(*instruction, i))
                        {
                            _varied.Add(instruction);
                            changed = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    MirValue *Primal(MirValue *value)
    {
        MirValue **found = _primal.Find(value);
        return found != nullptr ? *found : value;
    }

    /// The tangent of *value* along the current direction in forward mode; null when it is zero.
    MirValue *Tangent(MirValue *value)
    {
        MirValue **found = _tangents[_direction].Find(value);
        return found != nullptr ? *found : nullptr;
    }

    void SetTangent(const MirValue *value, MirValue *tangent)
    {
        _tangents[_direction].Add(value, tangent);
    }

    /// The tangent of *value* along the current direction, zero spelled out.
    MirValue *TangentOrZero(MirValue *value)
    {
        MirValue *tangent = Tangent(value);
        return tangent != nullptr ? tangent : static_cast<MirValue *>(Splat(value->GetType(), 0.0));
    }

    /// A constant of *type* whose floating-point components are all *value*; its other components are zero.
    MirConstant *Splat(const MirType *type, double value)
    {
        switch (type->GetKind())
        {
        case MirTypeKind::Float:
            return _module->GetFloat(type, value);
        case MirTypeKind::Int:
        case MirTypeKind::Bool:
            return _module->GetInt(type, 0);
        case MirTypeKind::Vector:
        case MirTypeKind::Matrix:
        case MirTypeKind::Array:
            return _module->GetSplat(type, Splat(type->GetElement(), value));
        default:
            return _module->GetNull(type);
        }
    }

    /// 1 where *condition* holds and 0 elsewhere, as a *type*.
    MirValue *Mask(MirValue *condition, const MirType *type)
    {
        return _builder.Select(condition, Splat(type, 1.0), Splat(type, 0.0));
    }

    MirValue *Intrinsic(MirIntrinsic intrinsic, const MirType *type, Span<MirValue *const> operands)
    {
        return _builder.Intrinsic(intrinsic, type, operands);
    }

    MirValue *Unary(MirIntrinsic intrinsic, MirValue *x)
    {
        MirValue *operands[] = {x};
        return Intrinsic(intrinsic, x->GetType(), operands);
    }

    // Arithmetic on derivatives, where null stands for zero.

    MirValue *Add(MirValue *a, MirValue *b)
    {
        if (a == nullptr || b == nullptr)
        {
            return a == nullptr ? b : a;
        }
        const MirType *type = a->GetType();
        if (!type->Is(MirTypeKind::Struct) && !type->Is(MirTypeKind::Array))
        {
            return _builder.Binary(MirOp::Add, a, b);
        }
        // Field by field and element by element; the parts without derivatives are zero in both.
        List<MirValue *> parts;
        const uint32 count = type->Is(MirTypeKind::Struct) ? static_cast<uint32>(type->GetFields().Count())
                                                           : type->GetCount();
        for (uint32 i = 0; i < count; ++i)
        {
            const bool isStruct = type->Is(MirTypeKind::Struct);
            const MirType *partType = isStruct ? type->GetFields()[i].Type : type->GetElement();
            if ((isStruct && type->GetFields()[i].NoDiff) || !HasDerivative(partType))
            {
                parts.Add(Splat(partType, 0.0));
                continue;
            }
            const uint32 index[] = {i};
            parts.Add(Add(_builder.Extract(a, index), _builder.Extract(b, index)));
        }
        return _builder.Construct(type, parts.AsSpan());
    }

    MirValue *Mul(MirValue *a, MirValue *b)
    {
        return a == nullptr || b == nullptr ? nullptr : _builder.Binary(MirOp::Mul, a, b);
    }

    MirValue *Div(MirValue *a, MirValue *b)
    {
        return a == nullptr ? nullptr : _builder.Binary(MirOp::Div, a, b);
    }

    MirValue *Sub(MirValue *a, MirValue *b)
    {
        return b == nullptr ? a : a == nullptr ? _builder.Unary(MirOp::Negate, b) : _builder.Binary(MirOp::Sub, a, b);
    }

    /// *value*, a scalar, vector or matrix, times the scalar *scalar*.
    MirValue *Scaled(MirValue *value, MirValue *scalar)
    {
        if (value == nullptr || scalar == nullptr)
        {
            return nullptr;
        }
        const MirType *type = value->GetType();
        return type->Is(MirTypeKind::Vector)   ? _builder.Binary(MirOp::VectorTimesScalar, value, scalar)
               : type->Is(MirTypeKind::Matrix) ? _builder.Binary(MirOp::MatrixTimesScalar, value, scalar)
                                               : _builder.Binary(MirOp::Mul, value, scalar);
    }

    MirValue *DotOf(MirValue *a, MirValue *b)
    {
        if (a == nullptr || b == nullptr)
        {
            return nullptr;
        }
        MirValue *operands[] = {a, b};
        return a->GetType()->Is(MirTypeKind::Vector) ? Intrinsic(MirIntrinsic::Dot, a->GetType()->GetScalar(), operands)
                                                     : _builder.Binary(MirOp::Mul, a, b);
    }

    /// The sum of the componentwise product of *factor* and *value*: a scalar.
    MirValue *Sum(MirValue *factor, MirValue *value)
    {
        if (!factor->GetType()->Is(MirTypeKind::Matrix))
        {
            return DotOf(factor, value);
        }
        MirValue *sum = nullptr;
        for (uint32 row = 0; row < factor->GetType()->GetRows(); ++row)
        {
            const uint32 index[] = {row};
            sum = Add(sum, DotOf(_builder.Extract(factor, index), _builder.Extract(value, index)));
        }
        return sum;
    }

    MirValue *TransposeOf(MirValue *matrix)
    {
        const MirType *type = matrix->GetType();
        MirValue *operands[] = {matrix};
        return Intrinsic(MirIntrinsic::Transpose,
                         _types->GetMatrix(type->GetScalar(), type->GetCount(), type->GetRows()), operands);
    }

    MirValue *Cross(MirValue *a, MirValue *b)
    {
        MirValue *operands[] = {a, b};
        return Intrinsic(MirIntrinsic::Cross, a->GetType(), operands);
    }

    MirValue *NormalizeTerm(const Term &term, MirValue *value)
    {
        MirValue *along = Scaled(term.Factor, DotOf(term.Factor, value));
        return Scaled(Sub(value, along), Div(Splat(term.Extra->GetType(), 1.0), term.Extra));
    }

    /// The contribution of *term* to the derivative of the result, given the derivative *d* of its operand.
    MirValue *Apply(const Term &term, MirValue *d, const MirType *resultType)
    {
        switch (term.Kind)
        {
        case TermKind::Identity:
            return d;
        case TermKind::Negate:
            return _builder.Unary(MirOp::Negate, d);
        case TermKind::Scale:
            return Mul(term.Factor, d);
        case TermKind::ByScalar:
            return Scaled(d, term.Factor);
        case TermKind::OfScalar:
            return Scaled(term.Factor, d);
        case TermKind::Product:
            if (term.Op == MirOp::Intrinsic)
            {
                return term.FactorFirst ? Cross(term.Factor, d) : Cross(d, term.Factor);
            }
            return term.FactorFirst ? _builder.Binary(term.Op, term.Factor, d)
                                    : _builder.Binary(term.Op, d, term.Factor);
        case TermKind::Dot:
            return DotOf(term.Factor, d);
        case TermKind::Convert:
            return _builder.Convert(d, resultType);
        case TermKind::Transpose:
            return TransposeOf(d);
        case TermKind::Normalize:
            return NormalizeTerm(term, d);
        }
        return nullptr;
    }

    /// The contribution of *term* to the adjoint of its operand, the primal *operand*, given the adjoint *a* of the
    /// result: the transpose of Apply.
    MirValue *Transpose(const Term &term, MirValue *a, MirValue *operand)
    {
        const MirType *operandType = operand->GetType();
        switch (term.Kind)
        {
        case TermKind::Identity:
            return a;
        case TermKind::Negate:
            return _builder.Unary(MirOp::Negate, a);
        case TermKind::Scale:
            return Mul(term.Factor, a);
        case TermKind::ByScalar:
            return Scaled(a, term.Factor);
        case TermKind::OfScalar:
            return Sum(term.Factor, a);
        case TermKind::Product:
        {
            switch (term.Op)
            {
            case MirOp::Intrinsic:
                return term.FactorFirst ? Cross(a, term.Factor) : Cross(term.Factor, a);
            case MirOp::MatrixTimesVector:
                if (term.FactorFirst)
                {
                    return _builder.Binary(MirOp::VectorTimesMatrix, a, term.Factor);
                }
                break;
            case MirOp::VectorTimesMatrix:
                if (!term.FactorFirst)
                {
                    return _builder.Binary(MirOp::MatrixTimesVector, term.Factor, a);
                }
                break;
            case MirOp::MatrixTimesMatrix:
                return term.FactorFirst ? _builder.Binary(MirOp::MatrixTimesMatrix, TransposeOf(term.Factor), a)
                                        : _builder.Binary(MirOp::MatrixTimesMatrix, a, TransposeOf(term.Factor));
            default:
                return nullptr;
            }
            // The matrix operand of a matrix-vector product gets an outer product: row i is a vector scaled by
            // component i of the other.
            MirValue *components = term.Op == MirOp::MatrixTimesVector ? a : term.Factor;
            MirValue *vector = term.Op == MirOp::MatrixTimesVector ? term.Factor : a;
            List<MirValue *> rows;
            for (uint32 row = 0; row < operandType->GetRows(); ++row)
            {
                const uint32 index[] = {row};
                rows.Add(Scaled(vector, _builder.Extract(components, index)));
            }
            return _builder.Construct(operandType, rows.AsSpan());
        }
        case TermKind::Dot:
            return Scaled(term.Factor, a);
        case TermKind::Convert:
            return _builder.Convert(a, operandType);
        case TermKind::Transpose:
            return TransposeOf(a);
        case TermKind::Normalize:
            return NormalizeTerm(term, a);
        }
        return nullptr;
    }

    /// The derivative rule of *source* as terms, built from its primal operands *x* and result *y* at the insertion
    /// point; false, after reporting it, when the operation has none.
    bool Terms(const MirInstruction &source, Span<MirValue *const> x, MirValue *y, List<Term> &terms)
    {
        const MirType *type = y->GetType();
        auto add = [&](uint32 operand, TermKind kind, MirValue *factor = nullptr, MirValue *extra = nullptr) {
            Term term;
            term.Operand = operand;
            term.Kind = kind;
            term.Factor = factor;
            term.Extra = extra;
            terms.Add(term);
        };
        auto product = [&](uint32 operand, MirOp op, MirValue *factor, bool factorFirst) {
            Term term;
            term.Operand = operand;
            term.Kind = TermKind::Product;
            term.Factor = factor;
            term.Op = op;
            term.FactorFirst = factorFirst;
            terms.Add(term);
        };
        MirValue *one = Splat(type, 1.0);
        auto negate = [&](MirValue *value) { return _builder.Unary(MirOp::Negate, value); };
        auto compare = [&](MirOp op, MirValue *a, MirValue *b) { return _builder.Compare(op, a, b); };
        constexpr double Ln2 = 0.69314718055994530942;
        switch (source.GetOp())
        {
        case MirOp::Add:
            add(0, TermKind::Identity);
            add(1, TermKind::Identity);
            return true;
        case MirOp::Sub:
            add(0, TermKind::Identity);
            add(1, TermKind::Negate);
            return true;
        case MirOp::Mul:
            add(0, TermKind::Scale, x[1]);
            add(1, TermKind::Scale, x[0]);
            return true;
        case MirOp::Div:
        {
            MirValue *reciprocal = Div(one, x[1]);
            add(0, TermKind::Scale, reciprocal);
            add(1, TermKind::Scale, negate(Mul(y, reciprocal)));
            return true;
        }
        case MirOp::Negate:
            add(0, TermKind::Negate);
            return true;
        case MirOp::MatrixTimesVector:
        case MirOp::VectorTimesMatrix:
        case MirOp::MatrixTimesMatrix:
            product(0, source.GetOp(), x[1], false);
            product(1, source.GetOp(), x[0], true);
            return true;
        case MirOp::MatrixTimesScalar:
        case MirOp::VectorTimesScalar:
            add(0, TermKind::ByScalar, x[1]);
            add(1, TermKind::OfScalar, x[0]);
            return true;
        case MirOp::Convert:
            if (HasDerivative(x[0]->GetType()))
            {
                add(0, TermKind::Convert);
            }
            return true;
        case MirOp::Intrinsic:
            break;
        default:
            Fail(&source, Format("{0} has no derivative rule", GetMirOpName(source.GetOp())).AsView());
            return false;
        }
        switch (source.GetIntrinsic())
        {
        case MirIntrinsic::Abs:
            add(0, TermKind::Scale, Unary(MirIntrinsic::Sign, x[0]));
            return true;
        case MirIntrinsic::Sign:
        case MirIntrinsic::Floor:
        case MirIntrinsic::Ceil:
        case MirIntrinsic::Round:
        case MirIntrinsic::Trunc:
        case MirIntrinsic::Step:
            return true;
        case MirIntrinsic::Fract:
            add(0, TermKind::Identity);
            return true;
        case MirIntrinsic::Sqrt:
            add(0, TermKind::Scale, Div(Splat(type, 0.5), y));
            return true;
        case MirIntrinsic::InverseSqrt:
            add(0, TermKind::Scale, Div(Mul(Splat(type, -0.5), y), x[0]));
            return true;
        case MirIntrinsic::Exp:
            add(0, TermKind::Scale, y);
            return true;
        case MirIntrinsic::Exp2:
            add(0, TermKind::Scale, Mul(y, Splat(type, Ln2)));
            return true;
        case MirIntrinsic::Log:
            add(0, TermKind::Scale, Div(one, x[0]));
            return true;
        case MirIntrinsic::Log2:
            add(0, TermKind::Scale, Div(one, Mul(x[0], Splat(type, Ln2))));
            return true;
        case MirIntrinsic::Pow:
        {
            MirValue *operands[] = {x[0], Sub(x[1], one)};
            add(0, TermKind::Scale, Mul(x[1], Intrinsic(MirIntrinsic::Pow, type, operands)));
            add(1, TermKind::Scale, Mul(y, Unary(MirIntrinsic::Log, x[0])));
            return true;
        }
        case MirIntrinsic::Sin:
            add(0, TermKind::Scale, Unary(MirIntrinsic::Cos, x[0]));
            return true;
        case MirIntrinsic::Cos:
            add(0, TermKind::Scale, negate(Unary(MirIntrinsic::Sin, x[0])));
            return true;
        case MirIntrinsic::Tan:
            add(0, TermKind::Scale, Add(one, Mul(y, y)));
            return true;
        case MirIntrinsic::Asin:
        case MirIntrinsic::Acos:
        {
            MirValue *root = Unary(MirIntrinsic::InverseSqrt, Sub(one, Mul(x[0], x[0])));
            add(0, TermKind::Scale, source.GetIntrinsic() == MirIntrinsic::Asin ? root : negate(root));
            return true;
        }
        case MirIntrinsic::Atan:
            add(0, TermKind::Scale, Div(one, Add(one, Mul(x[0], x[0]))));
            return true;
        case MirIntrinsic::Atan2:
        {
            MirValue *reciprocal = Div(one, Add(Mul(x[0], x[0]), Mul(x[1], x[1])));
            add(0, TermKind::Scale, Mul(x[1], reciprocal));
            add(1, TermKind::Scale, negate(Mul(x[0], reciprocal)));
            return true;
        }
        case MirIntrinsic::Sinh:
            add(0, TermKind::Scale, Unary(MirIntrinsic::Cosh, x[0]));
            return true;
        case MirIntrinsic::Cosh:
            add(0, TermKind::Scale, Unary(MirIntrinsic::Sinh, x[0]));
            return true;
        case MirIntrinsic::Tanh:
            add(0, TermKind::Scale, Sub(one, Mul(y, y)));
            return true;
        case MirIntrinsic::Min:
        case MirIntrinsic::Max:
        {
            const MirOp op = source.GetIntrinsic() == MirIntrinsic::Min ? MirOp::LessEqual : MirOp::GreaterEqual;
            MirValue *first = Mask(compare(op, x[0], x[1]), type);
            add(0, TermKind::Scale, first);
            add(1, TermKind::Scale, Sub(one, first));
            return true;
        }
        case MirIntrinsic::Clamp:
        {
            MirValue *below = Mask(compare(MirOp::Less, x[0], x[1]), type);
            MirValue *above = Mask(compare(MirOp::Greater, x[0], x[2]), type);
            add(0, TermKind::Scale, Sub(Sub(one, below), above));
            add(1, TermKind::Scale, below);
            add(2, TermKind::Scale, above);
            return true;
        }
        case MirIntrinsic::Saturate:
            add(0, TermKind::Scale,
                Mul(Mask(compare(MirOp::GreaterEqual, x[0], Splat(type, 0.0)), type),
                    Mask(compare(MirOp::LessEqual, x[0], one), type)));
            return true;
        case MirIntrinsic::Lerp:
            add(0, TermKind::Scale, Sub(one, x[2]));
            add(1, TermKind::Scale, x[2]);
            add(2, TermKind::Scale, Sub(x[1], x[0]));
            return true;
        case MirIntrinsic::SmoothStep:
        {
            MirValue *range = Sub(x[1], x[0]);
            MirValue *t = Unary(MirIntrinsic::Saturate, Div(Sub(x[2], x[0]), range));
            MirValue *slope = Div(Mul(Mul(Splat(type, 6.0), t), Sub(one, t)), range);
            add(0, TermKind::Scale, Mul(slope, Sub(t, one)));
            add(1, TermKind::Scale, negate(Mul(slope, t)));
            add(2, TermKind::Scale, slope);
            return true;
        }
        case MirIntrinsic::Fma:
            add(0, TermKind::Scale, x[1]);
            add(1, TermKind::Scale, x[0]);
            add(2, TermKind::Identity);
            return true;
        case MirIntrinsic::Ldexp:
            add(0, TermKind::Scale, Unary(MirIntrinsic::Exp2, _builder.Convert(x[1], type)));
            return true;
        case MirIntrinsic::Dot:
            add(0, TermKind::Dot, x[1]);
            add(1, TermKind::Dot, x[0]);
            return true;
        case MirIntrinsic::Cross:
            product(0, MirOp::Intrinsic, x[1], false);
            product(1, MirOp::Intrinsic, x[0], true);
            return true;
        case MirIntrinsic::Length:
            add(0, TermKind::Dot, Scaled(x[0], Div(one, y)));
            return true;
        case MirIntrinsic::Distance:
        {
            MirValue *direction = Scaled(Sub(x[0], x[1]), Div(one, y));
            add(0, TermKind::Dot, direction);
            add(1, TermKind::Dot, negate(direction));
            return true;
        }
        case MirIntrinsic::Normalize:
        {
            MirValue *operands[] = {x[0]};
            add(0, TermKind::Normalize, y, Intrinsic(MirIntrinsic::Length, type->GetScalar(), operands));
            return true;
        }
        case MirIntrinsic::Transpose:
            add(0, TermKind::Transpose);
            return true;
        default:
            Fail(&source, Format("{0} has no derivative rule", GetMirIntrinsicName(source.GetIntrinsic())).AsView());
            return false;
        }
    }

    /// *value*, a derivative, with its `[NoDiff]` fields zero, as every derivative keeps them; *value* itself when
    /// its type has none.
    MirValue *ClearNoDiff(MirValue *value)
    {
        const MirType *type = value->GetType();
        if (!HasNoDiffField(type))
        {
            return value;
        }
        List<MirValue *> parts;
        const uint32 count = type->Is(MirTypeKind::Struct) ? static_cast<uint32>(type->GetFields().Count())
                                                           : type->GetCount();
        for (uint32 i = 0; i < count; ++i)
        {
            const uint32 index[] = {i};
            if (type->Is(MirTypeKind::Struct) && type->GetFields()[i].NoDiff)
            {
                parts.Add(Splat(type->GetFields()[i].Type, 0.0));
                continue;
            }
            parts.Add(ClearNoDiff(_builder.Extract(value, index)));
        }
        return _builder.Construct(type, parts.AsSpan());
    }

    /// Starts the entry block: splits each active pair into its value and derivative, and maps the other parameters
    /// to the generated function's.
    void Prologue()
    {
        _builder.SetInsertPoint(*_blocks.Find(_source->GetEntry()));
        const Span<MirArgument *const> arguments = _result->GetArguments();
        for (const MirArgument *parameter : _source->GetArguments())
        {
            MirArgument *argument = arguments[parameter->GetIndex()];
            if (!IsActive(parameter->GetIndex()))
            {
                _primal.Add(parameter, argument);
                continue;
            }
            MirValue *pair = IsRef(parameter->GetIndex()) ? _builder.Load(argument) : static_cast<MirValue *>(argument);
            const uint32 value[] = {0};
            const uint32 derivative[] = {1};
            _primal.Add(parameter, _builder.Extract(pair, value));
            (void)derivative;
            for (_direction = 0; _forward && _direction < _tangentCount; ++_direction)
            {
                const uint32 field[] = {1 + _direction};
                MirInstruction *tangent = _builder.Extract(pair, field);
                tangent->SetName("tangent");
                SetTangent(parameter, ClearNoDiff(tangent));
            }
            _direction = 0;
        }
    }

    /// A copy of *original* at the end of *block* with its operands mapped; a phi gets its incoming values later.
    MirInstruction *Copy(const MirInstruction &original, MirBlock *block)
    {
        Arena &arena = _module->GetArena();
        auto *copy = arena.New<MirInstruction>(original.GetOp(), original.GetType(), arena);
        copy->SetIntrinsic(original.GetIntrinsic());
        copy->SetName(original.GetName());
        copy->Location = original.Location;
        copy->NonUniform = original.NonUniform;
        copy->Restrict = original.Restrict;
        copy->Precise = original.Precise;
        copy->Checked = original.Checked;
        for (const int64 literal : original.GetLiterals())
        {
            copy->AddLiteral(literal);
        }
        if (original.GetOp() != MirOp::Phi)
        {
            for (MirBlock *target : original.GetTargets())
            {
                copy->AddTarget(*_blocks.Find(target));
            }
            for (MirValue *operand : original.GetOperands())
            {
                copy->AddOperand(Primal(operand));
            }
        }
        block->Append(copy);
        return copy;
    }

    static MirInstruction *FirstNonPhi(const MirBlock *block)
    {
        MirInstruction *instruction = block->GetFirst();
        while (instruction != nullptr && instruction->GetOp() == MirOp::Phi)
        {
            instruction = instruction->GetNext();
        }
        return instruction;
    }

    /// Copies *block*; in forward mode with the tangents after each instruction, in reverse mode with the return
    /// left for the reverse sweep to replace.
    void CloneBlock(const MirBlock &block)
    {
        MirBlock *clone = *_blocks.Find(&block);
        _builder.SetInsertPoint(clone);
        if (block.Merge.Kind != MirMergeKind::None)
        {
            clone->Merge = block.Merge;
            clone->Merge.MergeBlock = *_blocks.Find(block.Merge.MergeBlock);
            clone->Merge.ContinueBlock =
                block.Merge.ContinueBlock != nullptr ? *_blocks.Find(block.Merge.ContinueBlock) : nullptr;
        }
        for (const MirInstruction *instruction : block)
        {
            _builder.SetLocation(instruction->Location);
            if (instruction->GetOp() == MirOp::Return)
            {
                MirValue *returned = instruction->GetOperands().IsEmpty() ? nullptr : instruction->GetOperand(0);
                if (_forward)
                {
                    List<MirValue *> parts;
                    parts.Add(Primal(returned));
                    for (_direction = 0; _direction < _tangentCount; ++_direction)
                    {
                        parts.Add(TangentOrZero(returned));
                    }
                    _direction = 0;
                    _builder.Return(_builder.Construct(_result->GetReturnType(), parts.AsSpan()));
                }
                else
                {
                    _returned = returned;
                    _returnBlock = clone;
                }
                continue;
            }
            if (_forward && instruction->GetOp() == MirOp::Call && _varied.Contains(instruction))
            {
                ForwardCall(*instruction);
                continue;
            }
            MirInstruction *copy = Copy(*instruction, clone);
            _primal.Add(instruction, copy);
            if (instruction->GetOp() == MirOp::Phi)
            {
                _phis.Add(instruction);
                for (_direction = 0; _forward && _varied.Contains(instruction) && _direction < _tangentCount;
                     ++_direction)
                {
                    MirInstruction *tangent = _builder.Phi(instruction->GetType());
                    tangent->SetName("tangent");
                    SetTangent(instruction, tangent);
                }
                _direction = 0;
            }
            else if (_forward && !instruction->IsTerminator())
            {
                ForwardTangent(*instruction);
            }
        }
    }

    /// Gives the copied phis, and in forward mode their tangents, their incoming values.
    void CompletePhis()
    {
        for (const MirInstruction *phi : _phis)
        {
            auto *copy = static_cast<MirInstruction *>(*_primal.Find(phi));
            for (usize i = 0; i < phi->GetOperands().Count(); ++i)
            {
                MirBlock *incoming = *_blocks.Find(phi->GetTargets()[i]);
                copy->AddOperand(Primal(phi->GetOperand(i)));
                copy->AddTarget(incoming);
            }
            for (_direction = 0; _direction < _tangentCount; ++_direction)
            {
                MirValue **found = _tangents[_direction].Find(phi);
                if (found == nullptr)
                {
                    continue;
                }
                auto *tangent = static_cast<MirInstruction *>(*found);
                for (usize i = 0; i < phi->GetOperands().Count(); ++i)
                {
                    MirValue *value = Tangent(phi->GetOperand(i));
                    tangent->AddOperand(value != nullptr ? value : Splat(phi->GetType(), 0.0));
                    tangent->AddTarget(*_blocks.Find(phi->GetTargets()[i]));
                }
            }
            _direction = 0;
        }
    }

    /// Whether argument *index* of a call to *callee* is differentiable, so its custom derivatives take a pair for it.
    static bool TakesPair(const MirFunction &callee, usize index)
    {
        return index < 32 && (callee.DerivativeArguments & (1u << index)) != 0;
    }

    /// Calls the custom forward derivative of the function *call* calls in its place: it takes a pair for each
    /// differentiable argument, and the primal and tangents of the result are the parts of the pair it returns. With
    /// several directions, the `[ForwardDerivative2]` takes them all at once; without one, the `[ForwardDerivative]`
    /// is called once per direction and the first call's primal kept.
    void ForwardCall(const MirInstruction &call)
    {
        const MirFunction *callee = CalleeOf(call);
        const bool together = _tangentCount > 1 && callee->ForwardDerivative2 != nullptr;
        MirFunction *derivative = together ? callee->ForwardDerivative2 : callee->ForwardDerivative;
        const Span<MirArgument *const> parameters = derivative->GetArguments();
        const uint32 calls = together ? 1 : _tangentCount;
        for (uint32 c = 0; c < calls; ++c)
        {
            List<MirValue *> arguments;
            for (usize i = 1; i < call.GetOperands().Count(); ++i)
            {
                MirValue *operand = call.GetOperand(i);
                if (!TakesPair(*callee, i - 1))
                {
                    arguments.Add(Primal(operand));
                    continue;
                }
                List<MirValue *> parts;
                parts.Add(Primal(operand));
                for (_direction = together ? 0 : c; _direction < (together ? _tangentCount : c + 1); ++_direction)
                {
                    parts.Add(TangentOrZero(operand));
                }
                arguments.Add(_builder.Construct(parameters[i - 1]->GetType(), parts.AsSpan()));
            }
            MirInstruction *pair = _builder.Call(derivative, arguments.AsSpan());
            if (c == 0)
            {
                const uint32 value[] = {0};
                _primal.Add(&call, _builder.Extract(pair, value));
            }
            for (_direction = together ? 0 : c; _direction < (together ? _tangentCount : c + 1); ++_direction)
            {
                const uint32 field[] = {together ? 1 + _direction : 1};
                MirValue *tangent = ClearNoDiff(_builder.Extract(pair, field));
                tangent->SetName("tangent");
                SetTangent(&call, tangent);
            }
        }
        _direction = 0;
    }

    /// Computes the tangent of *source* after its copy.
    void ForwardTangent(const MirInstruction &source)
    {
        if (!_varied.Contains(&source))
        {
            return;
        }
        if (IsStructural(source))
        {
            for (_direction = 0; _direction < _tangentCount; ++_direction)
            {
                SetTangent(&source, StructuralTangent(source));
            }
            _direction = 0;
            return;
        }
        List<MirValue *> x;
        for (MirValue *operand : source.GetOperands())
        {
            x.Add(Primal(operand));
        }
        List<Term> terms;
        if (!Terms(source, x.AsSpan(), *_primal.Find(&source), terms))
        {
            return;
        }
        for (_direction = 0; _direction < _tangentCount; ++_direction)
        {
            MirValue *sum = nullptr;
            for (const Term &term : terms)
            {
                if (MirValue *d = Tangent(source.GetOperand(term.Operand)))
                {
                    sum = Add(sum, Apply(term, d, source.GetType()));
                }
            }
            if (sum != nullptr)
            {
                sum->SetName("tangent");
            }
            SetTangent(&source, sum != nullptr ? sum : Splat(source.GetType(), 0.0));
        }
        _direction = 0;
    }

    /// Whether *source* only moves values, so that its tangent is the same operation on the tangents.
    static bool IsStructural(const MirInstruction &source)
    {
        switch (source.GetOp())
        {
        case MirOp::CompositeConstruct:
        case MirOp::CompositeExtract:
        case MirOp::CompositeInsert:
        case MirOp::VectorShuffle:
        case MirOp::Select:
        case MirOp::VectorExtractDynamic:
        case MirOp::VectorInsertDynamic:
            return true;
        default:
            return false;
        }
    }

    /// The tangent along the current direction of an operation that only moves values (IsStructural): the same
    /// operation on the tangents, with zero for `[NoDiff]` fields.
    MirValue *StructuralTangent(const MirInstruction &source)
    {
        auto tangentOf = [&](usize index) { return TangentOrZero(source.GetOperand(index)); };
        switch (source.GetOp())
        {
        case MirOp::CompositeConstruct:
        {
            const MirType *type = source.GetType();
            List<MirValue *> parts;
            for (usize i = 0; i < source.GetOperands().Count(); ++i)
            {
                const bool noDiff = type->Is(MirTypeKind::Struct) && type->GetFields()[i].NoDiff;
                parts.Add(noDiff ? static_cast<MirValue *>(Splat(source.GetOperand(i)->GetType(), 0.0))
                                 : tangentOf(i));
            }
            return _builder.Construct(type, parts.AsSpan());
        }
        case MirOp::CompositeExtract:
            if (IsNoDiffPath(source.GetOperand(0)->GetType(), source.GetLiterals()))
            {
                return Splat(source.GetType(), 0.0);
            }
            return _builder.Extract(tangentOf(0), Indices(source));
        case MirOp::CompositeInsert:
            if (IsNoDiffPath(source.GetOperand(0)->GetType(), source.GetLiterals()))
            {
                return tangentOf(0);
            }
            return _builder.Insert(tangentOf(0), tangentOf(1), Indices(source));
        case MirOp::VectorShuffle:
            return _builder.Shuffle(tangentOf(0), tangentOf(1), Indices(source));
        case MirOp::Select:
            return _builder.Select(Primal(source.GetOperand(0)), tangentOf(1), tangentOf(2));
        case MirOp::VectorExtractDynamic:
            return _builder.ExtractDynamic(tangentOf(0), Primal(source.GetOperand(1)));
        case MirOp::VectorInsertDynamic:
            return _builder.InsertDynamic(tangentOf(0), tangentOf(1), Primal(source.GetOperand(2)));
        default:
            return nullptr;
        }
    }

    Span<const uint32> Indices(const MirInstruction &source)
    {
        _indices.Clear();
        for (const int64 literal : source.GetLiterals())
        {
            _indices.Add(static_cast<uint32>(literal));
        }
        return _indices.AsSpan();
    }

    /// A loop of the source as the reverse sweep walks it: backwards trip by trip, reading what the primal code kept
    /// of each trip.
    struct Loop
    {
        const MirBlock *Header = nullptr;
        const MirBlock *Latch = nullptr;
        const MirBlock *Merge = nullptr;
        const MirBlock *Preheader = nullptr;
        Loop *Parent = nullptr;
        /// Whether the condition is tested before the body (`for`, `while`), so the head runs once more than it.
        bool TestFirst = true;
        /// The slots a kept value has per trip: the bound on the trips plus one; 0 until a value is kept.
        uint32 Slots = 0;
        /// The trips the primal code has counted, and the trip the reverse sweep is on; null until needed.
        MirInstruction *Counter = nullptr;
        MirInstruction *Trip = nullptr;
        /// For a loop inside another, its trip count kept per trip of the outer loops.
        MirInstruction *Trips = nullptr;
    };

    /// A straight-line block, the header of a selection with the items of each branch, or a loop with the items of
    /// its head and body, as the reverse sweep walks them.
    struct Item
    {
        const MirBlock *Block = nullptr;
        /// Whether *Block* starts a selection on *Condition*, which ends at *Merge*.
        bool IsSelection = false;
        MirValue *Condition = nullptr;
        List<Item> Then;
        List<Item> Else;
        const MirBlock *ThenLast = nullptr;
        const MirBlock *ElseLast = nullptr;
        const MirBlock *Merge = nullptr;
        /// The loop *Block* is the header of, or null. Every trip runs its *Head* (the header, and the block that
        /// tests the condition before the body), then, unless the test ends the loop, its *Body* up to the latch.
        Loop *LoopOf = nullptr;
        List<Item> Head;
        List<Item> Body;
    };

    void Enter(const MirBlock *block, uint32 depth, Loop *loop)
    {
        _depths.Set(block, depth);
        if (loop != nullptr)
        {
            _blockLoops.Set(block, loop);
        }
    }

    /// Reports *branch* when its *target* leaves the body of a loop other than at its end: `break` and `continue`.
    bool Leaves(const MirInstruction *branch, const MirBlock *target, const MirBlock *stop)
    {
        if (target != stop && _exits.Contains(target))
        {
            Fail(branch, "it leaves a loop with break or continue");
            return true;
        }
        return false;
    }

    /// Splits the blocks from *start* up to *stop* into items at nesting *depth* in *loop*; returns the last block,
    /// the one that branches to *stop*.
    const MirBlock *Parse(const MirBlock *start, const MirBlock *stop, uint32 depth, Loop *loop, List<Item> &items)
    {
        const MirBlock *block = start;
        const MirBlock *last = start;
        while (block != nullptr && block != stop && !_failed)
        {
            Enter(block, depth, loop);
            last = block;
            Item item;
            item.Block = block;
            if (block->Merge.Kind == MirMergeKind::Loop)
            {
                if (!ParseLoop(block, depth, loop, item))
                {
                    return last;
                }
                block = item.LoopOf->Merge;
                items.Add(Move(item));
                continue;
            }
            const MirInstruction *terminator = block->GetTerminator();
            if (terminator->GetOp() != MirOp::CondBranch)
            {
                items.Add(Move(item));
                block = terminator->GetOp() == MirOp::Branch ? terminator->GetTargets()[0] : nullptr;
                if (block != nullptr && Leaves(terminator, block, stop))
                {
                    return last;
                }
                continue;
            }
            const MirBlock *merge = block->Merge.MergeBlock;
            if (block->Merge.Kind != MirMergeKind::Selection || merge == stop)
            {
                // RECOVERY-DECISION lang-ir-D3: reported like the other limits of the pass.
                Fail(terminator, "it branches outside a structured if/else; use if");
                return last;
            }
            item.IsSelection = true;
            item.Condition = terminator->GetOperand(0);
            item.Merge = merge;
            const MirBlock *whenTrue = terminator->GetTargets()[0];
            const MirBlock *whenFalse = terminator->GetTargets()[1];
            if (Leaves(terminator, whenTrue, merge) || Leaves(terminator, whenFalse, merge))
            {
                return last;
            }
            item.ThenLast = whenTrue == merge ? block : Parse(whenTrue, merge, depth + 1, loop, item.Then);
            item.ElseLast = whenFalse == merge ? block : Parse(whenFalse, merge, depth + 1, loop, item.Else);
            items.Add(Move(item));
            block = merge;
        }
        return last;
    }

    /// Fills *item* with the loop *header* starts, inside *parent*; false, after reporting it, when the loop leaves
    /// other than through its condition, tested before the body or at its end.
    bool ParseLoop(const MirBlock *header, uint32 depth, Loop *parent, Item &item)
    {
        _loops.Add(MakeUnique<Loop>());
        Loop *loop = _loops.Last().Get();
        loop->Header = header;
        loop->Latch = header->Merge.ContinueBlock;
        loop->Merge = header->Merge.MergeBlock;
        loop->Parent = parent;
        item.LoopOf = loop;
        for (MirBlock *predecessor : _flow->GetPredecessors(header))
        {
            if (predecessor != loop->Latch)
            {
                loop->Preheader = predecessor;
            }
        }
        // The other target of a conditional branch that leaves the loop.
        auto exits = [&](const MirInstruction *branch) -> const MirBlock * {
            if (branch->GetOp() != MirOp::CondBranch)
            {
                return nullptr;
            }
            const Span<MirBlock *const> targets = branch->GetTargets();
            return targets[0] == loop->Merge && targets[1] != loop->Merge   ? targets[1]
                   : targets[1] == loop->Merge && targets[0] != loop->Merge ? targets[0]
                                                                            : nullptr;
        };
        const MirInstruction *enter = header->GetTerminator();
        const MirBlock *check = nullptr;
        const MirBlock *body = exits(enter);
        if (body != nullptr)
        {
            check = header;
        }
        else if (enter->GetOp() == MirOp::Branch)
        {
            const MirBlock *next = enter->GetTargets()[0];
            const MirBlock *afterCheck = next != loop->Latch && next->Merge.Kind == MirMergeKind::None
                                             ? exits(next->GetTerminator())
                                             : nullptr;
            check = afterCheck != nullptr ? next : nullptr;
            body = afterCheck != nullptr ? afterCheck : next;
        }
        loop->TestFirst = check != nullptr;
        const MirInstruction *back = loop->Latch->GetTerminator();
        const bool backs = loop->TestFirst ? back->GetOp() == MirOp::Branch && back->GetTargets()[0] == header
                                           : exits(back) == header;
        if (body == nullptr || !backs || loop->Preheader == nullptr || _flow->GetPredecessors(header).Count() != 2)
        {
            Fail(enter, "it has a loop that leaves other than through its condition");
            return false;
        }
        Item headerItem;
        headerItem.Block = header;
        item.Head.Add(Move(headerItem));
        Enter(header, depth, loop);
        if (check != nullptr && check != header)
        {
            Item checkItem;
            checkItem.Block = check;
            item.Head.Add(Move(checkItem));
            Enter(check, depth, loop);
        }
        _exits.Add(loop->Merge);
        _exits.Add(loop->Latch);
        if (body != loop->Latch)
        {
            Parse(body, loop->Latch, depth + 1, loop, item.Body);
        }
        _exits.Pop();
        _exits.Pop();
        Item latchItem;
        latchItem.Block = loop->Latch;
        item.Body.Add(Move(latchItem));
        Enter(loop->Latch, depth + 1, loop);
        return !_failed;
    }

    /// Whether a block of *items* computes a value that carries a derivative.
    bool AnyVaried(const List<Item> &items) const
    {
        for (const Item &item : items)
        {
            for (const MirInstruction *instruction : *item.Block)
            {
                if (_varied.Contains(instruction))
                {
                    return true;
                }
            }
            if (AnyVaried(item.Then) || AnyVaried(item.Else) || AnyVaried(item.Head) || AnyVaried(item.Body))
            {
                return true;
            }
        }
        return false;
    }

    /// Runs *emit* with new instructions going before *position*, then goes back to the end of the current block.
    template <typename TEmit> void At(MirInstruction *position, TEmit emit)
    {
        MirBlock *block = _builder.GetBlock();
        _builder.SetInsertPoint(position);
        emit();
        _builder.SetInsertPoint(block);
    }

    const MirType *IndexType()
    {
        return _types->GetInt(32, true);
    }

    /// Makes the primal code count the trips of *loop*: zero before it starts, one more at each latch.
    void Count(Loop *loop)
    {
        if (loop->Counter != nullptr)
        {
            return;
        }
        const MirType *type = IndexType();
        loop->Counter = _builder.Variable(type, "trips");
        loop->Trip = _builder.Variable(type, "trip");
        At((*_blocks.Find(loop->Preheader))->GetTerminator(),
           [&] { _builder.Store(loop->Counter, _module->GetInt(type, 0)); });
        At((*_blocks.Find(loop->Latch))->GetTerminator(), [&] {
            _builder.Store(loop->Counter,
                           _builder.Binary(MirOp::Add, _builder.Load(loop->Counter), _module->GetInt(type, 1)));
        });
    }

    /// The slots a kept value has per trip of *loop*: its `[MaxIterations(n)]`, else its constant trip count, plus
    /// one for the test that ends it; 0 after reporting a loop with neither.
    uint32 SlotsOf(Loop *loop)
    {
        if (loop->Slots != 0 || _failed)
        {
            return loop->Slots;
        }
        uint32 bound = loop->Header->Merge.MaxIterations;
        if (bound == 0 && !GetConstantTripCount(const_cast<MirBlock *>(loop->Header), *_flow, bound))
        {
            Fail(loop->Header->GetTerminator(),
                 "the loop needs `[MaxIterations(n)]`: reverse mode keeps values of every iteration, and its trip "
                 "count is not a constant");
            return 0;
        }
        loop->Slots = bound + 1;
        return loop->Slots;
    }

    /// A variable keeping a *type* per trip of *loops*, outermost first: an array with a slot for each combination of
    /// their trips, or a plain variable for no loop; null after reporting a loop without a bound.
    MirInstruction *Tape(const MirType *type, Span<Loop *const> loops, StringView name)
    {
        uint64 count = 1;
        for (Loop *loop : loops)
        {
            count *= SlotsOf(loop);
        }
        if (_failed)
        {
            return nullptr;
        }
        constexpr uint64 MaxSlots = uint64{1} << 20;
        if (count > MaxSlots)
        {
            Fail(loops[0]->Header->GetTerminator(), "its loops keep too many values; lower their `[MaxIterations(n)]`");
            return nullptr;
        }
        return _builder.Variable(loops.IsEmpty() ? type : _types->GetArray(type, static_cast<uint32>(count)), name);
    }

    /// The slot of *tape* for the trips of *loops*: those the primal code is on when *primal*, else those the reverse
    /// sweep is on. The primal trips are clamped to the bound, so a loop that runs longer stays in the array.
    MirValue *Slot(MirValue *tape, Span<Loop *const> loops, bool primal)
    {
        if (loops.IsEmpty())
        {
            return tape;
        }
        const MirType *type = IndexType();
        MirValue *index = nullptr;
        for (Loop *loop : loops)
        {
            MirValue *trip = nullptr;
            if (primal)
            {
                MirValue *operands[] = {_builder.Load(loop->Counter),
                                        _module->GetInt(type, static_cast<int64>(loop->Slots) - 1)};
                trip = Intrinsic(MirIntrinsic::Min, type, operands);
            }
            else
            {
                trip = _builder.Load(loop->Trip);
            }
            index = index == nullptr ? trip
                                     : _builder.Binary(MirOp::Add,
                                                       _builder.Binary(MirOp::Mul, index,
                                                                       _module->GetInt(type, loop->Slots)),
                                                       trip);
        }
        MirValue *indices[] = {index};
        return _builder.AccessChain(tape, indices);
    }

    /// The loops enclosing *block* that the reverse sweep is in, outermost first: a value of the block read there is
    /// kept per trip of them.
    void ActiveLoopsOf(const MirBlock *block, List<Loop *> &loops)
    {
        List<Loop *> chain;
        Loop **found = _blockLoops.Find(block);
        for (Loop *loop = found != nullptr ? *found : nullptr; loop != nullptr; loop = loop->Parent)
        {
            chain.Insert(0, loop);
        }
        for (usize i = 0; i < chain.Count() && i < _active.Count() && chain[i] == _active[i]; ++i)
        {
            loops.Add(chain[i]);
        }
    }

    /// The primal *value* where the reverse sweep is. A value of a block inside a selection does not dominate the
    /// reverse sweep, so it is kept in a variable stored where it is computed; one of a loop the sweep is in is kept
    /// per trip, in a tape.
    MirValue *Kept(MirValue *value)
    {
        if (value->GetValueKind() != MirValueKind::Instruction)
        {
            return Primal(value);
        }
        const MirBlock *block = static_cast<const MirInstruction *>(value)->GetParent();
        List<Loop *> loops;
        ActiveLoopsOf(block, loops);
        const uint32 *depth = _depths.Find(block);
        if (loops.IsEmpty() && (depth == nullptr || *depth == 0))
        {
            return Primal(value);
        }
        while (_kept.Count() <= loops.Count())
        {
            _kept.Add({});
        }
        if (MirInstruction **tape = _kept[loops.Count()].Find(value))
        {
            return _builder.Load(Slot(*tape, loops.AsSpan(), false));
        }
        MirInstruction *tape = Tape(value->GetType(), loops.AsSpan(), {});
        if (tape == nullptr)
        {
            return Splat(value->GetType(), 0.0);
        }
        _tapes.Add(tape);
        auto *definition = static_cast<MirInstruction *>(Primal(value));
        MirBlock *definitionBlock = definition->GetParent();
        At(definition->GetOp() == MirOp::Phi ? FirstNonPhi(definitionBlock) : definition->GetNext(),
           [&] { _builder.Store(Slot(tape, loops.AsSpan(), true), definition); });
        _kept[loops.Count()].Add(value, tape);
        return _builder.Load(Slot(tape, loops.AsSpan(), false));
    }

    /// How many times the body of *loop* ran, where the reverse sweep is. A loop inside another keeps its count per
    /// trip of the outer ones.
    MirValue *Trips(Loop *loop)
    {
        Count(loop);
        if (loop->Parent == nullptr)
        {
            return _builder.Load(loop->Counter);
        }
        List<Loop *> outer;
        for (Loop *parent = loop->Parent; parent != nullptr; parent = parent->Parent)
        {
            outer.Insert(0, parent);
        }
        if (loop->Trips == nullptr)
        {
            loop->Trips = Tape(IndexType(), outer.AsSpan(), "trips");
            if (loop->Trips == nullptr)
            {
                return _module->GetInt(IndexType(), 0);
            }
            At(FirstNonPhi(*_blocks.Find(loop->Merge)),
               [&] { _builder.Store(Slot(loop->Trips, outer.AsSpan(), true), _builder.Load(loop->Counter)); });
        }
        return _builder.Load(Slot(loop->Trips, outer.AsSpan(), false));
    }

    /// The variable the adjoint of *value* accumulates in, zero when the function starts.
    MirValue *AdjointVariable(MirValue *value)
    {
        if (MirValue **found = _adjoints.Find(value))
        {
            return *found;
        }
        MirInstruction *variable = _builder.Variable(value->GetType(), "adjoint");
        Arena &arena = _module->GetArena();
        auto *store = arena.New<MirInstruction>(MirOp::Store, _types->GetVoid(), arena);
        store->AddOperand(variable);
        store->AddOperand(Splat(value->GetType(), 0.0));
        _initialize->GetParent()->InsertBefore(store, _initialize);
        _adjoints.Add(value, variable);
        return variable;
    }

    void Accumulate(MirValue *value, MirValue *contribution)
    {
        if (contribution == nullptr || !_varied.Contains(value))
        {
            return;
        }
        MirValue *variable = AdjointVariable(value);
        _builder.Store(variable, Add(_builder.Load(variable), contribution));
    }

    /// The adjoint of *value* accumulated so far; null when nothing reached it.
    MirValue *Adjoint(MirValue *value)
    {
        MirValue **found = _adjoints.Find(value);
        return found != nullptr ? _builder.Load(*found) : nullptr;
    }

    /// Inside a loop, sets the adjoint of *value* back to zero once it has been propagated, so the sweep of the
    /// previous trip accumulates the adjoint of that trip's value.
    void Consumed(MirValue *value)
    {
        MirValue **found = _adjoints.Find(value);
        if (found != nullptr && !_active.IsEmpty())
        {
            _builder.Store(*found, Splat(value->GetType(), 0.0));
        }
    }

    /// Appends the reverse sweep after the primal code: seeds the result with dResult, walks the items backwards,
    /// and stores each gradient into its pair.
    void Reverse()
    {
        _initialize = FirstNonPhi(_result->GetEntry());
        while (_initialize->GetOp() == MirOp::Variable)
        {
            _initialize = _initialize->GetNext();
        }
        MirBlock *start = _result->AddBlock("reverse");
        _builder.SetInsertPoint(_returnBlock);
        _builder.Branch(start);
        _builder.SetInsertPoint(start);
        _builder.SetLocation(_source->Location);
        if (_returned != nullptr)
        {
            Accumulate(_returned, ClearNoDiff(_result->GetArguments().Last()));
        }
        ReverseItems(_items);
        for (MirArgument *parameter : _source->GetArguments())
        {
            if (!IsActive(parameter->GetIndex()) || !IsRef(parameter->GetIndex()))
            {
                continue;
            }
            MirArgument *pointer = _result->GetArguments()[parameter->GetIndex()];
            // RECOVERY-DECISION lang-ir-D2: the gradient replaces the pair's Differential (adjoint or null, as 00422).
            MirValue *gradient = _module->GetNull(parameter->GetType());
            if (MirValue *adjoint = Adjoint(parameter))
            {
                gradient = adjoint;
            }
            const uint32 differential[] = {1};
            _builder.Store(pointer, _builder.Insert(_builder.Load(pointer), gradient, differential));
        }
        _builder.Return();
        DropUnreadTapes();
    }

    /// Drops the tapes nothing reads. A value is kept before it is known whether the derivative rule reading it
    /// uses it, so first the reverse sweep's unused computations go, and with them the reads of such tapes.
    void DropUnreadTapes()
    {
        bool removed = true;
        while (removed)
        {
            removed = false;
            for (MirBlock *block : _result->GetBlocks())
            {
                for (MirInstruction *instruction = block->GetFirst(); instruction != nullptr;)
                {
                    MirInstruction *next = instruction->GetNext();
                    if (!instruction->HasUses() && instruction->GetOp() != MirOp::Variable &&
                        !HasSideEffects(*instruction))
                    {
                        instruction->Erase();
                        removed = true;
                    }
                    instruction = next;
                }
            }
        }
        for (MirInstruction *tape : _tapes)
        {
            List<MirInstruction *> writes;
            bool read = false;
            for (const MirUse &use : tape->GetUses())
            {
                if (use.User->GetOp() != MirOp::AccessChain)
                {
                    read = read || use.User->GetOp() != MirOp::Store;
                    writes.Add(use.User);
                    continue;
                }
                for (const MirUse &element : use.User->GetUses())
                {
                    read = read || element.User->GetOp() != MirOp::Store;
                    writes.Add(element.User);
                }
                writes.Add(use.User);
            }
            if (read)
            {
                continue;
            }
            // The stores come before the element pointers they store through.
            for (MirInstruction *write : writes)
            {
                write->Erase();
            }
            tape->Erase();
        }
    }

    /// Propagates the adjoints of the phis of *merge* to the values they take from the predecessor *from*, or from
    /// any predecessor when null.
    void ReversePhis(const MirBlock *merge, const MirBlock *from)
    {
        for (const MirInstruction *phi : *merge)
        {
            if (phi->GetOp() != MirOp::Phi)
            {
                break;
            }
            MirValue *adjoint = Adjoint(const_cast<MirInstruction *>(phi));
            for (usize k = 0; adjoint != nullptr && k < phi->GetOperands().Count(); ++k)
            {
                if (from == nullptr || phi->GetTargets()[k] == from)
                {
                    Accumulate(phi->GetOperand(k), adjoint);
                }
            }
        }
    }

    void ReverseItems(const List<Item> &items)
    {
        for (usize i = items.Count(); i-- > 0 && !_failed;)
        {
            const Item &item = items[i];
            if (item.LoopOf != nullptr)
            {
                ReverseLoop(item);
                continue;
            }
            if (item.IsSelection)
            {
                MirBlock *header = _builder.GetBlock();
                MirBlock *branches[] = {_result->AddBlock("reverse.then"), _result->AddBlock("reverse.else")};
                _builder.CondBranch(Kept(item.Condition), branches[0], branches[1]);
                const MirBlock *lasts[] = {item.ThenLast, item.ElseLast};
                MirBlock *ends[2] = {};
                for (uint32 side = 0; side < 2; ++side)
                {
                    _builder.SetInsertPoint(branches[side]);
                    ReversePhis(item.Merge, lasts[side]);
                    ReverseItems(side == 0 ? item.Then : item.Else);
                    ends[side] = _builder.GetBlock();
                }
                MirBlock *merge = _result->AddBlock("reverse.merge");
                for (MirBlock *end : ends)
                {
                    _builder.SetInsertPoint(end);
                    _builder.Branch(merge);
                }
                header->Merge.Kind = MirMergeKind::Selection;
                header->Merge.MergeBlock = merge;
                _builder.SetInsertPoint(merge);
                for (const MirInstruction *phi : *item.Merge)
                {
                    if (phi->GetOp() != MirOp::Phi)
                    {
                        break;
                    }
                    Consumed(const_cast<MirInstruction *>(phi));
                }
            }
            ReverseBlock(*item.Block);
        }
    }

    /// Sweeps *item*, a loop, backwards: past the test that ended a `for` or `while` loop, then a reverse loop
    /// counting the trips down, each running the trip's body and head backwards on the values kept of that trip.
    void ReverseLoop(const Item &item)
    {
        Loop *loop = item.LoopOf;
        if (!AnyVaried(item.Head) && !AnyVaried(item.Body))
        {
            return;
        }
        ReversePhis(loop->Merge, nullptr);
        MirValue *trips = Trips(loop);
        if (_failed)
        {
            return;
        }
        const MirType *type = IndexType();
        MirConstant *zero = _module->GetInt(type, 0);
        MirConstant *one = _module->GetInt(type, 1);
        _active.Add(loop);
        if (loop->TestFirst)
        {
            _builder.Store(loop->Trip, trips);
            ReverseItems(item.Head);
            ReverseHeaderPhis(*loop);
        }
        _builder.Store(loop->Trip, _builder.Binary(MirOp::Sub, trips, one));
        MirBlock *header = _result->AddBlock("reverse.loop");
        MirBlock *check = _result->AddBlock("reverse.loop.check");
        MirBlock *body = _result->AddBlock("reverse.loop.body");
        MirBlock *latch = _result->AddBlock("reverse.loop.continue");
        MirBlock *merge = _result->AddBlock("reverse.loop.merge");
        _builder.Branch(header);
        header->Merge.Kind = MirMergeKind::Loop;
        header->Merge.MergeBlock = merge;
        header->Merge.ContinueBlock = latch;
        _builder.SetInsertPoint(header);
        _builder.Branch(check);
        _builder.SetInsertPoint(check);
        _builder.CondBranch(_builder.Compare(MirOp::GreaterEqual, _builder.Load(loop->Trip), zero), body, merge);
        _builder.SetInsertPoint(body);
        ReverseItems(item.Body);
        ReverseItems(item.Head);
        ReverseHeaderPhis(*loop);
        _builder.Branch(latch);
        _builder.SetInsertPoint(latch);
        _builder.Store(loop->Trip, _builder.Binary(MirOp::Sub, _builder.Load(loop->Trip), one));
        _builder.Branch(header);
        _builder.SetInsertPoint(merge);
        _active.Pop();
    }

    /// Propagates the adjoints of the header phis of *loop* on the trip the sweep is on: to the values the previous
    /// trip's latch gave, or on the first trip to those the loop starts with.
    void ReverseHeaderPhis(const Loop &loop)
    {
        List<const MirInstruction *> phis;
        List<MirValue *> adjoints;
        for (const MirInstruction *phi : *loop.Header)
        {
            if (phi->GetOp() != MirOp::Phi)
            {
                break;
            }
            auto *value = const_cast<MirInstruction *>(phi);
            if (MirValue *adjoint = Adjoint(value))
            {
                phis.Add(phi);
                adjoints.Add(adjoint);
                Consumed(value);
            }
        }
        if (phis.IsEmpty())
        {
            return;
        }
        MirBlock *header = _builder.GetBlock();
        MirBlock *later = _result->AddBlock("reverse.loop.later");
        MirBlock *first = _result->AddBlock("reverse.loop.first");
        MirBlock *merge = _result->AddBlock("reverse.loop.phis");
        header->Merge.Kind = MirMergeKind::Selection;
        header->Merge.MergeBlock = merge;
        _builder.CondBranch(_builder.Compare(MirOp::Greater, _builder.Load(loop.Trip), _module->GetInt(IndexType(), 0)),
                            later, first);
        for (uint32 side = 0; side < 2; ++side)
        {
            _builder.SetInsertPoint(side == 0 ? later : first);
            for (usize i = 0; i < phis.Count(); ++i)
            {
                Accumulate(PhiIncoming(phis[i], side == 0 ? loop.Latch : loop.Preheader), adjoints[i]);
            }
            _builder.Branch(merge);
        }
        _builder.SetInsertPoint(merge);
    }

    /// Propagates the adjoints of the results of *block*, last instruction first, to their operands.
    void ReverseBlock(const MirBlock &block)
    {
        for (const MirInstruction *instruction = block.GetLast(); instruction != nullptr && !_failed;
             instruction = instruction->GetPrevious())
        {
            if (instruction->GetOp() == MirOp::Phi || !_varied.Contains(instruction))
            {
                continue;
            }
            auto *value = const_cast<MirInstruction *>(instruction);
            MirValue **found = _adjoints.Find(value);
            if (found == nullptr)
            {
                continue;
            }
            _builder.SetLocation(instruction->Location);
            MirValue *a = _builder.Load(*found);
            ReverseInstruction(*instruction, a);
            Consumed(value);
        }
    }

    /// Propagates the adjoint *a* of the result of *instruction* to its operands.
    void ReverseInstruction(const MirInstruction &instruction, MirValue *a)
    {
        if (instruction.GetOp() == MirOp::Call)
        {
            ReverseCall(instruction, a);
            return;
        }
        if (ReverseStructural(instruction, a))
        {
            return;
        }
        List<MirValue *> x;
        for (MirValue *operand : instruction.GetOperands())
        {
            x.Add(Kept(operand));
        }
        List<Term> terms;
        if (!Terms(instruction, x.AsSpan(), Kept(const_cast<MirInstruction *>(&instruction)), terms))
        {
            return;
        }
        for (const Term &term : terms)
        {
            MirValue *operand = instruction.GetOperand(term.Operand);
            if (_varied.Contains(operand))
            {
                Accumulate(operand, Transpose(term, a, x[term.Operand]));
            }
        }
    }

    /// Propagates the adjoint *a* of the result of *call* through the `[BackwardDerivative]` of its callee: it gets a
    /// pair with a zero derivative for each differentiable argument, by reference, and writes the argument's
    /// gradient into it.
    void ReverseCall(const MirInstruction &call, MirValue *a)
    {
        const MirFunction *callee = CalleeOf(call);
        MirFunction *derivative = callee->BackwardDerivative;
        const Span<MirArgument *const> parameters = derivative->GetArguments();
        List<MirValue *> arguments;
        List<MirValue *> pairs;
        for (usize i = 1; i < call.GetOperands().Count(); ++i)
        {
            MirValue *operand = call.GetOperand(i);
            MirValue *primal = Kept(operand);
            if (!TakesPair(*callee, i - 1))
            {
                arguments.Add(primal);
                pairs.Add(nullptr);
                continue;
            }
            const MirType *pairType = parameters[i - 1]->GetType()->GetElement();
            MirInstruction *pair = _builder.Variable(pairType, "pair");
            MirValue *parts[] = {primal, Splat(operand->GetType(), 0.0)};
            _builder.Store(pair, _builder.Construct(pairType, parts));
            arguments.Add(pair);
            pairs.Add(pair);
        }
        arguments.Add(a);
        _builder.Call(derivative, arguments.AsSpan());
        for (usize i = 0; i < pairs.Count(); ++i)
        {
            if (pairs[i] != nullptr && _varied.Contains(call.GetOperand(i + 1)))
            {
                const uint32 differential[] = {1};
                Accumulate(call.GetOperand(i + 1),
                           ClearNoDiff(_builder.Extract(_builder.Load(pairs[i]), differential)));
            }
        }
    }

    /// Propagates the adjoint *a* of an operation that only moves values, but not out of `[NoDiff]` fields; false
    /// for other operations.
    bool ReverseStructural(const MirInstruction &source, MirValue *a)
    {
        auto operand = [&](usize index) { return source.GetOperand(index); };
        auto zero = [&](usize index) { return Splat(operand(index)->GetType(), 0.0); };
        switch (source.GetOp())
        {
        case MirOp::CompositeConstruct:
        {
            const MirType *type = source.GetType();
            uint32 component = 0;
            for (usize i = 0; i < source.GetOperands().Count(); ++i)
            {
                const MirType *partType = operand(i)->GetType();
                if (type->Is(MirTypeKind::Vector) && partType->Is(MirTypeKind::Vector))
                {
                    List<uint32> components;
                    for (uint32 c = 0; c < partType->GetCount(); ++c)
                    {
                        components.Add(component + c);
                    }
                    Accumulate(operand(i), _builder.Shuffle(a, a, components.AsSpan()));
                    component += partType->GetCount();
                    continue;
                }
                const uint32 index[] = {type->Is(MirTypeKind::Vector) ? component : static_cast<uint32>(i)};
                if (!(type->Is(MirTypeKind::Struct) && type->GetFields()[i].NoDiff))
                {
                    Accumulate(operand(i), _builder.Extract(a, index));
                }
                ++component;
            }
            return true;
        }
        case MirOp::CompositeExtract:
            if (!IsNoDiffPath(operand(0)->GetType(), source.GetLiterals()))
            {
                Accumulate(operand(0), _builder.Insert(zero(0), a, Indices(source)));
            }
            return true;
        case MirOp::CompositeInsert:
            if (!IsNoDiffPath(operand(0)->GetType(), source.GetLiterals()))
            {
                Accumulate(operand(1), _builder.Extract(a, Indices(source)));
            }
            Accumulate(operand(0), _builder.Insert(a, zero(1), Indices(source)));
            return true;
        case MirOp::VectorShuffle:
            for (usize k = 0; k < 2; ++k)
            {
                if (!_varied.Contains(operand(k)))
                {
                    continue;
                }
                const MirType *type = operand(k)->GetType();
                const uint32 offset = k == 0 ? 0 : operand(0)->GetType()->GetCount();
                List<MirValue *> parts;
                for (uint32 c = 0; c < type->GetCount(); ++c)
                {
                    MirValue *sum = nullptr;
                    for (usize j = 0; j < source.GetLiterals().Count(); ++j)
                    {
                        if (source.GetLiterals()[j] == static_cast<int64>(offset + c))
                        {
                            const uint32 index[] = {static_cast<uint32>(j)};
                            sum = Add(sum, source.GetLiterals().Count() == 1 ? a : _builder.Extract(a, index));
                        }
                    }
                    parts.Add(sum != nullptr ? sum : Splat(type->GetElement(), 0.0));
                }
                Accumulate(operand(k), _builder.Construct(type, parts.AsSpan()));
            }
            return true;
        case MirOp::Select:
        {
            MirValue *condition = Kept(operand(0));
            Accumulate(operand(1), _builder.Select(condition, a, zero(1)));
            Accumulate(operand(2), _builder.Select(condition, zero(2), a));
            return true;
        }
        case MirOp::VectorExtractDynamic:
            Accumulate(operand(0), _builder.InsertDynamic(zero(0), a, Kept(operand(1))));
            return true;
        case MirOp::VectorInsertDynamic:
        {
            MirValue *index = Kept(operand(2));
            Accumulate(operand(1), _builder.ExtractDynamic(a, index));
            Accumulate(operand(0), _builder.InsertDynamic(a, zero(1), index));
            return true;
        }
        default:
            return false;
        }
    }

    MirModule *_module;
    MirTypeContext *_types;
    MirBuilder _builder;
    MirFunction *_source;
    MirFunction *_result = nullptr;
    uint32 _pairs;
    uint32 _gradients;
    bool _forward;
    bool _failed = false;
    UniquePtr<MirControlFlow> _flow;
    HashSet<const MirValue *> _varied;
    HashMap<const MirValue *, MirValue *> _primal;
    /// The forward-mode directions carried at once, and the tangents of each; _direction picks the one Tangent and
    /// SetTangent use.
    static constexpr uint32 MaxTangents = 2;
    uint32 _tangentCount = 1;
    uint32 _direction = 0;
    HashMap<const MirValue *, MirValue *> _tangents[MaxTangents];
    HashMap<const MirValue *, MirValue *> _adjoints;
    /// The variables values are kept in, by how many loops the sweep is in where they are read.
    List<HashMap<const MirValue *, MirInstruction *>> _kept;
    List<MirInstruction *> _tapes;
    HashMap<const MirBlock *, MirBlock *> _blocks;
    HashMap<const MirBlock *, uint32> _depths;
    /// The innermost loop each block of a loop is in.
    HashMap<const MirBlock *, Loop *> _blockLoops;
    List<UniquePtr<Loop>> _loops;
    /// The loops the reverse sweep is in, outermost first.
    List<Loop *> _active;
    /// The merge and continue blocks of the loops being parsed, which only the end of their body branches to.
    List<const MirBlock *> _exits;
    List<const MirInstruction *> _phis;
    List<uint32> _indices;
    MirValue *_returned = nullptr;
    MirBlock *_returnBlock = nullptr;
    MirInstruction *_initialize = nullptr;
    List<Item> _items;
};

/// A derivative function already generated, which every call asking for the same one reuses.
struct Generated
{
    MirFunction *Source = nullptr;
    bool Forward = true;
    uint32 Pairs = 0;
    uint32 Gradients = 0;
    uint32 Tangents = 1;
    MirFunction *Function = nullptr;
};
} // namespace

MirPreservedAnalyses MirAutodiffPass::Run(MirModule &module, MirModuleAnalysisManager &analyses)
{
    List<MirInstruction *> derivatives;
    bool changed = false;
    for (MirFunction *function : module.GetFunctions())
    {
        changed = changed || function->ForwardDerivative != nullptr || function->ForwardDerivative2 != nullptr ||
                  function->BackwardDerivative != nullptr;
        for (MirBlock *block : function->GetBlocks())
        {
            for (MirInstruction *instruction : *block)
            {
                if (instruction->GetOp() == MirOp::Intrinsic &&
                    (instruction->GetIntrinsic() == MirIntrinsic::ForwardDerivative ||
                     instruction->GetIntrinsic() == MirIntrinsic::BackwardDerivative))
                {
                    derivatives.Add(instruction);
                }
                changed = changed || IsDetach(*instruction);
            }
        }
    }
    if (derivatives.IsEmpty() && !changed)
    {
        return MirPreservedAnalyses::All();
    }
    List<Generated> generated;
    MirBuilder builder(module);
    for (MirInstruction *derivative : derivatives)
    {
        MARLIN_ASSERT(!derivative->GetOperands().IsEmpty() &&
                      derivative->GetOperand(0)->GetValueKind() == MirValueKind::Function);
        auto *source = static_cast<MirFunction *>(derivative->GetOperand(0));
        const bool forward = derivative->GetIntrinsic() == MirIntrinsic::ForwardDerivative;
        const Span<const int64> literals = derivative->GetLiterals();
        // RECOVERY-DECISION lang-ir-D1: literals are PairMask then RefMask, the Differentiator's parameter order;
        // then the forward directions, one when absent.
        const uint32 pairs = !literals.IsEmpty() ? static_cast<uint32>(literals[0]) : 0;
        const uint32 gradients = literals.Count() > 1 ? static_cast<uint32>(literals[1]) : 0;
        const uint32 tangents = literals.Count() > 2 ? static_cast<uint32>(literals[2]) : 1;
        const Span<MirValue *const> arguments = derivative->GetOperands().Slice(1);
        MirFunction *function = nullptr;
        for (const Generated &existing : generated)
        {
            if (existing.Source == source && existing.Forward == forward && existing.Pairs == pairs &&
                existing.Gradients == gradients && existing.Tangents == tangents)
            {
                function = existing.Function;
            }
        }
        if (function == nullptr)
        {
            function = Differentiator(module, *source, pairs, gradients, forward, tangents)
                           .Run(arguments, derivative->GetType());
            if (function == nullptr)
            {
                continue;
            }
            generated.Add({source, forward, pairs, gradients, tangents, function});
        }
        analyses.GetFunctionAnalyses().Clear(*derivative->GetParent()->GetParent());
        builder.SetInsertPoint(derivative);
        builder.SetLocation(derivative->Location);
        MirInstruction *call = builder.Call(function, arguments);
        if (!derivative->GetType()->Is(MirTypeKind::Void))
        {
            derivative->ReplaceAllUsesWith(call);
        }
        derivative->Erase();
    }
    // Differentiation is done: `Detach` is its operand again, and the functions with custom derivatives can be
    // inlined like any other, after which the inline pass drops the derivatives nothing calls.
    List<MirInstruction *> detached;
    for (MirFunction *function : module.GetFunctions())
    {
        function->ForwardDerivative = nullptr;
        function->ForwardDerivative2 = nullptr;
        function->BackwardDerivative = nullptr;
        for (MirBlock *block : function->GetBlocks())
        {
            for (MirInstruction *instruction : *block)
            {
                if (IsDetach(*instruction))
                {
                    detached.Add(instruction);
                }
            }
        }
    }
    for (MirInstruction *detach : detached)
    {
        analyses.GetFunctionAnalyses().Clear(*detach->GetParent()->GetParent());
        detach->ReplaceAllUsesWith(detach->GetOperand(0));
        detach->Erase();
    }
    return MirPreservedAnalyses::None();
}
} // namespace Marlin
