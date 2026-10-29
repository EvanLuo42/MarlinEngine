#include "Sema/Binder.h"
#include "Sema/SemaHelpers.h"

namespace Marlin
{
namespace
{
const SemaType *WithoutNullable(const SemaType *type)
{
    return type->Is(SemaTypeKind::Nullable) ? type->GetElement() : type;
}

bool CanBeNull(const SemaType *type)
{
    return type->IsReference() || type->Is(SemaTypeKind::Nullable);
}

/// Whether control cannot leave *statement* at its end.
bool SectionEndsInJump(const BoundStmt *statement)
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
        return true;
    case BoundStmtKind::Block:
    {
        const Span<BoundStmt *> statements = statement->As<BoundBlock>()->Statements;
        return !statements.IsEmpty() && SectionEndsInJump(statements.Last());
    }
    case BoundStmtKind::If:
    {
        const auto *branch = statement->As<BoundIf>();
        return SectionEndsInJump(branch->Then) && SectionEndsInJump(branch->Else);
    }
    default:
        return false;
    }
}
} // namespace

BoundExpr *Binder::MakeLet(SemaLocal *local, BoundExpr *initial, BoundExpr *result)
{
    auto *node = MakeExpr<BoundLet>(*_sema, result->Type, result->Range);
    node->Local = local;
    node->Initial = initial;
    node->Result = result;
    return node;
}

BoundExpr *Binder::StoreInput(BoundExpr *value, SemaLocal *&local)
{
    local = nullptr;
    if (value->Kind == BoundKind::Local || value->Kind == BoundKind::Constant || value->Type->IsError())
    {
        return value;
    }
    local = _sema->New<SemaLocal>();
    local->Name = "input";
    local->Type = value->Type;
    local->Range = value->Range;
    auto *read = MakeExpr<BoundLocal>(*_sema, value->Type, value->Range);
    read->Local = local;
    return read;
}

BoundExpr *Binder::BindIs(const AstIsExpr *node)
{
    if (!InScript())
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "`is` exists only in scripts");
        return Error(node->Range);
    }
    BoundExpr *operand = BindExpression(node->Operand);
    if (operand->Type->IsError())
    {
        return operand;
    }
    SemaLocal *temporary = nullptr;
    BoundExpr *input = StoreInput(operand, temporary);
    BoundExpr *test = BindPattern(node->Pattern, input);
    test->Range = node->Range;
    return temporary != nullptr ? MakeLet(temporary, operand, test) : test;
}

BoundExpr *Binder::BindPattern(AstNode *pattern, BoundExpr *input)
{
    const SourceRange range = pattern->Range;
    const SemaType *boolType = _sema->Types.GetBool();
    const SemaType *inputType = input->Type;
    auto constant = [&](bool value) { return MakeConstant(*_sema, _sema->NewBool(value), range); };
    auto both = [&](BoundExpr *a, BoundExpr *b) -> BoundExpr * {
        if (a->Kind == BoundKind::Constant && a->As<BoundConstant>()->Value->Bits != 0)
        {
            return b;
        }
        auto *node = MakeExpr<BoundShortCircuit>(*_sema, boolType, range);
        node->IsAnd = true;
        node->Left = a;
        node->Right = b;
        return node;
    };
    auto assignTrue = [&](SemaLocal *local, BoundExpr *value) -> BoundExpr * {
        auto *target = MakeExpr<BoundLocal>(*_sema, local->Type, range);
        target->Local = local;
        target->IsLValue = true;
        auto *assign = MakeExpr<BoundAssign>(*_sema, local->Type, range);
        assign->Target = target;
        assign->Value = value;
        SemaLocal *result = _sema->New<SemaLocal>();
        result->Name = "matched";
        result->Type = boolType;
        result->Range = range;
        auto *sequence = MakeExpr<BoundSequence>(*_sema, boolType, range);
        sequence->Temporary = result;
        sequence->Initial = constant(true);
        BoundExpr *effects[] = {assign};
        sequence->Effects = _sema->Freeze(List<BoundExpr *>(Span<BoundExpr *const>(effects)));
        return sequence;
    };
    auto notNull = [&]() -> BoundExpr * {
        if (!CanBeNull(inputType))
        {
            return constant(true);
        }
        bool handled = false;
        return BindReferenceOperator(TokenKind::NotEqual, input,
                                     MakeExpr<BoundZero>(*_sema, _sema->Types.GetNull(), range), range, handled);
    };
    // Whether the input is a *type*, and the input converted to it once it is known to be.
    auto typeTest = [&](const SemaType *type, SourceRange where) -> BoundExpr * {
        const SemaType *inner = WithoutNullable(inputType);
        if (inner == type || _sema->ClassifyConversion(inner, type, nullptr) < SemaConversion::Explicit)
        {
            return notNull();
        }
        const bool related = inner->IsReference() &&
                             (type->IsReference() || _sema->ClassifyConversion(inner, type, nullptr) == SemaConversion::Explicit ||
                              inner->Is(SemaTypeKind::Interface) ||
                              (inner->Is(SemaTypeKind::Class) && inner->GetStruct() == _sema->GetObjectClass()));
        if (!related)
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, where, "a `{0}` is never a `{1}`",
                                     TypeNameOf(inputType).AsView(), TypeNameOf(type).AsView());
            return Error(where);
        }
        auto *test = MakeExpr<BoundTypeTest>(*_sema, boolType, where);
        test->Operand = input;
        test->Tested = type;
        return test;
    };

    switch (pattern->Kind)
    {
    case AstKind::DiscardPattern:
        return constant(true);
    case AstKind::VarPattern:
    {
        const auto *node = pattern->As<AstVarPattern>();
        return assignTrue(DeclareLocal(node->Name, inputType, node->NameRange), input);
    }
    case AstKind::DeclarationPattern:
    {
        const auto *node = pattern->As<AstDeclarationPattern>();
        const SemaType *type = _sema->ResolveType(node->Type, _context);
        if (type->IsError())
        {
            return Error(range);
        }
        BoundExpr *test = typeTest(type, node->Type->Range);
        if (node->Name.IsEmpty() || test->Type->IsError())
        {
            return test;
        }
        SemaLocal *local = DeclareLocal(node->Name, type, node->NameRange);
        return both(test, assignTrue(local, Convert(input, type, true)));
    }
    case AstKind::ConstantPattern:
    {
        AstNode *valueNode = pattern->As<AstConstantPattern>()->Value;
        if (valueNode->IsType() || valueNode->Kind == AstKind::NameExpr || valueNode->Kind == AstKind::MemberExpr)
        {
            BinderTarget target = ResolveTarget(valueNode);
            if (target.TargetKind == BinderTarget::Kind::Type)
            {
                return typeTest(target.Type, valueNode->Range);
            }
        }
        BoundExpr *value = BindExpression(valueNode, WithoutNullable(inputType));
        if (value->Type->IsError())
        {
            return value;
        }
        if (value->Type->Is(SemaTypeKind::Null))
        {
            bool handled = false;
            BoundExpr *test = BindReferenceOperator(TokenKind::Equal, input, value, range, handled);
            return handled ? test : Error(range);
        }
        if (value->Kind != BoundKind::Constant)
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotConstant, valueNode->Range, "a pattern compares with a constant")
                .WithLabel("computed at run time");
            return Error(range);
        }
        [[fallthrough]];
    }
    case AstKind::RelationalPattern:
    {
        TokenKind op = TokenKind::Equal;
        BoundExpr *value = nullptr;
        if (const auto *relational = pattern->TryAs<AstRelationalPattern>())
        {
            op = relational->Operator;
            value = BindExpression(relational->Value, WithoutNullable(inputType));
            if (value->Kind != BoundKind::Constant && !value->Type->IsError())
            {
                _sema->Diagnostics.Error(DiagnosticCode::NotConstant, relational->Value->Range,
                                         "a relational pattern compares with a constant");
                return Error(range);
            }
        }
        else
        {
            value = BindExpression(pattern->As<AstConstantPattern>()->Value, WithoutNullable(inputType));
        }
        if (value->Type->IsError())
        {
            return value;
        }
        BoundExpr *subject = input;
        BoundExpr *guard = constant(true);
        const SemaType *inner = WithoutNullable(inputType);
        if (inner->IsReference() && !value->Type->IsReference())
        {
            guard = typeTest(value->Type, range);
            subject = Convert(input, value->Type, true);
        }
        else if (inputType->Is(SemaTypeKind::Nullable) && !inner->IsReference())
        {
            guard = notNull();
            subject = Convert(input, inner, true);
        }
        if (guard->Type->IsError() || subject->Type->IsError())
        {
            return Error(range);
        }
        return both(guard, BindBinaryOperands(op, subject, value, range));
    }
    case AstKind::PropertyPattern:
    {
        const auto *node = pattern->As<AstPropertyPattern>();
        const SemaType *type = WithoutNullable(inputType);
        BoundExpr *test = notNull();
        BoundExpr *subject = CanBeNull(inputType) ? Convert(input, type, true) : input;
        if (node->Type != nullptr)
        {
            type = _sema->ResolveType(node->Type, _context);
            if (type->IsError())
            {
                return Error(range);
            }
            test = typeTest(type, node->Type->Range);
            subject = Convert(input, type, true);
        }
        if (test->Type->IsError() || subject->Type->IsError())
        {
            return Error(range);
        }
        SemaLocal *holder = _sema->New<SemaLocal>();
        holder->Name = "subject";
        holder->Type = type;
        holder->Range = range;
        auto read = [&]() {
            auto *local = MakeExpr<BoundLocal>(*_sema, type, range);
            local->Local = holder;
            local->IsLValue = !type->IsReference();
            return local;
        };
        BoundExpr *matches = constant(true);
        for (const AstSubpattern &subpattern : node->Subpatterns)
        {
            AstMemberExpr member;
            member.Kind = AstKind::MemberExpr;
            member.Range = subpattern.Range;
            member.NameRange = subpattern.Range;
            member.Name = subpattern.Name;
            BinderTarget target = type->IsReference() ? ResolveReferenceMember(read(), &member)
                                                      : ResolveInstanceMember(read(), &member);
            if (target.TargetKind != BinderTarget::Kind::Value)
            {
                if (target.TargetKind != BinderTarget::Kind::Error)
                {
                    _sema->Diagnostics.Error(DiagnosticCode::NotAValue, subpattern.Range,
                                             "`{0}` is not a field or property of `{1}`", subpattern.Name,
                                             TypeNameOf(type).AsView());
                }
                return Error(range);
            }
            SemaLocal *memberLocal = nullptr;
            BoundExpr *memberInput = StoreInput(target.Value, memberLocal);
            BoundExpr *test = BindPattern(subpattern.Pattern, memberInput);
            if (memberLocal != nullptr)
            {
                test = MakeLet(memberLocal, target.Value, test);
            }
            matches = both(matches, test);
        }
        if (!node->Name.IsEmpty())
        {
            SemaLocal *local = DeclareLocal(node->Name, type, node->NameRange);
            matches = both(matches, assignTrue(local, read()));
        }
        return both(test, MakeLet(holder, subject, matches));
    }
    case AstKind::BinaryPattern:
    {
        const auto *node = pattern->As<AstBinaryPattern>();
        auto *combined = MakeExpr<BoundShortCircuit>(*_sema, boolType, range);
        combined->IsAnd = node->IsAnd;
        combined->Left = BindPattern(node->Left, input);
        combined->Right = BindPattern(node->Right, input);
        return combined;
    }
    case AstKind::NotPattern:
    {
        auto *negation = MakeExpr<BoundUnary>(*_sema, boolType, range);
        negation->Op = MirOp::LogicalNot;
        negation->Operand = BindPattern(pattern->As<AstNotPattern>()->Operand, input);
        return negation;
    }
    default:
        _sema->Diagnostics.Error(DiagnosticCode::Expected, range, "expected a pattern");
        return Error(range);
    }
}

bool Binder::MatchesEverything(const AstNode *pattern, const SemaType *type)
{
    switch (pattern->Kind)
    {
    case AstKind::DiscardPattern:
    case AstKind::VarPattern:
        return true;
    case AstKind::DeclarationPattern:
    {
        if (CanBeNull(type))
        {
            return false;
        }
        const SemaType *declared = _sema->ResolveType(pattern->As<AstDeclarationPattern>()->Type, _context);
        return declared == type;
    }
    case AstKind::BinaryPattern:
    {
        const auto *node = pattern->As<AstBinaryPattern>();
        return node->IsAnd ? MatchesEverything(node->Left, type) && MatchesEverything(node->Right, type)
                           : MatchesEverything(node->Left, type) || MatchesEverything(node->Right, type);
    }
    default:
        return false;
    }
}

bool Binder::IsExhaustive(Span<AstNode *const> patterns, const SemaType *type)
{
    List<uint64> constants;
    bool sawNull = false;
    for (const AstNode *pattern : patterns)
    {
        if (MatchesEverything(pattern, type))
        {
            return true;
        }
        if (const auto *constantPattern = pattern->TryAs<AstConstantPattern>())
        {
            if (const auto *literal = constantPattern->Value->TryAs<AstLiteralExpr>();
                literal != nullptr && literal->Literal.Is(TokenKind::KeywordNull))
            {
                sawNull = true;
                continue;
            }
            BoundExpr *value = BindExpression(constantPattern->Value, WithoutNullable(type));
            if (value->Kind == BoundKind::Constant)
            {
                constants.Add(value->As<BoundConstant>()->Value->Bits);
            }
        }
        if (const auto *declaration = pattern->TryAs<AstDeclarationPattern>())
        {
            if (_sema->ResolveType(declaration->Type, _context) == WithoutNullable(type))
            {
                constants.Add(~0ull);
            }
        }
    }
    const SemaType *inner = WithoutNullable(type);
    if (CanBeNull(type) && !sawNull)
    {
        return false;
    }
    if (constants.Contains(~0ull))
    {
        return true;
    }
    if (inner->Is(SemaTypeKind::Bool))
    {
        return constants.Contains(0ull) && constants.Contains(1ull);
    }
    if (inner->Is(SemaTypeKind::Enum))
    {
        for (SemaSymbol *member : inner->GetSymbol()->Ordered)
        {
            SemaConstant *value = _sema->GetEnumMemberValue(member);
            if (value == nullptr || !constants.Contains(value->Bits))
            {
                return false;
            }
        }
        return true;
    }
    return false;
}

BoundExpr *Binder::BindSwitchExpression(const AstSwitchExpr *node, const SemaType *expected)
{
    if (!InScript())
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "`switch` expressions exist only in scripts");
        return Error(node->Range);
    }
    BoundExpr *selector = BindExpression(node->Selector);
    if (selector->Type->IsError())
    {
        return selector;
    }
    if (node->Arms.IsEmpty())
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, node->Range, "a `switch` expression needs an arm");
        return Error(node->Range);
    }
    SemaLocal *temporary = nullptr;
    BoundExpr *input = StoreInput(selector, temporary);
    List<BoundExpr *> conditions;
    List<BoundExpr *> values;
    List<AstNode *> unguarded;
    bool failed = false;
    for (const AstSwitchArm &arm : node->Arms)
    {
        PushScope();
        BoundExpr *condition = BindPattern(arm.Pattern, input);
        if (arm.When != nullptr)
        {
            auto *guarded = MakeExpr<BoundShortCircuit>(*_sema, _sema->Types.GetBool(), arm.Range);
            guarded->IsAnd = true;
            guarded->Left = condition;
            guarded->Right = BindCondition(arm.When);
            condition = guarded;
        }
        else
        {
            unguarded.Add(arm.Pattern);
        }
        values.Add(BindExpression(arm.Value, expected));
        PopScope();
        conditions.Add(condition);
        failed = failed || condition->Type->IsError() || values.Last()->Type->IsError();
    }
    if (failed)
    {
        return Error(node->Range);
    }
    const SemaType *type = nullptr;
    bool allFit = expected != nullptr;
    bool sawNull = false;
    for (BoundExpr *value : values)
    {
        allFit = allFit && _sema->ClassifyConversion(value->Type, expected, nullptr) < SemaConversion::Explicit;
        if (value->Type->Is(SemaTypeKind::Null))
        {
            sawNull = true;
        }
        else if (type == nullptr)
        {
            type = value->Type;
        }
    }
    if (allFit)
    {
        type = expected;
    }
    else if (type == nullptr)
    {
        _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, node->Range, "the arms give no type");
        return Error(node->Range);
    }
    else if (sawNull)
    {
        type = _sema->Types.GetNullable(type);
    }
    for (BoundExpr *&value : values)
    {
        value = ConvertImplicit(value, type);
    }
    if (!IsExhaustive(unguarded.AsSpan(), selector->Type))
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, node->Range,
                                 "the `switch` expression does not cover every value of `{0}`",
                                 TypeNameOf(selector->Type).AsView())
            .WithHelp("add a `_ => …` arm (MarlinScript §3.9)");
        return Error(node->Range);
    }
    // The arms are tried in order; the last one is reached only when every earlier one failed, and exhaustiveness
    // means it matches then, but it still runs its pattern to set the pattern's variables.
    BoundExpr *result = MakeExpr<BoundZero>(*_sema, type, node->Range);
    for (usize i = values.Count(); i-- > 0;)
    {
        auto *conditional = MakeExpr<BoundConditional>(*_sema, type, node->Arms[i].Range);
        conditional->Condition = conditions[i];
        conditional->WhenTrue = values[i];
        conditional->WhenFalse = result;
        result = conditional;
    }
    result->Range = node->Range;
    return temporary != nullptr ? MakeLet(temporary, selector, result) : result;
}

BoundStmt *Binder::BindPatternSwitch(const AstSwitchStmt *statement)
{
    BoundExpr *selector = BindExpression(statement->Selector);
    if (selector->Type->IsError())
    {
        return nullptr;
    }
    SemaLocal *input = _sema->New<SemaLocal>();
    input->Name = "switch";
    input->Type = selector->Type;
    input->Range = statement->Selector->Range;
    input->Function = _function;
    auto read = [&](SourceRange range) {
        auto *local = MakeExpr<BoundLocal>(*_sema, input->Type, range);
        local->Local = input;
        return local;
    };
    auto *node = MakeStmt<BoundSwitch>(*_sema, statement->Range);
    node->Selector = selector;
    node->Input = input;
    List<BoundSwitchSection> sections;
    SourceRange defaultRange;
    ++_breakDepth;
    for (const AstSwitchSection &section : statement->Sections)
    {
        PushScope();
        BoundSwitchSection bound;
        bound.IsDefault = section.HasDefault;
        if (section.HasDefault)
        {
            if (defaultRange.IsValid())
            {
                _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, section.Range,
                                         "`switch` has more than one `default`")
                    .WithSecondaryLabel(defaultRange, "first declared here");
            }
            defaultRange = section.Range;
        }
        BoundExpr *match = nullptr;
        for (const AstCaseLabel &label : section.Labels)
        {
            BoundExpr *test = BindPattern(label.Pattern, read(label.Range));
            if (label.When != nullptr)
            {
                auto *guarded = MakeExpr<BoundShortCircuit>(*_sema, _sema->Types.GetBool(), label.Range);
                guarded->IsAnd = true;
                guarded->Left = test;
                guarded->Right = BindCondition(label.When);
                test = guarded;
            }
            if (match != nullptr)
            {
                auto *either = MakeExpr<BoundShortCircuit>(*_sema, _sema->Types.GetBool(), section.Range);
                either->IsAnd = false;
                either->Left = match;
                either->Right = test;
                test = either;
            }
            match = test;
        }
        bound.Match = match;
        List<BoundStmt *> statements;
        for (AstNode *inner : section.Statements)
        {
            if (BoundStmt *boundStatement = BindStatement(inner))
            {
                statements.Add(boundStatement);
            }
        }
        if (statements.IsEmpty() || !SectionEndsInJump(statements.Last()))
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, section.Range,
                                     "a `switch` section must end in `break`, `continue` or `return`")
                .WithNote("control cannot fall through to the next section");
        }

        bound.Statements = _sema->Freeze(statements);
        PopScope();
        sections.Add(bound);
    }
    --_breakDepth;
    node->Sections = _sema->Freeze(sections);
    return node;
}
} // namespace Marlin
