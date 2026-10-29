#include "Sema/Binder.h"

namespace Marlin
{
namespace
{
enum class Flow : uint8
{
    Normal,
    Break,
    Continue,
    Return,
    Failed,
};

/// Interprets bound trees whose inputs are constants: folding and `const` function calls (§6).
class Evaluator
{
public:
    Evaluator(Sema &sema, bool report) : _sema(&sema), _report(report), _limit(sema.Options->EvaluationLimit)
    {
        _frames.AddDefaulted();
    }

    SemaConstant *Evaluate(BoundExpr *expression)
    {
        if (++_steps > _limit)
        {
            if (!_limitReported)
            {
                _limitReported = true;
                _sema->Diagnostics.Error(DiagnosticCode::EvaluationLimit, expression->Range,
                                         "compile-time evaluation took more than {0} steps", _limit)
                    .WithHelp("this usually means unbounded recursion in a `const` function");
            }
            return nullptr;
        }
        switch (expression->Kind)
        {
        case BoundKind::Constant:
            return expression->As<BoundConstant>()->Value;
        case BoundKind::Zero:
            return expression->Type->IsError() ? nullptr : _sema->NewZero(expression->Type);
        case BoundKind::Local:
        {
            SemaConstant **value = _frames.Last().Find(expression->As<BoundLocal>()->Local);
            return value != nullptr ? *value : NotConstant(expression);
        }
        case BoundKind::Field:
        {
            const auto *field = expression->As<BoundField>();
            SemaConstant *object = Evaluate(field->Object);
            return object != nullptr ? ElementOf(object, field->FieldIndex, expression->Type) : nullptr;
        }
        case BoundKind::Swizzle:
        {
            const auto *swizzle = expression->As<BoundSwizzle>();
            SemaConstant *object = Evaluate(swizzle->Object);
            if (object == nullptr)
            {
                return nullptr;
            }
            const SemaType *scalar = expression->Type->GetScalar();
            if (swizzle->Object->Type->IsScalar())
            {
                return swizzle->Count == 1 ? object : Splat(object, expression->Type);
            }
            if (swizzle->Count == 1)
            {
                return ElementOf(object, swizzle->Components[0], scalar);
            }
            List<SemaConstant *> elements;
            for (uint8 i = 0; i < swizzle->Count; ++i)
            {
                elements.Add(ElementOf(object, swizzle->Components[i], scalar));
            }
            return _sema->NewComposite(expression->Type, elements.AsSpan());
        }
        case BoundKind::Index:
        {
            const auto *index = expression->As<BoundIndex>();
            SemaConstant *object = Evaluate(index->Object);
            SemaConstant *position = object != nullptr ? Evaluate(index->Index) : nullptr;
            if (position == nullptr)
            {
                return nullptr;
            }
            const uint32 count = index->Object->Type->Is(SemaTypeKind::Matrix) ? index->Object->Type->GetRows()
                                                                                : index->Object->Type->GetCount();
            if ((position->Type->IsSigned() && position->GetInt() < 0) || position->Bits >= count)
            {
                if (_report)
                {
                    _sema->Diagnostics.Error(DiagnosticCode::IndexOutOfRange, index->Index->Range,
                                             "the index {0} is out of range", position->GetInt());
                }
                return nullptr;
            }
            return ElementOf(object, static_cast<uint32>(position->Bits), expression->Type);
        }
        case BoundKind::Construct:
            return Construct(expression->As<BoundConstruct>());
        case BoundKind::Convert:
        {
            SemaConstant *operand = Evaluate(expression->As<BoundConvert>()->Operand);
            return operand != nullptr ? _sema->ConvertConstant(operand, expression->Type) : nullptr;
        }
        case BoundKind::Unary:
        {
            const auto *unary = expression->As<BoundUnary>();
            SemaConstant *operand = Evaluate(unary->Operand);
            return operand != nullptr ? Map(expression->Type, operand, nullptr,
                                            [&](const SemaType *type, SemaConstant *a, SemaConstant *) {
                                                return Unary(unary->Op, type, a);
                                            })
                                      : nullptr;
        }
        case BoundKind::Binary:
            return Binary(expression->As<BoundBinary>());
        case BoundKind::ShortCircuit:
        {
            const auto *node = expression->As<BoundShortCircuit>();
            SemaConstant *left = Evaluate(node->Left);
            if (left == nullptr)
            {
                return nullptr;
            }
            const bool value = !left->IsZero && left->Bits != 0;
            if (value != node->IsAnd)
            {
                return _sema->NewBool(value);
            }
            return Evaluate(node->Right);
        }
        case BoundKind::Conditional:
        {
            const auto *node = expression->As<BoundConditional>();
            SemaConstant *condition = Evaluate(node->Condition);
            if (condition == nullptr)
            {
                return nullptr;
            }
            return Evaluate(!condition->IsZero && condition->Bits != 0 ? node->WhenTrue : node->WhenFalse);
        }
        case BoundKind::Sequence:
        {
            const auto *node = expression->As<BoundSequence>();
            SemaConstant *initial = Evaluate(node->Initial);
            if (initial == nullptr)
            {
                return nullptr;
            }
            _frames.Last().Set(node->Temporary, initial);
            for (BoundExpr *effect : node->Effects)
            {
                if (Evaluate(effect) == nullptr)
                {
                    return nullptr;
                }
            }
            return *_frames.Last().Find(node->Temporary);
        }
        case BoundKind::Assign:
        {
            const auto *node = expression->As<BoundAssign>();
            SemaConstant *value = Evaluate(node->Value);
            if (value == nullptr || (_frames.Count() == 1 && !IsTemporaryTarget(node->Target)))
            {
                return value == nullptr ? nullptr : NotConstant(expression);
            }
            if (node->IsCompound)
            {
                SemaConstant *current = Evaluate(node->Target);
                SemaConstant *operand = current != nullptr ? _sema->ConvertConstant(current, node->OperationType) : nullptr;
                if (operand == nullptr)
                {
                    return nullptr;
                }
                value = Map(node->OperationType, operand, value, [&](const SemaType *type, SemaConstant *a, SemaConstant *b) {
                    return BinaryScalar(node->Op, type, a, b, node->Range);
                });
                value = value != nullptr ? _sema->ConvertConstant(value, node->Target->Type) : nullptr;
            }
            return value != nullptr && Store(node->Target, value) ? value : nullptr;
        }
        case BoundKind::IncDec:
        {
            const auto *node = expression->As<BoundIncDec>();
            SemaConstant *current = Evaluate(node->Target);
            if (current == nullptr || _frames.Count() == 1)
            {
                return current == nullptr ? nullptr : NotConstant(expression);
            }
            const SemaType *scalar = node->Type->GetScalar();
            SemaConstant *one = scalar->Is(SemaTypeKind::Float) ? _sema->NewFloat(scalar, 1) : _sema->NewScalar(scalar, 1);
            SemaConstant *next = Map(node->Type, current, Splat(one, node->Type),
                                     [&](const SemaType *type, SemaConstant *a, SemaConstant *b) {
                                         return BinaryScalar(node->Increment ? MirOp::Add : MirOp::Sub, type, a, b,
                                                             node->Range);
                                     });
            if (next == nullptr || !Store(node->Target, next))
            {
                return nullptr;
            }
            return node->Postfix ? current : next;
        }
        case BoundKind::Call:
            return Call(expression->As<BoundCall>());
        default:
            return NotConstant(expression);
        }
    }

private:
    SemaConstant *NotConstant(BoundExpr *expression)
    {
        if (_report && !_reported)
        {
            _reported = true;
            _sema->Diagnostics.Error(DiagnosticCode::NotConstant, expression->Range, "this is not a constant expression")
                .WithLabel("computed at run time")
                .WithNote("constants are literals, `const` values, constant generic arguments and `const` function calls "
                          "on them (§6.1)");
        }
        return nullptr;
    }

    static bool IsTemporaryTarget(const BoundExpr *target)
    {
        while (target->Kind == BoundKind::Field || target->Kind == BoundKind::Index || target->Kind == BoundKind::Swizzle)
        {
            target = target->Kind == BoundKind::Field   ? target->As<BoundField>()->Object
                     : target->Kind == BoundKind::Index ? target->As<BoundIndex>()->Object
                                                        : target->As<BoundSwizzle>()->Object;
        }
        return target->Kind == BoundKind::Local;
    }

    /// Element *index* of a composite constant, the zero element of a zero constant.
    SemaConstant *ElementOf(SemaConstant *composite, uint32 index, const SemaType *elementType)
    {
        if (composite->IsZero)
        {
            return _sema->NewZero(elementType);
        }
        return index < composite->Elements.Count() ? composite->Elements[index] : nullptr;
    }

    SemaConstant *Splat(SemaConstant *scalar, const SemaType *type)
    {
        return _sema->ConvertConstant(scalar, type);
    }

    /// The scalars of a constant vector, matrix or scalar, zero expanded.
    void Flatten(SemaConstant *value, const SemaType *scalarType, List<SemaConstant *> &scalars)
    {
        const SemaType *type = value->Type;
        if (type->IsScalar())
        {
            scalars.Add(value->IsZero ? _sema->NewZero(type) : value);
            return;
        }
        const uint32 count = type->Is(SemaTypeKind::Matrix) ? type->GetRows() : type->GetCount();
        for (uint32 i = 0; i < count; ++i)
        {
            const SemaType *elementType = type->Is(SemaTypeKind::Matrix) ? _sema->Types.GetVector(scalarType, type->GetCount())
                                                                         : scalarType;
            Flatten(ElementOf(value, i, elementType), scalarType, scalars);
        }
    }

    SemaConstant *Construct(const BoundConstruct *node)
    {
        List<SemaConstant *> elements;
        for (BoundExpr *element : node->Elements)
        {
            SemaConstant *value = Evaluate(element);
            if (value == nullptr)
            {
                return nullptr;
            }
            elements.Add(value);
        }
        if (node->Type->Is(SemaTypeKind::Vector))
        {
            List<SemaConstant *> scalars;
            for (SemaConstant *element : elements)
            {
                Flatten(element, node->Type->GetElement(), scalars);
            }
            return _sema->NewComposite(node->Type, scalars.AsSpan());
        }
        return _sema->NewComposite(node->Type, elements.AsSpan());
    }

    template <typename TOp>
    SemaConstant *Map(const SemaType *type, SemaConstant *a, SemaConstant *b, TOp op)
    {
        if (type->IsScalar() || type->Is(SemaTypeKind::Enum))
        {
            SemaConstant *left = a->IsZero ? Zeroed(type) : a;
            SemaConstant *right = b == nullptr ? nullptr : (b->IsZero ? Zeroed(b->Type) : b);
            return op(type, left, right);
        }
        if (!type->Is(SemaTypeKind::Vector) && !type->Is(SemaTypeKind::Matrix))
        {
            return nullptr;
        }
        const uint32 count = type->Is(SemaTypeKind::Matrix) ? type->GetRows() : type->GetCount();
        const SemaType *elementType = type->Is(SemaTypeKind::Matrix)
                                          ? _sema->Types.GetVector(type->GetScalar(), type->GetCount())
                                          : type->GetElement();
        List<SemaConstant *> elements;
        for (uint32 i = 0; i < count; ++i)
        {
            SemaConstant *left = ElementOf(a, i, elementType);
            SemaConstant *right = b == nullptr ? nullptr
                                  : b->Type->IsScalar() ? b
                                                        : ElementOf(b, i, b->Type->Is(SemaTypeKind::Matrix)
                                                                              ? _sema->Types.GetVector(b->Type->GetScalar(),
                                                                                                       b->Type->GetCount())
                                                                              : b->Type->GetElement());
            SemaConstant *element = Map(elementType, left, right, op);
            if (element == nullptr)
            {
                return nullptr;
            }
            elements.Add(element);
        }
        return _sema->NewComposite(type, elements.AsSpan());
    }

    SemaConstant *Zeroed(const SemaType *type)
    {
        if (type->Is(SemaTypeKind::Float))
        {
            return _sema->NewFloat(type, 0);
        }
        return _sema->NewScalar(type, 0);
    }

    SemaConstant *Unary(MirOp op, const SemaType *type, SemaConstant *a)
    {
        switch (op)
        {
        case MirOp::Negate:
            return type->Is(SemaTypeKind::Float) ? _sema->NewFloat(type, -a->Float) : _sema->NewScalar(type, ~a->Bits + 1);
        case MirOp::BitNot:
            return _sema->NewScalar(type, ~a->Bits);
        case MirOp::LogicalNot:
            return _sema->NewBool(a->Bits == 0);
        default:
            return nullptr;
        }
    }

    SemaConstant *BinaryScalar(MirOp op, const SemaType *type, SemaConstant *a, SemaConstant *b, SourceRange range)
    {
        const SemaType *operandType = a->Type->Is(SemaTypeKind::Enum) ? a->Type->GetElement() : a->Type;
        const bool isFloat = operandType->Is(SemaTypeKind::Float);
        const bool isSigned = operandType->IsSigned();
        if (isFloat)
        {
            const double x = a->Float;
            const double y = b->Float;
            switch (op)
            {
            case MirOp::Add:
                return _sema->NewFloat(type, x + y);
            case MirOp::Sub:
                return _sema->NewFloat(type, x - y);
            case MirOp::Mul:
                return _sema->NewFloat(type, x * y);
            case MirOp::Div:
                return _sema->NewFloat(type, x / y);
            case MirOp::Rem:
                return _sema->NewFloat(type, __builtin_fmod(x, y));
            case MirOp::Equal:
                return _sema->NewBool(x == y);
            case MirOp::NotEqual:
                return _sema->NewBool(x != y);
            case MirOp::Less:
                return _sema->NewBool(x < y);
            case MirOp::LessEqual:
                return _sema->NewBool(x <= y);
            case MirOp::Greater:
                return _sema->NewBool(x > y);
            case MirOp::GreaterEqual:
                return _sema->NewBool(x >= y);
            default:
                return nullptr;
            }
        }
        const uint64 x = a->Bits;
        const uint64 y = b->Bits;
        const auto sx = static_cast<int64>(x);
        const auto sy = static_cast<int64>(y);
        const uint32 width = operandType->Is(SemaTypeKind::Bool) ? 1 : operandType->GetWidth();
        switch (op)
        {
        case MirOp::Add:
            return _sema->NewScalar(type, x + y);
        case MirOp::Sub:
            return _sema->NewScalar(type, x - y);
        case MirOp::Mul:
            return _sema->NewScalar(type, x * y);
        case MirOp::Div:
        case MirOp::Rem:
            if (y == 0)
            {
                if (_report)
                {
                    _sema->Diagnostics.Error(DiagnosticCode::DivisionByZero, range, "division by zero in a constant");
                }
                return nullptr;
            }
            if (op == MirOp::Div)
            {
                return _sema->NewScalar(type, isSigned ? static_cast<uint64>(sx / sy) : x / y);
            }
            return _sema->NewScalar(type, isSigned ? static_cast<uint64>(sx % sy) : x % y);
        case MirOp::BitAnd:
            return _sema->NewScalar(type, x & y);
        case MirOp::BitOr:
            return _sema->NewScalar(type, x | y);
        case MirOp::BitXor:
            return _sema->NewScalar(type, x ^ y);
        case MirOp::ShiftLeft:
            return _sema->NewScalar(type, x << (y % width));
        case MirOp::ShiftRight:
            return _sema->NewScalar(type, isSigned ? static_cast<uint64>(sx >> (y % width))
                                                   : (x & (width >= 64 ? ~uint64{0} : (uint64{1} << width) - 1)) >>
                                                         (y % width));
        case MirOp::LogicalAnd:
            return _sema->NewBool(x != 0 && y != 0);
        case MirOp::LogicalOr:
            return _sema->NewBool(x != 0 || y != 0);
        case MirOp::Equal:
            return _sema->NewBool(x == y);
        case MirOp::NotEqual:
            return _sema->NewBool(x != y);
        case MirOp::Less:
            return _sema->NewBool(isSigned ? sx < sy : x < y);
        case MirOp::LessEqual:
            return _sema->NewBool(isSigned ? sx <= sy : x <= y);
        case MirOp::Greater:
            return _sema->NewBool(isSigned ? sx > sy : x > y);
        case MirOp::GreaterEqual:
            return _sema->NewBool(isSigned ? sx >= sy : x >= y);
        default:
            return nullptr;
        }
    }

    SemaConstant *Binary(const BoundBinary *node)
    {
        SemaConstant *left = Evaluate(node->Left);
        SemaConstant *right = left != nullptr ? Evaluate(node->Right) : nullptr;
        if (right == nullptr)
        {
            return nullptr;
        }
        switch (node->Op)
        {
        case MirOp::MatrixTimesVector:
        case MirOp::VectorTimesMatrix:
        case MirOp::MatrixTimesMatrix:
        case MirOp::MatrixTimesScalar:
            return MatrixProduct(node, left, right);
        default:
            break;
        }
        const SemaType *operandType = node->Left->Type;
        const bool comparison = node->Op >= MirOp::Equal && node->Op <= MirOp::GreaterEqual;
        if (comparison)
        {
            return Map(operandType, left, right, [&](const SemaType *type, SemaConstant *a, SemaConstant *b) {
                (void)type;
                return BinaryScalar(node->Op, _sema->Types.GetBool(), a, b, node->Range);
            });
        }
        return Map(node->Type, left, right, [&](const SemaType *type, SemaConstant *a, SemaConstant *b) {
            return BinaryScalar(node->Op, type, a, b, node->Range);
        });
    }

    SemaConstant *MatrixProduct(const BoundBinary *node, SemaConstant *left, SemaConstant *right)
    {
        const SemaType *scalar = node->Type->GetScalar();
        List<SemaConstant *> a;
        List<SemaConstant *> b;
        Flatten(left, scalar, a);
        Flatten(right, scalar, b);
        auto multiplyAdd = [&](SemaConstant *sum, SemaConstant *x, SemaConstant *y) {
            SemaConstant *product = BinaryScalar(MirOp::Mul, scalar, x, y, node->Range);
            return sum == nullptr ? product : BinaryScalar(MirOp::Add, scalar, sum, product, node->Range);
        };
        List<SemaConstant *> result;
        const SemaType *leftType = node->Left->Type;
        const SemaType *rightType = node->Right->Type;
        switch (node->Op)
        {
        case MirOp::MatrixTimesScalar:
            for (SemaConstant *element : a)
            {
                result.Add(BinaryScalar(MirOp::Mul, scalar, element, b[0], node->Range));
            }
            break;
        case MirOp::MatrixTimesVector:
            for (uint32 row = 0; row < leftType->GetRows(); ++row)
            {
                SemaConstant *sum = nullptr;
                for (uint32 column = 0; column < leftType->GetCount(); ++column)
                {
                    sum = multiplyAdd(sum, a[row * leftType->GetCount() + column], b[column]);
                }
                result.Add(sum);
            }
            return _sema->NewComposite(node->Type, result.AsSpan());
        case MirOp::VectorTimesMatrix:
            for (uint32 column = 0; column < rightType->GetCount(); ++column)
            {
                SemaConstant *sum = nullptr;
                for (uint32 row = 0; row < rightType->GetRows(); ++row)
                {
                    sum = multiplyAdd(sum, a[row], b[row * rightType->GetCount() + column]);
                }
                result.Add(sum);
            }
            return _sema->NewComposite(node->Type, result.AsSpan());
        default:
            for (uint32 row = 0; row < leftType->GetRows(); ++row)
            {
                for (uint32 column = 0; column < rightType->GetCount(); ++column)
                {
                    SemaConstant *sum = nullptr;
                    for (uint32 k = 0; k < leftType->GetCount(); ++k)
                    {
                        sum = multiplyAdd(sum, a[row * leftType->GetCount() + k], b[k * rightType->GetCount() + column]);
                    }
                    result.Add(sum);
                }
            }
            break;
        }
        const SemaType *rowType = _sema->Types.GetVector(scalar, node->Type->GetCount());
        List<SemaConstant *> rows;
        for (uint32 row = 0; row < node->Type->GetRows(); ++row)
        {
            rows.Add(_sema->NewComposite(rowType, Span<SemaConstant *const>(result.Data() + row * node->Type->GetCount(),
                                                                            node->Type->GetCount())));
        }
        return _sema->NewComposite(node->Type, rows.AsSpan());
    }

    /// Writes *value* to the storage *target* names in the current frame.
    bool Store(BoundExpr *target, SemaConstant *value)
    {
        switch (target->Kind)
        {
        case BoundKind::Local:
            _frames.Last().Set(target->As<BoundLocal>()->Local, value);
            return true;
        case BoundKind::Field:
        case BoundKind::Index:
        case BoundKind::Swizzle:
        {
            BoundExpr *objectExpression = target->Kind == BoundKind::Field   ? target->As<BoundField>()->Object
                                          : target->Kind == BoundKind::Index ? target->As<BoundIndex>()->Object
                                                                             : target->As<BoundSwizzle>()->Object;
            SemaConstant *object = Evaluate(objectExpression);
            if (object == nullptr)
            {
                return false;
            }
            const SemaType *type = objectExpression->Type;
            uint32 count = 0;
            if (type->Is(SemaTypeKind::Struct))
            {
                count = static_cast<uint32>(type->GetStruct()->Fields.Count());
            }
            else
            {
                count = type->Is(SemaTypeKind::Matrix) ? type->GetRows() : type->GetCount();
            }
            List<SemaConstant *> elements;
            for (uint32 i = 0; i < count; ++i)
            {
                const SemaType *elementType = type->Is(SemaTypeKind::Struct) ? type->GetStruct()->Fields[i].Type
                                              : type->Is(SemaTypeKind::Matrix)
                                                  ? _sema->Types.GetVector(type->GetScalar(), type->GetCount())
                                                  : type->GetElement();
                elements.Add(ElementOf(object, i, elementType));
            }
            if (target->Kind == BoundKind::Field)
            {
                elements[target->As<BoundField>()->FieldIndex] = value;
            }
            else if (target->Kind == BoundKind::Index)
            {
                SemaConstant *position = Evaluate(target->As<BoundIndex>()->Index);
                if (position == nullptr || position->Bits >= count)
                {
                    return false;
                }
                elements[static_cast<uint32>(position->Bits)] = value;
            }
            else
            {
                const auto *swizzle = target->As<BoundSwizzle>();
                for (uint8 i = 0; i < swizzle->Count; ++i)
                {
                    elements[swizzle->Components[i]] =
                        swizzle->Count == 1 ? value : ElementOf(value, i, type->GetElement());
                }
            }
            return Store(objectExpression, _sema->NewComposite(type, elements.AsSpan()));
        }
        default:
            return false;
        }
    }

    SemaConstant *Call(const BoundCall *call)
    {
        SemaFunction *function = call->Function;
        if (!function->IsConstFunction)
        {
            return NotConstant(const_cast<BoundCall *>(call));
        }
        List<SemaConstant *> arguments;
        for (BoundExpr *argument : call->Arguments)
        {
            SemaConstant *value = Evaluate(argument);
            if (value == nullptr)
            {
                return nullptr;
            }
            arguments.Add(value);
        }
        if (function->State == SemaFunctionState::Binding || !_sema->BindBody(function) || function->Body == nullptr)
        {
            return nullptr;
        }
        _frames.AddDefaulted();
        for (usize i = 0; i < function->Parameters.Count() && i < arguments.Count(); ++i)
        {
            _frames.Last().Set(function->Parameters[i], arguments[i]);
        }
        _returned = nullptr;
        const Flow flow = Execute(function->Body);
        SemaConstant *result = flow == Flow::Return ? _returned : nullptr;
        _frames.Pop();
        if (result == nullptr && flow != Flow::Failed && !function->ReturnType->Is(SemaTypeKind::Void))
        {
            return NotConstant(const_cast<BoundCall *>(call));
        }
        return result;
    }

    Flow Execute(BoundStmt *statement)
    {
        if (statement == nullptr)
        {
            return Flow::Normal;
        }
        if (++_steps > _limit)
        {
            return Flow::Failed;
        }
        switch (statement->Kind)
        {
        case BoundStmtKind::Block:
            for (BoundStmt *inner : statement->As<BoundBlock>()->Statements)
            {
                const Flow flow = Execute(inner);
                if (flow != Flow::Normal)
                {
                    return flow;
                }
            }
            return Flow::Normal;
        case BoundStmtKind::Local:
        {
            const auto *local = statement->As<BoundLocalStmt>();
            SemaConstant *value = local->Initializer != nullptr ? Evaluate(local->Initializer)
                                                                : _sema->NewZero(local->Local->Type);
            if (value == nullptr)
            {
                return Flow::Failed;
            }
            _frames.Last().Set(local->Local, value);
            return Flow::Normal;
        }
        case BoundStmtKind::Expression:
            return Evaluate(statement->As<BoundExprStmt>()->Expression) != nullptr ? Flow::Normal : Flow::Failed;
        case BoundStmtKind::If:
        {
            const auto *branch = statement->As<BoundIf>();
            SemaConstant *condition = Evaluate(branch->Condition);
            if (condition == nullptr)
            {
                return Flow::Failed;
            }
            return Execute(!condition->IsZero && condition->Bits != 0 ? branch->Then : branch->Else);
        }
        case BoundStmtKind::Loop:
        {
            const auto *loop = statement->As<BoundLoop>();
            for (BoundStmt *initializer : loop->Initializers)
            {
                if (Execute(initializer) == Flow::Failed)
                {
                    return Flow::Failed;
                }
            }
            bool first = true;
            while (true)
            {
                if (loop->Condition != nullptr && (loop->TestFirst || !first))
                {
                    SemaConstant *condition = Evaluate(loop->Condition);
                    if (condition == nullptr)
                    {
                        return Flow::Failed;
                    }
                    if (condition->IsZero || condition->Bits == 0)
                    {
                        return Flow::Normal;
                    }
                }
                first = false;
                const Flow flow = Execute(loop->Body);
                if (flow == Flow::Break)
                {
                    return Flow::Normal;
                }
                if (flow == Flow::Return || flow == Flow::Failed)
                {
                    return flow;
                }
                for (BoundExpr *increment : loop->Increments)
                {
                    if (Evaluate(increment) == nullptr)
                    {
                        return Flow::Failed;
                    }
                }
            }
        }
        case BoundStmtKind::Switch:
        {
            const auto *node = statement->As<BoundSwitch>();
            SemaConstant *selector = Evaluate(node->Selector);
            if (selector == nullptr)
            {
                return Flow::Failed;
            }
            const BoundSwitchSection *chosen = nullptr;
            for (const BoundSwitchSection &section : node->Sections)
            {
                for (const int64 value : section.Values)
                {
                    chosen = chosen == nullptr && value == selector->GetInt() ? &section : chosen;
                }
            }
            for (const BoundSwitchSection &section : node->Sections)
            {
                chosen = chosen == nullptr && section.IsDefault ? &section : chosen;
            }
            if (chosen == nullptr)
            {
                return Flow::Normal;
            }
            for (BoundStmt *inner : chosen->Statements)
            {
                const Flow flow = Execute(inner);
                if (flow == Flow::Break)
                {
                    return Flow::Normal;
                }
                if (flow != Flow::Normal)
                {
                    return flow;
                }
            }
            return Flow::Normal;
        }
        case BoundStmtKind::Break:
            return Flow::Break;
        case BoundStmtKind::Continue:
            return Flow::Continue;
        case BoundStmtKind::Return:
        {
            const auto *ret = statement->As<BoundReturn>();
            if (ret->Value != nullptr)
            {
                _returned = Evaluate(ret->Value);
                if (_returned == nullptr)
                {
                    return Flow::Failed;
                }
            }
            return Flow::Return;
        }
        default:
            return Flow::Failed;
        }
    }

    Sema *_sema;
    bool _report;
    bool _reported = false;
    bool _limitReported = false;
    uint32 _limit;
    uint32 _steps = 0;
    List<HashMap<const SemaLocal *, SemaConstant *>> _frames;
    SemaConstant *_returned = nullptr;
};
} // namespace

SemaConstant *Sema::Evaluate(BoundExpr *expression, bool report)
{
    if (expression == nullptr || expression->Type->IsError())
    {
        return nullptr;
    }
    switch (expression->Kind)
    {
    case BoundKind::Constant:
        return expression->As<BoundConstant>()->Value;
    case BoundKind::Local:
    case BoundKind::Global:
    case BoundKind::BufferElement:
    case BoundKind::Intrinsic:
    case BoundKind::Bitcast:
        if (!report)
        {
            return nullptr;
        }
        break;
    default:
        break;
    }
    Evaluator evaluator(*this, report);
    return evaluator.Evaluate(expression);
}

SemaConstant *Sema::EvaluateConstantExpression(AstNode *node, const SemaContext &context, const SemaType *type)
{
    Binder binder(*this, nullptr, context);
    BoundExpr *expression = binder.BindExpression(node, type);
    if (type != nullptr)
    {
        expression = binder.ConvertImplicit(expression, type);
    }
    if (expression->Type->IsError())
    {
        return nullptr;
    }
    return Evaluate(expression, true);
}

bool Sema::HasFeature(StringView feature) const
{
    for (const String &name : Options->Features)
    {
        if (name.AsView() == feature)
        {
            return true;
        }
    }
    return false;
}

bool Sema::BindBody(SemaFunction *function)
{
    switch (function->State)
    {
    case SemaFunctionState::Bound:
        return true;
    case SemaFunctionState::Failed:
    case SemaFunctionState::Binding:
        return false;
    default:
        break;
    }
    if (!ResolveSignature(function))
    {
        function->State = SemaFunctionState::Failed;
        return false;
    }
    AstDecl *decl = function->Symbol->Decl;
    AstBody *body = nullptr;
    switch (decl->Kind)
    {
    case AstKind::FunctionDecl:
        body = &decl->As<AstFunctionDecl>()->Body;
        break;
    case AstKind::ConstructorDecl:
        body = &decl->As<AstConstructorDecl>()->Body;
        break;
    case AstKind::OperatorDecl:
        body = &decl->As<AstOperatorDecl>()->Body;
        break;
    case AstKind::PropertyDecl:
        body = function->Accessor == SemaAccessor::Setter ? &decl->As<AstPropertyDecl>()->Setter.Body
                                                          : &decl->As<AstPropertyDecl>()->Getter;
        break;
    default:
        break;
    }
    const bool automatic = IsAutoProperty(function->Symbol);
    if (!automatic && (body == nullptr || body->IsMissing()))
    {
        function->State = SemaFunctionState::Bound;
        return true;
    }
    function->State = SemaFunctionState::Binding;
    if (!automatic)
    {
        EnsureBodyParsed(function->Symbol->File, *body);
    }
    const uint32 errors = Diagnostics.GetErrorCount();
    List<SemaGenericBinding> bindings;
    GetBindings(function, bindings);
    Binder binder(*this, function, ContextOf(function->Symbol, Freeze(bindings), function->Owner));
    function->Body = automatic ? binder.BindAutoAccessor() : binder.BindFunctionBody(*body);
    if (Diagnostics.GetErrorCount() == errors)
    {
        CheckFlow(function);
    }
    if (Diagnostics.GetErrorCount() == errors)
    {
        ResolveCustomDerivatives(function);
    }
    function->State = Diagnostics.GetErrorCount() == errors ? SemaFunctionState::Bound : SemaFunctionState::Failed;
    return function->State == SemaFunctionState::Bound;
}
} // namespace Marlin
