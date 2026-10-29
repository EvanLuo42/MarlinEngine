#include "Sema/Binder.h"
#include "Sema/SemaBuiltins.h"
#include "Sema/SemaHelpers.h"

namespace Marlin
{
namespace
{
bool IsValueType(const SemaType *type)
{
    return !type->IsReference() && !type->Is(SemaTypeKind::Nullable) && !type->Is(SemaTypeKind::Null) &&
           !type->Is(SemaTypeKind::Void) && !type->IsError();
}

/// The struct or class instance whose members a value of *type* has, or null.
SemaStructInstance *InstanceOf(const SemaType *type)
{
    return type->HasInstance() ? type->GetStruct() : nullptr;
}
} // namespace

bool Binder::IsInsideType(const SemaSymbol *symbol) const
{
    for (const SemaSymbol *type = _context.TypeSymbol; type != nullptr; type = type->Parent)
    {
        if (type == symbol)
        {
            return true;
        }
    }
    for (const SemaStructInstance *base = _context.Owner; base != nullptr; base = base->Base)
    {
        if (base->Symbol == symbol)
        {
            return true;
        }
    }
    return false;
}

BoundExpr *Binder::BindThis(SourceRange range)
{
    SemaFunction *owner = ThisOwner();
    for (SemaFunction *lambda = _function; lambda != nullptr && lambda != owner; lambda = lambda->Enclosing)
    {
        if (lambda->IsStaticLambda)
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, range, "a `static` lambda cannot use `this`");
            return Error(range);
        }
        lambda->CapturesThis = true;
    }
    if (owner == nullptr || owner->This == nullptr)
    {
        _sema->Diagnostics.Error(DiagnosticCode::UndefinedName, range, "`this` is not available here")
            .WithLabel("only instance members have `this`");
        return Error(range);
    }
    if (owner != _function && !owner->This->Type->IsReference())
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, range, "a lambda cannot use `this` of a struct")
            .WithHelp("copy the value into a local first");
        return Error(range);
    }
    auto *local = MakeExpr<BoundLocal>(*_sema, owner->This->Type, range);
    local->Local = owner->This;
    local->IsLValue = !owner->This->Type->IsReference() && !owner->This->IsReadOnly;
    return local;
}

BinderTarget Binder::ResolveBaseMember(const AstMemberExpr *member)
{
    BinderTarget target;
    target.Range = member->Range;
    SemaStructInstance *owner = _function != nullptr ? _function->Owner : nullptr;
    const bool inClass = InScript() && _function != nullptr && _function->This != nullptr && owner != nullptr &&
                         owner->Symbol->Kind == SemaSymbolKind::Class;
    if (!inClass || !_sema->ResolveHierarchy(owner) || owner->Base == nullptr)
    {
        if (!inClass)
        {
            _sema->Diagnostics.Error(DiagnosticCode::UndefinedName, member->Target->Range,
                                     "`base` is only available in instance members of a class");
        }
        return target;
    }
    BoundExpr *self = MakeTypeConversion(BindThis(member->Target->Range), _sema->Types.GetClass(owner->Base),
                                         BoundTypeConversionKind::Upcast, false);
    const bool previous = _baseAccess;
    _baseAccess = true;
    target = ResolveReferenceMember(self, member);
    _baseAccess = previous;
    target.ViaBase = true;
    return target;
}

BinderTarget Binder::ResolveReferenceMember(BoundExpr *object, const AstMemberExpr *member)
{
    BinderTarget target;
    target.Range = member->Range;
    const SemaType *type = object->Type;
    if (type->Is(SemaTypeKind::Nullable) && !type->GetElement()->IsReference())
    {
        bool isMethod = false;
        if (BoundExpr *value = BindNullableValueMember(object, member, isMethod))
        {
            if (isMethod)
            {
                target.TargetKind = BinderTarget::Kind::Functions;
                target.Receiver = value;
                return target;
            }
            target.TargetKind = value->Type->IsError() ? BinderTarget::Kind::Error : BinderTarget::Kind::Value;
            target.Value = value;
            return target;
        }
    }
    if (type->Is(SemaTypeKind::Nullable))
    {
        object = ConvertImplicit(object, type->GetElement());
        type = object->Type;
    }
    SemaStructInstance *owner = InstanceOf(type);
    if (owner == nullptr)
    {
        owner = MagicOwner(type);
    }
    List<SemaSymbol *> members;
    if (owner != nullptr)
    {
        List<SemaSymbol *> all;
        _sema->GetInstanceMembers(owner, member->Name, all);
        for (SemaSymbol *symbol : all)
        {
            if (_sema->IsAccessible(_context, symbol))
            {
                members.Add(symbol);
            }
        }
    }
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
    BinderTarget result = FromSymbols(members.AsSpan(), owner, object, member->Range, member->Name);
    result.GenericArguments = member->GenericArguments;
    result.HasGenericArguments = member->HasGenericArguments;
    return result;
}

BinderTarget Binder::BindFieldSymbol(SemaSymbol *field, SemaStructInstance *owner, BoundExpr *receiver,
                                     SourceRange range)
{
    BinderTarget target;
    target.Range = range;
    const AstDecl *decl = field->Decl;
    owner = _sema->FindDeclaringInstance(owner, field);
    if (HasAnyFlags(decl->Modifiers, AstModifiers::Const))
    {
        if (SemaConstant *value = _sema->GetStaticValue(field, owner))
        {
            target.TargetKind = BinderTarget::Kind::Value;
            target.Value = MakeConstant(*_sema, value, range);
        }
        return target;
    }
    const bool isEvent = HasAnyFlags(decl->Modifiers, AstModifiers::Event);
    if (isEvent && !IsInsideType(field->Parent) && !_eventCompound)
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotAssignable, range,
                                 "the event `{0}` can only be added to and removed from outside `{1}`", field->Name,
                                 field->Parent->Name)
            .WithHelp("use `+=` or `-=`");
        return target;
    }
    if (HasAnyFlags(decl->Modifiers, AstModifiers::Static))
    {
        SemaGlobal *global = _sema->GetStaticField(field, owner);
        auto *bound = MakeExpr<BoundGlobal>(*_sema, global->Type, range);
        bound->Global = global;
        bound->IsLValue = !HasAnyFlags(decl->Modifiers, AstModifiers::Readonly);
        target.TargetKind = global->Type->IsError() ? BinderTarget::Kind::Error : BinderTarget::Kind::Value;
        target.Value = bound;
        return target;
    }
    if (receiver == nullptr || owner == nullptr)
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, range, "`{0}` is an instance field", field->Name)
            .WithLabel("needs an object, such as `value.{0}`");
        return target;
    }
    if (!_sema->ResolveFields(owner))
    {
        return target;
    }
    for (usize i = 0; i < owner->Fields.Count(); ++i)
    {
        if (owner->Fields[i].Symbol != field)
        {
            continue;
        }
        const bool readonly = HasAnyFlags(decl->Modifiers, AstModifiers::Readonly) &&
                              !(_function != nullptr && _function->Symbol->Kind == SemaSymbolKind::Constructor &&
                                _function->Owner == owner);
        if (receiver->Type->IsReference())
        {
            auto *node = MakeExpr<BoundObjectField>(*_sema, owner->Fields[i].Type, range);
            node->Object = receiver;
            node->Instance = owner;
            node->FieldIndex = static_cast<uint32>(i);
            node->IsLValue = !readonly;
            target.Value = node;
        }
        else
        {
            auto *node = MakeExpr<BoundField>(*_sema, owner->Fields[i].Type, range);
            node->Object = receiver;
            node->FieldIndex = static_cast<uint32>(i);
            node->IsLValue = receiver->IsLValue && !readonly;
            target.Value = node;
        }
        target.TargetKind = BinderTarget::Kind::Value;
        return target;
    }
    return target;
}

BoundDispatch Binder::DispatchOf(SemaFunction *function, BoundExpr *receiver)
{
    SemaStructInstance *owner = function->Owner;
    if (_baseAccess || receiver == nullptr || owner == nullptr || !receiver->Type->IsReference())
    {
        return BoundDispatch::Direct;
    }
    if (!_sema->ResolveHierarchy(owner))
    {
        return BoundDispatch::Direct;
    }
    if (owner->Symbol->Kind == SemaSymbolKind::Interface)
    {
        return BoundDispatch::Interface;
    }
    return Sema::IsVirtual(function) ? BoundDispatch::Virtual : BoundDispatch::Direct;
}

BoundExpr *Binder::MakeProperty(SemaSymbol *property, SemaStructInstance *owner, BoundExpr *receiver,
                                Span<BoundExpr *> arguments, SourceRange range)
{
    owner = _sema->FindDeclaringInstance(owner, property);
    const auto *decl = property->Decl->As<AstPropertyDecl>();
    const bool isStatic = HasAnyFlags(decl->Modifiers, AstModifiers::Static);
    if (!decl->HasGetter)
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, decl->NameRange, "`{0}` has no `get` accessor",
                                 property->Name)
            .WithNote("scripts have no write-only properties");
        return Error(range);
    }
    SemaFunction *getter = _sema->InstantiateFunction(property, owner, {}, SemaAccessor::Getter);
    SemaFunction *setter =
        decl->Setter.Present ? _sema->InstantiateFunction(property, owner, {}, SemaAccessor::Setter) : nullptr;
    if (!_sema->ResolveSignature(getter) || (setter != nullptr && !_sema->ResolveSignature(setter)))
    {
        return Error(range);
    }
    if (!isStatic && receiver == nullptr)
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotAValue, range, "`{0}` is an instance property", property->Name)
            .WithHelp("read it from a value: `value.Property`");
        return Error(range);
    }
    if (setter != nullptr && !_sema->IsAccessible(_context, property))
    {
        setter = nullptr;
    }
    if (setter != nullptr && HasAnyFlags(decl->Setter.Modifiers, AstModifiers::Private) &&
        !IsInsideType(property->Parent))
    {
        setter = nullptr;
    }
    auto *node = MakeExpr<BoundProperty>(*_sema, getter->ReturnType, range);
    node->Getter = getter;
    node->Setter = setter;
    node->Arguments = arguments;
    if (!isStatic)
    {
        node->Receiver = receiver->Type->IsReference() ? ConvertImplicit(receiver, getter->This->Type) : receiver;
        node->Dispatch = DispatchOf(getter, receiver);
    }
    NoteCallee(getter, range);
    if (setter != nullptr)
    {
        NoteCallee(setter, range);
    }
    return node;
}

bool Binder::CanAssign(BoundExpr *target)
{
    if (target->Kind != BoundKind::Property)
    {
        return RequireLValue(target, "assign to");
    }
    auto *property = target->As<BoundProperty>();
    if (property->Setter == nullptr)
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotAssignable, target->Range, "`{0}` cannot be assigned to here",
                                 property->Getter->Symbol->Name)
            .WithLabel("it has no `set` accessor this code can use");
        return false;
    }
    const auto *decl = property->Getter->Symbol->Decl->As<AstPropertyDecl>();
    const bool constructing = _initializing || (_function != nullptr &&
                                                _function->Symbol->Kind == SemaSymbolKind::Constructor &&
                                                IsInsideType(property->Getter->Symbol->Parent));
    if (decl->SetterIsInit && !constructing)
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotAssignable, target->Range,
                                 "`{0}` is `init`: it can only be set by a constructor or an object initializer",
                                 property->Getter->Symbol->Name);
        return false;
    }
    return true;
}

BoundExpr *Binder::BindNewObject(const SemaType *type, Span<AstArgument> astArguments, SourceRange range)
{
    SemaStructInstance *owner = type->GetStruct();
    const AstModifiers modifiers = owner->Symbol->Decl->Modifiers;
    if (HasAnyFlags(modifiers, AstModifiers::Abstract | AstModifiers::Static))
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, range, "`{0}` is {1} and cannot be created",
                                 TypeNameOf(type).AsView(),
                                 StringView(HasAnyFlags(modifiers, AstModifiers::Abstract) ? "abstract" : "static"));
        return Error(range);
    }
    if (!_sema->ResolveHierarchy(owner))
    {
        return Error(range);
    }
    List<BinderArgument> arguments;
    if (!BindArguments(astArguments, arguments))
    {
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
    if (target.Functions.IsEmpty())
    {
        _sema->Diagnostics.Error(DiagnosticCode::InaccessibleMember, range, "`{0}` has no constructor usable here",
                                 TypeNameOf(type).AsView());
        return Error(range);
    }
    BoundExpr *call = ResolveCall(target, arguments.AsSpan(), range);
    if (call->Kind != BoundKind::Call)
    {
        return call;
    }
    auto *node = MakeExpr<BoundNewObject>(*_sema, type, range);
    node->Constructor = call->As<BoundCall>()->Function;
    node->Arguments = call->As<BoundCall>()->Arguments;
    return node;
}

BoundExpr *Binder::BindArrayCreation(const AstArrayCreationExpr *node)
{
    if (!InScript())
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "arrays of run-time length exist only in scripts")
            .WithHelp("shaders keep run-time arrays in GPU memory: Span<T>");
        return Error(node->Range);
    }
    const SemaType *element = node->Element != nullptr ? _sema->ResolveType(node->Element, _context) : nullptr;
    if (element != nullptr && element->IsError())
    {
        return Error(node->Range);
    }
    List<BoundExpr *> elements;
    for (AstNode *value : node->Elements)
    {
        elements.Add(BindExpression(value, element));
    }
    if (element == nullptr)
    {
        for (BoundExpr *value : elements)
        {
            if (value->Type->IsError())
            {
                return Error(node->Range);
            }
            if (element == nullptr || (_sema->ClassifyConversion(element, value->Type, nullptr) < SemaConversion::Explicit &&
                                       _sema->ClassifyConversion(value->Type, element, nullptr) >= SemaConversion::Explicit))
            {
                element = value->Type;
            }
        }
        if (element == nullptr || element->Is(SemaTypeKind::Null))
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, node->Range, "the element type cannot be inferred")
                .WithHelp("write it: `new T[] { … }`");
            return Error(node->Range);
        }
    }
    for (BoundExpr *&value : elements)
    {
        value = ConvertImplicit(value, element);
    }
    auto *array = MakeExpr<BoundNewArray>(*_sema, _sema->Types.GetGcArray(element), node->Range);
    if (node->Length != nullptr)
    {
        const SemaType *intType = _sema->Types.GetInt(32, true);
        array->Length = ConvertImplicit(BindExpression(node->Length, intType), intType);
        if (node->HasElements)
        {
            const BoundExpr *length = array->Length;
            if (length->Kind != BoundKind::Constant ||
                static_cast<usize>(length->As<BoundConstant>()->Value->GetInt()) != elements.Count())
            {
                _sema->Diagnostics.Error(DiagnosticCode::WrongArgumentCount, node->Length->Range,
                                         "the length must be a constant equal to the number of elements, {0}",
                                         elements.Count());
                return Error(node->Range);
            }
            array->Length = nullptr;
        }
    }
    array->Elements = _sema->Freeze(elements);
    return array;
}

BoundExpr *Binder::BindIndexer(BoundExpr *object, BoundExpr *index, SourceRange range, bool viaBase)
{
    const SemaType *type = object->Type;
    if (type->Is(SemaTypeKind::Nullable) && type->GetElement()->IsReference())
    {
        object = ConvertImplicit(object, type->GetElement());
        type = object->Type;
    }
    if (type->Is(SemaTypeKind::GcArray))
    {
        const SemaType *intType = _sema->Types.GetInt(32, true);
        auto *element = MakeExpr<BoundArrayElement>(*_sema, type->GetElement(), range);
        element->Array = object;
        element->Index = ConvertImplicit(index, intType);
        element->IsLValue = true;
        return element;
    }
    SemaStructInstance *owner = InstanceOf(type);
    if (owner == nullptr)
    {
        owner = MagicOwner(type);
    }
    List<SemaSymbol *> indexers;
    if (owner != nullptr)
    {
        _sema->GetInstanceMembers(owner, "this", indexers);
    }
    if (indexers.IsEmpty())
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, range, "`{0}` cannot be indexed",
                                 TypeNameOf(type).AsView())
            .WithSecondaryLabel(object->Range, TypeNameOf(type).AsView());
        return Error(range);
    }
    BinderTarget target;
    target.TargetKind = BinderTarget::Kind::Functions;
    target.Range = range;
    target.Owner = owner;
    target.Receiver = object;
    target.Functions = Move(indexers);
    target.ViaBase = viaBase;
    BinderArgument argument;
    argument.Value = index;
    argument.Range = index->Range;
    argument.Constant = index->Kind == BoundKind::Constant ? index->As<BoundConstant>()->Value : nullptr;
    BoundExpr *call = ResolveCall(target, Span<BinderArgument>(&argument, 1), range);
    if (call->Kind != BoundKind::Call)
    {
        return call;
    }
    auto *getterCall = call->As<BoundCall>();
    SemaFunction *getter = getterCall->Function;
    List<BoundExpr *> indices;
    indices.AddRange(getterCall->Arguments.Slice(1));
    const bool previous = _baseAccess;
    _baseAccess = viaBase;
    BoundExpr *property =
        MakeProperty(getter->Symbol, getter->Owner, getterCall->Arguments[0], _sema->Freeze(indices), range);
    _baseAccess = previous;
    return property;
}

BoundExpr *Binder::BindCollectionInitializer(BoundExpr *initial, Span<AstNode *> elements, SourceRange range)
{
    SemaLocal *temporary = _sema->New<SemaLocal>();
    temporary->Name = "collection";
    temporary->Type = initial->Type;
    temporary->Range = range;
    List<BoundExpr *> effects;
    for (AstNode *element : elements)
    {
        auto *collection = MakeExpr<BoundLocal>(*_sema, initial->Type, element->Range);
        collection->Local = temporary;
        BoundExpr *arguments[] = {BindExpression(element)};
        if (BoundExpr *call = CallMethod(collection, "Add", Span<BoundExpr *>(arguments), element->Range))
        {
            effects.Add(call);
        }
    }
    auto *sequence = MakeExpr<BoundSequence>(*_sema, initial->Type, range);
    sequence->Temporary = temporary;
    sequence->Initial = initial;
    sequence->Effects = _sema->Freeze(effects);
    return sequence;
}

BoundExpr *Binder::CallMethod(BoundExpr *receiver, StringView name, Span<BoundExpr *> arguments, SourceRange range)
{
    AstMemberExpr member;
    member.Kind = AstKind::MemberExpr;
    member.Range = range;
    member.NameRange = range;
    member.Name = name;
    BinderTarget target = receiver->Type->IsReference() || receiver->Type->Is(SemaTypeKind::Nullable)
                              ? ResolveReferenceMember(receiver, &member)
                              : ResolveInstanceMember(receiver, &member);
    if (target.TargetKind != BinderTarget::Kind::Functions)
    {
        if (target.TargetKind != BinderTarget::Kind::Error)
        {
            _sema->Diagnostics.Error(DiagnosticCode::NotAValue, range, "`{0}` is not a method of `{1}`", name,
                                     TypeNameOf(receiver->Type).AsView());
        }
        return nullptr;
    }
    List<BinderArgument> bound;
    for (BoundExpr *argument : arguments)
    {
        BinderArgument entry;
        entry.Value = argument;
        entry.Range = argument->Range;
        entry.Constant = argument->Kind == BoundKind::Constant ? argument->As<BoundConstant>()->Value : nullptr;
        if (argument->Type->IsError())
        {
            return nullptr;
        }
        bound.Add(entry);
    }
    BoundExpr *call = ResolveCall(target, bound.AsSpan(), range);
    return call->Type->IsError() ? nullptr : call;
}

BoundExpr *Binder::CallToString(BoundExpr *value)
{
    const SemaType *type = value->Type;
    if (type->Is(SemaTypeKind::String) || type->IsError())
    {
        return value;
    }
    if (type->Is(SemaTypeKind::Null))
    {
        _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, value->Range, "`null` has no text")
            .WithHelp("write the empty string `""` instead");
        return Error(value->Range);
    }
    if (type->Is(SemaTypeKind::Enum) || (IsValueType(type) && type->HasInstance() == false && !type->IsScalar()))
    {
        value = MakeTypeConversion(value, _sema->Types.GetClass(_sema->GetObjectClass()), BoundTypeConversionKind::Box,
                                   false);
    }
    BoundExpr *call = CallMethod(value, "ToString", {}, value->Range);
    return call != nullptr ? call : Error(value->Range);
}

BoundExpr *Binder::MakeTypeConversion(BoundExpr *operand, const SemaType *type, BoundTypeConversionKind kind,
                                      bool isExplicit)
{
    auto *node = MakeExpr<BoundTypeConversion>(*_sema, type, operand->Range);
    node->Operand = operand;
    node->Conversion = kind;
    node->Explicit = isExplicit;
    return node;
}

BoundExpr *Binder::ConvertReference(BoundExpr *expression, const SemaType *type, bool isExplicit, bool &handled)
{
    const SemaType *from = expression->Type;
    const bool involved = from->IsReference() || type->IsReference() || from->Is(SemaTypeKind::Nullable) ||
                          type->Is(SemaTypeKind::Nullable) ||
                          (from->Is(SemaTypeKind::Null) && (type->IsReference() || type->Is(SemaTypeKind::Nullable)));
    handled = involved;
    if (!involved)
    {
        return expression;
    }
    auto fail = [&]() -> BoundExpr * {
        const bool explicitWouldDo = !isExplicit && from->IsReference() && type->IsReference();
        LanguageDiagnostic &error = _sema->Diagnostics.Error(DiagnosticCode::NoConversion, expression->Range,
                                                             "cannot convert `{0}` to `{1}`", TypeNameOf(from).AsView(),
                                                             TypeNameOf(type).AsView());
        error.WithLabel("this expression");
        if (explicitWouldDo)
        {
            error.WithHelp("cast it if the object is known to be one: `(T)x`, or test it: `x as T`, `x is T t`");
        }
        return Error(expression->Range);
    };

    if (from->Is(SemaTypeKind::Null))
    {
        if (type->IsReference() && !type->Is(SemaTypeKind::Nullable))
        {
            _sema->Diagnostics.Error(DiagnosticCode::NoConversion, expression->Range,
                                     "`null` is not a `{0}`", TypeNameOf(type).AsView())
                .WithHelp(Format("declare the type `{0}?` to allow null (MarlinScript §3.6)", TypeNameOf(type).AsView())
                              .AsView());
            return Error(expression->Range);
        }
        return MakeExpr<BoundZero>(*_sema, type, expression->Range);
    }
    if (from->Is(SemaTypeKind::Nullable))
    {
        const SemaType *inner = from->GetElement();
        if (type->Is(SemaTypeKind::Nullable))
        {
            if (inner->IsReference() && _sema->IsImplicitReference(inner, type->GetElement()))
            {
                return MakeTypeConversion(expression, type, BoundTypeConversionKind::Upcast, isExplicit);
            }
            return fail();
        }
        BoundExpr *removed = MakeTypeConversion(expression, inner, BoundTypeConversionKind::RemoveNullable, isExplicit);
        return inner == type ? removed : Convert(removed, type, isExplicit);
    }
    if (type->Is(SemaTypeKind::Nullable))
    {
        const SemaType *inner = type->GetElement();
        BoundExpr *converted = Convert(expression, inner, isExplicit);
        if (converted->Type->IsError())
        {
            return converted;
        }
        return MakeTypeConversion(converted, type,
                                  inner->IsReference() ? BoundTypeConversionKind::Upcast
                                                       : BoundTypeConversionKind::WrapNullable,
                                  isExplicit);
    }
    if (from->IsReference() && type->IsReference())
    {
        if (_sema->IsImplicitReference(from, type))
        {
            return MakeTypeConversion(expression, type, BoundTypeConversionKind::Upcast, isExplicit);
        }
        const bool interfaces = from->Is(SemaTypeKind::Interface) || type->Is(SemaTypeKind::Interface);
        if (isExplicit && (_sema->IsImplicitReference(type, from) || interfaces))
        {
            return MakeTypeConversion(expression, type, BoundTypeConversionKind::Downcast, true);
        }
        return fail();
    }
    if (type->IsReference())
    {
        const bool toObject = type->Is(SemaTypeKind::Class) && type->GetStruct() == _sema->GetObjectClass();
        const bool toInterface = type->Is(SemaTypeKind::Interface) && type->GetStruct() != nullptr &&
                                 _sema->ImplementsInterface(from, type->GetStruct());
        if (IsValueType(from) && (toObject || toInterface))
        {
            return MakeTypeConversion(expression, type, BoundTypeConversionKind::Box, isExplicit);
        }
        return fail();
    }
    const bool fromBox = (from->Is(SemaTypeKind::Class) && from->GetStruct() == _sema->GetObjectClass()) ||
                         from->Is(SemaTypeKind::Interface);
    if (isExplicit && fromBox && IsValueType(type))
    {
        return MakeTypeConversion(expression, type, BoundTypeConversionKind::Unbox, true);
    }
    return fail();
}

BoundExpr *Binder::BindReferenceOperator(TokenKind op, BoundExpr *left, BoundExpr *right, SourceRange range,
                                         bool &handled)
{
    const SemaType *leftType = left->Type;
    const SemaType *rightType = right->Type;
    handled = false;
    const bool leftString = leftType->Is(SemaTypeKind::String);
    const bool rightString = rightType->Is(SemaTypeKind::String);
    if (op == TokenKind::Plus && (leftString || rightString))
    {
        handled = true;
        BoundExpr *a = CallToString(left);
        BoundExpr *b = CallToString(right);
        if (a->Type->IsError() || b->Type->IsError())
        {
            return Error(range);
        }
        SemaStructInstance *owner = MagicOwner(_sema->Types.GetString());
        for (SemaSymbol *symbol : owner != nullptr ? _sema->GetMembers(owner->Symbol, "Concat") : Span<SemaSymbol *const>())
        {
            SemaFunction *concat = _sema->InstantiateFunction(symbol, owner, {});
            if (_sema->ResolveSignature(concat) && concat->Parameters.Count() == 2)
            {
                BoundExpr *arguments[] = {a, b};
                return MakeCall(concat, nullptr, _sema->Freeze(List<BoundExpr *>(Span<BoundExpr *const>(arguments))),
                                range);
            }
        }
        _sema->Diagnostics.Error(DiagnosticCode::UndefinedName, range, "the core library lacks `string.Concat`");
        return Error(range);
    }
    if ((op == TokenKind::Plus || op == TokenKind::Minus) &&
        (leftType->Is(SemaTypeKind::Delegate) || rightType->Is(SemaTypeKind::Delegate)))
    {
        handled = true;
        const SemaType *type = leftType->Is(SemaTypeKind::Delegate) ? leftType : rightType;
        auto *node = MakeExpr<BoundCombineDelegates>(*_sema, type, range);
        node->Left = ConvertImplicit(left, type);
        node->Right = ConvertImplicit(right, type);
        node->Remove = op == TokenKind::Minus;
        return node->Left->Type->IsError() || node->Right->Type->IsError() ? Error(range) : node;
    }
    if (op != TokenKind::Equal && op != TokenKind::NotEqual)
    {
        return nullptr;
    }
    const bool leftReference = leftType->IsReference() || leftType->Is(SemaTypeKind::Nullable);
    const bool rightReference = rightType->IsReference() || rightType->Is(SemaTypeKind::Nullable);
    const bool leftNull = leftType->Is(SemaTypeKind::Null);
    const bool rightNull = rightType->Is(SemaTypeKind::Null);
    if (!(leftReference || rightReference) && !(leftNull && rightNull))
    {
        return nullptr;
    }
    handled = true;
    auto compare = [&](BoundExpr *a, BoundExpr *b) -> BoundExpr * {
        auto *node = MakeExpr<BoundBinary>(*_sema, _sema->Types.GetBool(), range);
        node->Op = op == TokenKind::Equal ? MirOp::Equal : MirOp::NotEqual;
        node->Left = a;
        node->Right = b;
        return node;
    };
    if (leftNull || rightNull)
    {
        BoundExpr *other = leftNull ? right : left;
        if (rightNull && leftNull)
        {
            return MakeConstant(*_sema, _sema->NewBool(op == TokenKind::Equal), range);
        }
        if (!other->Type->IsReference() && !other->Type->Is(SemaTypeKind::Nullable))
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, range, "`{0}` is never null",
                                     TypeNameOf(other->Type).AsView())
                .WithSecondaryLabel(other->Range, "a value type");
            return Error(range);
        }
        return compare(other, MakeExpr<BoundZero>(*_sema, _sema->Types.GetNullable(other->Type), range));
    }
    if (leftType->HasInstance() && leftType->Is(SemaTypeKind::Class))
    {
        bool found = false;
        BoundExpr *operands[] = {left, right};
        BoundExpr *user = BindUserOperator(op, Span<BoundExpr *>(operands), range, found);
        if (found)
        {
            return user;
        }
    }
    const SemaType *a = leftType->Is(SemaTypeKind::Nullable) ? leftType->GetElement() : leftType;
    const SemaType *b = rightType->Is(SemaTypeKind::Nullable) ? rightType->GetElement() : rightType;
    if (a->Is(SemaTypeKind::String) && b->Is(SemaTypeKind::String))
    {
        return compare(left, right);
    }
    if (a->IsReference() && b->IsReference() &&
        (_sema->IsImplicitReference(a, b) || _sema->IsImplicitReference(b, a) || a->Is(SemaTypeKind::Interface) ||
         b->Is(SemaTypeKind::Interface)))
    {
        return compare(left, right);
    }
    if (!a->IsReference() && a == b && (a->IsScalar() || a->Is(SemaTypeKind::Enum)))
    {
        return compare(left, right);
    }
    _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, range, "cannot compare `{0}` and `{1}`",
                             TypeNameOf(leftType).AsView(), TypeNameOf(rightType).AsView())
        .WithNote("references compare by identity, so the types must be related");
    return Error(range);
}

BoundExpr *Binder::BindAs(const AstAsExpr *node)
{
    BoundExpr *operand = BindExpression(node->Operand);
    const SemaType *type = _sema->ResolveType(node->Type, _context);
    if (operand->Type->IsError() || type->IsError())
    {
        return Error(node->Range);
    }
    if (!InScript())
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "`as` exists only in scripts");
        return Error(node->Range);
    }
    const SemaType *target = type->Is(SemaTypeKind::Nullable) ? type->GetElement() : type;
    if (!target->IsReference())
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, node->Type->Range,
                                 "`as` converts to reference types, not `{0}`", TypeNameOf(type).AsView())
            .WithHelp("test a value type with a pattern: `x is int n`");
        return Error(node->Range);
    }
    const SemaType *operandType =
        operand->Type->Is(SemaTypeKind::Nullable) ? operand->Type->GetElement() : operand->Type;
    if (!operandType->IsReference())
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, node->Operand->Range, "`{0}` is not a reference",
                                 TypeNameOf(operand->Type).AsView());
        return Error(node->Range);
    }
    const SemaType *result = _sema->Types.GetNullable(target);
    if (_sema->IsImplicitReference(operandType, target))
    {
        return MakeTypeConversion(operand, result, BoundTypeConversionKind::Upcast, true);
    }
    return MakeTypeConversion(operand, result, BoundTypeConversionKind::TryDowncast, true);
}

BoundExpr *Binder::BindTypeof(const AstTypeofExpr *node)
{
    if (!InScript())
    {
        _sema->Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "`typeof` exists only in scripts");
        return Error(node->Range);
    }
    const SemaType *type = _sema->ResolveType(node->Type, _context);
    SemaSymbol *typeInfo = _sema->FindQualified("Marlin.TypeInfo");
    if (type->IsError() || typeInfo == nullptr || typeInfo->Kind != SemaSymbolKind::Class)
    {
        if (!type->IsError())
        {
            _sema->Diagnostics.Error(DiagnosticCode::UndefinedName, node->Range, "the core library lacks `TypeInfo`");
        }
        return Error(node->Range);
    }
    auto *bound = MakeExpr<BoundTypeOf>(*_sema, _sema->Types.GetClass(_sema->InstantiateStruct(typeInfo, {})), node->Range);
    bound->Described = type;
    return bound;
}

BoundExpr *Binder::BindNullForgiving(const AstNullForgivingExpr *node)
{
    BoundExpr *operand = BindExpression(node->Operand);
    if (operand->Type->Is(SemaTypeKind::Null))
    {
        return MakeTypeConversion(operand, operand->Type, BoundTypeConversionKind::RemoveNullable, true);
    }
    if (!operand->Type->Is(SemaTypeKind::Nullable))
    {
        return operand;
    }
    return MakeTypeConversion(operand, operand->Type->GetElement(), BoundTypeConversionKind::RemoveNullable, true);
}

void Binder::BindConstructorPrologue(List<BoundStmt *> &statements)
{
    const auto *decl = _function->Symbol->Decl->As<AstConstructorDecl>();
    SemaStructInstance *owner = _function->Owner;
    const SourceRange range = decl->NameRange;
    auto self = [&]() { return BindThis(range); };
    const bool chainsToThis = decl->HasInitializer && !decl->InitializerIsBase;
    if (!chainsToThis && _sema->ResolveFields(owner))
    {
        List<SemaGenericBinding> bindings;
        _sema->GetBindings(owner, bindings);
        for (usize i = 0; i < owner->Fields.Count(); ++i)
        {
            const SemaField &field = owner->Fields[i];
            AstNode *initializer = field.Symbol->Kind == SemaSymbolKind::Property
                                       ? field.Symbol->Decl->As<AstPropertyDecl>()->Initializer
                                       : field.Symbol->Decl->As<AstFieldDecl>()->Initializer;
            if (initializer == nullptr)
            {
                continue;
            }
            Binder fieldBinder(*_sema, nullptr, _sema->ContextOf(field.Symbol, _sema->Freeze(bindings), owner));
            BoundExpr *value = fieldBinder.ConvertImplicit(fieldBinder.BindExpression(initializer, field.Type), field.Type);
            auto *target = MakeExpr<BoundObjectField>(*_sema, field.Type, initializer->Range);
            target->Object = self();
            target->Instance = owner;
            target->FieldIndex = static_cast<uint32>(i);
            target->IsLValue = true;
            auto *assign = MakeExpr<BoundAssign>(*_sema, field.Type, initializer->Range);
            assign->Target = target;
            assign->Value = value;
            auto *statement = MakeStmt<BoundExprStmt>(*_sema, initializer->Range);
            statement->Expression = assign;
            statements.Add(statement);
        }
    }
    SemaStructInstance *callee = chainsToThis ? owner : owner->Base;
    if (callee == nullptr)
    {
        return;
    }
    List<BinderArgument> arguments;
    if (decl->HasInitializer && !BindArguments(decl->InitializerArguments, arguments))
    {
        return;
    }
    BinderTarget target;
    target.TargetKind = BinderTarget::Kind::Functions;
    target.Range = decl->HasInitializer ? decl->InitializerRange : range;
    target.Owner = callee;
    for (SemaSymbol *member : _sema->GetMembers(callee->Symbol, callee->Symbol->Name))
    {
        if (member->Kind == SemaSymbolKind::Constructor && member != _function->Symbol &&
            _sema->IsAccessible(_context, member))
        {
            target.Functions.Add(member);
        }
    }
    if (target.Functions.IsEmpty())
    {
        _sema->Diagnostics.Error(DiagnosticCode::NoMatchingOverload, target.Range,
                                 "`{0}` has no constructor this one can call", callee->Symbol->Name);
        return;
    }
    BoundExpr *call = ResolveCall(target, arguments.AsSpan(), target.Range);
    if (call->Kind != BoundKind::Call)
    {
        if (!decl->HasInitializer)
        {
            _sema->Diagnostics.Error(DiagnosticCode::NoMatchingOverload, range,
                                     "the base class `{0}` has no constructor without parameters",
                                     callee->Symbol->Name)
                .WithHelp("call one of its constructors: `: base(…)`");
        }
        return;
    }
    auto *constructorCall = call->As<BoundCall>();
    List<BoundExpr *> all;
    all.Add(MakeTypeConversion(self(), _sema->Types.GetClass(callee), BoundTypeConversionKind::Upcast, false));
    all.AddRange(constructorCall->Arguments);
    constructorCall->Arguments = _sema->Freeze(all);
    auto *statement = MakeStmt<BoundExprStmt>(*_sema, target.Range);
    statement->Expression = constructorCall;
    statements.Add(statement);
}

BoundStmt *Binder::BindAutoAccessor()
{
    SemaSymbol *property = _function->Symbol;
    SemaStructInstance *owner = _function->Owner;
    const SourceRange range = property->Decl->NameRange;
    BoundExpr *storage = nullptr;
    if (property->IsStatic())
    {
        SemaGlobal *global = _sema->GetStaticField(property, owner);
        auto *bound = MakeExpr<BoundGlobal>(*_sema, global->Type, range);
        bound->Global = global;
        bound->IsLValue = true;
        storage = bound;
    }
    else if (_sema->ResolveFields(owner))
    {
        for (usize i = 0; i < owner->Fields.Count(); ++i)
        {
            if (owner->Fields[i].Symbol != property)
            {
                continue;
            }
            BoundExpr *self = BindThis(range);
            if (self->Type->IsReference())
            {
                auto *field = MakeExpr<BoundObjectField>(*_sema, owner->Fields[i].Type, range);
                field->Object = self;
                field->Instance = owner;
                field->FieldIndex = static_cast<uint32>(i);
                field->IsLValue = true;
                storage = field;
            }
            else
            {
                auto *field = MakeExpr<BoundField>(*_sema, owner->Fields[i].Type, range);
                field->Object = self;
                field->FieldIndex = static_cast<uint32>(i);
                field->IsLValue = true;
                storage = field;
            }
        }
    }
    if (storage == nullptr)
    {
        return nullptr;
    }
    auto *block = MakeStmt<BoundBlock>(*_sema, range);
    if (_function->Accessor == SemaAccessor::Setter)
    {
        auto *value = MakeExpr<BoundLocal>(*_sema, _function->Parameters.Last()->Type, range);
        value->Local = _function->Parameters.Last();
        auto *assign = MakeExpr<BoundAssign>(*_sema, storage->Type, range);
        assign->Target = storage;
        assign->Value = value;
        auto *statement = MakeStmt<BoundExprStmt>(*_sema, range);
        statement->Expression = assign;
        BoundStmt *statements[] = {statement};
        block->Statements = _sema->Freeze(List<BoundStmt *>(Span<BoundStmt *const>(statements)));
        return block;
    }
    auto *statement = MakeStmt<BoundReturn>(*_sema, range);
    statement->Value = storage;
    BoundStmt *statements[] = {statement};
    block->Statements = _sema->Freeze(List<BoundStmt *>(Span<BoundStmt *const>(statements)));
    return block;
}

BoundExpr *Binder::MakeNotNull(BoundExpr *value, SourceRange range)
{
    bool handled = false;
    BoundExpr *test = BindReferenceOperator(TokenKind::NotEqual, value,
                                           MakeExpr<BoundZero>(*_sema, _sema->Types.GetNull(), range), range, handled);
    return handled ? test : Error(range);
}

BoundExpr *Binder::BindNullableValueMember(BoundExpr *object, const AstMemberExpr *member, bool &isMethod)
{
    const SemaType *inner = object->Type->GetElement();
    isMethod = false;
    if (member->Name == "HasValue")
    {
        return MakeNotNull(object, member->Range);
    }
    if (member->Name == "Value")
    {
        return MakeTypeConversion(object, inner, BoundTypeConversionKind::RemoveNullable, true);
    }
    if (member->Name == "GetValueOrDefault")
    {
        SemaLocal *temporary = nullptr;
        BoundExpr *input = StoreInput(object, temporary);
        auto *conditional = MakeExpr<BoundConditional>(*_sema, inner, member->Range);
        conditional->Condition = MakeNotNull(input, member->Range);
        conditional->WhenTrue = MakeTypeConversion(input, inner, BoundTypeConversionKind::RemoveNullable, false);
        conditional->WhenFalse = MakeExpr<BoundZero>(*_sema, inner, member->Range);
        isMethod = true;
        return temporary != nullptr ? MakeLet(temporary, object, conditional) : conditional;
    }
    return nullptr;
}

BoundExpr *Binder::BindConditionalAccess(const AstConditionalAccessExpr *node)
{
    BoundExpr *target = BindExpression(node->Target);
    if (target->Type->IsError())
    {
        return target;
    }
    if (!target->Type->Is(SemaTypeKind::Nullable) && !target->Type->IsReference())
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, node->Target->Range, "`{0}` is never null",
                                 TypeNameOf(target->Type).AsView())
            .WithHelp("use `.` instead of `?.`");
        return Error(node->Range);
    }
    SemaLocal *temporary = nullptr;
    BoundExpr *input = StoreInput(target, temporary);
    const SemaType *inner = target->Type->Is(SemaTypeKind::Nullable) ? target->Type->GetElement() : target->Type;
    BoundExpr *receiver = target->Type->Is(SemaTypeKind::Nullable)
                              ? MakeTypeConversion(input, inner, BoundTypeConversionKind::RemoveNullable, false)
                              : input;
    _conditionalReceivers.Add(receiver);
    BoundExpr *access = BindExpression(node->Access);
    _conditionalReceivers.Pop();
    if (access->Type->IsError())
    {
        return access;
    }
    const SemaType *type = access->Type->Is(SemaTypeKind::Void) ? access->Type : _sema->Types.GetNullable(access->Type);
    auto *conditional = MakeExpr<BoundConditional>(*_sema, type, node->Range);
    conditional->Condition = MakeNotNull(input, node->Range);
    conditional->WhenTrue = type->Is(SemaTypeKind::Void) ? access : ConvertImplicit(access, type);
    conditional->WhenFalse = MakeExpr<BoundZero>(*_sema, type, node->Range);
    return temporary != nullptr ? MakeLet(temporary, target, conditional) : conditional;
}

BoundExpr *Binder::BindCoalesce(BoundExpr *left, AstNode *right, SourceRange range)
{
    if (left->Type->IsError())
    {
        BindExpression(right);
        return left;
    }
    if (!left->Type->Is(SemaTypeKind::Nullable) && !left->Type->IsReference())
    {
        _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, left->Range, "`{0}` is never null",
                                 TypeNameOf(left->Type).AsView())
            .WithHelp("`??` takes a value that may be null on its left");
        BindExpression(right);
        return Error(range);
    }
    SemaLocal *temporary = nullptr;
    BoundExpr *input = StoreInput(left, temporary);
    const SemaType *inner = left->Type->Is(SemaTypeKind::Nullable) ? left->Type->GetElement() : left->Type;
    BoundExpr *fallback = BindExpression(right, inner);
    if (fallback->Type->IsError())
    {
        return fallback;
    }
    const bool fallbackMayBeNull = fallback->Type->Is(SemaTypeKind::Nullable) || fallback->Type->Is(SemaTypeKind::Null);
    const SemaType *type = fallbackMayBeNull ? _sema->Types.GetNullable(inner) : inner;
    auto *conditional = MakeExpr<BoundConditional>(*_sema, type, range);
    conditional->Condition = MakeNotNull(input, range);
    BoundExpr *present = left->Type->Is(SemaTypeKind::Nullable)
                             ? MakeTypeConversion(input, inner, BoundTypeConversionKind::RemoveNullable, false)
                             : input;
    conditional->WhenTrue = ConvertImplicit(present, type);
    conditional->WhenFalse = ConvertImplicit(fallback, type);
    return temporary != nullptr ? MakeLet(temporary, left, conditional) : conditional;
}

BoundExpr *Binder::BindInterpolatedString(const AstInterpolatedStringExpr *node)
{
    SemaSymbol *builderSymbol = _sema->FindQualified("Marlin.StringBuilder");
    if (builderSymbol == nullptr || builderSymbol->Kind != SemaSymbolKind::Class)
    {
        _sema->Diagnostics.Error(DiagnosticCode::UndefinedName, node->Range, "the core library lacks `StringBuilder`");
        return Error(node->Range);
    }
    const SemaType *builderType = _sema->Types.GetClass(_sema->InstantiateStruct(builderSymbol, {}));
    BoundExpr *initial = BindNewObject(builderType, {}, node->Range);
    if (initial->Type->IsError())
    {
        return initial;
    }
    SemaLocal *builder = _sema->New<SemaLocal>();
    builder->Name = "builder";
    builder->Type = builderType;
    builder->Range = node->Range;
    List<BoundExpr *> effects;
    for (const AstInterpolationPart &part : node->Parts)
    {
        auto *receiver = MakeExpr<BoundLocal>(*_sema, builderType, part.Range);
        receiver->Local = builder;
        List<BoundExpr *> arguments;
        if (part.Expression == nullptr)
        {
            arguments.Add(MakeConstant(*_sema, _sema->NewString(part.Text), part.Range));
        }
        else
        {
            BoundExpr *value = BindExpression(part.Expression);
            if (value->Type->IsError())
            {
                continue;
            }
            const SemaType *scalar = value->Type->IsScalar() ? value->Type : nullptr;
            const bool appendable = scalar != nullptr && (scalar->Is(SemaTypeKind::Bool) ||
                                                          scalar->GetWidth() == 32 || scalar->GetWidth() == 64);
            if (!part.Format.IsEmpty() && (scalar == nullptr || !scalar->IsNumeric()))
            {
                _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, part.Range,
                                         "a format applies to numbers, not `{0}`", TypeNameOf(value->Type).AsView());
                continue;
            }
            arguments.Add(appendable ? value : CallToString(value));
            if (!part.Format.IsEmpty())
            {
                arguments.Add(MakeConstant(*_sema, _sema->NewString(part.Format), part.Range));
            }
        }
        if (BoundExpr *append = CallMethod(receiver, "Append", arguments.AsSpan(), part.Range))
        {
            effects.Add(append);
        }
    }
    auto *sequence = MakeExpr<BoundSequence>(*_sema, builderType, node->Range);
    sequence->Temporary = builder;
    sequence->Initial = initial;
    sequence->Effects = _sema->Freeze(effects);
    BoundExpr *text = CallMethod(sequence, "ToString", {}, node->Range);
    return text != nullptr ? text : Error(node->Range);
}
} // namespace Marlin
