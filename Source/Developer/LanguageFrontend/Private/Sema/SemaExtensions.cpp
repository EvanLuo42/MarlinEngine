#include "Sema/Sema.h"
#include "Sema/SemaHelpers.h"

namespace Marlin
{
namespace
{
/// Whether *member* of an interface is a requirement its implementations provide: an instance method, property or
/// indexer, or a static method, property or operator that is `abstract` or has no body (§3.5). Other static members
/// are helpers of the interface itself.
bool IsRequirement(const SemaSymbol *member)
{
    if (member->Kind != SemaSymbolKind::Function && member->Kind != SemaSymbolKind::Property &&
        member->Kind != SemaSymbolKind::Operator)
    {
        return false;
    }
    if (!member->IsStatic())
    {
        return member->Kind != SemaSymbolKind::Operator;
    }
    const AstDecl *decl = member->Decl;
    if (HasAnyFlags(decl->Modifiers, AstModifiers::Abstract))
    {
        return true;
    }
    if (const auto *function = decl->TryAs<AstFunctionDecl>())
    {
        return function->Body.IsMissing();
    }
    if (const auto *op = decl->TryAs<AstOperatorDecl>())
    {
        return op->Body.IsMissing();
    }
    const auto *property = decl->As<AstPropertyDecl>();
    return property->Getter.IsMissing() && (!property->Setter.Present || property->Setter.Body.IsMissing());
}

/// The accessors a property member stands for, or None for a method or operator.
usize AccessorsOf(const SemaSymbol *member, SemaAccessor (&accessors)[2])
{
    const auto *property = member->Decl->TryAs<AstPropertyDecl>();
    if (property == nullptr)
    {
        accessors[0] = SemaAccessor::None;
        return 1;
    }
    usize count = 0;
    if (property->HasGetter)
    {
        accessors[count++] = SemaAccessor::Getter;
    }
    if (property->Setter.Present)
    {
        accessors[count++] = SemaAccessor::Setter;
    }
    return count;
}

bool HasSignature(const SemaFunction *function, const SemaSignature &wanted)
{
    if (function->ReturnType != wanted.ReturnType || function->Parameters.Count() != wanted.Parameters.Count())
    {
        return false;
    }
    for (usize i = 0; i < wanted.Parameters.Count(); ++i)
    {
        if (function->Parameters[i]->Type != wanted.Parameters[i] || function->Parameters[i]->Mode != wanted.Modes[i])
        {
            return false;
        }
    }
    return true;
}

/// Whether the type *node* names the implementing type: `this`, or an associated type of *interfaceSymbol*.
bool NamesImplementingType(const AstNode *node, const SemaSymbol *interfaceSymbol)
{
    if (node == nullptr)
    {
        return false;
    }
    switch (node->Kind)
    {
    case AstKind::SelfType:
        return true;
    case AstKind::ArrayType:
        return NamesImplementingType(node->As<AstArrayType>()->Element, interfaceSymbol);
    case AstKind::NullableType:
        return NamesImplementingType(node->As<AstNullableType>()->Element, interfaceSymbol);
    case AstKind::NamedType:
    {
        const auto *named = node->As<AstNamedType>();
        if (named->Qualifier == nullptr)
        {
            if (const List<SemaSymbol *> *members = interfaceSymbol->Members.Find(named->Name))
            {
                for (const SemaSymbol *member : *members)
                {
                    if (member->Kind == SemaSymbolKind::AssociatedType)
                    {
                        return true;
                    }
                }
            }
        }
        else if (named->Qualifier->Kind == AstKind::SelfType)
        {
            return true;
        }
        for (const AstNode *argument : named->GenericArguments)
        {
            if (NamesImplementingType(argument, interfaceSymbol))
            {
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

SemaParameterMode ModeOf(AstModifiers modifiers)
{
    return HasAnyFlags(modifiers, AstModifiers::Ref)   ? SemaParameterMode::Ref
           : HasAnyFlags(modifiers, AstModifiers::Out) ? SemaParameterMode::Out
           : HasAnyFlags(modifiers, AstModifiers::In)  ? SemaParameterMode::In
                                                       : SemaParameterMode::Value;
}
} // namespace

bool Sema::UsesImplementingType(const SemaSymbol *member)
{
    const AstDecl *decl = member->Decl;
    const AstNode *type = nullptr;
    if (const auto *function = decl->TryAs<AstFunctionDecl>())
    {
        type = function->ReturnType;
    }
    else if (const auto *property = decl->TryAs<AstPropertyDecl>())
    {
        type = property->Type;
    }
    else if (const auto *op = decl->TryAs<AstOperatorDecl>())
    {
        type = op->ReturnType;
    }
    if (NamesImplementingType(type, member->Parent))
    {
        return true;
    }
    for (const AstParameterDecl *parameter : ParametersOf(decl))
    {
        if (NamesImplementingType(parameter->Type, member->Parent))
        {
            return true;
        }
    }
    return false;
}

bool Sema::ConformsTo(const SemaType *type, const SemaType *interfaceType)
{
    SemaStructInstance *wanted = interfaceType->GetStruct();
    const bool declared = wanted != nullptr && type->HasInstance() ? ImplementsInterface(type, wanted)
                                                                   : Satisfies(type, interfaceType->GetSymbol());
    if (declared || wanted == nullptr)
    {
        return declared;
    }
    for (SemaStructInstance *extension : GetExtensions(type))
    {
        if (ResolveHierarchy(extension) && extension->Interfaces.Contains(wanted))
        {
            return true;
        }
    }
    return false;
}

Span<SemaStructInstance *const> Sema::GetExtensions(const SemaType *type)
{
    // A new extension anywhere may apply to *type*, so matching depends on every file (NameDependencies.h).
    _queriedExtensions = _queriedExtensions || (_recordNames && !type->IsError());
    if (type->IsError() || _extensions.IsEmpty())
    {
        return {};
    }
    if (UniquePtr<List<SemaStructInstance *>> *found = _extensionSets.Find(type))
    {
        return (*found)->AsSpan();
    }
    _extensionSets.Add(type, MakeUnique<List<SemaStructInstance *>>());
    List<SemaStructInstance *> &instances = **_extensionSets.Find(type);
    for (SemaSymbol *extension : _extensions)
    {
        List<SemaGenericArgument> arguments;
        if (MatchExtension(extension, type, arguments))
        {
            UseFile(extension->File);
            instances.Add(InstantiateStruct(extension, arguments.AsSpan()));
        }
    }
    CheckExtensions(type, instances.AsSpan());
    return instances.AsSpan();
}

bool Sema::MatchExtension(SemaSymbol *extension, const SemaType *type, List<SemaGenericArgument> &arguments)
{
    const auto *decl = extension->Decl->As<AstExtensionDecl>();
    if (decl->GenericParameters.IsEmpty())
    {
        return GetOwnerType(InstantiateStruct(extension, {})) == type;
    }
    // `extension<T> Texture2D<T>` applies to the types its pattern infers every parameter from, where they satisfy
    // its constraints.
    List<SemaGenericBinding> bindings;
    for (const AstGenericParameterDecl *parameter : decl->GenericParameters)
    {
        bindings.Add({parameter->Name, {}});
    }
    if (!InferGenerics(extension, decl->ExtendedType, type, bindings))
    {
        return false;
    }
    for (const SemaGenericBinding &binding : bindings)
    {
        if (binding.Argument.Type == nullptr && binding.Argument.Value == nullptr)
        {
            return false;
        }
        arguments.Add(binding.Argument);
    }
    if (!CheckConstraints(extension, bindings.AsSpan(), decl->NameRange, false))
    {
        return false;
    }
    // The pattern may leave parts of the type unsaid, such as a writable texture's format: the extension applies only
    // when its arguments give back the type itself.
    SemaStructInstance *instance = InstantiateStruct(extension, arguments.AsSpan());
    return GetOwnerType(instance) == type;
}

void Sema::CheckExtensions(const SemaType *type, Span<SemaStructInstance *const> extensions)
{
    bool declaresInterfaces = false;
    for (const SemaStructInstance *extension : extensions)
    {
        declaresInterfaces =
            declaresInterfaces || !extension->Symbol->Decl->As<AstExtensionDecl>()->Interfaces.IsEmpty();
    }
    if (!declaresInterfaces)
    {
        return;
    }
    // Where each interface the type implements is implemented: by its declaration or by an extension.
    List<SemaStructInstance *> implemented;
    List<SourceRange> where;
    SemaStructInstance *own = nullptr;
    if (type->HasInstance())
    {
        own = type->GetStruct();
    }
    else
    {
        List<SemaGenericArgument> arguments;
        const uint32 family = GetMagicFamily(type, arguments);
        SemaSymbol *magic = family != 0 ? GetMagicSymbol(family) : nullptr;
        own = magic != nullptr ? InstantiateStruct(magic, arguments.AsSpan()) : nullptr;
    }
    if (own != nullptr && ResolveHierarchy(own))
    {
        for (SemaStructInstance *interfaceInstance : own->Interfaces)
        {
            implemented.Add(interfaceInstance);
            where.Add(own->Symbol->Decl->NameRange);
        }
    }
    for (SemaStructInstance *extension : extensions)
    {
        if (!ResolveHierarchy(extension))
        {
            continue;
        }
        const auto *decl = extension->Symbol->Decl->As<AstExtensionDecl>();
        List<SemaGenericBinding> bindings;
        GetBindings(extension, bindings);
        const SemaContext context = ContextOf(extension->Symbol, Freeze(bindings), extension);
        for (AstNode *node : decl->Interfaces)
        {
            const SemaType *interfaceType = ResolveType(node, context);
            SemaStructInstance *declared =
                interfaceType->Is(SemaTypeKind::Interface) ? interfaceType->GetStruct() : nullptr;
            if (declared == nullptr)
            {
                continue;
            }
            const usize first = implemented.IndexOf(declared);
            if (first != List<SemaStructInstance *>::NotFound)
            {
                Diagnostics
                    .Error(DiagnosticCode::DuplicateConformance, node->Range, "`{0}` implements `{1}` more than once",
                           TypeNameOf(type).AsView(), TypeNameOf(interfaceType).AsView())
                    .WithLabel("implemented again by this extension")
                    .WithSecondaryLabel(where[first], "first implemented here")
                    .WithNote("a type implements an interface once: in its declaration or in one extension (§3.5)");
                continue;
            }
            List<SemaStructInstance *> all;
            all.Add(declared);
            all.AddRange(declared->Interfaces.AsSpan());
            for (SemaStructInstance *interfaceInstance : all)
            {
                if (!implemented.Contains(interfaceInstance))
                {
                    implemented.Add(interfaceInstance);
                    where.Add(node->Range);
                    // An extension that leaves a requirement out does not make the type conform.
                    if (!CheckConformance(type, nullptr, interfaceInstance, decl->NameRange, nullptr))
                    {
                        extension->HierarchyFailed = true;
                    }
                }
            }
        }
    }
}

void Sema::FindExtensionMembers(const SemaType *type, StringView name, const SemaContext &context, SourceRange range,
                                List<SemaSymbol *> &members, List<SemaStructInstance *> &owners)
{
    // The most specific extensions hide the others: those outside the core library, then those with the fewest
    // generic parameters, so `extension half3` wins over the core library's `extension<T> T`.
    auto rankOf = [](const SemaStructInstance *extension) {
        return (extension->Symbol->IsCore ? 1000u : 0u) + static_cast<uint32>(extension->Arguments.Count());
    };
    uint32 best = ~0u;
    for (SemaStructInstance *extension : GetExtensions(type))
    {
        if (!IsAccessible(context, extension->Symbol) || rankOf(extension) > best)
        {
            continue;
        }
        List<SemaSymbol *> found;
        for (SemaSymbol *member : GetMembers(extension->Symbol, name))
        {
            if (IsAccessible(context, member))
            {
                found.Add(member);
            }
        }
        if (found.IsEmpty())
        {
            continue;
        }
        if (rankOf(extension) < best)
        {
            best = rankOf(extension);
            members.Clear();
            owners.Clear();
        }
        members.AddRange(found.AsSpan());
        owners.Add(extension);
    }
    bool onlyFunctions = true;
    for (const SemaSymbol *member : members)
    {
        onlyFunctions =
            onlyFunctions && (member->Kind == SemaSymbolKind::Function || member->Kind == SemaSymbolKind::Operator);
    }
    if (owners.Count() > 1 && !onlyFunctions)
    {
        LanguageDiagnostic &error =
            Diagnostics.Error(DiagnosticCode::AmbiguousName, range,
                              "`{0}` is added to `{1}` by more than one extension", name, TypeNameOf(type).AsView());
        for (const SemaStructInstance *owner : owners)
        {
            error.WithNote(owner->Symbol->Decl->NameRange, "an extension adding it");
        }
        members.Clear();
        owners.Clear();
    }
}

bool Sema::CheckConformance(const SemaType *self, SemaStructInstance *implementer,
                            SemaStructInstance *interfaceInstance, SourceRange range, SemaInterfaceMap *map)
{
    List<SemaGenericBinding> interfaceBindings;
    GetBindings(interfaceInstance, interfaceBindings);
    const Span<const SemaGenericBinding> frozen = Freeze(interfaceBindings);
    // The requirements are read with `this` and the associated types standing for the implementing type.
    auto requirementContext = [&](const SemaSymbol *requirement) {
        SemaContext context = ContextOf(requirement, frozen, interfaceInstance);
        context.SelfType = self;
        return context;
    };

    // Where implementations are looked for: the declaring struct or class and its bases, else the type itself, and
    // then the type's extensions. A class implements its own interfaces' instance members itself, so that its
    // interface maps dispatch to methods of the class.
    List<SemaStructInstance *> scopes;
    if (implementer != nullptr)
    {
        for (SemaStructInstance *current = implementer; current != nullptr; current = current->Base)
        {
            scopes.Add(current);
        }
    }
    else if (self->HasInstance())
    {
        scopes.Add(self->GetStruct());
    }
    else
    {
        List<SemaGenericArgument> arguments;
        const uint32 family = GetMagicFamily(self, arguments);
        SemaSymbol *magic = family != 0 ? GetMagicSymbol(family) : nullptr;
        if (magic != nullptr)
        {
            scopes.Add(InstantiateStruct(magic, arguments.AsSpan()));
        }
    }
    const usize ownScopes = scopes.Count();
    scopes.AddRange(GetExtensions(self));

    bool ok = true;
    for (SemaSymbol *requirement : interfaceInstance->Symbol->Ordered)
    {
        if (requirement->Kind == SemaSymbolKind::AssociatedType)
        {
            SemaStructInstance *owner = nullptr;
            SemaSymbol *binding = FindAssociatedBinding(self, requirement->Name, owner);
            if (binding == nullptr)
            {
                Diagnostics
                    .Error(DiagnosticCode::MissingAssociatedType, range,
                           "`{0}` does not bind the associated type `{1}.{2}`", TypeNameOf(self).AsView(),
                           interfaceInstance->Symbol->Name, requirement->Name)
                    .WithSecondaryLabel(requirement->Decl->NameRange, "required here")
                    .WithHelp(Format("bind it: `public type {0} = Type;`", requirement->Name).AsView());
                ok = false;
                continue;
            }
            const auto *bindingDecl = binding->Decl->As<AstTypeDecl>();
            const SemaType *bound = ResolveAssociatedType(binding, owner, self, bindingDecl->Target->Range);
            if (bound->IsError())
            {
                ok = false;
                continue;
            }
            const SemaContext context = requirementContext(requirement);
            for (AstNode *node : requirement->Decl->As<AstTypeDecl>()->Bounds)
            {
                const SemaType *boundType = ResolveType(node, context);
                if (boundType->IsError())
                {
                    ok = false;
                    continue;
                }
                if (!boundType->Is(SemaTypeKind::Interface))
                {
                    Diagnostics.Error(DiagnosticCode::NotAType, node->Range,
                                      "an associated type's constraint must be an interface");
                    ok = false;
                    continue;
                }
                if (!ConformsTo(bound, boundType))
                {
                    Diagnostics
                        .Error(DiagnosticCode::ConstraintNotSatisfied, bindingDecl->Target->Range,
                               "`{0}` does not satisfy `{1}`", TypeNameOf(bound).AsView(),
                               TypeNameOf(boundType).AsView())
                        .WithLabel(
                            Format("bound to `{0}.{1}` here", TypeNameOf(self).AsView(), requirement->Name).AsView())
                        .WithSecondaryLabel(node->Range, "required by the associated type");
                    ok = false;
                }
            }
            continue;
        }
        if (!IsRequirement(requirement))
        {
            continue;
        }
        const bool isStatic = requirement->IsStatic();
        // Only instance members whose signature is the same for every implementation have virtual slots.
        const bool slotted = !isStatic && !UsesImplementingType(requirement);
        const auto *requiredOperator = requirement->Decl->TryAs<AstOperatorDecl>();
        SemaAccessor accessors[2];
        const usize accessorCount = AccessorsOf(requirement, accessors);
        for (usize a = 0; a < accessorCount; ++a)
        {
            const SemaAccessor accessor = accessors[a];
            SemaSignature wanted;
            if (!ResolveRequirement(requirement, accessor, requirementContext(requirement), wanted))
            {
                ok = false;
                if (slotted && map != nullptr)
                {
                    map->Methods.Add(nullptr);
                }
                continue;
            }
            SemaFunction *found = nullptr;
            const bool classInstanceMember =
                !isStatic && implementer != nullptr && implementer->Symbol->Kind == SemaSymbolKind::Class;
            const usize searched = classInstanceMember ? ownScopes : scopes.Count();
            for (usize s = 0; s < searched && found == nullptr; ++s)
            {
                SemaStructInstance *scope = scopes[s];
                for (SemaSymbol *candidate : GetMembers(scope->Symbol, requirement->Name))
                {
                    if (found != nullptr || candidate->Kind != requirement->Kind || candidate->IsStatic() != isStatic ||
                        candidate->Visibility != SemaVisibility::Public ||
                        !GenericParametersOf(candidate->Decl).IsEmpty())
                    {
                        continue;
                    }
                    if (requiredOperator != nullptr &&
                        candidate->Decl->As<AstOperatorDecl>()->Operator != requiredOperator->Operator)
                    {
                        continue;
                    }
                    if (const auto *property = candidate->Decl->TryAs<AstPropertyDecl>();
                        property != nullptr &&
                        (accessor == SemaAccessor::Setter ? !property->Setter.Present : !property->HasGetter))
                    {
                        continue;
                    }
                    SemaFunction *function = InstantiateFunction(candidate, scope, {}, accessor);
                    if (ResolveSignature(function) && HasSignature(function, wanted))
                    {
                        found = function;
                    }
                }
            }
            if (found == nullptr && FindAttribute(requirement->Decl->Attributes, "Optional") == nullptr)
            {
                const String name = requiredOperator != nullptr
                                        ? Format("operator {0}", GetOperatorText(requiredOperator->Operator))
                                        : String(requirement->Name);
                Diagnostics
                    .Error(DiagnosticCode::MissingInterfaceMember, range, "`{0}` does not implement `{1}.{2}`",
                           TypeNameOf(self).AsView(), interfaceInstance->Symbol->Name, name.AsView())
                    .WithSecondaryLabel(requiredOperator != nullptr ? requirement->Decl->Range
                                                                    : requirement->Decl->NameRange,
                                        "required here")
                    .WithHelp(isStatic ? "implement it as a public static member with the same signature"
                                       : "implement it as a public instance member with the same signature");
                ok = false;
            }
            if (slotted && map != nullptr)
            {
                map->Methods.Add(found);
            }
        }
    }
    return ok;
}

bool Sema::ResolveRequirement(SemaSymbol *requirement, SemaAccessor accessor, const SemaContext &context,
                              SemaSignature &signature)
{
    UseFile(requirement->File);
    const AstDecl *decl = requirement->Decl;
    bool ok = true;
    const SemaType *valueType = nullptr;
    if (const auto *property = decl->TryAs<AstPropertyDecl>())
    {
        const SemaType *propertyType = ResolveType(property->Type, context);
        ok = !propertyType->IsError();
        signature.ReturnType = accessor == SemaAccessor::Setter ? Types.GetVoid() : propertyType;
        valueType = accessor == SemaAccessor::Setter ? propertyType : nullptr;
    }
    else
    {
        const auto *function = decl->TryAs<AstFunctionDecl>();
        AstNode *returnType = function != nullptr ? function->ReturnType : decl->As<AstOperatorDecl>()->ReturnType;
        signature.ReturnType = ResolveType(returnType, context);
        ok = !signature.ReturnType->IsError();
    }
    for (const AstParameterDecl *parameter : ParametersOf(decl))
    {
        const SemaType *type = ResolveDeclaredType(parameter->Type, parameter->Attributes, context);
        ok = ok && !type->IsError();
        signature.Parameters.Add(type);
        signature.Modes.Add(ModeOf(parameter->Modifiers));
    }
    if (valueType != nullptr)
    {
        signature.Parameters.Add(valueType);
        signature.Modes.Add(SemaParameterMode::Value);
    }
    return ok;
}

const SemaType *Sema::ResolveAssociatedType(SemaSymbol *member, SemaStructInstance *owner, const SemaType *self,
                                            SourceRange range)
{
    UseFile(member->File);
    const auto *decl = member->Decl->As<AstTypeDecl>();
    if (decl->Target == nullptr)
    {
        // A requirement of an interface stands for the binding of the type implementing it.
        if (self == nullptr)
        {
            Diagnostics
                .Error(DiagnosticCode::NotAType, range,
                       "`{0}` is an associated type of `{1}`, known only for a type implementing it", member->Name,
                       member->Parent->Name)
                .WithHelp(Format("name it through a type parameter: `T.{0}`", member->Name).AsView());
            return Types.GetError();
        }
        SemaStructInstance *bindingOwner = nullptr;
        SemaSymbol *binding = FindAssociatedBinding(self, member->Name, bindingOwner);
        if (binding == nullptr)
        {
            Diagnostics
                .Error(DiagnosticCode::MissingAssociatedType, range,
                       "`{0}` does not bind the associated type `{1}.{2}`", TypeNameOf(self).AsView(),
                       member->Parent->Name, member->Name)
                .WithSecondaryLabel(decl->NameRange, "declared here")
                .WithHelp(Format("bind it: `public type {0} = Type;`", member->Name).AsView());
            return Types.GetError();
        }
        return ResolveAssociatedType(binding, bindingOwner, self, range);
    }
    if ((owner == nullptr || owner->Symbol != member->Parent) && GenericParametersOf(member->Parent->Decl).IsEmpty())
    {
        owner = InstantiateStruct(member->Parent, {});
    }
    else if (owner != nullptr && owner->Symbol != member->Parent)
    {
        owner = nullptr;
    }
    const bool cacheable = owner == nullptr || owner->Arguments.IsEmpty();
    if (cacheable && member->Type != nullptr)
    {
        return member->Type;
    }
    if (member->TypeResolving)
    {
        Diagnostics.Error(DiagnosticCode::NotAType, decl->Target->Range, "the associated type `{0}` refers to itself",
                          member->Name);
        return Types.GetError();
    }
    List<SemaGenericBinding> bindings;
    GetBindings(owner, bindings);
    const SemaContext context = ContextOf(member, Freeze(bindings), owner);
    member->TypeResolving = true;
    const SemaType *type = ResolveType(decl->Target, context);
    member->TypeResolving = false;
    if (cacheable)
    {
        member->Type = type;
    }
    return type;
}

SemaSymbol *Sema::FindAssociatedBinding(const SemaType *type, StringView name, SemaStructInstance *&owner)
{
    auto search = [&](SemaStructInstance *instance) -> SemaSymbol * {
        for (SemaSymbol *member : GetMembers(instance->Symbol, name))
        {
            if (member->Kind == SemaSymbolKind::AssociatedType && member->Decl->As<AstTypeDecl>()->Target != nullptr)
            {
                owner = instance;
                return member;
            }
        }
        return nullptr;
    };
    if (type->HasInstance())
    {
        SemaStructInstance *instance = type->GetStruct();
        if (instance->Symbol->Kind == SemaSymbolKind::Class)
        {
            ResolveHierarchy(instance);
        }
        for (SemaStructInstance *current = instance; current != nullptr; current = current->Base)
        {
            if (SemaSymbol *found = search(current))
            {
                return found;
            }
        }
    }
    for (SemaStructInstance *extension : GetExtensions(type))
    {
        if (SemaSymbol *found = search(extension))
        {
            return found;
        }
    }
    return nullptr;
}
} // namespace Marlin
