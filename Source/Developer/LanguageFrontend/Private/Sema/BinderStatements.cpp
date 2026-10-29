#include "Sema/Binder.h"
#include "Sema/SemaHelpers.h"
#include "Sema/SemaBuiltins.h"
#include "LanguageIR/MirPasses.h"

#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
bool IsStatementExpression(const BoundExpr *expression)
{
    switch (expression->Kind)
    {
    case BoundKind::Call:
    case BoundKind::Intrinsic:
    case BoundKind::Assign:
    case BoundKind::IncDec:
    case BoundKind::Derivative:
    case BoundKind::NewObject:
    case BoundKind::InvokeDelegate:
    case BoundKind::Await:
        return true;
    case BoundKind::Let:
        // `a?.Run()` and the like: the access on the value when it is not null.
        return IsStatementExpression(expression->As<BoundLet>()->Result);
    case BoundKind::Conditional:
        return expression->Type->Is(SemaTypeKind::Void) &&
               IsStatementExpression(expression->As<BoundConditional>()->WhenTrue);
    case BoundKind::Sequence:
        return !expression->As<BoundSequence>()->Effects.IsEmpty() &&
               IsStatementExpression(expression->As<BoundSequence>()->Effects.Last());
    default:
        return expression->Type->IsError();
    }
}

bool EndsInJump(const BoundStmt *statement)
{
    if (statement == nullptr)
    {
        return false;
    }
    switch (statement->Kind)
    {
    case BoundStmtKind::Break:
    case BoundStmtKind::Continue:
    case BoundStmtKind::Return:
    case BoundStmtKind::Discard:
        return true;
    case BoundStmtKind::Block:
    {
        const Span<BoundStmt *> statements = statement->As<BoundBlock>()->Statements;
        return !statements.IsEmpty() && EndsInJump(statements.Last());
    }
    case BoundStmtKind::If:
    {
        const auto *branch = statement->As<BoundIf>();
        return EndsInJump(branch->Then) && EndsInJump(branch->Else);
    }
    default:
        return false;
    }
}

/// Whether *condition* is `Device.Supports(…)` or a chain of them joined by `&&`, the shape whose features a
/// `static if` proves.
bool IsGuardShape(const AstNode *condition)
{
    if (const auto *binary = condition->TryAs<AstBinaryExpr>())
    {
        return binary->Operator == TokenKind::AmpersandAmpersand && IsGuardShape(binary->Left) &&
               IsGuardShape(binary->Right);
    }
    const auto *call = condition->TryAs<AstCallExpr>();
    const auto *member = call != nullptr ? call->Callee->TryAs<AstMemberExpr>() : nullptr;
    const auto *owner = member != nullptr ? member->Target->TryAs<AstNameExpr>() : nullptr;
    return owner != nullptr && owner->Name == "Device" && member->Name == "Supports" && call->Arguments.Count() == 1;
}
} // namespace

void Binder::PushScope()
{
    _scopes.AddDefaulted();
}

void Binder::PopScope()
{
    _scopes.Pop();
}

SemaLocal *Binder::FindLocal(StringView name) const
{
    for (usize i = _scopes.Count(); i-- > 0;)
    {
        if (SemaLocal *const *local = _scopes[i].Find(name))
        {
            return *local;
        }
    }
    return nullptr;
}

SemaLocal *Binder::DeclareLocal(StringView name, const SemaType *type, SourceRange range)
{
    if (SemaLocal *existing = FindLocal(name))
    {
        _sema->Diagnostics.Error(DiagnosticCode::DuplicateDefinition, range, "`{0}` is already declared", name)
            .WithLabel("redeclared here")
            .WithSecondaryLabel(existing->Range, "first declared here");
    }
    SemaLocal *local = _sema->New<SemaLocal>();
    local->Name = name;
    local->Type = type;
    local->Range = range;
    local->Function = _function;
    local->Iteration = _iteration;
    _scopes.Last().Set(name, local);
    return local;
}

SemaIteration *Binder::BeginIteration(BoundLoop *loop)
{
    // RECOVERY-DECISION lang-sema-D5: script loops start an iteration before their body is bound (a `foreach` before
    // its variable), so the body's locals and the lambdas made in it belong to one run of the body.
    auto *iteration = _sema->New<SemaIteration>();
    iteration->Parent = _iteration;
    _iteration = iteration;
    if (loop != nullptr)
    {
        loop->Iteration = iteration;
    }
    return iteration;
}

void Binder::CollectGuards(AstNode *condition, List<StringView> &features)
{
    if (const auto *binary = condition->TryAs<AstBinaryExpr>())
    {
        CollectGuards(binary->Left, features);
        CollectGuards(binary->Right, features);
        return;
    }
    const auto *call = condition->As<AstCallExpr>();
    BoundExpr *feature = BindExpression(call->Arguments[0].Value);
    if (feature->Kind == BoundKind::Constant && feature->Type->Is(SemaTypeKind::Feature))
    {
        features.Add(feature->As<BoundConstant>()->Value->Text);
    }
}

BoundStmt *Binder::BindFunctionBody(const AstBody &body)
{
    for (SemaLocal *parameter : _function->Parameters)
    {
        _scopes.Last().Set(parameter->Name, parameter);
    }
    if (InScript())
    {
        const bool isAsync = HasAnyFlags(_function->Symbol->Decl->Modifiers, AstModifiers::Async);
        AstNode *bodyNode = body.Block != nullptr ? static_cast<AstNode *>(body.Block) : body.Expression;
        PrepareFunctionKind(_function, isAsync, bodyNode, _function->Symbol->Decl->NameRange);
    }
    const bool isConstructor = _function->Symbol->Kind == SemaSymbolKind::Constructor;
    // A class constructor runs on the new object, `this`; a struct constructor builds the value and returns it.
    const bool isClassConstructor = isConstructor && _function->Owner != nullptr &&
                                    _function->Owner->Symbol->Kind == SemaSymbolKind::Class;
    const bool isStructConstructor = isConstructor && !isClassConstructor;
    List<BoundStmt *> statements;
    if (isClassConstructor)
    {
        BindConstructorPrologue(statements);
    }
    if (isStructConstructor)
    {
        SemaLocal *self = _sema->New<SemaLocal>();
        self->Name = "this";
        self->Type = _function->ReturnType;
        self->Range = _function->Symbol->Decl->NameRange;
        self->Function = _function;
        _function->This = self;
        auto *initialize = MakeStmt<BoundLocalStmt>(*_sema, self->Range);
        initialize->Local = self;
        initialize->Initializer = BindConstruction(self->Type, {}, self->Range);
        statements.Add(initialize);
    }
    if (body.Expression != nullptr)
    {
        const SemaType *returnType = ReturnTypeForStatements();
        BoundExpr *value =
            BindExpression(body.Expression, returnType->Is(SemaTypeKind::Void) ? nullptr : returnType);
        if (returnType->Is(SemaTypeKind::Void) || isConstructor)
        {
            auto *statement = MakeStmt<BoundExprStmt>(*_sema, body.Expression->Range);
            statement->Expression = value;
            statements.Add(statement);
        }
        else
        {
            auto *statement = MakeStmt<BoundReturn>(*_sema, body.Expression->Range);
            statement->Value = ConvertImplicit(value, returnType);
            statements.Add(statement);
        }
    }
    else if (body.Block != nullptr)
    {
        statements.Add(BindBlock(body.Block));
    }
    if (isStructConstructor)
    {
        auto *statement = MakeStmt<BoundReturn>(*_sema, _function->Symbol->Decl->NameRange);
        auto *self = MakeExpr<BoundLocal>(*_sema, _function->This->Type, statement->Range);
        self->Local = _function->This;
        self->IsLValue = true;
        statement->Value = self;
        statements.Add(statement);
    }
    auto *block = MakeStmt<BoundBlock>(*_sema, body.Block != nullptr ? body.Block->Range
                                                                     : (body.Expression != nullptr ? body.Expression->Range
                                                                                                   : SourceRange{}));
    block->Statements = _sema->Freeze(statements);
    return block;
}

BoundBlock *Binder::BindBlock(const AstBlockStmt *block)
{
    PushScope();
    List<BoundStmt *> statements;
    for (AstNode *node : block->Statements)
    {
        if (BoundStmt *statement = BindStatement(node))
        {
            statements.Add(statement);
        }
    }
    PopScope();
    auto *result = MakeStmt<BoundBlock>(*_sema, block->Range);
    result->Statements = _sema->Freeze(statements);
    return result;
}

BoundExpr *Binder::BindCondition(AstNode *node)
{
    BoundExpr *condition = BindExpression(node, _sema->Types.GetBool());
    if (condition->Type->IsError())
    {
        return condition;
    }
    if (condition->Type->Is(SemaTypeKind::Vector))
    {
        _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, node->Range, "a condition must be a single bool")
            .WithLabel(TypeNameOf(condition->Type).AsView())
            .WithHelp("reduce a bool vector with `any(…)` or `all(…)`");
        return Error(node->Range);
    }
    return ConvertImplicit(condition, _sema->Types.GetBool());
}

BoundStmt *Binder::BindStatement(AstNode *node)
{
    switch (node->Kind)
    {
    case AstKind::BlockStmt:
        return BindBlock(node->As<AstBlockStmt>());
    case AstKind::EmptyStmt:
        return nullptr;
    case AstKind::VarStmt:
        return BindLocal(node->As<AstVarStmt>());
    case AstKind::ExprStmt:
    {
        BoundExpr *expression = BindExpression(node->As<AstExprStmt>()->Expression);
        if (expression->Kind == BoundKind::Zero && expression->Type->Is(SemaTypeKind::Void))
        {
            // A `[DebugOnly]` call without debug output: nothing runs, not even its arguments.
            return nullptr;
        }
        if (!IsStatementExpression(expression))
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, node->Range, "this expression does nothing")
                .WithHelp("only calls, assignments, `++` and `--` can be statements");
        }
        auto *statement = MakeStmt<BoundExprStmt>(*_sema, node->Range);
        statement->Expression = expression;
        return statement;
    }
    case AstKind::IfStmt:
        return BindIf(node->As<AstIfStmt>());
    case AstKind::WhileStmt:
    {
        const auto *loop = node->As<AstWhileStmt>();
        auto *statement = MakeStmt<BoundLoop>(*_sema, node->Range);
        BindLoopControl(loop->Attributes, statement);
        statement->Condition = BindCondition(loop->Condition);
        SemaIteration *iteration = InScript() ? BeginIteration(statement) : nullptr;
        ++_loopDepth;
        ++_breakDepth;
        statement->Body = BindStatement(loop->Body);
        --_loopDepth;
        --_breakDepth;
        if (iteration != nullptr)
        {
            _iteration = iteration->Parent;
        }
        return statement;
    }
    case AstKind::DoStmt:
    {
        const auto *loop = node->As<AstDoStmt>();
        auto *statement = MakeStmt<BoundLoop>(*_sema, node->Range);
        BindLoopControl(loop->Attributes, statement);
        statement->TestFirst = false;
        SemaIteration *iteration = InScript() ? BeginIteration(statement) : nullptr;
        ++_loopDepth;
        ++_breakDepth;
        statement->Body = BindStatement(loop->Body);
        --_loopDepth;
        --_breakDepth;
        if (iteration != nullptr)
        {
            _iteration = iteration->Parent;
        }
        statement->Condition = BindCondition(loop->Condition);
        return statement;
    }
    case AstKind::ForStmt:
        return BindFor(node->As<AstForStmt>());
    case AstKind::ForeachStmt:
        return BindForeach(node->As<AstForeachStmt>());
    case AstKind::SwitchStmt:
        return BindSwitch(node->As<AstSwitchStmt>());
    case AstKind::BreakStmt:
        if (_breakDepth == 0)
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, node->Range,
                                     "`break` is only allowed in a loop or switch");
        }
        return MakeStmt<BoundBreak>(*_sema, node->Range);
    case AstKind::ContinueStmt:
        if (_loopDepth == 0)
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, node->Range, "`continue` is only allowed in a loop");
        }
        return MakeStmt<BoundContinue>(*_sema, node->Range);
    case AstKind::ReturnStmt:
    {
        const auto *ret = node->As<AstReturnStmt>();
        auto *statement = MakeStmt<BoundReturn>(*_sema, node->Range);
        const bool isConstructor = _function->Symbol->Kind == SemaSymbolKind::Constructor;
        const bool isStructConstructor = isConstructor && (_function->Owner == nullptr ||
                                                           _function->Owner->Symbol->Kind != SemaSymbolKind::Class);
        const SemaType *returnType = isConstructor ? _sema->Types.GetVoid() : ReturnTypeForStatements();
        if (_function->Kind == SemaFunctionKind::Iterator)
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, node->Range,
                                     "an iterator ends with `yield break`, not `return`");
            return statement;
        }
        if (ret->Value == nullptr)
        {
            if (!returnType->Is(SemaTypeKind::Void) && !returnType->IsError())
            {
                _sema->Diagnostics.Error(DiagnosticCode::MissingReturn, node->Range, "this function returns `{0}`",
                                         TypeNameOf(returnType).AsView())
                    .WithLabel("needs a value");
            }
            if (isStructConstructor)
            {
                auto *self = MakeExpr<BoundLocal>(*_sema, _function->This->Type, node->Range);
                self->Local = _function->This;
                self->IsLValue = true;
                statement->Value = self;
            }
            return statement;
        }
        BoundExpr *value = BindExpression(ret->Value, returnType->Is(SemaTypeKind::Void) ? nullptr : returnType);
        if (returnType->Is(SemaTypeKind::Void))
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, ret->Value->Range,
                                     "a void function cannot return a value");
            return statement;
        }
        statement->Value = ConvertImplicit(value, returnType);
        return statement;
    }
    case AstKind::YieldStmt:
        return BindYield(node->As<AstYieldStmt>());
    case AstKind::DiscardStmt:
        if (_context.Dialect != LanguageDialect::Shader)
        {
            NotInScript(node);
        }
        else if (_function != nullptr)
        {
            constexpr uint32 PixelBit = 1u << static_cast<uint32>(MirStage::Pixel);
            if ((_function->AllowedStages & PixelBit) != 0 && _function->AllowedStages != PixelBit)
            {
                _function->StageUse = node->Range;
            }
            _function->AllowedStages &= PixelBit;
        }
        return MakeStmt<BoundDiscard>(*_sema, node->Range);
    default:
        _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, node->Range, "expected a statement");
        return nullptr;
    }
}

BoundStmt *Binder::BindLocal(const AstVarStmt *declaration)
{
    const SemaType *declared =
        declaration->Type != nullptr ? _sema->ResolveDeclaredType(declaration->Type, declaration->Attributes, _context)
                                     : nullptr;
    List<BoundStmt *> statements;
    for (const AstVariableDeclarator &declarator : declaration->Declarators)
    {
        BoundExpr *initializer = nullptr;
        const SemaType *type = declared;
        if (declaration->IsRef)
        {
            // `ref T x = ref y;`: a name for the storage of *y*, of exactly its type.
            const auto *reference =
                declarator.Initializer != nullptr ? declarator.Initializer->TryAs<AstRefExpr>() : nullptr;
            if (reference == nullptr)
            {
                // RECOVERY-DECISION lang-sema-D6: the parser already requires `ref` here; this text is a fallback.
                _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, declarator.Range,
                                         "a `ref` local needs a `ref` initializer");
                continue;
            }
            initializer = BindExpression(reference->Operand);
            if (!RequireLValue(initializer, "pass by reference"))
            {
                continue;
            }
            if (type == nullptr)
            {
                type = initializer->Type;
            }
            else if (!initializer->Type->IsError() && initializer->Type != type)
            {
                _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, initializer->Range,
                                         "a `ref` argument must be exactly `{0}`", TypeNameOf(type).AsView());
                continue;
            }
            SemaLocal *local = DeclareLocal(declarator.Name, type, declarator.Range);
            local->Mode = SemaParameterMode::Ref;
            auto *statement = MakeStmt<BoundLocalStmt>(*_sema, declarator.Range);
            statement->Local = local;
            statement->Initializer = initializer;
            statements.Add(statement);
            continue;
        }
        if (declarator.Initializer != nullptr)
        {
            initializer = BindExpression(declarator.Initializer, declared);
            if (type == nullptr)
            {
                type = initializer->Type;
                if (type->Is(SemaTypeKind::Null) || type->Is(SemaTypeKind::Void) ||
                    type->Is(SemaTypeKind::FunctionGroup))
                {
                    _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, declarator.Initializer->Range,
                                             "`var` cannot take its type from this expression")
                        .WithHelp("write the type");
                    type = _sema->Types.GetError();
                }
            }
            else
            {
                initializer = ConvertImplicit(initializer, type);
            }
        }
        else if (type == nullptr)
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, declarator.Range, "`var` needs an initializer");
            type = _sema->Types.GetError();
        }
        if ((type->Is(SemaTypeKind::String) && !InScript()) || type->Is(SemaTypeKind::Feature))
        {
            if (!declaration->IsConst)
            {
                _sema->Diagnostics.Error(DiagnosticCode::NotConstant, declarator.Range,
                                         "a `{0}` exists only at compile time", TypeNameOf(type).AsView())
                    .WithHelp("declare it `const`");
            }
        }
        SemaLocal *local = DeclareLocal(declarator.Name, type, declarator.Range);
        local->Attributes = declaration->Attributes;
        if (declaration->IsConst)
        {
            if (initializer == nullptr)
            {
                _sema->Diagnostics.Error(DiagnosticCode::NotConstant, declarator.Range, "a constant needs a value");
            }
            else if (initializer->Kind == BoundKind::Constant)
            {
                local->Constant = initializer->As<BoundConstant>()->Value;
            }
            else if (!initializer->Type->IsError())
            {
                _sema->Diagnostics.Error(DiagnosticCode::NotConstant, initializer->Range,
                                         "the value of `{0}` is not a constant", declarator.Name)
                    .WithLabel("computed at run time");
            }
            continue;
        }
        auto *statement = MakeStmt<BoundLocalStmt>(*_sema, declarator.Range);
        statement->Local = local;
        statement->Initializer = initializer;
        statements.Add(statement);
    }
    if (statements.Count() == 1)
    {
        return statements[0];
    }
    auto *block = MakeStmt<BoundBlock>(*_sema, declaration->Range);
    block->Statements = _sema->Freeze(statements);
    return block;
}

BoundStmt *Binder::BindIf(const AstIfStmt *statement)
{
    if (statement->IsStatic)
    {
        List<StringView> features;
        if (IsGuardShape(statement->Condition))
        {
            CollectGuards(statement->Condition, features);
        }
        BoundExpr *condition = BindCondition(statement->Condition);
        if (condition->Type->IsError())
        {
            return nullptr;
        }
        if (condition->Kind != BoundKind::Constant)
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotConstant, statement->Condition->Range,
                                     "the condition of `static if` must be a constant")
                .WithLabel("computed at run time");
            return nullptr;
        }
        const bool taken = condition->As<BoundConstant>()->Value->Bits != 0;
        AstNode *branch = taken ? statement->Then : statement->Else;
        if (branch == nullptr)
        {
            return nullptr;
        }
        const usize guards = _guards.Count();
        if (taken)
        {
            _guards.AddRange(features.AsSpan());
        }
        BoundStmt *result = BindStatement(branch);
        _guards.Resize(guards);
        return result;
    }
    auto *result = MakeStmt<BoundIf>(*_sema, statement->Range);
    result->Condition = BindCondition(statement->Condition);
    result->Then = BindStatement(statement->Then);
    result->Else = statement->Else != nullptr ? BindStatement(statement->Else) : nullptr;
    return result;
}

void Binder::BindLoopControl(Span<AstAttribute> attributes, BoundLoop *loop)
{
    for (const AstAttribute &attribute : attributes)
    {
        if (attribute.Name == "MaxIterations")
        {
            // The bound reverse-mode differentiation sizes what it keeps per iteration by (§9.12).
            const SemaConstant *count = attribute.Arguments.Count() == 1
                                            ? _sema->EvaluateAttributeArgument(attribute, 0, {}, _context)
                                            : nullptr;
            if (count == nullptr || !count->Type->Is(SemaTypeKind::Int) || count->GetInt() < 1 ||
                count->GetInt() > 65536)
            {
                _sema->Diagnostics.Error(DiagnosticCode::InvalidAttribute, attribute.Range,
                                         "`[MaxIterations(n)]` takes a constant from 1 to 65536")
                    .WithLabel("this attribute");
                continue;
            }
            loop->MaxIterations = static_cast<uint32>(count->GetInt());
            continue;
        }
        if (attribute.Name != "Unroll")
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidAttribute, attribute.Range,
                                     "`[{0}]` does not apply to a loop", attribute.Name)
                .WithLabel("this attribute")
                .WithNote("a loop takes `[Unroll]`, `[Unroll(n)]` or `[MaxIterations(n)]`");
            continue;
        }
        loop->Control = MirLoopControl::Unroll;
        if (attribute.Arguments.IsEmpty())
        {
            continue;
        }
        const SemaConstant *count = _sema->EvaluateAttributeArgument(attribute, 0, {}, _context);
        if (count != nullptr && (!count->Type->Is(SemaTypeKind::Int) || count->GetInt() < 1 ||
                                 count->Bits > MirLoopUnrollPass::MaxTripCount))
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidAttribute, attribute.Range,
                                     "`[Unroll(n)]` takes a constant from 1 to {0}", MirLoopUnrollPass::MaxTripCount)
                .WithLabel("this attribute");
            continue;
        }
        loop->UnrollCount = count != nullptr ? static_cast<uint32>(count->Bits) : 0;
    }
}

BoundStmt *Binder::BindFor(const AstForStmt *statement)
{
    PushScope();
    auto *loop = MakeStmt<BoundLoop>(*_sema, statement->Range);
    BindLoopControl(statement->Attributes, loop);
    List<BoundStmt *> initializers;
    for (AstNode *initializer : statement->Initializers)
    {
        if (BoundStmt *bound = BindStatement(initializer))
        {
            initializers.Add(bound);
        }
    }
    loop->Initializers = _sema->Freeze(initializers);
    loop->Condition = statement->Condition != nullptr ? BindCondition(statement->Condition) : nullptr;
    List<BoundExpr *> increments;
    for (AstNode *increment : statement->Increments)
    {
        BoundExpr *bound = BindExpression(increment);
        if (!IsStatementExpression(bound))
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, increment->Range, "this expression does nothing");
        }
        increments.Add(bound);
    }
    loop->Increments = _sema->Freeze(increments);
    // The loop variables are shared by every iteration; the body's locals are not.
    SemaIteration *iteration = InScript() ? BeginIteration(loop) : nullptr;
    ++_loopDepth;
    ++_breakDepth;
    loop->Body = BindStatement(statement->Body);
    --_loopDepth;
    --_breakDepth;
    if (iteration != nullptr)
    {
        _iteration = iteration->Parent;
    }
    PopScope();
    return loop;
}

BoundStmt *Binder::BindForeach(const AstForeachStmt *statement)
{
    BoundExpr *collection = BindExpression(statement->Collection);
    const SemaType *collectionType = collection->Type;
    if (collectionType->IsError())
    {
        return nullptr;
    }
    // A Span (§4.5) goes over its Length elements, with no clamping: the index is known to be in range.
    const bool isSpan = collectionType->Is(SemaTypeKind::Struct) &&
                        collectionType->GetStruct()->Symbol->BuiltinId == static_cast<uint32>(BuiltinTypeId::Span) &&
                        _sema->ResolveFields(collectionType->GetStruct());
    const bool isRange = collectionType->Is(SemaTypeKind::Struct) &&
                         collectionType->GetStruct()->Symbol->BuiltinId == static_cast<uint32>(BuiltinTypeId::Range);
    if (!isRange && !isSpan && !collectionType->Is(SemaTypeKind::Array))
    {
        if (InScript())
        {
            // Script arrays and enumerables; the loop's variable belongs to one iteration (MarlinScript §3.9).
            SemaIteration *iteration = BeginIteration(nullptr);
            BoundStmt *loop = BindScriptForeach(statement, collection);
            _iteration = iteration->Parent;
            if (loop != nullptr && loop->Kind == BoundStmtKind::Loop)
            {
                loop->As<BoundLoop>()->Iteration = iteration;
            }
            return loop;
        }
        _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, statement->Collection->Range,
                                 "`foreach` goes over a fixed-size array, a `Span` or a `Range`, not `{0}`",
                                 TypeNameOf(collectionType).AsView());
        return nullptr;
    }
    PushScope();
    const SemaType *intType = _sema->Types.GetInt(32, true);
    const SemaType *uintType = _sema->Types.GetInt(32, false);
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
    SemaLocal *source = _sema->New<SemaLocal>();
    source->Name = "foreach";
    source->Type = collectionType;
    source->Range = statement->Collection->Range;
    source->Function = _function;
    const SemaType *indexType = isRange ? intType : uintType;
    SemaLocal *index = _sema->New<SemaLocal>();
    index->Name = "index";
    index->Type = indexType;
    index->Range = statement->Range;
    index->Function = _function;

    auto *loop = MakeStmt<BoundLoop>(*_sema, statement->Range);
    BindLoopControl(statement->Attributes, loop);
    List<BoundStmt *> initializers;
    initializers.Add(declare(source, collection));
    BoundExpr *start = nullptr;
    BoundExpr *end = nullptr;
    if (isRange)
    {
        auto field = [&](uint32 fieldIndex) {
            auto *node = MakeExpr<BoundField>(*_sema, intType, statement->Range);
            node->Object = local(source);
            node->FieldIndex = fieldIndex;
            return node;
        };
        _sema->ResolveFields(collectionType->GetStruct());
        start = field(0);
        end = field(1);
    }
    else
    {
        auto *zero = MakeExpr<BoundConstant>(*_sema, uintType, statement->Range);
        zero->Value = _sema->NewScalar(uintType, 0);
        start = zero;
        if (isSpan)
        {
            auto *length = MakeExpr<BoundField>(*_sema, uintType, statement->Range);
            length->Object = local(source);
            length->FieldIndex = 1;
            end = length;
        }
        else
        {
            auto *count = MakeExpr<BoundConstant>(*_sema, uintType, statement->Range);
            count->Value = _sema->NewScalar(uintType, collectionType->GetCount());
            end = count;
        }
    }
    initializers.Add(declare(index, start));
    loop->Initializers = _sema->Freeze(initializers);
    auto *condition = MakeExpr<BoundBinary>(*_sema, _sema->Types.GetBool(), statement->Range);
    condition->Op = MirOp::Less;
    condition->Left = local(index);
    condition->Right = end;
    loop->Condition = condition;
    auto *increment = MakeExpr<BoundIncDec>(*_sema, indexType, statement->Range);
    increment->Target = local(index);
    BoundExpr *increments[] = {increment};
    loop->Increments = _sema->Freeze(List<BoundExpr *>(Span<BoundExpr *const>(increments)));

    SemaIteration *iteration = InScript() ? BeginIteration(loop) : nullptr;
    const SemaType *pointerType = isSpan ? collectionType->GetStruct()->Fields[0].Type : nullptr;
    const SemaType *elementType = isRange ? intType : isSpan ? pointerType->GetElement() : collectionType->GetElement();
    BoundExpr *element = local(index);
    if (isSpan)
    {
        auto *pointer = MakeExpr<BoundField>(*_sema, pointerType, statement->Range);
        pointer->Object = local(source);
        pointer->FieldIndex = 0;
        auto *node = MakeExpr<BoundBufferElement>(*_sema, elementType, statement->Range);
        node->Buffer = pointer;
        node->Index = local(index);
        element = node;
    }
    else if (!isRange)
    {
        auto *node = MakeExpr<BoundIndex>(*_sema, elementType, statement->Range);
        node->Object = local(source);
        node->Index = local(index);
        node->InBounds = true;
        element = node;
    }
    const SemaType *variableType = elementType;
    if (statement->Type != nullptr)
    {
        variableType = _sema->ResolveType(statement->Type, _context);
        element = ConvertImplicit(element, variableType);
    }
    SemaLocal *variable = DeclareLocal(statement->Name, variableType, statement->NameRange);
    ++_loopDepth;
    ++_breakDepth;
    BoundStmt *body = BindStatement(statement->Body);
    --_loopDepth;
    --_breakDepth;
    if (iteration != nullptr)
    {
        _iteration = iteration->Parent;
    }
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

BoundStmt *Binder::BindSwitch(const AstSwitchStmt *statement)
{
    // RECOVERY-DECISION lang-sema-D10: every script `switch` statement is a pattern switch (constants of any type are
    // constant patterns); shaders and the common subset keep the integer switch (MarlinScript §3.9).
    if (InScript())
    {
        return BindPatternSwitch(statement);
    }
    BoundExpr *selector = BindExpression(statement->Selector);
    const SemaType *selectorType = selector->Type;
    if (!selectorType->IsError() && !selectorType->Is(SemaTypeKind::Int) && !selectorType->Is(SemaTypeKind::Enum))
    {
        _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, statement->Selector->Range,
                                 "`switch` takes an integer or an enum, not `{0}`", TypeNameOf(selectorType).AsView());
        selectorType = _sema->Types.GetError();
    }
    auto *result = MakeStmt<BoundSwitch>(*_sema, statement->Range);
    result->Selector = selector;
    List<BoundSwitchSection> sections;
    List<int64> seen;
    bool hasDefault = false;
    ++_breakDepth;
    for (const AstSwitchSection &section : statement->Sections)
    {
        BoundSwitchSection bound;
        bound.IsDefault = section.HasDefault;
        if (section.HasDefault)
        {
            if (hasDefault)
            {
                _sema->Diagnostics.Error(DiagnosticCode::DuplicateDefinition, section.Range,
                                         "`switch` has more than one `default`");
            }
            hasDefault = true;
        }
        List<int64> values;
        for (const AstCaseLabel &caseLabel : section.Labels)
        {
            const AstConstantPattern *constant =
                caseLabel.Pattern != nullptr ? caseLabel.Pattern->TryAs<AstConstantPattern>() : nullptr;
            if (constant == nullptr || caseLabel.When != nullptr)
            {
                _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, caseLabel.Range,
                                         "`case` patterns and `when` exist only in scripts");
                continue;
            }
            AstNode *label = constant->Value;
            BoundExpr *value = BindExpression(label, selectorType);
            if (!selectorType->IsError())
            {
                value = ConvertImplicit(value, selectorType);
            }
            if (value->Type->IsError())
            {
                continue;
            }
            if (value->Kind != BoundKind::Constant)
            {
                _sema->Diagnostics.Error(DiagnosticCode::NotConstant, label->Range, "a `case` label must be a constant");
                continue;
            }
            const int64 number = value->As<BoundConstant>()->Value->GetInt();
            if (seen.Contains(number))
            {
                _sema->Diagnostics.Error(DiagnosticCode::DuplicateDefinition, label->Range, "this case is already handled");
                continue;
            }
            seen.Add(number);
            values.Add(number);
        }
        bound.Values = _sema->Freeze(values);
        PushScope();
        List<BoundStmt *> statements;
        for (AstNode *node : section.Statements)
        {
            if (BoundStmt *inner = BindStatement(node))
            {
                statements.Add(inner);
            }
        }
        PopScope();
        if (statements.IsEmpty() || !EndsInJump(statements.Last()))
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, section.Range,
                                     "a `switch` section must end in `break`, `continue`, `return` or `discard`")
                .WithNote("control cannot fall through to the next section");
        }
        bound.Statements = _sema->Freeze(statements);
        sections.Add(bound);
    }
    --_breakDepth;
    result->Sections = _sema->Freeze(sections);
    return result;
}
} // namespace Marlin
