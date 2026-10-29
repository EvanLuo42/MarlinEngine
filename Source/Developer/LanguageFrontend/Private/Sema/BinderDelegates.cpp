#include "Sema/Binder.h"
#include "Sema/SemaHelpers.h"

namespace Marlin
{
namespace
{
SemaParameterMode LambdaModeOf(AstModifiers modifiers)
{
    return HasAnyFlags(modifiers, AstModifiers::Ref)   ? SemaParameterMode::Ref
           : HasAnyFlags(modifiers, AstModifiers::Out) ? SemaParameterMode::Out
           : HasAnyFlags(modifiers, AstModifiers::In)  ? SemaParameterMode::In
                                                       : SemaParameterMode::Value;
}

/// Whether *function*'s parameters, their modes and its result are exactly *signature*'s.
bool MatchesSignature(const SemaFunction *function, const SemaSignature &signature)
{
    if (function->Parameters.Count() != signature.Parameters.Count() || function->ReturnType != signature.ReturnType)
    {
        return false;
    }
    for (usize i = 0; i < signature.Parameters.Count(); ++i)
    {
        if (function->Parameters[i]->Type != signature.Parameters[i] ||
            function->Parameters[i]->Mode != signature.Modes[i])
        {
            return false;
        }
    }
    return true;
}

/// Whether a block contains `return value;` outside nested lambdas.
bool ReturnsValue(const AstNode *node)
{
    if (node == nullptr)
    {
        return false;
    }
    switch (node->Kind)
    {
    case AstKind::ReturnStmt:
        return node->As<AstReturnStmt>()->Value != nullptr;
    case AstKind::BlockStmt:
        for (const AstNode *statement : node->As<AstBlockStmt>()->Statements)
        {
            if (ReturnsValue(statement))
            {
                return true;
            }
        }
        return false;
    case AstKind::IfStmt:
        return ReturnsValue(node->As<AstIfStmt>()->Then) || ReturnsValue(node->As<AstIfStmt>()->Else);
    case AstKind::ForStmt:
        return ReturnsValue(node->As<AstForStmt>()->Body);
    case AstKind::ForeachStmt:
        return ReturnsValue(node->As<AstForeachStmt>()->Body);
    case AstKind::WhileStmt:
        return ReturnsValue(node->As<AstWhileStmt>()->Body);
    case AstKind::DoStmt:
        return ReturnsValue(node->As<AstDoStmt>()->Body);
    case AstKind::SwitchStmt:
        for (const AstSwitchSection &section : node->As<AstSwitchStmt>()->Sections)
        {
            for (const AstNode *statement : section.Statements)
            {
                if (ReturnsValue(statement))
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
} // namespace

BoundExpr *Binder::MakeUnbound(BinderTarget &target, const AstLambdaExpr *lambda, SourceRange range)
{
    auto *node = MakeExpr<BoundUnbound>(*_sema, _sema->Types.GetFunctionGroup(), range);
    node->Lambda = lambda;
    node->Functions = _sema->Freeze(target.Functions);
    node->Owner = target.Owner;
    node->Receiver = target.Receiver;
    node->ViaBase = target.ViaBase;
    node->GenericArguments = target.GenericArguments;
    node->HasGenericArguments = target.HasGenericArguments;
    return node;
}

bool Binder::FitsDelegate(const BoundExpr *value, const SemaType *type)
{
    if (value->Kind != BoundKind::Unbound || type == nullptr || !type->Is(SemaTypeKind::Delegate))
    {
        return false;
    }
    SemaSignature signature;
    if (!_sema->GetDelegateSignature(type->GetStruct(), signature))
    {
        return false;
    }
    const auto *unbound = value->As<BoundUnbound>();
    if (unbound->Lambda != nullptr)
    {
        const Span<AstLambdaParameter> parameters = unbound->Lambda->Parameters;
        if (parameters.Count() != signature.Parameters.Count())
        {
            return false;
        }
        for (usize i = 0; i < parameters.Count(); ++i)
        {
            if (LambdaModeOf(parameters[i].Modifier) != signature.Modes[i])
            {
                return false;
            }
            if (parameters[i].Type != nullptr &&
                _sema->ResolveType(parameters[i].Type, _context) != signature.Parameters[i])
            {
                return false;
            }
        }
        return true;
    }
    for (SemaSymbol *symbol : unbound->Functions)
    {
        if (!GenericParametersOf(symbol->Decl).IsEmpty() && !unbound->HasGenericArguments)
        {
            continue;
        }
        List<SemaGenericArgument> arguments;
        if (unbound->HasGenericArguments &&
            !_sema->ResolveGenericArguments(symbol, unbound->GenericArguments, _context, arguments))
        {
            continue;
        }
        SemaFunction *function = _sema->InstantiateFunction(symbol, unbound->Owner, arguments.AsSpan());
        if (_sema->ResolveSignature(function) && MatchesSignature(function, signature))
        {
            return true;
        }
    }
    return false;
}

BoundExpr *Binder::ConvertToDelegate(BoundExpr *value, const SemaType *type)
{
    auto *unbound = value->As<BoundUnbound>();
    if (!type->Is(SemaTypeKind::Delegate))
    {
        _sema->Diagnostics.Error(DiagnosticCode::NoConversion, value->Range, "{0} cannot become a delegate",
                                 StringView(unbound->Lambda != nullptr ? "a lambda" : "a method"))
            .WithLabel(Format("used where `{0}` is needed", TypeNameOf(type).AsView()).AsView())
            .WithHelp(unbound->Lambda != nullptr ? StringView("a lambda converts only to a delegate type")
                                                 : StringView("call it with `(…)`, or convert it to a delegate type"));
        return Error(value->Range);
    }
    SemaSignature signature;
    if (!_sema->GetDelegateSignature(type->GetStruct(), signature))
    {
        return Error(value->Range);
    }
    if (unbound->Lambda != nullptr)
    {
        BoundExpr *lambda = BindLambda(unbound->Lambda, signature, type);
        return lambda != nullptr ? lambda : Error(value->Range);
    }
    // A method group: the one method whose signature is exactly the delegate's (MarlinScript §3.7).
    SemaFunction *found = nullptr;
    for (SemaSymbol *symbol : unbound->Functions)
    {
        if (!GenericParametersOf(symbol->Decl).IsEmpty() && !unbound->HasGenericArguments)
        {
            continue;
        }
        List<SemaGenericArgument> arguments;
        if (unbound->HasGenericArguments &&
            !_sema->ResolveGenericArguments(symbol, unbound->GenericArguments, _context, arguments))
        {
            continue;
        }
        SemaFunction *function = _sema->InstantiateFunction(symbol, unbound->Owner, arguments.AsSpan());
        if (!_sema->ResolveSignature(function) || !MatchesSignature(function, signature))
        {
            continue;
        }
        if (found != nullptr)
        {
            _sema->Diagnostics.Error(DiagnosticCode::AmbiguousOverload, value->Range, "more than one `{0}` fits `{1}`",
                                     symbol->Name, TypeNameOf(type).AsView());
            return Error(value->Range);
        }
        found = function;
    }
    if (found == nullptr)
    {
        _sema->Diagnostics.Error(DiagnosticCode::NoConversion, value->Range, "no `{0}` has the signature of `{1}`",
                                 unbound->Functions.IsEmpty() ? StringView() : unbound->Functions[0]->Name,
                                 TypeNameOf(type).AsView())
            .WithNote("the parameters, their modes and the result must be exactly the delegate's");
        return Error(value->Range);
    }
    const bool isInstance = found->This != nullptr;
    if (isInstance && unbound->Receiver == nullptr)
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, value->Range, "`{0}` is an instance method",
                                 found->Symbol->Name)
            .WithHelp("name it on an object: `value.Method`");
        return Error(value->Range);
    }
    if (isInstance && !unbound->Receiver->Type->IsReference())
    {
        _sema->Diagnostics.Error(DiagnosticCode::NoConversion, value->Range, "{0} cannot become a delegate",
                                 StringView("a method of a value type"))
            .WithHelp("wrap it in a lambda");
        return Error(value->Range);
    }
    NoteCallee(found, value->Range);
    auto *node = MakeExpr<BoundMakeDelegate>(*_sema, type, value->Range);
    node->Function = found;
    if (isInstance)
    {
        node->Receiver = ConvertImplicit(unbound->Receiver, found->This->Type);
        const bool previous = _baseAccess;
        _baseAccess = unbound->ViaBase;
        node->Dispatch = DispatchOf(found, unbound->Receiver);
        _baseAccess = previous;
    }
    return node;
}

BoundExpr *Binder::BindLambda(const AstLambdaExpr *lambda, const SemaSignature &signature, const SemaType *type)
{
    if (!InScript())
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, lambda->Range, "lambdas exist only in scripts");
        return Error(lambda->Range);
    }
    const bool inferring = type == nullptr;
    if (lambda->Parameters.Count() != signature.Parameters.Count())
    {
        if (!inferring)
        {
            _sema->Diagnostics.Error(DiagnosticCode::WrongArgumentCount, lambda->Range,
                                     "`{0}` takes {1} parameter(s), but the lambda has {2}", TypeNameOf(type).AsView(),
                                     signature.Parameters.Count(), lambda->Parameters.Count());
        }
        return inferring ? nullptr : Error(lambda->Range);
    }
    // RECOVERY-DECISION lang-sema-D9: a lambda's result is inferred (for generic inference) from an expression body,
    // or as void from a block without `return value;`; async lambdas and other blocks give no inference.
    const bool blockBody = lambda->Body->Kind == AstKind::BlockStmt;
    if (inferring && (lambda->IsAsync || (blockBody && ReturnsValue(lambda->Body))))
    {
        return nullptr;
    }

    // A lambda is a function of its own, declared where it is written and bound in the scopes it sees.
    auto *decl = _sema->New<AstFunctionDecl>();
    decl->Kind = AstKind::FunctionDecl;
    decl->Range = lambda->Range;
    decl->NameRange = lambda->Range;
    decl->Name = "$lambda";
    decl->Parent = const_cast<AstDecl *>(_context.Decl);
    decl->Modifiers = (lambda->IsStatic ? AstModifiers::Static : AstModifiers::None) |
                      (lambda->IsAsync ? AstModifiers::Async : AstModifiers::None);
    decl->Body.Block = blockBody ? lambda->Body->As<AstBlockStmt>() : nullptr;
    decl->Body.Expression = blockBody ? nullptr : lambda->Body;
    decl->Body.BodyBegin = 0;
    decl->Body.BodyEnd = 1;
    decl->Body.Parsed = true;
    const SemaSymbol *enclosingSymbol = _function != nullptr ? _function->Symbol : nullptr;
    SemaSymbol *symbol = _sema->NewSymbol(SemaSymbolKind::Function, decl->Name,
                                          enclosingSymbol != nullptr ? enclosingSymbol->Parent
                                                                     : _sema->GetGlobalNamespace());
    symbol->Decl = decl;
    symbol->File = _context.File;
    symbol->Package = enclosingSymbol != nullptr ? enclosingSymbol->Package : 0;
    symbol->Visibility = SemaVisibility::Private;
    symbol->IsCore = enclosingSymbol != nullptr && enclosingSymbol->IsCore;
    SemaFunction *function = _sema->InstantiateFunction(symbol, _function != nullptr ? _function->Owner : _context.Owner, {});
    function->SignatureResolved = true;
    function->State = SemaFunctionState::Binding;
    function->Enclosing = _function;
    function->Iteration = _iteration;
    function->IsStaticLambda = lambda->IsStatic;
    function->ReturnType = signature.ReturnType;
    if (_function != nullptr)
    {
        function->Requires.AddRange(_function->Requires.AsSpan());
    }

    SemaFunction *enclosing = _function;
    SemaIteration *iteration = _iteration;
    const uint32 loopDepth = _loopDepth;
    const uint32 breakDepth = _breakDepth;
    _function = function;
    _iteration = nullptr;
    _loopDepth = 0;
    _breakDepth = 0;
    PushScope();
    bool ok = true;
    for (usize i = 0; i < lambda->Parameters.Count(); ++i)
    {
        const AstLambdaParameter &parameter = lambda->Parameters[i];
        const SemaType *parameterType = signature.Parameters[i];
        if (parameter.Type != nullptr)
        {
            const SemaType *written = _sema->ResolveType(parameter.Type, _context);
            if (!written->IsError() && written != parameterType)
            {
                if (!inferring)
                {
                    _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, parameter.Range,
                                             "the parameter is `{0}` in `{1}`, not `{2}`",
                                             TypeNameOf(parameterType).AsView(), TypeNameOf(type).AsView(),
                                             TypeNameOf(written).AsView());
                }
                ok = false;
            }
        }
        if (LambdaModeOf(parameter.Modifier) != signature.Modes[i])
        {
            if (!inferring)
            {
                _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, parameter.Range,
                                         "the parameter's `ref`, `out` or `in` differs from `{0}`",
                                         TypeNameOf(type).AsView());
            }
            ok = false;
        }
        SemaLocal *local = DeclareLocal(parameter.Name, parameterType, parameter.Range);
        local->IsParameter = true;
        local->Mode = signature.Modes[i];
        function->Parameters.Add(local);
    }
    if (lambda->IsAsync && ok)
    {
        PrepareFunctionKind(function, true, lambda->Body, lambda->Range);
    }

    auto *block = MakeStmt<BoundBlock>(*_sema, lambda->Body->Range);
    if (ok && blockBody)
    {
        if (function->ReturnType == nullptr)
        {
            function->ReturnType = _sema->Types.GetVoid();
        }
        block = BindBlock(lambda->Body->As<AstBlockStmt>());
    }
    else if (ok)
    {
        // `x => expression`: returns the expression, or runs it when the delegate returns nothing.
        const SemaType *result = function->ReturnType != nullptr ? ReturnTypeForStatements() : nullptr;
        BoundExpr *value = BindExpression(lambda->Body, result != nullptr && !result->Is(SemaTypeKind::Void) ? result
                                                                                                            : nullptr);
        if (function->ReturnType == nullptr)
        {
            function->ReturnType = value->Type->Is(SemaTypeKind::Null) ? _sema->Types.GetError() : value->Type;
            result = function->ReturnType;
        }
        BoundStmt *statement = nullptr;
        if (result->Is(SemaTypeKind::Void))
        {
            auto *expression = MakeStmt<BoundExprStmt>(*_sema, lambda->Body->Range);
            expression->Expression = value;
            statement = expression;
        }
        else
        {
            auto *returned = MakeStmt<BoundReturn>(*_sema, lambda->Body->Range);
            returned->Value = ConvertImplicit(value, result);
            statement = returned;
        }
        BoundStmt *statements[] = {statement};
        block->Statements = _sema->Freeze(List<BoundStmt *>(Span<BoundStmt *const>(statements)));
        ok = !value->Type->IsError();
    }
    PopScope();
    _function = enclosing;
    _iteration = iteration;
    _loopDepth = loopDepth;
    _breakDepth = breakDepth;

    function->Body = block;
    function->State = ok ? SemaFunctionState::Bound : SemaFunctionState::Failed;
    if (!ok)
    {
        return inferring ? nullptr : Error(lambda->Range);
    }
    if (function->ReturnType == nullptr || function->ReturnType->IsError())
    {
        return inferring ? nullptr : Error(lambda->Range);
    }
    _sema->CheckFlow(function);
    if (!inferring && enclosing != nullptr)
    {
        enclosing->Lambdas.Add(function);
    }
    auto *node = MakeExpr<BoundLambda>(*_sema, inferring ? _sema->Types.GetFunctionGroup() : type, lambda->Range);
    node->Function = function;
    return node;
}

BoundExpr *Binder::BindDelegateInvoke(BoundExpr *delegate, Span<AstArgument> astArguments, SourceRange range)
{
    const SemaType *type = delegate->Type;
    if (type->Is(SemaTypeKind::Nullable))
    {
        delegate = ConvertImplicit(delegate, type->GetElement());
        type = delegate->Type;
    }
    SemaSignature signature;
    if (!_sema->GetDelegateSignature(type->GetStruct(), signature))
    {
        return Error(range);
    }
    List<BinderArgument> arguments;
    if (!BindArguments(astArguments, arguments))
    {
        return Error(range);
    }
    if (arguments.Count() != signature.Parameters.Count())
    {
        _sema->Diagnostics.Error(DiagnosticCode::WrongArgumentCount, range, "`{0}` takes {1} argument(s), not {2}",
                                 TypeNameOf(type).AsView(), signature.Parameters.Count(), arguments.Count());
        return Error(range);
    }
    // Raising an event, a field declared `event`, does nothing when no handler is attached.
    const SemaSymbol *field = delegate->Kind == BoundKind::ObjectField
                                  ? delegate->As<BoundObjectField>()->Instance->Fields[delegate->As<BoundObjectField>()->FieldIndex].Symbol
                              : delegate->Kind == BoundKind::Global ? delegate->As<BoundGlobal>()->Global->Symbol
                                                                    : nullptr;
    const bool isEvent = field != nullptr && HasAnyFlags(field->Decl->Modifiers, AstModifiers::Event);
    if (isEvent && !signature.ReturnType->Is(SemaTypeKind::Void))
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, range,
                                 "an event with no handlers has no result to give")
            .WithNote("an event's delegate returns nothing");
        return Error(range);
    }
    List<BoundExpr *> bound;
    for (usize i = 0; i < arguments.Count(); ++i)
    {
        const SemaParameterMode mode = signature.Modes[i];
        const bool byReference = mode == SemaParameterMode::Ref || mode == SemaParameterMode::Out;
        if (byReference != (arguments[i].Mode == SemaParameterMode::Ref || arguments[i].Mode == SemaParameterMode::Out))
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, arguments[i].Range, "argument {0} must be passed {1}",
                                     i, StringView(byReference ? "by `ref` or `out`" : "by value"));
            return Error(range);
        }
        if (byReference && arguments[i].Value->Type != signature.Parameters[i])
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, arguments[i].Range,
                                     "a `ref` argument must be exactly `{0}`",
                                     TypeNameOf(signature.Parameters[i]).AsView());
            return Error(range);
        }
        bound.Add(byReference ? arguments[i].Value : ConvertImplicit(arguments[i].Value, signature.Parameters[i]));
    }
    auto *node = MakeExpr<BoundInvokeDelegate>(*_sema, signature.ReturnType, range);
    node->Delegate = delegate;
    node->Arguments = _sema->Freeze(bound);
    node->IsEvent = isEvent;
    return node;
}

void Binder::NoteCapture(SemaLocal *local, SourceRange range)
{
    // Every lambda between the use and the function that declares the local captures it.
    for (SemaFunction *lambda = _function; lambda != nullptr && lambda != local->Function; lambda = lambda->Enclosing)
    {
        if (lambda->Enclosing == nullptr)
        {
            break;
        }
        if (lambda->IsStaticLambda)
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, range, "a `static` lambda cannot capture `{0}`",
                                     local->Name)
                .WithSecondaryLabel(local->Range, "declared here");
            return;
        }
        if (local->IsParameter && (local->Mode == SemaParameterMode::Ref || local->Mode == SemaParameterMode::Out ||
                                   local->Mode == SemaParameterMode::In))
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, range,
                                     "a lambda cannot capture the `ref` parameter `{0}`", local->Name)
                .WithSecondaryLabel(local->Range, "declared here");
            return;
        }
        if (!lambda->Captures.Contains(local))
        {
            lambda->Captures.Add(local);
        }
        local->IsCaptured = true;
    }
}

SemaFunction *Binder::ThisOwner() const
{
    SemaFunction *owner = _function;
    while (owner != nullptr && owner->Enclosing != nullptr)
    {
        owner = owner->Enclosing;
    }
    return owner;
}
} // namespace Marlin
