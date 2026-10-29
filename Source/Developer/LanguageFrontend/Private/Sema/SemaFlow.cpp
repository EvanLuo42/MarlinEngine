#include "Sema/Sema.h"
#include "Sema/SemaHelpers.h"

namespace Marlin
{
namespace
{
/// What is known at a point of the function: the locals definitely assigned, in scripts the nullable locals known
/// not to be null, and in a class constructor the fields assigned; or that the point cannot be reached.
struct FlowState
{
    bool Unreachable = false;
    List<const SemaLocal *> Assigned;
    List<const SemaLocal *> NotNull;
    List<const SemaSymbol *> Fields;

    void Add(const SemaLocal *local)
    {
        if (!Assigned.Contains(local))
        {
            Assigned.Add(local);
        }
    }

    void SetNotNull(const SemaLocal *local, bool notNull)
    {
        if (notNull && !NotNull.Contains(local))
        {
            NotNull.Add(local);
        }
        else if (!notNull)
        {
            NotNull.Remove(local);
        }
    }

    void AddField(const SemaSymbol *field)
    {
        if (!Fields.Contains(field))
        {
            Fields.Add(field);
        }
    }
};

template <typename T> List<T> Intersect(const List<T> &a, const List<T> &b)
{
    List<T> result;
    for (const T &item : a)
    {
        if (b.Contains(item))
        {
            result.Add(item);
        }
    }
    return result;
}

FlowState Meet(const FlowState &a, const FlowState &b)
{
    if (a.Unreachable)
    {
        return b;
    }
    if (b.Unreachable)
    {
        return a;
    }
    FlowState result;
    result.Assigned = Intersect(a.Assigned, b.Assigned);
    result.NotNull = Intersect(a.NotNull, b.NotNull);
    result.Fields = Intersect(a.Fields, b.Fields);
    return result;
}

FlowState Unreachable()
{
    FlowState state;
    state.Unreachable = true;
    return state;
}

/// Whether *type* has a null value the flow analysis watches: `T?` (MarlinScript §3.6).
bool IsNullable(const SemaType *type)
{
    return type->Is(SemaTypeKind::Nullable);
}

/// The local *expression* reads, through conversions that keep its value.
const SemaLocal *ReadLocal(const BoundExpr *expression)
{
    while (expression->Kind == BoundKind::TypeConversion &&
           expression->As<BoundTypeConversion>()->Conversion == BoundTypeConversionKind::Upcast)
    {
        expression = expression->As<BoundTypeConversion>()->Operand;
    }
    return expression->Kind == BoundKind::Local ? expression->As<BoundLocal>()->Local : nullptr;
}

/// The local and whether `!= null` (true) or `== null` (false) when *expression* compares a local with null.
const SemaLocal *NullTest(const BoundExpr *expression, bool &notNull)
{
    if (expression->Kind == BoundKind::Let)
    {
        return NullTest(expression->As<BoundLet>()->Result, notNull);
    }
    if (expression->Kind == BoundKind::TypeTest)
    {
        notNull = true;
        return ReadLocal(expression->As<BoundTypeTest>()->Operand);
    }
    if (expression->Kind != BoundKind::Binary)
    {
        return nullptr;
    }
    const auto *binary = expression->As<BoundBinary>();
    if (binary->Op != MirOp::Equal && binary->Op != MirOp::NotEqual)
    {
        return nullptr;
    }
    const BoundExpr *other = binary->Right->Kind == BoundKind::Zero    ? binary->Left
                             : binary->Left->Kind == BoundKind::Zero ? binary->Right
                                                                       : nullptr;
    if (other == nullptr)
    {
        return nullptr;
    }
    notNull = binary->Op == MirOp::NotEqual;
    return ReadLocal(other);
}

class FlowChecker
{
public:
    FlowChecker(Sema &sema, SemaFunction &function) : _sema(&sema), _function(&function)
    {
        _script = function.Symbol->File != nullptr && function.Symbol->File->File->GetDialect() == LanguageDialect::Script;
    }

    void Run()
    {
        FlowState state;
        for (const SemaLocal *parameter : _function->Parameters)
        {
            if (parameter->Mode == SemaParameterMode::Out && !parameter->Type->Is(SemaTypeKind::MeshOutput))
            {
                _tracked.Add(parameter);
            }
        }
        CollectRequiredFields();
        Statement(_function->Body, state);
        if (!state.Unreachable)
        {
            CheckOutParameters(state, _function->Symbol->Decl->NameRange);
            CheckFields(state, _function->Symbol->Decl->NameRange);
            const bool isConstructor = _function->Symbol->Kind == SemaSymbolKind::Constructor;
            const SemaType *result = _function->Kind == SemaFunctionKind::Async && _function->ResultType != nullptr
                                         ? _function->ResultType
                                         : _function->ReturnType;
            if (!isConstructor && _function->Kind != SemaFunctionKind::Iterator && !result->Is(SemaTypeKind::Void) &&
                !result->IsError())
            {
                _sema->Diagnostics
                    .Error(DiagnosticCode::MissingReturn, _function->Symbol->Decl->NameRange,
                           "`{0}` does not return a value on every path", _function->Symbol->Name)
                    .WithLabel("control can reach the end of this function");
            }
        }
    }

private:
    [[nodiscard]] bool IsAssigned(const FlowState &state, const SemaLocal *local) const
    {
        return state.Unreachable || !_tracked.Contains(local) || state.Assigned.Contains(local);
    }

    void CheckOutParameters(const FlowState &state, SourceRange range)
    {
        for (const SemaLocal *parameter : _function->Parameters)
        {
            if (parameter->Mode == SemaParameterMode::Out && !parameter->Type->Is(SemaTypeKind::MeshOutput) &&
                !IsAssigned(state, parameter))
            {
                _sema->Diagnostics
                    .Error(DiagnosticCode::UnassignedVariable, range, "the `out` parameter `{0}` is not assigned",
                           parameter->Name)
                    .WithLabel("returns here without assigning it")
                    .WithSecondaryLabel(parameter->Range, "declared here");
            }
        }
    }

    /// The fields a class constructor must assign: non-nullable references without an initial value, unless the
    /// constructor hands the work to another one with `: this(…)` (MarlinScript §3.3).
    void CollectRequiredFields()
    {
        if (!_script || _function->Symbol->Kind != SemaSymbolKind::Constructor || _function->Owner == nullptr ||
            _function->Owner->Symbol->Kind != SemaSymbolKind::Class)
        {
            return;
        }
        const auto *decl = _function->Symbol->Decl->As<AstConstructorDecl>();
        if (decl->HasInitializer && !decl->InitializerIsBase)
        {
            return;
        }
        for (const SemaSymbol *member : _function->Owner->Symbol->Ordered)
        {
            if (member->IsStatic())
            {
                continue;
            }
            const AstNode *initializer = nullptr;
            AstNode *typeNode = nullptr;
            if (member->Kind == SemaSymbolKind::Field)
            {
                initializer = member->Decl->As<AstFieldDecl>()->Initializer;
                typeNode = member->Decl->As<AstFieldDecl>()->Type;
                if (HasAnyFlags(member->Decl->Modifiers, AstModifiers::Event))
                {
                    continue;
                }
            }
            else if (member->Kind == SemaSymbolKind::Property && Sema::IsAutoProperty(member))
            {
                initializer = member->Decl->As<AstPropertyDecl>()->Initializer;
                typeNode = member->Decl->As<AstPropertyDecl>()->Type;
            }
            if (typeNode == nullptr || initializer != nullptr)
            {
                continue;
            }
            for (const SemaField &field : _function->Owner->Fields)
            {
                if (field.Symbol == member && field.Type->IsReference())
                {
                    _required.Add(member);
                }
            }
        }
    }

    void CheckFields(const FlowState &state, SourceRange range)
    {
        for (const SemaSymbol *field : _required)
        {
            if (!state.Unreachable && !state.Fields.Contains(field) && !_reportedFields.Contains(field))
            {
                _reportedFields.Add(field);
                _sema->Diagnostics
                    .Error(DiagnosticCode::UnassignedVariable, range, "the constructor leaves `{0}` unassigned",
                           field->Name)
                    .WithSecondaryLabel(field->Decl->NameRange, "declared here without a value")
                    .WithHelp("assign it in every constructor, give it an initial value, or make it nullable");
            }
        }
    }

    /// Whether *value*, stored in a nullable variable, is known not to be null.
    [[nodiscard]] bool IsNotNull(const BoundExpr *value, const FlowState &state) const
    {
        if (!IsNullable(value->Type))
        {
            return !value->Type->Is(SemaTypeKind::Null);
        }
        if (value->Kind == BoundKind::TypeConversion)
        {
            const auto *conversion = value->As<BoundTypeConversion>();
            if (conversion->Conversion == BoundTypeConversionKind::WrapNullable ||
                (conversion->Conversion == BoundTypeConversionKind::Upcast && !IsNullable(conversion->Operand->Type)))
            {
                return !conversion->Operand->Type->Is(SemaTypeKind::Null);
            }
            return IsNotNull(conversion->Operand, state);
        }
        if (const SemaLocal *local = ReadLocal(value))
        {
            return state.NotNull.Contains(local);
        }
        return false;
    }

    static const SemaLocal *RootLocal(const BoundExpr *target)
    {
        while (target->Kind == BoundKind::Field || target->Kind == BoundKind::Index || target->Kind == BoundKind::Swizzle)
        {
            target = target->Kind == BoundKind::Field   ? target->As<BoundField>()->Object
                     : target->Kind == BoundKind::Index ? target->As<BoundIndex>()->Object
                                                        : target->As<BoundSwizzle>()->Object;
        }
        return target->Kind == BoundKind::Local ? target->As<BoundLocal>()->Local : nullptr;
    }

    /// Visits the parts of an assignment target that are read, such as indices and objects, then marks what it
    /// assigns: a local, or a field of `this` in a constructor.
    void Write(const BoundExpr *target, FlowState &state, const BoundExpr *value = nullptr)
    {
        const BoundExpr *current = target;
        while (current->Kind == BoundKind::Field || current->Kind == BoundKind::Index ||
               current->Kind == BoundKind::Swizzle)
        {
            if (current->Kind == BoundKind::Index)
            {
                Expression(current->As<BoundIndex>()->Index, state);
                current = current->As<BoundIndex>()->Object;
            }
            else
            {
                current = current->Kind == BoundKind::Field ? current->As<BoundField>()->Object
                                                            : current->As<BoundSwizzle>()->Object;
            }
        }
        switch (current->Kind)
        {
        case BoundKind::Local:
        {
            const SemaLocal *local = current->As<BoundLocal>()->Local;
            state.Add(local);
            if (current == target && IsNullable(local->Type))
            {
                state.SetNotNull(local, value != nullptr && IsNotNull(value, state));
            }
            break;
        }
        case BoundKind::BufferElement:
            Expression(current->As<BoundBufferElement>()->Buffer, state);
            Expression(current->As<BoundBufferElement>()->Index, state);
            if (current->As<BoundBufferElement>()->Length != nullptr)
            {
                Expression(current->As<BoundBufferElement>()->Length, state);
            }
            break;
        case BoundKind::ObjectField:
        {
            const auto *field = current->As<BoundObjectField>();
            const BoundExpr *object = field->Object;
            if (object->Kind == BoundKind::Local && object->As<BoundLocal>()->Local == _function->This)
            {
                state.AddField(field->Instance->Fields[field->FieldIndex].Symbol);
            }
            else
            {
                Expression(const_cast<BoundExpr *>(object), state);
            }
            break;
        }
        case BoundKind::ArrayElement:
            Expression(current->As<BoundArrayElement>()->Array, state);
            Expression(current->As<BoundArrayElement>()->Index, state);
            break;
        case BoundKind::Property:
        {
            const auto *property = current->As<BoundProperty>();
            if (property->Receiver != nullptr && property->Receiver->Kind == BoundKind::Local &&
                property->Receiver->As<BoundLocal>()->Local == _function->This &&
                Sema::IsAutoProperty(property->Getter->Symbol))
            {
                state.AddField(property->Getter->Symbol);
            }
            else
            {
                Expression(property->Receiver, state);
            }
            for (BoundExpr *argument : property->Arguments)
            {
                Expression(argument, state);
            }
            break;
        }
        case BoundKind::TypeConversion:
            Write(current->As<BoundTypeConversion>()->Operand, state, value);
            break;
        default:
            break;
        }
    }

    void Arguments(Span<BoundExpr *> arguments, Span<SemaLocal *const> parameters, usize offset, FlowState &state)
    {
        List<const BoundExpr *> outputs;
        for (usize i = 0; i < arguments.Count(); ++i)
        {
            const bool isOut = i >= offset && i - offset < parameters.Count() &&
                               parameters[i - offset]->Mode == SemaParameterMode::Out;
            if (isOut)
            {
                outputs.Add(arguments[i]);
            }
            else
            {
                Expression(arguments[i], state);
            }
        }
        for (const BoundExpr *output : outputs)
        {
            Write(output, state);
        }
    }

    void ReportPossiblyNull(const SemaLocal *local, SourceRange range)
    {
        if (_reportedNull.Contains(local))
        {
            return;
        }
        _reportedNull.Add(local);
        _sema->Diagnostics.Error(DiagnosticCode::PossiblyNull, range, "`{0}` may be null here", local->Name)
            .WithLabel("cannot be null")
            .WithHelp("prove it first: `if (x != null)`, `x is T t`, `x?.Member`, `x ?? fallback`, or assert it with "
                      "`x!` (MarlinScript §3.6)");
    }

    /// Evaluates a condition, and what is known when it holds and when it does not.
    void Condition(BoundExpr *condition, FlowState &state, FlowState &whenTrue, FlowState &whenFalse)
    {
        if (condition == nullptr)
        {
            whenTrue = state;
            whenFalse = Unreachable();
            return;
        }
        if (condition->Kind == BoundKind::ShortCircuit)
        {
            auto *node = condition->As<BoundShortCircuit>();
            FlowState leftTrue;
            FlowState leftFalse;
            Condition(node->Left, state, leftTrue, leftFalse);
            FlowState rightTrue;
            FlowState rightFalse;
            if (node->IsAnd)
            {
                Condition(node->Right, leftTrue, rightTrue, rightFalse);
                whenTrue = rightTrue;
                whenFalse = Meet(leftFalse, rightFalse);
            }
            else
            {
                Condition(node->Right, leftFalse, rightTrue, rightFalse);
                whenTrue = Meet(leftTrue, rightTrue);
                whenFalse = rightFalse;
            }
            state = Meet(whenTrue, whenFalse);
            return;
        }
        if (condition->Kind == BoundKind::Unary && condition->As<BoundUnary>()->Op == MirOp::LogicalNot)
        {
            Condition(condition->As<BoundUnary>()->Operand, state, whenFalse, whenTrue);
            return;
        }
        Expression(condition, state);
        whenTrue = state;
        whenFalse = state;
        bool notNull = false;
        if (const SemaLocal *local = NullTest(condition, notNull); local != nullptr && IsNullable(local->Type))
        {
            (notNull ? whenTrue : whenFalse).SetNotNull(local, true);
        }
        if (condition->Kind == BoundKind::Constant)
        {
            (condition->As<BoundConstant>()->Value->Bits != 0 ? whenFalse : whenTrue) = Unreachable();
        }
    }

    void Expression(BoundExpr *expression, FlowState &state)
    {
        if (expression == nullptr)
        {
            return;
        }
        switch (expression->Kind)
        {
        case BoundKind::Local:
        {
            const SemaLocal *local = expression->As<BoundLocal>()->Local;
            if (!IsAssigned(state, local) && !_reported.Contains(local) && !local->Type->Is(SemaTypeKind::RayQuery))
            {
                _reported.Add(local);
                _sema->Diagnostics
                    .Error(DiagnosticCode::UnassignedVariable, expression->Range,
                           "`{0}` is used before it is assigned", local->Name)
                    .WithLabel("read here")
                    .WithSecondaryLabel(local->Range, "declared here without a value");
            }
            break;
        }
        case BoundKind::Field:
            Expression(expression->As<BoundField>()->Object, state);
            break;
        case BoundKind::Swizzle:
            Expression(expression->As<BoundSwizzle>()->Object, state);
            break;
        case BoundKind::Index:
            Expression(expression->As<BoundIndex>()->Object, state);
            Expression(expression->As<BoundIndex>()->Index, state);
            break;
        case BoundKind::BufferElement:
            Expression(expression->As<BoundBufferElement>()->Buffer, state);
            Expression(expression->As<BoundBufferElement>()->Index, state);
            if (expression->As<BoundBufferElement>()->Length != nullptr)
            {
                Expression(expression->As<BoundBufferElement>()->Length, state);
            }
            break;
        case BoundKind::Call:
        {
            auto *call = expression->As<BoundCall>();
            const usize offset = call->Function->This != nullptr && call->Function->Symbol->Kind != SemaSymbolKind::Constructor
                                     ? 1
                                     : 0;
            Arguments(call->Arguments, call->Function->Parameters.AsSpan(), offset, state);
            // RECOVERY-DECISION lang-sema-D7: a constructor that calls a method of its object may assign the fields
            // there, so the check trusts it, as the library's `Allocate(…)` helpers rely on.
            if (offset == 1 && !_required.IsEmpty() && ReadLocal(call->Arguments[0]) == _function->This &&
                _function->This != nullptr)
            {
                for (const SemaSymbol *field : _required)
                {
                    state.AddField(field);
                }
            }
            break;
        }
        case BoundKind::Intrinsic:
            for (BoundExpr *argument : expression->As<BoundIntrinsic>()->Arguments)
            {
                Expression(argument, state);
            }
            break;
        case BoundKind::Derivative:
            for (BoundExpr *argument : expression->As<BoundDerivative>()->Arguments)
            {
                Expression(argument, state);
            }
            break;
        case BoundKind::Construct:
            for (BoundExpr *element : expression->As<BoundConstruct>()->Elements)
            {
                Expression(element, state);
            }
            break;
        case BoundKind::Convert:
            Expression(expression->As<BoundConvert>()->Operand, state);
            break;
        case BoundKind::Bitcast:
            Expression(expression->As<BoundBitcast>()->Operand, state);
            break;
        case BoundKind::Unary:
            Expression(expression->As<BoundUnary>()->Operand, state);
            break;
        case BoundKind::Binary:
            Expression(expression->As<BoundBinary>()->Left, state);
            Expression(expression->As<BoundBinary>()->Right, state);
            break;
        case BoundKind::ShortCircuit:
        {
            FlowState whenTrue;
            FlowState whenFalse;
            Condition(expression, state, whenTrue, whenFalse);
            break;
        }
        case BoundKind::Assign:
        {
            auto *assign = expression->As<BoundAssign>();
            if (assign->IsCompound && !assign->Combined)
            {
                Expression(assign->Target, state);
            }
            Expression(assign->Value, state);
            Write(assign->Target, state, assign->IsCompound && !assign->Combined ? nullptr : assign->Value);
            break;
        }
        case BoundKind::IncDec:
            Expression(expression->As<BoundIncDec>()->Target, state);
            break;
        case BoundKind::Conditional:
        {
            auto *conditional = expression->As<BoundConditional>();
            FlowState whenTrue;
            FlowState whenFalse;
            Condition(conditional->Condition, state, whenTrue, whenFalse);
            Expression(conditional->WhenTrue, whenTrue);
            Expression(conditional->WhenFalse, whenFalse);
            state = Meet(whenTrue, whenFalse);
            break;
        }
        case BoundKind::Sequence:
        {
            auto *sequence = expression->As<BoundSequence>();
            Expression(sequence->Initial, state);
            state.Add(sequence->Temporary);
            for (BoundExpr *effect : sequence->Effects)
            {
                Expression(effect, state);
            }
            break;
        }
        case BoundKind::ObjectField:
            Expression(expression->As<BoundObjectField>()->Object, state);
            break;
        case BoundKind::NewObject:
        {
            auto *node = expression->As<BoundNewObject>();
            Arguments(node->Arguments, node->Constructor->Parameters.AsSpan(), 0, state);
            break;
        }
        case BoundKind::NewArray:
            Expression(expression->As<BoundNewArray>()->Length, state);
            for (BoundExpr *element : expression->As<BoundNewArray>()->Elements)
            {
                Expression(element, state);
            }
            break;
        case BoundKind::ArrayElement:
            Expression(expression->As<BoundArrayElement>()->Array, state);
            Expression(expression->As<BoundArrayElement>()->Index, state);
            break;
        case BoundKind::Property:
            Expression(expression->As<BoundProperty>()->Receiver, state);
            for (BoundExpr *argument : expression->As<BoundProperty>()->Arguments)
            {
                Expression(argument, state);
            }
            break;
        case BoundKind::TypeConversion:
        {
            auto *conversion = expression->As<BoundTypeConversion>();
            Expression(conversion->Operand, state);
            // Taking the value out of `T?` without `!` needs a proof that it is not null (MarlinScript §3.6).
            // RECOVERY-DECISION lang-sema-D8: only locals and parameters are tracked; values read from fields,
            // properties and calls are trusted, and captured locals too, since a lambda may assign them.
            if (_script && conversion->Conversion == BoundTypeConversionKind::RemoveNullable && !conversion->Explicit &&
                !state.Unreachable)
            {
                if (const SemaLocal *local = ReadLocal(conversion->Operand);
                    local != nullptr && !local->IsCaptured && !state.NotNull.Contains(local))
                {
                    ReportPossiblyNull(local, conversion->Operand->Range);
                }
            }
            break;
        }
        case BoundKind::TypeTest:
            Expression(expression->As<BoundTypeTest>()->Operand, state);
            break;
        case BoundKind::Let:
        {
            auto *let = expression->As<BoundLet>();
            Expression(let->Initial, state);
            state.Add(let->Local);
            if (IsNullable(let->Local->Type))
            {
                state.SetNotNull(let->Local, IsNotNull(let->Initial, state));
            }
            Expression(let->Result, state);
            break;
        }
        case BoundKind::MakeDelegate:
            Expression(expression->As<BoundMakeDelegate>()->Receiver, state);
            break;
        case BoundKind::Lambda:
            // A lambda reads the locals it captures when it runs, which may be at once.
            for (const SemaLocal *captured : expression->As<BoundLambda>()->Function->Captures)
            {
                if (captured->Function == _function && !IsAssigned(state, captured) && !_reported.Contains(captured))
                {
                    _reported.Add(captured);
                    _sema->Diagnostics
                        .Error(DiagnosticCode::UnassignedVariable, expression->Range,
                               "the lambda captures `{0}` before it is assigned", captured->Name)
                        .WithSecondaryLabel(captured->Range, "declared here without a value");
                }
            }
            break;
        case BoundKind::InvokeDelegate:
            Expression(expression->As<BoundInvokeDelegate>()->Delegate, state);
            for (BoundExpr *argument : expression->As<BoundInvokeDelegate>()->Arguments)
            {
                Expression(argument, state);
            }
            break;
        case BoundKind::CombineDelegates:
            Expression(expression->As<BoundCombineDelegates>()->Left, state);
            Expression(expression->As<BoundCombineDelegates>()->Right, state);
            break;
        case BoundKind::Await:
            Expression(expression->As<BoundAwait>()->Operand, state);
            break;
        default:
            break;
        }
    }

    /// The locals a statement or expression may assign, so facts about them do not survive into the next
    /// iteration of a loop.
    void CollectWrites(const BoundStmt *statement, List<const SemaLocal *> &writes)
    {
        if (statement == nullptr)
        {
            return;
        }
        switch (statement->Kind)
        {
        case BoundStmtKind::Block:
            for (const BoundStmt *inner : statement->As<BoundBlock>()->Statements)
            {
                CollectWrites(inner, writes);
            }
            break;
        case BoundStmtKind::Local:
            writes.Add(statement->As<BoundLocalStmt>()->Local);
            CollectWrites(statement->As<BoundLocalStmt>()->Initializer, writes);
            break;
        case BoundStmtKind::Expression:
            CollectWrites(statement->As<BoundExprStmt>()->Expression, writes);
            break;
        case BoundStmtKind::If:
            CollectWrites(statement->As<BoundIf>()->Condition, writes);
            CollectWrites(statement->As<BoundIf>()->Then, writes);
            CollectWrites(statement->As<BoundIf>()->Else, writes);
            break;
        case BoundStmtKind::Loop:
        {
            const auto *loop = statement->As<BoundLoop>();
            for (const BoundStmt *initializer : loop->Initializers)
            {
                CollectWrites(initializer, writes);
            }
            CollectWrites(loop->Condition, writes);
            for (const BoundExpr *increment : loop->Increments)
            {
                CollectWrites(increment, writes);
            }
            CollectWrites(loop->Body, writes);
            break;
        }
        case BoundStmtKind::Switch:
            for (const BoundSwitchSection &section : statement->As<BoundSwitch>()->Sections)
            {
                CollectWrites(section.Match, writes);
                for (const BoundStmt *inner : section.Statements)
                {
                    CollectWrites(inner, writes);
                }
            }
            break;
        case BoundStmtKind::Return:
            CollectWrites(statement->As<BoundReturn>()->Value, writes);
            break;
        case BoundStmtKind::Yield:
            CollectWrites(statement->As<BoundYield>()->Value, writes);
            break;
        default:
            break;
        }
    }

    void CollectWrites(const BoundExpr *expression, List<const SemaLocal *> &writes)
    {
        if (expression == nullptr)
        {
            return;
        }
        if (expression->Kind == BoundKind::Assign)
        {
            if (const SemaLocal *local = RootLocal(expression->As<BoundAssign>()->Target))
            {
                writes.Add(local);
            }
            CollectWrites(expression->As<BoundAssign>()->Value, writes);
        }
        else if (expression->Kind == BoundKind::Call)
        {
            // `out` arguments.
            for (const BoundExpr *argument : expression->As<BoundCall>()->Arguments)
            {
                if (const SemaLocal *local = argument->IsLValue ? RootLocal(argument) : nullptr)
                {
                    writes.Add(local);
                }
            }
        }
        else if (expression->Kind == BoundKind::Conditional)
        {
            CollectWrites(expression->As<BoundConditional>()->WhenTrue, writes);
            CollectWrites(expression->As<BoundConditional>()->WhenFalse, writes);
        }
        else if (expression->Kind == BoundKind::Let)
        {
            CollectWrites(expression->As<BoundLet>()->Result, writes);
        }
    }

    void Statement(BoundStmt *statement, FlowState &state)
    {
        if (statement == nullptr)
        {
            return;
        }
        switch (statement->Kind)
        {
        case BoundStmtKind::Block:
            for (BoundStmt *inner : statement->As<BoundBlock>()->Statements)
            {
                Statement(inner, state);
            }
            break;
        case BoundStmtKind::Local:
        {
            auto *local = statement->As<BoundLocalStmt>();
            if (local->Initializer != nullptr)
            {
                Expression(local->Initializer, state);
                state.Add(local->Local);
                if (IsNullable(local->Local->Type))
                {
                    state.SetNotNull(local->Local, IsNotNull(local->Initializer, state));
                }
            }
            else
            {
                _tracked.Add(local->Local);
                state.Assigned.Remove(local->Local);
                state.NotNull.Remove(local->Local);
            }
            break;
        }
        case BoundStmtKind::Expression:
            Expression(statement->As<BoundExprStmt>()->Expression, state);
            break;
        case BoundStmtKind::If:
        {
            auto *branch = statement->As<BoundIf>();
            FlowState whenTrue;
            FlowState whenFalse;
            Condition(branch->Condition, state, whenTrue, whenFalse);
            Statement(branch->Then, whenTrue);
            Statement(branch->Else, whenFalse);
            state = Meet(whenTrue, whenFalse);
            break;
        }
        case BoundStmtKind::Loop:
        {
            auto *loop = statement->As<BoundLoop>();
            for (BoundStmt *initializer : loop->Initializers)
            {
                Statement(initializer, state);
            }
            // What the loop assigns may differ in a later iteration.
            List<const SemaLocal *> writes;
            CollectWrites(loop->Body, writes);
            for (const BoundExpr *increment : loop->Increments)
            {
                CollectWrites(increment, writes);
            }
            CollectWrites(loop->Condition, writes);
            for (const SemaLocal *written : writes)
            {
                state.NotNull.Remove(written);
            }
            _breaks.AddDefaulted();
            _continues.AddDefaulted();
            FlowState exit;
            if (loop->TestFirst)
            {
                FlowState body;
                FlowState after;
                Condition(loop->Condition, state, body, after);
                Statement(loop->Body, body);
                for (const FlowState &continued : _continues.Last())
                {
                    body = Meet(body, continued);
                }
                for (BoundExpr *increment : loop->Increments)
                {
                    Expression(increment, body);
                }
                exit = after;
            }
            else
            {
                Statement(loop->Body, state);
                for (const FlowState &continued : _continues.Last())
                {
                    state = Meet(state, continued);
                }
                FlowState again;
                Condition(loop->Condition, state, again, exit);
            }
            for (const FlowState &broken : _breaks.Last())
            {
                exit = Meet(exit, broken);
            }
            _breaks.Pop();
            _continues.Pop();
            state = exit;
            break;
        }
        case BoundStmtKind::Switch:
        {
            auto *node = statement->As<BoundSwitch>();
            Expression(node->Selector, state);
            if (node->Input != nullptr)
            {
                state.Add(node->Input);
            }
            _breaks.AddDefaulted();
            bool hasDefault = false;
            FlowState tested = state;
            for (const BoundSwitchSection &section : node->Sections)
            {
                hasDefault = hasDefault || section.IsDefault;
                FlowState inner = tested;
                if (section.Match != nullptr)
                {
                    FlowState failed;
                    Condition(section.Match, tested, inner, failed);
                    tested = failed;
                }
                for (BoundStmt *innerStatement : section.Statements)
                {
                    Statement(innerStatement, inner);
                }
            }
            FlowState exit = hasDefault ? Unreachable() : tested;
            for (const FlowState &broken : _breaks.Last())
            {
                exit = Meet(exit, broken);
            }
            _breaks.Pop();
            state = exit;
            break;
        }
        case BoundStmtKind::Break:
            if (!_breaks.IsEmpty())
            {
                _breaks.Last().Add(state);
            }
            state = Unreachable();
            break;
        case BoundStmtKind::Continue:
            if (!_continues.IsEmpty())
            {
                _continues.Last().Add(state);
            }
            state = Unreachable();
            break;
        case BoundStmtKind::Return:
            Expression(statement->As<BoundReturn>()->Value, state);
            if (!state.Unreachable)
            {
                CheckOutParameters(state, statement->Range);
                CheckFields(state, statement->Range);
            }
            state = Unreachable();
            break;
        case BoundStmtKind::Discard:
            state = Unreachable();
            break;
        case BoundStmtKind::Yield:
            if (statement->As<BoundYield>()->Value == nullptr)
            {
                state = Unreachable();
            }
            else
            {
                Expression(statement->As<BoundYield>()->Value, state);
            }
            break;
        }
    }

    Sema *_sema;
    SemaFunction *_function;
    bool _script = false;
    List<const SemaLocal *> _tracked;
    List<const SemaLocal *> _reported;
    List<const SemaLocal *> _reportedNull;
    List<const SemaSymbol *> _required;
    List<const SemaSymbol *> _reportedFields;
    List<List<FlowState>> _breaks;
    List<List<FlowState>> _continues;
};
} // namespace

void Sema::CheckFlow(SemaFunction *function)
{
    if (function->Body != nullptr)
    {
        FlowChecker(*this, *function).Run();
    }
}
} // namespace Marlin
