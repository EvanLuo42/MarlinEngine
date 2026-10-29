#include "Sema/Lowering.h"

#include "Sema/SemaHelpers.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
SemaParameterMode ModeOf(const SemaLocal *local)
{
    return local->Mode;
}

bool ByReference(SemaParameterMode mode)
{
    return mode == SemaParameterMode::Ref || mode == SemaParameterMode::Out;
}
} // namespace

const MirType *Lowering::LowerScriptType(const SemaType *type)
{
    MirTypeContext &types = _module->GetTypes();
    StringBuilder<128> name;
    if (type->HasInstance())
    {
        AppendQualifiedName(*_sema, *type->GetStruct(), name);
    }
    else
    {
        type->Print(name);
    }
    switch (type->GetKind())
    {
    case SemaTypeKind::Class:
    case SemaTypeKind::Interface:
    case SemaTypeKind::Delegate:
        return types.GetReference(types.CreateStruct(_module->Intern(name.AsView()), {}));
    case SemaTypeKind::String:
        return types.GetReference(types.CreateStruct("string", {}));
    case SemaTypeKind::GcArray:
        return types.GetReference(types.GetRuntimeArray(LowerType(type->GetElement())));
    case SemaTypeKind::Nullable:
    {
        if (type->GetElement()->IsReference())
        {
            return LowerType(type->GetElement());
        }
        const MirStructField fields[] = {{"Value", LowerType(type->GetElement()), MirTypeContext::AutoOffset},
                                         {"HasValue", types.GetBool(), MirTypeContext::AutoOffset}};
        return types.CreateStruct(_module->Intern(name.AsView()), fields);
    }
    default:
        return types.GetVoid();
    }
}

uint32 Lowering::BaseFieldCount(const SemaStructInstance *instance)
{
    if (instance->Base == nullptr)
    {
        return 0;
    }
    _sema->ResolveFields(instance->Base);
    return static_cast<uint32>(instance->Base->Fields.Count());
}

const MirType *Lowering::ClassLayout(SemaStructInstance *instance)
{
    if (const MirType *const *found = _classLayouts.Find(instance))
    {
        return *found;
    }
    _sema->ResolveFields(instance);
    // The fields of a class start with those of its base, at the same indices (MarlinScript §3.4).
    MARLIN_ASSERT(BaseFieldCount(instance) <= instance->Fields.Count());
    List<MirStructField> fields;
    for (const SemaField &field : instance->Fields)
    {
        fields.Add({field.Name, LowerType(field.Type), MirTypeContext::AutoOffset});
    }
    StringBuilder<128> name;
    AppendQualifiedName(*_sema, *instance, name);
    const MirType *layout = _module->GetTypes().CreateStruct(name.AsView(), fields.AsSpan());
    _classLayouts.Add(instance, layout);
    return layout;
}

uint32 Lowering::ScriptTypeOf(const SemaType *type)
{
    if (type->Is(SemaTypeKind::Nullable))
    {
        type = type->GetElement();
    }
    if (type->Is(SemaTypeKind::Class) && type->GetStruct()->Symbol->BuiltinId != 0)
    {
        // `string` and `Array<T>` are declared as classes; values have their magic types.
        const SemaType *magic = _sema->GetOwnerType(type->GetStruct());
        if (magic != type && !magic->IsError())
        {
            return ScriptTypeOf(magic);
        }
    }
    if (const uint32 *found = _scriptTypes.Find(type))
    {
        return *found;
    }
    MirScriptType *entry = _module->NewScriptType();
    StringBuilder<128> name;
    if (type->HasInstance())
    {
        AppendQualifiedName(*_sema, *type->GetStruct(), name);
    }
    else
    {
        type->Print(name);
    }
    entry->Name = _module->Intern(name.AsView());
    // Registered before its members are lowered: they may name the type itself.
    const uint32 index = _module->AddScriptType(entry);
    _scriptTypes.Add(type, index);
    SemaStructInstance *instance = type->HasInstance() ? type->GetStruct() : nullptr;
    auto isImported = [&](const SemaStructInstance *declared) {
        return _package != 0 && declared->Symbol->Package != _package && declared->Arguments.IsEmpty();
    };
    switch (type->GetKind())
    {
    case SemaTypeKind::Class:
    {
        entry->Kind = MirScriptTypeKind::Class;
        entry->Reference = LowerType(type);
        _sema->ResolveHierarchy(instance);
        const AstModifiers modifiers = instance->Symbol->Decl->Modifiers;
        entry->Abstract = HasAnyFlags(modifiers, AstModifiers::Abstract | AstModifiers::Static);
        entry->Sealed = HasAnyFlags(modifiers, AstModifiers::Sealed | AstModifiers::Static);
        entry->Imported = isImported(instance);
        if (instance->Base != nullptr)
        {
            entry->Base = ScriptTypeOf(_sema->Types.GetClass(instance->Base));
        }
        entry->Layout = ClassLayout(instance);
        for (SemaFunction *slot : instance->VirtualSlots)
        {
            if (slot != nullptr)
            {
                entry->VirtualSlots.Add(LowerFunction(slot));
            }
        }
        // Implementations are made in place: their arena lists must keep the module's arena.
        entry->Interfaces.Reserve(instance->InterfaceMaps.Count());
        for (const SemaInterfaceMap &map : instance->InterfaceMaps)
        {
            const uint32 interfaceType = ScriptTypeOf(_sema->GetInstanceType(map.Interface));
            MirInterfaceImpl &implementation = entry->Interfaces.Emplace(_module->GetArena());
            implementation.Interface = interfaceType;
            for (SemaFunction *method : map.Methods)
            {
                if (method != nullptr)
                {
                    implementation.Methods.Add(LowerFunction(method));
                }
            }
        }
        break;
    }
    case SemaTypeKind::Interface:
    {
        entry->Kind = MirScriptTypeKind::Interface;
        entry->Reference = LowerType(type);
        if (instance == nullptr)
        {
            break;
        }
        _sema->ResolveHierarchy(instance);
        entry->Imported = isImported(instance);
        for (SemaFunction *method : instance->VirtualSlots)
        {
            if (method != nullptr)
            {
                entry->VirtualSlots.Add(LowerFunction(method));
            }
        }
        for (SemaStructInstance *inherited : instance->Interfaces)
        {
            if (inherited != instance)
            {
                entry->Implements.Add(ScriptTypeOf(_sema->GetInstanceType(inherited)));
            }
        }
        break;
    }
    case SemaTypeKind::Delegate:
    {
        entry->Kind = MirScriptTypeKind::Delegate;
        entry->Reference = LowerType(type);
        entry->Imported = isImported(instance);
        SemaSignature signature;
        if (_sema->GetDelegateSignature(instance, signature))
        {
            MirTypeContext &types = _module->GetTypes();
            List<const MirType *> parameters;
            for (usize i = 0; i < signature.Parameters.Count(); ++i)
            {
                const MirType *parameter = LowerType(signature.Parameters[i]);
                parameters.Add(ByReference(signature.Modes[i]) ? types.GetPointer(parameter, MirAddressSpace::Function)
                                                               : parameter);
            }
            entry->Layout = types.GetFunction(LowerType(signature.ReturnType), parameters.AsSpan());
        }
        break;
    }
    case SemaTypeKind::GcArray:
        entry->Kind = MirScriptTypeKind::Array;
        entry->Layout = LowerType(type->GetElement());
        entry->Reference = LowerType(type);
        break;
    case SemaTypeKind::String:
        entry->Kind = MirScriptTypeKind::String;
        entry->Reference = LowerType(type);
        break;
    default:
        // RECOVERY-DECISION lang-lower-D5: every other type is a value type boxed into an object of its own entry.
        entry->Kind = MirScriptTypeKind::Value;
        entry->Layout = LowerType(type);
        entry->Reference = _module->GetTypes().GetReference(entry->Layout);
        entry->Imported = instance != nullptr && isImported(instance);
        break;
    }
    return index;
}

String Lowering::LinkName(SemaFunction *function)
{
    StringBuilder<256> name;
    if (function->Enclosing != nullptr)
    {
        // A lambda is named after the function it is written in and its place among that function's lambdas.
        name.Append(LinkName(function->Enclosing).AsView());
        usize place = function->Enclosing->Lambdas.IndexOf(function);
        if (place == List<SemaFunction *>::NotFound)
        {
            place = function->Enclosing->Lambdas.Count() + _lambdaCount++;
        }
        name.Append(Format("$lambda{0}", place).AsView());
        return name.ToString();
    }
    if (function->Owner != nullptr)
    {
        AppendQualifiedName(*_sema, *function->Owner, name);
        name.Append('.').Append(function->Symbol->Name);
    }
    else
    {
        name.Append(_sema->GetQualifiedName(function->Symbol).AsView());
    }
    if (!function->GenericArguments.IsEmpty())
    {
        name.Append('<');
        for (usize i = 0; i < function->GenericArguments.Count(); ++i)
        {
            name.Append(i == 0 ? "" : ", ");
            const SemaGenericArgument &argument = function->GenericArguments[i];
            if (argument.Type != nullptr)
            {
                argument.Type->Print(name);
            }
            else
            {
                FormatTo(name, "{0}", argument.Value->GetInt());
            }
        }
        name.Append('>');
    }
    if (function->Accessor != SemaAccessor::None)
    {
        name.Append(function->Accessor == SemaAccessor::Getter ? ".get" : ".set");
    }
    name.Append('(');
    for (usize i = 0; i < function->Parameters.Count(); ++i)
    {
        const SemaLocal *parameter = function->Parameters[i];
        name.Append(i == 0 ? "" : ", ");
        const SemaParameterMode mode = ModeOf(parameter);
        name.Append(mode == SemaParameterMode::Ref ? StringView("ref ")
                    : mode == SemaParameterMode::Out ? StringView("out ")
                    : mode == SemaParameterMode::In  ? StringView("in ")
                                                     : StringView());
        parameter->Type->Print(name);
    }
    name.Append(')');
    return name.ToString();
}

Lowering::Environment *Lowering::NewEnvironment(StringView name, bool hasOuter, const Environment *outer,
                                                Span<const SemaLocal *const> locals)
{
    auto environment = MakeUnique<Environment>();
    environment->Outer = outer;
    List<MirStructField> fields;
    if (hasOuter)
    {
        environment->Parent = 0;
        fields.Add({"outer", outer != nullptr ? outer->Reference : LowerType(_sema->Types.GetClass(_sema->GetObjectClass())),
                    MirTypeContext::AutoOffset});
    }
    for (const SemaLocal *local : locals)
    {
        environment->Fields.Add(local, static_cast<uint32>(fields.Count()));
        fields.Add({local->Name, LowerType(local->Type), MirTypeContext::AutoOffset});
    }
    MirScriptType *entry = _module->NewScriptType();
    entry->Kind = MirScriptTypeKind::Class;
    entry->Name = _module->Intern(name);
    entry->Layout = _module->GetTypes().CreateStruct(entry->Name, fields.AsSpan());
    entry->Sealed = true;
    if (SemaStructInstance *object = _sema->GetObjectClass())
    {
        entry->Base = ScriptTypeOf(_sema->Types.GetClass(object));
    }
    environment->Layout = entry->Layout;
    environment->ScriptType = _module->AddScriptType(entry);
    environment->Reference = _module->GetTypes().GetReference(_module->GetTypes().CreateStruct(entry->Name, {}));
    entry->Reference = environment->Reference;
    Environment *result = environment.Get();
    _environmentStorage.Add(Move(environment));
    return result;
}

Lowering::Environment *Lowering::PrepareEnvironment(SemaFunction *function)
{
    if (function->Lambdas.IsEmpty())
    {
        return nullptr;
    }
    if (Environment **found = _environments.Find(function))
    {
        return *found;
    }
    // The lambdas written in the function and in its lambdas: a nested lambda may reach a local of this function
    // that the lambda around it does not use itself.
    List<const SemaFunction *> lambdas;
    for (const SemaFunction *lambda : function->Lambdas)
    {
        lambdas.Add(lambda);
    }
    for (usize i = 0; i < lambdas.Count(); ++i)
    {
        for (const SemaFunction *nested : lambdas[i]->Lambdas)
        {
            lambdas.Add(nested);
        }
    }
    List<const SemaLocal *> locals;
    HashMap<const SemaIteration *, List<const SemaLocal *>> iterations;
    bool capturesThis = false;
    for (const SemaFunction *lambda : lambdas)
    {
        capturesThis = capturesThis || lambda->CapturesThis;
        for (const SemaLocal *local : lambda->Captures)
        {
            if (local->Function != function)
            {
                continue;
            }
            List<const SemaLocal *> &group = local->Iteration != nullptr ? iterations.FindOrAdd(local->Iteration) : locals;
            if (!group.Contains(local))
            {
                group.Add(local);
            }
        }
    }
    if (capturesThis && function->Enclosing == nullptr && function->This != nullptr)
    {
        locals.Add(function->This);
    }
    const String link = LinkName(function);
    Environment *result = NewEnvironment(Format("{0}$closure", link.AsView()).AsView(), function->Enclosing != nullptr,
                                         function->Enclosing != nullptr ? EnclosingEnvironment(function) : nullptr,
                                         locals.AsSpan());
    result->Function = function;
    _environments.Add(function, result);
    uint32 count = 0;
    auto create = [&](auto &self, const SemaIteration *iteration) -> Environment * {
        if (iteration == nullptr)
        {
            return result;
        }
        if (Environment **existing = _iterationEnvironments.Find(iteration))
        {
            return *existing;
        }
        Environment *outer = self(self, iteration->Parent);
        const List<const SemaLocal *> *group = iterations.Find(iteration);
        if (group == nullptr)
        {
            return outer;
        }
        Environment *environment =
            NewEnvironment(Format("{0}$iteration{1}", link.AsView(), count++).AsView(), true, outer, group->AsSpan());
        environment->Function = function;
        _iterationEnvironments.Add(iteration, environment);
        return environment;
    };
    for (const SemaFunction *lambda : lambdas)
    {
        for (const SemaLocal *local : lambda->Captures)
        {
            if (local->Function == function)
            {
                create(create, local->Iteration);
            }
        }
    }
    return result;
}

const Lowering::Environment *Lowering::ScopeEnvironment(SemaFunction *function, const SemaIteration *iteration)
{
    for (; iteration != nullptr; iteration = iteration->Parent)
    {
        if (Environment **found = _iterationEnvironments.Find(iteration))
        {
            return *found;
        }
    }
    return PrepareEnvironment(function);
}

const Lowering::Environment *Lowering::EnclosingEnvironment(SemaFunction *lambda)
{
    return ScopeEnvironment(lambda->Enclosing, lambda->Iteration);
}

MirValue *Lowering::ScopeValue(const SemaIteration *iteration)
{
    for (; iteration != nullptr; iteration = iteration->Parent)
    {
        if (MirValue **found = _iterationValues.Find(iteration))
        {
            return *found;
        }
    }
    return _environment;
}

void Lowering::BeginIteration(const SemaIteration *iteration)
{
    Environment **found = iteration != nullptr ? _iterationEnvironments.Find(iteration) : nullptr;
    if (found == nullptr)
    {
        return;
    }
    const Environment &environment = **found;
    MirValue *value = _builder.New(environment.Reference, environment.ScriptType);
    const MirType *parentType = environment.Layout->GetFields()[environment.Parent].Type;
    _builder.Store(_builder.FieldAddress(value, parentType, environment.Parent, environment.ScriptType),
                   ScopeValue(iteration->Parent));
    _iterationValues.Set(iteration, value);
}

void Lowering::BeginScriptFunction(SemaFunction *function, MirFunction *result)
{
    const AstDecl *decl = function->Symbol->Decl;
    result->AlwaysInline = decl != nullptr && Sema::FindAttribute(decl->Attributes, "Inline") != nullptr;
    result->Common = function->Symbol->File != nullptr &&
                     function->Symbol->File->File->GetDialect() == LanguageDialect::Common;
    // RECOVERY-DECISION lang-lower-D6: `[Test]` functions are marked for the script test runner (MarlinScript §15).
    result->Test = decl != nullptr && function->Enclosing == nullptr &&
                   Sema::FindAttribute(decl->Attributes, "Test") != nullptr;
    // The engine reads which pointers a function only reads through, such as the components of a script system.
    const uint32 first = function->This != nullptr ? 1 : 0;
    for (usize i = 0; i < function->Parameters.Count() && first + i < 64; ++i)
    {
        if (function->Parameters[i]->Mode == SemaParameterMode::In)
        {
            result->ReadOnlyArguments |= uint64{1} << (first + i);
        }
    }
    if (function->Kind == SemaFunctionKind::Async)
    {
        result->Kind = MirFunctionKind::Async;
        result->ResultType = LowerType(function->ResultType);
    }
    else if (function->Kind == SemaFunctionKind::Iterator)
    {
        result->Kind = MirFunctionKind::Iterator;
    }
    if (decl != nullptr)
    {
        if (const AstAttribute *native = Sema::FindAttribute(decl->Attributes, "Native"))
        {
            if (SemaConstant *text = _sema->EvaluateAttributeArgument(*native, 0, {}, _sema->ContextOf(function->Symbol)))
            {
                result->NativeName = _module->Intern(text->Text);
            }
        }
    }
}

MirValue *Lowering::CapturedAddress(const SemaLocal *local)
{
    MirValue *environment = nullptr;
    const Environment *info = nullptr;
    if (local->Function == _currentSema)
    {
        environment = ScopeValue(local->Iteration);
        info = ScopeEnvironment(_currentSema, local->Iteration);
    }
    else
    {
        environment = _outerEnvironment;
        info = EnclosingEnvironment(_currentSema);
        while (info != nullptr && info->Fields.Find(local) == nullptr && info->Parent != ~0u)
        {
            const MirType *parentType = info->Layout->GetFields()[info->Parent].Type;
            environment = _builder.Load(_builder.FieldAddress(environment, parentType, info->Parent, info->ScriptType));
            info = info->Outer;
        }
    }
    const uint32 *field = info != nullptr ? info->Fields.Find(local) : nullptr;
    MARLIN_ASSERT(field != nullptr && environment != nullptr);
    return _builder.FieldAddress(environment, info->Layout->GetFields()[*field].Type, *field, info->ScriptType);
}

MirValue *Lowering::CallFunction(SemaFunction *function, BoundDispatch dispatch, Span<MirValue *const> arguments)
{
    MirFunction *target = LowerFunction(function);
    const Span<const MirType *const> parameters = target->GetType()->GetParameters();
    List<MirValue *> passed;
    for (usize i = 0; i < arguments.Count(); ++i)
    {
        MirValue *argument = arguments[i];
        // A reference converts to the declared type of its parameter without a check: upcasts are checked by Sema.
        if (i < parameters.Count() && argument->GetType() != parameters[i] &&
            argument->GetType()->Is(MirTypeKind::Reference) && parameters[i]->Is(MirTypeKind::Reference))
        {
            argument = _builder.Bitcast(argument, parameters[i]);
        }
        passed.Add(argument);
    }
    if (dispatch == BoundDispatch::Virtual && function->Slot != ~0u)
    {
        return _builder.CallVirtual(target, function->Slot, passed.AsSpan());
    }
    if (dispatch == BoundDispatch::Interface && function->Slot != ~0u && function->Owner != nullptr)
    {
        return _builder.CallInterface(target, ScriptTypeOf(_sema->GetInstanceType(function->Owner)), function->Slot,
                                      passed.AsSpan());
    }
    return _builder.Call(target, passed.AsSpan());
}

void Lowering::PropertyOperands(BoundProperty *property, MirValue *&receiver, List<MirValue *> &indices)
{
    receiver = nullptr;
    if (property->Receiver != nullptr)
    {
        const SemaFunction *accessor = property->Getter != nullptr ? property->Getter : property->Setter;
        if (accessor->This == nullptr || accessor->This->Mode == SemaParameterMode::Value)
        {
            receiver = Value(property->Receiver);
        }
        else
        {
            // RECOVERY-DECISION lang-lower-D7: a struct receiver outside function memory is copied, as `in` would.
            MirValue *pointer = property->Receiver->IsLValue ? Address(property->Receiver) : nullptr;
            receiver = pointer != nullptr && pointer->GetType()->GetAddressSpace() == MirAddressSpace::Function
                           ? pointer
                           : Spill(pointer != nullptr ? _builder.Load(pointer) : Value(property->Receiver));
        }
    }
    for (BoundExpr *index : property->Arguments)
    {
        indices.Add(Value(index));
    }
}

MirValue *Lowering::ReadProperty(BoundProperty *property, MirValue *receiver, Span<MirValue *const> indices)
{
    List<MirValue *> arguments;
    if (receiver != nullptr)
    {
        arguments.Add(receiver);
    }
    arguments.AddRange(indices);
    return CallFunction(property->Getter, property->Dispatch, arguments.AsSpan());
}

void Lowering::WriteProperty(BoundProperty *property, MirValue *receiver, Span<MirValue *const> indices,
                             MirValue *value)
{
    List<MirValue *> arguments;
    if (receiver != nullptr)
    {
        arguments.Add(receiver);
    }
    arguments.AddRange(indices);
    arguments.Add(value);
    CallFunction(property->Setter, property->Dispatch, arguments.AsSpan());
}

MirValue *Lowering::TypeConversion(BoundTypeConversion *node)
{
    MirValue *value = Value(node->Operand);
    const SemaType *from = node->Operand->Type;
    const SemaType *to = node->Type;
    const MirType *target = LowerType(to);
    switch (node->Conversion)
    {
    case BoundTypeConversionKind::Upcast:
        return value->GetType() == target ? value : _builder.Bitcast(value, target);
    case BoundTypeConversionKind::Downcast:
    case BoundTypeConversionKind::TryDowncast:
        return _builder.Cast(value, target, ScriptTypeOf(to), node->Conversion == BoundTypeConversionKind::TryDowncast);
    case BoundTypeConversionKind::Box:
        return _builder.Box(value, target, ScriptTypeOf(from));
    case BoundTypeConversionKind::Unbox:
        return _builder.Unbox(value, target, ScriptTypeOf(to));
    case BoundTypeConversionKind::WrapNullable:
    {
        if (target->Is(MirTypeKind::Reference))
        {
            return value->GetType() == target ? value : _builder.Bitcast(value, target);
        }
        MirValue *fields[] = {value, _module->GetBool(true)};
        return _builder.Construct(target, fields);
    }
    case BoundTypeConversionKind::RemoveNullable:
        break;
    }
    if (value->GetType()->Is(MirTypeKind::Reference))
    {
        // `x!` checks; `?.`, `??` and flow-proven uses already tested for null (MarlinScript §3.6).
        if (node->Explicit)
        {
            return _builder.NullCheck(value, target);
        }
        return value->GetType() == target ? value : _builder.Bitcast(value, target);
    }
    if (node->Explicit)
    {
        const uint32 hasValue[] = {1};
        MirBlock *header = _builder.GetBlock();
        MirBlock *empty = NewBlock("null.fault");
        MirBlock *merge = NewBlock("null.merge");
        header->Merge = {MirMergeKind::Selection, merge, nullptr};
        _builder.CondBranch(_builder.Extract(value, hasValue), merge, empty);
        _builder.SetInsertPoint(empty);
        _builder.Fault(MirFault::NullReference);
        _current->MoveBlock(merge, _current->GetBlocks().Count() - 1);
        _builder.SetInsertPoint(merge);
    }
    const uint32 valueField[] = {0};
    return _builder.Extract(value, valueField);
}

MirValue *Lowering::ScriptAddress(BoundExpr *expression)
{
    switch (expression->Kind)
    {
    case BoundKind::ObjectField:
    {
        auto *node = expression->As<BoundObjectField>();
        MirValue *object = Value(node->Object);
        const MirType *layout = ClassLayout(node->Instance);
        return _builder.FieldAddress(object, layout->GetFields()[node->FieldIndex].Type, node->FieldIndex,
                                     ScriptTypeOf(_sema->Types.GetClass(node->Instance)));
    }
    case BoundKind::ArrayElement:
    {
        auto *node = expression->As<BoundArrayElement>();
        MirValue *array = Value(node->Array);
        MirValue *index = ConvertValue(Value(node->Index), node->Index->Type, _sema->Types.GetInt(32, true));
        return _builder.ElementAddress(array, index);
    }
    default:
        return nullptr;
    }
}

MirValue *Lowering::ScriptValue(BoundExpr *expression)
{
    const MirType *type = LowerType(expression->Type);
    switch (expression->Kind)
    {
    case BoundKind::ObjectField:
    case BoundKind::ArrayElement:
        return _builder.Load(ScriptAddress(expression));
    case BoundKind::NewObject:
    {
        auto *node = expression->As<BoundNewObject>();
        MirValue *object = _builder.New(type, ScriptTypeOf(node->Type));
        // The constructor runs on the new object as its `this`, which a hidden local holds.
        SemaLocal *self = _sema->New<SemaLocal>();
        self->Name = "this";
        self->Type = node->Type;
        self->Function = _currentSema;
        _locals.Set(self, Spill(object, "new"));
        auto *receiver = MakeExpr<BoundLocal>(*_sema, node->Type, node->Range);
        receiver->Local = self;
        List<BoundExpr *> arguments;
        arguments.Add(receiver);
        arguments.AddRange(node->Arguments);
        auto *call = MakeExpr<BoundCall>(*_sema, node->Type, node->Range);
        call->Function = node->Constructor;
        call->Arguments = _sema->Freeze(arguments);
        Call(call);
        return object;
    }
    case BoundKind::NewArray:
    {
        auto *node = expression->As<BoundNewArray>();
        const SemaType *intType = _sema->Types.GetInt(32, true);
        const uint32 scriptType = ScriptTypeOf(node->Type);
        if (node->Length != nullptr)
        {
            return _builder.NewArray(type, scriptType, ConvertValue(Value(node->Length), node->Length->Type, intType));
        }
        const MirType *i32 = LowerType(intType);
        MirValue *array =
            _builder.NewArray(type, scriptType, _module->GetInt(i32, static_cast<int64>(node->Elements.Count())));
        for (usize i = 0; i < node->Elements.Count(); ++i)
        {
            MirValue *value = Value(node->Elements[i]);
            _builder.Store(_builder.ElementAddress(array, _module->GetInt(i32, static_cast<int64>(i))), value);
        }
        return array;
    }
    case BoundKind::Property:
    {
        auto *property = expression->As<BoundProperty>();
        MirValue *receiver = nullptr;
        List<MirValue *> indices;
        PropertyOperands(property, receiver, indices);
        return ReadProperty(property, receiver, indices.AsSpan());
    }
    case BoundKind::TypeConversion:
        return TypeConversion(expression->As<BoundTypeConversion>());
    case BoundKind::TypeTest:
    {
        auto *node = expression->As<BoundTypeTest>();
        return _builder.IsInstance(Value(node->Operand), ScriptTypeOf(node->Tested));
    }
    case BoundKind::TypeOf:
        return _builder.TypeOf(type, ScriptTypeOf(expression->As<BoundTypeOf>()->Described));
    case BoundKind::Let:
    {
        auto *node = expression->As<BoundLet>();
        MirValue *initial = Value(node->Initial);
        if (InScript() && node->Local->IsCaptured && node->Local->Function == _currentSema)
        {
            MirValue *field = CapturedAddress(node->Local);
            _builder.Store(field, initial);
            _locals.Set(node->Local, field);
        }
        else
        {
            _locals.Set(node->Local, Spill(initial, node->Local->Name));
        }
        return Value(node->Result);
    }
    case BoundKind::MakeDelegate:
    {
        auto *node = expression->As<BoundMakeDelegate>();
        MirFunction *function = LowerFunction(node->Function);
        MirValue *receiver = node->Receiver != nullptr ? Value(node->Receiver) : nullptr;
        // RECOVERY-DECISION lang-lower-D8: a delegate of a virtual or interface member names the declared function
        // and its slot, which the VM resolves on the receiver like CallVirtual and CallInterface; -1 calls it directly.
        const int64 slot = node->Dispatch != BoundDispatch::Direct && node->Function->Slot != ~0u
                               ? static_cast<int64>(node->Function->Slot)
                               : -1;
        return _builder.MakeDelegate(type, ScriptTypeOf(node->Type), function, receiver, slot);
    }
    case BoundKind::Lambda:
    {
        SemaFunction *lambda = expression->As<BoundLambda>()->Function;
        MirFunction *function = LowerFunction(lambda);
        MirValue *receiver = ScopeValue(lambda->Iteration);
        if (receiver == nullptr)
        {
            receiver = _module->GetNull(function->GetType()->GetParameters()[0]);
        }
        return _builder.MakeDelegate(type, ScriptTypeOf(expression->Type), function, receiver, -1);
    }
    case BoundKind::InvokeDelegate:
    {
        auto *node = expression->As<BoundInvokeDelegate>();
        MirValue *delegate = Value(node->Delegate);
        SemaSignature signature;
        const SemaType *delegateType = node->Delegate->Type->Is(SemaTypeKind::Nullable)
                                           ? node->Delegate->Type->GetElement()
                                           : node->Delegate->Type;
        const bool known = delegateType->HasInstance() && _sema->GetDelegateSignature(delegateType->GetStruct(), signature);
        List<MirValue *> arguments;
        for (usize i = 0; i < node->Arguments.Count(); ++i)
        {
            const bool byReference = known && i < signature.Modes.Count() && ByReference(signature.Modes[i]);
            arguments.Add(byReference ? Address(node->Arguments[i]) : Value(node->Arguments[i]));
        }
        return _builder.CallDelegate(delegate, type, arguments.AsSpan(), node->IsEvent);
    }
    case BoundKind::CombineDelegates:
    {
        auto *node = expression->As<BoundCombineDelegates>();
        MirValue *left = Value(node->Left);
        MirValue *right = Value(node->Right);
        if (right->GetType() != left->GetType())
        {
            right = _builder.Bitcast(right, left->GetType());
        }
        return _builder.CombineDelegates(left, right, node->Remove);
    }
    case BoundKind::Await:
        return _builder.Await(Value(expression->As<BoundAwait>()->Operand), type);
    default:
        break;
    }
    MARLIN_ASSERT(expression->Kind != BoundKind::Unbound);
    return _module->GetUndef(type);
}

void Lowering::PatternSwitch(BoundSwitch *node)
{
    // The sections are tested in order into a choice, then switched on, so the tests stay structured and `break`
    // leaves the switch (MarlinScript §3.9).
    MirValue *input = Value(node->Selector);
    _locals.Set(node->Input, Spill(input, node->Input->Name));
    const MirType *i32 = _module->GetTypes().GetInt(32, true);
    MirInstruction *choice = _builder.Variable(i32, "case");
    _builder.Store(choice, _module->GetInt(i32, -1));
    List<int64> cases;
    for (usize i = 0; i < node->Sections.Count(); ++i)
    {
        const BoundSwitchSection &section = node->Sections[i];
        if (section.Match == nullptr || _builder.IsTerminated())
        {
            continue;
        }
        // A section is tested only while no earlier one matched, so its pattern variables and `when` run once.
        MirBlock *next = nullptr;
        if (!cases.IsEmpty())
        {
            MirValue *open = _builder.Compare(MirOp::Equal, _builder.Load(choice), _module->GetInt(i32, -1));
            MirBlock *test = NewBlock("case.test");
            next = NewBlock("case.next");
            _builder.GetBlock()->Merge = {MirMergeKind::Selection, next, nullptr};
            _builder.CondBranch(open, test, next);
            _builder.SetInsertPoint(test);
        }
        MirValue *matches = Value(section.Match);
        MirBlock *taken = NewBlock("case.match");
        MirBlock *tested = NewBlock("case.tested");
        _builder.GetBlock()->Merge = {MirMergeKind::Selection, tested, nullptr};
        _builder.CondBranch(matches, taken, tested);
        _builder.SetInsertPoint(taken);
        _builder.Store(choice, _module->GetInt(i32, static_cast<int64>(i)));
        _builder.Branch(tested);
        _current->MoveBlock(tested, _current->GetBlocks().Count() - 1);
        _builder.SetInsertPoint(tested);
        if (next != nullptr)
        {
            _builder.Branch(next);
            _current->MoveBlock(next, _current->GetBlocks().Count() - 1);
            _builder.SetInsertPoint(next);
        }
        cases.Add(static_cast<int64>(i));
    }
    MirBlock *header = _builder.GetBlock();
    MirBlock *merge = NewBlock("switch.merge");
    MirBlock *defaultBlock = merge;
    List<MirBlock *> targets;
    List<MirBlock *> sectionBlocks;
    for (const BoundSwitchSection &section : node->Sections)
    {
        MirBlock *block = NewBlock(section.Match == nullptr ? StringView("switch.default") : StringView("switch.case"));
        sectionBlocks.Add(block);
        if (section.Match != nullptr)
        {
            targets.Add(block);
        }
        if (section.IsDefault || section.Match == nullptr)
        {
            defaultBlock = block;
        }
    }
    header->Merge = {MirMergeKind::Selection, merge, nullptr};
    _builder.Switch(_builder.Load(choice), defaultBlock, cases.AsSpan(), targets.AsSpan());
    _loops.Add({merge, _loops.IsEmpty() ? nullptr : _loops.Last().Continue});
    for (usize i = 0; i < node->Sections.Count(); ++i)
    {
        _builder.SetInsertPoint(sectionBlocks[i]);
        Block(node->Sections[i].Statements);
        if (!_builder.IsTerminated())
        {
            _builder.Branch(merge);
        }
    }
    _loops.Pop();
    _current->MoveBlock(merge, _current->GetBlocks().Count() - 1);
    _builder.SetInsertPoint(merge);
}

void Lowering::LowerStaticInitializer(StringView name, Span<SemaGlobal *const> statics)
{
    MirTypeContext &types = _module->GetTypes();
    MirFunction *function = _module->AddFunction(types.GetFunction(types.GetVoid(), {}), name);
    function->AlwaysInline = false;
    MirFunction *savedCurrent = _current;
    SemaFunction *savedSema = _currentSema;
    MirBlock *savedBlock = _builder.GetBlock();
    const MirLocation savedLocation = _builder.GetLocation();
    HashMap<const SemaLocal *, MirValue *> savedLocals = Move(_locals);
    _locals = {};
    _current = function;
    _currentSema = nullptr;
    _builder.SetInsertPoint(function->AddBlock("entry"));
    for (SemaGlobal *global : statics)
    {
        if (global->Initializer == nullptr || _builder.IsTerminated())
        {
            continue;
        }
        SetLocation(global->Initializer->Range);
        if (function->Location.File == 0)
        {
            function->Location = _builder.GetLocation();
        }
        MirValue *value = Value(global->Initializer);
        _builder.Store(LowerGlobal(global), value);
    }
    if (!_builder.IsTerminated())
    {
        _builder.Return();
    }
    _module->AddStaticInitializer(function);
    _current = savedCurrent;
    _currentSema = savedSema;
    _locals = Move(savedLocals);
    _builder.SetLocation(savedLocation);
    if (savedBlock != nullptr)
    {
        _builder.SetInsertPoint(savedBlock);
    }
}
} // namespace Marlin
