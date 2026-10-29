#include "LanguageIR/MirModule.h"

#include "Hash/Hash.h"
#include "Math/Float16.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"

namespace Marlin
{
void MirValue::ReplaceAllUsesWith(MirValue *other)
{
    MARLIN_ASSERT(other != this);
    while (!_uses.IsEmpty())
    {
        const MirUse use = _uses.Last();
        use.User->SetOperand(use.Index, other);
    }
}

uint32 MirValue::AddUse(MirInstruction *user, uint32 index)
{
    _uses.Add({user, index});
    return static_cast<uint32>(_uses.Count() - 1);
}

void MirValue::RemoveUse(uint32 position, MirInstruction *user, uint32 index)
{
    MARLIN_ASSERT(position < _uses.Count() && _uses[position].User == user && _uses[position].Index == index);
    (void)user;
    (void)index;
    _uses.RemoveAtSwap(position);
    if (position < _uses.Count())
    {
        const MirUse &moved = _uses[position];
        moved.User->_usePositions[moved.Index] = position;
    }
}

double MirConstant::GetFloat() const
{
    switch (GetType()->GetWidth())
    {
    case 16:
        return Float16::FromBits(static_cast<uint16>(_bits)).ToFloat();
    case 32:
        return BitCast<float32>(static_cast<uint32>(_bits));
    default:
        return BitCast<float64>(_bits);
    }
}

bool MirInstruction::IsTerminator() const
{
    switch (_op)
    {
    case MirOp::Branch:
    case MirOp::CondBranch:
    case MirOp::Switch:
    case MirOp::Return:
    case MirOp::Discard:
    case MirOp::Unreachable:
    case MirOp::IgnoreHit:
    case MirOp::AcceptHitAndEndSearch:
    case MirOp::EmitMeshTasks:
    case MirOp::Fault:
        return true;
    default:
        return false;
    }
}

void MirInstruction::Use(usize index)
{
    _usePositions[index] = _operands[index]->AddUse(this, static_cast<uint32>(index));
}

void MirInstruction::Unuse(usize index)
{
    _operands[index]->RemoveUse(_usePositions[index], this, static_cast<uint32>(index));
}

void MirInstruction::AddOperand(MirValue *value)
{
    _operands.Add(value);
    _usePositions.Add(0);
    Use(_operands.Count() - 1);
}

void MirInstruction::SetOperand(usize index, MirValue *value)
{
    Unuse(index);
    _operands[index] = value;
    Use(index);
}

void MirInstruction::RemoveOperand(usize index)
{
    for (usize i = index; i < _operands.Count(); ++i)
    {
        Unuse(i);
    }
    _operands.RemoveAt(index);
    _usePositions.RemoveAt(index);
    for (usize i = index; i < _operands.Count(); ++i)
    {
        Use(i);
    }
    if (_op == MirOp::Phi && index < _targets.Count())
    {
        _targets.RemoveAt(index);
    }
}

void MirInstruction::DropOperands()
{
    for (usize i = 0; i < _operands.Count(); ++i)
    {
        Unuse(i);
    }
    _operands.Clear();
    _usePositions.Clear();
    _targets.Clear();
}

void MirInstruction::Erase()
{
    MARLIN_ASSERT(!HasUses());
    if (_parent != nullptr)
    {
        _parent->Unlink(this);
    }
    DropOperands();
}

MirInstruction *MirBlock::GetTerminator() const
{
    return _last != nullptr && _last->IsTerminator() ? _last : nullptr;
}

void MirBlock::Append(MirInstruction *instruction)
{
    InsertBefore(instruction, nullptr);
}

void MirBlock::InsertBefore(MirInstruction *instruction, MirInstruction *position)
{
    MARLIN_ASSERT(instruction->_parent == nullptr);
    instruction->_parent = this;
    instruction->_next = position;
    instruction->_previous = position != nullptr ? position->_previous : _last;
    (instruction->_previous != nullptr ? instruction->_previous->_next : _first) = instruction;
    (position != nullptr ? position->_previous : _last) = instruction;
}

void MirBlock::Unlink(MirInstruction *instruction)
{
    MARLIN_ASSERT(instruction->_parent == this);
    (instruction->_previous != nullptr ? instruction->_previous->_next : _first) = instruction->_next;
    (instruction->_next != nullptr ? instruction->_next->_previous : _last) = instruction->_previous;
    instruction->_parent = nullptr;
    instruction->_previous = nullptr;
    instruction->_next = nullptr;
}

MirFunction::MirFunction(const MirType *functionType, MirModule &module, Arena &arena)
    : MirValue(MirValueKind::Function, functionType, arena), _module(&module), _arguments(ArenaAllocator{&arena}),
      _blocks(ArenaAllocator{&arena})
{
    const Span<const MirType *const> parameters = functionType->GetParameters();
    for (usize i = 0; i < parameters.Count(); ++i)
    {
        _arguments.Add(arena.New<MirArgument>(parameters[i], this, static_cast<uint32>(i), arena));
    }
}

MirBlock *MirFunction::AddBlock(StringView name)
{
    MirBlock *block = _module->GetArena().New<MirBlock>(this);
    block->SetName(name);
    _blocks.Add(block);
    return block;
}

void MirFunction::RemoveBlock(MirBlock *block)
{
    while (MirInstruction *instruction = block->GetLast())
    {
        block->Unlink(instruction);
        instruction->DropOperands();
    }
    MARLIN_VERIFY(_blocks.Remove(block));
}

void MirFunction::MoveBlock(MirBlock *block, usize index)
{
    MARLIN_VERIFY(_blocks.Remove(block));
    _blocks.Insert(index, block);
}

uint64 MirModule::ScalarKey::GetHash() const
{
    return HashCombine(HashInteger(reinterpret_cast<uintptr>(Type)), HashInteger(Bits));
}

uint64 MirModule::CompositeKey::GetHash() const
{
    uint64 hash = HashInteger(reinterpret_cast<uintptr>(Type));
    for (const MirConstant *element : Elements)
    {
        hash = HashCombine(hash, HashInteger(reinterpret_cast<uintptr>(element)));
    }
    return hash;
}

bool MirModule::CompositeKey::operator==(const CompositeKey &other) const
{
    if (Type != other.Type || Elements.Count() != other.Elements.Count())
    {
        return false;
    }
    for (usize i = 0; i < Elements.Count(); ++i)
    {
        if (Elements[i] != other.Elements[i])
        {
            return false;
        }
    }
    return true;
}

MirModule::MirModule() : _types(_arena)
{
}

MirModule::~MirModule() = default;

MirConstant *MirModule::GetBool(bool value)
{
    return GetScalarBits(_types.GetBool(), value ? 1 : 0);
}

MirConstant *MirModule::GetInt(const MirType *type, int64 value)
{
    MARLIN_ASSERT(type->Is(MirTypeKind::Int) || type->Is(MirTypeKind::Bool));
    uint64 bits = static_cast<uint64>(value);
    const uint32 width = type->Is(MirTypeKind::Bool) ? 1 : type->GetWidth();
    if (width < 64)
    {
        const uint64 mask = (uint64{1} << width) - 1;
        bits &= mask;
        if (type->IsSigned() && (bits >> (width - 1)) != 0)
        {
            bits |= ~mask;
        }
    }
    return GetScalarBits(type, bits);
}

MirConstant *MirModule::GetFloat(const MirType *type, double value)
{
    MARLIN_ASSERT(type->Is(MirTypeKind::Float));
    switch (type->GetWidth())
    {
    case 16:
        return GetScalarBits(type, Float16(static_cast<float32>(value)).Bits);
    case 32:
        return GetScalarBits(type, BitCast<uint32>(static_cast<float32>(value)));
    default:
        return GetScalarBits(type, BitCast<uint64>(value));
    }
}

MirConstant *MirModule::GetScalarBits(const MirType *type, uint64 bits)
{
    const ScalarKey key{type, bits};
    if (MirConstant **found = _scalars.Find(key))
    {
        return *found;
    }
    MirConstant *constant = _arena.New<MirConstant>(type, MirConstantKind::Scalar, _arena);
    constant->_bits = bits;
    _scalars.Add(key, constant);
    return constant;
}

MirConstant *MirModule::GetComposite(const MirType *type, Span<MirConstant *const> elements)
{
    if (MirConstant **found = _composites.Find(CompositeKey{type, elements}))
    {
        return *found;
    }
    MirConstant *constant = _arena.New<MirConstant>(type, MirConstantKind::Composite, _arena);
    constant->_elements.AddRange(elements);
    _composites.Add(CompositeKey{type, constant->_elements.AsSpan()}, constant);
    return constant;
}

MirConstant *MirModule::GetSplat(const MirType *type, MirConstant *element)
{
    const uint32 count = type->Is(MirTypeKind::Matrix) ? type->GetRows() : type->GetCount();
    InlineList<MirConstant *, 16> elements;
    for (uint32 i = 0; i < count; ++i)
    {
        elements.Add(element);
    }
    return GetComposite(type, elements.AsSpan());
}

MirConstant *MirModule::GetNull(const MirType *type)
{
    if (MirConstant **found = _nulls.Find(type))
    {
        return *found;
    }
    MirConstant *constant = _arena.New<MirConstant>(type, MirConstantKind::Null, _arena);
    _nulls.Add(type, constant);
    return constant;
}

MirConstant *MirModule::GetUndef(const MirType *type)
{
    if (MirConstant **found = _undefs.Find(type))
    {
        return *found;
    }
    MirConstant *constant = _arena.New<MirConstant>(type, MirConstantKind::Undef, _arena);
    _undefs.Add(type, constant);
    return constant;
}

MirGlobal *MirModule::AddGlobal(const MirType *valueType, MirAddressSpace space, StringView name)
{
    MirGlobal *global = _arena.New<MirGlobal>(_types.GetPointer(valueType, space), _arena);
    global->SetName(Intern(name));
    _globals.Add(global);
    return global;
}

MirFunction *MirModule::AddFunction(const MirType *functionType, StringView name)
{
    MirFunction *function = _arena.New<MirFunction>(functionType, *this, _arena);
    function->SetName(Intern(name));
    _functions.Add(function);
    return function;
}

MirEntryPoint &MirModule::AddEntryPoint(StringView name, MirStage stage, MirFunction *function)
{
    MirEntryPoint *entryPoint = _arena.New<MirEntryPoint>(_arena);
    entryPoint->Name = Intern(name);
    entryPoint->Stage = stage;
    entryPoint->Function = function;
    _entryPoints.Add(entryPoint);
    return *entryPoint;
}

void MirModule::RemoveFunction(MirFunction *function)
{
    MARLIN_ASSERT(!function->HasUses());
    while (!function->GetBlocks().IsEmpty())
    {
        function->RemoveBlock(function->GetBlocks().Last());
    }
    MARLIN_VERIFY(_functions.Remove(function));
    _attributes.RemoveAll([&](const MirAttribute *attribute) { return attribute->Function == function; });
}

MirAttribute &MirModule::AddAttribute(StringView name)
{
    MirAttribute *attribute = _arena.New<MirAttribute>(_arena);
    attribute->Name = Intern(name);
    _attributes.Add(attribute);
    return *attribute;
}

void MirModule::RemoveGlobal(MirGlobal *global)
{
    MARLIN_ASSERT(!global->HasUses());
    MARLIN_VERIFY(_globals.Remove(global));
    for (MirEntryPoint *entryPoint : _entryPoints)
    {
        entryPoint->Interface.Remove(global);
    }
}

uint32 MirModule::AddFile(StringView path, StringView text)
{
    _files.Add(Intern(path));
    _fileTexts.Add(Intern(text));
    return static_cast<uint32>(_files.Count());
}

uint32 MirModule::AddDebugVariable(StringView name, const MirType *type, MirLocation location)
{
    _debugVariables.Add({Intern(name), type, location});
    return static_cast<uint32>(_debugVariables.Count() - 1);
}

uint32 MirModule::AddScriptType(MirScriptType *type)
{
    _scriptTypes.Add(type);
    return static_cast<uint32>(_scriptTypes.Count() - 1);
}

uint32 MirModule::AddString(StringView text)
{
    for (usize i = 0; i < _strings.Count(); ++i)
    {
        if (_strings[i] == text)
        {
            return static_cast<uint32>(i);
        }
    }
    _strings.Add(Intern(text));
    return static_cast<uint32>(_strings.Count() - 1);
}

StringView MirModule::Intern(StringView text)
{
    if (text.IsEmpty())
    {
        return {};
    }
    char *data = _arena.AllocateArray<char>(text.Length() + 1);
    MemCopy(data, text.Data(), text.Length());
    data[text.Length()] = '\0';
    return {data, text.Length()};
}
} // namespace Marlin
