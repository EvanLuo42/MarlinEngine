#include <initializer_list>

#include "Containers/HashSet.h"
#include "Hash/Xxh3.h"
#include "LanguageIR/MirPrinter.h"
#include "Memory/MemoryOps.h"
#include "ScriptModuleBuilder.h"
#include "ScriptRegisters.h"
#include "String/Format.h"
#include "Templates/NumericLimits.h"

namespace Marlin
{
namespace
{
constexpr uint32 NoSlot = ~0u;

/// Where a pointer value points, so loads and stores through it can address the memory directly.
enum class AddressKind : uint8
{
    Register,
    Frame,
    Static,
    Field,
    Element,
};

struct Address
{
    AddressKind Kind = AddressKind::Register;
    uint32 Slot = 0;
    uint32 Offset = 0;
    MirValue *Object = nullptr;
    MirValue *Index = nullptr;
};

/// The registers a value is assigned: *Slots* of them from *Slot*.
struct ValueRegisters
{
    uint32 Slot = NoSlot;
    uint32 Slots = 0;
};

/// A set of value ids, a bit each.
struct ValueSet
{
    List<uint64> Words;

    void Reset(usize count)
    {
        Words.Clear();
        Words.Resize((count + 63) / 64, 0);
    }

    [[nodiscard]] bool Contains(uint32 id) const
    {
        return ((Words[id / 64] >> (id % 64)) & 1) != 0;
    }

    void Add(uint32 id)
    {
        Words[id / 64] |= uint64{1} << (id % 64);
    }

    void Remove(uint32 id)
    {
        Words[id / 64] &= ~(uint64{1} << (id % 64));
    }

    /// Adds the ids of *other*; true when that adds any.
    bool Union(const ValueSet &other)
    {
        bool grew = false;
        for (usize i = 0; i < Words.Count(); ++i)
        {
            const uint64 merged = Words[i] | other.Words[i];
            grew = grew || merged != Words[i];
            Words[i] = merged;
        }
        return grew;
    }

    template <typename TVisit> void ForEach(TVisit &&visit) const
    {
        for (usize i = 0; i < Words.Count(); ++i)
        {
            for (uint64 bits = Words[i]; bits != 0; bits &= bits - 1)
            {
                visit(static_cast<uint32>(i * 64 + static_cast<usize>(__builtin_ctzll(bits))));
            }
        }
    }
};

struct ConstantKey
{
    ScriptSlot Slot;

    [[nodiscard]] uint64 GetHash() const
    {
        return Xxh3Hash64(Slot.Bytes, sizeof(Slot.Bytes));
    }

    bool operator==(const ConstantKey &other) const = default;
};

/// A jump whose target is patched once every label is bound.
struct Fixup
{
    uint32 Word = 0;
    uint32 Label = 0;
    uint32 Id = 0;
    ScriptOp Op = ScriptOp::Jump;
    uint32 A = 0;
    bool Wide = false;
    /// Word is the 32-bit offset that ends a compare-and-jump, which always reaches.
    bool Trailing = false;
};

struct SwitchFixup
{
    uint32 Table = 0;
    uint32 Default = 0;
    List<uint32> Cases;
};

struct PendingSafepoint
{
    uint32 Pc = 0;
    List<uint32> Values;
};

struct SlotMove
{
    uint32 Dst = 0;
    uint32 Src = 0;
};

/// Whether *instruction* is a call with a window of registers (CallWindow).
bool IsCall(const MirInstruction *instruction)
{
    const MirOp op = instruction->GetOp();
    return op == MirOp::Call || op == MirOp::CallVirtual || op == MirOp::CallInterface || op == MirOp::CallDelegate;
}

/// The first operand of a call that goes in its window: a delegate call passes the delegate, the others start after
/// the function.
usize FirstWindowOperand(const MirInstruction *call)
{
    return call->GetOp() == MirOp::CallDelegate ? 0 : 1;
}

/// Whether the collector may run at *instruction*: calls, allocations, await and yield (MarlinScript §9.3).
bool IsSafepoint(const MirInstruction *instruction)
{
    switch (instruction->GetOp())
    {
    case MirOp::Call:
    case MirOp::CallVirtual:
    case MirOp::CallInterface:
    case MirOp::CallDelegate:
    case MirOp::New:
    case MirOp::NewArray:
    case MirOp::Box:
    case MirOp::TypeOf:
    case MirOp::MakeDelegate:
    case MirOp::CombineDelegates:
    case MirOp::Await:
    case MirOp::Yield:
        return true;
    default:
        return false;
    }
}

bool IsCompare(MirOp op)
{
    return op == MirOp::Equal || op == MirOp::NotEqual || op == MirOp::Less || op == MirOp::LessEqual ||
           op == MirOp::Greater || op == MirOp::GreaterEqual;
}

/// The comparison that holds exactly when *op* does not, for integers.
MirOp NegatedCompare(MirOp op)
{
    switch (op)
    {
    case MirOp::Equal:
        return MirOp::NotEqual;
    case MirOp::NotEqual:
        return MirOp::Equal;
    case MirOp::Less:
        return MirOp::GreaterEqual;
    case MirOp::LessEqual:
        return MirOp::Greater;
    case MirOp::Greater:
        return MirOp::LessEqual;
    default:
        return MirOp::Less;
    }
}

/// The comparison of the swapped operands that holds when *op* does.
MirOp MirroredCompare(MirOp op)
{
    switch (op)
    {
    case MirOp::Less:
        return MirOp::Greater;
    case MirOp::LessEqual:
        return MirOp::GreaterEqual;
    case MirOp::Greater:
        return MirOp::Less;
    case MirOp::GreaterEqual:
        return MirOp::LessEqual;
    default:
        return op;
    }
}

bool IsScalarConstant(const MirValue *value)
{
    return value->GetValueKind() == MirValueKind::Constant &&
           static_cast<const MirConstant *>(value)->GetConstantKind() == MirConstantKind::Scalar;
}

bool IsSmall(ScriptNumber number)
{
    return number == ScriptNumber::I8 || number == ScriptNumber::U8 || number == ScriptNumber::I16 ||
           number == ScriptNumber::U16;
}

bool IsSigned(ScriptNumber number)
{
    return number == ScriptNumber::I8 || number == ScriptNumber::I16 || number == ScriptNumber::I32 ||
           number == ScriptNumber::I64;
}

bool IsFloat(ScriptNumber number)
{
    return number == ScriptNumber::F16 || number == ScriptNumber::F32 || number == ScriptNumber::F64;
}

bool Is64(ScriptNumber number)
{
    return number == ScriptNumber::I64 || number == ScriptNumber::U64 || number == ScriptNumber::F64;
}

/// The instruction that re-extends a narrow integer register after arithmetic.
ScriptOp ExtendOf(ScriptNumber number)
{
    switch (number)
    {
    case ScriptNumber::I8:
        return ScriptOp::SExt8;
    case ScriptNumber::U8:
        return ScriptOp::ZExt8;
    case ScriptNumber::I16:
        return ScriptOp::SExt16;
    default:
        return ScriptOp::ZExt16;
    }
}

/// The number arithmetic on *number* runs in: 32 bits for narrow integers and F32 for F16.
ScriptNumber Widened(ScriptNumber number)
{
    switch (number)
    {
    case ScriptNumber::I8:
    case ScriptNumber::I16:
        return ScriptNumber::I32;
    case ScriptNumber::U8:
    case ScriptNumber::U16:
        return ScriptNumber::U32;
    case ScriptNumber::F16:
        return ScriptNumber::F32;
    case ScriptNumber::Bool:
        return ScriptNumber::U32;
    default:
        return number;
    }
}

const MirType *ScalarOf(const MirType *type)
{
    switch (type->GetKind())
    {
    case MirTypeKind::Vector:
        return type->GetElement();
    case MirTypeKind::Matrix:
        return type->GetElement()->GetElement();
    default:
        return type;
    }
}

uint32 LanesOf(const MirType *type)
{
    return type->Is(MirTypeKind::Vector) ? type->GetCount() : 1;
}

/// The type of element *index* of *type*, adding its offset to *offset*; null when *type* has no such element.
const MirType *ElementAt(const MirType *type, uint32 index, uint32 &offset)
{
    switch (type->GetKind())
    {
    case MirTypeKind::Struct:
        if (index >= type->GetFields().Count())
        {
            return nullptr;
        }
        offset += type->GetFields()[index].Offset;
        return type->GetFields()[index].Type;
    case MirTypeKind::Array:
    case MirTypeKind::Matrix:
        offset += index * type->GetStride();
        return type->GetElement();
    case MirTypeKind::Vector:
        offset += index * type->GetElement()->GetSize();
        return type->GetElement();
    default:
        return nullptr;
    }
}

uint32 StrideOf(const MirType *type)
{
    return type->Is(MirTypeKind::Vector) ? type->GetElement()->GetSize() : type->GetStride();
}

uint32 Log2(uint32 value)
{
    uint32 result = 0;
    while ((1u << result) < value)
    {
        ++result;
    }
    return result;
}

/// The element word of arrays of *element*: the stride, and the log2 of the alignment above bit 24.
uint32 ElementWord(const MirType *element)
{
    const uint32 alignment = element->GetAlignment() == 0 ? 1 : element->GetAlignment();
    const uint32 stride = (element->GetSize() + alignment - 1) / alignment * alignment;
    return stride | (Log2(alignment) << 24);
}

ScriptMathOp MathOpOf(MirIntrinsic intrinsic, bool &ok)
{
    ok = true;
    switch (intrinsic)
    {
#define MARLIN_SCRIPT_MATH_OP(name)                                                                                    \
    case MirIntrinsic::name:                                                                                           \
        return ScriptMathOp::name;
        MARLIN_SCRIPT_MATH_OPS(MARLIN_SCRIPT_MATH_OP)
#undef MARLIN_SCRIPT_MATH_OP
    default:
        ok = false;
        return ScriptMathOp::Abs;
    }
}

class FunctionCompiler
{
public:
    FunctionCompiler(ScriptModuleBuilder &builder, MirFunction &function, ScriptFunctionRecord &record)
        : _builder(&builder), _module(&builder.GetModule()), _function(&function), _record(&record)
    {
    }

    void Compile()
    {
        AssignFrame();
        // Jumps start short; one that does not reach is made wide and the function emitted again.
        for (uint32 attempt = 0; attempt < 8; ++attempt)
        {
            EmitFunction();
            if (_failed || ResolveJumps())
            {
                break;
            }
        }
        if (_failed)
        {
            return;
        }
        Finish();
    }

private:
    // The frame: the parameters from register 0, then the frame variables (the only registers PushFrame zeroes), and
    // the values' registers, shared by values that are never live at once (AllocateRegisters). Each call's window
    // starts above what is live across it, so it may share registers with values that die there; temporaries go above
    // all of them.
    void AssignFrame()
    {
        const Span<MirBlock *const> blocks = _function->GetBlocks();
        for (usize i = 0; i < blocks.Count(); ++i)
        {
            _blockIndex.Add(blocks[i], static_cast<uint32>(i));
        }
        uint32 next = 0;
        for (MirArgument *argument : _function->GetArguments())
        {
            AddValue(argument);
            _registers.Last().Slot = next;
            next += ScriptSlotsOf(argument->GetType());
        }
        next = next > _record->ParameterSlots ? next : _record->ParameterSlots;
        _variablesStart = next;
        for (const MirBlock *block : blocks)
        {
            for (MirInstruction *instruction : *block)
            {
                if (instruction->GetOp() == MirOp::Variable)
                {
                    Address address;
                    address.Kind = AddressKind::Frame;
                    address.Slot = next;
                    _addresses.Add(instruction, address);
                    _variables.Add(instruction);
                    next += ScriptSlotsOf(instruction->GetType()->GetElement());
                }
            }
        }
        _variablesEnd = next;
        _record->ZeroSlots = _variablesEnd - _record->ParameterSlots;
        FindFusedCompares();
        for (const MirBlock *block : blocks)
        {
            for (MirInstruction *instruction : *block)
            {
                const MirType *type = instruction->GetType();
                if (instruction->GetOp() == MirOp::Variable || type == nullptr || type->Is(MirTypeKind::Void) ||
                    !instruction->HasUses() || _fused.Contains(instruction))
                {
                    continue;
                }
                if (type->Is(MirTypeKind::Pointer) && AddressOf(instruction).Kind != AddressKind::Register)
                {
                    continue;
                }
                AddValue(instruction);
            }
        }
        AllocateRegisters();
        _frameSlots = _fixedSlots > _record->ParameterSlots ? _fixedSlots : _record->ParameterSlots;
        CheckRegister(_fixedSlots);
    }

    /// Marks the comparisons a CondBranch can test with a compare-and-jump: those in its block that nothing else
    /// uses, on integers, F32, F64, references or pointers. They get no register of their own.
    void FindFusedCompares()
    {
        for (const MirBlock *block : _function->GetBlocks())
        {
            const MirInstruction *branch = block->GetTerminator();
            if (branch == nullptr || branch->GetOp() != MirOp::CondBranch ||
                branch->GetOperand(0)->GetValueKind() != MirValueKind::Instruction)
            {
                continue;
            }
            const auto *compare = static_cast<const MirInstruction *>(branch->GetOperand(0));
            if (compare->GetParent() != block || compare->GetUses().Count() != 1 || !IsCompare(compare->GetOp()))
            {
                continue;
            }
            const MirType *type = compare->GetOperand(0)->GetType();
            const bool address = type->Is(MirTypeKind::Reference) || type->Is(MirTypeKind::Pointer);
            if (address || (type->IsScalar() && ScriptNumberOf(type) != ScriptNumber::F16))
            {
                _fused.Add(compare);
            }
        }
    }

    /// Calls *visit* with the id of each value *value* reads registers of when used as an operand: its own, or for a
    /// folded field or element address those of its object and index.
    template <typename TVisit> void ForEachValueOf(const MirValue *value, TVisit &&visit) const
    {
        const uint32 id = IdOf(value);
        if (id != NoSlot)
        {
            visit(id);
            return;
        }
        const Address *address = _addresses.Find(value);
        if (address != nullptr && (address->Kind == AddressKind::Field || address->Kind == AddressKind::Element))
        {
            ForEachValueOf(address->Object, visit);
            if (address->Kind == AddressKind::Element)
            {
                ForEachValueOf(address->Index, visit);
            }
        }
    }

    /// Calls *visit* with the id of each value *instruction* reads; a CondBranch reads the operands of its fused
    /// comparison. Phis read theirs on the edges, so nothing here.
    template <typename TVisit> void ForEachUse(const MirInstruction *instruction, TVisit &&visit) const
    {
        if (instruction->GetOp() == MirOp::Phi)
        {
            return;
        }
        const MirValue *condition = instruction->GetOp() == MirOp::CondBranch ? instruction->GetOperand(0) : nullptr;
        if (condition != nullptr && condition->GetValueKind() == MirValueKind::Instruction &&
            _fused.Contains(static_cast<const MirInstruction *>(condition)))
        {
            instruction = static_cast<const MirInstruction *>(condition);
        }
        for (const MirValue *operand : instruction->GetOperands())
        {
            ForEachValueOf(operand, visit);
        }
    }

    /// Whether the code of *instruction* reads all its operands before it writes its result, so the result may share
    /// a register with an operand that dies there.
    [[nodiscard]] static bool ReadsBeforeWriting(const MirInstruction *instruction)
    {
        const MirType *operand = instruction->GetOperands().IsEmpty() ? nullptr : instruction->GetOperand(0)->GetType();
        switch (instruction->GetOp())
        {
        case MirOp::Add:
        case MirOp::Sub:
        case MirOp::Mul:
        case MirOp::Div:
        case MirOp::Rem:
        case MirOp::Negate:
        case MirOp::BitAnd:
        case MirOp::BitOr:
        case MirOp::BitXor:
        case MirOp::BitNot:
        case MirOp::ShiftLeft:
        case MirOp::ShiftRight:
        case MirOp::LogicalAnd:
        case MirOp::LogicalOr:
        case MirOp::LogicalNot:
        case MirOp::Equal:
        case MirOp::NotEqual:
        case MirOp::Less:
        case MirOp::LessEqual:
        case MirOp::Greater:
        case MirOp::GreaterEqual:
            return operand != nullptr && !operand->Is(MirTypeKind::Vector) && !operand->Is(MirTypeKind::Matrix) &&
                   !instruction->GetType()->Is(MirTypeKind::Vector);
        case MirOp::Convert:
            return operand != nullptr && operand->IsScalar();
        case MirOp::NullCheck:
            return operand != nullptr && !operand->Is(MirTypeKind::Struct);
        case MirOp::Load:
        case MirOp::Bitcast:
        case MirOp::ObjectLength:
        case MirOp::Cast:
        case MirOp::Unbox:
        case MirOp::IsInstance:
        // The arguments are copied into the window before the call, and the result out of it after.
        case MirOp::Call:
        case MirOp::CallVirtual:
        case MirOp::CallInterface:
        case MirOp::CallDelegate:
            return true;
        default:
            return false;
        }
    }

    /// The registers a call's window takes: its arguments, or its result when that is larger.
    [[nodiscard]] static uint32 WindowSlotsOf(const MirInstruction *call)
    {
        uint32 slots = 0;
        for (usize i = FirstWindowOperand(call); i < call->GetOperands().Count(); ++i)
        {
            slots += ScriptSlotsOf(call->GetOperand(i)->GetType());
        }
        const uint32 result = ScriptSlotsOf(call->GetType());
        return slots > result ? slots : result;
    }

    [[nodiscard]] bool HoldsReferences(uint32 id) const
    {
        const MirType *type = _values[id]->GetType();
        return ScriptHasReferences(type) ||
               (type->Is(MirTypeKind::Pointer) && type->GetAddressSpace() != MirAddressSpace::Static);
    }

    /// The ids of *live* that hold references or managed pointers, which a map names.
    [[nodiscard]] List<uint32> ReferencesIn(const ValueSet &live) const
    {
        List<uint32> values;
        live.ForEach([&](uint32 id) {
            if (HoldsReferences(id))
            {
                values.Add(id);
            }
        });
        return values;
    }

    // Liveness over the blocks, then a greedy assignment in definition order over the interference graph: two values
    // interfere when one is live where the other is defined, and an instruction's result interferes with its operands
    // unless its code reads them all first. A phi is defined at the start of its block (its moves at the end of each
    // predecessor only write registers no value live into the block holds), and prefers the registers of its incoming
    // values, so most of its moves disappear. The maps name only the references live across each safepoint, and what
    // the safepoint itself reads, all of which are written before the safepoint runs.
    void AllocateRegisters()
    {
        const Span<MirBlock *const> blocks = _function->GetBlocks();
        const usize count = _values.Count();
        List<ValueSet> uses;
        List<ValueSet> defs;
        List<ValueSet> edges;
        List<ValueSet> liveIn;
        List<ValueSet> liveOut;
        for (List<ValueSet> *sets : {&uses, &defs, &edges, &liveIn, &liveOut})
        {
            sets->Resize(blocks.Count());
            for (ValueSet &set : *sets)
            {
                set.Reset(count);
            }
        }
        List<List<uint32>> hints;
        hints.Resize(count);
        for (usize b = 0; b < blocks.Count(); ++b)
        {
            const MirBlock *block = blocks[b];
            for (const MirInstruction *instruction : *block)
            {
                if (_fused.Contains(instruction))
                {
                    continue;
                }
                ForEachUse(instruction, [&](uint32 id) {
                    if (!defs[b].Contains(id))
                    {
                        uses[b].Add(id);
                    }
                });
                const uint32 id = IdOf(instruction);
                if (id == NoSlot)
                {
                    continue;
                }
                defs[b].Add(id);
                if (instruction->GetOp() == MirOp::Phi)
                {
                    for (const MirValue *operand : instruction->GetOperands())
                    {
                        const uint32 incoming = IdOf(operand);
                        if (incoming != NoSlot)
                        {
                            hints[id].Add(incoming);
                            hints[incoming].Add(id);
                        }
                    }
                }
                else if (ReadsBeforeWriting(instruction))
                {
                    ForEachUse(instruction, [&](uint32 operand) { hints[id].Add(operand); });
                }
            }
            // What the phis of the successors read on the edges from this block, as EdgeMoves finds it.
            const MirInstruction *terminator = block->GetTerminator();
            for (const MirBlock *successor : terminator != nullptr ? terminator->GetTargets() : Span<MirBlock *const>())
            {
                for (const MirInstruction *phi : *successor)
                {
                    if (phi->GetOp() != MirOp::Phi)
                    {
                        break;
                    }
                    for (usize i = 0; i < phi->GetTargets().Count(); ++i)
                    {
                        if (phi->GetTargets()[i] == block)
                        {
                            ForEachValueOf(phi->GetOperand(i), [&](uint32 id) { edges[b].Add(id); });
                            break;
                        }
                    }
                }
            }
        }
        for (bool changed = true; changed;)
        {
            changed = false;
            for (usize b = blocks.Count(); b-- > 0;)
            {
                ValueSet &out = liveOut[b];
                out.Union(edges[b]);
                const MirInstruction *terminator = blocks[b]->GetTerminator();
                for (const MirBlock *successor :
                     terminator != nullptr ? terminator->GetTargets() : Span<MirBlock *const>())
                {
                    out.Union(liveIn[BlockLabel(successor)]);
                }
                ValueSet in = out;
                for (usize i = 0; i < in.Words.Count(); ++i)
                {
                    in.Words[i] = (in.Words[i] & ~defs[b].Words[i]) | uses[b].Words[i];
                }
                changed = liveIn[b].Union(in) || changed;
            }
        }
        List<List<uint32>> neighbors;
        neighbors.Resize(count);
        const auto interfere = [&](uint32 a, uint32 b) {
            if (a != b)
            {
                neighbors[a].Add(b);
                neighbors[b].Add(a);
            }
        };
        // The values live after each call, which its window must stay above, and those live after any call.
        struct CallSite
        {
            const MirInstruction *Call;
            ValueSet After;
        };
        List<CallSite> calls;
        ValueSet crossing;
        crossing.Reset(count);
        List<const MirInstruction *> body;
        for (usize b = 0; b < blocks.Count(); ++b)
        {
            const MirBlock *block = blocks[b];
            ValueSet live = liveOut[b];
            body.Clear();
            List<const MirInstruction *> phis;
            for (const MirInstruction *instruction : *block)
            {
                (instruction->GetOp() == MirOp::Phi ? phis : body).Add(instruction);
            }
            for (usize i = body.Count(); i-- > 0;)
            {
                const MirInstruction *instruction = body[i];
                if (_fused.Contains(instruction))
                {
                    continue;
                }
                const uint32 id = IdOf(instruction);
                if (IsCall(instruction))
                {
                    CallSite site{instruction, live};
                    if (id != NoSlot)
                    {
                        site.After.Remove(id);
                    }
                    crossing.Union(site.After);
                    calls.Add(Move(site));
                }
                if (IsSafepoint(instruction))
                {
                    ValueSet across = live;
                    if (id != NoSlot)
                    {
                        across.Remove(id);
                    }
                    ForEachUse(instruction, [&](uint32 operand) { across.Add(operand); });
                    _safepointValues.Set(instruction, ReferencesIn(across));
                }
                if (id != NoSlot)
                {
                    live.Remove(id);
                    live.ForEach([&](uint32 other) { interfere(id, other); });
                    if (!ReadsBeforeWriting(instruction))
                    {
                        ForEachUse(instruction, [&](uint32 operand) { interfere(id, operand); });
                    }
                }
                ForEachUse(instruction, [&](uint32 operand) { live.Add(operand); });
            }
            for (const MirInstruction *phi : phis)
            {
                const uint32 id = IdOf(phi);
                if (id == NoSlot)
                {
                    continue;
                }
                live.ForEach([&](uint32 other) { interfere(id, other); });
                for (const MirInstruction *other : phis)
                {
                    if (IdOf(other) != NoSlot)
                    {
                        interfere(id, IdOf(other));
                    }
                }
            }
            if (block->Merge.Kind == MirMergeKind::Loop)
            {
                _headerValues.Set(block, ReferencesIn(live));
            }
        }
        // Arguments keep the registers they arrive in; the others take the first that fits, a hinted one if free.
        // The values live across calls go first. Each call's window then starts just above those live across it (and
        // the frame variables, which every map names), since the callee's frame covers it and what lies beyond; the
        // others, which no call outlives, prefer the window registers they are passed in or returned to, so the moves
        // into and out of the window disappear.
        uint32 high = _variablesEnd;
        List<bool> busy;
        List<List<uint32>> slotHints;
        slotHints.Resize(count);
        const auto assign = [&](uint32 id) {
            ValueRegisters &registers = _registers[id];
            if (registers.Slot != NoSlot)
            {
                return;
            }
            const uint32 slots = registers.Slots;
            uint32 extent = high;
            for (const uint32 hint : slotHints[id])
            {
                extent = hint > extent ? hint : extent;
            }
            busy.Clear();
            busy.Resize(usize{extent} + slots, false);
            for (uint32 s = _variablesStart; s < _variablesEnd; ++s)
            {
                busy[s] = true;
            }
            for (const uint32 other : neighbors[id])
            {
                const ValueRegisters &taken = _registers[other];
                for (uint32 s = 0; taken.Slot != NoSlot && s < taken.Slots && taken.Slot + s < busy.Count(); ++s)
                {
                    busy[taken.Slot + s] = true;
                }
            }
            const auto fits = [&](uint32 slot) {
                for (uint32 s = 0; s < slots; ++s)
                {
                    if (slot + s < busy.Count() && busy[slot + s])
                    {
                        return false;
                    }
                }
                return true;
            };
            for (const uint32 slot : slotHints[id])
            {
                if (fits(slot))
                {
                    registers.Slot = slot;
                    break;
                }
            }
            for (usize i = 0; i < hints[id].Count() && registers.Slot == NoSlot; ++i)
            {
                const ValueRegisters &hinted = _registers[hints[id][i]];
                if (hinted.Slot != NoSlot && hinted.Slots == slots && fits(hinted.Slot))
                {
                    registers.Slot = hinted.Slot;
                }
            }
            for (uint32 slot = 0; registers.Slot == NoSlot; ++slot)
            {
                if (fits(slot))
                {
                    registers.Slot = slot;
                }
            }
            high = registers.Slot + slots > high ? registers.Slot + slots : high;
        };
        for (uint32 id = 0; id < count; ++id)
        {
            if (crossing.Contains(id))
            {
                assign(id);
            }
        }
        _callBases.Clear();
        for (const CallSite &site : calls)
        {
            uint32 base = _variablesEnd;
            site.After.ForEach([&](uint32 id) {
                const ValueRegisters &registers = _registers[id];
                base = registers.Slot + registers.Slots > base ? registers.Slot + registers.Slots : base;
            });
            _callBases.Set(site.Call, base);
            uint32 at = base;
            for (usize i = FirstWindowOperand(site.Call); i < site.Call->GetOperands().Count(); ++i)
            {
                const MirValue *operand = site.Call->GetOperand(i);
                if (const uint32 id = IdOf(operand); id != NoSlot)
                {
                    slotHints[id].Add(at);
                }
                at += ScriptSlotsOf(operand->GetType());
            }
            if (const uint32 id = IdOf(site.Call); id != NoSlot)
            {
                slotHints[id].Insert(0, base);
            }
        }
        for (uint32 id = 0; id < count; ++id)
        {
            assign(id);
        }
        for (const ValueRegisters &registers : _registers)
        {
            high = registers.Slot + registers.Slots > high ? registers.Slot + registers.Slots : high;
        }
        // Temporaries go above every window, so materializing an argument never writes one.
        for (const CallSite &site : calls)
        {
            const uint32 end = *_callBases.Find(site.Call) + WindowSlotsOf(site.Call);
            high = end > high ? end : high;
        }
        _fixedSlots = high;
    }

    /// Whether every use of the address *instruction* computes loads from it or stores to it.
    [[nodiscard]] bool OnlyLoadsAndStores(const MirInstruction *instruction) const
    {
        for (const MirUse &use : instruction->GetUses())
        {
            const MirOp op = use.User->GetOp();
            if (use.Index != 0 || (op != MirOp::Load && op != MirOp::Store))
            {
                return false;
            }
        }
        return true;
    }

    Address AddressOf(MirValue *pointer)
    {
        if (const Address *found = _addresses.Find(pointer))
        {
            return *found;
        }
        Address address;
        if (pointer->GetValueKind() == MirValueKind::Global)
        {
            const uint32 offset = _builder->StaticOffsetOf(static_cast<const MirGlobal *>(pointer));
            if (offset != ~0u)
            {
                address.Kind = AddressKind::Static;
                address.Offset = offset;
            }
        }
        else if (pointer->GetValueKind() == MirValueKind::Instruction)
        {
            MirInstruction *instruction = static_cast<MirInstruction *>(pointer);
            const bool folds = OnlyLoadsAndStores(instruction);
            switch (instruction->GetOp())
            {
            case MirOp::AccessChain: {
                const Address base = AddressOf(instruction->GetOperand(0));
                if (!folds || (base.Kind != AddressKind::Frame && base.Kind != AddressKind::Static &&
                               base.Kind != AddressKind::Field))
                {
                    break;
                }
                const MirType *type = instruction->GetOperand(0)->GetType()->GetElement();
                uint32 offset = base.Offset;
                bool constant = true;
                for (usize i = 1; i < instruction->GetOperands().Count() && constant && type != nullptr; ++i)
                {
                    MirValue *index = instruction->GetOperand(i);
                    constant = index->GetValueKind() == MirValueKind::Constant &&
                               static_cast<MirConstant *>(index)->GetConstantKind() == MirConstantKind::Scalar;
                    if (constant)
                    {
                        type = ElementAt(type, static_cast<uint32>(static_cast<MirConstant *>(index)->GetInt()), offset);
                    }
                }
                if (constant && type != nullptr)
                {
                    address = base;
                    address.Offset = offset;
                }
                break;
            }
            case MirOp::FieldAddress:
                if (folds)
                {
                    address.Kind = AddressKind::Field;
                    address.Object = instruction->GetOperand(0);
                    address.Offset = FieldOffset(instruction);
                }
                break;
            case MirOp::ElementAddress:
                if (folds)
                {
                    address.Kind = AddressKind::Element;
                    address.Object = instruction->GetOperand(0);
                    address.Index = instruction->GetOperand(1);
                }
                break;
            default:
                break;
            }
        }
        _addresses.Add(pointer, address);
        return address;
    }

    /// The offset of a FieldAddress's field in its class's fields.
    [[nodiscard]] uint32 FieldOffset(const MirInstruction *instruction) const
    {
        const MirScriptType *type = _module->GetScriptTypes()[static_cast<usize>(instruction->GetLiterals()[1])];
        return type->Layout->GetFields()[static_cast<usize>(instruction->GetLiterals()[0])].Offset;
    }

    /// Gives *value* an id; its registers are assigned later, except an argument's.
    void AddValue(MirValue *value)
    {
        _ids.Add(value, static_cast<uint32>(_values.Count()));
        _values.Add(value);
        _registers.Add({NoSlot, ScriptSlotsOf(value->GetType())});
    }

    [[nodiscard]] uint32 IdOf(const MirValue *value) const
    {
        const uint32 *found = _ids.Find(value);
        return found != nullptr ? *found : NoSlot;
    }

    void EmitFunction()
    {
        _code.Clear();
        _labels.Clear();
        _fixups.Clear();
        _switchFixups.Clear();
        _jumps = 0;
        _constants.Clear();
        _safepoints.Clear();
        _record->Constants.Clear();
        _record->Switches.Clear();
        _record->InterfaceSites.Clear();
        _record->LineTable.Clear();
        _record->Safepoints.Clear();
        _record->Maps.Clear();
        _lastLine = {};
        _lastLine.Line = ~0u;
        _location = {};
        const Span<MirBlock *const> blocks = _function->GetBlocks();
        for (usize i = 0; i < blocks.Count(); ++i)
        {
            _labels.Add(NoSlot);
        }
        _loopBodies.Clear();
        _forwarders.Clear();
        for (const MirBlock *block : blocks)
        {
            if (block->Merge.Kind == MirMergeKind::Loop)
            {
                _loopBodies.Set(block, NewLabel());
            }
            if (IsForwarder(block))
            {
                _forwarders.Add(block);
            }
        }
        // A forwarder's target must not be one, so a jump threads through one block at most.
        for (usize i = _forwarders.Count(); i-- > 0;)
        {
            if (_forwarders.Contains(ForwardTarget(_forwarders[i])))
            {
                _forwarders.RemoveAt(i);
            }
        }
        for (usize i = 0; i < blocks.Count() && !_failed; ++i)
        {
            const MirBlock *block = blocks[i];
            if (_forwarders.Contains(block))
            {
                continue;
            }
            _block = static_cast<uint32>(i);
            _next = nullptr;
            for (usize j = i + 1; j < blocks.Count() && _next == nullptr; ++j)
            {
                _next = _forwarders.Contains(blocks[j]) ? nullptr : blocks[j];
            }
            Bind(static_cast<uint32>(i));
            _tempNext = _fixedSlots;
            if (block->Merge.Kind == MirMergeKind::Loop)
            {
                // The header's Loop is the safepoint of entering the loop and of its conditional back edges; an
                // unconditional back edge has a Loop of its own that jumps past it (BranchTo).
                RecordSafepoint(*_headerValues.Find(block));
                Emit(ScriptOp::Loop);
                Bind(*_loopBodies.Find(block));
            }
            for (MirInstruction *instruction : *block)
            {
                if (_fused.Contains(instruction))
                {
                    continue;
                }
                _tempNext = _fixedSlots;
                SetPosition(instruction);
                Select(instruction);
                if (_failed)
                {
                    break;
                }
            }
        }
        // A jump to a forwarder, such as a switch case's, lands where it leads.
        for (const MirBlock *forwarder : _forwarders)
        {
            _labels[BlockLabel(forwarder)] = _labels[BlockLabel(ForwardTarget(forwarder))];
        }
    }

    /// Whether *block* only branches on with no moves on the way: it is not emitted, and jumps to it go where it leads.
    [[nodiscard]] bool IsForwarder(const MirBlock *block)
    {
        const MirInstruction *first = block->GetFirst();
        if (block == _function->GetBlocks()[0] || block->Merge.Kind == MirMergeKind::Loop || first == nullptr ||
            first->GetOp() != MirOp::Branch)
        {
            return false;
        }
        const MirBlock *target = first->GetTargets()[0];
        return target != block && (!HasPhis(target) || EdgeIsFree(block, target));
    }

    [[nodiscard]] static const MirBlock *ForwardTarget(const MirBlock *forwarder)
    {
        return forwarder->GetFirst()->GetTargets()[0];
    }

    /// Where a jump from *from* to *to* goes, past a forwarder, and the block the target's phis see it come from.
    struct Threaded
    {
        const MirBlock *Block;
        const MirBlock *From;
    };

    [[nodiscard]] Threaded Thread(const MirBlock *from, const MirBlock *to) const
    {
        return _forwarders.Contains(to) ? Threaded{ForwardTarget(to), to} : Threaded{to, from};
    }

    void SetPosition(const MirInstruction *instruction)
    {
        _location = instruction->Location;
        MarkLine();
    }

    uint32 Temp(uint32 slots)
    {
        const uint32 first = _tempNext;
        _tempNext += slots == 0 ? 1 : slots;
        Reserve(_tempNext);
        return first;
    }

    void Reserve(uint32 slots)
    {
        _frameSlots = slots > _frameSlots ? slots : _frameSlots;
    }

    void Fail(StringView message)
    {
        if (!_failed)
        {
            _builder->Error(Format("{0}: {1}", _function->GetName(), message));
        }
        _failed = true;
    }

    /// Adds a line table row when the source position changes.
    void MarkLine()
    {
        if (!_builder->GetOptions().DebugInfo || _location.Line == 0 || _location.File == 0)
        {
            return;
        }
        const ScriptLineEntry entry{static_cast<uint32>(_code.Count()), _location.File - 1, _location.Line,
                                    _location.Column};
        if (entry.File == _lastLine.File && entry.Line == _lastLine.Line && entry.Column == _lastLine.Column)
        {
            return;
        }
        const ScriptLineEntry previous = _lastLine.Line == ~0u ? ScriptLineEntry{} : _lastLine;
        AppendScriptLineEntry(_record->LineTable, previous, entry);
        _lastLine = entry;
    }

    void Word(uint32 word)
    {
        _code.Add(word);
    }

    bool CheckRegister(uint32 reg)
    {
        if (reg > 0xFFFF)
        {
            Fail("the function needs more than 65536 registers");
            return false;
        }
        return true;
    }

    void Trailing(ScriptOp op, std::initializer_list<uint32> trailing)
    {
        MARLIN_ASSERT(trailing.size() == GetScriptOpInfo(op).TrailingWords);
        for (const uint32 word : trailing)
        {
            Word(word);
        }
    }

    void Emit(ScriptOp op)
    {
        Word(static_cast<uint32>(op));
    }

    void EmitABC(ScriptOp op, uint32 a, uint32 b, uint32 c, std::initializer_list<uint32> trailing = {})
    {
        if (!CheckRegister(a) || !CheckRegister(b) || !CheckRegister(c))
        {
            return;
        }
        if (a > 0xFF || b > 0xFF || c > 0xFF)
        {
            Word(static_cast<uint32>(ScriptOp::Wide) | ((a >> 8) << 8) | ((b >> 8) << 16) | ((c >> 8) << 24));
        }
        Word(static_cast<uint32>(op) | ((a & 0xFF) << 8) | ((b & 0xFF) << 16) | ((c & 0xFF) << 24));
        Trailing(op, trailing);
    }

    void EmitAB(ScriptOp op, uint32 a, uint32 b, std::initializer_list<uint32> trailing = {})
    {
        EmitABC(op, a, b, 0, trailing);
    }

    void EmitABx(ScriptOp op, uint32 a, uint32 bx, std::initializer_list<uint32> trailing = {})
    {
        if (!CheckRegister(a))
        {
            return;
        }
        if (a > 0xFF || bx > 0xFFFF)
        {
            Word(static_cast<uint32>(ScriptOp::Wide) | ((a >> 8) << 8) | ((bx >> 16) << 16));
        }
        Word(static_cast<uint32>(op) | ((a & 0xFF) << 8) | ((bx & 0xFFFF) << 16));
        Trailing(op, trailing);
    }

    void EmitAsBx(ScriptOp op, uint32 a, int64 sbx)
    {
        if (!CheckRegister(a))
        {
            return;
        }
        const uint32 bits = static_cast<uint32>(sbx);
        if (a > 0xFF || sbx < -32768 || sbx > 32767)
        {
            Word(static_cast<uint32>(ScriptOp::Wide) | ((a >> 8) << 8) | ((bits >> 16) << 16));
        }
        Word(static_cast<uint32>(op) | ((a & 0xFF) << 8) | ((bits & 0xFFFF) << 16));
    }

    uint32 NewLabel()
    {
        _labels.Add(NoSlot);
        return static_cast<uint32>(_labels.Count() - 1);
    }

    void Bind(uint32 label)
    {
        _labels[label] = static_cast<uint32>(_code.Count());
    }

    [[nodiscard]] uint32 BlockLabel(const MirBlock *block) const
    {
        return *_blockIndex.Find(block);
    }

    void Jump(uint32 label)
    {
        EmitJump(ScriptOp::Jump, 0, label);
    }

    void EmitJump(ScriptOp op, uint32 a, uint32 label)
    {
        Fixup fixup;
        fixup.Label = label;
        fixup.Id = _jumps++;
        fixup.Op = op;
        fixup.A = a;
        fixup.Wide = a > 0xFF || _wideJumps.Contains(fixup.Id);
        if (!CheckRegister(a))
        {
            return;
        }
        if (fixup.Wide)
        {
            Word(static_cast<uint32>(ScriptOp::Wide) | ((a >> 8) << 8));
        }
        fixup.Word = static_cast<uint32>(_code.Count());
        Word(static_cast<uint32>(op) | ((a & 0xFF) << 8));
        _fixups.Add(fixup);
    }

    /// A compare-and-jump of the registers *a* and *b*, or of *a* and *immediate* for the K forms, to *label*.
    void EmitCompareJump(ScriptOp op, uint32 a, uint32 b, uint32 immediate, uint32 label)
    {
        if (GetScriptOpInfo(op).TrailingWords == 2)
        {
            EmitAB(op, a, 0, {immediate, 0});
        }
        else
        {
            EmitAB(op, a, b, {0});
        }
        Fixup fixup;
        fixup.Word = static_cast<uint32>(_code.Count() - 1);
        fixup.Label = label;
        fixup.Id = _jumps++;
        fixup.Op = op;
        fixup.Trailing = true;
        _fixups.Add(fixup);
    }

    /// Patches the jumps; false when one did not reach, which is then made wide for the next attempt.
    bool ResolveJumps()
    {
        bool resolved = true;
        for (const Fixup &fixup : _fixups)
        {
            const uint32 target = _labels[fixup.Label];
            const int64 offset = static_cast<int64>(target) - static_cast<int64>(fixup.Word + 1);
            const uint32 bits = static_cast<uint32>(offset);
            uint32 &word = _code[fixup.Word];
            if (fixup.Trailing)
            {
                word = bits;
                continue;
            }
            if (fixup.Op == ScriptOp::Jump || fixup.Op == ScriptOp::Loop)
            {
                if (!fixup.Wide && (offset < -(1 << 23) || offset >= (1 << 23)))
                {
                    _wideJumps.Add(fixup.Id);
                    resolved = false;
                    continue;
                }
                word = static_cast<uint32>(fixup.Op) | ((bits & 0xFFFFFF) << 8);
                if (fixup.Wide)
                {
                    _code[fixup.Word - 1] = static_cast<uint32>(ScriptOp::Wide) | ((bits >> 24) << 16);
                }
                continue;
            }
            if (!fixup.Wide && (offset < -32768 || offset > 32767))
            {
                _wideJumps.Add(fixup.Id);
                resolved = false;
                continue;
            }
            word = static_cast<uint32>(fixup.Op) | ((fixup.A & 0xFF) << 8) | ((bits & 0xFFFF) << 16);
            if (fixup.Wide)
            {
                _code[fixup.Word - 1] =
                    static_cast<uint32>(ScriptOp::Wide) | ((fixup.A >> 8) << 8) | ((bits >> 16) << 16);
            }
        }
        return resolved;
    }

    [[nodiscard]] uint32 SlotOf(const MirValue *value) const
    {
        const uint32 id = IdOf(value);
        return id == NoSlot ? NoSlot : _registers[id].Slot;
    }

    /// The register holding *value*, materializing it into a temporary if it has none.
    uint32 Reg(MirValue *value)
    {
        const uint32 slot = SlotOf(value);
        if (slot != NoSlot)
        {
            return slot;
        }
        const uint32 temp = Temp(ScriptSlotsOf(value->GetType()));
        Materialize(value, temp);
        return temp;
    }

    /// The register *instruction*'s result goes to: its own, or a temporary when nothing uses it.
    uint32 Dst(const MirInstruction *instruction)
    {
        const uint32 slot = SlotOf(instruction);
        if (slot != NoSlot)
        {
            return slot;
        }
        return Temp(ScriptSlotsOf(instruction->GetType()));
    }

    void Copy(uint32 dst, uint32 src, uint32 slots)
    {
        if (dst == src || slots == 0)
        {
            return;
        }
        if (slots == 1)
        {
            EmitAB(ScriptOp::Move, dst, src);
            return;
        }
        if (dst > src && dst < src + slots)
        {
            for (uint32 s = slots; s-- > 0;)
            {
                EmitAB(ScriptOp::Move, dst + s, src + s);
            }
            return;
        }
        EmitABC(ScriptOp::MoveN, dst, src, slots);
    }

    /// Puts *value* in the registers from *dst*.
    void Materialize(MirValue *value, uint32 dst)
    {
        const uint32 slot = SlotOf(value);
        if (slot != NoSlot)
        {
            Copy(dst, slot, ScriptSlotsOf(value->GetType()));
            return;
        }
        switch (value->GetValueKind())
        {
        case MirValueKind::Constant:
            EmitConstant(static_cast<MirConstant *>(value), dst);
            return;
        case MirValueKind::Function:
            Fail("a function is used as a value");
            return;
        default:
            break;
        }
        const Address address = AddressOf(value);
        switch (address.Kind)
        {
        case AddressKind::Frame:
            EmitAB(ScriptOp::RefLocal, dst, address.Slot);
            if (address.Offset != 0)
            {
                EmitAB(ScriptOp::RefOffset, dst, dst, {address.Offset});
            }
            return;
        case AddressKind::Static:
            EmitABx(ScriptOp::RefStatic, dst, address.Offset);
            return;
        case AddressKind::Field:
            EmitAB(ScriptOp::RefField, dst, Reg(address.Object), {address.Offset});
            return;
        case AddressKind::Element:
            EmitABC(ScriptOp::RefElem, dst, Reg(address.Object), Reg(address.Index),
                    {ElementWord(value->GetType()->GetElement())});
            return;
        case AddressKind::Register:
            Fail("a value is used before it is computed");
            return;
        }
    }

    /// Writes scalar bits of *type* at *at*: registers keep narrow integers extended to 32 bits, memory does not.
    void WriteScalar(uint8 *at, const MirType *type, uint64 bits, bool registerForm)
    {
        uint32 size = type->Is(MirTypeKind::Bool) ? 4 : type->GetWidth() / 8;
        if (registerForm && size < 4)
        {
            size = 4;
            if (type->Is(MirTypeKind::Int) && type->IsSigned())
            {
                bits = static_cast<uint64>(static_cast<int64>(bits) << (64 - type->GetWidth()) >> (64 - type->GetWidth()));
            }
        }
        MemCopy(at, &bits, size);
    }

    void WriteConstant(uint8 *bytes, uint32 capacity, const MirConstant *constant, uint32 offset, bool registerForm)
    {
        const MirType *type = constant->GetType();
        if (constant->GetConstantKind() == MirConstantKind::Null || constant->GetConstantKind() == MirConstantKind::Undef)
        {
            return;
        }
        if (constant->GetConstantKind() == MirConstantKind::Scalar)
        {
            if (offset + 8 <= capacity || offset + type->GetSize() <= capacity)
            {
                WriteScalar(bytes + offset, type, constant->GetBits(), registerForm);
            }
            return;
        }
        const Span<MirConstant *const> elements = constant->GetElements();
        for (usize i = 0; i < elements.Count(); ++i)
        {
            uint32 at = offset;
            if (ElementAt(type, static_cast<uint32>(i), at) == nullptr)
            {
                break;
            }
            WriteConstant(bytes, capacity, elements[i], at, false);
        }
    }

    uint32 AddConstant(const ScriptSlot &slot)
    {
        if (const uint32 *found = _constants.Find(ConstantKey{slot}))
        {
            return *found;
        }
        const uint32 index = static_cast<uint32>(_record->Constants.Count());
        _record->Constants.Add(slot);
        _constants.Add(ConstantKey{slot}, index);
        return index;
    }

    void EmitConstant(MirConstant *constant, uint32 dst)
    {
        const MirType *type = constant->GetType();
        const uint32 slots = ScriptSlotsOf(type);
        // Registers are shared and not zeroed on entry, so an undefined value is made zero: a map may name it, and
        // a boxed struct compares its padding.
        if (constant->GetConstantKind() == MirConstantKind::Null ||
            constant->GetConstantKind() == MirConstantKind::Undef)
        {
            EmitAB(ScriptOp::Zero, dst, slots);
            return;
        }
        if (constant->GetConstantKind() == MirConstantKind::Scalar)
        {
            ScriptSlot slot;
            WriteScalar(slot.Bytes, type, constant->GetBits(), true);
            int64 value = 0;
            MemCopy(&value, slot.Bytes, 8);
            const bool small = type->Is(MirTypeKind::Int) || type->Is(MirTypeKind::Bool);
            if (small && value >= -32768 && value <= 32767)
            {
                EmitAsBx(ScriptOp::LoadI, dst, value);
                return;
            }
            if (value == 0)
            {
                EmitAB(ScriptOp::Zero, dst, 1);
                return;
            }
            EmitABx(ScriptOp::LoadK, dst, AddConstant(slot));
            return;
        }
        List<ScriptSlot> bytes;
        bytes.Resize(slots);
        WriteConstant(bytes.Data()->Bytes, slots * ScriptSlotBytes, constant, 0, false);
        bool zero = true;
        for (const ScriptSlot &slot : bytes)
        {
            zero = zero && slot == ScriptSlot{};
        }
        if (zero)
        {
            EmitAB(ScriptOp::Zero, dst, slots);
            return;
        }
        if (slots == 1)
        {
            EmitABx(ScriptOp::LoadK, dst, AddConstant(bytes[0]));
            return;
        }
        const uint32 first = static_cast<uint32>(_record->Constants.Count());
        _record->Constants.AddRange(bytes.AsSpan());
        EmitABx(ScriptOp::LoadKN, dst, first, {slots});
    }

    /// Re-extends a narrow integer register to its type after arithmetic.
    void Extend(ScriptNumber number, uint32 reg)
    {
        if (IsSmall(number))
        {
            EmitAB(ExtendOf(number), reg, reg);
        }
    }

    /// Makes *reg* hold the operand in the number arithmetic on *number* runs in: F16 becomes F32.
    void ToF32(ScriptNumber number, uint32 &reg)
    {
        if (number == ScriptNumber::F16)
        {
            const uint32 temp = Temp(1);
            EmitAB(ScriptOp::F16ToF32, temp, reg);
            reg = temp;
        }
    }

    void ScalarBinary(MirOp op, ScriptNumber number, bool checked, bool common, uint32 dst, uint32 a, uint32 b)
    {
        if (number == ScriptNumber::Bool)
        {
            switch (op)
            {
            case MirOp::LogicalAnd:
            case MirOp::BitAnd:
                EmitABC(ScriptOp::And, dst, a, b);
                return;
            case MirOp::LogicalOr:
            case MirOp::BitOr:
                EmitABC(ScriptOp::Or, dst, a, b);
                return;
            case MirOp::BitXor:
                EmitABC(ScriptOp::Xor, dst, a, b);
                return;
            default:
                Fail(Format("{0} does not apply to bools", GetMirOpName(op)).AsView());
                return;
            }
        }
        if (IsFloat(number))
        {
            const bool half = number == ScriptNumber::F16;
            const bool wide = number == ScriptNumber::F64;
            ToF32(number, a);
            ToF32(number, b);
            const uint32 result = half ? Temp(1) : dst;
            ScriptOp code = ScriptOp::Count;
            switch (op)
            {
            case MirOp::Add:
                code = wide ? ScriptOp::AddF64 : ScriptOp::AddF32;
                break;
            case MirOp::Sub:
                code = wide ? ScriptOp::SubF64 : ScriptOp::SubF32;
                break;
            case MirOp::Mul:
                code = wide ? ScriptOp::MulF64 : ScriptOp::MulF32;
                break;
            case MirOp::Div:
                code = wide ? ScriptOp::DivF64 : ScriptOp::DivF32;
                break;
            case MirOp::Rem:
                code = wide ? ScriptOp::RemF64 : ScriptOp::RemF32;
                break;
            default:
                Fail(Format("{0} does not apply to floats", GetMirOpName(op)).AsView());
                return;
            }
            EmitABC(code, result, a, b);
            if (half)
            {
                EmitAB(ScriptOp::F32ToF16, dst, result);
            }
            return;
        }
        const bool wide = Is64(number);
        const bool isSigned = IsSigned(number);
        const bool small = IsSmall(number);
        ScriptOp code = ScriptOp::Count;
        switch (op)
        {
        case MirOp::Add:
            code = checked && !small ? (wide ? (isSigned ? ScriptOp::CheckedAddI64 : ScriptOp::CheckedAddU64)
                                             : (isSigned ? ScriptOp::CheckedAddI32 : ScriptOp::CheckedAddU32))
                                     : (wide ? ScriptOp::AddI64 : ScriptOp::AddI32);
            break;
        case MirOp::Sub:
            code = checked && !small ? (wide ? (isSigned ? ScriptOp::CheckedSubI64 : ScriptOp::CheckedSubU64)
                                             : (isSigned ? ScriptOp::CheckedSubI32 : ScriptOp::CheckedSubU32))
                                     : (wide ? ScriptOp::SubI64 : ScriptOp::SubI32);
            break;
        case MirOp::Mul:
            code = checked && !small ? (wide ? (isSigned ? ScriptOp::CheckedMulI64 : ScriptOp::CheckedMulU64)
                                             : (isSigned ? ScriptOp::CheckedMulI32 : ScriptOp::CheckedMulU32))
                                     : (wide ? ScriptOp::MulI64 : ScriptOp::MulI32);
            break;
        case MirOp::Div:
            code = wide ? (isSigned ? (common ? ScriptOp::DivI64Common : ScriptOp::DivI64)
                                    : (common ? ScriptOp::DivU64Common : ScriptOp::DivU64))
                        : (isSigned ? (common ? ScriptOp::DivI32Common : ScriptOp::DivI32)
                                    : (common ? ScriptOp::DivU32Common : ScriptOp::DivU32));
            break;
        case MirOp::Rem:
            code = wide ? (isSigned ? (common ? ScriptOp::RemI64Common : ScriptOp::RemI64)
                                    : (common ? ScriptOp::RemU64Common : ScriptOp::RemU64))
                        : (isSigned ? (common ? ScriptOp::RemI32Common : ScriptOp::RemI32)
                                    : (common ? ScriptOp::RemU32Common : ScriptOp::RemU32));
            break;
        case MirOp::BitAnd:
        case MirOp::LogicalAnd:
            code = ScriptOp::And;
            break;
        case MirOp::BitOr:
        case MirOp::LogicalOr:
            code = ScriptOp::Or;
            break;
        case MirOp::BitXor:
            code = ScriptOp::Xor;
            break;
        case MirOp::ShiftLeft:
            code = wide ? ScriptOp::ShlI64 : ScriptOp::ShlI32;
            break;
        case MirOp::ShiftRight:
            code = wide ? (isSigned ? ScriptOp::ShrI64 : ScriptOp::ShrU64)
                        : (isSigned ? ScriptOp::ShrI32 : ScriptOp::ShrU32);
            break;
        default:
            Fail(Format("{0} does not apply to integers", GetMirOpName(op)).AsView());
            return;
        }
        EmitABC(code, dst, a, b);
        if (!small)
        {
            return;
        }
        if (checked && (op == MirOp::Add || op == MirOp::Sub || op == MirOp::Mul))
        {
            const ScriptNumber from = isSigned ? ScriptNumber::I32 : ScriptNumber::U32;
            EmitABC(ScriptOp::ConvChecked, dst, dst, (static_cast<uint32>(from) << 4) | static_cast<uint32>(number));
            return;
        }
        if (op == MirOp::Add || op == MirOp::Sub || op == MirOp::Mul || op == MirOp::ShiftLeft || op == MirOp::Div ||
            op == MirOp::Rem)
        {
            Extend(number, dst);
        }
    }

    void ScalarUnary(MirOp op, ScriptNumber number, bool checked, uint32 dst, uint32 src)
    {
        switch (op)
        {
        case MirOp::LogicalNot:
            EmitAB(ScriptOp::NotBool, dst, src);
            return;
        case MirOp::BitNot:
            if (number == ScriptNumber::Bool)
            {
                EmitAB(ScriptOp::NotBool, dst, src);
                return;
            }
            EmitAB(ScriptOp::Not, dst, src);
            Extend(number, dst);
            return;
        case MirOp::Negate:
            if (IsFloat(number))
            {
                if (number == ScriptNumber::F16)
                {
                    ToF32(number, src);
                    const uint32 temp = Temp(1);
                    EmitAB(ScriptOp::NegF32, temp, src);
                    EmitAB(ScriptOp::F32ToF16, dst, temp);
                    return;
                }
                EmitAB(number == ScriptNumber::F64 ? ScriptOp::NegF64 : ScriptOp::NegF32, dst, src);
                return;
            }
            if (Is64(number))
            {
                EmitAB(checked ? ScriptOp::CheckedNegI64 : ScriptOp::NegI64, dst, src);
                return;
            }
            EmitAB(checked && !IsSmall(number) ? ScriptOp::CheckedNegI32 : ScriptOp::NegI32, dst, src);
            if (checked && IsSmall(number))
            {
                EmitABC(ScriptOp::ConvChecked, dst, dst,
                        (static_cast<uint32>(ScriptNumber::I32) << 4) | static_cast<uint32>(number));
                return;
            }
            Extend(number, dst);
            return;
        default:
            Fail(Format("the script target cannot express {0}", GetMirOpName(op)).AsView());
            return;
        }
    }

    void ScalarCompare(MirOp op, ScriptNumber number, uint32 dst, uint32 a, uint32 b)
    {
        if (op == MirOp::Greater || op == MirOp::GreaterEqual)
        {
            const uint32 swap = a;
            a = b;
            b = swap;
            op = op == MirOp::Greater ? MirOp::Less : MirOp::LessEqual;
        }
        ToF32(number, a);
        ToF32(number, b);
        const bool isFloat = IsFloat(number);
        const bool wide = Is64(number);
        const bool isSigned = IsSigned(number);
        ScriptOp code = ScriptOp::Count;
        switch (op)
        {
        case MirOp::Equal:
            code = isFloat ? (wide ? ScriptOp::EqF64 : ScriptOp::EqF32) : (wide ? ScriptOp::EqI64 : ScriptOp::EqI32);
            break;
        case MirOp::NotEqual:
            code = isFloat ? (wide ? ScriptOp::NeF64 : ScriptOp::NeF32) : (wide ? ScriptOp::NeI64 : ScriptOp::NeI32);
            break;
        case MirOp::Less:
            code = isFloat  ? (wide ? ScriptOp::LtF64 : ScriptOp::LtF32)
                   : wide   ? (isSigned ? ScriptOp::LtI64 : ScriptOp::LtU64)
                            : (isSigned ? ScriptOp::LtI32 : ScriptOp::LtU32);
            break;
        case MirOp::LessEqual:
            code = isFloat  ? (wide ? ScriptOp::LeF64 : ScriptOp::LeF32)
                   : wide   ? (isSigned ? ScriptOp::LeI64 : ScriptOp::LeU64)
                            : (isSigned ? ScriptOp::LeI32 : ScriptOp::LeU32);
            break;
        default:
            Fail(Format("the script target cannot express {0}", GetMirOpName(op)).AsView());
            return;
        }
        EmitABC(code, dst, a, b);
    }

    void ScalarConvert(const MirType *from, const MirType *to, bool checked, uint32 dst, uint32 src)
    {
        const ScriptNumber source = ScriptNumberOf(from);
        const ScriptNumber target = ScriptNumberOf(to);
        if (source == target)
        {
            Copy(dst, src, 1);
            return;
        }
        if (target == ScriptNumber::Bool)
        {
            const uint32 zero = Temp(1);
            EmitAB(ScriptOp::Zero, zero, 1);
            ScalarCompare(MirOp::NotEqual, source == ScriptNumber::Bool ? ScriptNumber::U32 : source, dst, src, zero);
            return;
        }
        if (source == ScriptNumber::Bool)
        {
            ScalarConvert(_module->GetTypes().GetInt(32, false), to, false, dst, src);
            return;
        }
        if (IsFloat(source) && IsFloat(target))
        {
            if (source == ScriptNumber::F16)
            {
                EmitAB(ScriptOp::F16ToF32, dst, src);
                if (target == ScriptNumber::F64)
                {
                    EmitAB(ScriptOp::F32ToF64, dst, dst);
                }
                return;
            }
            if (target == ScriptNumber::F16)
            {
                if (source == ScriptNumber::F64)
                {
                    EmitAB(ScriptOp::F64ToF32, dst, src);
                    EmitAB(ScriptOp::F32ToF16, dst, dst);
                    return;
                }
                EmitAB(ScriptOp::F32ToF16, dst, src);
                return;
            }
            EmitAB(target == ScriptNumber::F64 ? ScriptOp::F32ToF64 : ScriptOp::F64ToF32, dst, src);
            return;
        }
        if (checked && (IsFloat(source) || !IsFloat(target)))
        {
            EmitABC(ScriptOp::ConvChecked, dst, src, (static_cast<uint32>(source) << 4) | static_cast<uint32>(target));
            return;
        }
        if (IsFloat(target))
        {
            const ScriptNumber wide = Widened(source);
            const bool to64 = target == ScriptNumber::F64;
            ScriptOp code = ScriptOp::Count;
            switch (wide)
            {
            case ScriptNumber::I32:
                code = to64 ? ScriptOp::I32ToF64 : ScriptOp::I32ToF32;
                break;
            case ScriptNumber::U32:
                code = to64 ? ScriptOp::U32ToF64 : ScriptOp::U32ToF32;
                break;
            case ScriptNumber::I64:
                code = to64 ? ScriptOp::I64ToF64 : ScriptOp::I64ToF32;
                break;
            default:
                code = to64 ? ScriptOp::U64ToF64 : ScriptOp::U64ToF32;
                break;
            }
            EmitAB(code, dst, src);
            if (target == ScriptNumber::F16)
            {
                EmitAB(ScriptOp::F32ToF16, dst, dst);
            }
            return;
        }
        if (IsFloat(source))
        {
            if (source == ScriptNumber::F16)
            {
                ToF32(source, src);
            }
            const bool from64 = source == ScriptNumber::F64;
            const ScriptNumber wide = Widened(target);
            ScriptOp code = ScriptOp::Count;
            switch (wide)
            {
            case ScriptNumber::I32:
                code = from64 ? ScriptOp::F64ToI32 : ScriptOp::F32ToI32;
                break;
            case ScriptNumber::U32:
                code = from64 ? ScriptOp::F64ToU32 : ScriptOp::F32ToU32;
                break;
            case ScriptNumber::I64:
                code = from64 ? ScriptOp::F64ToI64 : ScriptOp::F32ToI64;
                break;
            default:
                code = from64 ? ScriptOp::F64ToU64 : ScriptOp::F32ToU64;
                break;
            }
            EmitAB(code, dst, src);
            Extend(target, dst);
            return;
        }
        // Integer to integer: registers hold 32 bits for narrow and 32-bit integers, 64 for the rest.
        if (Is64(target) && !Is64(source))
        {
            EmitAB(IsSigned(source) ? ScriptOp::SExt32 : ScriptOp::ZExt32, dst, src);
            return;
        }
        if (IsSmall(target))
        {
            EmitAB(ExtendOf(target), dst, src);
            return;
        }
        Copy(dst, src, 1);
    }

    /// Runs *body* on each lane of *type*, with the matching lane of each operand in a register; scalars run once.
    template <typename TBody>
    void ForEachLane(const MirType *type, uint32 dst, Span<MirValue *const> operands, TBody &&body)
    {
        if (!type->Is(MirTypeKind::Vector))
        {
            List<uint32> registers;
            for (MirValue *operand : operands)
            {
                registers.Add(Reg(operand));
            }
            body(dst, registers.AsSpan());
            return;
        }
        List<uint32> sources;
        for (MirValue *operand : operands)
        {
            sources.Add(Reg(operand));
        }
        const MirType *lane = type->GetElement();
        for (uint32 i = 0; i < type->GetCount(); ++i)
        {
            const uint32 saved = _tempNext;
            List<uint32> lanes;
            for (usize o = 0; o < operands.Count(); ++o)
            {
                const MirType *operandType = operands[o]->GetType();
                if (!operandType->Is(MirTypeKind::Vector))
                {
                    lanes.Add(sources[o]);
                    continue;
                }
                const MirType *operandLane = operandType->GetElement();
                const uint32 t = Temp(1);
                EmitAB(ScriptOp::Extract, t, sources[o], {i * operandLane->GetSize(), ScriptReadAccessOf(operandLane)});
                lanes.Add(t);
            }
            const uint32 result = Temp(1);
            body(result, lanes.AsSpan());
            EmitAB(ScriptOp::Insert, dst, result, {i * lane->GetSize(), ScriptWriteAccessOf(lane, false, 0)});
            _tempNext = saved;
        }
    }

    /// The whole-register instruction for *op* on lanes of *lane*, or Count when there is none.
    [[nodiscard]] ScriptOp VectorOpOf(MirOp op, const MirType *lane) const
    {
        const ScriptNumber number = ScriptNumberOf(lane);
        switch (number)
        {
        case ScriptNumber::F32:
            switch (op)
            {
            case MirOp::Add:
                return ScriptOp::AddF32x4;
            case MirOp::Sub:
                return ScriptOp::SubF32x4;
            case MirOp::Mul:
                return ScriptOp::MulF32x4;
            case MirOp::Div:
                return ScriptOp::DivF32x4;
            case MirOp::Negate:
                return ScriptOp::NegF32x4;
            default:
                return ScriptOp::Count;
            }
        case ScriptNumber::F64:
            switch (op)
            {
            case MirOp::Add:
                return ScriptOp::AddF64x2;
            case MirOp::Sub:
                return ScriptOp::SubF64x2;
            case MirOp::Mul:
                return ScriptOp::MulF64x2;
            case MirOp::Div:
                return ScriptOp::DivF64x2;
            case MirOp::Negate:
                return ScriptOp::NegF64x2;
            default:
                return ScriptOp::Count;
            }
        case ScriptNumber::I32:
        case ScriptNumber::U32:
            switch (op)
            {
            case MirOp::Add:
                return ScriptOp::AddI32x4;
            case MirOp::Sub:
                return ScriptOp::SubI32x4;
            case MirOp::Mul:
                return ScriptOp::MulI32x4;
            case MirOp::Negate:
                return ScriptOp::NegI32x4;
            default:
                return ScriptOp::Count;
            }
        case ScriptNumber::I64:
        case ScriptNumber::U64:
            switch (op)
            {
            case MirOp::Add:
                return ScriptOp::AddI64x2;
            case MirOp::Sub:
                return ScriptOp::SubI64x2;
            case MirOp::Mul:
                return ScriptOp::MulI64x2;
            case MirOp::Negate:
                return ScriptOp::NegI64x2;
            default:
                return ScriptOp::Count;
            }
        default:
            return ScriptOp::Count;
        }
    }

    [[nodiscard]] static bool IsBitwise(MirOp op)
    {
        return op == MirOp::BitAnd || op == MirOp::BitOr || op == MirOp::BitXor || op == MirOp::BitNot ||
               op == MirOp::LogicalAnd || op == MirOp::LogicalOr;
    }

    [[nodiscard]] static ScriptOp BitwiseOp(MirOp op)
    {
        switch (op)
        {
        case MirOp::BitAnd:
        case MirOp::LogicalAnd:
            return ScriptOp::And;
        case MirOp::BitOr:
        case MirOp::LogicalOr:
            return ScriptOp::Or;
        case MirOp::BitXor:
            return ScriptOp::Xor;
        default:
            return ScriptOp::Not;
        }
    }

    /// Arithmetic, logic and comparisons, on scalars, vectors and matrices.
    /// An operation of a 32-bit int or float and a constant as a K form: AndI32K, ShlI32K, ShrI32K and ShrU32K with the
    /// constant on the right (either side for And), AddF32K and MulF32K (x - k as x + -k). False when it is not one.
    bool Immediate32(MirInstruction *instruction, uint32 dst)
    {
        const MirOp op = instruction->GetOp();
        const ScriptNumber number = ScriptNumberOf(instruction->GetType());
        MirValue *left = instruction->GetOperand(0);
        MirValue *right = instruction->GetOperand(1);
        const bool commutes = op == MirOp::BitAnd || op == MirOp::Add || op == MirOp::Mul;
        if (IsScalarConstant(left) && commutes && !IsScalarConstant(right))
        {
            MirValue *swap = left;
            left = right;
            right = swap;
        }
        if (IsScalarConstant(left) || !IsScalarConstant(right))
        {
            return false;
        }
        const auto *constant = static_cast<const MirConstant *>(right);
        ScriptSlot bits;
        WriteScalar(bits.Bytes, constant->GetType(), constant->GetBits(), true);
        uint32 value = 0;
        MemCopy(&value, bits.Bytes, 4);
        ScriptOp code = ScriptOp::Count;
        if (number == ScriptNumber::I32 || number == ScriptNumber::U32)
        {
            code = op == MirOp::BitAnd      ? ScriptOp::AndI32K
                   : op == MirOp::ShiftLeft ? ScriptOp::ShlI32K
                   : op == MirOp::ShiftRight
                       ? (number == ScriptNumber::I32 ? ScriptOp::ShrI32K : ScriptOp::ShrU32K)
                       : ScriptOp::Count;
        }
        else if (number == ScriptNumber::F32)
        {
            code = op == MirOp::Mul ? ScriptOp::MulF32K : op == MirOp::Add || op == MirOp::Sub ? ScriptOp::AddF32K
                                                                                               : ScriptOp::Count;
            value ^= op == MirOp::Sub ? 0x80000000u : 0u;
        }
        if (code == ScriptOp::Count)
        {
            return false;
        }
        EmitAB(code, dst, Reg(left), {value});
        return true;
    }

    /// Unchecked Add, Sub and Mul of an integer and a constant that fits the immediate: AddI32K and MulI32K, or
    /// AddI64K, and the other K forms of Immediate32. False when *instruction* is not one.
    bool Immediate(MirInstruction *instruction, uint32 dst)
    {
        const MirOp op = instruction->GetOp();
        const MirType *type = instruction->GetType();
        if (instruction->Checked || instruction->GetOperands().Count() != 2)
        {
            return false;
        }
        if (type->Is(MirTypeKind::Float) || op == MirOp::BitAnd || op == MirOp::ShiftLeft || op == MirOp::ShiftRight)
        {
            return Immediate32(instruction, dst);
        }
        if ((op != MirOp::Add && op != MirOp::Sub && op != MirOp::Mul) || !type->Is(MirTypeKind::Int))
        {
            return false;
        }
        MirValue *left = instruction->GetOperand(0);
        MirValue *right = instruction->GetOperand(1);
        if (IsScalarConstant(left) == IsScalarConstant(right) || (IsScalarConstant(left) && op == MirOp::Sub))
        {
            return false;
        }
        MirValue *variable = IsScalarConstant(left) ? right : left;
        const auto *constant = static_cast<const MirConstant *>(IsScalarConstant(left) ? left : right);
        ScriptSlot bits;
        WriteScalar(bits.Bytes, type, constant->GetBits(), true);
        const ScriptNumber number = ScriptNumberOf(type);
        if (Is64(number))
        {
            int64 value = 0;
            MemCopy(&value, bits.Bytes, 8);
            if (op == MirOp::Sub && value != NumericLimits<int64>::Min())
            {
                value = -value;
            }
            else if (op != MirOp::Add)
            {
                return false;
            }
            if (value != static_cast<int64>(static_cast<int32>(value)))
            {
                return false;
            }
            EmitAB(ScriptOp::AddI64K, dst, Reg(variable), {static_cast<uint32>(static_cast<int32>(value))});
            return true;
        }
        uint32 value = 0;
        MemCopy(&value, bits.Bytes, 4);
        EmitAB(op == MirOp::Mul ? ScriptOp::MulI32K : ScriptOp::AddI32K, dst, Reg(variable),
               {op == MirOp::Sub ? 0u - value : value});
        Extend(number, dst);
        return true;
    }

    void Elementwise(MirInstruction *instruction)
    {
        const MirOp op = instruction->GetOp();
        const MirType *type = instruction->GetType();
        const MirType *operandType = instruction->GetOperand(0)->GetType();
        const bool compare = op == MirOp::Equal || op == MirOp::NotEqual || op == MirOp::Less ||
                             op == MirOp::LessEqual || op == MirOp::Greater || op == MirOp::GreaterEqual;
        const uint32 dst = Dst(instruction);
        if (compare && (operandType->Is(MirTypeKind::Reference) || operandType->Is(MirTypeKind::Pointer)))
        {
            ScalarCompare(op, ScriptNumber::U64, dst, Reg(instruction->GetOperand(0)), Reg(instruction->GetOperand(1)));
            return;
        }
        const bool composite = operandType->Is(MirTypeKind::Vector) || operandType->Is(MirTypeKind::Matrix);
        if (!composite && Immediate(instruction, dst))
        {
            return;
        }
        if (composite && !compare)
        {
            const MirType *lane = ScalarOf(operandType);
            const ScriptOp whole = IsBitwise(op) && (op != MirOp::BitNot || lane->Is(MirTypeKind::Int))
                                       ? BitwiseOp(op)
                                       : VectorOpOf(op, lane);
            const bool checked = instruction->Checked && lane->Is(MirTypeKind::Int);
            if (whole != ScriptOp::Count && !checked && op != MirOp::LogicalNot)
            {
                const uint32 a = Reg(instruction->GetOperand(0));
                const uint32 b = instruction->GetOperands().Count() > 1 ? Reg(instruction->GetOperand(1)) : 0;
                for (uint32 s = 0; s < ScriptSlotsOf(type); ++s)
                {
                    if (instruction->GetOperands().Count() > 1)
                    {
                        EmitABC(whole, dst + s, a + s, b + s);
                    }
                    else
                    {
                        EmitAB(whole, dst + s, a + s);
                    }
                }
                return;
            }
            if (operandType->Is(MirTypeKind::Matrix))
            {
                Fail(Format("{0} on matrices of this type is not supported", GetMirOpName(op)).AsView());
                return;
            }
            if (op == MirOp::BitNot && IsSmall(ScriptNumberOf(lane)))
            {
                Fail("~ on vectors of 8- and 16-bit integers is not supported");
                return;
            }
        }
        if (operandType->Is(MirTypeKind::Matrix))
        {
            Fail(Format("{0} on matrices of this type is not supported", GetMirOpName(op)).AsView());
            return;
        }
        ForEachLane(type, dst, instruction->GetOperands(), [&](uint32 result, Span<const uint32> lanes) {
            ScalarOperation(instruction, result, lanes);
        });
    }

    void ScalarOperation(MirInstruction *instruction, uint32 dst, Span<const uint32> operands)
    {
        const MirOp op = instruction->GetOp();
        const ScriptNumber number = ScriptNumberOf(ScalarOf(instruction->GetOperand(0)->GetType()));
        if (operands.Count() == 1)
        {
            ScalarUnary(op, number, instruction->Checked, dst, operands[0]);
            return;
        }
        if (op == MirOp::Equal || op == MirOp::NotEqual || op == MirOp::Less || op == MirOp::LessEqual ||
            op == MirOp::Greater || op == MirOp::GreaterEqual)
        {
            ScalarCompare(op, number == ScriptNumber::Bool ? ScriptNumber::U32 : number, dst, operands[0], operands[1]);
            return;
        }
        ScalarBinary(op, number, instruction->Checked, _function->Common, dst, operands[0], operands[1]);
    }

    /// The register holding row *index* of the matrix in *reg*: in place when rows fill whole registers.
    uint32 Row(const MirType *matrix, uint32 reg, uint32 index)
    {
        if (matrix->GetStride() % ScriptSlotBytes == 0)
        {
            return reg + index * matrix->GetStride() / ScriptSlotBytes;
        }
        const MirType *row = matrix->GetElement();
        const uint32 t = Temp(ScriptSlotsOf(row));
        EmitAB(ScriptOp::Extract, t, reg, {index * matrix->GetStride(), MakeScriptAccess(ScriptAccess::Bytes, row->GetSize())});
        return t;
    }

    void Splat(const MirType *lane, uint32 dst, uint32 scalar)
    {
        EmitAB(lane->GetSize() == 8 ? ScriptOp::Splat64 : ScriptOp::Splat32, dst, scalar);
    }

    /// The sum of the rows of *matrix* (in *m*) each scaled by the matching lane of *weights* (in *w*): the
    /// product of a row vector and a matrix.
    void WeightedRows(const MirType *result, const MirType *matrix, uint32 m, uint32 w, uint32 dst)
    {
        const MirType *lane = ScalarOf(result);
        const uint32 slots = ScriptSlotsOf(result);
        const ScriptOp mul = VectorOpOf(MirOp::Mul, lane);
        const ScriptOp add = VectorOpOf(MirOp::Add, lane);
        if (mul == ScriptOp::Count || add == ScriptOp::Count)
        {
            Fail("matrix products on 8- and 16-bit lanes are not supported");
            return;
        }
        const uint32 accumulator = Temp(slots);
        const uint32 weight = Temp(1);
        const uint32 splat = Temp(1);
        const uint32 product = Temp(slots);
        for (uint32 i = 0; i < matrix->GetRows(); ++i)
        {
            const uint32 saved = _tempNext;
            const uint32 row = Row(matrix, m, i);
            EmitAB(ScriptOp::Extract, weight, w, {i * lane->GetSize(), ScriptReadAccessOf(lane)});
            Splat(lane, splat, weight);
            for (uint32 s = 0; s < slots; ++s)
            {
                EmitABC(mul, i == 0 ? accumulator + s : product + s, row + s, splat);
                if (i != 0)
                {
                    EmitABC(add, accumulator + s, accumulator + s, product + s);
                }
            }
            _tempNext = saved;
        }
        Copy(dst, accumulator, slots);
    }

    void MatrixOperation(MirInstruction *instruction)
    {
        const MirType *type = instruction->GetType();
        const uint32 dst = Dst(instruction);
        MirValue *left = instruction->GetOperand(0);
        MirValue *right = instruction->GetOperand(1);
        switch (instruction->GetOp())
        {
        case MirOp::MatrixTimesScalar:
        case MirOp::VectorTimesScalar: {
            const MirType *lane = ScalarOf(type);
            const ScriptOp mul = VectorOpOf(MirOp::Mul, lane);
            if (mul == ScriptOp::Count)
            {
                if (type->Is(MirTypeKind::Matrix))
                {
                    Fail("matrix products on 8- and 16-bit lanes are not supported");
                    return;
                }
                ForEachLane(type, dst, instruction->GetOperands(), [&](uint32 result, Span<const uint32> lanes) {
                    ScalarBinary(MirOp::Mul, ScriptNumberOf(lane), false, false, result, lanes[0], lanes[1]);
                });
                return;
            }
            const uint32 a = Reg(left);
            const uint32 splat = Temp(1);
            Splat(lane, splat, Reg(right));
            for (uint32 s = 0; s < ScriptSlotsOf(type); ++s)
            {
                EmitABC(mul, dst + s, a + s, splat);
            }
            return;
        }
        case MirOp::MatrixTimesVector: {
            const MirType *matrix = left->GetType();
            const MirType *lane = ScalarOf(type);
            const uint32 m = Reg(left);
            const uint32 v = Reg(right);
            for (uint32 i = 0; i < matrix->GetRows(); ++i)
            {
                const uint32 saved = _tempNext;
                const uint32 row = Row(matrix, m, i);
                const uint32 dot = Temp(1);
                EmitABC(ScriptOp::Math, dot, row, v,
                        {MakeScriptMath(ScriptMathOp::Dot, ScriptNumberOf(lane), matrix->GetCount(), 0)});
                EmitAB(ScriptOp::Insert, dst, dot, {i * lane->GetSize(), ScriptWriteAccessOf(lane, false, 0)});
                _tempNext = saved;
            }
            return;
        }
        case MirOp::VectorTimesMatrix:
            WeightedRows(type, right->GetType(), Reg(right), Reg(left), dst);
            return;
        case MirOp::MatrixTimesMatrix: {
            const MirType *leftType = left->GetType();
            const uint32 a = Reg(left);
            const uint32 b = Reg(right);
            const MirType *rowType = type->GetElement();
            const bool aligned = type->GetStride() % ScriptSlotBytes == 0;
            for (uint32 i = 0; i < type->GetRows(); ++i)
            {
                const uint32 saved = _tempNext;
                const uint32 weights = Row(leftType, a, i);
                const uint32 row =
                    aligned ? dst + i * type->GetStride() / ScriptSlotBytes : Temp(ScriptSlotsOf(rowType));
                WeightedRows(rowType, right->GetType(), b, weights, row);
                if (!aligned)
                {
                    EmitAB(ScriptOp::Insert, dst, row,
                           {i * type->GetStride(), MakeScriptAccess(ScriptAccess::Bytes, rowType->GetSize())});
                }
                _tempNext = saved;
            }
            return;
        }
        default:
            Fail("unexpected matrix operation");
            return;
        }
    }

    void Intrinsic(MirInstruction *instruction)
    {
        const MirIntrinsic intrinsic = instruction->GetIntrinsic();
        if (intrinsic == MirIntrinsic::DebugDeclare)
        {
            return;
        }
        if (intrinsic == MirIntrinsic::Transpose)
        {
            Transpose(instruction);
            return;
        }
        bool ok = false;
        const ScriptMathOp op = MathOpOf(intrinsic, ok);
        if (!ok || instruction->GetOperands().Count() > 3 || instruction->GetOperands().IsEmpty())
        {
            Fail(Format("the script target cannot express {0}", GetMirIntrinsicName(intrinsic)).AsView());
            return;
        }
        const MirType *type = instruction->GetOperand(0)->GetType();
        const bool matrix = type->Is(MirTypeKind::Matrix);
        const MirType *lane = ScalarOf(type);
        ScriptNumber number = ScriptNumberOf(lane);
        const uint32 lanes = matrix ? type->GetRows() : LanesOf(type);
        const uint32 dst = Dst(instruction);
        if (IsSmall(number) || number == ScriptNumber::F16)
        {
            if (lanes != 1)
            {
                Fail(Format("{0} on vectors of 8- and 16-bit values is not supported", GetMirIntrinsicName(intrinsic))
                         .AsView());
                return;
            }
            List<uint32> registers;
            for (MirValue *operand : instruction->GetOperands())
            {
                uint32 reg = Reg(operand);
                if (operand->GetType() == type)
                {
                    ToF32(number, reg);
                }
                registers.Add(reg);
            }
            const ScriptNumber wide = Widened(number);
            const bool sameResult = instruction->GetType() == type;
            const uint32 result = sameResult && number == ScriptNumber::F16 ? Temp(1) : dst;
            EmitMath(op, wide, 1, result, registers.AsSpan());
            if (sameResult && number == ScriptNumber::F16)
            {
                EmitAB(ScriptOp::F32ToF16, dst, result);
            }
            else if (sameResult)
            {
                Extend(number, dst);
            }
            return;
        }
        List<uint32> registers;
        for (MirValue *operand : instruction->GetOperands())
        {
            registers.Add(Reg(operand));
        }
        EmitMath(op, number, lanes, dst, registers.AsSpan());
    }

    void EmitMath(ScriptMathOp op, ScriptNumber number, uint32 lanes, uint32 dst, Span<const uint32> operands)
    {
        const uint32 b = operands[0];
        const uint32 c = operands.Count() > 1 ? operands[1] : 0;
        const uint32 d = operands.Count() > 2 ? operands[2] : 0;
        if (d > 0xFFFF)
        {
            Fail("the function needs more than 65536 registers");
            return;
        }
        // The common F32 operations have instructions of their own, which skip decoding the Math word.
        if (number == ScriptNumber::F32)
        {
            ScriptOp dedicated = ScriptOp::Count;
            switch (op)
            {
            case ScriptMathOp::Min:
                dedicated = lanes == 1 ? ScriptOp::MinF32 : ScriptOp::Count;
                break;
            case ScriptMathOp::Max:
                dedicated = lanes == 1 ? ScriptOp::MaxF32 : ScriptOp::Count;
                break;
            case ScriptMathOp::Abs:
                dedicated = lanes == 1 ? ScriptOp::AbsF32 : ScriptOp::Count;
                break;
            case ScriptMathOp::Sqrt:
                dedicated = lanes == 1 ? ScriptOp::SqrtF32 : ScriptOp::Count;
                break;
            case ScriptMathOp::Floor:
                dedicated = lanes == 1 ? ScriptOp::FloorF32 : ScriptOp::Count;
                break;
            case ScriptMathOp::Dot:
                dedicated = lanes == 3 ? ScriptOp::DotF32x3 : lanes == 4 ? ScriptOp::DotF32x4 : ScriptOp::Count;
                break;
            case ScriptMathOp::Length:
                dedicated = lanes == 3 ? ScriptOp::LengthF32x3 : ScriptOp::Count;
                break;
            case ScriptMathOp::Cross:
                dedicated = lanes == 3 ? ScriptOp::CrossF32x3 : ScriptOp::Count;
                break;
            case ScriptMathOp::Normalize:
                dedicated = lanes == 3 ? ScriptOp::NormalizeF32x3 : ScriptOp::Count;
                break;
            default:
                break;
            }
            if (dedicated != ScriptOp::Count)
            {
                EmitABC(dedicated, dst, b, c);
                return;
            }
        }
        EmitABC(ScriptOp::Math, dst, b, c, {MakeScriptMath(op, number, lanes, d)});
    }

    void Transpose(MirInstruction *instruction)
    {
        const MirType *source = instruction->GetOperand(0)->GetType();
        const MirType *type = instruction->GetType();
        const MirType *lane = ScalarOf(type);
        const uint32 src = Reg(instruction->GetOperand(0));
        const uint32 dst = Dst(instruction);
        const uint32 t = Temp(1);
        for (uint32 row = 0; row < source->GetRows(); ++row)
        {
            for (uint32 column = 0; column < source->GetCount(); ++column)
            {
                EmitAB(ScriptOp::Extract, t, src,
                       {row * source->GetStride() + column * lane->GetSize(), ScriptReadAccessOf(lane)});
                EmitAB(ScriptOp::Insert, dst, t,
                       {column * type->GetStride() + row * lane->GetSize(), ScriptWriteAccessOf(lane, false, 0)});
            }
        }
    }

    void Load(MirInstruction *instruction)
    {
        const MirType *type = instruction->GetType();
        MirValue *pointer = instruction->GetOperand(0);
        const Address address = AddressOf(pointer);
        const uint32 access = ScriptReadAccessOf(type);
        const uint32 dst = Dst(instruction);
        // 4- and 8-byte values have instructions of their own, which need no access word.
        const ScriptAccess kind = GetScriptAccess(access);
        const bool b32 = kind == ScriptAccess::B32;
        const bool fast = b32 || kind == ScriptAccess::B64;
        switch (address.Kind)
        {
        case AddressKind::Frame:
            EmitAB(ScriptOp::Extract, dst, address.Slot, {address.Offset, access});
            return;
        case AddressKind::Field:
            if (fast)
            {
                EmitAB(b32 ? ScriptOp::LoadField32 : ScriptOp::LoadField64, dst, Reg(address.Object), {address.Offset});
                return;
            }
            EmitAB(ScriptOp::LoadField, dst, Reg(address.Object), {address.Offset, access});
            return;
        case AddressKind::Element:
            if (fast)
            {
                EmitABC(b32 ? ScriptOp::LoadElem32 : ScriptOp::LoadElem64, dst, Reg(address.Object), Reg(address.Index),
                        {ElementWord(type)});
                return;
            }
            EmitABC(ScriptOp::LoadElem, dst, Reg(address.Object), Reg(address.Index), {ElementWord(type), access});
            return;
        default:
            if (fast)
            {
                EmitAB(b32 ? ScriptOp::Load32 : ScriptOp::Load64, dst, Reg(pointer));
                return;
            }
            EmitAB(ScriptOp::Load, dst, Reg(pointer), {access});
            return;
        }
    }

    /// The store instruction of *access* among the 32-bit, 64-bit and reference forms of a store, or Count when it
    /// needs the generic one.
    [[nodiscard]] static ScriptOp FastStore(uint32 access, ScriptOp b32, ScriptOp b64, ScriptOp reference)
    {
        switch (GetScriptAccess(access))
        {
        case ScriptAccess::B32:
            return b32;
        case ScriptAccess::B64:
            return b64;
        case ScriptAccess::Reference:
            return reference;
        default:
            return ScriptOp::Count;
        }
    }

    void Store(MirInstruction *instruction)
    {
        MirValue *pointer = instruction->GetOperand(0);
        MirValue *value = instruction->GetOperand(1);
        const MirType *type = value->GetType();
        const Address address = AddressOf(pointer);
        const uint32 source = Reg(value);
        if (address.Kind == AddressKind::Frame)
        {
            EmitAB(ScriptOp::Insert, address.Slot, source, {address.Offset, ScriptWriteAccessOf(type, false, 0)});
            return;
        }
        const uint32 access = ScriptWriteAccessOf(type, true, ScriptHasReferences(type) ? _builder->ShapeOf(type) : 0);
        switch (address.Kind)
        {
        case AddressKind::Field: {
            const ScriptOp fast =
                FastStore(access, ScriptOp::StoreField32, ScriptOp::StoreField64, ScriptOp::StoreFieldRef);
            if (fast != ScriptOp::Count)
            {
                EmitAB(fast, Reg(address.Object), source, {address.Offset});
                return;
            }
            EmitAB(ScriptOp::StoreField, Reg(address.Object), source, {address.Offset, access});
            return;
        }
        case AddressKind::Element: {
            const ScriptOp fast =
                FastStore(access, ScriptOp::StoreElem32, ScriptOp::StoreElem64, ScriptOp::StoreElemRef);
            if (fast != ScriptOp::Count)
            {
                EmitABC(fast, Reg(address.Object), Reg(address.Index), source, {ElementWord(type)});
                return;
            }
            EmitABC(ScriptOp::StoreElem, Reg(address.Object), Reg(address.Index), source, {ElementWord(type), access});
            return;
        }
        default: {
            const ScriptOp fast = FastStore(access, ScriptOp::Store32, ScriptOp::Store64, ScriptOp::StoreRef);
            if (fast != ScriptOp::Count)
            {
                EmitAB(fast, Reg(pointer), source);
                return;
            }
            EmitAB(ScriptOp::Store, Reg(pointer), source, {access});
            return;
        }
        }
    }

    void AccessChain(MirInstruction *instruction)
    {
        const uint32 dst = Dst(instruction);
        Materialize(instruction->GetOperand(0), dst);
        const MirType *type = instruction->GetOperand(0)->GetType()->GetElement();
        uint32 pending = 0;
        auto flush = [&] {
            if (pending != 0)
            {
                EmitAB(ScriptOp::RefOffset, dst, dst, {pending});
                pending = 0;
            }
        };
        for (usize i = 1; i < instruction->GetOperands().Count() && type != nullptr; ++i)
        {
            MirValue *index = instruction->GetOperand(i);
            if (index->GetValueKind() == MirValueKind::Constant &&
                static_cast<MirConstant *>(index)->GetConstantKind() == MirConstantKind::Scalar)
            {
                type = ElementAt(type, static_cast<uint32>(static_cast<MirConstant *>(index)->GetInt()), pending);
                continue;
            }
            flush();
            const uint32 count = type->Is(MirTypeKind::Matrix) ? type->GetRows() : type->GetCount();
            EmitABC(_function->Common ? ScriptOp::RefIndexClamp : ScriptOp::RefIndex, dst, dst, Reg(index),
                    {StrideOf(type), count});
            type = type->GetElement();
        }
        if (type == nullptr)
        {
            Fail("an access chain indexes past its type");
            return;
        }
        flush();
    }

    // A call's window starts where AllocateRegisters put it, above every value live across the call; the arguments' own
    // registers may lie in it, so they are moved in all at once, after the operands without registers are computed into
    // temporaries (which lie above every window) and before the constants are written.
    template <typename TCall> void CallWindow(MirInstruction *instruction, TCall &&call)
    {
        const uint32 base = *_callBases.Find(instruction);
        const usize first = FirstWindowOperand(instruction);
        Reserve(base + WindowSlotsOf(instruction));
        List<SlotMove> moves;
        List<uint32> constants;
        uint32 slots = 0;
        for (usize i = first; i < instruction->GetOperands().Count(); ++i)
        {
            MirValue *operand = instruction->GetOperand(i);
            const uint32 operandSlots = ScriptSlotsOf(operand->GetType());
            uint32 src = SlotOf(operand);
            if (src == NoSlot && operand->GetValueKind() == MirValueKind::Constant)
            {
                constants.Add(static_cast<uint32>(i));
                slots += operandSlots;
                continue;
            }
            if (src == NoSlot)
            {
                src = Temp(operandSlots);
                Materialize(operand, src);
            }
            for (uint32 s = 0; s < operandSlots; ++s)
            {
                if (src + s != base + slots + s)
                {
                    moves.Add({base + slots + s, src + s});
                }
            }
            slots += operandSlots;
        }
        ParallelMoves(moves);
        uint32 at = base;
        for (usize i = first, next = 0; i < instruction->GetOperands().Count(); ++i)
        {
            MirValue *operand = instruction->GetOperand(i);
            if (next < constants.Count() && constants[next] == i)
            {
                Materialize(operand, at);
                ++next;
            }
            at += ScriptSlotsOf(operand->GetType());
        }
        // The window's registers belong to the callee's frame now, which the collector scans instead: the map leaves
        // out the arguments whose own registers lie in it.
        List<uint32> values;
        for (const uint32 id : *_safepointValues.Find(instruction))
        {
            if (_registers[id].Slot + _registers[id].Slots <= base)
            {
                values.Add(id);
            }
        }
        RecordSafepoint(values);
        call(base, slots);
        if (const uint32 slot = SlotOf(instruction); slot != NoSlot)
        {
            Copy(slot, base, ScriptSlotsOf(instruction->GetType()));
        }
    }

    [[nodiscard]] bool HasPhis(const MirBlock *block) const
    {
        const MirInstruction *first = block->GetFirst();
        return first != nullptr && first->GetOp() == MirOp::Phi;
    }

    /// Emits *moves*, register to register, as if all at once: a move waits while another still reads its target, and a
    /// cycle goes through a temporary.
    void ParallelMoves(List<SlotMove> &moves)
    {
        while (!moves.IsEmpty())
        {
            bool progress = false;
            for (usize i = 0; i < moves.Count(); ++i)
            {
                bool blocked = false;
                for (usize j = 0; j < moves.Count() && !blocked; ++j)
                {
                    blocked = j != i && moves[j].Src == moves[i].Dst;
                }
                if (!blocked)
                {
                    EmitAB(ScriptOp::Move, moves[i].Dst, moves[i].Src);
                    moves.RemoveAt(i);
                    progress = true;
                    break;
                }
            }
            if (!progress)
            {
                const uint32 saved = Temp(1);
                const uint32 dst = moves[0].Dst;
                EmitAB(ScriptOp::Move, saved, dst);
                for (SlotMove &move : moves)
                {
                    move.Src = move.Src == dst ? saved : move.Src;
                }
            }
        }
    }

    void EdgeMoves(const MirBlock *from, const MirBlock *to)
    {
        _tempNext = _fixedSlots;
        List<SlotMove> moves;
        List<MirInstruction *> direct;
        for (MirInstruction *phi : *to)
        {
            if (phi->GetOp() != MirOp::Phi)
            {
                break;
            }
            const uint32 dst = SlotOf(phi);
            if (dst == NoSlot)
            {
                continue;
            }
            for (usize i = 0; i < phi->GetTargets().Count(); ++i)
            {
                if (phi->GetTargets()[i] != from)
                {
                    continue;
                }
                MirValue *value = phi->GetOperand(i);
                const uint32 slots = ScriptSlotsOf(value->GetType());
                uint32 src = SlotOf(value);
                const AddressKind kind = value->GetValueKind() == MirValueKind::Constant ? AddressKind::Frame
                                         : src == NoSlot                                 ? AddressOf(value).Kind
                                                                                         : AddressKind::Register;
                if (src == NoSlot && (kind == AddressKind::Frame || kind == AddressKind::Static))
                {
                    direct.Add(phi);
                    break;
                }
                if (src == NoSlot)
                {
                    src = Temp(slots);
                    Materialize(value, src);
                }
                for (uint32 s = 0; s < slots; ++s)
                {
                    if (dst + s != src + s)
                    {
                        moves.Add({dst + s, src + s});
                    }
                }
                break;
            }
        }
        ParallelMoves(moves);
        for (MirInstruction *phi : direct)
        {
            for (usize i = 0; i < phi->GetTargets().Count(); ++i)
            {
                if (phi->GetTargets()[i] == from)
                {
                    Materialize(phi->GetOperand(i), SlotOf(phi));
                    break;
                }
            }
        }
    }

    void BranchTo(const MirBlock *to)
    {
        EdgeMoves(_function->GetBlocks()[_block], to);
        const MirBlock *target = Thread(_function->GetBlocks()[_block], to).Block;
        if (target == _next)
        {
            return;
        }
        // To a loop header: the header's safepoint, then past its Loop, in one instruction.
        if (const uint32 *body = _loopBodies.Find(target))
        {
            RecordSafepoint(*_headerValues.Find(target));
            EmitJump(ScriptOp::Loop, 0, *body);
            return;
        }
        Jump(BlockLabel(target));
    }

    /// Whether the edge from *from* to *to* needs no moves: every phi of *to* gets the value already in its registers.
    [[nodiscard]] bool EdgeIsFree(const MirBlock *from, const MirBlock *to)
    {
        for (MirInstruction *phi : *to)
        {
            if (phi->GetOp() != MirOp::Phi)
            {
                break;
            }
            const uint32 dst = SlotOf(phi);
            if (dst == NoSlot)
            {
                continue;
            }
            for (usize i = 0; i < phi->GetTargets().Count(); ++i)
            {
                if (phi->GetTargets()[i] != from)
                {
                    continue;
                }
                const MirValue *value = phi->GetOperand(i);
                if (value->GetValueKind() == MirValueKind::Constant || SlotOf(value) != dst)
                {
                    return false;
                }
                break;
            }
        }
        return true;
    }

    /// Whether *value* is a constant a K form can compare with, and its immediate: the register bits of a scalar, or 0
    /// for null. With *wide* the comparison is 64-bit and the constant must survive sign extension from 32 bits.
    bool ImmediateOf(const MirValue *value, bool wide, uint32 &immediate)
    {
        if (value->GetValueKind() != MirValueKind::Constant)
        {
            return false;
        }
        const auto *constant = static_cast<const MirConstant *>(value);
        if (constant->GetConstantKind() == MirConstantKind::Null)
        {
            immediate = 0;
            return true;
        }
        if (constant->GetConstantKind() != MirConstantKind::Scalar)
        {
            return false;
        }
        ScriptSlot bits;
        WriteScalar(bits.Bytes, constant->GetType(), constant->GetBits(), true);
        if (!wide)
        {
            MemCopy(&immediate, bits.Bytes, 4);
            return true;
        }
        int64 full = 0;
        MemCopy(&full, bits.Bytes, 8);
        if (full != static_cast<int64>(static_cast<int32>(full)))
        {
            return false;
        }
        immediate = static_cast<uint32>(static_cast<int32>(full));
        return true;
    }

    /// Jumps to *label* when the comparison *compare* comes out *when*, with one compare-and-jump instruction.
    void EmitBranchCompare(const MirInstruction *compare, bool when, uint32 label)
    {
        MirOp op = compare->GetOp();
        MirValue *left = compare->GetOperand(0);
        MirValue *right = compare->GetOperand(1);
        const MirType *type = left->GetType();
        const bool address = type->Is(MirTypeKind::Reference) || type->Is(MirTypeKind::Pointer);
        const ScriptNumber exact = address ? ScriptNumber::U64 : ScriptNumberOf(type);
        const ScriptNumber number = exact == ScriptNumber::Bool ? ScriptNumber::U32 : exact;
        const bool isFloat = IsFloat(number);
        // Integers jump on the opposite comparison; floats test that theirs fails, since NaN fails both.
        bool fails = false;
        if (!when)
        {
            if (isFloat && op != MirOp::Equal && op != MirOp::NotEqual)
            {
                fails = true;
            }
            else
            {
                op = NegatedCompare(op);
            }
        }
        // A constant operand becomes the immediate of a K form: any comparison of values registers hold as 32-bit
        // integers that compare correctly as signed ones, the equality of other 32-bit values, and the equality of
        // 64-bit integers and references (with null) against a constant that fits 32 bits.
        const bool ordered = op != MirOp::Equal && op != MirOp::NotEqual;
        const bool int32Compare = exact == ScriptNumber::I8 || exact == ScriptNumber::I16 ||
                                  exact == ScriptNumber::I32 || exact == ScriptNumber::U8 || exact == ScriptNumber::U16;
        const bool any32 = int32Compare || exact == ScriptNumber::U32 || exact == ScriptNumber::Bool;
        const bool any64 = address || exact == ScriptNumber::I64 || exact == ScriptNumber::U64;
        uint32 immediate = 0;
        if (!isFloat && (ordered ? int32Compare : (any32 || any64)))
        {
            const bool leftImmediate = ImmediateOf(left, any64, immediate);
            if (leftImmediate != ImmediateOf(right, any64, immediate))
            {
                if (leftImmediate)
                {
                    MirValue *swap = left;
                    left = right;
                    right = swap;
                    op = MirroredCompare(op);
                }
                const ScriptOp code = any64 ? (op == MirOp::Equal ? ScriptOp::JumpEqI64K : ScriptOp::JumpNeI64K)
                                      : op == MirOp::Equal     ? ScriptOp::JumpEqI32K
                                      : op == MirOp::NotEqual  ? ScriptOp::JumpNeI32K
                                      : op == MirOp::Less      ? ScriptOp::JumpLtI32K
                                      : op == MirOp::LessEqual ? ScriptOp::JumpLeI32K
                                      : op == MirOp::Greater   ? ScriptOp::JumpGtI32K
                                                               : ScriptOp::JumpGeI32K;
                EmitCompareJump(code, Reg(left), 0, immediate, label);
                return;
            }
        }
        if (op == MirOp::Greater || op == MirOp::GreaterEqual)
        {
            MirValue *swap = left;
            left = right;
            right = swap;
            op = MirroredCompare(op);
        }
        const bool wide = Is64(number);
        const bool isSigned = IsSigned(number);
        ScriptOp code = ScriptOp::Count;
        switch (op)
        {
        case MirOp::Equal:
            code = isFloat ? (wide ? ScriptOp::JumpEqF64 : ScriptOp::JumpEqF32)
                           : (wide ? ScriptOp::JumpEqI64 : ScriptOp::JumpEqI32);
            break;
        case MirOp::NotEqual:
            code = isFloat ? (wide ? ScriptOp::JumpNeF64 : ScriptOp::JumpNeF32)
                           : (wide ? ScriptOp::JumpNeI64 : ScriptOp::JumpNeI32);
            break;
        case MirOp::Less:
            code = isFloat ? (wide ? (fails ? ScriptOp::JumpNotLtF64 : ScriptOp::JumpLtF64)
                                   : (fails ? ScriptOp::JumpNotLtF32 : ScriptOp::JumpLtF32))
                   : wide  ? (isSigned ? ScriptOp::JumpLtI64 : ScriptOp::JumpLtU64)
                           : (isSigned ? ScriptOp::JumpLtI32 : ScriptOp::JumpLtU32);
            break;
        default:
            code = isFloat ? (wide ? (fails ? ScriptOp::JumpNotLeF64 : ScriptOp::JumpLeF64)
                                   : (fails ? ScriptOp::JumpNotLeF32 : ScriptOp::JumpLeF32))
                   : wide  ? (isSigned ? ScriptOp::JumpLeI64 : ScriptOp::JumpLeU64)
                           : (isSigned ? ScriptOp::JumpLeI32 : ScriptOp::JumpLeU32);
            break;
        }
        const uint32 a = Reg(left);
        const uint32 b = Reg(right);
        EmitCompareJump(code, a, b, 0, label);
    }

    /// The comparison a CondBranch's condition is fused into, or null.
    [[nodiscard]] const MirInstruction *FusedCompareOf(const MirValue *condition) const
    {
        if (condition->GetValueKind() != MirValueKind::Instruction)
        {
            return nullptr;
        }
        const auto *compare = static_cast<const MirInstruction *>(condition);
        return _fused.Contains(compare) ? compare : nullptr;
    }

    void CondBranch(MirInstruction *instruction)
    {
        const MirInstruction *compare = FusedCompareOf(instruction->GetOperand(0));
        const uint32 condition = compare == nullptr ? Reg(instruction->GetOperand(0)) : 0;
        const auto jump = [&](bool when, uint32 label) {
            if (compare != nullptr)
            {
                EmitBranchCompare(compare, when, label);
                return;
            }
            EmitJump(when ? ScriptOp::JumpIf : ScriptOp::JumpIfNot, condition, label);
        };
        // A target past a forwarder is reached with no moves, so only one this block leads to directly may need them.
        const MirBlock *current = _function->GetBlocks()[_block];
        const Threaded threadedTrue = Thread(current, instruction->GetTargets()[0]);
        const Threaded threadedFalse = Thread(current, instruction->GetTargets()[1]);
        const MirBlock *whenTrue = threadedTrue.Block;
        const MirBlock *whenFalse = threadedFalse.Block;
        const bool trueFree = !HasPhis(whenTrue) || EdgeIsFree(threadedTrue.From, whenTrue);
        const bool falseFree = !HasPhis(whenFalse) || EdgeIsFree(threadedFalse.From, whenFalse);
        if (trueFree && falseFree)
        {
            if (whenTrue == _next)
            {
                jump(false, BlockLabel(whenFalse));
                return;
            }
            jump(true, BlockLabel(whenTrue));
            if (whenFalse != _next)
            {
                Jump(BlockLabel(whenFalse));
            }
            return;
        }
        // One edge needs no moves: jump on it, and fall into the moves of the other.
        if (trueFree || falseFree)
        {
            jump(trueFree, BlockLabel(trueFree ? whenTrue : whenFalse));
            BranchTo(trueFree ? whenFalse : whenTrue);
            return;
        }
        const uint32 otherwise = NewLabel();
        jump(false, otherwise);
        EdgeMoves(_function->GetBlocks()[_block], whenTrue);
        Jump(BlockLabel(whenTrue));
        Bind(otherwise);
        BranchTo(whenFalse);
    }

    void Switch(MirInstruction *instruction)
    {
        const uint32 selector = Reg(instruction->GetOperand(0));
        const Span<MirBlock *const> targets = instruction->GetTargets();
        HashMap<const MirBlock *, uint32> stubs;
        List<const MirBlock *> stubTargets;
        auto labelOf = [&](const MirBlock *target) {
            if (!HasPhis(target))
            {
                return BlockLabel(target);
            }
            if (const uint32 *found = stubs.Find(target))
            {
                return *found;
            }
            const uint32 label = NewLabel();
            stubs.Add(target, label);
            stubTargets.Add(target);
            return label;
        };
        SwitchFixup fixup;
        fixup.Table = static_cast<uint32>(_record->Switches.Count());
        fixup.Default = labelOf(targets[0]);
        ScriptSwitchTable &table = _record->Switches.AddDefaulted();
        table.Is64 = instruction->GetOperand(0)->GetType()->GetWidth() == 64;
        // The cases go in value order, so Switch can search them; a repeated value keeps its first target.
        List<ScriptSwitchCase> cases;
        for (usize i = 1; i < targets.Count(); ++i)
        {
            cases.Add({instruction->GetLiterals()[i - 1], labelOf(targets[i])});
        }
        cases.StableSort([](const ScriptSwitchCase &a, const ScriptSwitchCase &b) { return a.Value < b.Value; });
        for (const ScriptSwitchCase &entry : cases)
        {
            table.Cases.Add({entry.Value, 0});
            fixup.Cases.Add(entry.Target);
        }
        _switchFixups.Add(Move(fixup));
        EmitABx(ScriptOp::Switch, selector, static_cast<uint32>(_record->Switches.Count() - 1));
        for (const MirBlock *target : stubTargets)
        {
            Bind(*stubs.Find(target));
            EdgeMoves(_function->GetBlocks()[_block], target);
            Jump(BlockLabel(target));
        }
    }

    void RecordSafepoint(const List<uint32> &values)
    {
        _safepoints.Add({static_cast<uint32>(_code.Count()), values});
    }

    /// The interface whose method *function* is, at *slot*; ~0u for a class method.
    [[nodiscard]] uint32 InterfaceOf(const MirFunction *function, uint32 slot) const
    {
        const Span<MirScriptType *const> types = _module->GetScriptTypes();
        for (usize i = 0; i < types.Count(); ++i)
        {
            if (types[i]->Kind == MirScriptTypeKind::Interface && slot < types[i]->VirtualSlots.Count() &&
                types[i]->VirtualSlots[slot] == function)
            {
                return static_cast<uint32>(i);
            }
        }
        return ~0u;
    }

    void Select(MirInstruction *instruction)
    {
        const MirOp op = instruction->GetOp();
        switch (op)
        {
        case MirOp::Add:
        case MirOp::Sub:
        case MirOp::Mul:
        case MirOp::Div:
        case MirOp::Rem:
        case MirOp::Negate:
        case MirOp::BitAnd:
        case MirOp::BitOr:
        case MirOp::BitXor:
        case MirOp::BitNot:
        case MirOp::ShiftLeft:
        case MirOp::ShiftRight:
        case MirOp::LogicalAnd:
        case MirOp::LogicalOr:
        case MirOp::LogicalNot:
        case MirOp::Equal:
        case MirOp::NotEqual:
        case MirOp::Less:
        case MirOp::LessEqual:
        case MirOp::Greater:
        case MirOp::GreaterEqual:
            Elementwise(instruction);
            return;
        case MirOp::MatrixTimesVector:
        case MirOp::VectorTimesMatrix:
        case MirOp::MatrixTimesMatrix:
        case MirOp::MatrixTimesScalar:
        case MirOp::VectorTimesScalar:
            MatrixOperation(instruction);
            return;
        case MirOp::Convert: {
            const MirType *from = instruction->GetOperand(0)->GetType();
            const MirType *to = instruction->GetType();
            const uint32 dst = Dst(instruction);
            if (from->IsScalar())
            {
                ScalarConvert(from, to, instruction->Checked, dst, Reg(instruction->GetOperand(0)));
                return;
            }
            if (from->Is(MirTypeKind::Matrix))
            {
                Fail("converting matrices is not supported");
                return;
            }
            ForEachLane(to, dst, instruction->GetOperands(), [&](uint32 result, Span<const uint32> lanes) {
                ScalarConvert(ScalarOf(from), ScalarOf(to), instruction->Checked, result, lanes[0]);
            });
            return;
        }
        case MirOp::Bitcast: {
            const MirType *to = instruction->GetType();
            const uint32 dst = Dst(instruction);
            const uint32 src = Reg(instruction->GetOperand(0));
            const ScriptNumber number = to->IsScalar() ? ScriptNumberOf(to) : ScriptNumber::U64;
            if (IsSmall(number))
            {
                EmitAB(ExtendOf(number), dst, src);
            }
            else if (number == ScriptNumber::F16)
            {
                EmitAB(ScriptOp::ZExt16, dst, src);
            }
            else
            {
                Copy(dst, src, ScriptSlotsOf(to));
            }
            return;
        }
        case MirOp::CompositeConstruct: {
            const MirType *type = instruction->GetType();
            const uint32 dst = Dst(instruction);
            bool splat = type->Is(MirTypeKind::Vector) && type->GetElement()->GetSize() >= 4;
            for (MirValue *element : instruction->GetOperands())
            {
                splat = splat && element == instruction->GetOperand(0);
            }
            if (splat)
            {
                const uint32 scalar = Reg(instruction->GetOperand(0));
                for (uint32 s = 0; s < ScriptSlotsOf(type); ++s)
                {
                    Splat(type->GetElement(), dst + s, scalar);
                }
                return;
            }
            uint32 offset = 0;
            for (usize i = 0; i < instruction->GetOperands().Count(); ++i)
            {
                MirValue *element = instruction->GetOperand(i);
                const MirType *elementType = element->GetType();
                uint32 at = 0;
                if (type->Is(MirTypeKind::Vector))
                {
                    at = offset;
                    offset += elementType->GetSize();
                }
                else
                {
                    ElementAt(type, static_cast<uint32>(i), at);
                }
                EmitAB(ScriptOp::Insert, dst, Reg(element), {at, ScriptWriteAccessOf(elementType, false, 0)});
            }
            return;
        }
        case MirOp::CompositeExtract: {
            const MirType *type = instruction->GetOperand(0)->GetType();
            uint32 offset = 0;
            for (const int64 index : instruction->GetLiterals())
            {
                type = ElementAt(type, static_cast<uint32>(index), offset);
            }
            EmitAB(ScriptOp::Extract, Dst(instruction), Reg(instruction->GetOperand(0)),
                   {offset, ScriptReadAccessOf(instruction->GetType())});
            return;
        }
        case MirOp::CompositeInsert: {
            const MirType *type = instruction->GetType();
            const uint32 dst = Dst(instruction);
            Materialize(instruction->GetOperand(0), dst);
            uint32 offset = 0;
            const MirType *element = type;
            for (const int64 index : instruction->GetLiterals())
            {
                element = ElementAt(element, static_cast<uint32>(index), offset);
            }
            EmitAB(ScriptOp::Insert, dst, Reg(instruction->GetOperand(1)),
                   {offset, ScriptWriteAccessOf(element, false, 0)});
            return;
        }
        case MirOp::VectorExtractDynamic: {
            const MirType *vector = instruction->GetOperand(0)->GetType();
            EmitABC(ScriptOp::ExtractDyn, Dst(instruction), Reg(instruction->GetOperand(0)),
                    Reg(instruction->GetOperand(1)), {ScriptReadAccessOf(vector->GetElement()), vector->GetCount()});
            return;
        }
        case MirOp::VectorInsertDynamic: {
            const MirType *vector = instruction->GetType();
            const uint32 dst = Dst(instruction);
            Materialize(instruction->GetOperand(0), dst);
            EmitABC(ScriptOp::InsertDyn, dst, Reg(instruction->GetOperand(1)), Reg(instruction->GetOperand(2)),
                    {ScriptWriteAccessOf(vector->GetElement(), false, 0), vector->GetCount()});
            return;
        }
        case MirOp::VectorShuffle: {
            const MirType *firstType = instruction->GetOperand(0)->GetType();
            const MirType *lane = firstType->GetElement();
            const uint32 firstCount = firstType->GetCount();
            const uint32 first = Reg(instruction->GetOperand(0));
            const uint32 second = Reg(instruction->GetOperand(1));
            const uint32 dst = Dst(instruction);
            const uint32 t = Temp(1);
            const Span<const int64> components = instruction->GetLiterals();
            for (uint32 j = 0; j < components.Count(); ++j)
            {
                const int64 component = components[j];
                if (component < 0)
                {
                    continue;
                }
                const bool fromFirst = static_cast<uint32>(component) < firstCount;
                const uint32 index =
                    fromFirst ? static_cast<uint32>(component) : static_cast<uint32>(component) - firstCount;
                EmitAB(ScriptOp::Extract, t, fromFirst ? first : second,
                       {index * lane->GetSize(), ScriptReadAccessOf(lane)});
                EmitAB(ScriptOp::Insert, dst, t, {j * lane->GetSize(), ScriptWriteAccessOf(lane, false, 0)});
            }
            return;
        }
        case MirOp::Select:
            SelectValue(instruction);
            return;
        case MirOp::Variable:
        case MirOp::Phi:
            return;
        case MirOp::Load:
            Load(instruction);
            return;
        case MirOp::Store:
            Store(instruction);
            return;
        case MirOp::AccessChain:
            if (AddressOf(instruction).Kind == AddressKind::Register && SlotOf(instruction) != NoSlot)
            {
                AccessChain(instruction);
            }
            return;
        case MirOp::Call: {
            const uint32 function =
                _builder->FunctionIndexOf(static_cast<const MirFunction *>(instruction->GetOperand(0)));
            CallWindow(instruction, [&](uint32 base, uint32) { EmitABx(ScriptOp::Call, base, function); });
            return;
        }
        case MirOp::CallVirtual: {
            const uint32 slot = static_cast<uint32>(instruction->GetLiterals()[0]);
            CallWindow(instruction, [&](uint32 base, uint32) { EmitABx(ScriptOp::CallVirtual, base, slot); });
            return;
        }
        case MirOp::CallInterface: {
            const uint32 site = static_cast<uint32>(_record->InterfaceSites.Count());
            _record->InterfaceSites.Add({static_cast<uint32>(instruction->GetLiterals()[0]),
                                         static_cast<uint32>(instruction->GetLiterals()[1])});
            CallWindow(instruction, [&](uint32 base, uint32) { EmitABx(ScriptOp::CallInterface, base, site); });
            return;
        }
        case MirOp::CallDelegate: {
            const uint32 allowEmpty = instruction->GetLiterals()[0] != 0 ? 1 : 0;
            CallWindow(instruction, [&](uint32 base, uint32 slots) {
                EmitABC(ScriptOp::CallDelegate, base, slots - 1, allowEmpty);
            });
            return;
        }
        case MirOp::Intrinsic:
            Intrinsic(instruction);
            return;
        case MirOp::Branch:
            BranchTo(instruction->GetTargets()[0]);
            return;
        case MirOp::CondBranch:
            CondBranch(instruction);
            return;
        case MirOp::Switch:
            Switch(instruction);
            return;
        case MirOp::Return:
            if (instruction->GetOperands().IsEmpty())
            {
                EmitAB(ScriptOp::Return, 0, 0);
            }
            else
            {
                MirValue *value = instruction->GetOperand(0);
                EmitAB(ScriptOp::Return, Reg(value), ScriptSlotsOf(value->GetType()));
            }
            return;
        case MirOp::Unreachable:
            EmitABx(ScriptOp::Fault, static_cast<uint32>(ScriptFault::Panic), 0);
            return;
        case MirOp::New:
            RecordSafepoint(*_safepointValues.Find(instruction));
            EmitABx(ScriptOp::New, Dst(instruction), static_cast<uint32>(instruction->GetLiterals()[0]));
            return;
        case MirOp::NewArray: {
            const uint32 length = Reg(instruction->GetOperand(0));
            const uint32 dst = Dst(instruction);
            RecordSafepoint(*_safepointValues.Find(instruction));
            EmitAB(ScriptOp::NewArray, dst, length, {static_cast<uint32>(instruction->GetLiterals()[0])});
            return;
        }
        case MirOp::FieldAddress:
            if (AddressOf(instruction).Kind == AddressKind::Register && SlotOf(instruction) != NoSlot)
            {
                EmitAB(ScriptOp::RefField, Dst(instruction), Reg(instruction->GetOperand(0)),
                       {FieldOffset(instruction)});
            }
            return;
        case MirOp::ElementAddress:
            if (AddressOf(instruction).Kind == AddressKind::Register && SlotOf(instruction) != NoSlot)
            {
                EmitABC(ScriptOp::RefElem, Dst(instruction), Reg(instruction->GetOperand(0)),
                        Reg(instruction->GetOperand(1)), {ElementWord(instruction->GetType()->GetElement())});
            }
            return;
        case MirOp::ObjectLength:
            EmitAB(ScriptOp::ArrayLength, Dst(instruction), Reg(instruction->GetOperand(0)));
            return;
        case MirOp::Box:
        case MirOp::Unbox:
        case MirOp::IsInstance: {
            const uint32 src = Reg(instruction->GetOperand(0));
            const uint32 dst = Dst(instruction);
            if (op == MirOp::Box)
            {
                RecordSafepoint(*_safepointValues.Find(instruction));
            }
            EmitAB(op == MirOp::Box     ? ScriptOp::Box
                   : op == MirOp::Unbox ? ScriptOp::Unbox
                                        : ScriptOp::IsInstance,
                   dst, src, {static_cast<uint32>(instruction->GetLiterals()[0])});
            return;
        }
        case MirOp::Cast:
            EmitAB(instruction->GetLiterals()[1] != 0 ? ScriptOp::TryCast : ScriptOp::Cast, Dst(instruction),
                   Reg(instruction->GetOperand(0)), {static_cast<uint32>(instruction->GetLiterals()[0])});
            return;
        case MirOp::TypeOf:
            RecordSafepoint(*_safepointValues.Find(instruction));
            EmitABx(ScriptOp::TypeOf, Dst(instruction), static_cast<uint32>(instruction->GetLiterals()[0]));
            return;
        case MirOp::NullCheck:
            NullCheck(instruction);
            return;
        case MirOp::MakeDelegate: {
            const uint32 receiver = instruction->GetOperands().Count() > 1 ? Reg(instruction->GetOperand(1)) : 0;
            const uint32 dst = Dst(instruction);
            const int64 slot = instruction->GetLiterals()[1];
            const auto *declared = static_cast<const MirFunction *>(instruction->GetOperand(0));
            uint32 mode = instruction->GetOperands().Count() < 2 ? 0 : slot >= 0 ? 2 : 1;
            uint32 target = mode == 2 ? static_cast<uint32>(slot) : _builder->FunctionIndexOf(declared);
            // RECOVERY-DECISION S6: a delegate of an interface method (mode 3) names an interface site, so the
            // method is found on the receiver's interface table when the delegate is made.
            if (mode == 2)
            {
                const uint32 interface = InterfaceOf(declared, static_cast<uint32>(slot));
                if (interface != ~0u)
                {
                    mode = 3;
                    target = static_cast<uint32>(_record->InterfaceSites.Count());
                    _record->InterfaceSites.Add({interface, static_cast<uint32>(slot)});
                }
            }
            RecordSafepoint(*_safepointValues.Find(instruction));
            EmitABC(ScriptOp::MakeDelegate, dst, receiver, mode,
                    {static_cast<uint32>(instruction->GetLiterals()[0]), target});
            return;
        }
        case MirOp::CombineDelegates: {
            const uint32 a = Reg(instruction->GetOperand(0));
            const uint32 b = Reg(instruction->GetOperand(1));
            const uint32 dst = Dst(instruction);
            RecordSafepoint(*_safepointValues.Find(instruction));
            EmitABC(instruction->GetLiterals()[0] != 0 ? ScriptOp::RemoveDelegates : ScriptOp::CombineDelegates, dst, a,
                    b);
            return;
        }
        case MirOp::StringConstant:
            EmitABx(ScriptOp::LoadString, Dst(instruction),
                    _builder->MirString(static_cast<uint32>(instruction->GetLiterals()[0])));
            return;
        case MirOp::Await: {
            const uint32 task = Reg(instruction->GetOperand(0));
            const uint32 slots = ScriptSlotsOf(instruction->GetType());
            const uint32 dst = slots != 0 ? Dst(instruction) : 0;
            RecordSafepoint(*_safepointValues.Find(instruction));
            EmitABC(ScriptOp::Await, dst, task, slots);
            return;
        }
        case MirOp::Yield: {
            if (instruction->GetOperands().IsEmpty())
            {
                RecordSafepoint(*_safepointValues.Find(instruction));
                EmitAB(ScriptOp::Yield, 0, 0);
                return;
            }
            MirValue *value = instruction->GetOperand(0);
            const uint32 reg = Reg(value);
            RecordSafepoint(*_safepointValues.Find(instruction));
            EmitAB(ScriptOp::Yield, reg, ScriptSlotsOf(value->GetType()));
            return;
        }
        case MirOp::Fault: {
            const int64 message = instruction->GetLiterals()[1];
            EmitABx(ScriptOp::Fault, static_cast<uint32>(instruction->GetLiterals()[0]),
                    message < 0 ? 0 : _builder->MirString(static_cast<uint32>(message)) + 1);
            return;
        }
        default:
            Fail(Format("the script target cannot express {0}", GetMirOpName(op)).AsView());
            return;
        }
    }

    void SelectValue(MirInstruction *instruction)
    {
        const MirType *conditionType = instruction->GetOperand(0)->GetType();
        const uint32 dst = Dst(instruction);
        if (conditionType->Is(MirTypeKind::Vector))
        {
            ForEachLane(instruction->GetType(), dst, instruction->GetOperands(),
                        [&](uint32 result, Span<const uint32> lanes) {
                            const uint32 otherwise = NewLabel();
                            const uint32 done = NewLabel();
                            EmitJump(ScriptOp::JumpIfNot, lanes[0], otherwise);
                            Copy(result, lanes[1], 1);
                            Jump(done);
                            Bind(otherwise);
                            Copy(result, lanes[2], 1);
                            Bind(done);
                        });
            return;
        }
        const uint32 condition = Reg(instruction->GetOperand(0));
        const uint32 otherwise = NewLabel();
        const uint32 done = NewLabel();
        EmitJump(ScriptOp::JumpIfNot, condition, otherwise);
        const uint32 saved = _tempNext;
        Materialize(instruction->GetOperand(1), dst);
        Jump(done);
        Bind(otherwise);
        _tempNext = saved;
        Materialize(instruction->GetOperand(2), dst);
        Bind(done);
    }

    void NullCheck(MirInstruction *instruction)
    {
        MirValue *value = instruction->GetOperand(0);
        const MirType *type = value->GetType();
        const uint32 src = Reg(value);
        const uint32 dst = Dst(instruction);
        if (!type->Is(MirTypeKind::Struct))
        {
            EmitAB(ScriptOp::NullCheck, dst, src);
            return;
        }
        const MirStructField &hasValue = type->GetFields()[1];
        const uint32 flag = Temp(1);
        EmitAB(ScriptOp::Extract, flag, src, {hasValue.Offset, ScriptReadAccessOf(hasValue.Type)});
        const uint32 present = NewLabel();
        EmitJump(ScriptOp::JumpIf, flag, present);
        EmitABx(ScriptOp::Fault, static_cast<uint32>(ScriptFault::NullReference), 0);
        Bind(present);
        const MirStructField &content = type->GetFields()[0];
        EmitAB(ScriptOp::Extract, dst, src, {content.Offset, ScriptReadAccessOf(content.Type)});
    }

    void Finish()
    {
        for (const SwitchFixup &fixup : _switchFixups)
        {
            ScriptSwitchTable &table = _record->Switches[fixup.Table];
            table.Default = _labels[fixup.Default];
            for (usize i = 0; i < fixup.Cases.Count(); ++i)
            {
                table.Cases[i].Target = _labels[fixup.Cases[i]];
            }
        }
        _record->Code = Move(_code);
        _record->FrameSlots = _frameSlots;
        List<uint32> blocks;
        for (const uint32 label : _labels)
        {
            if (label != NoSlot && !blocks.Contains(label))
            {
                blocks.Add(label);
            }
        }
        blocks.Sort();
        _record->Blocks = Move(blocks);
        // The maps cover the values' and variables' registers; temporaries are never named.
        const uint32 mapSlots = _fixedSlots == 0 ? 1 : _fixedSlots;
        const uint32 mapBytes = (mapSlots * 2 + 7) / 8;
        _record->MapBytes = mapBytes;
        List<List<uint8>> maps;
        for (const PendingSafepoint &safepoint : _safepoints)
        {
            List<uint8> objects;
            List<uint8> managed;
            objects.Resize(mapBytes, 0);
            managed.Resize(mapBytes, 0);
            for (const MirInstruction *variable : _variables)
            {
                MarkScriptReferences(variable->GetType()->GetElement(),
                                     _addresses.Find(variable)->Slot * ScriptSlotBytes, objects, managed);
            }
            for (const uint32 id : safepoint.Values)
            {
                MarkScriptReferences(_values[id]->GetType(), _registers[id].Slot * ScriptSlotBytes, objects, managed);
            }
            objects.Resize(mapBytes, 0);
            managed.Resize(mapBytes, 0);
            List<uint8> map = Move(objects);
            map.AddRange(managed.AsSpan());
            usize index = maps.IndexOf(map);
            if (index == static_cast<usize>(-1))
            {
                index = maps.Count();
                maps.Add(Move(map));
            }
            _record->Safepoints.Add({safepoint.Pc, static_cast<uint32>(index)});
        }
        _record->Maps.Clear();
        for (const List<uint8> &map : maps)
        {
            _record->Maps.AddRange(map.AsSpan());
        }
        if (maps.IsEmpty())
        {
            _record->MapBytes = 0;
        }
    }

    ScriptModuleBuilder *_builder;
    MirModule *_module;
    MirFunction *_function;
    ScriptFunctionRecord *_record;
    bool _failed = false;

    uint32 _fixedSlots = 0;
    uint32 _frameSlots = 0;
    List<const MirInstruction *> _variables;
    HashMap<const MirValue *, Address> _addresses;
    HashMap<const MirBlock *, uint32> _blockIndex;
    HashMap<const MirValue *, uint32> _ids;
    List<MirValue *> _values;
    List<ValueRegisters> _registers;
    /// The frame variables' registers, after the parameters.
    uint32 _variablesStart = 0;
    uint32 _variablesEnd = 0;
    /// The comparisons CondBranch tests with a compare-and-jump, which have no register.
    HashSet<const MirInstruction *> _fused;
    HashMap<const MirInstruction *, List<uint32>> _safepointValues;
    HashMap<const MirBlock *, List<uint32>> _headerValues;
    /// The label just after each loop header's Loop, where unconditional back edges jump.
    HashMap<const MirBlock *, uint32> _loopBodies;
    /// The blocks that are not emitted (IsForwarder).
    List<const MirBlock *> _forwarders;
    /// Where each call's window starts (AllocateRegisters).
    HashMap<const MirInstruction *, uint32> _callBases;

    List<uint32> _code;
    List<uint32> _labels;
    List<Fixup> _fixups;
    List<SwitchFixup> _switchFixups;
    List<uint32> _wideJumps;
    uint32 _jumps = 0;
    HashMap<ConstantKey, uint32> _constants;
    List<PendingSafepoint> _safepoints;
    ScriptLineEntry _lastLine;
    MirLocation _location;
    uint32 _tempNext = 0;
    uint32 _block = 0;
    const MirBlock *_next = nullptr;
};
} // namespace

void CompileScriptFunction(ScriptModuleBuilder &builder, MirFunction &function, ScriptFunctionRecord &record)
{
    FunctionCompiler(builder, function, record).Compile();
}
} // namespace Marlin
