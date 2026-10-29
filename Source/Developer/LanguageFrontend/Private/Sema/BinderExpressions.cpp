#include "Sema/Binder.h"
#include "Sema/SemaHelpers.h"

#include "Memory/MemoryOps.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
bool IsBoolLike(const SemaType *type)
{
    const SemaType *scalar = type->GetScalar();
    return scalar != nullptr && scalar->Is(SemaTypeKind::Bool) && !type->Is(SemaTypeKind::Matrix);
}

bool IsIntegerLike(const SemaType *type)
{
    const SemaType *scalar = type->GetScalar();
    return scalar != nullptr && scalar->Is(SemaTypeKind::Int) && !type->Is(SemaTypeKind::Matrix);
}

bool IsFlagsEnum(const SemaType *type)
{
    return type->Is(SemaTypeKind::Enum) && Sema::FindAttribute(type->GetSymbol()->Decl->Attributes, "Flags") != nullptr;
}

int32 SwizzleIndex(char c, bool &rgba)
{
    switch (c)
    {
    case 'x':
        return 0;
    case 'y':
        return 1;
    case 'z':
        return 2;
    case 'w':
        return 3;
    case 'r':
        rgba = true;
        return 0;
    case 'g':
        rgba = true;
        return 1;
    case 'b':
        rgba = true;
        return 2;
    case 'a':
        rgba = true;
        return 3;
    default:
        return -1;
    }
}
} // namespace

Binder::Binder(Sema &sema, SemaFunction *function, const SemaContext &context)
    : _sema(&sema), _function(function), _context(context)
{
    PushScope();
}

BoundExpr *Binder::Error(SourceRange range)
{
    auto *zero = _sema->New<BoundZero>();
    zero->Kind = BoundKind::Zero;
    zero->Type = _sema->Types.GetError();
    zero->Range = range;
    return zero;
}

BoundExpr *Binder::NotInScript(const AstNode *node)
{
    if (node->Kind == AstKind::DiscardStmt)
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "`discard` exists only in shaders");
    }
    else
    {
        // RECOVERY-DECISION lang-sema-D2: the message for other shader-only constructs met in a script.
        _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "this construct exists only in shaders");
    }
    return Error(node->Range);
}

bool Binder::RequireLValue(BoundExpr *expression, StringView what)
{
    if (expression->IsLValue || expression->Type->IsError())
    {
        return true;
    }
    _sema->Diagnostics.Error(DiagnosticCode::NotAssignable, expression->Range, "cannot {0} this expression", what)
        .WithLabel("not a variable, field or element");
    return false;
}

BoundExpr *Binder::Fold(BoundExpr *expression)
{
    if (expression->Type->IsError())
    {
        return expression;
    }
    if (SemaConstant *value = _sema->Evaluate(expression, false))
    {
        return MakeConstant(*_sema, value, expression->Range);
    }
    return expression;
}

BoundExpr *Binder::BindExpression(AstNode *node, const SemaType *expected)
{
    if (node == nullptr)
    {
        return Error({});
    }
    switch (node->Kind)
    {
    case AstKind::LiteralExpr:
        return BindLiteral(node->As<AstLiteralExpr>(), expected);
    case AstKind::NameExpr:
    case AstKind::MemberExpr:
    {
        BinderTarget target = ResolveTarget(node);
        BoundExpr *value = TargetToValue(target);
        if (value->Kind == BoundKind::Unbound && expected != nullptr && expected->Is(SemaTypeKind::Delegate))
        {
            return ConvertToDelegate(value, expected);
        }
        return value;
    }
    case AstKind::CallExpr:
        return BindCall(node->As<AstCallExpr>());
    case AstKind::IndexExpr:
        return BindIndex(node->As<AstIndexExpr>());
    case AstKind::UnaryExpr:
        return BindUnary(node->As<AstUnaryExpr>());
    case AstKind::BinaryExpr:
        return BindBinary(node->As<AstBinaryExpr>());
    case AstKind::AssignExpr:
        return BindAssign(node->As<AstAssignExpr>());
    case AstKind::ConditionalExpr:
        return BindConditional(node->As<AstConditionalExpr>(), expected);
    case AstKind::CastExpr:
        return BindCast(node->As<AstCastExpr>());
    case AstKind::NewExpr:
        return BindNew(node->As<AstNewExpr>());
    case AstKind::WithExpr:
        return BindWith(node->As<AstWithExpr>());
    case AstKind::SizeofExpr:
        return BindSizeof(node->As<AstSizeofExpr>());
    case AstKind::NameofExpr:
        return BindNameof(node->As<AstNameofExpr>());
    case AstKind::DefaultExpr:
        return BindDefault(node->As<AstDefaultExpr>(), expected);
    case AstKind::ThisExpr:
        return BindThis(node->Range);
    case AstKind::BaseExpr:
        // RECOVERY-DECISION lang-sema-D11: the help text for a lone `base` is not recovered.
        _sema->Diagnostics.Error(DiagnosticCode::UndefinedName, node->Range,
                                 "`base` is only available in instance members of a class")
            .WithHelp("name a member of the base class: `base.Member`");
        return Error(node->Range);
    case AstKind::TupleExpr:
        _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, node->Range, "a tuple is only allowed in attributes");
        return Error(node->Range);
    case AstKind::TypeofExpr:
        return BindTypeof(node->As<AstTypeofExpr>());
    case AstKind::IsExpr:
        return BindIs(node->As<AstIsExpr>());
    case AstKind::AsExpr:
        return BindAs(node->As<AstAsExpr>());
    case AstKind::LambdaExpr:
    {
        const auto *lambda = node->As<AstLambdaExpr>();
        if (!InScript())
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "lambdas exist only in scripts");
            return Error(node->Range);
        }
        BinderTarget none;
        BoundExpr *unbound = MakeUnbound(none, lambda, node->Range);
        return expected != nullptr && expected->Is(SemaTypeKind::Delegate) ? ConvertToDelegate(unbound, expected)
                                                                           : unbound;
    }
    case AstKind::AwaitExpr:
        return BindAwait(node->As<AstAwaitExpr>());
    case AstKind::InterpolatedStringExpr:
        if (!InScript())
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "this operator exists only in scripts");
            return Error(node->Range);
        }
        return BindInterpolatedString(node->As<AstInterpolatedStringExpr>());
    case AstKind::NullForgivingExpr:
        return BindNullForgiving(node->As<AstNullForgivingExpr>());
    case AstKind::ConditionalAccessExpr:
        if (!InScript())
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "this operator exists only in scripts");
            return Error(node->Range);
        }
        return BindConditionalAccess(node->As<AstConditionalAccessExpr>());
    case AstKind::ConditionalReceiverExpr:
        if (_conditionalReceivers.IsEmpty())
        {
            return Error(node->Range);
        }
        return _conditionalReceivers.Last();
    case AstKind::SwitchExpr:
        return BindSwitchExpression(node->As<AstSwitchExpr>(), expected);
    case AstKind::CheckedExpr:
    {
        const bool previous = _checked;
        _checked = true;
        BoundExpr *value = BindExpression(node->As<AstCheckedExpr>()->Operand, expected);
        _checked = previous;
        return value;
    }
    case AstKind::RefExpr:
        _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, node->Range,
                                 "expected a `ref` local declaration, found {0}", StringView("`ref`"));
        return Error(node->Range);
    case AstKind::ArrayCreationExpr:
        return BindArrayCreation(node->As<AstArrayCreationExpr>());
    default:
        if (const auto *named = node->TryAs<AstNamedType>();
            named != nullptr && named->Qualifier == nullptr && !named->HasGenericArguments)
        {
            AstNameExpr name;
            name.Kind = AstKind::NameExpr;
            name.Range = named->Range;
            name.Name = named->Name;
            BinderTarget target = ResolveName(&name);
            return TargetToValue(target);
        }
        if (const auto *named = node->TryAs<AstNamedType>(); named != nullptr && named->Qualifier != nullptr)
        {
            // lang-lower: a qualified name parsed as a type, such as the generic value argument of
            // `RayQuery<RayFlags.CullBackFacingTriangles>`, is bound as the member access it also is.
            auto toExpression = [&](auto &self, AstNode *part) -> AstNode * {
                const auto *type = part->TryAs<AstNamedType>();
                if (type == nullptr)
                {
                    return part;
                }
                if (type->Qualifier == nullptr)
                {
                    auto *simple = _sema->New<AstNameExpr>();
                    simple->Kind = AstKind::NameExpr;
                    simple->Range = type->Range;
                    simple->Name = type->Name;
                    simple->GenericArguments = type->GenericArguments;
                    simple->HasGenericArguments = type->HasGenericArguments;
                    return simple;
                }
                auto *member = _sema->New<AstMemberExpr>();
                member->Kind = AstKind::MemberExpr;
                member->Range = type->Range;
                member->NameRange = type->Range;
                member->Target = self(self, type->Qualifier);
                member->Name = type->Name;
                member->GenericArguments = type->GenericArguments;
                member->HasGenericArguments = type->HasGenericArguments;
                return member;
            };
            return BindExpression(toExpression(toExpression, node), expected);
        }
        if (node->IsType())
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotAValue, node->Range, "a type is not a value")
                .WithHelp("to make a value of a vector or matrix type, call it: `float3(0, 1, 0)`");
            return Error(node->Range);
        }
        _sema->Diagnostics.Error(DiagnosticCode::UnexpectedToken, node->Range, "expected an expression");
        return Error(node->Range);
    }
}

BoundExpr *Binder::BindLiteral(const AstLiteralExpr *literal, const SemaType *expected)
{
    (void)expected;
    SemaTypeTable &types = _sema->Types;
    const Token &token = literal->Literal;
    SemaConstant *value = nullptr;
    switch (token.Kind)
    {
    case TokenKind::KeywordTrue:
    case TokenKind::KeywordFalse:
        value = _sema->NewBool(token.Kind == TokenKind::KeywordTrue);
        break;
    case TokenKind::KeywordNull:
    {
        SemaConstant *null = _sema->NewZero(types.GetNull());
        value = null;
        break;
    }
    case TokenKind::StringLiteral:
        value = _sema->NewString(DecodeStringLiteral(*_context.File->File, token).AsView());
        break;
    case TokenKind::RuneLiteral:
    {
        // `'a'` is a `Marlin.Rune`: its scalar value in the core library struct (MarlinScript §2.2).
        if (!InScript())
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, literal->Range, "rune literals exist only in scripts");
            return Error(literal->Range);
        }
        SemaSymbol *rune = _sema->FindQualified("Marlin.Rune");
        if (rune == nullptr || rune->Kind != SemaSymbolKind::Struct)
        {
            _sema->Diagnostics.Error(DiagnosticCode::UndefinedName, literal->Range, "cannot find `{0}`",
                                     StringView("Marlin.Rune"));
            return Error(literal->Range);
        }
        const SemaType *runeType = _sema->GetInstanceType(_sema->InstantiateStruct(rune, {}));
        SemaConstant *scalar[] = {_sema->NewScalar(types.GetInt(32, false), token.Integer)};
        value = _sema->NewComposite(runeType, Span<SemaConstant *const>(scalar));
        break;
    }
    case TokenKind::FloatLiteral:
        value = _sema->NewFloat(token.Suffix == LiteralSuffix::H   ? types.GetFloat(16)
                                : token.Suffix == LiteralSuffix::D ? types.GetFloat(64)
                                                                   : types.GetFloat(32),
                                token.Float);
        break;
    default:
    {
        const uint64 bits = token.Integer;
        const SemaType *type = nullptr;
        switch (token.Suffix)
        {
        case LiteralSuffix::U:
            type = bits <= 0xFFFFFFFFull ? types.GetInt(32, false) : types.GetInt(64, false);
            break;
        case LiteralSuffix::L:
            type = bits <= 0x7FFFFFFFFFFFFFFFull ? types.GetInt(64, true) : types.GetInt(64, false);
            break;
        case LiteralSuffix::UL:
            type = types.GetInt(64, false);
            break;
        default:
            type = bits <= 0x7FFFFFFFull           ? types.GetInt(32, true)
                   : bits <= 0xFFFFFFFFull         ? types.GetInt(32, false)
                   : bits <= 0x7FFFFFFFFFFFFFFFull ? types.GetInt(64, true)
                                                   : types.GetInt(64, false);
            break;
        }
        value = _sema->NewScalar(type, bits);
        break;
    }
    }
    return MakeConstant(*_sema, value, literal->Range);
}

BinderTarget Binder::ResolveTarget(AstNode *node)
{
    switch (node->Kind)
    {
    case AstKind::NameExpr:
        return ResolveName(node->As<AstNameExpr>());
    case AstKind::MemberExpr:
        return ResolveMember(node->As<AstMemberExpr>());
    case AstKind::PredefinedType:
    case AstKind::NamedType:
    case AstKind::ArrayType:
    case AstKind::NullableType:
    {
        BinderTarget target;
        target.Range = node->Range;
        target.Type = _sema->ResolveType(node, _context);
        target.TargetKind = target.Type->IsError() ? BinderTarget::Kind::Error : BinderTarget::Kind::Type;
        return target;
    }
    default:
    {
        BinderTarget target;
        target.Range = node->Range;
        target.Value = BindExpression(node);
        target.TargetKind = target.Value->Type->IsError() ? BinderTarget::Kind::Error : BinderTarget::Kind::Value;
        return target;
    }
    }
}

BinderTarget Binder::ResolveName(const AstNameExpr *name)
{
    BinderTarget target;
    target.Range = name->Range;
    target.GenericArguments = name->GenericArguments;
    target.HasGenericArguments = name->HasGenericArguments;
    if (!name->HasGenericArguments)
    {
        if (SemaLocal *local = FindLocal(name->Name))
        {
            target.TargetKind = BinderTarget::Kind::Value;
            if (local->Constant != nullptr)
            {
                target.Value = MakeConstant(*_sema, local->Constant, name->Range);
                return target;
            }
            if (local->Function != nullptr && local->Function != _function)
            {
                NoteCapture(local, name->Range);
            }
            auto *bound = MakeExpr<BoundLocal>(*_sema, local->Type, name->Range);
            bound->Local = local;
            bound->IsLValue = !local->IsReadOnly;
            target.Value = bound;
            return target;
        }
    }
    SemaLookup lookup;
    _sema->Lookup(_context, name->Name, lookup);
    if (lookup.Generic != nullptr)
    {
        if (lookup.Generic->Type != nullptr)
        {
            target.TargetKind = BinderTarget::Kind::Type;
            target.Type = lookup.Generic->Type;
        }
        else
        {
            target.TargetKind = BinderTarget::Kind::Value;
            target.Value = MakeConstant(*_sema, lookup.Generic->Value, name->Range);
        }
        return target;
    }
    if (lookup.Alias != nullptr)
    {
        SemaContext aliasContext;
        aliasContext.File = lookup.AliasFile;
        aliasContext.Decl = lookup.Alias->Parent;
        aliasContext.Dialect = lookup.AliasFile->File->GetDialect();
        target.Type = _sema->ResolveType(lookup.Alias->Target, aliasContext);
        target.TargetKind = target.Type->IsError() ? BinderTarget::Kind::Error : BinderTarget::Kind::Type;
        return target;
    }
    if (lookup.Ambiguous)
    {
        _sema->Diagnostics.Error(DiagnosticCode::AmbiguousName, name->Range, "`{0}` is ambiguous", name->Name)
            .WithLabel("found in more than one imported namespace");
        return target;
    }
    if (lookup.Symbols.IsEmpty())
    {
        _sema->Diagnostics.Error(DiagnosticCode::UndefinedName, name->Range, "cannot find `{0}` in this scope",
                                 name->Name)
            .WithLabel("not found in this scope");
        return target;
    }
    // A member of the enclosing type, or of one of its bases, is used through `this` when it is an instance member.
    BoundExpr *receiver = nullptr;
    SemaStructInstance *owner = nullptr;
    const SemaSymbol *parent = lookup.Symbols[0]->Parent;
    if (_context.Owner != nullptr && parent != nullptr && parent->IsTypeScope() && IsInsideType(parent))
    {
        owner = _context.Owner;
        const SemaFunction *thisOwner = ThisOwner();
        bool instance = false;
        for (const SemaSymbol *symbol : lookup.Symbols)
        {
            instance = instance || ((symbol->Kind == SemaSymbolKind::Function || symbol->Kind == SemaSymbolKind::Field ||
                                     symbol->Kind == SemaSymbolKind::Property) &&
                                    !symbol->IsStatic());
        }
        if (instance && thisOwner != nullptr && thisOwner->This != nullptr)
        {
            receiver = BindThis(name->Range);
            if (receiver->Type->IsError())
            {
                return target;
            }
        }
    }
    BinderTarget result = FromSymbols(lookup.Symbols.AsSpan(), owner, receiver, name->Range, name->Name);
    result.GenericArguments = name->GenericArguments;
    result.HasGenericArguments = name->HasGenericArguments;
    if (result.TargetKind == BinderTarget::Kind::Type && name->HasGenericArguments)
    {
        for (SemaSymbol *symbol : lookup.Symbols)
        {
            if (symbol->IsAggregate() && GenericParametersOf(symbol->Decl).Count() >= name->GenericArguments.Count())
            {
                result.Type = _sema->ResolveSymbolType(symbol, name->GenericArguments, _context, name->Range);
                result.TargetKind = result.Type->IsError() ? BinderTarget::Kind::Error : BinderTarget::Kind::Type;
                break;
            }
        }
    }
    return result;
}

BinderTarget Binder::FromSymbols(Span<SemaSymbol *const> symbols, SemaStructInstance *owner, BoundExpr *receiver,
                                 SourceRange range, StringView name)
{
    BinderTarget target;
    target.Range = range;
    for (SemaSymbol *symbol : symbols)
    {
        if (symbol->Kind == SemaSymbolKind::Function || symbol->Kind == SemaSymbolKind::Constructor)
        {
            target.Functions.Add(symbol);
        }
    }
    if (!target.Functions.IsEmpty())
    {
        target.TargetKind = BinderTarget::Kind::Functions;
        target.Owner = owner;
        target.Receiver = receiver;
        return target;
    }
    // Of types that share a name, such as `Task` and `Task<T>`, a name without generic arguments is the one
    // without generic parameters.
    SemaSymbol *symbol = symbols[0];
    for (SemaSymbol *candidate : symbols)
    {
        if (candidate->IsAggregate() && GenericParametersOf(candidate->Decl).IsEmpty())
        {
            symbol = candidate;
            break;
        }
    }
    _sema->UseFile(symbol->File);
    switch (symbol->Kind)
    {
    case SemaSymbolKind::Namespace:
        target.TargetKind = BinderTarget::Kind::Namespace;
        target.Namespace = symbol;
        return target;
    case SemaSymbolKind::Struct:
    case SemaSymbolKind::Class:
    case SemaSymbolKind::Interface:
    case SemaSymbolKind::Delegate:
    case SemaSymbolKind::Enum:
    case SemaSymbolKind::Alias:
        if (!GenericParametersOf(symbol->Decl).IsEmpty())
        {
            // A generic type named with its arguments is resolved by the caller.
            target.TargetKind = BinderTarget::Kind::Type;
            target.Type = _sema->Types.GetError();
            return target;
        }
        target.Type = _sema->ResolveSymbolType(symbol, {}, _context, range);
        target.TargetKind = target.Type->IsError() ? BinderTarget::Kind::Error : BinderTarget::Kind::Type;
        return target;
    case SemaSymbolKind::AssociatedType:
        // `T.Sample` written as an expression, as in `T.Sample.Create()` (§3.5).
        target.Type = _sema->ResolveAssociatedType(symbol, owner, nullptr, range);
        target.TargetKind = target.Type->IsError() ? BinderTarget::Kind::Error : BinderTarget::Kind::Type;
        return target;
    case SemaSymbolKind::EnumMember:
        if (SemaConstant *value = _sema->GetEnumMemberValue(symbol))
        {
            target.TargetKind = BinderTarget::Kind::Value;
            target.Value = MakeConstant(*_sema, value, range);
        }
        return target;
    case SemaSymbolKind::Property:
    {
        const AstDecl *decl = symbol->Decl;
        BoundExpr *value = InScript() && Sema::FindAttribute(decl->Attributes, "SystemValue") == nullptr
                               ? MakeProperty(symbol, owner, receiver, {}, range)
                               : CallGetter(symbol, owner, receiver, range);
        if (value != nullptr)
        {
            target.TargetKind = value->Type->IsError() ? BinderTarget::Kind::Error : BinderTarget::Kind::Value;
            target.Value = value;
        }
        return target;
    }
    case SemaSymbolKind::Field:
    {
        const AstDecl *decl = symbol->Decl;
        if (HasAnyFlags(decl->Modifiers, AstModifiers::Const) ||
            (HasAnyFlags(decl->Modifiers, AstModifiers::Static) && HasAnyFlags(decl->Modifiers, AstModifiers::Readonly) &&
             (symbol->File == nullptr || symbol->File->File->GetDialect() != LanguageDialect::Script)))
        {
            if (SemaConstant *value = _sema->GetStaticValue(symbol, owner))
            {
                target.TargetKind = BinderTarget::Kind::Value;
                target.Value = MakeConstant(*_sema, value, range);
            }
            return target;
        }
        // Script fields: class fields, static fields with storage, events (MarlinScript §3.3).
        const bool scriptField = (symbol->File != nullptr && symbol->File->File->GetDialect() == LanguageDialect::Script) ||
                                 (owner != nullptr && owner->Symbol->Kind == SemaSymbolKind::Class);
        if (scriptField)
        {
            return BindFieldSymbol(symbol, owner, receiver, range);
        }
        const bool isGlobal = Sema::FindAttribute(decl->Attributes, "GroupShared") != nullptr ||
                              Sema::FindAttribute(decl->Attributes, "Specialization") != nullptr;
        if (isGlobal)
        {
            SemaGlobal *global = _sema->GetGlobal(symbol);
            if (global->Kind == SemaGlobalKind::GroupShared && _context.Dialect != LanguageDialect::Shader)
            {
                _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, range, "groupshared memory exists only in shaders");
                return target;
            }
            if (global->Kind == SemaGlobalKind::Specialization && _context.Dialect != LanguageDialect::Shader)
            {
                _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, range,
                                         "specialization constants exist only in shaders");
                return target;
            }
            auto *bound = MakeExpr<BoundGlobal>(*_sema, global->Type, range);
            bound->Global = global;
            bound->IsLValue = global->Kind == SemaGlobalKind::GroupShared;
            target.TargetKind = global->Type->IsError() ? BinderTarget::Kind::Error : BinderTarget::Kind::Value;
            target.Value = bound;
            return target;
        }
        if (HasAnyFlags(decl->Modifiers, AstModifiers::Static))
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, range,
                                     "a static field must be `const`, `static readonly`, `[GroupShared]` or `[Specialization]`")
                .WithNote("shaders have no mutable global variables (§3.1)");
            return target;
        }
        if (receiver == nullptr || owner == nullptr)
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotAValue, range, "`{0}` is an instance field", name)
                .WithLabel("needs an object, such as `value.{0}`");
            return target;
        }
        if (!_sema->ResolveFields(owner))
        {
            return target;
        }
        for (usize i = 0; i < owner->Fields.Count(); ++i)
        {
            if (owner->Fields[i].Symbol == symbol)
            {
                auto *field = MakeExpr<BoundField>(*_sema, owner->Fields[i].Type, range);
                field->Object = receiver;
                field->FieldIndex = static_cast<uint32>(i);
                field->IsLValue = receiver->IsLValue;
                target.TargetKind = BinderTarget::Kind::Value;
                target.Value = field;
                return target;
            }
        }
        return target;
    }
    default:
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, range, "`{0}` cannot be used here", name);
        return target;
    }
}

BinderTarget Binder::ResolveMember(const AstMemberExpr *member)
{
    if (member->Target->Kind == AstKind::BaseExpr)
    {
        return ResolveBaseMember(member);
    }
    BinderTarget object = ResolveTarget(member->Target);
    BinderTarget target;
    target.Range = member->Range;
    switch (object.TargetKind)
    {
    case BinderTarget::Kind::Error:
        return target;
    case BinderTarget::Kind::Namespace:
    {
        List<SemaSymbol *> symbols;
        for (SemaSymbol *symbol : _sema->GetMembers(object.Namespace, member->Name))
        {
            if (_sema->IsAccessible(_context, symbol))
            {
                symbols.Add(symbol);
            }
        }
        if (symbols.IsEmpty())
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::UndefinedName, member->NameRange, "cannot find `{0}` in namespace `{1}`",
                       member->Name, object.Namespace->Name)
                .WithLabel("not found in this namespace");
            return target;
        }
        target = FromSymbols(symbols.AsSpan(), nullptr, nullptr, member->Range, member->Name);
        if (target.TargetKind == BinderTarget::Kind::Type && member->HasGenericArguments)
        {
            for (SemaSymbol *symbol : symbols)
            {
                if (symbol->IsType() && GenericParametersOf(symbol->Decl).Count() >= member->GenericArguments.Count())
                {
                    target.Type = _sema->ResolveSymbolType(symbol, member->GenericArguments, _context, member->Range);
                    break;
                }
            }
            target.TargetKind = target.Type->IsError() ? BinderTarget::Kind::Error : BinderTarget::Kind::Type;
        }
        target.GenericArguments = member->GenericArguments;
        target.HasGenericArguments = member->HasGenericArguments;
        return target;
    }
    case BinderTarget::Kind::Type:
        return ResolveStaticMember(object.Type, member);
    case BinderTarget::Kind::Value:
    {
        const SemaType *type = object.Value->Type;
        if (type->IsReference() || type->Is(SemaTypeKind::Nullable))
        {
            return ResolveReferenceMember(object.Value, member);
        }
        return ResolveInstanceMember(object.Value, member);
    }
    case BinderTarget::Kind::Functions:
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, member->Target->Range, "a function has no members")
            .WithHelp("call it first");
        return target;
    }
    return target;
}

BinderTarget Binder::ResolveStaticMember(const SemaType *type, const AstMemberExpr *member)
{
    BinderTarget target;
    target.Range = member->Range;
    target.GenericArguments = member->GenericArguments;
    target.HasGenericArguments = member->HasGenericArguments;
    if (member->Name == "Implements")
    {
        target.TargetKind = BinderTarget::Kind::Functions;
        target.TypeParameterTarget = type;
        return target;
    }
    SemaSymbol *symbol = nullptr;
    SemaStructInstance *owner = nullptr;
    List<SemaSymbol *> members;
    if (type->HasInstance())
    {
        owner = type->GetStruct();
        symbol = owner->Symbol;
        // A class's static members include those of its bases.
        _sema->GetInstanceMembers(owner, member->Name, members);
    }
    else if (type->Is(SemaTypeKind::Enum))
    {
        symbol = type->GetSymbol();
        members.AddRange(_sema->GetMembers(symbol, member->Name));
    }
    else
    {
        owner = MagicOwner(type);
        symbol = owner != nullptr ? owner->Symbol : nullptr;
        if (symbol != nullptr)
        {
            members.AddRange(_sema->GetMembers(symbol, member->Name));
        }
    }
    List<SemaSymbol *> accessible;
    for (SemaSymbol *candidate : members)
    {
        if (_sema->IsAccessible(_context, candidate))
        {
            accessible.Add(candidate);
        }
    }
    if (accessible.IsEmpty())
    {
        bool found = false;
        BinderTarget extension = ResolveExtensionMember(type, nullptr, member, found);
        if (found)
        {
            return extension;
        }
        _sema->Diagnostics
            .Error(DiagnosticCode::UndefinedName, member->NameRange, "`{0}` has no member `{1}`",
                   TypeNameOf(type).AsView(), member->Name)
            .WithLabel("unknown member");
        return target;
    }
    BinderTarget result = FromSymbols(accessible.AsSpan(), owner, nullptr, member->Range, member->Name);
    result.GenericArguments = member->GenericArguments;
    result.HasGenericArguments = member->HasGenericArguments;
    return result;
}

BinderTarget Binder::ResolveInstanceMember(BoundExpr *object, const AstMemberExpr *member)
{
    BinderTarget target;
    target.Range = member->Range;
    const SemaType *type = object->Type;
    if (type->Is(SemaTypeKind::Buffer) && type->GetElement() != nullptr && member->Name == "Value")
    {
        // `p.Value` is `p[0]` (§4.5).
        const SemaType *uintType = _sema->Types.GetInt(32, false);
        auto *zero = MakeExpr<BoundConstant>(*_sema, uintType, member->Range);
        zero->Value = _sema->NewScalar(uintType, 0);
        auto *element = MakeExpr<BoundBufferElement>(*_sema, type->GetElement(), member->Range);
        element->Buffer = object;
        element->Index = zero;
        element->IsLValue = type->IsWritable();
        target.TargetKind = BinderTarget::Kind::Value;
        target.Value = element;
        return target;
    }
    if (type->Is(SemaTypeKind::Vector) || type->IsScalar())
    {
        const uint32 count = type->Is(SemaTypeKind::Vector) ? type->GetCount() : 1;
        const StringView name = member->Name;
        bool rgba = false;
        bool xyzw = false;
        bool valid = name.Length() >= 1 && name.Length() <= 4;
        bool repeated = false;
        uint8 components[4] = {};
        for (usize i = 0; valid && i < name.Length(); ++i)
        {
            bool isRgba = false;
            const int32 index = SwizzleIndex(name[i], isRgba);
            rgba = rgba || isRgba;
            xyzw = xyzw || (index >= 0 && !isRgba);
            valid = index >= 0 && static_cast<uint32>(index) < count;
            if (valid)
            {
                components[i] = static_cast<uint8>(index);
                for (usize j = 0; j < i; ++j)
                {
                    repeated = repeated || components[j] == components[i];
                }
            }
        }
        if (valid && !(rgba && xyzw))
        {
            const SemaType *scalar = type->GetScalar();
            const SemaType *result =
                name.Length() == 1 ? scalar : _sema->Types.GetVector(scalar, static_cast<uint32>(name.Length()));
            auto *swizzle = MakeExpr<BoundSwizzle>(*_sema, result, member->Range);
            swizzle->Object = object;
            swizzle->Count = static_cast<uint8>(name.Length());
            MemCopy(swizzle->Components, components, sizeof(components));
            swizzle->IsLValue = object->IsLValue && !repeated && type->Is(SemaTypeKind::Vector);
            target.TargetKind = BinderTarget::Kind::Value;
            target.Value = Fold(swizzle);
            return target;
        }
        bool found = false;
        BinderTarget added = ResolveExtensionMember(type, object, member, found);
        if (found)
        {
            return added;
        }
        BinderTarget extension = FindExtensions(object, member);
        if (extension.TargetKind == BinderTarget::Kind::Functions)
        {
            return extension;
        }
        if (type->IsScalar() || name.Length() <= 4)
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::UndefinedName, member->NameRange, "`{0}` is not a component of `{1}`", name,
                       TypeNameOf(type).AsView())
                .WithHelp("swizzles use `xyzw` or `rgba`, not mixed, within the vector's size");
            return target;
        }
    }
    SemaSymbol *symbol = nullptr;
    SemaStructInstance *owner = nullptr;
    if (type->Is(SemaTypeKind::Struct))
    {
        owner = type->GetStruct();
        symbol = owner->Symbol;
    }
    else
    {
        owner = MagicOwner(type);
        symbol = owner != nullptr ? owner->Symbol : nullptr;
    }
    const Span<SemaSymbol *const> members =
        symbol != nullptr ? _sema->GetMembers(symbol, member->Name) : Span<SemaSymbol *const>();
    if (members.IsEmpty())
    {
        bool found = false;
        BinderTarget added = ResolveExtensionMember(type, object, member, found);
        if (found)
        {
            return added;
        }
        BinderTarget extension = FindExtensions(object, member);
        if (extension.TargetKind == BinderTarget::Kind::Functions)
        {
            return extension;
        }
        if (!type->IsError())
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::UndefinedName, member->NameRange, "`{0}` has no member `{1}`",
                       TypeNameOf(type).AsView(), member->Name)
                .WithLabel("unknown member");
        }
        return target;
    }
    BinderTarget result = FromSymbols(members, owner, object, member->Range, member->Name);
    result.GenericArguments = member->GenericArguments;
    result.HasGenericArguments = member->HasGenericArguments;
    return result;
}

BinderTarget Binder::ResolveExtensionMember(const SemaType *type, BoundExpr *receiver, const AstMemberExpr *member,
                                            bool &found)
{
    List<SemaSymbol *> members;
    List<SemaStructInstance *> owners;
    _sema->FindExtensionMembers(type, member->Name, _context, member->NameRange, members, owners);
    found = !members.IsEmpty();
    if (!found)
    {
        BinderTarget target;
        target.Range = member->Range;
        return target;
    }
    SemaStructInstance *owner = owners[0];
    for (SemaStructInstance *extension : owners)
    {
        owner = extension->Symbol == members[0]->Parent ? extension : owner;
    }
    BinderTarget result = FromSymbols(members.AsSpan(), owner, receiver, member->Range, member->Name);
    result.Extensions = Move(owners);
    result.GenericArguments = member->GenericArguments;
    result.HasGenericArguments = member->HasGenericArguments;
    return result;
}

BinderTarget Binder::FindExtensions(BoundExpr *object, const AstMemberExpr *member)
{
    BinderTarget target;
    target.Range = member->Range;
    // Extension methods are declared in namespaces; members of the enclosing types do not hide them.
    SemaContext namespaces = _context;
    namespaces.TypeSymbol = nullptr;
    namespaces.Owner = nullptr;
    SemaLookup lookup;
    _sema->Lookup(namespaces, member->Name, lookup);
    for (SemaSymbol *symbol : lookup.Symbols)
    {
        const auto *function = symbol->Kind == SemaSymbolKind::Function ? symbol->Decl->TryAs<AstFunctionDecl>() : nullptr;
        if (function != nullptr && !function->Parameters.IsEmpty() &&
            HasAnyFlags(function->Parameters[0]->Modifiers, AstModifiers::This))
        {
            target.Functions.Add(symbol);
        }
    }
    if (!target.Functions.IsEmpty())
    {
        target.TargetKind = BinderTarget::Kind::Functions;
        target.ExtensionReceiver = object;
        target.GenericArguments = member->GenericArguments;
        target.HasGenericArguments = member->HasGenericArguments;
    }
    return target;
}

BoundExpr *Binder::TargetToValue(BinderTarget &target)
{
    switch (target.TargetKind)
    {
    case BinderTarget::Kind::Value:
        return target.Value;
    case BinderTarget::Kind::Type:
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, target.Range, "`{0}` is a type, not a value",
                                 TypeNameOf(target.Type).AsView())
            .WithHelp("to make a value, call it or use `new`");
        break;
    case BinderTarget::Kind::Namespace:
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, target.Range, "`{0}` is a namespace, not a value",
                                 target.Namespace->Name);
        break;
    case BinderTarget::Kind::Functions:
        // In scripts a method group is a value that becomes a delegate where one is expected (MarlinScript §3.7).
        if (InScript() && target.TypeParameterTarget == nullptr && target.ExtensionReceiver == nullptr)
        {
            return MakeUnbound(target, nullptr, target.Range);
        }
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, target.Range, "a function is not a value")
            .WithHelp("call it with `(…)`");
        break;
    case BinderTarget::Kind::Error:
        break;
    }
    return Error(target.Range);
}

const SemaType *Binder::CommonScalar(BoundExpr *left, BoundExpr *right)
{
    const SemaType *a = left->Type->GetScalar();
    const SemaType *b = right->Type->GetScalar();
    if (a == nullptr || b == nullptr)
    {
        return nullptr;
    }
    if (a == b)
    {
        return a;
    }
    if (a->Is(SemaTypeKind::Bool) || b->Is(SemaTypeKind::Bool))
    {
        return nullptr;
    }
    const SemaConstant *leftConstant =
        left->Kind == BoundKind::Constant && left->Type->IsScalar() ? left->As<BoundConstant>()->Value : nullptr;
    const SemaConstant *rightConstant =
        right->Kind == BoundKind::Constant && right->Type->IsScalar() ? right->As<BoundConstant>()->Value : nullptr;
    if (rightConstant != nullptr && leftConstant == nullptr && _sema->FitsExactly(rightConstant, a))
    {
        return a;
    }
    if (leftConstant != nullptr && rightConstant == nullptr && _sema->FitsExactly(leftConstant, b))
    {
        return b;
    }
    if (a->Is(SemaTypeKind::Float) || b->Is(SemaTypeKind::Float))
    {
        if (a->Is(SemaTypeKind::Float) && b->Is(SemaTypeKind::Float))
        {
            return a->GetWidth() >= b->GetWidth() ? a : b;
        }
        return a->Is(SemaTypeKind::Float) ? a : b;
    }
    if (a->IsSigned() == b->IsSigned())
    {
        return a->GetWidth() >= b->GetWidth() ? a : b;
    }
    const SemaType *signedType = a->IsSigned() ? a : b;
    const SemaType *unsignedType = a->IsSigned() ? b : a;
    if (signedType->GetWidth() > unsignedType->GetWidth())
    {
        return signedType;
    }
    return unsignedType->GetWidth() < 64 ? _sema->Types.GetInt(unsignedType->GetWidth() * 2, true) : nullptr;
}

BoundExpr *Binder::Convert(BoundExpr *expression, const SemaType *type, bool isExplicit)
{
    const SemaType *from = expression->Type;
    if (from == type || from->IsError() || type->IsError())
    {
        return expression;
    }
    if (expression->Kind == BoundKind::Unbound)
    {
        return ConvertToDelegate(expression, type);
    }
    // `null!` and `default!` give the zero value of any type (MarlinScript §3.6).
    if (from->Is(SemaTypeKind::Null) && expression->Kind == BoundKind::TypeConversion &&
        expression->As<BoundTypeConversion>()->Conversion == BoundTypeConversionKind::RemoveNullable)
    {
        return MakeExpr<BoundZero>(*_sema, type, expression->Range);
    }
    bool handled = false;
    BoundExpr *reference = ConvertReference(expression, type, isExplicit, handled);
    if (handled)
    {
        return reference;
    }
    if (from->Is(SemaTypeKind::Null))
    {
        if (type->Is(SemaTypeKind::Struct) || type->IsHandle() || type->IsArithmetic() || type->Is(SemaTypeKind::Enum) ||
            type->Is(SemaTypeKind::Array))
        {
            return MakeExpr<BoundZero>(*_sema, type, expression->Range);
        }
    }
    if (BoundExpr *pointer = ConvertPointer(expression, type))
    {
        return pointer;
    }
    const SemaConstant *constant =
        expression->Kind == BoundKind::Constant ? expression->As<BoundConstant>()->Value : nullptr;
    const SemaConversion conversion = _sema->ClassifyConversion(from, type, constant);
    if (conversion == SemaConversion::None || (conversion == SemaConversion::Explicit && !isExplicit))
    {
        bool found = false;
        if (from->Is(SemaTypeKind::Struct) || type->Is(SemaTypeKind::Struct))
        {
            BoundExpr *operand[] = {expression};
            BoundExpr *user = BindUserOperator(isExplicit ? TokenKind::KeywordExplicit : TokenKind::KeywordImplicit,
                                               Span<BoundExpr *>(operand), expression->Range, found);
            if (found && user->Type == type)
            {
                return user;
            }
        }
        LanguageDiagnostic &error = _sema->Diagnostics.Error(
            DiagnosticCode::NoConversion, expression->Range, "cannot convert `{0}` to `{1}`{2}", TypeNameOf(from).AsView(),
            TypeNameOf(type).AsView(), StringView(conversion == SemaConversion::Explicit ? " implicitly" : ""));
        error.WithLabel("this expression");
        if (conversion == SemaConversion::Explicit)
        {
            error.WithHelp("convert explicitly with a cast if this is intended");
        }
        else if (from->Is(SemaTypeKind::Vector) && type->Is(SemaTypeKind::Vector))
        {
            error.WithHelp("vectors convert only between the same number of components; use a swizzle such as `.xyz`");
        }
        return Error(expression->Range);
    }
    if (conversion == SemaConversion::Identity)
    {
        if (from->Is(SemaTypeKind::Int) && type->Is(SemaTypeKind::Int))
        {
            auto *convert = MakeExpr<BoundConvert>(*_sema, type, expression->Range);
            convert->Operand = expression;
            return Fold(convert);
        }
        if (from->Is(SemaTypeKind::Texture) && from != type)
        {
            auto *bitcast = MakeExpr<BoundBitcast>(*_sema, type, expression->Range);
            bitcast->Operand = expression;
            return bitcast;
        }
        return expression;
    }
    auto *convert = MakeExpr<BoundConvert>(*_sema, type, expression->Range);
    convert->Operand = expression;
    return Fold(convert);
}

BoundExpr *Binder::ConvertPointer(BoundExpr *expression, const SemaType *type)
{
    // GPU memory (§4.5): a writable Ptr or Span converts to a read-only one of the same element, and a Span to its Ptr.
    const auto isSpan = [&](const SemaType *candidate) {
        return candidate->Is(SemaTypeKind::Struct) &&
               candidate->GetStruct()->Symbol->BuiltinId == static_cast<uint32>(BuiltinTypeId::Span) &&
               _sema->ResolveFields(candidate->GetStruct());
    };
    const auto narrows = [](const SemaType *source, const SemaType *target) {
        return source->Is(SemaTypeKind::Buffer) && target->Is(SemaTypeKind::Buffer) &&
               source->GetElement() == target->GetElement() && (source == target || !target->IsWritable());
    };
    const auto reinterpret = [&](BoundExpr *operand, const SemaType *target) -> BoundExpr * {
        if (operand->Type == target)
        {
            return operand;
        }
        auto *bitcast = MakeExpr<BoundBitcast>(*_sema, target, operand->Range);
        bitcast->Operand = operand;
        return bitcast;
    };
    const SemaType *from = expression->Type;
    if (narrows(from, type))
    {
        return reinterpret(expression, type);
    }
    if (!isSpan(from))
    {
        return nullptr;
    }
    // Fields 0 and 1 of a Span are Pointer and Length.
    const auto field = [&](BoundExpr *object, uint32 index) {
        auto *node = MakeExpr<BoundField>(*_sema, from->GetStruct()->Fields[index].Type, expression->Range);
        node->Object = object;
        node->FieldIndex = index;
        return node;
    };
    const SemaType *pointer = from->GetStruct()->Fields[0].Type;
    if (narrows(pointer, type))
    {
        return reinterpret(field(expression, 0), type);
    }
    if (!isSpan(type) || !narrows(pointer, type->GetStruct()->Fields[0].Type))
    {
        return nullptr;
    }
    SemaLocal *temporary = nullptr;
    BoundExpr *input = StoreInput(expression, temporary);
    BoundExpr *elements[] = {reinterpret(field(input, 0), type->GetStruct()->Fields[0].Type), field(input, 1)};
    auto *span = MakeExpr<BoundConstruct>(*_sema, type, expression->Range);
    span->Elements = _sema->Freeze(List<BoundExpr *>(Span<BoundExpr *const>(elements)));
    return temporary != nullptr ? MakeLet(temporary, expression, span) : span;
}

BoundExpr *Binder::ConvertImplicit(BoundExpr *expression, const SemaType *type)
{
    return Convert(expression, type, false);
}

BoundExpr *Binder::BindUnary(const AstUnaryExpr *unary)
{
    BoundExpr *operand = BindExpression(unary->Operand);
    const SemaType *type = operand->Type;
    if (type->IsError())
    {
        return operand;
    }
    if (type->Is(SemaTypeKind::Struct) || type->Is(SemaTypeKind::Class))
    {
        bool found = false;
        BoundExpr *operands[] = {operand};
        BoundExpr *result = BindUserOperator(unary->Operator, Span<BoundExpr *>(operands), unary->Range, found);
        if (found)
        {
            return result;
        }
    }
    switch (unary->Operator)
    {
    case TokenKind::Plus:
        if (type->IsArithmetic() && !IsBoolLike(type))
        {
            return operand;
        }
        break;
    case TokenKind::Minus:
        if (type->IsArithmetic() && !IsBoolLike(type))
        {
            auto *negate = MakeExpr<BoundUnary>(*_sema, type, unary->Range);
            negate->Op = MirOp::Negate;
            negate->Operand = operand;
            negate->Checked = _checked && type->GetScalar()->Is(SemaTypeKind::Int);
            return Fold(negate);
        }
        break;
    case TokenKind::Exclamation:
        if (IsBoolLike(type))
        {
            auto *notExpr = MakeExpr<BoundUnary>(*_sema, type, unary->Range);
            notExpr->Op = MirOp::LogicalNot;
            notExpr->Operand = operand;
            return Fold(notExpr);
        }
        break;
    case TokenKind::Tilde:
        if (IsIntegerLike(type) || IsFlagsEnum(type))
        {
            auto *notExpr = MakeExpr<BoundUnary>(*_sema, type, unary->Range);
            notExpr->Op = MirOp::BitNot;
            notExpr->Operand = operand;
            return Fold(notExpr);
        }
        break;
    case TokenKind::PlusPlus:
    case TokenKind::MinusMinus:
        if (type->IsArithmetic() && !IsBoolLike(type) && !type->Is(SemaTypeKind::Matrix))
        {
            if (!CanAssign(operand))
            {
                return Error(unary->Range);
            }
            auto *incDec = MakeExpr<BoundIncDec>(*_sema, type, unary->Range);
            incDec->Target = operand;
            incDec->Increment = unary->Operator == TokenKind::PlusPlus;
            incDec->Postfix = unary->Postfix;
            return incDec;
        }
        break;
    default:
        break;
    }
    if (!type->Is(SemaTypeKind::Struct) && !type->Is(SemaTypeKind::Class))
    {
        // Where no built-in operator fits, an extension may add one, also for vectors and scalars (§3.5).
        bool found = false;
        BoundExpr *operands[] = {operand};
        if (BoundExpr *result = BindUserOperator(unary->Operator, Span<BoundExpr *>(operands), unary->Range, found);
            found)
        {
            return result;
        }
    }
    _sema->Diagnostics
        .Error(DiagnosticCode::InvalidOperands, unary->Range, "the operator `{0}` cannot be applied to `{1}`",
               GetOperatorText(unary->Operator), TypeNameOf(type).AsView())
        .WithSecondaryLabel(operand->Range, "this has the wrong type");
    return Error(unary->Range);
}

BoundExpr *Binder::BindBinary(const AstBinaryExpr *binary)
{
    BoundExpr *left = BindExpression(binary->Left);
    if (binary->Operator == TokenKind::QuestionQuestion)
    {
        if (!InScript())
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, binary->Range, "`??` exists only in scripts");
            BindExpression(binary->Right);
            return Error(binary->Range);
        }
        return BindCoalesce(left, binary->Right, binary->Range);
    }
    BoundExpr *right = BindExpression(binary->Right);
    return BindBinaryOperands(binary->Operator, left, right, binary->Range);
}

BoundExpr *Binder::BindBinaryOperands(TokenKind op, BoundExpr *left, BoundExpr *right, SourceRange range)
{
    const SemaType *leftType = left->Type;
    const SemaType *rightType = right->Type;
    if (leftType->IsError() || rightType->IsError())
    {
        return Error(range);
    }
    auto report = [&]() {
        // Where no built-in operator fits, an extension may add one, also for vectors and scalars (§3.5).
        bool found = false;
        BoundExpr *operands[] = {left, right};
        if (BoundExpr *user = BindUserOperator(op, Span<BoundExpr *>(operands), range, found); found)
        {
            return user;
        }
        _sema->Diagnostics
            .Error(DiagnosticCode::InvalidOperands, range, "no operator `{0}` for `{1}` and `{2}`",
                   GetOperatorText(op), TypeNameOf(leftType).AsView(), TypeNameOf(rightType).AsView())
            .WithSecondaryLabel(left->Range, TypeNameOf(leftType).AsView())
            .WithSecondaryLabel(right->Range, TypeNameOf(rightType).AsView());
        return Error(range);
    };
    // Strings, delegates and references (MarlinScript §2.3).
    bool handled = false;
    BoundExpr *reference = BindReferenceOperator(op, left, right, range, handled);
    if (handled)
    {
        return reference;
    }
    if (leftType->Is(SemaTypeKind::Struct) || rightType->Is(SemaTypeKind::Struct) ||
        leftType->Is(SemaTypeKind::Class) || rightType->Is(SemaTypeKind::Class) || leftType->Is(SemaTypeKind::Buffer))
    {
        bool found = false;
        BoundExpr *operands[] = {left, right};
        BoundExpr *result = BindUserOperator(op, Span<BoundExpr *>(operands), range, found);
        return found ? result : report();
    }
    if (leftType->Is(SemaTypeKind::Nullable) || rightType->Is(SemaTypeKind::Nullable))
    {
        // `T?` takes part in arithmetic once its value is taken out, `x.Value` or `x ?? 0`.
        return report();
    }

    if (op == TokenKind::AmpersandAmpersand || op == TokenKind::PipePipe)
    {
        const SemaType *boolType = _sema->Types.GetBool();
        if (!leftType->Is(SemaTypeKind::Bool) || !rightType->Is(SemaTypeKind::Bool))
        {
            if (IsBoolLike(leftType) && IsBoolLike(rightType))
            {
                _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, range,
                                         "`&&` and `||` take bool scalars")
                    .WithHelp("combine bool vectors with `&`, `|` or `select`");
                return Error(range);
            }
            return report();
        }
        auto *shortCircuit = MakeExpr<BoundShortCircuit>(*_sema, boolType, range);
        shortCircuit->IsAnd = op == TokenKind::AmpersandAmpersand;
        shortCircuit->Left = left;
        shortCircuit->Right = right;
        return Fold(shortCircuit);
    }

    auto makeBinary = [&](MirOp mirOp, const SemaType *type, BoundExpr *a, BoundExpr *b) -> BoundExpr * {
        auto *node = MakeExpr<BoundBinary>(*_sema, type, range);
        node->Op = mirOp;
        node->Left = a;
        node->Right = b;
        const SemaType *scalar = type->GetScalar();
        node->Checked = _checked && scalar != nullptr && scalar->Is(SemaTypeKind::Int) &&
                        (mirOp == MirOp::Add || mirOp == MirOp::Sub || mirOp == MirOp::Mul || mirOp == MirOp::Div);
        return Fold(node);
    };

    auto comparisonOp = [&]() -> MirOp {
        switch (op)
        {
        case TokenKind::Equal:
            return MirOp::Equal;
        case TokenKind::NotEqual:
            return MirOp::NotEqual;
        case TokenKind::Less:
            return MirOp::Less;
        case TokenKind::LessEqual:
            return MirOp::LessEqual;
        case TokenKind::Greater:
            return MirOp::Greater;
        case TokenKind::GreaterEqual:
            return MirOp::GreaterEqual;
        default:
            return MirOp::Unreachable;
        }
    };
    const MirOp comparison = comparisonOp();

    if (leftType->Is(SemaTypeKind::Enum) || rightType->Is(SemaTypeKind::Enum))
    {
        if (leftType != rightType)
        {
            if (leftType->Is(SemaTypeKind::Enum) && right->Kind == BoundKind::Constant &&
                rightType->Is(SemaTypeKind::Int) && right->As<BoundConstant>()->Value->Bits == 0)
            {
                right = MakeExpr<BoundZero>(*_sema, leftType, right->Range);
            }
            else
            {
                return report();
            }
        }
        if (comparison != MirOp::Unreachable)
        {
            return makeBinary(comparison, _sema->Types.GetBool(), left, right);
        }
        if (IsFlagsEnum(leftType) && (op == TokenKind::Pipe || op == TokenKind::Ampersand || op == TokenKind::Caret))
        {
            return makeBinary(op == TokenKind::Pipe        ? MirOp::BitOr
                              : op == TokenKind::Ampersand ? MirOp::BitAnd
                                                           : MirOp::BitXor,
                              leftType, left, right);
        }
        return report();
    }

    const auto isCooperative = [](const SemaType *type) {
        return type->Is(SemaTypeKind::CoopVector) || type->Is(SemaTypeKind::CoopMatrix);
    };
    if (isCooperative(leftType) || isCooperative(rightType))
    {
        // lang-lower: cooperative vectors and matrices add, subtract, multiply and divide element by element, and
        // scale by a scalar of their element type (§9.9).
        const MirOp arithmetic = op == TokenKind::Plus    ? MirOp::Add
                                 : op == TokenKind::Minus ? MirOp::Sub
                                 : op == TokenKind::Star  ? MirOp::Mul
                                 : op == TokenKind::Slash ? MirOp::Div
                                                          : MirOp::Unreachable;
        if (arithmetic == MirOp::Unreachable)
        {
            return report();
        }
        if (leftType == rightType && !(leftType->Is(SemaTypeKind::CoopMatrix) && arithmetic != MirOp::Add &&
                                       arithmetic != MirOp::Sub))
        {
            return makeBinary(arithmetic, leftType, left, right);
        }
        const bool leftCooperative = isCooperative(leftType);
        BoundExpr *cooperative = leftCooperative ? left : right;
        BoundExpr *scalar = leftCooperative ? right : left;
        if (arithmetic != MirOp::Mul || !scalar->Type->IsScalar())
        {
            return report();
        }
        scalar = ConvertImplicit(scalar, cooperative->Type->GetElement());
        if (scalar->Type->IsError())
        {
            return scalar;
        }
        return makeBinary(cooperative->Type->Is(SemaTypeKind::CoopVector) ? MirOp::VectorTimesScalar
                                                                           : MirOp::MatrixTimesScalar,
                          cooperative->Type, cooperative, scalar);
    }

    if (!leftType->IsArithmetic() || !rightType->IsArithmetic())
    {
        if (comparison == MirOp::Equal || comparison == MirOp::NotEqual)
        {
            if (leftType == rightType && (leftType->Is(SemaTypeKind::Feature) || leftType->Is(SemaTypeKind::String)))
            {
                return makeBinary(comparison, _sema->Types.GetBool(), left, right);
            }
        }
        return report();
    }

    const bool leftMatrix = leftType->Is(SemaTypeKind::Matrix);
    const bool rightMatrix = rightType->Is(SemaTypeKind::Matrix);
    if (op == TokenKind::Star && (leftMatrix || rightMatrix))
    {
        const SemaType *scalar = CommonScalar(left, right);
        if (scalar == nullptr || !scalar->IsNumeric())
        {
            return report();
        }
        left = ConvertImplicit(left, _sema->Types.WithScalar(leftType, scalar));
        right = ConvertImplicit(right, _sema->Types.WithScalar(rightType, scalar));
        leftType = left->Type;
        rightType = right->Type;
        if (leftMatrix && rightMatrix)
        {
            if (leftType->GetCount() != rightType->GetRows())
            {
                _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, range,
                                         "cannot multiply a {0}x{1} matrix by a {2}x{3} matrix", leftType->GetRows(),
                                         leftType->GetCount(), rightType->GetRows(), rightType->GetCount())
                    .WithHelp("for a component-wise product use `a.MultiplyComponents(b)`");
                return Error(range);
            }
            return makeBinary(MirOp::MatrixTimesMatrix,
                              _sema->Types.GetMatrix(scalar, leftType->GetRows(), rightType->GetCount()), left, right);
        }
        if (leftMatrix && rightType->Is(SemaTypeKind::Vector))
        {
            if (rightType->GetCount() != leftType->GetCount())
            {
                return report();
            }
            return makeBinary(MirOp::MatrixTimesVector, _sema->Types.GetVector(scalar, leftType->GetRows()), left,
                              right);
        }
        if (rightMatrix && leftType->Is(SemaTypeKind::Vector))
        {
            if (leftType->GetCount() != rightType->GetRows())
            {
                return report();
            }
            return makeBinary(MirOp::VectorTimesMatrix, _sema->Types.GetVector(scalar, rightType->GetCount()), left,
                              right);
        }
        if (leftMatrix)
        {
            return makeBinary(MirOp::MatrixTimesScalar, leftType, left, right);
        }
        return makeBinary(MirOp::MatrixTimesScalar, rightType, right, left);
    }

    // A shift keeps the type of its left operand; the count is any integer (§5.2).
    if ((op == TokenKind::LessLess || op == TokenKind::GreaterGreater) && IsIntegerLike(leftType) &&
        IsIntegerLike(rightType) && (rightType->IsScalar() || rightType->GetCount() == leftType->GetCount()))
    {
        const SemaType *countType =
            _sema->Types.WithScalar(leftType->IsScalar() ? rightType : leftType, leftType->GetScalar());
        BoundExpr *count = Convert(right, countType, true);
        return makeBinary(op == TokenKind::LessLess ? MirOp::ShiftLeft : MirOp::ShiftRight, leftType, left,
                          leftType->IsScalar() ? count : Convert(count, leftType, true));
    }
    const SemaType *scalar = CommonScalar(left, right);
    if (scalar == nullptr)
    {
        return report();
    }
    const SemaType *shape = leftType;
    if (leftType->IsScalar())
    {
        shape = rightType;
    }
    else if (!rightType->IsScalar() &&
             (leftType->GetKind() != rightType->GetKind() || leftType->GetCount() != rightType->GetCount() ||
              leftType->GetRows() != rightType->GetRows()))
    {
        _sema->Diagnostics
            .Error(DiagnosticCode::InvalidOperands, range, "the operands `{0}` and `{1}` have different shapes",
                   TypeNameOf(leftType).AsView(), TypeNameOf(rightType).AsView())
            .WithSecondaryLabel(left->Range, TypeNameOf(leftType).AsView())
            .WithSecondaryLabel(right->Range, TypeNameOf(rightType).AsView())
            .WithHelp("vectors and matrices combine only with the same size, or with a scalar");
        return Error(range);
    }
    const SemaType *type = _sema->Types.WithScalar(shape, scalar);

    if (op == TokenKind::LessLess || op == TokenKind::GreaterGreater)
    {
        if (!IsIntegerLike(leftType) || !IsIntegerLike(rightType))
        {
            return report();
        }
        right = ConvertImplicit(right, _sema->Types.WithScalar(leftType->IsScalar() ? rightType : leftType,
                                                               leftType->GetScalar()));
        return makeBinary(op == TokenKind::LessLess ? MirOp::ShiftLeft : MirOp::ShiftRight, leftType, left,
                          ConvertImplicit(right, leftType));
    }

    left = ConvertImplicit(left, type);
    right = ConvertImplicit(right, type);
    if (left->Type->IsError() || right->Type->IsError())
    {
        return Error(range);
    }
    if (comparison != MirOp::Unreachable)
    {
        if (scalar->Is(SemaTypeKind::Bool) && comparison != MirOp::Equal && comparison != MirOp::NotEqual)
        {
            return report();
        }
        const SemaType *result = _sema->Types.WithScalar(type, _sema->Types.GetBool());
        if (type->Is(SemaTypeKind::Matrix))
        {
            return report();
        }
        return makeBinary(comparison, result, left, right);
    }
    switch (op)
    {
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
        if (scalar->Is(SemaTypeKind::Bool))
        {
            return report();
        }
        return makeBinary(op == TokenKind::Plus    ? MirOp::Add
                          : op == TokenKind::Minus ? MirOp::Sub
                          : op == TokenKind::Star  ? MirOp::Mul
                          : op == TokenKind::Slash ? MirOp::Div
                                                   : MirOp::Rem,
                          type, left, right);
    case TokenKind::Ampersand:
    case TokenKind::Pipe:
    case TokenKind::Caret:
        if (scalar->Is(SemaTypeKind::Bool))
        {
            return makeBinary(op == TokenKind::Ampersand ? MirOp::LogicalAnd
                              : op == TokenKind::Pipe    ? MirOp::LogicalOr
                                                         : MirOp::NotEqual,
                              type, left, right);
        }
        if (!scalar->Is(SemaTypeKind::Int) || type->Is(SemaTypeKind::Matrix))
        {
            return report();
        }
        return makeBinary(op == TokenKind::Ampersand ? MirOp::BitAnd
                          : op == TokenKind::Pipe    ? MirOp::BitOr
                                                     : MirOp::BitXor,
                          type, left, right);
    default:
        return report();
    }
}

BoundExpr *Binder::BindAssign(const AstAssignExpr *assign)
{
    // `e += handler` and `e -= handler` may name an event outside its class (MarlinScript §3.7).
    const bool previous = _eventCompound;
    _eventCompound =
        assign->Operator == TokenKind::PlusAssign || assign->Operator == TokenKind::MinusAssign;
    BoundExpr *target = BindExpression(assign->Target);
    _eventCompound = previous;
    if (target->Type->IsError())
    {
        BindExpression(assign->Value);
        return target;
    }
    if (!CanAssign(target))
    {
        BindExpression(assign->Value);
        return Error(assign->Range);
    }
    auto *node = MakeExpr<BoundAssign>(*_sema, target->Type, assign->Range);
    node->Target = target;
    if (assign->Operator == TokenKind::QuestionQuestionAssign)
    {
        // `x ??= y` assigns `x ?? y`; the value reads the target, which lowering evaluates once.
        if (!InScript())
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, assign->Range, "`??` exists only in scripts");
            return Error(assign->Range);
        }
        BoundExpr *combined = BindCoalesce(target, assign->Value, assign->Range);
        if (combined->Type->IsError())
        {
            return combined;
        }
        node->IsCompound = true;
        node->Combined = true;
        node->Value = ConvertImplicit(combined, target->Type);
        return node;
    }
    BoundExpr *value = BindExpression(assign->Value, target->Type);
    if (assign->Operator == TokenKind::Assign)
    {
        node->Value = ConvertImplicit(value, target->Type);
        return node;
    }
    TokenKind op;
    switch (assign->Operator)
    {
    case TokenKind::PlusAssign:
        op = TokenKind::Plus;
        break;
    case TokenKind::MinusAssign:
        op = TokenKind::Minus;
        break;
    case TokenKind::StarAssign:
        op = TokenKind::Star;
        break;
    case TokenKind::SlashAssign:
        op = TokenKind::Slash;
        break;
    case TokenKind::PercentAssign:
        op = TokenKind::Percent;
        break;
    case TokenKind::AmpersandAssign:
        op = TokenKind::Ampersand;
        break;
    case TokenKind::PipeAssign:
        op = TokenKind::Pipe;
        break;
    case TokenKind::CaretAssign:
        op = TokenKind::Caret;
        break;
    case TokenKind::LessLessAssign:
        op = TokenKind::LessLess;
        break;
    default:
        op = TokenKind::GreaterGreater;
        break;
    }
    if (value->Kind == BoundKind::Unbound && target->Type->Is(SemaTypeKind::Delegate))
    {
        value = ConvertToDelegate(value, target->Type);
    }
    BoundExpr *operation = BindBinaryOperands(op, target, value, assign->Range);
    if (operation->Type->IsError())
    {
        return operation;
    }
    // A script operation that is not a MIR operation, such as string concatenation or combining delegates, gives
    // the whole new value; it reads the target, which lowering evaluates once.
    if (InScript() && operation->Kind != BoundKind::Binary)
    {
        node->IsCompound = true;
        node->Combined = true;
        node->Value = ConvertImplicit(operation, target->Type);
        return node;
    }
    if (operation->Kind != BoundKind::Binary || operation->As<BoundBinary>()->Left->Kind == BoundKind::Constant)
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, assign->Range,
                                 "this compound assignment needs a built-in operator")
            .WithHelp("write it out: `x = x + y`");
        return Error(assign->Range);
    }
    auto *binary = operation->As<BoundBinary>();
    const SemaConversion back = _sema->ClassifyConversion(binary->Type, target->Type, nullptr);
    if (back == SemaConversion::None)
    {
        Convert(operation, target->Type, false);
        return Error(assign->Range);
    }
    node->IsCompound = true;
    node->Op = binary->Op;
    node->OperationType = binary->Type;
    node->Value = binary->Right;
    return node;
}

BoundExpr *Binder::BindConditional(const AstConditionalExpr *conditional, const SemaType *expected)
{
    BoundExpr *condition = BindCondition(conditional->Condition);
    BoundExpr *whenTrue = BindExpression(conditional->WhenTrue, expected);
    BoundExpr *whenFalse = BindExpression(conditional->WhenFalse, expected);
    if (whenTrue->Type->IsError() || whenFalse->Type->IsError() || condition->Type->IsError())
    {
        return Error(conditional->Range);
    }
    const SemaType *trueType = whenTrue->Type;
    const SemaType *falseType = whenFalse->Type;
    const SemaType *type = nullptr;
    if (trueType == falseType)
    {
        type = trueType;
    }
    else if (expected != nullptr && _sema->ClassifyConversion(trueType, expected, nullptr) < SemaConversion::Explicit &&
             _sema->ClassifyConversion(falseType, expected, nullptr) < SemaConversion::Explicit)
    {
        type = expected;
    }
    else if (trueType->IsArithmetic() && falseType->IsArithmetic())
    {
        const SemaType *scalar = CommonScalar(whenTrue, whenFalse);
        if (scalar != nullptr)
        {
            type = _sema->Types.WithScalar(trueType->IsScalar() ? falseType : trueType, scalar);
        }
    }
    else if (InScript())
    {
        // `c ? x : null` is nullable; of two related references the result is the one the other converts to.
        if (trueType->Is(SemaTypeKind::Null) || falseType->Is(SemaTypeKind::Null))
        {
            type = _sema->Types.GetNullable(trueType->Is(SemaTypeKind::Null) ? falseType : trueType);
        }
        else if (_sema->ClassifyConversion(trueType, falseType, nullptr) < SemaConversion::Explicit)
        {
            type = falseType;
        }
        else if (_sema->ClassifyConversion(falseType, trueType, nullptr) < SemaConversion::Explicit)
        {
            type = trueType;
        }
    }
    if (type == nullptr)
    {
        _sema->Diagnostics
            .Error(DiagnosticCode::TypeMismatch, conditional->Range, "the branches have different types `{0}` and `{1}`",
                   TypeNameOf(trueType).AsView(), TypeNameOf(falseType).AsView())
            .WithSecondaryLabel(whenTrue->Range, TypeNameOf(trueType).AsView())
            .WithSecondaryLabel(whenFalse->Range, TypeNameOf(falseType).AsView());
        return Error(conditional->Range);
    }
    auto *node = MakeExpr<BoundConditional>(*_sema, type, conditional->Range);
    node->Condition = condition;
    node->WhenTrue = ConvertImplicit(whenTrue, type);
    node->WhenFalse = ConvertImplicit(whenFalse, type);
    return Fold(node);
}

BoundExpr *Binder::BindCast(const AstCastExpr *cast)
{
    const SemaType *type = _sema->ResolveType(cast->Type, _context);
    BoundExpr *operand = BindExpression(cast->Operand, type);
    if (type->IsError())
    {
        return Error(cast->Range);
    }
    BoundExpr *converted = Convert(operand, type, true);
    converted->Range = cast->Range;
    return converted;
}

BoundExpr *Binder::BindIndex(const AstIndexExpr *index)
{
    const bool viaBase = index->Target->Kind == AstKind::BaseExpr;
    BoundExpr *object = nullptr;
    if (viaBase)
    {
        SemaStructInstance *owner = _function != nullptr ? _function->Owner : nullptr;
        if (!InScript() || owner == nullptr || !_sema->ResolveHierarchy(owner) || owner->Base == nullptr ||
            ThisOwner() == nullptr || ThisOwner()->This == nullptr)
        {
            _sema->Diagnostics.Error(DiagnosticCode::UndefinedName, index->Target->Range,
                                     "`base` is only available in instance members of a class");
            BindExpression(index->Index);
            return Error(index->Range);
        }
        object = MakeTypeConversion(BindThis(index->Target->Range), _sema->Types.GetClass(owner->Base),
                                    BoundTypeConversionKind::Upcast, false);
    }
    else
    {
        object = BindExpression(index->Target);
    }
    BoundExpr *position = BindExpression(index->Index);
    const SemaType *type = object->Type;
    if (type->IsError() || position->Type->IsError())
    {
        return Error(index->Range);
    }
    // Script arrays, strings, classes and structs with indexers (MarlinScript §3.5).
    if (type->IsReference() || type->Is(SemaTypeKind::Nullable) ||
        (InScript() && type->Is(SemaTypeKind::Struct)))
    {
        return BindIndexer(object, position, index->Range, viaBase);
    }
    if (!position->Type->Is(SemaTypeKind::Int))
    {
        _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, position->Range, "an index must be an integer")
            .WithLabel(TypeNameOf(position->Type).AsView());
        return Error(index->Range);
    }
    if (type->Is(SemaTypeKind::Buffer))
    {
        if (type->GetElement() == nullptr)
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, index->Range,
                                     "a `Ptr<byte>` is read with `Read<T>(offset)`, not indexed");
            return Error(index->Range);
        }
        auto *element = MakeExpr<BoundBufferElement>(*_sema, type->GetElement(), index->Range);
        element->Buffer = object;
        element->Index = position;
        element->IsLValue = type->IsWritable();
        return element;
    }
    if (type->Is(SemaTypeKind::Struct) &&
        type->GetStruct()->Symbol->BuiltinId == static_cast<uint32>(BuiltinTypeId::Span) &&
        _sema->ResolveFields(type->GetStruct()))
    {
        // `span[i]` is element min(i, Length - 1) of span.Pointer (§4.5); fields 0 and 1 are Pointer and Length.
        auto field = [&](uint32 fieldIndex) {
            const SemaField &declared = type->GetStruct()->Fields[fieldIndex];
            auto *node = MakeExpr<BoundField>(*_sema, declared.Type, index->Range);
            node->Object = object;
            node->FieldIndex = fieldIndex;
            return node;
        };
        BoundField *pointer = field(0);
        auto *element = MakeExpr<BoundBufferElement>(*_sema, pointer->Type->GetElement(), index->Range);
        element->Buffer = pointer;
        element->Index = position;
        element->Length = field(1);
        element->IsLValue = pointer->Type->IsWritable();
        return element;
    }
    const SemaType *elementType = nullptr;
    uint32 count = 0;
    switch (type->GetKind())
    {
    case SemaTypeKind::Array:
        elementType = type->GetElement();
        count = type->GetCount();
        break;
    case SemaTypeKind::Vector:
        elementType = type->GetElement();
        count = type->GetCount();
        break;
    case SemaTypeKind::Matrix:
        elementType = _sema->Types.GetVector(type->GetElement(), type->GetCount());
        count = type->GetRows();
        break;
    case SemaTypeKind::MeshOutput:
        elementType = type->GetElement();
        count = type->GetCount();
        break;
    default:
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, index->Range, "`{0}` cannot be indexed",
                                 TypeNameOf(type).AsView())
            .WithSecondaryLabel(object->Range, TypeNameOf(type).AsView());
        return Error(index->Range);
    }
    if (position->Kind == BoundKind::Constant)
    {
        const SemaConstant *value = position->As<BoundConstant>()->Value;
        const bool negative = value->Type->IsSigned() && value->GetInt() < 0;
        if (negative || value->Bits >= count)
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::IndexOutOfRange, position->Range, "the index is out of range")
                .WithLabel(Format("the valid indices are 0 to {0}", count - 1).AsView());
            return Error(index->Range);
        }
    }
    auto *node = MakeExpr<BoundIndex>(*_sema, elementType, index->Range);
    node->Object = object;
    node->Index = position;
    node->IsLValue = object->IsLValue;
    return Fold(node);
}

BoundExpr *Binder::BindNew(const AstNewExpr *node)
{
    const SemaType *type = _sema->ResolveType(node->Type, _context);
    if (type->IsError())
    {
        return Error(node->Range);
    }
    BoundExpr *value = nullptr;
    if (type->Is(SemaTypeKind::Class))
    {
        value = BindNewObject(type, node->Arguments, node->Range);
    }
    else if (type->IsReference() || type->Is(SemaTypeKind::Nullable))
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, node->Range, "a value of `{0}` cannot be constructed",
                                 TypeNameOf(type).AsView());
        return Error(node->Range);
    }
    else
    {
        value = BindConstruction(type, node->Arguments, node->Range);
    }
    if (node->HasInitializer && !value->Type->IsError())
    {
        value = ApplyInitializers(value, node->Initializers, node->Range);
    }
    if (node->HasCollectionInitializer && !value->Type->IsError())
    {
        value = BindCollectionInitializer(value, node->Elements, node->Range);
    }
    return value;
}

BoundExpr *Binder::BindWith(const AstWithExpr *with)
{
    BoundExpr *initial = BindExpression(with->Target);
    if (initial->Type->IsError())
    {
        return initial;
    }
    if (!initial->Type->Is(SemaTypeKind::Struct))
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, with->Range, "`with` applies to structs, not `{0}`",
                                 TypeNameOf(initial->Type).AsView());
        return Error(with->Range);
    }
    return ApplyInitializers(initial, with->Initializers, with->Range);
}

BoundExpr *Binder::ApplyInitializers(BoundExpr *initial, Span<AstMemberInitializer> initializers, SourceRange range)
{
    const SemaType *type = initial->Type;
    const bool byMember = type->Is(SemaTypeKind::Class) || (InScript() && type->Is(SemaTypeKind::Struct));
    if (!type->Is(SemaTypeKind::Struct) && !byMember)
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, range, "only a struct has fields to initialize");
        return Error(range);
    }
    SemaStructInstance *instance = type->GetStruct();
    if (!_sema->ResolveFields(instance))
    {
        return Error(range);
    }
    SemaLocal *temporary = _sema->New<SemaLocal>();
    temporary->Name = "with";
    temporary->Type = type;
    temporary->Range = range;
    temporary->Function = _function;
    List<BoundExpr *> effects;
    List<StringView> assigned;
    for (const AstMemberInitializer &initializer : initializers)
    {
        if (assigned.Contains(initializer.Name))
        {
            _sema->Diagnostics.Error(DiagnosticCode::DuplicateDefinition, initializer.Range,
                                     "`{0}` is initialized twice", initializer.Name);
            continue;
        }
        assigned.Add(initializer.Name);
        auto *object = MakeExpr<BoundLocal>(*_sema, type, initializer.Range);
        object->Local = temporary;
        object->IsLValue = true;
        if (byMember)
        {
            // Object initializers set fields and properties, `init` ones included (MarlinScript §2.4).
            AstMemberExpr member;
            member.Kind = AstKind::MemberExpr;
            member.Range = initializer.Range;
            member.NameRange = initializer.Range;
            member.Name = initializer.Name;
            BinderTarget target =
                type->IsReference() ? ResolveReferenceMember(object, &member) : ResolveInstanceMember(object, &member);
            if (target.TargetKind != BinderTarget::Kind::Value)
            {
                if (target.TargetKind != BinderTarget::Kind::Error)
                {
                    _sema->Diagnostics.Error(DiagnosticCode::NotAValue, initializer.Range,
                                             "`{0}` is not a field or property of `{1}`", initializer.Name,
                                             TypeNameOf(type).AsView());
                }
                BindExpression(initializer.Value);
                continue;
            }
            const bool previous = _initializing;
            _initializing = true;
            const bool assignable = CanAssign(target.Value);
            _initializing = previous;
            if (!assignable)
            {
                BindExpression(initializer.Value);
                continue;
            }
            auto *assign = MakeExpr<BoundAssign>(*_sema, target.Value->Type, initializer.Range);
            assign->Target = target.Value;
            assign->Value =
                ConvertImplicit(BindExpression(initializer.Value, target.Value->Type), target.Value->Type);
            effects.Add(assign);
            continue;
        }
        usize fieldIndex = ~usize{0};
        for (usize i = 0; i < instance->Fields.Count(); ++i)
        {
            if (instance->Fields[i].Name == initializer.Name)
            {
                fieldIndex = i;
            }
        }
        if (fieldIndex == ~usize{0})
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::UndefinedName, initializer.Range, "`{0}` has no field `{1}`",
                       TypeNameOf(type).AsView(), initializer.Name)
                .WithLabel("unknown field");
            continue;
        }
        const SemaField &field = instance->Fields[fieldIndex];
        auto *fieldExpr = MakeExpr<BoundField>(*_sema, field.Type, initializer.Range);
        fieldExpr->Object = object;
        fieldExpr->FieldIndex = static_cast<uint32>(fieldIndex);
        fieldExpr->IsLValue = true;
        auto *assign = MakeExpr<BoundAssign>(*_sema, field.Type, initializer.Range);
        assign->Target = fieldExpr;
        assign->Value = ConvertImplicit(BindExpression(initializer.Value, field.Type), field.Type);
        effects.Add(assign);
    }
    auto *sequence = MakeExpr<BoundSequence>(*_sema, type, range);
    sequence->Temporary = temporary;
    sequence->Initial = initial;
    sequence->Effects = _sema->Freeze(effects);
    return byMember ? sequence : Fold(sequence);
}

BoundExpr *Binder::BindDefault(const AstDefaultExpr *node, const SemaType *expected)
{
    const SemaType *type = node->Type != nullptr ? _sema->ResolveType(node->Type, _context)
                                                 : (expected != nullptr ? expected : _sema->Types.GetNull());
    if (type->IsError())
    {
        return Error(node->Range);
    }
    // The zero value of a reference or nullable type is null, which is not a constant of the type.
    if (type->IsReference() || type->Is(SemaTypeKind::Nullable))
    {
        return MakeExpr<BoundZero>(*_sema, type->Is(SemaTypeKind::Nullable) ? type : _sema->Types.GetNullable(type),
                                   node->Range);
    }
    return MakeConstant(*_sema, _sema->NewZero(type), node->Range);
}

BoundExpr *Binder::BindSizeof(const AstSizeofExpr *node)
{
    const SemaType *type = _sema->ResolveType(node->Type, _context);
    if (type->IsError())
    {
        return Error(node->Range);
    }
    const uint32 size = _sema->SizeOf(type);
    if (size == 0)
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, node->Range, "`{0}` has no size in memory",
                                 TypeNameOf(type).AsView());
        return Error(node->Range);
    }
    return MakeConstant(*_sema, _sema->NewScalar(_sema->Types.GetInt(32, false), size), node->Range);
}

BoundExpr *Binder::BindNameof(const AstNameofExpr *node)
{
    StringView name;
    if (const auto *simple = node->Operand->TryAs<AstNameExpr>())
    {
        name = simple->Name;
    }
    else if (const auto *member = node->Operand->TryAs<AstMemberExpr>())
    {
        name = member->Name;
    }
    else if (const auto *named = node->Operand->TryAs<AstNamedType>())
    {
        name = named->Name;
    }
    else
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, node->Operand->Range, "`nameof` takes a name");
        return Error(node->Range);
    }
    return MakeConstant(*_sema, _sema->NewString(name), node->Range);
}
} // namespace Marlin
