#include "Sema/Sema.h"
#include "Sema/SemaBuiltins.h"
#include "Sema/SemaHelpers.h"

namespace Marlin
{
namespace
{
Span<AstNode *> BasesOf(const AstDecl *decl)
{
    if (const auto *structDecl = decl->TryAs<AstStructDecl>())
    {
        return structDecl->Interfaces;
    }
    if (const auto *interfaceDecl = decl->TryAs<AstInterfaceDecl>())
    {
        return interfaceDecl->Bases;
    }
    if (const auto *extension = decl->TryAs<AstExtensionDecl>())
    {
        return extension->Interfaces;
    }
    return {};
}

bool SameSignature(const SemaFunction *a, const SemaFunction *b)
{
    if (a->Accessor != b->Accessor || a->Parameters.Count() != b->Parameters.Count() ||
        a->ReturnType != b->ReturnType)
    {
        return false;
    }
    for (usize i = 0; i < a->Parameters.Count(); ++i)
    {
        if (a->Parameters[i]->Type != b->Parameters[i]->Type || a->Parameters[i]->Mode != b->Parameters[i]->Mode)
        {
            return false;
        }
    }
    return true;
}

bool HasBody(const SemaFunction *function)
{
    const AstDecl *decl = function->Symbol->Decl;
    // A `[Native]` member's body is in the VM (MarlinScript §7.1).
    if (Sema::FindAttribute(decl->Attributes, "Native") != nullptr ||
        Sema::FindAttribute(decl->Attributes, "Intrinsic") != nullptr)
    {
        return true;
    }
    switch (decl->Kind)
    {
    case AstKind::FunctionDecl:
        return !decl->As<AstFunctionDecl>()->Body.IsMissing();
    case AstKind::PropertyDecl:
    {
        const auto *property = decl->As<AstPropertyDecl>();
        return function->Accessor == SemaAccessor::Setter ? !property->Setter.Body.IsMissing()
                                                          : !property->Getter.IsMissing();
    }
    default:
        return true;
    }
}
} // namespace

const SemaType *Sema::GetInstanceType(SemaStructInstance *instance)
{
    switch (instance->Symbol->Kind)
    {
    case SemaSymbolKind::Class:
        return Types.GetClass(instance);
    case SemaSymbolKind::Delegate:
        return Types.GetDelegate(instance);
    case SemaSymbolKind::Interface:
        return Types.GetInterface(instance);
    default:
        return GetOwnerType(instance);
    }
}

SemaStructInstance *Sema::GetObjectClass()
{
    return _objectSymbol != nullptr ? InstantiateStruct(_objectSymbol, {}) : nullptr;
}

AstConstructorDecl *Sema::MakeDefaultConstructor(AstStructDecl *owner)
{
    auto *block = Memory.New<AstBlockStmt>();
    block->Kind = AstKind::BlockStmt;
    block->Range = owner->NameRange;
    auto *decl = Memory.New<AstConstructorDecl>();
    decl->Kind = AstKind::ConstructorDecl;
    decl->Range = owner->NameRange;
    decl->Name = owner->Name;
    decl->NameRange = owner->NameRange;
    decl->Parent = owner;
    decl->Modifiers = HasAnyFlags(owner->Modifiers, AstModifiers::Abstract) ? AstModifiers::Protected
                                                                              : AstModifiers::Public;
    decl->Body.Block = block;
    decl->Body.BodyBegin = 0;
    decl->Body.BodyEnd = 1;
    decl->Body.Parsed = true;
    return decl;
}

bool Sema::IsVirtual(const SemaFunction *function)
{
    return function->Slot != ~0u;
}

bool Sema::IsAutoProperty(const SemaSymbol *symbol)
{
    if (symbol->Kind != SemaSymbolKind::Property || symbol->Parent == nullptr ||
        (symbol->Parent->Kind != SemaSymbolKind::Class && symbol->Parent->Kind != SemaSymbolKind::Struct) ||
        symbol->File == nullptr || symbol->File->File->GetDialect() != LanguageDialect::Script)
    {
        return false;
    }
    const auto *property = symbol->Decl->As<AstPropertyDecl>();
    return !property->IsIndexer && property->HasGetter && property->Getter.IsMissing() &&
           (!property->Setter.Present || property->Setter.Body.IsMissing()) &&
           !HasAnyFlags(property->Modifiers, AstModifiers::Abstract) &&
           FindAttribute(property->Attributes, "Native") == nullptr;
}

bool Sema::ResolveHierarchy(SemaStructInstance *instance)
{
    if (instance->HierarchyResolved || (instance->BasesResolved && instance->HierarchyResolving))
    {
        return !instance->HierarchyFailed;
    }
    SemaSymbol *symbol = instance->Symbol;
    if (instance->BasesResolving)
    {
        Diagnostics.Error(DiagnosticCode::InvalidGenericArguments, symbol->Decl->NameRange,
                          "`{0}` derives from itself", symbol->Name)
            .WithLabel("the inheritance goes around in a circle");
        instance->HierarchyFailed = true;
        return false;
    }
    instance->BasesResolving = true;
    instance->HierarchyResolving = true;
    UseFile(symbol->File);
    List<SemaGenericBinding> bindings;
    GetBindings(instance, bindings);
    const SemaContext context = ContextOf(symbol, Freeze(bindings), instance);
    const bool isClass = symbol->Kind == SemaSymbolKind::Class;
    bool ok = true;

    auto addInterface = [&](SemaStructInstance *implemented) {
        if (!instance->Interfaces.Contains(implemented))
        {
            instance->Interfaces.Add(implemented);
        }
    };
    const Span<AstNode *> bases = BasesOf(symbol->Decl);
    for (usize i = 0; i < bases.Count(); ++i)
    {
        const SemaType *type = ResolveType(bases[i], context);
        if (type->IsError())
        {
            ok = false;
            continue;
        }
        if (type->Is(SemaTypeKind::Class))
        {
            SemaStructInstance *base = type->GetStruct();
            if (!isClass || i != 0)
            {
                Diagnostics.Error(DiagnosticCode::NotAType, bases[i]->Range, "{0}",
                                  isClass ? StringView("a class has one base class, written first")
                                          : StringView("only a class can derive from a class"))
                    .WithLabel("a class");
                ok = false;
                continue;
            }
            const AstModifiers baseModifiers = base->Symbol->Decl->Modifiers;
            if (HasAnyFlags(baseModifiers, AstModifiers::Sealed | AstModifiers::Static))
            {
                Diagnostics.Error(DiagnosticCode::NotAType, bases[i]->Range, "`{0}` is {1} and cannot be derived from",
                                  base->Symbol->Name,
                                  StringView(HasAnyFlags(baseModifiers, AstModifiers::Sealed) ? "sealed" : "static"))
                    .WithSecondaryLabel(base->Symbol->Decl->NameRange, "declared here");
                ok = false;
                continue;
            }
            if (!ResolveHierarchy(base))
            {
                ok = false;
                continue;
            }
            instance->Base = base;
            continue;
        }
        if (type->Is(SemaTypeKind::Interface) && type->GetStruct() != nullptr)
        {
            SemaStructInstance *implemented = type->GetStruct();
            if (!ResolveHierarchy(implemented))
            {
                ok = false;
                continue;
            }
            addInterface(implemented);
            for (SemaStructInstance *inherited : implemented->Interfaces)
            {
                addInterface(inherited);
            }
            continue;
        }
        if (type->Is(SemaTypeKind::Interface) && symbol->Kind == SemaSymbolKind::Struct &&
            type->GetSymbol()->BuiltinId == static_cast<uint32>(BuiltinTypeId::IDifferentiable))
        {
            // The one built-in interface a struct may name, to have its differentiability checked (§9.12).
            addInterface(InstantiateStruct(type->GetSymbol(), {}));
            continue;
        }
        Diagnostics.Error(DiagnosticCode::NotAType, bases[i]->Range, "`{0}` is not {1}", TypeNameOf(type).AsView(),
                          StringView(isClass ? "a class or an interface" : "an interface"));
        ok = false;
    }
    if (isClass && instance->Base == nullptr && symbol != _objectSymbol)
    {
        instance->Base = GetObjectClass();
        if (instance->Base != nullptr && !ResolveHierarchy(instance->Base))
        {
            ok = false;
        }
    }
    if (instance->Base != nullptr)
    {
        for (SemaStructInstance *inherited : instance->Base->Interfaces)
        {
            addInterface(inherited);
        }
    }
    instance->BasesResolving = false;
    instance->BasesResolved = true;
    instance->HierarchyFailed = !ok;

    // The functions each member stands for: a method, or the accessors of a property or indexer.
    auto functionsOf = [&](SemaSymbol *member, SemaStructInstance *owner, List<SemaFunction *> &functions) {
        if (member->Kind == SemaSymbolKind::Function && !member->IsStatic())
        {
            functions.Add(InstantiateFunction(member, owner, {}));
        }
        else if (member->Kind == SemaSymbolKind::Property && !member->IsStatic())
        {
            const auto *property = member->Decl->As<AstPropertyDecl>();
            if (property->HasGetter)
            {
                functions.Add(InstantiateFunction(member, owner, {}, SemaAccessor::Getter));
            }
            if (property->Setter.Present)
            {
                functions.Add(InstantiateFunction(member, owner, {}, SemaAccessor::Setter));
            }
        }
    };

    if (symbol->Kind == SemaSymbolKind::Interface)
    {
        uint32 slot = 0;
        for (SemaSymbol *member : symbol->Ordered)
        {
            if (member->Decl != nullptr && member->Decl->Kind != AstKind::TypeDecl && UsesImplementingType(member))
            {
                continue;
            }
            List<SemaFunction *> functions;
            functionsOf(member, instance, functions);
            for (SemaFunction *function : functions)
            {
                if (!GenericParametersOf(member->Decl).IsEmpty() && context.Dialect == LanguageDialect::Script)
                {
                    Diagnostics.Error(DiagnosticCode::InvalidStatement, member->Decl->NameRange,
                                      "an interface method cannot have generic parameters in a script")
                        .WithNote("calls through an interface are dispatched at run time, and generics are "
                                  "monomorphized (MarlinScript §3.4)");
                    ok = false;
                    continue;
                }
                function->Slot = slot++;
                instance->VirtualSlots.Add(function);
            }
        }
    }
    else if (isClass)
    {
        const bool isAbstractClass = HasAnyFlags(symbol->Decl->Modifiers, AstModifiers::Abstract);
        if (instance->Base != nullptr)
        {
            instance->VirtualSlots.AddRange(instance->Base->VirtualSlots.AsSpan());
        }
        for (SemaSymbol *member : symbol->Ordered)
        {
            if (member->Decl == nullptr)
            {
                continue;
            }
            const AstModifiers modifiers = member->Decl->Modifiers;
            if (!HasAnyFlags(modifiers, AstModifiers::Virtual | AstModifiers::Abstract | AstModifiers::Override))
            {
                continue;
            }
            if (!GenericParametersOf(member->Decl).IsEmpty())
            {
                Diagnostics.Error(DiagnosticCode::InvalidStatement, member->Decl->NameRange,
                                  "a virtual method cannot have generic parameters")
                    .WithNote(
                        "generics are monomorphized, so a virtual slot cannot hold every instance (MarlinScript §3.4)");
                ok = false;
                continue;
            }
            if (HasAnyFlags(modifiers, AstModifiers::Abstract) && !isAbstractClass)
            {
                Diagnostics.Error(DiagnosticCode::InvalidModifier, member->Decl->NameRange,
                                  "`{0}` is abstract, but `{1}` is not", member->Name, symbol->Name)
                    .WithHelp("mark the class `abstract`, or give the member a body");
                ok = false;
            }
            List<SemaFunction *> functions;
            functionsOf(member, instance, functions);
            for (SemaFunction *function : functions)
            {
                if (!ResolveSignature(function))
                {
                    ok = false;
                    continue;
                }
                const bool isAbstract = HasAnyFlags(modifiers, AstModifiers::Abstract);
                if (isAbstract == HasBody(function))
                {
                    Diagnostics.Error(DiagnosticCode::InvalidModifier, member->Decl->NameRange, "{0}",
                                      isAbstract ? StringView("an abstract member has no body")
                                                 : StringView("a virtual or overriding member needs a body"));
                    ok = false;
                }
                if (!HasAnyFlags(modifiers, AstModifiers::Override))
                {
                    function->Slot = static_cast<uint32>(instance->VirtualSlots.Count());
                    instance->VirtualSlots.Add(function);
                    continue;
                }
                bool found = false;
                for (usize slot = 0; slot < instance->VirtualSlots.Count() && !found; ++slot)
                {
                    SemaFunction *inherited = instance->VirtualSlots[slot];
                    if (inherited->Symbol->Name != member->Name || !ResolveSignature(inherited) ||
                        !SameSignature(inherited, function))
                    {
                        continue;
                    }
                    found = true;
                    if (HasAnyFlags(inherited->Symbol->Decl->Modifiers, AstModifiers::Sealed))
                    {
                        Diagnostics.Error(DiagnosticCode::InvalidModifier, member->Decl->NameRange,
                                          "`{0}` is sealed and cannot be overridden", member->Name)
                            .WithSecondaryLabel(inherited->Symbol->Decl->NameRange, "sealed here");
                        ok = false;
                    }
                    function->Slot = static_cast<uint32>(slot);
                    instance->VirtualSlots[slot] = function;
                }
                if (!found)
                {
                    Diagnostics.Error(DiagnosticCode::NoMatchingOverload, member->Decl->NameRange,
                                      "`{0}` overrides nothing", member->Name)
                        .WithLabel("no virtual member of a base class has this name and signature");
                    ok = false;
                }
            }
        }
        if (!isAbstractClass && ok)
        {
            for (SemaFunction *function : instance->VirtualSlots)
            {
                if (HasAnyFlags(function->Symbol->Decl->Modifiers, AstModifiers::Abstract))
                {
                    Diagnostics.Error(DiagnosticCode::MissingInterfaceMember, symbol->Decl->NameRange,
                                      "`{0}` does not implement the abstract member `{1}`", symbol->Name,
                                      function->Symbol->Name)
                        .WithSecondaryLabel(function->Symbol->Decl->NameRange, "declared here");
                    ok = false;
                }
            }
        }
    }

    if (symbol->Kind == SemaSymbolKind::Class || symbol->Kind == SemaSymbolKind::Struct)
    {
        const SemaType *self = GetInstanceType(instance);
        for (SemaStructInstance *implemented : instance->Interfaces)
        {
            SemaInterfaceMap map;
            map.Interface = implemented;
            if (implemented->Symbol->BuiltinId == static_cast<uint32>(BuiltinTypeId::IDifferentiable))
            {
                // The compiler implements IDifferentiable for every differentiable type (§9.12); saying so only
                // checks that the struct is one.
                ok = CheckDeclaredDifferentiable(self, symbol->Decl->NameRange) && ok;
                instance->InterfaceMaps.Add(Move(map));
                continue;
            }
            ok = CheckConformance(self, instance, implemented, symbol->Decl->NameRange, &map) && ok;
            instance->InterfaceMaps.Add(Move(map));
        }
    }

    instance->HierarchyResolving = false;
    instance->HierarchyResolved = true;
    instance->HierarchyFailed = !ok;
    return ok;
}

SemaStructInstance *Sema::FindDeclaringInstance(SemaStructInstance *instance, const SemaSymbol *member)
{
    if (instance == nullptr || member->Parent == instance->Symbol)
    {
        return instance;
    }
    if (!ResolveHierarchy(instance))
    {
        return nullptr;
    }
    for (SemaStructInstance *base = instance->Base; base != nullptr; base = base->Base)
    {
        if (base->Symbol == member->Parent)
        {
            return base;
        }
    }
    for (SemaStructInstance *implemented : instance->Interfaces)
    {
        if (implemented->Symbol == member->Parent)
        {
            return implemented;
        }
    }
    if (instance->Symbol->Kind == SemaSymbolKind::Interface && _objectSymbol == member->Parent)
    {
        return GetObjectClass();
    }
    return nullptr;
}

void Sema::GetInstanceMembers(SemaStructInstance *instance, StringView name, List<SemaSymbol *> &members)
{
    auto take = [&](const SemaStructInstance *declaring, bool inherited) {
        for (SemaSymbol *symbol : GetMembers(declaring->Symbol, name))
        {
            if (inherited && symbol->Kind == SemaSymbolKind::Constructor)
            {
                continue;
            }
            if (HasAnyFlags(symbol->Decl->Modifiers, AstModifiers::Override) || members.Contains(symbol))
            {
                continue;
            }
            members.Add(symbol);
        }
    };
    take(instance, false);
    if (!ResolveHierarchy(instance))
    {
        return;
    }
    for (SemaStructInstance *base = instance->Base; base != nullptr; base = base->Base)
    {
        take(base, true);
    }
    if (instance->Symbol->Kind == SemaSymbolKind::Interface)
    {
        for (SemaStructInstance *inherited : instance->Interfaces)
        {
            take(inherited, true);
        }
        if (SemaStructInstance *object = GetObjectClass())
        {
            take(object, true);
        }
    }
}

bool Sema::DerivesFrom(const SemaStructInstance *derived, const SemaStructInstance *base)
{
    for (const SemaStructInstance *current = derived; current != nullptr; current = current->Base)
    {
        if (current == base)
        {
            return true;
        }
    }
    return false;
}

bool Sema::ImplementsInterface(const SemaType *type, const SemaStructInstance *interfaceInstance)
{
    SemaStructInstance *instance = nullptr;
    if (type->HasInstance())
    {
        instance = type->GetStruct();
    }
    else
    {
        List<SemaGenericArgument> arguments;
        const uint32 family = GetMagicFamily(type, arguments);
        SemaSymbol *magic = family != 0 ? GetMagicSymbol(family) : nullptr;
        instance = magic != nullptr ? InstantiateStruct(magic, arguments.AsSpan()) : nullptr;
    }
    if (instance == nullptr || !ResolveHierarchy(instance))
    {
        return false;
    }
    return instance == interfaceInstance || instance->Interfaces.Contains(const_cast<SemaStructInstance *>(interfaceInstance));
}

bool Sema::IsImplicitReference(const SemaType *from, const SemaType *to)
{
    if (from == to)
    {
        return true;
    }
    if (from->Is(SemaTypeKind::Null))
    {
        return to->IsReference() || to->Is(SemaTypeKind::Nullable);
    }
    if (!from->IsReference() || !to->IsReference())
    {
        return false;
    }
    // Every reference is an `object`; a string is one of the string class, an array one of `Array<T>`.
    const SemaStructInstance *object = GetObjectClass();
    if (to->Is(SemaTypeKind::Class) && to->GetStruct() == object && object != nullptr)
    {
        return true;
    }
    if (to->Is(SemaTypeKind::Interface))
    {
        return to->GetStruct() != nullptr && ImplementsInterface(from, to->GetStruct());
    }
    if (from->Is(SemaTypeKind::Class) && to->Is(SemaTypeKind::Class))
    {
        return ResolveHierarchy(from->GetStruct()) && DerivesFrom(from->GetStruct(), to->GetStruct());
    }
    return false;
}

bool Sema::GetDelegateSignature(SemaStructInstance *delegate, SemaSignature &signature)
{
    const auto *decl = delegate->Symbol->Decl->As<AstDelegateDecl>();
    List<SemaGenericBinding> bindings;
    GetBindings(delegate, bindings);
    const SemaContext context = ContextOf(delegate->Symbol, Freeze(bindings), delegate);
    signature.ReturnType = ResolveType(decl->ReturnType, context);
    bool ok = !signature.ReturnType->IsError();
    for (const AstParameterDecl *parameter : decl->Parameters)
    {
        const SemaType *type = ResolveType(parameter->Type, context);
        ok = ok && !type->IsError();
        signature.Parameters.Add(type);
        signature.Modes.Add(HasAnyFlags(parameter->Modifiers, AstModifiers::Ref)   ? SemaParameterMode::Ref
                            : HasAnyFlags(parameter->Modifiers, AstModifiers::Out) ? SemaParameterMode::Out
                            : HasAnyFlags(parameter->Modifiers, AstModifiers::In)  ? SemaParameterMode::In
                                                                                   : SemaParameterMode::Value);
    }
    return ok;
}

void Sema::GetFunctionSignature(SemaFunction *function, SemaSignature &signature)
{
    signature.ReturnType = function->ReturnType;
    for (const SemaLocal *parameter : function->Parameters)
    {
        signature.Parameters.Add(parameter->Type);
        signature.Modes.Add(parameter->Mode);
    }
}

SemaGlobal *Sema::GetStaticField(SemaSymbol *field, SemaStructInstance *owner)
{
    const StaticKey key{field, owner};
    if (SemaGlobal **found = _staticFields.Find(key))
    {
        return *found;
    }
    _globals.Add(MakeUnique<SemaGlobal>());
    SemaGlobal *global = _globals.Last().Get();
    global->Symbol = field;
    global->Owner = owner;
    global->Kind = SemaGlobalKind::Static;
    _staticFields.Add(key, global);
    UseFile(field->File);
    List<SemaGenericBinding> bindings;
    GetBindings(owner, bindings);
    const SemaContext context = ContextOf(field, Freeze(bindings), owner);
    const AstNode *type = field->Kind == SemaSymbolKind::Property ? field->Decl->As<AstPropertyDecl>()->Type
                                                                   : field->Decl->As<AstFieldDecl>()->Type;
    global->Type = ResolveType(const_cast<AstNode *>(type), context);
    return global;
}
} // namespace Marlin
