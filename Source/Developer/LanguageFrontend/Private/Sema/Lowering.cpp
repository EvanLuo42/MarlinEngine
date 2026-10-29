#include "Sema/Lowering.h"

#include "LanguageIR/MirPrinter.h"
#include "Sema/SemaHelpers.h"

#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
/// The bytes of [Uniform] parameters every target passes directly: Vulkan push data, D3D12 root constants (§9.3).
constexpr uint32 DirectArgumentsSize = 128;

/// Appends *text* to the `Debug.Print` format *format* as literal text, its braces escaped (§9.11).
void AppendEscapedFormat(String &format, StringView text)
{
    for (const char c : text)
    {
        format.Append(c);
        if (c == '{' || c == '}')
        {
            format.Append(c);
        }
    }
}

/// Whether the Ptr *buffer* reads is declared `[NoAlias]` (§4.5): a parameter or field with the attribute, or the Pointer
/// of a Span that is.
bool IsNoAlias(const BoundExpr *buffer)
{
    if (buffer->Kind == BoundKind::Local)
    {
        return Sema::FindAttribute(buffer->As<BoundLocal>()->Local->Attributes, "NoAlias") != nullptr;
    }
    if (buffer->Kind != BoundKind::Field || !buffer->As<BoundField>()->Object->Type->Is(SemaTypeKind::Struct))
    {
        return false;
    }
    const BoundExpr *object = buffer->As<BoundField>()->Object;
    const SemaStructInstance *owner = object->Type->GetStruct();
    const SemaField &field = owner->Fields[buffer->As<BoundField>()->FieldIndex];
    if (field.Symbol != nullptr && Sema::FindAttribute(field.Symbol->Decl->Attributes, "NoAlias") != nullptr)
    {
        return true;
    }
    return owner->Symbol->BuiltinId == static_cast<uint32>(BuiltinTypeId::Span) && IsNoAlias(object);
}
} // namespace

Lowering::Lowering(Sema &sema, MirModule &module) : _sema(&sema), _module(&module), _builder(module)
{
}

MirLocation Lowering::LocationOf(SourceRange range)
{
    const SourceFile *file = _sema->Diagnostics.GetFile(range.File);
    if (file == nullptr)
    {
        return {};
    }
    uint32 *index = _files.Find(range.File);
    if (index == nullptr)
    {
        index = &_files.Add(range.File, _module->AddFile(file->GetPath(),
                                                         _sema->Options->DebugInfo ? file->GetText() : StringView()));
    }
    MirLocation location;
    location.File = *index;
    file->GetLineAndColumn(range.Begin, location.Line, location.Column);
    return location;
}

void Lowering::SetLocation(SourceRange range)
{
    _builder.SetLocation(LocationOf(range));
}

void Lowering::DeclareForDebugger(MirValue *variable, StringView name, const SemaType *type, SourceRange range)
{
    // RECOVERY-DECISION lang-lower-D2: shaders only (the script target keeps line tables, not variables), and the
    // value is declared where the variable gets its first value.
    if (!_sema->Options->DebugInfo || InScript() || !type->IsStorable() || _builder.IsTerminated())
    {
        return;
    }
    const MirType *valueType = variable->GetType()->GetElement();
    MirValue *operands[] = {_builder.Load(variable)};
    const int64 literals[] = {_module->AddDebugVariable(name, valueType, LocationOf(range))};
    _builder.Intrinsic(MirIntrinsic::DebugDeclare, _module->GetTypes().GetVoid(), operands, literals);
}

const MirType *Lowering::LowerType(const SemaType *type)
{
    if (const MirType *const *found = _types.Find(type))
    {
        return *found;
    }
    MirTypeContext &types = _module->GetTypes();
    const MirType *result = nullptr;
    switch (type->GetKind())
    {
    case SemaTypeKind::Void:
        result = types.GetVoid();
        break;
    case SemaTypeKind::Bool:
        result = types.GetBool();
        break;
    case SemaTypeKind::Int:
        result = types.GetInt(type->GetWidth(), type->IsSigned());
        break;
    case SemaTypeKind::Float:
        result = types.GetFloat(type->GetWidth());
        break;
    case SemaTypeKind::Enum:
        result = LowerType(type->GetElement());
        break;
    case SemaTypeKind::Vector:
        result = types.GetVector(LowerType(type->GetElement()), type->GetCount());
        break;
    case SemaTypeKind::Matrix:
        result = types.GetMatrix(LowerType(type->GetElement()), type->GetRows(), type->GetCount());
        break;
    case SemaTypeKind::Array:
        result = types.GetArray(LowerType(type->GetElement()), type->GetCount());
        break;
    case SemaTypeKind::Struct:
    {
        SemaStructInstance *instance = type->GetStruct();
        _sema->ResolveFields(instance);
        List<MirStructField> fields;
        for (const SemaField &field : instance->Fields)
        {
            fields.Add({field.Name, LowerType(field.Type), field.Offset != ~0u ? field.Offset : MirTypeContext::AutoOffset,
                        Sema::IsNoDiff(field.Symbol->Decl->Attributes)});
        }
        StringBuilder<128> name;
        // A script module names its structs with their namespaces, as the engine finds them by those names
        // (MarlinScript §11.2).
        const SemaSymbol *container = instance->Symbol->Parent;
        if (_package != 0 && container != nullptr && container != _sema->GetGlobalNamespace())
        {
            name.Append(_sema->GetQualifiedName(container).AsView()).Append('.');
        }
        type->Print(name);
        result = types.CreateStruct(name.AsView(), fields.AsSpan(), instance->Size);
        if (result == nullptr)
        {
            _sema->Diagnostics
                .Error(DiagnosticCode::LayoutViolation, instance->Symbol->Decl->NameRange,
                       "the layout of `{0}` breaks the alignment rules", name.AsView())
                .WithNote("an `[Offset]` must be at least the end of the previous field and a multiple of the field's "
                          "alignment; a `[StructLayout(Size)]` must cover every field (§10)");
            result = types.CreateStruct(name.AsView(), {}, 0);
        }
        break;
    }
    case SemaTypeKind::Texture:
        result = types.GetTexture(LowerType(type->GetElement()), type->GetTextureDim(), type->IsArrayed(),
                                  type->IsMultisampled(), type->IsWritable(), type->GetImageFormat());
        break;
    case SemaTypeKind::Buffer:
        result = types.GetBuffer(type->GetElement() != nullptr ? LowerType(type->GetElement()) : nullptr,
                                 type->IsWritable());
        break;
    case SemaTypeKind::Sampler:
        result = types.GetSampler(type->IsComparison());
        break;
    case SemaTypeKind::AccelerationStructure:
        result = types.GetAccelerationStructure();
        break;
    case SemaTypeKind::RayQuery:
        result = types.GetRayQuery(type->GetCount());
        break;
    case SemaTypeKind::HitObject:
        result = types.GetHitObject();
        break;
    case SemaTypeKind::CoopVector:
        result = types.GetCoopVector(LowerType(type->GetElement()), type->GetCount());
        break;
    case SemaTypeKind::CoopMatrix:
        result = types.GetCoopMatrix(LowerType(type->GetElement()), type->GetRows(), type->GetCount(),
                                     type->GetMatrixUse());
        break;
    case SemaTypeKind::MatrixRef:
    case SemaTypeKind::VectorRef:
    {
        // RECOVERY-DECISION lang-spirv-D4: a MatrixRef is {buffer, offset, layout, stride} and a VectorRef
        // {buffer, offset}, built with CompositeConstruct so the backend finds the buffer operand.
        const MirType *u32 = types.GetInt(32, false);
        const MirStructField fields[] = {{"Buffer", types.GetBuffer(nullptr, false), MirTypeContext::AutoOffset},
                                         {"Offset", u32, MirTypeContext::AutoOffset},
                                         {"Layout", u32, MirTypeContext::AutoOffset},
                                         {"Stride", u32, MirTypeContext::AutoOffset}};
        StringBuilder<64> name;
        type->Print(name);
        const usize count = type->Is(SemaTypeKind::MatrixRef) ? 4 : 2;
        result = types.CreateStruct(name.AsView(), Span<const MirStructField>(fields, count));
        break;
    }
    case SemaTypeKind::Class:
    case SemaTypeKind::Interface:
    case SemaTypeKind::Delegate:
    case SemaTypeKind::GcArray:
    case SemaTypeKind::Nullable:
    case SemaTypeKind::String:
        result = InScript() || !type->Is(SemaTypeKind::String) ? LowerScriptType(type) : types.GetVoid();
        break;
    default:
        result = types.GetVoid();
        break;
    }
    _types.Add(type, result);
    if (InScript() && result->Is(MirTypeKind::Reference) &&
        (type->Is(SemaTypeKind::String) || type->Is(SemaTypeKind::GcArray)))
    {
        ScriptTypeOf(type);
    }
    return result;
}

MirConstant *Lowering::LowerConstant(const SemaConstant *constant, const SemaType *type)
{
    const MirType *mirType = LowerType(type);
    if (constant->IsZero)
    {
        return _module->GetNull(mirType);
    }
    const SemaType *scalar = UnderlyingOf(type);
    switch (scalar->GetKind())
    {
    case SemaTypeKind::Bool:
        return _module->GetBool(constant->Bits != 0);
    case SemaTypeKind::Int:
        return _module->GetInt(mirType, constant->GetInt());
    case SemaTypeKind::Float:
        return _module->GetFloat(mirType, constant->Float);
    case SemaTypeKind::Vector:
    case SemaTypeKind::Matrix:
    case SemaTypeKind::Array:
    case SemaTypeKind::Struct:
    {
        List<MirConstant *> elements;
        for (usize i = 0; i < constant->Elements.Count(); ++i)
        {
            const SemaType *elementType = nullptr;
            if (scalar->Is(SemaTypeKind::Struct))
            {
                elementType = scalar->GetStruct()->Fields[i].Type;
            }
            else if (scalar->Is(SemaTypeKind::Matrix))
            {
                elementType = _sema->Types.GetVector(scalar->GetElement(), scalar->GetCount());
            }
            else
            {
                elementType = scalar->GetElement();
            }
            elements.Add(LowerConstant(constant->Elements[i], elementType));
        }
        return _module->GetComposite(mirType, elements.AsSpan());
    }
    default:
        return _module->GetNull(mirType);
    }
}

void Lowering::UseGlobal(MirGlobal *global)
{
    if (!_interface.Contains(global))
    {
        _interface.Add(global);
    }
}

MirGlobal *Lowering::LowerGlobal(SemaGlobal *global)
{
    if (MirGlobal **found = _globals.Find(global))
    {
        UseGlobal(*found);
        return *found;
    }
    const String name = _sema->GetQualifiedName(global->Symbol);
    if (global->Kind == SemaGlobalKind::Specialization)
    {
        MirGlobal *result = _module->AddGlobal(LowerType(global->Type), MirAddressSpace::Specialization, name.AsView());
        result->Location = _specializations++;
        if (global->Default != nullptr)
        {
            result->Initializer = LowerConstant(global->Default, global->Type);
        }
        _globals.Add(global, result);
        UseGlobal(result);
        return result;
    }
    if (global->Kind == SemaGlobalKind::Static)
    {
        // RECOVERY-DECISION lang-lower-D3: a static field is a Static global named by its qualified name (with the
        // generic arguments of its owner); the script target lays the module's statics out and links them by name.
        StringBuilder<128> qualified;
        if (global->Owner != nullptr)
        {
            AppendQualifiedName(*_sema, *global->Owner, qualified);
            qualified.Append('.').Append(global->Symbol->Name);
        }
        MirGlobal *result = _module->AddGlobal(LowerType(global->Type), MirAddressSpace::Static,
                                               global->Owner != nullptr ? qualified.AsView() : name.AsView());
        _globals.Add(global, result);
        return result;
    }
    MirGlobal *result = _module->AddGlobal(LowerType(global->Type), MirAddressSpace::GroupShared, name.AsView());
    _globals.Add(global, result);
    UseGlobal(result);
    return result;
}

MirGlobal *Lowering::TaskPayload(const MirType *type)
{
    if (_taskPayload == nullptr)
    {
        _taskPayload = _module->AddGlobal(type, MirAddressSpace::TaskPayload, "payload");
    }
    UseGlobal(_taskPayload);
    return _taskPayload;
}

MirGlobal *Lowering::SystemValue(MirBuiltin builtin, const MirType *type, MirAddressSpace space, StringView name)
{
    const uint32 key = static_cast<uint32>(builtin) | (static_cast<uint32>(space) << 16);
    if (MirGlobal **found = _systemValues.Find(key))
    {
        UseGlobal(*found);
        return *found;
    }
    MirGlobal *global = _module->AddGlobal(type, space, name);
    global->Builtin = builtin;
    _systemValues.Add(key, global);
    UseGlobal(global);
    return global;
}

MirBlock *Lowering::NewBlock(StringView name)
{
    return _current->AddBlock(name);
}

MirValue *Lowering::Spill(MirValue *value, StringView name)
{
    MirInstruction *variable = _builder.Variable(value->GetType(), name);
    _builder.Store(variable, value);
    return variable;
}

MirValue *Lowering::Splat(MirValue *scalar, const MirType *type)
{
    if (type->Is(MirTypeKind::Vector))
    {
        List<MirValue *> elements;
        elements.Resize(type->GetCount(), scalar);
        return _builder.Construct(type, elements.AsSpan());
    }
    MirValue *row = Splat(scalar, type->GetElement());
    List<MirValue *> rows;
    rows.Resize(type->GetRows(), row);
    return _builder.Construct(type, rows.AsSpan());
}

MirFunction *Lowering::LowerFunction(SemaFunction *function)
{
    if (MirFunction **found = _functions.Find(function))
    {
        return *found;
    }
    MirTypeContext &types = _module->GetTypes();
    List<const MirType *> parameters;
    const bool referenceThis = function->This != nullptr && function->This->Type->IsReference();
    const bool isLambda = function->Enclosing != nullptr;
    const bool isClassConstructor = referenceThis && function->Symbol->Kind == SemaSymbolKind::Constructor;
    const bool hasThis = !isLambda && function->This != nullptr &&
                         (function->Symbol->Kind != SemaSymbolKind::Constructor || referenceThis);
    // A class's `this` and an extension's are passed by value, a struct's by reference.
    const bool valueThis = function->This != nullptr && function->This->Mode == SemaParameterMode::Value;
    if (isLambda)
    {
        const Environment *outer = EnclosingEnvironment(function);
        parameters.Add(outer != nullptr ? outer->Reference
                                        : LowerType(_sema->Types.GetClass(_sema->GetObjectClass())));
    }
    else if (hasThis)
    {
        parameters.Add(valueThis ? LowerType(function->This->Type)
                                 : types.GetPointer(LowerType(function->This->Type), MirAddressSpace::Function));
    }
    for (const SemaLocal *parameter : function->Parameters)
    {
        if (parameter->Type->Is(SemaTypeKind::MeshOutput))
        {
            continue;
        }
        const MirType *type = LowerType(parameter->Type);
        const bool byReference = parameter->Mode == SemaParameterMode::Ref || parameter->Mode == SemaParameterMode::Out;
        parameters.Add(byReference ? types.GetPointer(type, MirAddressSpace::Function) : type);
    }
    // RECOVERY-DECISION lang-lower-D4: a class constructor runs on the object `new` allocated and returns nothing.
    const MirType *returnType = isClassConstructor ? types.GetVoid() : LowerType(function->ReturnType);
    const MirType *functionType = types.GetFunction(returnType, parameters.AsSpan());
    const String name = InScript() ? LinkName(function) : _sema->GetQualifiedName(function->Symbol);
    MirFunction *result = _module->AddFunction(functionType, name.AsView());
    result->Location = LocationOf(function->Symbol->Decl->NameRange);
    _functions.Add(function, result);
    if (InScript())
    {
        BeginScriptFunction(function, result);
        const bool generic = !function->GenericArguments.IsEmpty() ||
                             (function->Owner != nullptr && !function->Owner->Arguments.IsEmpty());
        result->Imported = _package != 0 && function->Symbol->Package != _package && !generic &&
                           result->NativeName.IsEmpty() && !isLambda;
        if (function->Body == nullptr || result->Imported)
        {
            return result;
        }
    }
    MirValue *savedEnvironment = _environment;
    MirValue *savedOuterEnvironment = _outerEnvironment;
    _environment = nullptr;
    _outerEnvironment = nullptr;

    MirFunction *savedCurrent = _current;
    SemaFunction *savedSema = _currentSema;
    HashMap<const SemaLocal *, MirValue *> savedLocals = Move(_locals);
    List<LoopTargets> savedLoops = Move(_loops);
    MirBlock *savedBlock = _builder.GetBlock();
    const MirLocation savedLocation = _builder.GetLocation();
    _locals = {};
    _loops = {};
    _current = result;
    _currentSema = function;
    const bool savedUnchecked = _unchecked;
    const bool savedPrecise = _builder.IsPrecise();
    _unchecked = Sema::FindAttribute(function->Symbol->Decl->Attributes, "Unchecked") != nullptr;
    _builder.SetPrecise(Sema::FindAttribute(function->Symbol->Decl->Attributes, "Precise") != nullptr);
    SetLocation(function->Symbol->Decl->NameRange);
    _builder.SetInsertPoint(result->AddBlock("entry"));

    usize argument = 0;
    if (isLambda)
    {
        _outerEnvironment = result->GetArguments()[argument++];
    }
    else if (hasThis)
    {
        MirArgument *self = result->GetArguments()[argument++];
        self->SetName("this");
        _locals.Add(function->This, valueThis ? Spill(self, "this") : static_cast<MirValue *>(self));
    }
    const Environment *environment = InScript() ? PrepareEnvironment(function) : nullptr;
    if (environment != nullptr)
    {
        _environment = _builder.New(environment->Reference, environment->ScriptType);
        if (environment->Parent != ~0u)
        {
            const MirType *parentType = environment->Layout->GetFields()[environment->Parent].Type;
            _builder.Store(_builder.FieldAddress(_environment, parentType, environment->Parent, environment->ScriptType),
                           _outerEnvironment);
        }
        if (function->This != nullptr && environment->Fields.Find(function->This) != nullptr)
        {
            _builder.Store(CapturedAddress(function->This), _builder.Load(*_locals.Find(function->This)));
        }
    }
    for (const SemaLocal *parameter : function->Parameters)
    {
        if (parameter->Type->Is(SemaTypeKind::MeshOutput))
        {
            continue;
        }
        MirArgument *value = result->GetArguments()[argument++];
        value->SetName(parameter->Name);
        if (parameter->Mode == SemaParameterMode::Ref || parameter->Mode == SemaParameterMode::Out)
        {
            _locals.Add(parameter, value);
        }
        else if (environment != nullptr && environment->Fields.Find(parameter) != nullptr)
        {
            MirValue *field = CapturedAddress(parameter);
            _builder.Store(field, value);
            _locals.Add(parameter, field);
        }
        else
        {
            // A [NoDiff] parameter is not differentiated, also where the function is inlined into one that is.
            MirValue *incoming = value;
            if (!InScript() && Sema::IsNoDiff(parameter->Attributes) && _sema->IsDifferentiable(parameter->Type))
            {
                MirValue *operands[] = {value};
                incoming = _builder.Intrinsic(MirIntrinsic::Detach, value->GetType(), operands);
            }
            MirValue *spilled = Spill(incoming, parameter->Name);
            _locals.Add(parameter, spilled);
            DeclareForDebugger(spilled, parameter->Name, parameter->Type, parameter->Range);
        }
    }
    Statement(function->Body);
    if (!_builder.IsTerminated())
    {
        const bool returnsNothing = isClassConstructor || function->Kind == SemaFunctionKind::Iterator ||
                                    (function->Kind == SemaFunctionKind::Async
                                         ? function->ResultType->Is(SemaTypeKind::Void)
                                         : function->ReturnType->Is(SemaTypeKind::Void));
        if (returnsNothing)
        {
            _builder.Return();
        }
        else
        {
            _builder.Terminate(MirOp::Unreachable);
        }
    }
    _environment = savedEnvironment;
    _outerEnvironment = savedOuterEnvironment;

    _current = savedCurrent;
    _currentSema = savedSema;
    _unchecked = savedUnchecked;
    _builder.SetPrecise(savedPrecise);
    _builder.SetLocation(savedLocation);
    _locals = Move(savedLocals);
    _loops = Move(savedLoops);
    if (savedBlock != nullptr)
    {
        _builder.SetInsertPoint(savedBlock);
    }
    // Calls to the function stay calls until differentiation has used its custom derivatives (§9.12).
    if (!InScript() && (function->ForwardDerivative != nullptr || function->ForwardDerivative2 != nullptr ||
                        function->BackwardDerivative != nullptr))
    {
        result->DerivativeArguments = function->DerivativeParameters;
        result->ForwardDerivative =
            function->ForwardDerivative != nullptr ? LowerFunction(function->ForwardDerivative) : nullptr;
        result->ForwardDerivative2 =
            function->ForwardDerivative2 != nullptr ? LowerFunction(function->ForwardDerivative2) : nullptr;
        result->BackwardDerivative =
            function->BackwardDerivative != nullptr ? LowerFunction(function->BackwardDerivative) : nullptr;
    }
    return result;
}

void Lowering::Block(Span<BoundStmt *> statements)
{
    for (BoundStmt *statement : statements)
    {
        if (_builder.IsTerminated())
        {
            return;
        }
        Statement(statement);
    }
}

void Lowering::Statement(BoundStmt *statement)
{
    if (statement == nullptr)
    {
        return;
    }
    SetLocation(statement->Range);
    switch (statement->Kind)
    {
    case BoundStmtKind::Block:
        Block(statement->As<BoundBlock>()->Statements);
        break;
    case BoundStmtKind::Local:
    {
        auto *local = statement->As<BoundLocalStmt>();
        if (InScript() && local->Local->IsCaptured && local->Local->Function == _currentSema)
        {
            MirValue *field = CapturedAddress(local->Local);
            _locals.Set(local->Local, field);
            _builder.Store(field, local->Initializer != nullptr ? Value(local->Initializer)
                                                                : _module->GetNull(LowerType(local->Local->Type)));
            break;
        }
        MirInstruction *variable = _builder.Variable(LowerType(local->Local->Type), local->Local->Name);
        _locals.Set(local->Local, variable);
        if (local->Initializer != nullptr && local->Local->Type->Is(SemaTypeKind::HitObject))
        {
            HitObjectInto(local->Initializer, variable);
        }
        else if (local->Initializer != nullptr)
        {
            const bool precise = _builder.IsPrecise();
            _builder.SetPrecise(precise || Sema::FindAttribute(local->Local->Attributes, "Precise") != nullptr);
            MirValue *value = Value(local->Initializer);
            _builder.SetPrecise(precise);
            _builder.Store(variable, value);
            DeclareForDebugger(variable, local->Local->Name, local->Local->Type, local->Local->Range);
        }
        else if (InScript())
        {
            _builder.Store(variable, _module->GetNull(LowerType(local->Local->Type)));
        }
        break;
    }
    case BoundStmtKind::Expression:
        Value(statement->As<BoundExprStmt>()->Expression);
        break;
    case BoundStmtKind::If:
    {
        auto *branch = statement->As<BoundIf>();
        MirValue *condition = Value(branch->Condition);
        MirBlock *header = _builder.GetBlock();
        MirBlock *thenBlock = NewBlock("if.then");
        MirBlock *elseBlock = branch->Else != nullptr ? NewBlock("if.else") : nullptr;
        MirBlock *merge = NewBlock("if.merge");
        header->Merge = {MirMergeKind::Selection, merge, nullptr};
        _builder.CondBranch(condition, thenBlock, elseBlock != nullptr ? elseBlock : merge);
        _builder.SetInsertPoint(thenBlock);
        Statement(branch->Then);
        if (!_builder.IsTerminated())
        {
            _builder.Branch(merge);
        }
        if (elseBlock != nullptr)
        {
            _builder.SetInsertPoint(elseBlock);
            Statement(branch->Else);
            if (!_builder.IsTerminated())
            {
                _builder.Branch(merge);
            }
        }
        _current->MoveBlock(merge, _current->GetBlocks().Count() - 1);
        _builder.SetInsertPoint(merge);
        break;
    }
    case BoundStmtKind::Loop:
    {
        auto *loop = statement->As<BoundLoop>();
        for (BoundStmt *initializer : loop->Initializers)
        {
            Statement(initializer);
        }
        MirBlock *header = NewBlock("loop.header");
        MirBlock *check = loop->TestFirst && loop->Condition != nullptr ? NewBlock("loop.check") : nullptr;
        MirBlock *body = NewBlock("loop.body");
        MirBlock *latch = NewBlock("loop.continue");
        MirBlock *merge = NewBlock("loop.merge");
        _builder.Branch(header);
        header->Merge = {MirMergeKind::Loop, merge, latch, loop->Control, loop->UnrollCount, loop->MaxIterations};
        // The header's branch stands for the loop, as where problems with the loop are reported.
        SetLocation(statement->Range);
        _builder.SetInsertPoint(header);
        BeginIteration(loop->Iteration);
        _builder.Branch(check != nullptr ? check : body);
        if (check != nullptr)
        {
            _builder.SetInsertPoint(check);
            MirValue *condition = Value(loop->Condition);
            _builder.CondBranch(condition, body, merge);
        }
        _loops.Add({merge, latch});
        _builder.SetInsertPoint(body);
        Statement(loop->Body);
        if (!_builder.IsTerminated())
        {
            _builder.Branch(latch);
        }
        _loops.Pop();
        _current->MoveBlock(latch, _current->GetBlocks().Count() - 1);
        _builder.SetInsertPoint(latch);
        for (BoundExpr *increment : loop->Increments)
        {
            Value(increment);
        }
        if (!loop->TestFirst && loop->Condition != nullptr)
        {
            _builder.CondBranch(Value(loop->Condition), header, merge);
        }
        else
        {
            _builder.Branch(header);
        }
        _current->MoveBlock(merge, _current->GetBlocks().Count() - 1);
        _builder.SetInsertPoint(merge);
        break;
    }
    case BoundStmtKind::Switch:
    {
        auto *node = statement->As<BoundSwitch>();
        if (node->Input != nullptr)
        {
            PatternSwitch(node);
            break;
        }
        MirValue *selector = Value(node->Selector);
        MirBlock *header = _builder.GetBlock();
        MirBlock *merge = NewBlock("switch.merge");
        MirBlock *defaultBlock = merge;
        List<int64> cases;
        List<MirBlock *> targets;
        List<MirBlock *> sectionBlocks;
        for (const BoundSwitchSection &section : node->Sections)
        {
            MirBlock *block = NewBlock(section.IsDefault ? StringView("switch.default") : StringView("switch.case"));
            sectionBlocks.Add(block);
            for (const int64 value : section.Values)
            {
                cases.Add(value);
                targets.Add(block);
            }
            if (section.IsDefault)
            {
                defaultBlock = block;
            }
        }
        header->Merge = {MirMergeKind::Selection, merge, nullptr};
        _builder.Switch(selector, defaultBlock, cases.AsSpan(), targets.AsSpan());
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
        break;
    }
    case BoundStmtKind::Break:
        _builder.Branch(_loops.Last().Break);
        break;
    case BoundStmtKind::Continue:
        for (usize i = _loops.Count(); i-- > 0;)
        {
            if (_loops[i].Continue != nullptr)
            {
                _builder.Branch(_loops[i].Continue);
                break;
            }
        }
        break;
    case BoundStmtKind::Return:
    {
        auto *ret = statement->As<BoundReturn>();
        _builder.Return(ret->Value != nullptr ? Value(ret->Value) : nullptr);
        break;
    }
    case BoundStmtKind::Discard:
        _builder.Terminate(MirOp::Discard);
        break;
    case BoundStmtKind::Yield:
    {
        auto *yield = statement->As<BoundYield>();
        if (yield->Value == nullptr)
        {
            _builder.Return();
            break;
        }
        _builder.Yield(Value(yield->Value));
        break;
    }
    }
}

MirValue *Lowering::Arithmetic(MirOp op, MirValue *left, MirValue *right)
{
    if (right == nullptr)
    {
        return _builder.Unary(op, left);
    }
    if (op >= MirOp::Equal && op <= MirOp::GreaterEqual)
    {
        return _builder.Compare(op, left, right);
    }
    return _builder.Binary(op, left, right);
}

MirValue *Lowering::ConvertValue(MirValue *value, const SemaType *from, const SemaType *to)
{
    if (from == to || from->Is(SemaTypeKind::Null))
    {
        return from->Is(SemaTypeKind::Null) ? _module->GetNull(LowerType(to)) : value;
    }
    from = UnderlyingOf(from);
    to = UnderlyingOf(to);
    const MirType *target = LowerType(to);
    if (value->GetType() == target)
    {
        return value;
    }
    if (from->IsScalar() && (to->Is(SemaTypeKind::Vector) || to->Is(SemaTypeKind::Matrix)))
    {
        return Splat(ConvertValue(value, from, to->GetScalar()), target);
    }
    const SemaType *fromScalar = from->GetScalar();
    const SemaType *toScalar = to->GetScalar();
    if (toScalar->Is(SemaTypeKind::Bool))
    {
        MirConstant *zero = fromScalar->Is(SemaTypeKind::Float) ? _module->GetFloat(LowerType(fromScalar), 0)
                                                                : _module->GetInt(LowerType(fromScalar), 0);
        return _builder.Compare(MirOp::NotEqual, value, from->IsScalar() ? zero : Splat(zero, value->GetType()));
    }
    if (fromScalar->Is(SemaTypeKind::Bool))
    {
        const MirType *scalarType = LowerType(toScalar);
        MirConstant *one = toScalar->Is(SemaTypeKind::Float) ? _module->GetFloat(scalarType, 1) : _module->GetInt(scalarType, 1);
        MirConstant *zero = _module->GetNull(scalarType);
        if (to->IsScalar())
        {
            return _builder.Select(value, one, zero);
        }
        return _builder.Select(value, Splat(one, target), Splat(zero, target));
    }
    return _builder.Convert(value, target);
}

MirValue *Lowering::IndexValue(BoundIndex *node)
{
    MirValue *index = Value(node->Index);
    if (_unchecked || node->InBounds || node->Index->Kind == BoundKind::Constant)
    {
        return index;
    }
    const SemaType *objectType = node->Object->Type;
    const uint32 count = objectType->Is(SemaTypeKind::Matrix) ? objectType->GetRows() : objectType->GetCount();
    const SemaType *uintType = _sema->Types.GetInt(32, false);
    const MirType *type = LowerType(uintType);
    MirValue *operands[] = {ConvertValue(index, node->Index->Type, uintType), _module->GetInt(type, count - 1)};
    return _builder.Intrinsic(MirIntrinsic::Min, type, operands);
}

bool Lowering::IsPreciseTarget(const BoundExpr *target)
{
    while (true)
    {
        switch (target->Kind)
        {
        case BoundKind::Local:
            return Sema::FindAttribute(target->As<BoundLocal>()->Local->Attributes, "Precise") != nullptr;
        case BoundKind::Field:
            target = target->As<BoundField>()->Object;
            break;
        case BoundKind::Index:
            target = target->As<BoundIndex>()->Object;
            break;
        case BoundKind::Swizzle:
            target = target->As<BoundSwizzle>()->Object;
            break;
        default:
            return false;
        }
    }
}

MirValue *Lowering::Address(BoundExpr *expression)
{
    switch (expression->Kind)
    {
    case BoundKind::Local:
    {
        const SemaLocal *local = expression->As<BoundLocal>()->Local;
        if (MirValue **found = _locals.Find(local))
        {
            return *found;
        }
        if (InScript() && local->IsCaptured)
        {
            return CapturedAddress(local);
        }
        // A pattern variable is first assigned where its pattern matches, without a declaration statement.
        MirInstruction *variable = _builder.Variable(LowerType(local->Type), local->Name);
        _locals.Set(local, variable);
        return variable;
    }
    case BoundKind::Global:
        return LowerGlobal(expression->As<BoundGlobal>()->Global);
    case BoundKind::Field:
    {
        auto *field = expression->As<BoundField>();
        if (const List<MirGlobal *> *outputs = MeshOutputOf(field->Object))
        {
            MirValue *index[] = {IndexValue(field->Object->As<BoundIndex>())};
            return _builder.AccessChain((*outputs)[field->FieldIndex], index);
        }
        MirValue *object = field->Object->IsLValue ? Address(field->Object) : Spill(Value(field->Object));
        MirValue *index[] = {_module->GetInt(_module->GetTypes().GetInt(32, false), field->FieldIndex)};
        return _builder.AccessChain(object, index);
    }
    case BoundKind::Index:
    {
        auto *node = expression->As<BoundIndex>();
        if (const List<MirGlobal *> *outputs = MeshOutputOf(node))
        {
            MirValue *index[] = {IndexValue(node)};
            return _builder.AccessChain((*outputs)[0], index);
        }
        MirValue *object = node->Object->IsLValue ? Address(node->Object) : Spill(Value(node->Object));
        MirValue *index[] = {IndexValue(node)};
        return _builder.AccessChain(object, index);
    }
    case BoundKind::Swizzle:
    {
        auto *swizzle = expression->As<BoundSwizzle>();
        if (swizzle->Count == 1 && swizzle->Object->Type->Is(SemaTypeKind::Vector))
        {
            MirValue *object = swizzle->Object->IsLValue ? Address(swizzle->Object) : Spill(Value(swizzle->Object));
            MirValue *index[] = {_module->GetInt(_module->GetTypes().GetInt(32, false), swizzle->Components[0])};
            return _builder.AccessChain(object, index);
        }
        return Spill(Value(expression));
    }
    case BoundKind::BufferElement:
    {
        auto *element = expression->As<BoundBufferElement>();
        MirValue *buffer = Value(element->Buffer);
        MirInstruction *pointer = _builder.BufferPointer(buffer);
        pointer->Restrict = IsNoAlias(element->Buffer);
        MirValue *position = Value(element->Index);
        if (element->Length != nullptr)
        {
            // A Span clamps its index to the last element (§4.3, §4.5).
            const MirType *u32 = _module->GetTypes().GetInt(32, false);
            MirValue *length = Value(element->Length);
            MirValue *index = position->GetType() == u32 ? position : _builder.Convert(position, u32);
            if (_sema->Options->DebugOutput && _sema->Options->CheckSpans)
            {
                CheckSpanAccess(element, buffer, index, length);
            }
            MirValue *operands[] = {index, _builder.Binary(MirOp::Sub, length, _module->GetInt(u32, 1))};
            position = _builder.Intrinsic(MirIntrinsic::Min, u32, operands);
        }
        MirValue *index[] = {position};
        MirInstruction *chain = _builder.AccessChain(pointer, index);
        return chain;
    }
    default:
        if (MirValue *address = InScript() ? ScriptAddress(expression) : nullptr)
        {
            return address;
        }
        return Spill(Value(expression));
    }
}

const List<MirGlobal *> *Lowering::MeshOutputOf(const BoundExpr *expression) const
{
    if (expression->Kind != BoundKind::Index)
    {
        return nullptr;
    }
    const BoundExpr *object = expression->As<BoundIndex>()->Object;
    return object->Kind == BoundKind::Local ? _meshOutputs.Find(object->As<BoundLocal>()->Local) : nullptr;
}

void Lowering::Assign(BoundExpr *target, MirValue *value)
{
    if (const List<MirGlobal *> *outputs = MeshOutputOf(target);
        outputs != nullptr && target->Type->Is(SemaTypeKind::Struct))
    {
        MirValue *index[] = {IndexValue(target->As<BoundIndex>())};
        for (usize i = 0; i < outputs->Count(); ++i)
        {
            const uint32 field[] = {static_cast<uint32>(i)};
            _builder.Store(_builder.AccessChain((*outputs)[i], index), _builder.Extract(value, field));
        }
        return;
    }
    if (target->Kind == BoundKind::Swizzle && target->As<BoundSwizzle>()->Count > 1)
    {
        auto *swizzle = target->As<BoundSwizzle>();
        MirValue *pointer = Address(swizzle->Object);
        MirValue *vector = _builder.Load(pointer);
        const uint32 count = swizzle->Object->Type->GetCount();
        uint32 components[4] = {0, 1, 2, 3};
        for (uint8 i = 0; i < swizzle->Count; ++i)
        {
            components[swizzle->Components[i]] = count + i;
        }
        _builder.Store(pointer, _builder.Shuffle(vector, value, Span<const uint32>(components, count)));
        return;
    }
    if (target->Kind == BoundKind::Property)
    {
        auto *property = target->As<BoundProperty>();
        MirValue *receiver = nullptr;
        List<MirValue *> indices;
        PropertyOperands(property, receiver, indices);
        WriteProperty(property, receiver, indices.AsSpan(), value);
        return;
    }
    _builder.Store(Address(target), value);
}

MirValue *Lowering::Conditional(BoundExpr *condition, BoundExpr *whenTrue, BoundExpr *whenFalse, const SemaType *type)
{
    MirValue *test = Value(condition);
    MirBlock *header = _builder.GetBlock();
    MirBlock *thenBlock = NewBlock("cond.true");
    MirBlock *elseBlock = NewBlock("cond.false");
    MirBlock *merge = NewBlock("cond.merge");
    header->Merge = {MirMergeKind::Selection, merge, nullptr};
    _builder.CondBranch(test, thenBlock, elseBlock);
    _builder.SetInsertPoint(thenBlock);
    MirValue *a = Value(whenTrue);
    MirBlock *thenEnd = _builder.GetBlock();
    _builder.Branch(merge);
    _builder.SetInsertPoint(elseBlock);
    MirValue *b = Value(whenFalse);
    MirBlock *elseEnd = _builder.GetBlock();
    _builder.Branch(merge);
    _current->MoveBlock(merge, _current->GetBlocks().Count() - 1);
    _builder.SetInsertPoint(merge);
    MirInstruction *phi = _builder.Phi(LowerType(type));
    phi->AddOperand(a);
    phi->AddTarget(thenEnd);
    phi->AddOperand(b);
    phi->AddTarget(elseEnd);
    return phi;
}

MirValue *Lowering::Value(BoundExpr *expression)
{
    if (MirValue **combined = _combined.Find(expression))
    {
        return *combined;
    }
    const MirLocation enclosing = _builder.GetLocation();
    SetLocation(expression->Range);
    MirValue *value = LowerValue(expression);
    _builder.SetLocation(enclosing);
    return value;
}

MirValue *Lowering::LowerValue(BoundExpr *expression)
{
    switch (expression->Kind)
    {
    case BoundKind::Constant:
    {
        const SemaConstant *constant = expression->As<BoundConstant>()->Value;
        if (InScript() && expression->Type->Is(SemaTypeKind::String) && !constant->IsZero)
        {
            return _builder.StringConstant(LowerType(expression->Type), _module->AddString(constant->Text));
        }
        return LowerConstant(constant, expression->Type);
    }
    case BoundKind::Zero:
        return _module->GetNull(LowerType(expression->Type));
    case BoundKind::Local:
    case BoundKind::Global:
        return _builder.Load(Address(expression));
    case BoundKind::Field:
    {
        auto *field = expression->As<BoundField>();
        if (field->Object->IsLValue)
        {
            return _builder.Load(Address(expression));
        }
        const uint32 index[] = {field->FieldIndex};
        return _builder.Extract(Value(field->Object), index);
    }
    case BoundKind::Swizzle:
    {
        auto *swizzle = expression->As<BoundSwizzle>();
        MirValue *object = Value(swizzle->Object);
        if (swizzle->Object->Type->IsScalar())
        {
            return swizzle->Count == 1 ? object : Splat(object, LowerType(expression->Type));
        }
        if (swizzle->Count == 1)
        {
            const uint32 index[] = {swizzle->Components[0]};
            return _builder.Extract(object, index);
        }
        uint32 components[4];
        for (uint8 i = 0; i < swizzle->Count; ++i)
        {
            components[i] = swizzle->Components[i];
        }
        return _builder.Shuffle(object, object, Span<const uint32>(components, swizzle->Count));
    }
    case BoundKind::Index:
    {
        auto *node = expression->As<BoundIndex>();
        const SemaType *objectType = node->Object->Type;
        if (const List<MirGlobal *> *outputs = MeshOutputOf(node);
            outputs != nullptr && node->Type->Is(SemaTypeKind::Struct))
        {
            MirValue *index[] = {IndexValue(node)};
            List<MirValue *> fields;
            for (MirGlobal *output : *outputs)
            {
                fields.Add(_builder.Load(_builder.AccessChain(output, index)));
            }
            return _builder.Construct(LowerType(node->Type), fields.AsSpan());
        }
        if (node->Index->Kind == BoundKind::Constant && !node->Object->IsLValue)
        {
            const uint32 index[] = {static_cast<uint32>(node->Index->As<BoundConstant>()->Value->Bits)};
            return _builder.Extract(Value(node->Object), index);
        }
        if (objectType->Is(SemaTypeKind::Vector) && !node->Object->IsLValue)
        {
            MirValue *object = Value(node->Object);
            return _builder.ExtractDynamic(object, IndexValue(node));
        }
        return _builder.Load(Address(expression));
    }
    case BoundKind::BufferElement:
        return _builder.Load(Address(expression));
    case BoundKind::Call:
        return Call(expression->As<BoundCall>());
    case BoundKind::Intrinsic:
        return Intrinsic(expression->As<BoundIntrinsic>());
    case BoundKind::Derivative:
    {
        auto *node = expression->As<BoundDerivative>();
        List<MirValue *> operands;
        operands.Add(LowerFunction(node->Function));
        _pending.Add(node->Function);
        // An instance method's `this` comes first, as in its calls; it is never a pair, so the masks move past it.
        uint32 shift = 0;
        if (node->Receiver != nullptr)
        {
            const SemaLocal *self = node->Function->This;
            const bool valueThis = self != nullptr && self->Mode == SemaParameterMode::Value;
            MirValue *receiver = valueThis ? Value(node->Receiver)
                                 : node->Receiver->IsLValue && node->Receiver->Kind != BoundKind::BufferElement
                                     ? Address(node->Receiver)
                                     : Spill(Value(node->Receiver));
            operands.Add(receiver);
            shift = 1;
        }
        for (usize i = 0; i < node->Arguments.Count(); ++i)
        {
            const bool byReference = i < 32 && (node->RefMask & (1u << i)) != 0;
            operands.Add(byReference ? Address(node->Arguments[i]) : Value(node->Arguments[i]));
        }
        // RECOVERY-DECISION lang-ir-D1: literals are PairMask then RefMask, then the forward directions.
        const int64 literals[] = {static_cast<int64>(node->PairMask) << shift,
                                  static_cast<int64>(node->RefMask) << shift, node->TangentCount};
        return _builder.Intrinsic(node->Forward ? MirIntrinsic::ForwardDerivative : MirIntrinsic::BackwardDerivative,
                                  LowerType(node->Type), operands.AsSpan(), literals);
    }
    case BoundKind::Construct:
    {
        List<MirValue *> elements;
        for (BoundExpr *element : expression->As<BoundConstruct>()->Elements)
        {
            elements.Add(Value(element));
        }
        const MirType *type = LowerType(expression->Type);
        if (type->Is(MirTypeKind::CoopVector) || type->Is(MirTypeKind::CoopMatrix))
        {
            // A cooperative value from one scalar: a splat constant, or one constituent per element of a vector and
            // the single constituent a cooperative matrix takes.
            if (elements[0]->GetValueKind() == MirValueKind::Constant)
            {
                return _module->GetSplat(type, static_cast<MirConstant *>(elements[0]));
            }
            if (type->Is(MirTypeKind::CoopVector))
            {
                elements.Resize(type->GetCount(), elements[0]);
            }
        }
        return _builder.Construct(type, elements.AsSpan());
    }
    case BoundKind::Convert:
    {
        auto *convert = expression->As<BoundConvert>();
        return ConvertValue(Value(convert->Operand), convert->Operand->Type, expression->Type);
    }
    case BoundKind::Bitcast:
        return _builder.Bitcast(Value(expression->As<BoundBitcast>()->Operand), LowerType(expression->Type));
    case BoundKind::Unary:
    {
        auto *unary = expression->As<BoundUnary>();
        MirValue *result = Arithmetic(unary->Op, Value(unary->Operand), nullptr);
        if (unary->Checked && result->GetValueKind() == MirValueKind::Instruction)
        {
            static_cast<MirInstruction *>(result)->Checked = true;
        }
        return result;
    }
    case BoundKind::Binary:
    {
        auto *binary = expression->As<BoundBinary>();
        MirValue *left = Value(binary->Left);
        MirValue *right = Value(binary->Right);
        const SemaType *leftType = binary->Left->Type;
        const SemaType *rightType = binary->Right->Type;
        const bool leftNullable = leftType->Is(SemaTypeKind::Nullable) && !leftType->GetElement()->IsReference();
        const bool rightNullable = rightType->Is(SemaTypeKind::Nullable) && !rightType->GetElement()->IsReference();
        if (leftNullable || rightNullable)
        {
            // `T?` of a value type compares by HasValue, then by Value when both have one (MarlinScript §3.6).
            const uint32 valueField[] = {0};
            const uint32 hasField[] = {1};
            MirValue *leftHas = leftNullable ? static_cast<MirValue *>(_builder.Extract(left, hasField))
                                             : _module->GetBool(true);
            MirValue *rightHas = rightNullable ? static_cast<MirValue *>(_builder.Extract(right, hasField))
                                               : _module->GetBool(true);
            MirValue *sameHas = _builder.Compare(MirOp::Equal, leftHas, rightHas);
            MirValue *result = sameHas;
            const bool leftZero = binary->Left->Kind == BoundKind::Zero;
            const bool rightZero = binary->Right->Kind == BoundKind::Zero;
            if (!leftZero && !rightZero)
            {
                MirValue *a = leftNullable ? _builder.Extract(left, valueField) : left;
                MirValue *b = rightNullable ? _builder.Extract(right, valueField) : right;
                MirValue *same = _builder.Compare(MirOp::Equal, a, b);
                MirValue *either = _builder.Binary(MirOp::LogicalOr, _builder.Unary(MirOp::LogicalNot, leftHas), same);
                result = _builder.Binary(MirOp::LogicalAnd, sameHas, either);
            }
            return binary->Op == MirOp::NotEqual ? _builder.Unary(MirOp::LogicalNot, result) : result;
        }
        if (left->GetType()->Is(MirTypeKind::Reference) && right->GetType() != left->GetType())
        {
            right = _builder.Bitcast(right, left->GetType());
        }
        switch (binary->Op)
        {
        case MirOp::MatrixTimesVector:
        case MirOp::VectorTimesMatrix:
        case MirOp::MatrixTimesMatrix:
        case MirOp::MatrixTimesScalar:
            return _builder.Binary(binary->Op, left, right);
        default:
        {
            MirValue *result = Arithmetic(binary->Op, left, right);
            if (binary->Checked && result->GetValueKind() == MirValueKind::Instruction)
            {
                static_cast<MirInstruction *>(result)->Checked = true;
            }
            return result;
        }
        }
    }
    case BoundKind::ShortCircuit:
    {
        auto *node = expression->As<BoundShortCircuit>();
        MirValue *left = Value(node->Left);
        MirBlock *header = _builder.GetBlock();
        MirBlock *right = NewBlock(node->IsAnd ? StringView("and.right") : StringView("or.right"));
        MirBlock *merge = NewBlock(node->IsAnd ? StringView("and.merge") : StringView("or.merge"));
        header->Merge = {MirMergeKind::Selection, merge, nullptr};
        if (node->IsAnd)
        {
            _builder.CondBranch(left, right, merge);
        }
        else
        {
            _builder.CondBranch(left, merge, right);
        }
        _builder.SetInsertPoint(right);
        MirValue *rightValue = Value(node->Right);
        MirBlock *rightEnd = _builder.GetBlock();
        _builder.Branch(merge);
        _current->MoveBlock(merge, _current->GetBlocks().Count() - 1);
        _builder.SetInsertPoint(merge);
        MirInstruction *phi = _builder.Phi(_module->GetTypes().GetBool());
        phi->AddOperand(_module->GetBool(!node->IsAnd));
        phi->AddTarget(header);
        phi->AddOperand(rightValue);
        phi->AddTarget(rightEnd);
        return phi;
    }
    case BoundKind::Assign:
    {
        auto *assign = expression->As<BoundAssign>();
        if (assign->Target->Type->Is(SemaTypeKind::HitObject))
        {
            HitObjectInto(assign->Value, Address(assign->Target));
            return nullptr;
        }
        if (assign->Target->Kind == BoundKind::Property && (assign->IsCompound || assign->Combined))
        {
            auto *property = assign->Target->As<BoundProperty>();
            MirValue *receiver = nullptr;
            List<MirValue *> indices;
            PropertyOperands(property, receiver, indices);
            MirValue *current = ReadProperty(property, receiver, indices.AsSpan());
            MirValue *stored = nullptr;
            if (assign->Combined)
            {
                _combined.Set(assign->Target, current);
                stored = Value(assign->Value);
                _combined.Remove(assign->Target);
            }
            else
            {
                MirValue *operand = ConvertValue(current, assign->Target->Type, assign->OperationType);
                MirValue *result = Arithmetic(assign->Op, operand, Value(assign->Value));
                stored = ConvertValue(result, assign->OperationType, assign->Target->Type);
            }
            WriteProperty(property, receiver, indices.AsSpan(), stored);
            return stored;
        }
        if (assign->Combined)
        {
            MirValue *pointer = Address(assign->Target);
            _combined.Set(assign->Target, _builder.Load(pointer));
            MirValue *stored = Value(assign->Value);
            _combined.Remove(assign->Target);
            _builder.Store(pointer, stored);
            return stored;
        }
        if (assign->Target->Kind == BoundKind::Property && !assign->IsCompound)
        {
            // RECOVERY-DECISION S12 (foreign, script workspace): a plain assignment to a property or indexer calls
            // its setter, with the receiver and indices evaluated before the value.
            auto *property = assign->Target->As<BoundProperty>();
            MirValue *receiver = nullptr;
            List<MirValue *> indices;
            PropertyOperands(property, receiver, indices);
            MirValue *value = Value(assign->Value);
            WriteProperty(property, receiver, indices.AsSpan(), value);
            return value;
        }
        const bool precise = _builder.IsPrecise();
        _builder.SetPrecise(precise || IsPreciseTarget(assign->Target));
        if (!assign->IsCompound)
        {
            MirValue *value = Value(assign->Value);
            _builder.SetPrecise(precise);
            Assign(assign->Target, value);
            return value;
        }
        MirValue *pointer = Address(assign->Target);
        MirValue *current = _builder.Load(pointer);
        MirValue *operand = ConvertValue(current, assign->Target->Type, assign->OperationType);
        MirValue *result = Arithmetic(assign->Op, operand, Value(assign->Value));
        MirValue *stored = ConvertValue(result, assign->OperationType, assign->Target->Type);
        _builder.SetPrecise(precise);
        _builder.Store(pointer, stored);
        return stored;
    }
    case BoundKind::IncDec:
    {
        auto *node = expression->As<BoundIncDec>();
        if (node->Target->Kind == BoundKind::Property)
        {
            auto *property = node->Target->As<BoundProperty>();
            MirValue *receiver = nullptr;
            List<MirValue *> indices;
            PropertyOperands(property, receiver, indices);
            MirValue *current = ReadProperty(property, receiver, indices.AsSpan());
            const MirType *type = current->GetType();
            MirValue *one = type->Is(MirTypeKind::Float) ? _module->GetFloat(type, 1) : _module->GetInt(type, 1);
            MirValue *next = _builder.Binary(node->Increment ? MirOp::Add : MirOp::Sub, current, one);
            WriteProperty(property, receiver, indices.AsSpan(), next);
            return node->Postfix ? current : next;
        }
        MirValue *pointer = Address(node->Target);
        MirValue *current = _builder.Load(pointer);
        const SemaType *scalar = node->Type->GetScalar();
        const MirType *scalarType = LowerType(scalar);
        MirValue *one = scalar->Is(SemaTypeKind::Float) ? _module->GetFloat(scalarType, 1) : _module->GetInt(scalarType, 1);
        if (!node->Type->IsScalar())
        {
            one = Splat(one, current->GetType());
        }
        MirValue *next = _builder.Binary(node->Increment ? MirOp::Add : MirOp::Sub, current, one);
        _builder.Store(pointer, next);
        return node->Postfix ? current : next;
    }
    case BoundKind::Conditional:
    {
        auto *node = expression->As<BoundConditional>();
        return Conditional(node->Condition, node->WhenTrue, node->WhenFalse, node->Type);
    }
    case BoundKind::Sequence:
    {
        auto *node = expression->As<BoundSequence>();
        MirInstruction *temporary = _builder.Variable(LowerType(node->Type), "with");
        _locals.Set(node->Temporary, temporary);
        _builder.Store(temporary, Value(node->Initial));
        for (BoundExpr *effect : node->Effects)
        {
            Value(effect);
        }
        return _builder.Load(temporary);
    }
    default:
        return ScriptValue(expression);
    }
}

MirValue *Lowering::Call(BoundCall *call)
{
    SemaFunction *function = call->Function;
    if (!_sema->Options->DebugOutput && Sema::FindAttribute(function->Symbol->Decl->Attributes, "DebugOnly") != nullptr)
    {
        return nullptr;
    }
    if (function->Symbol->Kind == SemaSymbolKind::Constructor &&
        (function->ReturnType->Is(SemaTypeKind::MatrixRef) || function->ReturnType->Is(SemaTypeKind::VectorRef)))
    {
        List<MirValue *> fields;
        for (BoundExpr *argument : call->Arguments)
        {
            fields.Add(Value(argument));
        }
        // The view takes a Ptr where the data starts (§9.9); the MIR form keeps a byte offset after it, here zero.
        fields.Insert(1, _module->GetInt(_module->GetTypes().GetInt(32, false), 0));
        return _builder.Construct(LowerType(function->ReturnType), fields.AsSpan());
    }
    _pending.Add(function);
    const bool referenceThis = function->This != nullptr && function->This->Type->IsReference();
    const bool hasThis = function->This != nullptr &&
                         (function->Symbol->Kind != SemaSymbolKind::Constructor || referenceThis);
    const bool valueThis = function->This != nullptr && function->This->Mode == SemaParameterMode::Value;
    List<MirValue *> arguments;
    struct WriteBack
    {
        MirValue *Temporary;
        BoundExpr *Target;
    };
    List<WriteBack> writeBacks;
    auto byReference = [&](BoundExpr *argument, bool copyIn) {
        MirValue *pointer = argument->IsLValue ? Address(argument) : Spill(Value(argument));
        if (pointer->GetType()->GetAddressSpace() == MirAddressSpace::Function &&
            argument->Kind != BoundKind::BufferElement)
        {
            return pointer;
        }
        MirInstruction *temporary = _builder.Variable(pointer->GetType()->GetElement(), "ref");
        if (copyIn)
        {
            _builder.Store(temporary, _builder.Load(pointer));
        }
        writeBacks.Add({temporary, argument});
        return static_cast<MirValue *>(temporary);
    };
    usize index = 0;
    if (hasThis)
    {
        BoundExpr *receiver = call->Arguments[index++];
        arguments.Add(valueThis ? Value(receiver) : byReference(receiver, true));
    }
    for (const SemaLocal *parameter : function->Parameters)
    {
        BoundExpr *argument = call->Arguments[index++];
        if (parameter->Mode == SemaParameterMode::Ref || parameter->Mode == SemaParameterMode::Out)
        {
            arguments.Add(byReference(argument, parameter->Mode == SemaParameterMode::Ref));
        }
        else
        {
            arguments.Add(Value(argument));
        }
    }
    MirValue *result = CallFunction(function, call->Dispatch, arguments.AsSpan());
    for (const WriteBack &writeBack : writeBacks)
    {
        if (writeBack.Target->IsLValue)
        {
            Assign(writeBack.Target, _builder.Load(writeBack.Temporary));
        }
    }
    return result;
}

void Lowering::DebugOutput(BoundIntrinsic *intrinsic)
{
    if (!_sema->Options->DebugOutput)
    {
        return;
    }
    _debugOutput = true;
    const MirType *voidType = _module->GetTypes().GetVoid();
    const Span<BoundExpr *const> arguments = intrinsic->Arguments;
    if (intrinsic->Target.Kind == IntrinsicKind::DebugPrint)
    {
        List<MirValue *> operands;
        for (usize i = 1; i < arguments.Count(); ++i)
        {
            operands.Add(Value(arguments[i]));
        }
        const int64 format[] = {_module->AddString(arguments[0]->As<BoundConstant>()->Value->Text)};
        _builder.Intrinsic(MirIntrinsic::DebugPrint, voidType, operands.AsSpan(), format);
        return;
    }
    String format("assertion failed: ");
    AppendEscapedFormat(format, arguments[1]->As<BoundConstant>()->Value->Text);
    DebugCheck(Value(arguments[0]), intrinsic->Range, format.AsView(), {});
}

void Lowering::CheckSpanAccess(const BoundBufferElement *element, MirValue *buffer, MirValue *index, MirValue *length)
{
    const MirType *u64 = _module->GetTypes().GetInt(64, false);
    MirValue *address = _builder.Bitcast(buffer, u64);
    MirValue *valid = _builder.Binary(MirOp::LogicalAnd, _builder.Compare(MirOp::Less, index, length),
                                      _builder.Compare(MirOp::NotEqual, address, _module->GetInt(u64, 0)));
    String format("assertion failed: `");
    if (const SourceFile *file = _sema->Diagnostics.GetFile(element->Range.File))
    {
        AppendEscapedFormat(format,
                            file->GetText().Substring(element->Range.Begin, element->Range.End - element->Range.Begin));
    }
    format.Append("` reads index {0} of a Span of {1} at {2:x}");
    MirValue *operands[] = {index, length, address};
    DebugCheck(valid, element->Range, format.AsView(), operands);
}

void Lowering::DebugCheck(MirValue *condition, SourceRange range, StringView format, Span<MirValue *const> operands)
{
    _debugOutput = true;
    String text;
    if (const SourceFile *file = _sema->Diagnostics.GetFile(range.File))
    {
        uint32 line = 0;
        uint32 column = 0;
        file->GetLineAndColumn(range.Begin, line, column);
        AppendEscapedFormat(text, file->GetPath());
        AppendEscapedFormat(text, Format(":{0}: ", line).AsView());
    }
    text.Append(format);
    MirBlock *header = _builder.GetBlock();
    MirBlock *failed = NewBlock("assert.failed");
    MirBlock *merge = NewBlock("assert.merge");
    header->Merge = {MirMergeKind::Selection, merge, nullptr};
    _builder.CondBranch(condition, merge, failed);
    _builder.SetInsertPoint(failed);
    const int64 index[] = {_module->AddString(text.AsView())};
    _builder.Intrinsic(MirIntrinsic::DebugPrint, _module->GetTypes().GetVoid(), operands, index);
    _builder.Branch(merge);
    _builder.SetInsertPoint(merge);
}

int64 Lowering::ElementCode(const SemaType *scalar)
{
    const uint32 width = scalar->GetWidth();
    const int64 size = width == 8 ? 0 : width == 16 ? 1 : width == 32 ? 2 : 3;
    return scalar->Is(SemaTypeKind::Float) ? size - 1 : (scalar->IsSigned() ? 3 : 7) + size;
}

void Lowering::HitObjectInto(BoundExpr *value, MirValue *destination)
{
    if (value->Kind == BoundKind::Intrinsic)
    {
        Intrinsic(value->As<BoundIntrinsic>(), destination);
        return;
    }
    _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, value->Range, "a HitObject cannot be copied")
        .WithHelp("make it in place with `HitObject.TraceRay`, `HitObject.MakeMiss`, `HitObject.MakeNop` or "
                  "`query.CommittedHitObject` (§9.8)");
}

MirGlobal *Lowering::StageVariable(MirAddressSpace space, const MirType *type, StringView name)
{
    for (MirGlobal *global : _stageVariables)
    {
        if (global->GetAddressSpace() == space && global->GetValueType() == type)
        {
            UseGlobal(global);
            return global;
        }
    }
    MirGlobal *global = _module->AddGlobal(type, space, name);
    _stageVariables.Add(global);
    UseGlobal(global);
    return global;
}

uint32 Lowering::CountWords(const MirType *type)
{
    switch (type->GetKind())
    {
    case MirTypeKind::Bool:
        return 1;
    case MirTypeKind::Int:
    case MirTypeKind::Float:
        return type->GetWidth() <= 32 ? 1 : ~0u;
    case MirTypeKind::Vector:
    case MirTypeKind::Matrix:
    case MirTypeKind::Array:
    {
        const uint32 element = CountWords(type->GetElement());
        return element == ~0u ? ~0u : element * type->GetCount();
    }
    case MirTypeKind::Struct:
    {
        uint32 total = 0;
        for (const MirStructField &field : type->GetFields())
        {
            const uint32 words = CountWords(field.Type);
            if (words == ~0u)
            {
                return ~0u;
            }
            total += words;
        }
        return total;
    }
    default:
        return ~0u;
    }
}

bool Lowering::AppendWords(MirValue *value, List<MirValue *> &words)
{
    MirTypeContext &types = _module->GetTypes();
    const MirType *type = value->GetType();
    const MirType *word = types.GetInt(32, false);
    switch (type->GetKind())
    {
    case MirTypeKind::Bool:
        words.Add(_builder.Select(value, _module->GetInt(word, 1), _module->GetInt(word, 0)));
        return true;
    case MirTypeKind::Int:
        words.Add(type->GetWidth() == 32 ? (type->IsSigned() ? static_cast<MirValue *>(_builder.Bitcast(value, word))
                                                             : value)
                                         : _builder.Convert(value, word));
        return true;
    case MirTypeKind::Float:
        words.Add(_builder.Bitcast(type->GetWidth() == 32 ? value : _builder.Convert(value, types.GetFloat(32)), word));
        return true;
    case MirTypeKind::Vector:
    case MirTypeKind::Matrix:
    case MirTypeKind::Array:
    case MirTypeKind::Struct:
    {
        const uint32 count = type->Is(MirTypeKind::Struct) ? static_cast<uint32>(type->GetFields().Count())
                                                           : type->GetCount();
        for (uint32 i = 0; i < count; ++i)
        {
            const uint32 index[] = {i};
            if (!AppendWords(_builder.Extract(value, index), words))
            {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

MirValue *Lowering::ValueFromWords(const MirType *type, Span<MirValue *const> words, usize &index)
{
    MirTypeContext &types = _module->GetTypes();
    switch (type->GetKind())
    {
    case MirTypeKind::Bool:
        return _builder.Binary(MirOp::NotEqual, words[index++], _module->GetInt(words[0]->GetType(), 0));
    case MirTypeKind::Int:
    {
        MirValue *w = words[index++];
        return type->GetWidth() == 32 ? (type->IsSigned() ? static_cast<MirValue *>(_builder.Bitcast(w, type)) : w)
                                      : _builder.Convert(w, type);
    }
    case MirTypeKind::Float:
    {
        MirValue *f = _builder.Bitcast(words[index++], types.GetFloat(32));
        return type->GetWidth() == 32 ? f : _builder.Convert(f, type);
    }
    default:
    {
        const uint32 count = type->Is(MirTypeKind::Struct) ? static_cast<uint32>(type->GetFields().Count())
                                                           : type->GetCount();
        List<MirValue *> parts;
        for (uint32 i = 0; i < count; ++i)
        {
            const MirType *part = type->Is(MirTypeKind::Struct) ? type->GetFields()[i].Type : type->GetElement();
            parts.Add(ValueFromWords(part, words, index));
        }
        return _builder.Construct(type, parts.AsSpan());
    }
    }
}

MirValue *Lowering::Intrinsic(BoundIntrinsic *intrinsic, MirValue *destination)
{
    const MirType *resultType = LowerType(intrinsic->Type);
    if (intrinsic->SystemValue != MirBuiltin::None)
    {
        StringBuilder<64> name;
        name.Append(GetMirBuiltinName(intrinsic->SystemValue));
        return _builder.Load(SystemValue(intrinsic->SystemValue, resultType, MirAddressSpace::Input, name.AsView()));
    }
    if (intrinsic->Target.Kind == IntrinsicKind::DebugPrint || intrinsic->Target.Kind == IntrinsicKind::DebugAssert)
    {
        DebugOutput(intrinsic);
        return nullptr;
    }
    if (intrinsic->Function == nullptr && intrinsic->Target.Intrinsic == MirIntrinsic::Detach)
    {
        MirValue *operands[] = {Value(intrinsic->Arguments[0])};
        return _builder.Intrinsic(MirIntrinsic::Detach, resultType, operands);
    }
    if (intrinsic->Target.Kind == IntrinsicKind::Require)
    {
        const auto *message = intrinsic->Arguments[1]->As<BoundConstant>()->Value;
        MirValue *operands[] = {Value(intrinsic->Arguments[0])};
        const int64 literals[] = {_module->AddString(message->Text)};
        _builder.Intrinsic(MirIntrinsic::Require, _module->GetTypes().GetVoid(), operands, literals);
        return nullptr;
    }
    if (intrinsic->Target.Kind == IntrinsicKind::ToWords || intrinsic->Target.Kind == IntrinsicKind::FromWords)
    {
        const bool toWords = intrinsic->Target.Kind == IntrinsicKind::ToWords;
        const MirType *valueType = toWords ? LowerType(intrinsic->Arguments[0]->Type) : resultType;
        const MirType *wordsType = toWords ? resultType : LowerType(intrinsic->Arguments[0]->Type);
        const uint32 count = CountWords(valueType);
        if (count == ~0u || count > wordsType->GetCount())
        {
            _sema->Diagnostics.Error(DiagnosticCode::InvalidOperands, intrinsic->Range,
                                     "`{0}` holds {1}, more than the {2} words of a closure payload",
                                     TypeNameOf(toWords ? intrinsic->Arguments[0]->Type : intrinsic->Type).AsView(),
                                     StringView(count == ~0u ? "values that are not 32-bit scalars or smaller"
                                                             : "too many scalars"),
                                     wordsType->GetCount())
                .WithHelp("keep a custom BSDF's fields to at most that many scalars, vector components included");
            return toWords ? static_cast<MirValue *>(_module->GetNull(resultType)) : _module->GetNull(resultType);
        }
        const MirType *word = wordsType->GetElement();
        if (toWords)
        {
            List<MirValue *> words;
            (void)AppendWords(Value(intrinsic->Arguments[0]), words);
            while (words.Count() < wordsType->GetCount())
            {
                words.Add(_module->GetInt(word, 0));
            }
            return _builder.Construct(wordsType, words.AsSpan());
        }
        MirValue *array = Value(intrinsic->Arguments[0]);
        List<MirValue *> words;
        for (uint32 i = 0; i < count; ++i)
        {
            const uint32 index[] = {i};
            words.Add(_builder.Extract(array, index));
        }
        usize next = 0;
        return ValueFromWords(resultType, words.AsSpan(), next);
    }
    if (intrinsic->Target.Kind == IntrinsicKind::DifferentialAdd ||
        intrinsic->Target.Kind == IntrinsicKind::DifferentialScale)
    {
        const bool add = intrinsic->Target.Kind == IntrinsicKind::DifferentialAdd;
        MirValue *first = Value(intrinsic->Arguments[0]);
        MirValue *second = Value(intrinsic->Arguments[1]);
        return DifferentialArithmetic(intrinsic->Type, first, second, add);
    }
    SemaFunction *function = intrinsic->Function;
    const bool hasReceiver = intrinsic->Arguments.Count() > function->Parameters.Count();
    List<MirValue *> operands;
    for (usize i = 0; i < intrinsic->Arguments.Count(); ++i)
    {
        BoundExpr *argument = intrinsic->Arguments[i];
        const bool isReceiver = hasReceiver && i == 0;
        const SemaLocal *parameter = isReceiver ? nullptr : function->Parameters[i - (hasReceiver ? 1 : 0)];
        const bool byReference =
            (parameter != nullptr &&
             (parameter->Mode == SemaParameterMode::Ref || parameter->Mode == SemaParameterMode::Out)) ||
            (isReceiver && argument->Type->Is(SemaTypeKind::RayQuery)) || argument->Type->Is(SemaTypeKind::HitObject);
        operands.Add(byReference ? Address(argument) : Value(argument));
    }
    const IntrinsicTarget &target = intrinsic->Target;
    switch (target.Kind)
    {
    case IntrinsicKind::Mir:
    {
        switch (target.Intrinsic)
        {
        case MirIntrinsic::CoopVectorLoad:
        case MirIntrinsic::CoopVectorStore:
        case MirIntrinsic::CoopVectorOuterProductAccumulate:
        case MirIntrinsic::CoopVectorReduceSumAccumulate:
        case MirIntrinsic::CoopMatrixLoad:
        case MirIntrinsic::CoopMatrixStore:
            // These take a Ptr<byte> (§9.9); their MIR form has a byte offset after it, here zero.
            for (usize i = 0; i < operands.Count(); ++i)
            {
                if (operands[i]->GetType()->Is(MirTypeKind::Buffer))
                {
                    operands.Insert(i + 1, _module->GetInt(_module->GetTypes().GetInt(32, false), 0));
                    break;
                }
            }
            break;
        default:
            break;
        }
        const bool makesHitObject = intrinsic->Type->Is(SemaTypeKind::HitObject);
        if (makesHitObject)
        {
            if (destination == nullptr)
            {
                destination = _builder.Variable(resultType, "hit");
            }
            operands.Insert(0, destination);
            resultType = _module->GetTypes().GetVoid();
        }
        usize staged = ~usize{0};
        MirAddressSpace space = MirAddressSpace::RayPayload;
        switch (target.Intrinsic)
        {
        case MirIntrinsic::TraceRay:
            staged = 2;
            break;
        case MirIntrinsic::HitObjectTraceRay:
            staged = 3;
            break;
        case MirIntrinsic::HitObjectInvoke:
            staged = 1;
            break;
        case MirIntrinsic::CallShader:
            staged = 1;
            space = MirAddressSpace::CallableData;
            break;
        case MirIntrinsic::ReportHit:
        {
            MirGlobal *attributes = StageVariable(MirAddressSpace::HitAttribute, operands[2]->GetType(), "attributes");
            _builder.Store(attributes, operands[2]);
            operands.Pop();
            break;
        }
        default:
            break;
        }
        MirValue *source = nullptr;
        if (staged != ~usize{0})
        {
            source = operands[staged];
            MirGlobal *variable = StageVariable(space, source->GetType()->GetElement(),
                                                space == MirAddressSpace::RayPayload ? StringView("payload")
                                                                                     : StringView("callableData"));
            _builder.Store(variable, _builder.Load(source));
            operands[staged] = variable;
        }
        List<int64> literals(intrinsic->Literals);
        if ((target.Intrinsic == MirIntrinsic::CoopVectorMatMul || target.Intrinsic == MirIntrinsic::CoopVectorMatMulAdd) &&
            literals.IsEmpty())
        {
            // RECOVERY-DECISION lang-spirv-D5: literal 0 is the matrix element code, literal 1 the bias's.
            literals.Add(ElementCode(intrinsic->Arguments[0]->Type->GetElement()));
            if (target.Intrinsic == MirIntrinsic::CoopVectorMatMulAdd)
            {
                literals.Add(ElementCode(intrinsic->Arguments[2]->Type->GetElement()));
            }
        }
        MirValue *result = _builder.Intrinsic(target.Intrinsic, resultType, operands.AsSpan(), literals.AsSpan());
        if (source != nullptr)
        {
            _builder.Store(source, _builder.Load(operands[staged]));
        }
        return makesHitObject ? destination : result;
    }
    case IntrinsicKind::Op:
        if (target.Op == MirOp::EmitMeshTasks)
        {
            MirGlobal *payload = TaskPayload(operands[3]->GetType());
            _builder.Store(payload, operands[3]);
            MirValue *launch[] = {operands[0], operands[1], operands[2], payload};
            MirValue *end = _builder.Terminate(MirOp::EmitMeshTasks, launch);
            _builder.SetInsertPoint(NewBlock("after.dispatch"));
            return end;
        }
        if (target.Op == MirOp::IgnoreHit || target.Op == MirOp::AcceptHitAndEndSearch)
        {
            MirValue *end = _builder.Terminate(target.Op, {});
            _builder.SetInsertPoint(NewBlock("after.hit"));
            return end;
        }
        return _builder.Create(target.Op, resultType, operands.AsSpan());
    case IntrinsicKind::PtrOffset:
    case IntrinsicKind::PtrAdd:
    case IntrinsicKind::PtrIsNull:
    case IntrinsicKind::PtrEqual:
    case IntrinsicKind::PtrNotEqual:
    case IntrinsicKind::PtrCast:
    {
        // A Ptr is the 64-bit address of its first element (§4.5): arithmetic on it is integer arithmetic.
        MirTypeContext &types = _module->GetTypes();
        const MirType *u64 = types.GetInt(64, false);
        MirValue *address = _builder.Bitcast(operands[0], u64);
        if (target.Kind == IntrinsicKind::PtrIsNull)
        {
            return _builder.Compare(MirOp::Equal, address, _module->GetInt(u64, 0));
        }
        if (target.Kind == IntrinsicKind::PtrEqual || target.Kind == IntrinsicKind::PtrNotEqual)
        {
            return _builder.Compare(target.Kind == IntrinsicKind::PtrEqual ? MirOp::Equal : MirOp::NotEqual, address,
                                    _builder.Bitcast(operands[1], u64));
        }
        if (target.Kind == IntrinsicKind::PtrCast)
        {
            return _builder.Bitcast(operands[0], resultType);
        }
        MirValue *bytes = _builder.Convert(operands[1], u64);
        if (target.Kind == IntrinsicKind::PtrAdd)
        {
            const MirType *element = operands[0]->GetType()->GetElement();
            const uint32 stride = element != nullptr ? types.GetRuntimeArray(element)->GetStride() : 1;
            bytes = _builder.Binary(MirOp::Mul, bytes, _module->GetInt(u64, stride));
        }
        return _builder.Bitcast(_builder.Binary(MirOp::Add, address, bytes), resultType);
    }
    case IntrinsicKind::PtrRead:
    case IntrinsicKind::PtrWrite:
    {
        const bool isStore = target.Kind == IntrinsicKind::PtrWrite;
        const SemaType *valueType = isStore ? intrinsic->Arguments[2]->Type : intrinsic->Type;
        const SemaType *scalar = UnderlyingOf(valueType)->GetScalar();
        if (scalar == nullptr || scalar->GetWidth() != 32)
        {
            _sema->Diagnostics.Error(DiagnosticCode::TypeMismatch, intrinsic->Range,
                                     "a `Ptr<byte>` reads and writes 32-bit scalars and vectors")
                .WithHelp("use a `Ptr<T>` for other types");
            return _module->GetUndef(resultType);
        }
        const MirType *u32 = _module->GetTypes().GetInt(32, false);
        MirValue *words = _builder.BufferPointer(operands[0]);
        MirValue *first = _builder.Binary(MirOp::ShiftRight, operands[1], _module->GetInt(u32, 2));
        const uint32 count = valueType->Is(SemaTypeKind::Vector) ? valueType->GetCount() : 1;
        const MirType *wordsType = count == 1 ? u32 : _module->GetTypes().GetVector(u32, count);
        if (isStore)
        {
            MirValue *bits = _builder.Bitcast(operands[2], wordsType);
            for (uint32 i = 0; i < count; ++i)
            {
                const uint32 component[] = {i};
                MirValue *word = count == 1 ? bits : _builder.Extract(bits, component);
                MirValue *index[] = {i == 0 ? first : _builder.Binary(MirOp::Add, first, _module->GetInt(u32, i))};
                _builder.Store(_builder.AccessChain(words, index), word);
            }
            return nullptr;
        }
        List<MirValue *> loaded;
        for (uint32 i = 0; i < count; ++i)
        {
            MirValue *index[] = {i == 0 ? first : _builder.Binary(MirOp::Add, first, _module->GetInt(u32, i))};
            loaded.Add(_builder.Load(_builder.AccessChain(words, index)));
        }
        MirValue *bits = count == 1 ? loaded[0] : _builder.Construct(wordsType, loaded.AsSpan());
        return _builder.Bitcast(bits, resultType);
    }
    default:
        return _module->GetUndef(resultType);
    }
}

MirValue *Lowering::DifferentialArithmetic(const SemaType *type, MirValue *first, MirValue *second, bool add)
{
    const MirType *lowered = LowerType(type);
    switch (type->GetKind())
    {
    case SemaTypeKind::Float:
    case SemaTypeKind::Vector:
    case SemaTypeKind::Matrix:
    {
        if (add)
        {
            return _builder.Binary(MirOp::Add, first, second);
        }
        const MirType *scalar = type->Is(SemaTypeKind::Float) ? lowered : lowered->GetScalar();
        MirValue *scale = second->GetType() == scalar ? second : _builder.Convert(second, scalar);
        return _builder.Binary(type->Is(SemaTypeKind::Float)    ? MirOp::Mul
                               : type->Is(SemaTypeKind::Vector) ? MirOp::VectorTimesScalar
                                                                : MirOp::MatrixTimesScalar,
                               first, scale);
    }
    case SemaTypeKind::Array:
    case SemaTypeKind::Struct:
    {
        // Field by field, and element by element; the parts without a derivative, [NoDiff] fields among them, are
        // zero in a derivative.
        List<MirValue *> parts;
        const bool isStruct = type->Is(SemaTypeKind::Struct);
        const usize count = isStruct ? type->GetStruct()->Fields.Count() : type->GetCount();
        for (usize i = 0; i < count; ++i)
        {
            const SemaField *field = isStruct ? &type->GetStruct()->Fields[i] : nullptr;
            const SemaType *partType = isStruct ? field->Type : type->GetElement();
            if ((field != nullptr && Sema::IsNoDiff(field->Symbol->Decl->Attributes)) ||
                !_sema->IsDifferentiable(partType))
            {
                parts.Add(_module->GetNull(LowerType(partType)));
                continue;
            }
            const uint32 index[] = {static_cast<uint32>(i)};
            parts.Add(DifferentialArithmetic(partType, _builder.Extract(first, index),
                                             add ? _builder.Extract(second, index) : second, add));
        }
        return _builder.Construct(lowered, parts.AsSpan());
    }
    default:
        return _module->GetNull(lowered);
    }
}

bool Lowering::LowerEntryPoint(SemaFunction *entry, MirStage stage, StringView name, const MirEntryPoint &settings)
{
    _stage = stage;
    MirTypeContext &types = _module->GetTypes();
    MirFunction *wrapper = _module->AddFunction(types.GetFunction(types.GetVoid(), {}), name);
    _current = wrapper;
    _builder.SetInsertPoint(wrapper->AddBlock("entry"));
    SetLocation(entry->Symbol->Decl->NameRange);

    uint32 inputLocation = 0;
    uint32 outputLocation = 0;
    auto interpolationOf = [](Span<AstAttribute> attributes, MirGlobal *global) {
        if (Sema::FindAttribute(attributes, "NoInterpolation") != nullptr)
        {
            global->Interpolation = MirInterpolation::NoInterpolation;
        }
        else if (Sema::FindAttribute(attributes, "Linear") != nullptr ||
                 Sema::FindAttribute(attributes, "NoPerspective") != nullptr)
        {
            global->Interpolation = MirInterpolation::Linear;
        }
        if (Sema::FindAttribute(attributes, "Centroid") != nullptr)
        {
            global->Sampling = MirSampling::Centroid;
        }
        else if (Sema::FindAttribute(attributes, "Sample") != nullptr)
        {
            global->Sampling = MirSampling::Sample;
        }
    };
    struct SystemAttribute
    {
        StringView Attribute;
        MirBuiltin Builtin;
    };
    static constexpr SystemAttribute SystemAttributes[] = {
        {"Position", MirBuiltin::Position},
        {"VertexId", MirBuiltin::VertexIndex},
        {"InstanceId", MirBuiltin::InstanceIndex},
        {"DispatchThreadId", MirBuiltin::DispatchThreadId},
        {"GroupThreadId", MirBuiltin::GroupThreadId},
        {"GroupId", MirBuiltin::GroupId},
        {"GroupIndex", MirBuiltin::GroupIndex},
        {"Depth", MirBuiltin::Depth},
        {"Coverage", MirBuiltin::Coverage},
        {"FrontFace", MirBuiltin::FrontFace},
        {"SampleIndex", MirBuiltin::SampleIndex},
        {"Barycentrics", MirBuiltin::Barycentrics},
        {"PrimitiveId", MirBuiltin::PrimitiveId},
        {"ViewIndex", MirBuiltin::ViewIndex},
        {"ShadingRate", MirBuiltin::ShadingRate},
        {"RenderTargetIndex", MirBuiltin::RenderTargetIndex},
        {"ViewportIndex", MirBuiltin::ViewportIndex},
        {"CullPrimitive", MirBuiltin::CullPrimitive},
    };
    auto systemValueOf = [&](Span<AstAttribute> attributes) {
        for (const SystemAttribute &system : SystemAttributes)
        {
            if (Sema::FindAttribute(attributes, system.Attribute) != nullptr)
            {
                return system.Builtin;
            }
        }
        return MirBuiltin::None;
    };
    auto locationCount = [](const SemaType *type) -> uint32 {
        if (type->Is(SemaTypeKind::Matrix))
        {
            return type->GetRows();
        }
        if (type->Is(SemaTypeKind::Array))
        {
            return type->GetCount() * (type->GetElement()->Is(SemaTypeKind::Matrix) ? type->GetElement()->GetRows() : 1);
        }
        return 1;
    };
    /// One input or output variable for a value or a struct field of the stage interface.
    auto interfaceVariable = [&](const SemaType *type, Span<AstAttribute> attributes, MirAddressSpace space,
                                 StringView variableName) -> MirGlobal * {
        const MirBuiltin builtin = systemValueOf(attributes);
        if (builtin == MirBuiltin::Coverage)
        {
            return SystemValue(builtin, _module->GetTypes().GetArray(LowerType(type), 1), space, variableName);
        }
        if (builtin != MirBuiltin::None)
        {
            return SystemValue(builtin, LowerType(type), space, variableName);
        }
        MirGlobal *global = _module->AddGlobal(LowerType(type), space, variableName);
        uint32 &location = space == MirAddressSpace::Input ? inputLocation : outputLocation;
        if (const AstAttribute *targetAttribute = Sema::FindAttribute(attributes, "Target"))
        {
            SemaConstant *index = _sema->EvaluateAttributeArgument(*targetAttribute, 0, {}, _sema->ContextOf(entry->Symbol));
            global->Location = index != nullptr ? static_cast<uint32>(index->Bits) : 0;
        }
        else
        {
            global->Location = location;
            location += locationCount(type);
        }
        interpolationOf(attributes, global);
        UseGlobal(global);
        return global;
    };
    /// The coverage mask is an array of one word in SPIR-V's SampleMask and DXIL's coverage alike.
    auto element = [&](MirGlobal *global) -> MirValue * {
        if (global->Builtin != MirBuiltin::Coverage)
        {
            return global;
        }
        MirValue *index[] = {_module->GetInt(_module->GetTypes().GetInt(32, false), 0)};
        return _builder.AccessChain(global, index);
    };
    auto readInterface = [&](MirGlobal *global) { return _builder.Load(element(global)); };
    auto writeInterface = [&](MirGlobal *global, MirValue *value) { _builder.Store(element(global), value); };
    auto isStructInterface = [&](const SemaType *type) {
        return type->Is(SemaTypeKind::Struct) && (stage == MirStage::Pixel || stage == MirStage::Vertex);
    };

    /// The output array of a field of the vertices or primitives a mesh shader writes, or of its indices.
    auto meshVariable = [&](const SemaType *type, Span<AstAttribute> attributes, uint32 count, bool perPrimitive,
                            StringView variableName) -> MirGlobal * {
        const MirType *arrayType = types.GetArray(LowerType(type), count);
        const MirBuiltin builtin = systemValueOf(attributes);
        MirGlobal *global = nullptr;
        if (builtin != MirBuiltin::None)
        {
            global = SystemValue(builtin, arrayType, MirAddressSpace::Output, variableName);
        }
        else
        {
            global = _module->AddGlobal(arrayType, MirAddressSpace::Output, variableName);
            global->Location = outputLocation;
            outputLocation += locationCount(type);
            interpolationOf(attributes, global);
            UseGlobal(global);
        }
        global->PerPrimitive = perPrimitive;
        return global;
    };
    for (const SemaLocal *parameter : entry->Parameters)
    {
        if (!parameter->Type->Is(SemaTypeKind::MeshOutput))
        {
            continue;
        }
        const SemaType *element = parameter->Type->GetElement();
        const uint32 count = parameter->Type->GetCount();
        List<MirGlobal *> outputs;
        if (parameter->Type->GetMeshOutput() == SemaMeshOutput::Indices)
        {
            const MirBuiltin builtin = settings.Topology == MirMeshTopology::Triangle ? MirBuiltin::PrimitiveTriangleIndices
                                       : settings.Topology == MirMeshTopology::Line  ? MirBuiltin::PrimitiveLineIndices
                                                                                     : MirBuiltin::PrimitivePointIndices;
            outputs.Add(SystemValue(builtin, types.GetArray(LowerType(element), count), MirAddressSpace::Output,
                                    parameter->Name));
        }
        else
        {
            const bool perPrimitive = parameter->Type->GetMeshOutput() == SemaMeshOutput::Primitives;
            for (const SemaField &field : element->GetStruct()->Fields)
            {
                outputs.Add(meshVariable(field.Type, field.Symbol->Decl->Attributes, count, perPrimitive, field.Name));
            }
        }
        _meshOutputs.Add(parameter, Move(outputs));
    }
    MirFunction *body = LowerFunction(entry);
    for (usize i = 0; i < _pending.Count(); ++i)
    {
        LowerFunction(_pending[i]);
    }
    _current = wrapper;

    const bool rayStage = stage >= MirStage::RayGeneration;
    // The [Uniform] parameters form the entry point's parameter block in declaration order (§9.3); a single one is
    // the block itself, so its fields are what C++ matches. Hit, miss and callable stages read theirs from the
    // shader record.
    List<const SemaLocal *> uniforms;
    for (const SemaLocal *parameter : entry->Parameters)
    {
        if (Sema::FindAttribute(parameter->Attributes, "Uniform") != nullptr)
        {
            if (parameter->Mode == SemaParameterMode::Ref || parameter->Mode == SemaParameterMode::Out)
            {
                _sema->Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, parameter->Range,
                                         "the `[Uniform]` parameter `{0}` is read-only", parameter->Name)
                    .WithHelp("write it as `[Uniform] in T name` (§9.3)");
                return false;
            }
            uniforms.Add(parameter);
        }
    }
    MirValue *uniformBlock = nullptr;
    bool argumentsIndirect = false;
    const bool record = stage == MirStage::ClosestHit || stage == MirStage::AnyHit || stage == MirStage::Miss ||
                        stage == MirStage::Intersection || stage == MirStage::Callable;
    // Debug output goes to a buffer whose address follows the arguments in the Uniform global (§9.11). Whenever
    // debug output is compiled, the arguments keep room for it among those passed directly, so the stages of a
    // pipeline that do and do not print pass their shared arguments alike.
    const bool debugBuffer = _debugOutput && !record;
    const uint32 directSize = DirectArgumentsSize - (_sema->Options->DebugOutput && !record ? sizeof(uint64) : 0);
    // The Uniform global holding *arguments* (null for none) and the debug buffer's address; the arguments' value.
    auto uniformGlobal = [&](const MirType *arguments) -> MirValue * {
        if (!debugBuffer)
        {
            MirGlobal *global = _module->AddGlobal(arguments, MirAddressSpace::Uniform, "Arguments");
            UseGlobal(global);
            return _builder.Load(global);
        }
        List<MirStructField> fields;
        if (arguments != nullptr)
        {
            fields.Add({_module->Intern("Arguments"), arguments, MirTypeContext::AutoOffset});
        }
        fields.Add({_module->Intern("DebugBuffer"), types.GetBuffer(types.GetInt(32, false), true),
                    MirTypeContext::AutoOffset});
        StringBuilder<128> globalName;
        globalName.Append(name).Append(".Uniform");
        MirGlobal *global = _module->AddGlobal(types.CreateStruct(_module->Intern(globalName.AsView()), fields.AsSpan()),
                                               MirAddressSpace::Uniform, "Arguments");
        UseGlobal(global);
        if (arguments == nullptr)
        {
            return nullptr;
        }
        const uint32 first[] = {0};
        return _builder.Extract(_builder.Load(global), first);
    };
    if (!uniforms.IsEmpty())
    {
        const MirType *blockType = nullptr;
        if (uniforms.Count() == 1)
        {
            blockType = LowerType(uniforms[0]->Type);
        }
        else
        {
            List<MirStructField> fields;
            for (const SemaLocal *parameter : uniforms)
            {
                fields.Add({_module->Intern(parameter->Name), LowerType(parameter->Type), MirTypeContext::AutoOffset});
            }
            StringBuilder<128> blockName;
            blockName.Append(name).Append(".Arguments");
            blockType = types.CreateStruct(_module->Intern(blockName.AsView()), fields.AsSpan());
        }
        if (record)
        {
            uniformBlock = _builder.Load(StageVariable(MirAddressSpace::ShaderRecord, blockType, "Arguments"));
        }
        else if (blockType->GetSize() > directSize)
        {
            // Too large to pass directly: the arguments are a Ptr to the block, which ShaderLibrary writes into upload
            // memory (§9.3).
            argumentsIndirect = true;
            MirValue *first[] = {_module->GetInt(types.GetInt(32, false), 0)};
            uniformBlock = _builder.Load(
                _builder.AccessChain(_builder.BufferPointer(uniformGlobal(types.GetBuffer(blockType, false))), first));
        }
        else
        {
            uniformBlock = uniformGlobal(blockType);
        }
    }
    else if (debugBuffer)
    {
        (void)uniformGlobal(nullptr);
    }

    List<Pair<MirValue *, MirGlobal *>> copyBack;
    List<MirValue *> arguments;
    for (const SemaLocal *parameter : entry->Parameters)
    {
        if (uniformBlock != nullptr && Sema::FindAttribute(parameter->Attributes, "Uniform") != nullptr)
        {
            if (uniforms.Count() == 1)
            {
                arguments.Add(uniformBlock);
            }
            else
            {
                uint32 field = 0;
                while (uniforms[field] != parameter)
                {
                    ++field;
                }
                const uint32 index[] = {field};
                arguments.Add(_builder.Extract(uniformBlock, index));
            }
            continue;
        }
        if (parameter->Type->Is(SemaTypeKind::MeshOutput))
        {
            continue;
        }
        if (rayStage && (parameter->Mode == SemaParameterMode::Ref || parameter->Mode == SemaParameterMode::Out))
        {
            const MirType *type = LowerType(parameter->Type);
            MirGlobal *incoming = StageVariable(stage == MirStage::Callable ? MirAddressSpace::IncomingCallableData
                                                                            : MirAddressSpace::IncomingRayPayload,
                                                type, parameter->Name);
            MirInstruction *local = _builder.Variable(type, parameter->Name);
            if (parameter->Mode == SemaParameterMode::Ref)
            {
                _builder.Store(local, _builder.Load(incoming));
            }
            copyBack.Add({local, incoming});
            arguments.Add(local);
            continue;
        }
        if ((stage == MirStage::ClosestHit || stage == MirStage::AnyHit) && parameter->Type->Is(SemaTypeKind::Struct))
        {
            arguments.Add(_builder.Load(StageVariable(MirAddressSpace::HitAttribute, LowerType(parameter->Type),
                                                      parameter->Name)));
            continue;
        }
        if (Sema::FindAttribute(parameter->Attributes, "Payload") != nullptr)
        {
            arguments.Add(_builder.Load(TaskPayload(LowerType(parameter->Type))));
            continue;
        }
        if (systemValueOf(parameter->Attributes) != MirBuiltin::None)
        {
            arguments.Add(readInterface(interfaceVariable(parameter->Type, parameter->Attributes, MirAddressSpace::Input,
                                                          parameter->Name)));
            continue;
        }
        if (isStructInterface(parameter->Type))
        {
            SemaStructInstance *instance = parameter->Type->GetStruct();
            const bool perPrimitive = Sema::FindAttribute(parameter->Attributes, "PerPrimitive") != nullptr;
            List<MirValue *> fields;
            for (const SemaField &field : instance->Fields)
            {
                MirGlobal *input =
                    interfaceVariable(field.Type, field.Symbol->Decl->Attributes, MirAddressSpace::Input, field.Name);
                input->PerPrimitive = perPrimitive;
                fields.Add(readInterface(input));
            }
            arguments.Add(_builder.Construct(LowerType(parameter->Type), fields.AsSpan()));
            continue;
        }
        _sema->Diagnostics
            .Error(DiagnosticCode::InvalidEntryPoint, parameter->Range,
                   "the entry point parameter `{0}` is neither a system value, `[Uniform]` nor a stage input",
                   parameter->Name)
            .WithHelp("mark it with a system value attribute such as `[DispatchThreadId]`, or `[Uniform]` (§9.2, §9.3)");
        return false;
    }
    MirValue *result = _builder.Call(body, arguments.AsSpan());
    for (const auto &[local, incoming] : copyBack)
    {
        _builder.Store(incoming, _builder.Load(local));
    }
    if (!entry->ReturnType->Is(SemaTypeKind::Void))
    {
        const AstFunctionDecl *decl = entry->Symbol->Decl->As<AstFunctionDecl>();
        if (isStructInterface(entry->ReturnType))
        {
            SemaStructInstance *instance = entry->ReturnType->GetStruct();
            for (usize i = 0; i < instance->Fields.Count(); ++i)
            {
                const SemaField &field = instance->Fields[i];
                MirGlobal *output =
                    interfaceVariable(field.Type, field.Symbol->Decl->Attributes, MirAddressSpace::Output, field.Name);
                const uint32 index[] = {static_cast<uint32>(i)};
                writeInterface(output, _builder.Extract(result, index));
            }
        }
        else
        {
            List<AstAttribute> returnAttributes;
            for (const AstAttribute &attribute : decl->Attributes)
            {
                if (attribute.Target == "return")
                {
                    returnAttributes.Add(attribute);
                }
            }
            if (returnAttributes.IsEmpty())
            {
                _sema->Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, decl->NameRange,
                                         "the result of an entry point needs a meaning")
                    .WithHelp("mark it, for example `[return: Target(0)]`, or return a struct whose fields are marked");
                return false;
            }
            writeInterface(interfaceVariable(entry->ReturnType, returnAttributes.AsSpan(), MirAddressSpace::Output, "result"),
                           result);
        }
    }
    _builder.Return();
    MirEntryPoint &entryPoint = _module->AddEntryPoint(name, stage, wrapper);
    entryPoint.GroupSize[0] = settings.GroupSize[0];
    entryPoint.GroupSize[1] = settings.GroupSize[1];
    entryPoint.GroupSize[2] = settings.GroupSize[2];
    entryPoint.MaxVertices = settings.MaxVertices;
    entryPoint.MaxPrimitives = settings.MaxPrimitives;
    entryPoint.Topology = settings.Topology;
    entryPoint.ArgumentsIndirect = argumentsIndirect;
    entryPoint.DebugBuffer = debugBuffer;
    auto addFeature = [&](StringView feature) {
        for (const StringView existing : entryPoint.Features)
        {
            if (existing == feature)
            {
                return;
            }
        }
        entryPoint.Features.Add(_module->Intern(feature));
    };
    for (const StringView feature : settings.Features)
    {
        addFeature(feature);
    }
    for (const StringView feature : entry->Requires)
    {
        addFeature(feature);
    }
    for (const SemaFunction *function : _pending)
    {
        for (const StringView feature : function->Requires)
        {
            addFeature(feature);
        }
    }
    for (MirGlobal *global : _interface)
    {
        entryPoint.Interface.Add(global);
    }
    return true;
}
} // namespace Marlin
