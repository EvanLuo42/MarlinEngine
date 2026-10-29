#include "Sema/Binder.h"
#include "Sema/SemaHelpers.h"

namespace Marlin
{
namespace
{
/// Whether *node*, a statement or the expression body of a function, contains `yield` outside nested lambdas.
bool ContainsYield(const AstNode *node)
{
    if (node == nullptr)
    {
        return false;
    }
    switch (node->Kind)
    {
    case AstKind::YieldStmt:
        return true;
    case AstKind::BlockStmt:
        for (const AstNode *statement : node->As<AstBlockStmt>()->Statements)
        {
            if (ContainsYield(statement))
            {
                return true;
            }
        }
        return false;
    case AstKind::IfStmt:
        return ContainsYield(node->As<AstIfStmt>()->Then) || ContainsYield(node->As<AstIfStmt>()->Else);
    case AstKind::ForStmt:
        return ContainsYield(node->As<AstForStmt>()->Body);
    case AstKind::ForeachStmt:
        return ContainsYield(node->As<AstForeachStmt>()->Body);
    case AstKind::WhileStmt:
        return ContainsYield(node->As<AstWhileStmt>()->Body);
    case AstKind::DoStmt:
        return ContainsYield(node->As<AstDoStmt>()->Body);
    case AstKind::SwitchStmt:
        for (const AstSwitchSection &section : node->As<AstSwitchStmt>()->Sections)
        {
            for (const AstNode *statement : section.Statements)
            {
                if (ContainsYield(statement))
                {
                    return true;
                }
            }
        }
        return false;
    default:
        return false;
    }
}

/// The instance of a core library class or interface named *name* with one or no generic argument, if *type* is one.
const SemaStructInstance *CoreInstance(const SemaType *type, StringView name, usize arity)
{
    if (!type->HasInstance())
    {
        return nullptr;
    }
    const SemaStructInstance *instance = type->GetStruct();
    return instance->Symbol->IsCore && instance->Symbol->Name == name && instance->Arguments.Count() == arity
               ? instance
               : nullptr;
}
} // namespace

void Binder::PrepareFunctionKind(SemaFunction *function, bool isAsync, AstNode *body, SourceRange range)
{
    const SemaType *returnType = function->ReturnType;
    const bool iterator = ContainsYield(body);
    if (isAsync && iterator)
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, range, "a function cannot be both `async` and an iterator")
            .WithNote("`yield` and `await` cannot be mixed (MarlinScript §4.3)");
        return;
    }
    if (isAsync)
    {
        function->Kind = SemaFunctionKind::Async;
        if (CoreInstance(returnType, "Task", 0) != nullptr)
        {
            function->ResultType = _sema->Types.GetVoid();
        }
        else if (const SemaStructInstance *task = CoreInstance(returnType, "Task", 1))
        {
            function->ResultType = task->Arguments[0].Type;
        }
        else
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, range, "an `async` function returns `Task` or `Task<T>`, not `{0}`",
                                     TypeNameOf(returnType).AsView());
            function->ResultType = _sema->Types.GetError();
        }
        return;
    }
    if (iterator)
    {
        function->Kind = SemaFunctionKind::Iterator;
        const SemaStructInstance *sequence = CoreInstance(returnType, "IEnumerable", 1);
        if (sequence == nullptr)
        {
            sequence = CoreInstance(returnType, "IEnumerator", 1);
        }
        if (sequence == nullptr)
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, range,
                                     "a function with `yield` returns `IEnumerable<T>` or `IEnumerator<T>`, not `{0}`",
                                     TypeNameOf(returnType).AsView());
            function->ResultType = _sema->Types.GetError();
            return;
        }
        function->ResultType = sequence->Arguments[0].Type;
    }
}

const SemaType *Binder::ReturnTypeForStatements()
{
    if (_function->Kind == SemaFunctionKind::Async && _function->ResultType != nullptr)
    {
        return _function->ResultType;
    }
    return _function->ReturnType;
}

BoundExpr *Binder::BindAwait(const AstAwaitExpr *node)
{
    BoundExpr *operand = BindExpression(node->Operand);
    if (operand->Type->IsError())
    {
        return operand;
    }
    if (_function == nullptr || _function->Kind != SemaFunctionKind::Async)
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, node->Range, "`await` is only allowed in an `async` function")
            .WithHelp("mark the function or lambda `async` and make it return `Task`");
        return Error(node->Range);
    }
    const SemaType *result = nullptr;
    if (CoreInstance(operand->Type, "Task", 0) != nullptr)
    {
        result = _sema->Types.GetVoid();
    }
    else if (const SemaStructInstance *task = CoreInstance(operand->Type, "Task", 1))
    {
        result = task->Arguments[0].Type;
    }
    else
    {
        _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, node->Operand->Range, "only a `Task` can be awaited, not `{0}`",
                                 TypeNameOf(operand->Type).AsView());
        return Error(node->Range);
    }
    auto *await = MakeExpr<BoundAwait>(*_sema, result, node->Range);
    await->Operand = operand;
    return await;
}

BoundStmt *Binder::BindYield(const AstYieldStmt *node)
{
    auto *statement = MakeStmt<BoundYield>(*_sema, node->Range);
    if (_function == nullptr || _function->Kind != SemaFunctionKind::Iterator || _function->ResultType == nullptr)
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, node->Range, "`yield` is only allowed in an iterator");
        return statement;
    }
    if (!node->IsBreak)
    {
        statement->Value = ConvertImplicit(BindExpression(node->Value, _function->ResultType), _function->ResultType);
    }
    return statement;
}

BoundStmt *Binder::BindScriptForeach(const AstForeachStmt *statement, BoundExpr *collection)
{
    if (collection->Type->Is(SemaTypeKind::Nullable))
    {
        collection = ConvertImplicit(collection, collection->Type->GetElement());
        if (collection->Type->IsError())
        {
            return nullptr;
        }
    }
    const SemaType *collectionType = collection->Type;
    PushScope();
    auto local = [&](SemaLocal *variable) {
        auto *expression = MakeExpr<BoundLocal>(*_sema, variable->Type, statement->Range);
        expression->Local = variable;
        expression->IsLValue = true;
        return expression;
    };
    auto declare = [&](SemaLocal *variable, BoundExpr *value) {
        auto *declaration = MakeStmt<BoundLocalStmt>(*_sema, statement->Range);
        declaration->Local = variable;
        declaration->Initializer = value;
        return declaration;
    };
    auto hidden = [&](StringView name, const SemaType *type) {
        SemaLocal *variable = _sema->New<SemaLocal>();
        variable->Name = name;
        variable->Type = type;
        variable->Range = statement->Range;
        variable->Function = _function;
        return variable;
    };
    auto *loop = MakeStmt<BoundLoop>(*_sema, statement->Range);
    BindLoopControl(statement->Attributes, loop);
    List<BoundStmt *> initializers;
    BoundExpr *element = nullptr;
    if (collectionType->Is(SemaTypeKind::GcArray))
    {
        const SemaType *intType = _sema->Types.GetInt(32, true);
        SemaLocal *source = hidden("foreach", collectionType);
        SemaLocal *index = hidden("index", intType);
        initializers.Add(declare(source, collection));
        initializers.Add(declare(index, MakeConstant(*_sema, _sema->NewScalar(intType, 0), statement->Range)));
        AstMemberExpr length;
        length.Kind = AstKind::MemberExpr;
        length.Range = statement->Collection->Range;
        length.NameRange = statement->Collection->Range;
        length.Name = "Length";
        BinderTarget count = ResolveReferenceMember(local(source), &length);
        if (count.TargetKind != BinderTarget::Kind::Value)
        {
            PopScope();
            return nullptr;
        }
        auto *condition = MakeExpr<BoundBinary>(*_sema, _sema->Types.GetBool(), statement->Range);
        condition->Op = MirOp::Less;
        condition->Left = local(index);
        condition->Right = count.Value;
        loop->Condition = condition;
        auto *increment = MakeExpr<BoundIncDec>(*_sema, intType, statement->Range);
        increment->Target = local(index);
        BoundExpr *increments[] = {increment};
        loop->Increments = _sema->Freeze(List<BoundExpr *>(Span<BoundExpr *const>(increments)));
        auto *item = MakeExpr<BoundArrayElement>(*_sema, collectionType->GetElement(), statement->Range);
        item->Array = local(source);
        item->Index = local(index);
        item->IsLValue = true;
        element = item;
    }
    else
    {
        BoundExpr *enumerator = CallMethod(collection, "GetEnumerator", {}, statement->Collection->Range);
        if (enumerator == nullptr)
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, statement->Collection->Range,
                                     "`foreach` goes over arrays, ranges and types with `GetEnumerator()`, not `{0}`",
                                     TypeNameOf(collectionType).AsView());
            PopScope();
            return nullptr;
        }
        SemaLocal *cursor = hidden("enumerator", enumerator->Type);
        initializers.Add(declare(cursor, enumerator));
        BoundExpr *moveNext = CallMethod(local(cursor), "MoveNext", {}, statement->Collection->Range);
        AstMemberExpr current;
        current.Kind = AstKind::MemberExpr;
        current.Range = statement->Collection->Range;
        current.NameRange = statement->Collection->Range;
        current.Name = "Current";
        BinderTarget currentValue = enumerator->Type->IsReference() ? ResolveReferenceMember(local(cursor), &current)
                                                                    : ResolveInstanceMember(local(cursor), &current);
        if (moveNext == nullptr || !moveNext->Type->Is(SemaTypeKind::Bool) ||
            currentValue.TargetKind != BinderTarget::Kind::Value)
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, statement->Collection->Range,
                                     "the enumerator of `{0}` needs `bool MoveNext()` and `Current`",
                                     TypeNameOf(collectionType).AsView());
            PopScope();
            return nullptr;
        }
        loop->Condition = moveNext;
        element = currentValue.Value;
    }
    loop->Initializers = _sema->Freeze(initializers);
    const SemaType *variableType = element->Type;
    if (statement->Type != nullptr)
    {
        variableType = _sema->ResolveType(statement->Type, _context);
        element = Convert(element, variableType, true);
    }
    SemaLocal *variable = DeclareLocal(statement->Name, variableType, statement->NameRange);
    ++_loopDepth;
    ++_breakDepth;
    BoundStmt *body = BindStatement(statement->Body);
    --_loopDepth;
    --_breakDepth;
    List<BoundStmt *> statements;
    statements.Add(declare(variable, element));
    if (body != nullptr)
    {
        statements.Add(body);
    }
    auto *block = MakeStmt<BoundBlock>(*_sema, statement->Range);
    block->Statements = _sema->Freeze(statements);
    loop->Body = block;
    PopScope();
    return loop;
}
} // namespace Marlin
