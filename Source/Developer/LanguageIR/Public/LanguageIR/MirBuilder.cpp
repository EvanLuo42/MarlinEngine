#include "LanguageIR/MirBuilder.h"

namespace Marlin
{
const MirType *GetIndexedType(const MirType *type, Span<const uint32> indices)
{
    for (const uint32 index : indices)
    {
        switch (type->GetKind())
        {
        case MirTypeKind::Vector:
            if (index >= type->GetCount())
            {
                return nullptr;
            }
            type = type->GetElement();
            break;
        case MirTypeKind::Matrix:
            if (index >= type->GetRows())
            {
                return nullptr;
            }
            type = type->GetElement();
            break;
        case MirTypeKind::Array:
            if (index >= type->GetCount())
            {
                return nullptr;
            }
            type = type->GetElement();
            break;
        case MirTypeKind::Struct:
            if (index >= type->GetFields().Count())
            {
                return nullptr;
            }
            type = type->GetFields()[index].Type;
            break;
        default:
            return nullptr;
        }
    }
    return type;
}

MirInstruction *MirBuilder::NewInstruction(MirOp op, const MirType *type)
{
    MirInstruction *instruction = _module->GetArena().New<MirInstruction>(op, type, _module->GetArena());
    instruction->Location = _location;
    instruction->Precise = _precise;
    MARLIN_ASSERT(_block != nullptr);
    _block->InsertBefore(instruction, _before);
    return instruction;
}

const MirType *MirBuilder::BoolLike(const MirType *type) const
{
    return type->Is(MirTypeKind::Vector) ? _types->GetVector(_types->GetBool(), type->GetCount()) : _types->GetBool();
}

MirInstruction *MirBuilder::Create(MirOp op, const MirType *type, Span<MirValue *const> operands)
{
    MirInstruction *instruction = NewInstruction(op, type);
    for (MirValue *operand : operands)
    {
        instruction->AddOperand(operand);
    }
    return instruction;
}

MirInstruction *MirBuilder::Binary(MirOp op, MirValue *left, MirValue *right)
{
    const MirType *type = left->GetType();
    switch (op)
    {
    case MirOp::MatrixTimesVector:
        type = _types->GetVector(left->GetType()->GetScalar(), left->GetType()->GetRows());
        break;
    case MirOp::VectorTimesMatrix:
        type = right->GetType()->GetElement();
        break;
    case MirOp::MatrixTimesMatrix:
        type = _types->GetMatrix(left->GetType()->GetScalar(), left->GetType()->GetRows(),
                                 right->GetType()->GetCount());
        break;
    default:
        break;
    }
    MirValue *operands[] = {left, right};
    return Create(op, type, operands);
}

MirInstruction *MirBuilder::Compare(MirOp op, MirValue *left, MirValue *right)
{
    MirValue *operands[] = {left, right};
    return Create(op, BoolLike(left->GetType()), operands);
}

MirInstruction *MirBuilder::Unary(MirOp op, MirValue *operand)
{
    MirValue *operands[] = {operand};
    return Create(op, operand->GetType(), operands);
}

MirInstruction *MirBuilder::Convert(MirValue *value, const MirType *type)
{
    MirValue *operands[] = {value};
    return Create(MirOp::Convert, type, operands);
}

MirInstruction *MirBuilder::Bitcast(MirValue *value, const MirType *type)
{
    MirValue *operands[] = {value};
    return Create(MirOp::Bitcast, type, operands);
}

MirInstruction *MirBuilder::Construct(const MirType *type, Span<MirValue *const> elements)
{
    return Create(MirOp::CompositeConstruct, type, elements);
}

MirInstruction *MirBuilder::Extract(MirValue *composite, Span<const uint32> indices)
{
    const MirType *type = GetIndexedType(composite->GetType(), indices);
    MARLIN_ASSERT(type != nullptr);
    MirValue *operands[] = {composite};
    MirInstruction *instruction = Create(MirOp::CompositeExtract, type, operands);
    for (const uint32 index : indices)
    {
        instruction->AddLiteral(index);
    }
    return instruction;
}

MirInstruction *MirBuilder::Insert(MirValue *composite, MirValue *value, Span<const uint32> indices)
{
    MirValue *operands[] = {composite, value};
    MirInstruction *instruction = Create(MirOp::CompositeInsert, composite->GetType(), operands);
    for (const uint32 index : indices)
    {
        instruction->AddLiteral(index);
    }
    return instruction;
}

MirInstruction *MirBuilder::ExtractDynamic(MirValue *vector, MirValue *index)
{
    MirValue *operands[] = {vector, index};
    return Create(MirOp::VectorExtractDynamic, vector->GetType()->GetElement(), operands);
}

MirInstruction *MirBuilder::InsertDynamic(MirValue *vector, MirValue *value, MirValue *index)
{
    MirValue *operands[] = {vector, value, index};
    return Create(MirOp::VectorInsertDynamic, vector->GetType(), operands);
}

MirInstruction *MirBuilder::Shuffle(MirValue *first, MirValue *second, Span<const uint32> components)
{
    const MirType *scalar = first->GetType()->GetElement();
    const MirType *type = components.Count() == 1 ? scalar : _types->GetVector(scalar, components.Count());
    MirValue *operands[] = {first, second};
    MirInstruction *instruction = Create(MirOp::VectorShuffle, type, operands);
    for (const uint32 component : components)
    {
        instruction->AddLiteral(component);
    }
    return instruction;
}

MirInstruction *MirBuilder::Select(MirValue *condition, MirValue *whenTrue, MirValue *whenFalse)
{
    MirValue *operands[] = {condition, whenTrue, whenFalse};
    return Create(MirOp::Select, whenTrue->GetType(), operands);
}

MirInstruction *MirBuilder::Variable(const MirType *type, StringView name)
{
    MirBlock *entry = _block->GetParent()->GetEntry();
    MirInstruction *instruction = _module->GetArena().New<MirInstruction>(
        MirOp::Variable, _types->GetPointer(type, MirAddressSpace::Function), _module->GetArena());
    instruction->Location = _location;
    instruction->SetName(_module->Intern(name));
    MirInstruction *position = entry->GetFirst();
    while (position != nullptr && position->GetOp() == MirOp::Variable)
    {
        position = position->GetNext();
    }
    entry->InsertBefore(instruction, position);
    return instruction;
}

MirInstruction *MirBuilder::Load(MirValue *pointer)
{
    MirValue *operands[] = {pointer};
    return Create(MirOp::Load, pointer->GetType()->GetElement(), operands);
}

MirInstruction *MirBuilder::Store(MirValue *pointer, MirValue *value)
{
    MirValue *operands[] = {pointer, value};
    return Create(MirOp::Store, _types->GetVoid(), operands);
}

MirInstruction *MirBuilder::AccessChain(MirValue *pointer, Span<MirValue *const> indices)
{
    const MirType *type = pointer->GetType()->GetElement();
    for (MirValue *index : indices)
    {
        if (type->Is(MirTypeKind::Struct))
        {
            MARLIN_ASSERT(index->GetValueKind() == MirValueKind::Constant);
            type = type->GetFields()[static_cast<MirConstant *>(index)->GetBits()].Type;
        }
        else if (type->Is(MirTypeKind::Matrix) || type->Is(MirTypeKind::Vector) || type->Is(MirTypeKind::Array) ||
                 type->Is(MirTypeKind::RuntimeArray))
        {
            type = type->GetElement();
        }
        else
        {
            MARLIN_ASSERT(false);
        }
    }
    MirInstruction *instruction = NewInstruction(
        MirOp::AccessChain, _types->GetPointer(type, pointer->GetType()->GetAddressSpace()));
    instruction->AddOperand(pointer);
    for (MirValue *index : indices)
    {
        instruction->AddOperand(index);
    }
    return instruction;
}

MirInstruction *MirBuilder::ArrayLength(MirValue *pointer)
{
    MirValue *operands[] = {pointer};
    return Create(MirOp::ArrayLength, _types->GetInt(32, false), operands);
}

MirInstruction *MirBuilder::BufferPointer(MirValue *handle)
{
    const MirType *buffer = handle->GetType();
    const MirType *element = buffer->GetElement() != nullptr ? buffer->GetElement() : _types->GetInt(32, false);
    MirValue *operands[] = {handle};
    return Create(MirOp::BufferPointer,
                  _types->GetPointer(_types->GetRuntimeArray(element), MirAddressSpace::Buffer), operands);
}

MirInstruction *MirBuilder::Call(MirFunction *function, Span<MirValue *const> arguments)
{
    MirInstruction *instruction = NewInstruction(MirOp::Call, function->GetReturnType());
    instruction->AddOperand(function);
    for (MirValue *argument : arguments)
    {
        instruction->AddOperand(argument);
    }
    return instruction;
}

MirInstruction *MirBuilder::Phi(const MirType *type)
{
    MirBlock *block = _block;
    MirInstruction *instruction = _module->GetArena().New<MirInstruction>(MirOp::Phi, type, _module->GetArena());
    instruction->Location = _location;
    MirInstruction *position = block->GetFirst();
    while (position != nullptr && position->GetOp() == MirOp::Phi)
    {
        position = position->GetNext();
    }
    block->InsertBefore(instruction, position);
    return instruction;
}

MirInstruction *MirBuilder::Intrinsic(MirIntrinsic intrinsic, const MirType *type, Span<MirValue *const> operands,
                                      Span<const int64> literals)
{
    MirInstruction *instruction = Create(MirOp::Intrinsic, type, operands);
    instruction->SetIntrinsic(intrinsic);
    for (const int64 literal : literals)
    {
        instruction->AddLiteral(literal);
    }
    return instruction;
}

MirInstruction *MirBuilder::New(const MirType *referenceType, uint32 scriptType)
{
    MirInstruction *instruction = NewInstruction(MirOp::New, referenceType);
    instruction->AddLiteral(scriptType);
    return instruction;
}

MirInstruction *MirBuilder::NewArray(const MirType *referenceType, uint32 scriptType, MirValue *length)
{
    MirValue *operands[] = {length};
    MirInstruction *instruction = Create(MirOp::NewArray, referenceType, operands);
    instruction->AddLiteral(scriptType);
    return instruction;
}

MirInstruction *MirBuilder::FieldAddress(MirValue *object, const MirType *fieldType, uint32 field, uint32 scriptType)
{
    MirValue *operands[] = {object};
    MirInstruction *instruction =
        Create(MirOp::FieldAddress, _types->GetPointer(fieldType, MirAddressSpace::Managed), operands);
    instruction->AddLiteral(field);
    instruction->AddLiteral(scriptType);
    return instruction;
}

MirInstruction *MirBuilder::ElementAddress(MirValue *array, MirValue *index)
{
    MirValue *operands[] = {array, index};
    return Create(MirOp::ElementAddress,
                  _types->GetPointer(array->GetType()->GetElement()->GetElement(), MirAddressSpace::Managed), operands);
}

MirInstruction *MirBuilder::ObjectLength(MirValue *object)
{
    MirValue *operands[] = {object};
    return Create(MirOp::ObjectLength, _types->GetInt(32, true), operands);
}

MirInstruction *MirBuilder::Box(MirValue *value, const MirType *referenceType, uint32 scriptType)
{
    MirValue *operands[] = {value};
    MirInstruction *instruction = Create(MirOp::Box, referenceType, operands);
    instruction->AddLiteral(scriptType);
    return instruction;
}

MirInstruction *MirBuilder::Unbox(MirValue *object, const MirType *valueType, uint32 scriptType)
{
    MirValue *operands[] = {object};
    MirInstruction *instruction = Create(MirOp::Unbox, valueType, operands);
    instruction->AddLiteral(scriptType);
    return instruction;
}

MirInstruction *MirBuilder::IsInstance(MirValue *object, uint32 scriptType)
{
    MirValue *operands[] = {object};
    MirInstruction *instruction = Create(MirOp::IsInstance, _types->GetBool(), operands);
    instruction->AddLiteral(scriptType);
    return instruction;
}

MirInstruction *MirBuilder::Cast(MirValue *object, const MirType *referenceType, uint32 scriptType, bool orNull)
{
    MirValue *operands[] = {object};
    MirInstruction *instruction = Create(MirOp::Cast, referenceType, operands);
    instruction->AddLiteral(scriptType);
    instruction->AddLiteral(orNull ? 1 : 0);
    return instruction;
}

MirInstruction *MirBuilder::TypeOf(const MirType *typeInfo, uint32 scriptType)
{
    MirInstruction *instruction = NewInstruction(MirOp::TypeOf, typeInfo);
    instruction->AddLiteral(scriptType);
    return instruction;
}

MirInstruction *MirBuilder::NullCheck(MirValue *value, const MirType *type)
{
    MirValue *operands[] = {value};
    return Create(MirOp::NullCheck, type, operands);
}

MirInstruction *MirBuilder::CallVirtual(MirFunction *declared, uint32 slot, Span<MirValue *const> arguments)
{
    MirInstruction *instruction = NewInstruction(MirOp::CallVirtual, declared->GetReturnType());
    instruction->AddOperand(declared);
    for (MirValue *argument : arguments)
    {
        instruction->AddOperand(argument);
    }
    instruction->AddLiteral(slot);
    return instruction;
}

MirInstruction *MirBuilder::CallInterface(MirFunction *declared, uint32 interfaceType, uint32 method,
                                          Span<MirValue *const> arguments)
{
    MirInstruction *instruction = NewInstruction(MirOp::CallInterface, declared->GetReturnType());
    instruction->AddOperand(declared);
    for (MirValue *argument : arguments)
    {
        instruction->AddOperand(argument);
    }
    instruction->AddLiteral(interfaceType);
    instruction->AddLiteral(method);
    return instruction;
}

MirInstruction *MirBuilder::CallDelegate(MirValue *delegate, const MirType *result, Span<MirValue *const> arguments,
                                         bool allowEmpty)
{
    MirInstruction *instruction = NewInstruction(MirOp::CallDelegate, result);
    instruction->AddOperand(delegate);
    for (MirValue *argument : arguments)
    {
        instruction->AddOperand(argument);
    }
    instruction->AddLiteral(allowEmpty ? 1 : 0);
    return instruction;
}

MirInstruction *MirBuilder::MakeDelegate(const MirType *delegateType, uint32 scriptType, MirFunction *function,
                                         MirValue *receiver, int64 slot)
{
    MirInstruction *instruction = NewInstruction(MirOp::MakeDelegate, delegateType);
    instruction->AddOperand(function);
    if (receiver != nullptr)
    {
        instruction->AddOperand(receiver);
    }
    instruction->AddLiteral(scriptType);
    instruction->AddLiteral(slot);
    return instruction;
}

MirInstruction *MirBuilder::CombineDelegates(MirValue *left, MirValue *right, bool remove)
{
    MirValue *operands[] = {left, right};
    MirInstruction *instruction = Create(MirOp::CombineDelegates, left->GetType(), operands);
    instruction->AddLiteral(remove ? 1 : 0);
    return instruction;
}

MirInstruction *MirBuilder::StringConstant(const MirType *stringType, uint32 string)
{
    MirInstruction *instruction = NewInstruction(MirOp::StringConstant, stringType);
    instruction->AddLiteral(string);
    return instruction;
}

MirInstruction *MirBuilder::Await(MirValue *task, const MirType *result)
{
    MirValue *operands[] = {task};
    return Create(MirOp::Await, result, operands);
}

MirInstruction *MirBuilder::Yield(MirValue *value)
{
    MirValue *operands[] = {value};
    return Create(MirOp::Yield, _types->GetVoid(), operands);
}

MirInstruction *MirBuilder::Fault(MirFault fault, int64 message)
{
    MirInstruction *instruction = NewInstruction(MirOp::Fault, _types->GetVoid());
    instruction->AddLiteral(static_cast<int64>(fault));
    instruction->AddLiteral(message);
    return instruction;
}

MirInstruction *MirBuilder::Branch(MirBlock *target)
{
    MirInstruction *instruction = NewInstruction(MirOp::Branch, _types->GetVoid());
    instruction->AddTarget(target);
    return instruction;
}

MirInstruction *MirBuilder::CondBranch(MirValue *condition, MirBlock *whenTrue, MirBlock *whenFalse)
{
    MirInstruction *instruction = NewInstruction(MirOp::CondBranch, _types->GetVoid());
    instruction->AddOperand(condition);
    instruction->AddTarget(whenTrue);
    instruction->AddTarget(whenFalse);
    return instruction;
}

MirInstruction *MirBuilder::Switch(MirValue *selector, MirBlock *defaultTarget, Span<const int64> cases,
                                   Span<MirBlock *const> targets)
{
    MARLIN_ASSERT(cases.Count() == targets.Count());
    MirInstruction *instruction = NewInstruction(MirOp::Switch, _types->GetVoid());
    instruction->AddOperand(selector);
    instruction->AddTarget(defaultTarget);
    for (usize i = 0; i < cases.Count(); ++i)
    {
        instruction->AddLiteral(cases[i]);
        instruction->AddTarget(targets[i]);
    }
    return instruction;
}

MirInstruction *MirBuilder::Return(MirValue *value)
{
    MirInstruction *instruction = NewInstruction(MirOp::Return, _types->GetVoid());
    if (value != nullptr)
    {
        instruction->AddOperand(value);
    }
    return instruction;
}

MirInstruction *MirBuilder::Terminate(MirOp op, Span<MirValue *const> operands)
{
    return Create(op, _types->GetVoid(), operands);
}
} // namespace Marlin
