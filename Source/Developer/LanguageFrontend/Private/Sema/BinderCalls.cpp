#include "Sema/Binder.h"
#include "Sema/SemaHelpers.h"
#include "LanguageIR/MirDebugFormat.h"

#include "Hash/Xxh3.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
SemaParameterMode ModeOf(AstModifiers modifiers)
{
    return HasAnyFlags(modifiers, AstModifiers::Ref)   ? SemaParameterMode::Ref
           : HasAnyFlags(modifiers, AstModifiers::Out) ? SemaParameterMode::Out
           : HasAnyFlags(modifiers, AstModifiers::In)  ? SemaParameterMode::In
                                                       : SemaParameterMode::Value;
}

bool IsInstanceMember(const SemaSymbol *symbol)
{
    const SemaSymbol *parent = symbol->Parent;
    const bool inType = parent != nullptr && (parent->Kind == SemaSymbolKind::Struct || parent->Kind == SemaSymbolKind::Class ||
                                              parent->Kind == SemaSymbolKind::Interface ||
                                              parent->Kind == SemaSymbolKind::Extension);
    return inType && symbol->Kind != SemaSymbolKind::Constructor && symbol->Kind != SemaSymbolKind::Operator &&
           !HasAnyFlags(symbol->Decl->Modifiers, AstModifiers::Static | AstModifiers::Const);
}

/// A candidate of overload resolution that fits the arguments.
struct Applicable
{
    SemaFunction *Function = nullptr;
    List<SemaConversion> Conversions;
    /// For each parameter, the argument given, or null when its default applies.
    List<const BinderArgument *> Arguments;
    bool IsGeneric = false;
};

bool IsBetter(const Applicable &a, const Applicable &b)
{
    bool better = false;
    for (usize i = 0; i < a.Conversions.Count() && i < b.Conversions.Count(); ++i)
    {
        if (a.Conversions[i] > b.Conversions[i])
        {
            return false;
        }
        better = better || a.Conversions[i] < b.Conversions[i];
    }
    return better || (!a.IsGeneric && b.IsGeneric);
}
} // namespace

SemaStructInstance *Binder::MagicOwner(const SemaType *type)
{
    List<SemaGenericArgument> arguments;
    const uint32 family = _sema->GetMagicFamily(type, arguments);
    SemaSymbol *symbol = family != 0 ? _sema->GetMagicSymbol(family) : nullptr;
    return symbol != nullptr ? _sema->InstantiateStruct(symbol, arguments.AsSpan()) : nullptr;
}

void Binder::NoteCallee(SemaFunction *callee, SourceRange range)
{
    if (_function != nullptr)
    {
        _function->Callees.Add({callee, range, _sema->Freeze(_guards)});
    }
}

bool Binder::BindArguments(Span<AstArgument> arguments, List<BinderArgument> &bound)
{
    bool ok = true;
    for (const AstArgument &argument : arguments)
    {
        BinderArgument result;
        result.Name = argument.Name;
        result.Range = argument.Range;
        result.Mode = ModeOf(argument.Modifier);
        result.Value = BindExpression(argument.Value);
        if (result.Value->Kind == BoundKind::Constant)
        {
            result.Constant = result.Value->As<BoundConstant>()->Value;
        }
        if ((result.Mode == SemaParameterMode::Ref || result.Mode == SemaParameterMode::Out) &&
            !RequireLValue(result.Value, "pass by reference"))
        {
            ok = false;
        }
        ok = ok && !result.Value->Type->IsError();
        bound.Add(result);
    }
    return ok;
}

BoundExpr *Binder::BindCall(const AstCallExpr *call)
{
    // `Autodiff.Forward`, `Autodiff.Backward` and `Autodiff.Detach` are known to the compiler by name (§9.12).
    if (const auto *member = call->Callee->TryAs<AstMemberExpr>();
        member != nullptr && (member->Name == "Forward" || member->Name == "Backward" || member->Name == "Detach"))
    {
        const auto *owner = member->Target->TryAs<AstNameExpr>();
        if (owner != nullptr && owner->Name == "Autodiff" && FindLocal("Autodiff") == nullptr)
        {
            SemaLookup lookup;
            _sema->Lookup(_context, "Autodiff", lookup);
            if (lookup.IsEmpty())
            {
                return member->Name == "Detach" ? BindDetach(call) : BindDerivative(call, member->Name == "Forward");
            }
        }
    }
    BinderTarget target = ResolveTarget(call->Callee);
    if (target.TargetKind == BinderTarget::Kind::Type)
    {
        if (target.Type->Is(SemaTypeKind::Class))
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, call->Range, "a class is created with `new`")
                .WithHelp(Format("write `new {0}(…)`", TypeNameOf(target.Type).AsView()).AsView());
            return Error(call->Range);
        }
        return BindConstruction(target.Type, call->Arguments, call->Range);
    }
    if (target.TargetKind == BinderTarget::Kind::Value &&
        (target.Value->Type->Is(SemaTypeKind::Delegate) ||
         (target.Value->Type->Is(SemaTypeKind::Nullable) &&
          target.Value->Type->GetElement()->Is(SemaTypeKind::Delegate))))
    {
        return BindDelegateInvoke(target.Value, call->Arguments, call->Range);
    }
    List<BinderArgument> arguments;
    const bool argumentsOk = BindArguments(call->Arguments, arguments);
    switch (target.TargetKind)
    {
    case BinderTarget::Kind::Functions:
        if (!argumentsOk)
        {
            return Error(call->Range);
        }
        return ResolveCall(target, arguments.AsSpan(), call->Range);
    case BinderTarget::Kind::Value:
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, call->Callee->Range, "`{0}` is not a function",
                                 TypeNameOf(target.Value->Type).AsView())
            .WithLabel("cannot be called");
        return Error(call->Range);
    case BinderTarget::Kind::Namespace:
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, call->Callee->Range, "a namespace cannot be called");
        return Error(call->Range);
    default:
        return Error(call->Range);
    }
}

BoundExpr *Binder::BindConstruction(const SemaType *type, Span<AstArgument> astArguments, SourceRange range)
{
    if (type->IsError())
    {
        return Error(range);
    }
    List<BinderArgument> arguments;
    if (!BindArguments(astArguments, arguments))
    {
        return Error(range);
    }
    if (type->IsScalar())
    {
        if (arguments.Count() != 1)
        {
            _sema->Diagnostics.Error(DiagnosticCode::WrongArgumentCount, range, "`{0}(…)` converts one value",
                                     TypeNameOf(type).AsView());
            return Error(range);
        }
        return Convert(arguments[0].Value, type, true);
    }
    if (type->Is(SemaTypeKind::Vector) || type->Is(SemaTypeKind::Matrix))
    {
        return BindVectorConstruction(type, arguments.AsSpan(), range);
    }
    if (type->Is(SemaTypeKind::CoopVector) || type->Is(SemaTypeKind::CoopMatrix))
    {
        // lang-lower: `CoopVector<T, N>(x)` and `CoopMatrix<…>(x)` fill every element with one scalar (§9.9).
        if (arguments.Count() != 1)
        {
            _sema->Diagnostics.Error(DiagnosticCode::WrongArgumentCount, range, "`{0}(…)` converts one value",
                                     TypeNameOf(type).AsView());
            return Error(range);
        }
        BoundExpr *element = ConvertImplicit(arguments[0].Value, type->GetElement());
        if (element->Type->IsError())
        {
            return element;
        }
        BoundExpr *elements[] = {element};
        auto *construct = MakeExpr<BoundConstruct>(*_sema, type, range);
        construct->Elements = _sema->Freeze(List<BoundExpr *>(Span<BoundExpr *const>(elements)));
        return construct;
    }

    SemaStructInstance *owner = type->Is(SemaTypeKind::Struct) ? type->GetStruct() : MagicOwner(type);
    if (owner == nullptr || type->IsReference())
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, range, "a value of `{0}` cannot be constructed",
                                 TypeNameOf(type).AsView());
        return Error(range);
    }
    BinderTarget target;
    target.TargetKind = BinderTarget::Kind::Functions;
    target.Range = range;
    target.Owner = owner;
    for (SemaSymbol *member : _sema->GetMembers(owner->Symbol, owner->Symbol->Name))
    {
        if (member->Kind == SemaSymbolKind::Constructor && _sema->IsAccessible(_context, member))
        {
            target.Functions.Add(member);
        }
    }
    if (target.Functions.IsEmpty() || (arguments.IsEmpty() && type->Is(SemaTypeKind::Struct)))
    {
        if (!arguments.IsEmpty())
        {
            _sema->Diagnostics.Error(DiagnosticCode::WrongArgumentCount, range, "`{0}` has no constructor",
                                     TypeNameOf(type).AsView())
                .WithHelp("use an object initializer: `new S { Field = value }`");
            return Error(range);
        }
        if (!type->Is(SemaTypeKind::Struct) || !_sema->ResolveFields(owner))
        {
            return Error(range);
        }
        // Without a constructor, each field starts at its initial value, or zero.
        List<SemaGenericBinding> bindings;
        _sema->GetBindings(owner, bindings);
        List<BoundExpr *> fields;
        for (const SemaField &field : owner->Fields)
        {
            AstNode *initializer = field.Symbol->Kind == SemaSymbolKind::Property
                                       ? field.Symbol->Decl->As<AstPropertyDecl>()->Initializer
                                       : field.Symbol->Decl->As<AstFieldDecl>()->Initializer;
            if (initializer != nullptr)
            {
                Binder fieldBinder(*_sema, nullptr, _sema->ContextOf(field.Symbol, bindings.AsSpan(), owner));
                fields.Add(fieldBinder.ConvertImplicit(fieldBinder.BindExpression(initializer, field.Type), field.Type));
            }
            else
            {
                fields.Add(MakeExpr<BoundZero>(*_sema, field.Type, range));
            }
        }
        auto *construct = MakeExpr<BoundConstruct>(*_sema, type, range);
        construct->Elements = _sema->Freeze(fields);
        return Fold(construct);
    }
    return ResolveCall(target, arguments.AsSpan(), range);
}

BoundExpr *Binder::BindVectorConstruction(const SemaType *type, Span<BinderArgument> arguments, SourceRange range)
{
    const SemaType *scalar = type->GetScalar();
    const bool isMatrix = type->Is(SemaTypeKind::Matrix);
    const uint32 total = isMatrix ? type->GetRows() * type->GetCount() : type->GetCount();
    const SemaType *single = arguments.Count() == 1 ? arguments[0].Value->Type : nullptr;
    if (single != nullptr && (single->IsScalar() || (single->GetKind() == type->GetKind() &&
                                                     single->GetCount() == type->GetCount() &&
                                                     single->GetRows() == type->GetRows())))
    {
        return Convert(arguments[0].Value, type, true);
    }
    uint32 components = 0;
    List<BoundExpr *> elements;
    for (const BinderArgument &argument : arguments)
    {
        const SemaType *argumentType = argument.Value->Type;
        if (argumentType->IsScalar())
        {
            elements.Add(ConvertImplicit(argument.Value, scalar));
            components += 1;
        }
        else if (argumentType->Is(SemaTypeKind::Vector))
        {
            elements.Add(ConvertImplicit(argument.Value, _sema->Types.GetVector(scalar, argumentType->GetCount())));
            components += argumentType->GetCount();
        }
        else
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, argument.Range,
                                     "`{0}` is made of scalars and vectors, not `{1}`", TypeNameOf(type).AsView(),
                                     TypeNameOf(argumentType).AsView());
            return Error(range);
        }
    }
    if (components != total)
    {
        _sema->Diagnostics
            .Error(DiagnosticCode::WrongArgumentCount, range, "`{0}` needs {1} components, but {2} were given",
                   TypeNameOf(type).AsView(), total, components)
            .WithHelp("pass scalars and vectors whose sizes add up, or one scalar to fill every component");
        return Error(range);
    }
    for (BoundExpr *element : elements)
    {
        if (element->Type->IsError())
        {
            return Error(range);
        }
    }
    auto *construct = MakeExpr<BoundConstruct>(*_sema, type, range);
    if (!isMatrix)
    {
        construct->Elements = _sema->Freeze(elements);
        return Fold(construct);
    }
    const SemaType *rowType = _sema->Types.GetVector(scalar, type->GetCount());
    List<BoundExpr *> rows;
    List<BoundExpr *> pending;
    uint32 pendingCount = 0;
    for (BoundExpr *element : elements)
    {
        pending.Add(element);
        pendingCount += element->Type->IsScalar() ? 1 : element->Type->GetCount();
        if (pendingCount == type->GetCount())
        {
            if (pending.Count() == 1 && pending[0]->Type == rowType)
            {
                rows.Add(pending[0]);
            }
            else
            {
                auto *row = MakeExpr<BoundConstruct>(*_sema, rowType, range);
                row->Elements = _sema->Freeze(pending);
                rows.Add(Fold(row));
            }
            pending.Clear();
            pendingCount = 0;
        }
        else if (pendingCount > type->GetCount())
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, element->Range,
                                     "a vector argument crosses the end of a matrix row")
                .WithHelp("pass whole rows or single scalars");
            return Error(range);
        }
    }
    construct->Elements = _sema->Freeze(rows);
    return Fold(construct);
}

BoundExpr *Binder::ResolveCall(BinderTarget &target, Span<BinderArgument> arguments, SourceRange range)
{
    if (target.ExtensionReceiver != nullptr)
    {
        List<BinderArgument> withReceiver;
        BinderArgument receiver;
        receiver.Value = target.ExtensionReceiver;
        receiver.Range = target.ExtensionReceiver->Range;
        receiver.Constant = receiver.Value->Kind == BoundKind::Constant ? receiver.Value->As<BoundConstant>()->Value : nullptr;
        withReceiver.Add(receiver);
        withReceiver.AddRange(arguments);
        target.ExtensionReceiver = nullptr;
        return ResolveCall(target, withReceiver.AsSpan(), range);
    }
    if (target.TypeParameterTarget != nullptr)
    {
        if (arguments.Count() != 1 || arguments[0].Constant == nullptr ||
            !arguments[0].Constant->Type->Is(SemaTypeKind::String))
        {
            _sema->Diagnostics.Error(DiagnosticCode::WrongArgumentCount, range,
                                     "`Implements` takes a member name: `T.Implements(nameof(IBsdf.Sample))`");
            return Error(range);
        }
        const SemaType *type = target.TypeParameterTarget;
        bool implements = false;
        if (type->HasInstance())
        {
            List<SemaSymbol *> members;
            _sema->GetInstanceMembers(type->GetStruct(), arguments[0].Constant->Text, members);
            for (const SemaSymbol *member : members)
            {
                implements = implements || member->Kind == SemaSymbolKind::Function;
            }
        }
        // A member an extension adds counts too (§3.5).
        List<SemaSymbol *> added;
        List<SemaStructInstance *> extensions;
        _sema->FindExtensionMembers(type, arguments[0].Constant->Text, _context, range, added, extensions);
        for (const SemaSymbol *member : added)
        {
            implements = implements || member->Kind == SemaSymbolKind::Function;
        }
        return MakeConstant(*_sema, _sema->NewBool(implements), range);
    }

    List<Applicable> applicable;
    String rejection;
    SemaSymbol *rejected = nullptr;
    for (SemaSymbol *symbol : target.Functions)
    {
        const Span<AstParameterDecl *> parameters = ParametersOf(symbol->Decl);
        List<const BinderArgument *> mapped;
        mapped.Resize(parameters.Count(), nullptr);
        bool fits = arguments.Count() <= parameters.Count();
        usize positional = 0;
        for (const BinderArgument &argument : arguments)
        {
            if (!fits)
            {
                break;
            }
            usize index = positional;
            if (!argument.Name.IsEmpty())
            {
                index = ~usize{0};
                for (usize i = 0; i < parameters.Count(); ++i)
                {
                    if (parameters[i]->Name == argument.Name)
                    {
                        index = i;
                    }
                }
            }
            else
            {
                ++positional;
            }
            fits = index < parameters.Count() && mapped[index] == nullptr;
            if (fits)
            {
                mapped[index] = &argument;
            }
        }
        for (usize i = 0; fits && i < parameters.Count(); ++i)
        {
            fits = mapped[i] != nullptr || parameters[i]->Default != nullptr;
        }
        if (!fits)
        {
            rejected = symbol;
            rejection = Format("takes {0} argument(s)", parameters.Count());
            continue;
        }

        const Span<AstGenericParameterDecl *> genericParameters = GenericParametersOf(symbol->Decl);
        List<SemaGenericArgument> genericArguments;
        // Lambdas bound while inferring, used as the arguments when this candidate is chosen.
        List<BoundExpr *> inferredLambdas;
        inferredLambdas.Resize(parameters.Count(), nullptr);
        if (target.HasGenericArguments)
        {
            if (!_sema->ResolveGenericArguments(symbol, target.GenericArguments, _context, genericArguments) ||
                genericArguments.Count() != genericParameters.Count())
            {
                rejected = symbol;
                rejection = Format("takes {0} generic argument(s)", genericParameters.Count());
                continue;
            }
            List<SemaGenericBinding> check;
            for (usize i = 0; i < genericParameters.Count(); ++i)
            {
                check.Add({genericParameters[i]->Name, genericArguments[i]});
            }
            if (!_sema->CheckConstraints(symbol, check.AsSpan(), range, false))
            {
                rejected = symbol;
                rejection = String("the generic arguments do not satisfy its constraints");
                continue;
            }
        }
        else if (!genericParameters.IsEmpty())
        {
            List<SemaGenericBinding> bindings;
            for (AstGenericParameterDecl *parameter : genericParameters)
            {
                bindings.Add({parameter->Name, {}});
            }
            bool inferred = true;
            for (int32 pass = 0; pass < 2 && inferred; ++pass)
            {
                for (usize i = 0; i < parameters.Count() && inferred; ++i)
                {
                    if (mapped[i] != nullptr && (mapped[i]->Constant != nullptr) == (pass == 1) &&
                        mapped[i]->Value->Kind != BoundKind::Unbound)
                    {
                        const SemaType *argumentType = mapped[i]->Value->Type;
                        List<SemaGenericBinding> attempt;
                        attempt.AddRange(bindings.AsSpan());
                        if (_sema->InferGenerics(symbol, parameters[i]->Type, argumentType, attempt))
                        {
                            bindings = Move(attempt);
                        }
                        else if (pass == 0)
                        {
                            inferred = false;
                        }
                    }
                }
            }
            // Through lambdas: once the delegate's parameter types are known, the lambda's result gives the rest,
            // as `Select(items, x => x.Name)` gives `TResult`.
            for (usize i = 0; i < parameters.Count() && inferred; ++i)
            {
                if (mapped[i] == nullptr || mapped[i]->Value->Kind != BoundKind::Unbound ||
                    mapped[i]->Value->As<BoundUnbound>()->Lambda == nullptr)
                {
                    continue;
                }
                const auto *named = parameters[i]->Type->TryAs<AstNamedType>();
                if (named == nullptr || !named->HasGenericArguments)
                {
                    continue;
                }
                SemaLookup lookup;
                _sema->Lookup(_sema->ContextOf(symbol), named->Name, lookup);
                SemaSymbol *delegate = nullptr;
                for (SemaSymbol *candidate : lookup.Symbols)
                {
                    if (candidate->Kind == SemaSymbolKind::Delegate &&
                        GenericParametersOf(candidate->Decl).Count() == named->GenericArguments.Count())
                    {
                        delegate = candidate;
                    }
                }
                if (delegate == nullptr)
                {
                    continue;
                }
                const auto *delegateDecl = delegate->Decl->As<AstDelegateDecl>();
                // The type a node of the delegate's declaration stands for: a delegate generic parameter maps to the
                // generic argument written in the callee's parameter type, known when it names a bound parameter.
                auto argumentNodeOf = [&](AstNode *node) -> AstNode * {
                    const auto *nameNode = node != nullptr ? node->TryAs<AstNamedType>() : nullptr;
                    if (nameNode == nullptr || nameNode->Qualifier != nullptr || nameNode->HasGenericArguments)
                    {
                        return nullptr;
                    }
                    for (usize k = 0; k < delegateDecl->GenericParameters.Count(); ++k)
                    {
                        if (delegateDecl->GenericParameters[k]->Name == nameNode->Name)
                        {
                            return named->GenericArguments[k];
                        }
                    }
                    return nullptr;
                };
                auto knownType = [&](AstNode *node) -> const SemaType * {
                    const auto *nameNode = node != nullptr ? node->TryAs<AstNamedType>() : nullptr;
                    if (nameNode != nullptr && nameNode->Qualifier == nullptr && !nameNode->HasGenericArguments)
                    {
                        for (const SemaGenericBinding &binding : bindings)
                        {
                            if (binding.Name == nameNode->Name)
                            {
                                return binding.Argument.Type;
                            }
                        }
                    }
                    return nullptr;
                };
                SemaSignature signature;
                bool known = true;
                for (const AstParameterDecl *delegateParameter : delegateDecl->Parameters)
                {
                    const SemaType *type = knownType(argumentNodeOf(delegateParameter->Type));
                    known = known && type != nullptr;
                    signature.Parameters.Add(type);
                    signature.Modes.Add(HasAnyFlags(delegateParameter->Modifiers, AstModifiers::Ref)
                                            ? SemaParameterMode::Ref
                                            : SemaParameterMode::Value);
                }
                AstNode *resultNode = argumentNodeOf(delegateDecl->ReturnType);
                if (!known || resultNode == nullptr || knownType(resultNode) != nullptr)
                {
                    continue;
                }
                BoundExpr *lambda = BindLambda(mapped[i]->Value->As<BoundUnbound>()->Lambda, signature, nullptr);
                if (lambda == nullptr || lambda->Type->IsError() ||
                    !_sema->InferGenerics(symbol, resultNode, lambda->As<BoundLambda>()->Function->ReturnType, bindings))
                {
                    inferred = false;
                    continue;
                }
                inferredLambdas[i] = lambda;
            }
            // Parameters the arguments say nothing about take their defaults.
            for (usize k = 0; k < bindings.Count(); ++k)
            {
                SemaGenericBinding &binding = bindings[k];
                if (binding.Argument.Type == nullptr && binding.Argument.Value == nullptr &&
                    genericParameters[k]->Default != nullptr)
                {
                    binding.Argument.Value = _sema->GetGenericDefault(symbol, k);
                }
                inferred = inferred && (binding.Argument.Type != nullptr || binding.Argument.Value != nullptr);
                genericArguments.Add(binding.Argument);
            }
            if (!inferred)
            {
                rejected = symbol;
                rejection = String("its generic arguments cannot be inferred from these arguments");
                continue;
            }
            List<SemaGenericBinding> check;
            for (usize i = 0; i < genericParameters.Count(); ++i)
            {
                check.Add({genericParameters[i]->Name, genericArguments[i]});
            }
            bool satisfied = true;
            const SemaContext declContext = _sema->ContextOf(symbol, check.AsSpan());
            for (const AstConstraint &constraint : symbol->Decl->As<AstFunctionDecl>()->Constraints)
            {
                for (const SemaGenericBinding &binding : check)
                {
                    if (binding.Name != constraint.Parameter || binding.Argument.Type == nullptr)
                    {
                        continue;
                    }
                    const SemaType *type = binding.Argument.Type;
                    satisfied = satisfied && (!constraint.Class || type->IsReference());
                    for (AstNode *bound : constraint.Bounds)
                    {
                        const SemaType *boundType = _sema->ResolveType(bound, declContext);
                        if (boundType->Is(SemaTypeKind::Class))
                        {
                            satisfied = satisfied && _sema->IsImplicitReference(type, boundType);
                        }
                        else
                        {
                            satisfied = satisfied && boundType->Is(SemaTypeKind::Interface) &&
                                        _sema->ConformsTo(type, boundType);
                        }
                    }
                }
            }
            if (!satisfied)
            {
                rejected = symbol;
                rejection = String("the inferred types do not satisfy its constraints");
                continue;
            }
        }

        // RECOVERY-DECISION S11 (foreign, script workspace): an inherited method is instantiated on the class that
        // declares it, so it keeps its virtual slot and a call on a derived class dispatches to the override.
        SemaStructInstance *owner = target.Owner;
        for (SemaStructInstance *extension : target.Extensions)
        {
            owner = extension->Symbol == symbol->Parent ? extension : owner;
        }
        SemaStructInstance *declaring = _sema->FindDeclaringInstance(owner, symbol);
        SemaFunction *function = _sema->InstantiateFunction(symbol, declaring != nullptr ? declaring : owner,
                                                            genericArguments.AsSpan());
        if (!_sema->ResolveSignature(function))
        {
            continue;
        }
        Applicable candidate;
        candidate.Function = function;
        candidate.IsGeneric = !genericParameters.IsEmpty();
        bool ok = true;
        for (usize i = 0; i < parameters.Count() && ok; ++i)
        {
            const SemaLocal *parameter = function->Parameters[i];
            const BinderArgument *argument = mapped[i];
            if (argument != nullptr && inferredLambdas[i] != nullptr)
            {
                // The lambda is bound already; it is used as it is when this candidate is chosen.
                BinderArgument *replacement = _sema->Memory.New<BinderArgument>(*argument);
                replacement->Value = inferredLambdas[i];
                argument = replacement;
            }
            candidate.Arguments.Add(argument);
            if (argument == nullptr)
            {
                candidate.Conversions.Add(SemaConversion::Identity);
                continue;
            }
            const bool byReference =
                parameter->Mode == SemaParameterMode::Ref || parameter->Mode == SemaParameterMode::Out;
            if (byReference)
            {
                ok = argument->Mode == parameter->Mode && argument->Value->Type == parameter->Type;
                if (!ok)
                {
                    rejection = Format("`{0}` is passed by `{1}` and must be a variable of exactly its type",
                                       parameter->Name, StringView(parameter->Mode == SemaParameterMode::Ref ? "ref" : "out"));
                }
                candidate.Conversions.Add(SemaConversion::Identity);
                continue;
            }
            if (argument->Mode == SemaParameterMode::Ref || argument->Mode == SemaParameterMode::Out)
            {
                ok = false;
                rejection = Format("`{0}` is not passed by reference", parameter->Name);
                continue;
            }
            if (argument->Value->Kind == BoundKind::Unbound || argument->Value->Kind == BoundKind::Lambda)
            {
                ok = argument->Value->Kind == BoundKind::Lambda
                         ? parameter->Type->Is(SemaTypeKind::Delegate)
                         : FitsDelegate(argument->Value, parameter->Type);
                if (!ok)
                {
                    rejection = Format("`{0}` cannot take {1} where `{2}` is expected", parameter->Name,
                                       StringView(argument->Value->As<BoundUnbound>()->Lambda != nullptr ? "a lambda"
                                                                                                         : "a method"),
                                       TypeNameOf(parameter->Type).AsView());
                }
                candidate.Conversions.Add(SemaConversion::Widening);
                continue;
            }
            const SemaConversion conversion =
                _sema->ClassifyConversion(argument->Value->Type, parameter->Type, argument->Constant);
            ok = conversion < SemaConversion::Explicit;
            if (!ok)
            {
                rejection = Format("`{0}` cannot take `{1}` where `{2}` is expected", parameter->Name,
                                   TypeNameOf(argument->Value->Type).AsView(), TypeNameOf(parameter->Type).AsView());
            }
            candidate.Conversions.Add(conversion);
        }
        if (!ok)
        {
            rejected = symbol;
            continue;
        }
        applicable.Add(Move(candidate));
    }

    if (applicable.IsEmpty())
    {
        if (target.Functions.IsEmpty())
        {
            return Error(range);
        }
        const StringView name = target.Functions[0]->Name;
        LanguageDiagnostic &error =
            target.Functions.Count() == 1
                ? _sema->Diagnostics.Error(DiagnosticCode::NoMatchingOverload, range, "the arguments do not fit `{0}`",
                                           name)
                : _sema->Diagnostics.Error(DiagnosticCode::NoMatchingOverload, range,
                                           "no overload of `{0}` fits these arguments", name);
        StringBuilder<256> given;
        for (usize i = 0; i < arguments.Count(); ++i)
        {
            given.Append(i == 0 ? "" : ", ");
            arguments[i].Value->Type->Print(given);
        }
        error.WithLabel(Format("called with ({0})", given.AsView()).AsView());
        if (target.Functions.Count() == 1 && rejected != nullptr)
        {
            error.WithNote(rejected->Decl->NameRange, Format("`{0}` {1}", name, rejection.AsView()).AsView());
        }
        else
        {
            for (usize i = 0; i < target.Functions.Count() && i < 4; ++i)
            {
                error.WithNote(target.Functions[i]->Decl->NameRange, "a candidate");
            }
        }
        return Error(range);
    }

    usize best = 0;
    for (usize i = 1; i < applicable.Count(); ++i)
    {
        if (IsBetter(applicable[i], applicable[best]))
        {
            best = i;
        }
    }
    for (usize i = 0; i < applicable.Count(); ++i)
    {
        if (i != best && !IsBetter(applicable[best], applicable[i]))
        {
            _sema->Diagnostics.Error(DiagnosticCode::AmbiguousOverload, range, "the call to `{0}` is ambiguous",
                                     applicable[best].Function->Symbol->Name)
                .WithNote(applicable[best].Function->Symbol->Decl->NameRange, "could be this")
                .WithNote(applicable[i].Function->Symbol->Decl->NameRange, "or this");
            return Error(range);
        }
    }

    Applicable &chosen = applicable[best];
    SemaFunction *function = chosen.Function;
    const bool instance = IsInstanceMember(function->Symbol);
    if (instance && target.Receiver == nullptr)
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, range, "`{0}` is an instance member",
                                 function->Symbol->Name)
            .WithHelp("call it on a value: `value.Method(…)`");
        return Error(range);
    }
    List<BoundExpr *> bound;
    const Span<AstParameterDecl *> parameters = ParametersOf(function->Symbol->Decl);
    List<SemaGenericBinding> calleeBindings;
    _sema->GetBindings(function, calleeBindings);
    for (usize i = 0; i < function->Parameters.Count(); ++i)
    {
        const SemaLocal *parameter = function->Parameters[i];
        if (chosen.Arguments[i] != nullptr)
        {
            BoundExpr *value = chosen.Arguments[i]->Value;
            if (value->Kind == BoundKind::Lambda)
            {
                value->Type = parameter->Type;
                if (_function != nullptr)
                {
                    _function->Lambdas.Add(value->As<BoundLambda>()->Function);
                }
                bound.Add(value);
                continue;
            }
            bound.Add(parameter->Mode == SemaParameterMode::Ref || parameter->Mode == SemaParameterMode::Out
                          ? value
                          : ConvertImplicit(value, parameter->Type));
            continue;
        }
        // A default value is bound in the callee's context, so its locations are the callee's file.
        Binder defaults(*_sema, nullptr,
                        _sema->ContextOf(function->Symbol, calleeBindings.AsSpan(), function->Owner));
        bound.Add(defaults.ConvertImplicit(defaults.BindExpression(parameters[i]->Default, parameter->Type),
                                           parameter->Type));
    }
    const bool previous = _baseAccess;
    _baseAccess = target.ViaBase;
    BoundExpr *call = MakeCall(function, instance ? target.Receiver : nullptr, _sema->Freeze(bound), range);
    _baseAccess = previous;
    return call;
}

BoundExpr *Binder::MakeCall(SemaFunction *function, BoundExpr *receiver, Span<BoundExpr *> arguments,
                            SourceRange range)
{
    // Inside a function, CheckEntryPoint infers and checks the features each call needs over the call graph (§9.7);
    // only a call outside any function, such as in an initializer, is checked here.
    for (const StringView feature : function->Requires)
    {
        if (_function == nullptr && !_guards.Contains(feature))
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::FeatureRequired, range, "`{0}` needs the feature `{1}`", function->Symbol->Name,
                       feature)
                .WithLabel("used without checking for the feature")
                .WithHelp(Format("guard it with `static if (Device.Supports(Features.{0}))` and a fallback, or "
                                 "declare `[Requires(Features.{0})]` on the calling function",
                                 feature)
                              .AsView());
            return Error(range);
        }
    }
    const AstDecl *decl = function->Symbol->Decl;
    // RECOVERY-DECISION lang-sema-D4: a `[DebugOnly]` call without debug output becomes a void Zero, which a statement
    // drops with its arguments unevaluated (MarlinScript §10.3).
    if (Sema::FindAttribute(decl->Attributes, "DebugOnly") != nullptr && !_sema->Options->DebugOutput)
    {
        return MakeExpr<BoundZero>(*_sema, function->ReturnType, range);
    }
    NoteCallee(function, range);
    List<BoundExpr *> all;
    if (receiver != nullptr)
    {
        // A reference receiver converts to the class or interface that declares the method; a value receiver of
        // `object`'s or an interface's methods is boxed.
        if (function->This != nullptr && receiver->Type != function->This->Type &&
            (receiver->Type->IsReference() || function->This->Type->IsReference() ||
             receiver->Type->Is(SemaTypeKind::Nullable)))
        {
            receiver = ConvertImplicit(receiver, function->This->Type);
        }
        all.Add(receiver);
    }
    all.AddRange(arguments);

    if (const AstAttribute *compileTime = Sema::FindAttribute(decl->Attributes, "CompileTime"))
    {
        (void)compileTime;
        return EvaluateCompileTime(function, all.AsSpan(), range, nullptr);
    }
    if (const AstAttribute *intrinsic = Sema::FindAttribute(decl->Attributes, "Intrinsic"))
    {
        const SemaContext declContext = _sema->ContextOf(function->Symbol);
        SemaConstant *name = _sema->EvaluateAttributeArgument(*intrinsic, 0, {}, declContext);
        auto *node = MakeExpr<BoundIntrinsic>(*_sema, function->ReturnType, range);
        node->Function = function;
        node->Target = name != nullptr ? FindIntrinsic(name->Text) : IntrinsicTarget{};
        if (node->Target.Kind == IntrinsicKind::None)
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidAttribute, intrinsic->Range,
                                     "`{0}` is not a MIR operation", name != nullptr ? name->Text : StringView());
            return Error(range);
        }
        List<int64> literals;
        for (usize i = 1; i < intrinsic->Arguments.Count(); ++i)
        {
            if (SemaConstant *literal = _sema->EvaluateAttributeArgument(*intrinsic, i, {}, declContext))
            {
                literals.Add(literal->GetInt());
            }
        }
        node->Literals = _sema->Freeze(literals);
        node->Arguments = _sema->Freeze(all);
        if (!all.IsEmpty() && all[0]->Type->Is(SemaTypeKind::Texture) && name != nullptr)
        {
            // Sampling reads a texture with Access.Read; Store writes one with Write or ReadWrite (§4.4).
            const StringView op = name->Text;
            const bool samples = op.StartsWith("Sample") || op.StartsWith("Gather") || op == "CalculateLod" ||
                                 op == "TextureLevels";
            const bool writable = all[0]->Type->IsWritable();
            if ((samples && writable) || (op == "TextureStore" && !writable))
            {
                _sema->Diagnostics
                    .Error(DiagnosticCode::InvalidOperands, range, "`{0}` cannot be {1} through `{2}`",
                           function->Symbol->Name, samples ? "sampled" : "written",
                           TypeNameOf(all[0]->Type).AsView())
                    .WithHelp(samples ? "only textures with Access.Read are sampled; read others with `Load`"
                                      : "give the texture `Access.Write` or `Access.ReadWrite`");
                return Error(range);
            }
        }
        if ((node->Target.Kind == IntrinsicKind::DebugPrint || node->Target.Kind == IntrinsicKind::DebugAssert) &&
            !CheckDebugOutput(node))
        {
            return Error(range);
        }
        if (node->Target.Kind == IntrinsicKind::Require &&
            (all.Count() != 2 || all[1]->Kind != BoundKind::Constant))
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotConstant, range, "`Compiler.Require` needs a constant message");
            return Error(range);
        }
        return node;
    }
    if (!CheckDifferentiableCall(function, all.AsSpan(), range))
    {
        return Error(range);
    }
    auto *call = MakeExpr<BoundCall>(*_sema, function->ReturnType, range);
    call->Function = function;
    call->Arguments = _sema->Freeze(all);
    call->Dispatch = receiver != nullptr ? DispatchOf(function, receiver) : BoundDispatch::Direct;
    BoundExpr *folded = Fold(call);
    if (folded->Kind == BoundKind::Constant && _function != nullptr)
    {
        _function->Callees.Pop();
    }
    return folded;
}

bool Binder::IsDifferentiable(const SemaType *type)
{
    return _sema->IsDifferentiable(type);
}

bool Binder::InDifferentiableCode() const
{
    return _function != nullptr && _function->Enclosing == nullptr &&
           _sema->IsMarkedDifferentiable(_function->Symbol, _function->Owner);
}

bool Binder::CheckDifferentiableCall(SemaFunction *function, Span<BoundExpr *const> arguments, SourceRange range)
{
    if (!InDifferentiableCode() || _detached != 0 || function->Symbol->IsCore ||
        function->Symbol->Kind == SemaSymbolKind::Constructor || function->Accessor != SemaAccessor::None)
    {
        return true;
    }
    const Span<AstAttribute> attributes = function->Symbol->Decl->Attributes;
    if (_sema->IsMarkedDifferentiable(function->Symbol, function->Owner) ||
        Sema::FindAttribute(attributes, "ForwardDerivative") != nullptr ||
        Sema::FindAttribute(attributes, "ForwardDerivative2") != nullptr ||
        Sema::FindAttribute(attributes, "BackwardDerivative") != nullptr)
    {
        return true;
    }
    // Without a derivative of its own, the call is fine when no derivative can flow through it: nothing with one
    // goes in other than constants, or nothing with one comes out.
    bool takes = false;
    for (const BoundExpr *argument : arguments)
    {
        takes = takes || (argument->Kind != BoundKind::Constant && IsDifferentiable(argument->Type));
    }
    bool gives = IsDifferentiable(function->ReturnType);
    for (const SemaLocal *parameter : function->Parameters)
    {
        gives = gives || ((parameter->Mode == SemaParameterMode::Ref || parameter->Mode == SemaParameterMode::Out) &&
                          IsDifferentiable(parameter->Type));
    }
    if (!takes || !gives)
    {
        return true;
    }
    _sema->Diagnostics
        .Error(DiagnosticCode::NotDifferentiable, range, "`{0}` is not differentiable", function->Symbol->Name)
        .WithLabel(Format("called from `{0}`, which is `[Differentiable]`", _function->Symbol->Name).AsView())
        .WithSecondaryLabel(function->Symbol->Decl->NameRange, "declared here")
        .WithHelp("mark it `[Differentiable]`, give it a `[ForwardDerivative]` and `[BackwardDerivative]`, or call "
                  "it inside `Autodiff.Detach(…)` to give its result a zero derivative (§9.12)");
    return false;
}

BoundExpr *Binder::BindDetach(const AstCallExpr *call)
{
    if (call->Arguments.Count() != 1)
    {
        _sema->Diagnostics.Error(DiagnosticCode::WrongArgumentCount, call->Range,
                                 "`Autodiff.Detach` takes one argument, not {0}", call->Arguments.Count());
        return Error(call->Range);
    }
    if (_context.Dialect != LanguageDialect::Shader)
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, call->Range, "`Autodiff.Detach` exists only in shaders")
            .WithNote("only shaders are differentiated (§9.12)");
        return Error(call->Range);
    }
    ++_detached;
    BoundExpr *value = BindExpression(call->Arguments[0].Value);
    --_detached;
    if (value->Type->IsError())
    {
        return value;
    }
    if (!IsDifferentiable(value->Type))
    {
        _sema->Diagnostics
            .Error(DiagnosticCode::NotDifferentiable, call->Arguments[0].Value->Range,
                   "`Autodiff.Detach` takes a value with a derivative, not a `{0}`", TypeNameOf(value->Type).AsView())
            .WithLabel("has no derivative");
        return Error(call->Range);
    }
    auto *node = MakeExpr<BoundIntrinsic>(*_sema, value->Type, call->Range);
    node->Target = {IntrinsicKind::Mir, MirIntrinsic::Detach};
    List<BoundExpr *> arguments;
    arguments.Add(value);
    node->Arguments = _sema->Freeze(arguments);
    return node;
}

BoundExpr *Binder::BindDerivative(const AstCallExpr *call, bool forward)
{
    const StringView mode = forward ? StringView("Autodiff.Forward") : StringView("Autodiff.Backward");
    if (call->Arguments.IsEmpty())
    {
        _sema->Diagnostics.Error(DiagnosticCode::WrongArgumentCount, call->Range,
                                 "`{0}` needs the function to differentiate", mode)
            .WithHelp("name it first, such as `Autodiff.Forward(Shade, …)`");
        return Error(call->Range);
    }
    const AstNode &named = *call->Arguments[0].Value;
    BinderTarget function = ResolveTarget(call->Arguments[0].Value);
    if (function.TargetKind != BinderTarget::Kind::Functions || function.Functions.Count() != 1)
    {
        if (function.TargetKind != BinderTarget::Kind::Error)
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, named.Range,
                                     "the first argument of `{0}` names a function", mode)
                .WithLabel(function.TargetKind == BinderTarget::Kind::Functions ? "overloaded" : "not a function");
        }
        return Error(call->Range);
    }
    SemaSymbol *chosen = function.Functions[0];
    const usize count = ParametersOf(chosen->Decl).Count();
    const usize given = call->Arguments.Count() - 1;
    if (given != count + (forward ? 0 : 1))
    {
        LanguageDiagnostic &error = _sema->Diagnostics.Error(DiagnosticCode::WrongArgumentCount, call->Range,
                                                             "`{0}` takes {1} argument(s), not {2}", mode,
                                                             count + (forward ? 0 : 1), given);
        if (!forward)
        {
            error.WithNote("`Autodiff.Backward` needs dResult last");
        }
        return Error(call->Range);
    }
    if (!_sema->IsMarkedDifferentiable(chosen, function.Owner))
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, named.Range, "`{0}` is not `[Differentiable]`",
                                 chosen->Name)
            .WithHelp("mark it `[Differentiable]` (§9.12)");
        return Error(call->Range);
    }
    // An instance method is named through its object (`material.Evaluate`), which becomes its `this`.
    const bool instance = !HasAnyFlags(chosen->Decl->Modifiers, AstModifiers::Static) && function.Owner != nullptr;
    if (!GenericParametersOf(chosen->Decl).IsEmpty() || (instance && function.Receiver == nullptr))
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, named.Range,
                                 "only non-generic functions can be differentiated, and an instance method only "
                                 "through its object")
            .WithHelp("name an instance method through a value, such as `Autodiff.Forward(material.Evaluate, …)`");
        return Error(call->Range);
    }
    SemaFunction *callee = _sema->InstantiateFunction(chosen, function.Owner, {});
    if (!_sema->ResolveSignature(callee))
    {
        return Error(call->Range);
    }
    List<BinderArgument> arguments;
    if (!BindArguments(call->Arguments.Slice(1), arguments))
    {
        return Error(call->Range);
    }
    auto pairOf = [&](const SemaType *type) -> SemaStructInstance * {
        if (!type->Is(SemaTypeKind::Struct))
        {
            return nullptr;
        }
        SemaStructInstance *instance = type->GetStruct();
        return instance->Symbol->IsCore &&
                       (instance->Symbol->Name == "DifferentialPair" || instance->Symbol->Name == "DifferentialPair2")
                   ? instance
                   : nullptr;
    };
    SemaSymbol *pairSymbol = nullptr;
    auto *node = MakeExpr<BoundDerivative>(*_sema, _sema->Types.GetVoid(), call->Range);
    node->Function = callee;
    node->Receiver = instance ? function.Receiver : nullptr;
    node->Forward = forward;
    List<BoundExpr *> bound;
    for (usize i = 0; i < count; ++i)
    {
        const SemaLocal *parameter = callee->Parameters[i];
        BinderArgument &argument = arguments[i];
        SemaStructInstance *pair = pairOf(argument.Value->Type);
        if (pair != nullptr && Sema::IsNoDiff(parameter->Attributes))
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::PairForNoDiffParameter, argument.Range,
                       "the parameter `{0}` is `[NoDiff]`, so it takes a `{1}`, not a pair", parameter->Name,
                       TypeNameOf(parameter->Type).AsView())
                .WithSecondaryLabel(parameter->Range, "declared `[NoDiff]` here")
                .WithHelp("pass the value itself; it is not differentiated (§9.12)");
            return Error(call->Range);
        }
        if (pair != nullptr && IsDifferentiable(parameter->Type))
        {
            if (pair->Arguments[0].Type != parameter->Type)
            {
                _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, argument.Range,
                                         "the parameter `{0}` is `{1}`, so its pair is `{2}<{1}>`", parameter->Name,
                                         TypeNameOf(parameter->Type).AsView(), pair->Symbol->Name)
                    .WithLabel(TypeNameOf(argument.Value->Type).AsView());
                return Error(call->Range);
            }
            if (pairSymbol != nullptr && pairSymbol != pair->Symbol)
            {
                _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, argument.Range,
                                         "`{0}` takes one kind of pair: `{1}` here, `{2}` before", mode,
                                         pair->Symbol->Name, pairSymbol->Name)
                    .WithHelp("pass `DifferentialPair2` for every differentiated parameter to carry two directions");
                return Error(call->Range);
            }
            if (!forward && pair->Symbol->Name == "DifferentialPair2")
            {
                _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, argument.Range,
                                         "`DifferentialPair2` carries two forward directions; reverse mode takes "
                                         "`DifferentialPair`")
                    .WithLabel("here");
                return Error(call->Range);
            }
            pairSymbol = pair->Symbol;
            node->TangentCount = pair->Symbol->Name == "DifferentialPair2" ? 2u : 1u;
            node->PairMask |= 1u << i;
            if (argument.Mode == SemaParameterMode::Ref)
            {
                if (forward || !argument.Value->IsLValue)
                {
                    _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, argument.Range, "{0}",
                                             forward ? StringView("forward derivatives take pairs by value")
                                                     : StringView("a `ref` pair must be a variable"))
                        .WithLabel("here");
                    return Error(call->Range);
                }
                node->RefMask |= 1u << i;
            }
            bound.Add(argument.Value);
            continue;
        }
        if (parameter->Mode != SemaParameterMode::Value && parameter->Mode != SemaParameterMode::In)
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, parameter->Range,
                                     "differentiated functions take their parameters by value")
                .WithLabel("declared here");
            return Error(call->Range);
        }
        bound.Add(ConvertImplicit(argument.Value, parameter->Type));
    }
    if (count > 32 || pairSymbol == nullptr)
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, call->Range,
                                 "`{0}` needs a `DifferentialPair` for at least one parameter", mode)
            .WithHelp("pass `new DifferentialPair<T>(value, derivative)` for the parameters to differentiate");
        return Error(call->Range);
    }
    if (!IsDifferentiable(callee->ReturnType))
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, named.Range,
                                 "`{0}` returns `{1}`, which has no derivative", chosen->Name,
                                 TypeNameOf(callee->ReturnType).AsView());
        return Error(call->Range);
    }
    if (forward)
    {
        const SemaGenericArgument resultArgument[] = {{callee->ReturnType, nullptr}};
        node->Type = _sema->Types.GetStruct(_sema->InstantiateStruct(pairSymbol, resultArgument));
    }
    else
    {
        bound.Add(ConvertImplicit(arguments.Last().Value, callee->ReturnType));
    }
    node->Arguments = _sema->Freeze(bound);
    NoteCallee(callee, call->Range);
    return node;
}

bool Binder::CheckDebugOutput(const BoundIntrinsic *intrinsic)
{
    const bool isPrint = intrinsic->Target.Kind == IntrinsicKind::DebugPrint;
    const BoundExpr *text = intrinsic->Arguments[isPrint ? 0 : 1];
    if (text->Kind != BoundKind::Constant)
    {
        _sema->Diagnostics
            .Error(DiagnosticCode::NotConstant, text->Range, "the {0} must be a constant string",
                   StringView(isPrint ? "format" : "message"))
            .WithLabel("computed at run time");
        return false;
    }
    if (!isPrint)
    {
        return true;
    }
    const usize count = intrinsic->Arguments.Count() - 1;
    for (usize i = 1; i < intrinsic->Arguments.Count(); ++i)
    {
        const SemaType *type = intrinsic->Arguments[i]->Type;
        if (type->Is(SemaTypeKind::Matrix) || UnderlyingOf(type)->GetScalar() == nullptr)
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::TypeMismatch, intrinsic->Arguments[i]->Range,
                       "`Debug.Print` prints scalars and vectors, not `{0}`", TypeNameOf(type).AsView())
                .WithLabel("this argument");
            return false;
        }
    }
    List<MirFormatSegment> segments;
    String error;
    if (!ParseMirDebugFormat(text->As<BoundConstant>()->Value->Text, segments, error))
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, text->Range, "invalid format: {0}", error.AsView())
            .WithLabel("this format");
        return false;
    }
    List<bool> used;
    used.Resize(count, false);
    for (const MirFormatSegment &segment : segments)
    {
        if (segment.Argument == ~0u)
        {
            continue;
        }
        if (segment.Argument >= count)
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::InvalidOperands, text->Range, "the format references argument {0}, but there "
                       "are {1}", segment.Argument, count)
                .WithLabel("this format");
            return false;
        }
        const SemaType *scalar = UnderlyingOf(intrinsic->Arguments[segment.Argument + 1]->Type)->GetScalar();
        if (segment.Hexadecimal && !scalar->Is(SemaTypeKind::Int))
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::InvalidOperands, intrinsic->Arguments[segment.Argument + 1]->Range,
                       "argument {0} is printed in hexadecimal (`:x`), which needs an integer", segment.Argument)
                .WithLabel("not an integer");
            return false;
        }
        used[segment.Argument] = true;
    }
    for (usize i = 0; i < count; ++i)
    {
        if (!used[i])
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::InvalidOperands, intrinsic->Arguments[i + 1]->Range,
                       "the format does not print argument {0}", i)
                .WithLabel("unused")
                .WithHelp("reference every argument in the format, as `{0}` references the first");
            return false;
        }
    }
    return true;
}

BoundExpr *Binder::CallGetter(SemaSymbol *property, SemaStructInstance *owner, BoundExpr *receiver, SourceRange range)
{
    SemaFunction *getter = _sema->InstantiateFunction(property, owner, {});
    if (!_sema->ResolveSignature(getter))
    {
        return Error(range);
    }
    const AstDecl *decl = property->Decl;
    const bool isStatic = HasAnyFlags(decl->Modifiers, AstModifiers::Static);
    if (!isStatic && receiver == nullptr)
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, range, "`{0}` is an instance property", property->Name)
            .WithHelp("read it from a value: `value.Property`");
        return Error(range);
    }
    if (const AstAttribute *systemValue = Sema::FindAttribute(decl->Attributes, "SystemValue"))
    {
        SemaConstant *name = _sema->EvaluateAttributeArgument(*systemValue, 0, {}, _sema->ContextOf(property));
        const MirBuiltin builtin = name != nullptr ? FindSystemValue(name->Text) : MirBuiltin::None;
        if (builtin == MirBuiltin::None)
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidAttribute, systemValue->Range,
                                     "`{0}` is not a system value", name != nullptr ? name->Text : StringView());
            return Error(range);
        }
        NoteCallee(getter, range);
        auto *node = MakeExpr<BoundIntrinsic>(*_sema, getter->ReturnType, range);
        node->Function = getter;
        node->SystemValue = builtin;
        return node;
    }
    return MakeCall(getter, isStatic ? nullptr : receiver, {}, range);
}

BoundExpr *Binder::EvaluateCompileTime(SemaFunction *function, Span<BoundExpr *> arguments, SourceRange range,
                                       const SemaType *typeParameter)
{
    (void)typeParameter;
    const AstAttribute *attribute = Sema::FindAttribute(function->Symbol->Decl->Attributes, "CompileTime");
    SemaConstant *name = _sema->EvaluateAttributeArgument(*attribute, 0, {}, _sema->ContextOf(function->Symbol));
    const CompileTimeFunction which = name != nullptr ? FindCompileTimeFunction(name->Text) : CompileTimeFunction::None;
    auto constantArgument = [&](usize index) -> SemaConstant * {
        if (index >= arguments.Count() || arguments[index]->Kind != BoundKind::Constant)
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotConstant, index < arguments.Count() ? arguments[index]->Range : range,
                                     "`{0}` needs a constant argument", function->Symbol->Name);
            return nullptr;
        }
        return arguments[index]->As<BoundConstant>()->Value;
    };
    switch (which)
    {
    case CompileTimeFunction::DeviceSupports:
        if (SemaConstant *feature = constantArgument(0))
        {
            _sema->QueriedFeatures.Add(String(feature->Text));
            return MakeConstant(*_sema, _sema->NewBool(_sema->HasFeature(feature->Text)), range);
        }
        break;
    case CompileTimeFunction::FeatureFromName:
        if (SemaConstant *text = constantArgument(0))
        {
            return MakeConstant(*_sema, _sema->NewString(text->Text, _sema->Types.GetFeature()), range);
        }
        break;
    case CompileTimeFunction::TypeId:
    {
        // 16 bits of the hash of the type's qualified name, never zero: the same in every compilation, so passes
        // compiled apart agree on it (07 §4.3).
        const SemaType *type = function->GenericArguments.Count() == 1 ? function->GenericArguments[0].Type : nullptr;
        if (type == nullptr || !type->HasInstance())
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, range, "`TypeId` takes a struct type");
            break;
        }
        const String qualified = _sema->GetQualifiedName(type->GetStruct()->Symbol);
        if (!_sema->QueriedTypeIds.Contains(qualified))
        {
            _sema->QueriedTypeIds.Add(qualified);
        }
        const uint32 id = static_cast<uint32>(Xxh3Hash64(qualified.Data(), qualified.Length()) & 0xFFFF);
        return MakeConstant(*_sema, _sema->NewScalar(function->ReturnType, id != 0 ? id : 1), range);
    }
    default:
        _sema->Diagnostics.Error(DiagnosticCode::InvalidAttribute, attribute->Range, "unknown compile-time function");
        break;
    }
    return Error(range);
}

BoundExpr *Binder::BindUserOperator(TokenKind op, Span<BoundExpr *> operands, SourceRange range, bool &found)
{
    found = false;
    BinderTarget target;
    target.TargetKind = BinderTarget::Kind::Functions;
    target.Range = range;
    const bool conversion = op == TokenKind::KeywordImplicit || op == TokenKind::KeywordExplicit;
    auto matches = [&](const SemaSymbol *member) {
        const auto *decl = member->Decl->As<AstOperatorDecl>();
        const bool implicit = HasAnyFlags(decl->Modifiers, AstModifiers::Implicit);
        const bool explicitOnly = op == TokenKind::KeywordExplicit && HasAnyFlags(decl->Modifiers, AstModifiers::Explicit);
        return conversion ? decl->Operator == TokenKind::EndOfFile && (implicit || explicitOnly)
                          : decl->Operator == op && decl->Parameters.Count() == operands.Count();
    };
    for (BoundExpr *operand : operands)
    {
        // A Ptr declares its operators in the core library like a struct (§4.5).
        SemaStructInstance *owner = operand->Type->Is(SemaTypeKind::Struct) || operand->Type->Is(SemaTypeKind::Class)
                                        ? operand->Type->GetStruct()
                                    : operand->Type->Is(SemaTypeKind::Buffer) ? MagicOwner(operand->Type)
                                                                              : nullptr;
        for (SemaSymbol *member : owner != nullptr ? _sema->GetMembers(owner->Symbol, "operator") : Span<SemaSymbol *const>())
        {
            if (matches(member) && !target.Functions.Contains(member))
            {
                target.Functions.Add(member);
                target.Owner = owner;
            }
        }
        // Operators an extension adds (§3.5), also to vectors and scalars, which have no declared ones.
        List<SemaSymbol *> added;
        List<SemaStructInstance *> extensions;
        _sema->FindExtensionMembers(operand->Type, "operator", _context, range, added, extensions);
        for (SemaSymbol *member : added)
        {
            if (member->Kind == SemaSymbolKind::Operator && matches(member) && !target.Functions.Contains(member))
            {
                target.Functions.Add(member);
            }
        }
        for (SemaStructInstance *extension : extensions)
        {
            if (!target.Extensions.Contains(extension))
            {
                target.Extensions.Add(extension);
            }
        }
    }
    if (target.Functions.IsEmpty())
    {
        return nullptr;
    }
    found = true;
    List<BinderArgument> arguments;
    for (BoundExpr *operand : operands)
    {
        BinderArgument argument;
        argument.Value = operand;
        argument.Range = operand->Range;
        argument.Constant = operand->Kind == BoundKind::Constant ? operand->As<BoundConstant>()->Value : nullptr;
        arguments.Add(argument);
    }
    return ResolveCall(target, arguments.AsSpan(), range);
}
} // namespace Marlin
