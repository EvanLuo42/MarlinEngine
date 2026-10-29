#include "Hash/Hash.h"
#include "LanguageIR/MirBuilder.h"
#include "LanguageIR/MirPasses.h"

namespace Marlin
{
namespace
{
MirConstant *ConstantElement(MirModule &module, MirConstant *constant, uint32 index, const MirType *elementType)
{
    switch (constant->GetConstantKind())
    {
    case MirConstantKind::Composite:
        return index < constant->GetElements().Count() ? constant->GetElements()[index] : nullptr;
    case MirConstantKind::Null:
        return module.GetNull(elementType);
    case MirConstantKind::Undef:
        return module.GetUndef(elementType);
    default:
        return nullptr;
    }
}

MirConstant *AsConstant(MirValue *value)
{
    return value->GetValueKind() == MirValueKind::Constant ? static_cast<MirConstant *>(value) : nullptr;
}

/// A scalar constant as bits, the null constant as zero; false for undef.
bool ScalarBits(const MirConstant *constant, uint64 &bits)
{
    if (constant->GetConstantKind() == MirConstantKind::Scalar)
    {
        bits = constant->GetBits();
        return true;
    }
    if (constant->GetConstantKind() == MirConstantKind::Null)
    {
        bits = 0;
        return true;
    }
    return false;
}

double FloatOf(MirModule &module, const MirConstant *constant)
{
    if (constant->GetConstantKind() == MirConstantKind::Null)
    {
        return 0;
    }
    (void)module;
    return constant->GetFloat();
}

MirConstant *FoldScalar(MirModule &module, MirOp op, const MirType *resultType, MirConstant *a, MirConstant *b)
{
    const MirType *type = a->GetType();
    uint64 x = 0;
    uint64 y = 0;
    if (!ScalarBits(a, x) || (b != nullptr && !ScalarBits(b, y)))
    {
        return nullptr;
    }
    if (type->Is(MirTypeKind::Float))
    {
        const double fx = FloatOf(module, a);
        const double fy = b != nullptr ? FloatOf(module, b) : 0;
        switch (op)
        {
        case MirOp::Add:
            return module.GetFloat(resultType, fx + fy);
        case MirOp::Sub:
            return module.GetFloat(resultType, fx - fy);
        case MirOp::Mul:
            return module.GetFloat(resultType, fx * fy);
        case MirOp::Div:
            return module.GetFloat(resultType, fx / fy);
        case MirOp::Negate:
            return module.GetFloat(resultType, -fx);
        case MirOp::Equal:
            return module.GetBool(fx == fy);
        case MirOp::NotEqual:
            return module.GetBool(fx != fy);
        case MirOp::Less:
            return module.GetBool(fx < fy);
        case MirOp::LessEqual:
            return module.GetBool(fx <= fy);
        case MirOp::Greater:
            return module.GetBool(fx > fy);
        case MirOp::GreaterEqual:
            return module.GetBool(fx >= fy);
        default:
            return nullptr;
        }
    }
    const bool isSigned = type->IsSigned();
    const uint32 width = type->Is(MirTypeKind::Bool) ? 1 : type->GetWidth();
    const auto sx = static_cast<int64>(x);
    const auto sy = static_cast<int64>(y);
    auto integer = [&](uint64 value) { return module.GetInt(resultType, static_cast<int64>(value)); };
    switch (op)
    {
    case MirOp::Add:
        return integer(x + y);
    case MirOp::Sub:
        return integer(x - y);
    case MirOp::Mul:
        return integer(x * y);
    case MirOp::Div:
        return y == 0 ? nullptr : integer(isSigned ? static_cast<uint64>(sx / sy) : x / y);
    case MirOp::Rem:
        return y == 0 ? nullptr : integer(isSigned ? static_cast<uint64>(sx % sy) : x % y);
    case MirOp::Negate:
        return integer(~x + 1);
    case MirOp::BitAnd:
        return integer(x & y);
    case MirOp::BitOr:
        return integer(x | y);
    case MirOp::BitXor:
        return integer(x ^ y);
    case MirOp::BitNot:
        return integer(~x);
    case MirOp::ShiftLeft:
        return integer(x << (y % width));
    case MirOp::ShiftRight:
    {
        const uint64 mask = width >= 64 ? ~uint64{0} : (uint64{1} << width) - 1;
        return integer(isSigned ? static_cast<uint64>(sx >> (y % width)) : (x & mask) >> (y % width));
    }
    case MirOp::LogicalAnd:
        return module.GetBool(x != 0 && y != 0);
    case MirOp::LogicalOr:
        return module.GetBool(x != 0 || y != 0);
    case MirOp::LogicalNot:
        return module.GetBool(x == 0);
    case MirOp::Equal:
        return module.GetBool(x == y);
    case MirOp::NotEqual:
        return module.GetBool(x != y);
    case MirOp::Less:
        return module.GetBool(isSigned ? sx < sy : x < y);
    case MirOp::LessEqual:
        return module.GetBool(isSigned ? sx <= sy : x <= y);
    case MirOp::Greater:
        return module.GetBool(isSigned ? sx > sy : x > y);
    case MirOp::GreaterEqual:
        return module.GetBool(isSigned ? sx >= sy : x >= y);
    default:
        return nullptr;
    }
}

MirConstant *ConvertScalar(MirModule &module, MirConstant *value, const MirType *to)
{
    uint64 bits = 0;
    if (!ScalarBits(value, bits))
    {
        return nullptr;
    }
    const MirType *from = value->GetType();
    if (to->Is(MirTypeKind::Float))
    {
        if (from->Is(MirTypeKind::Float))
        {
            return module.GetFloat(to, FloatOf(module, value));
        }
        return module.GetFloat(to, from->IsSigned() ? static_cast<double>(static_cast<int64>(bits))
                                                    : static_cast<double>(bits));
    }
    if (to->Is(MirTypeKind::Int))
    {
        if (from->Is(MirTypeKind::Float))
        {
            const double f = FloatOf(module, value);
            if (f != f || f >= 18446744073709551616.0 || f <= -9223372036854775808.0)
            {
                return nullptr;
            }
            return module.GetInt(to, to->IsSigned() ? static_cast<int64>(f) : static_cast<int64>(static_cast<uint64>(f)));
        }
        return module.GetInt(to, static_cast<int64>(bits));
    }
    return nullptr;
}

/// Applies *op* component by component when the operands are vectors.
MirConstant *FoldComponentwise(MirModule &module, MirOp op, const MirType *resultType, MirConstant *a, MirConstant *b)
{
    if (!resultType->Is(MirTypeKind::Vector))
    {
        return op == MirOp::Convert ? ConvertScalar(module, a, resultType) : FoldScalar(module, op, resultType, a, b);
    }
    List<MirConstant *> elements;
    for (uint32 i = 0; i < resultType->GetCount(); ++i)
    {
        MirConstant *x = ConstantElement(module, a, i, a->GetType()->GetElement());
        MirConstant *y = b != nullptr ? ConstantElement(module, b, i, b->GetType()->GetElement()) : nullptr;
        if (x == nullptr || (b != nullptr && y == nullptr))
        {
            return nullptr;
        }
        MirConstant *element = op == MirOp::Convert ? ConvertScalar(module, x, resultType->GetElement())
                                                    : FoldScalar(module, op, resultType->GetElement(), x, y);
        if (element == nullptr)
        {
            return nullptr;
        }
        elements.Add(element);
    }
    return module.GetComposite(resultType, elements.AsSpan());
}

MirConstant *Fold(MirModule &module, MirInstruction &instruction)
{
    if (instruction.Checked)
    {
        return nullptr;
    }
    const MirOp op = instruction.GetOp();
    const Span<MirValue *const> operands = instruction.GetOperands();
    switch (op)
    {
    case MirOp::Add:
    case MirOp::Sub:
    case MirOp::Mul:
    case MirOp::Div:
    case MirOp::Rem:
    case MirOp::BitAnd:
    case MirOp::BitOr:
    case MirOp::BitXor:
    case MirOp::ShiftLeft:
    case MirOp::ShiftRight:
    case MirOp::LogicalAnd:
    case MirOp::LogicalOr:
    case MirOp::Equal:
    case MirOp::NotEqual:
    case MirOp::Less:
    case MirOp::LessEqual:
    case MirOp::Greater:
    case MirOp::GreaterEqual:
    {
        MirConstant *a = AsConstant(operands[0]);
        MirConstant *b = AsConstant(operands[1]);
        if (a == nullptr || b == nullptr || operands[0]->GetType()->Is(MirTypeKind::Matrix))
        {
            return nullptr;
        }
        return FoldComponentwise(module, op, instruction.GetType(), a, b);
    }
    case MirOp::Negate:
    case MirOp::BitNot:
    case MirOp::LogicalNot:
    case MirOp::Convert:
    {
        MirConstant *a = AsConstant(operands[0]);
        if (a == nullptr || operands[0]->GetType()->Is(MirTypeKind::Matrix))
        {
            return nullptr;
        }
        return FoldComponentwise(module, op, instruction.GetType(), a, nullptr);
    }
    case MirOp::CompositeExtract:
    {
        MirConstant *value = AsConstant(operands[0]);
        const MirType *type = operands[0]->GetType();
        for (const int64 literal : instruction.GetLiterals())
        {
            if (value == nullptr)
            {
                return nullptr;
            }
            const auto index = static_cast<uint32>(literal);
            const uint32 path[] = {index};
            type = GetIndexedType(type, path);
            value = type != nullptr ? ConstantElement(module, value, index, type) : nullptr;
        }
        return value;
    }
    case MirOp::CompositeConstruct:
    {
        List<MirConstant *> elements;
        const MirType *type = instruction.GetType();
        for (MirValue *operand : operands)
        {
            MirConstant *element = AsConstant(operand);
            if (element == nullptr)
            {
                return nullptr;
            }
            if (type->Is(MirTypeKind::Vector) && operand->GetType()->Is(MirTypeKind::Vector))
            {
                for (uint32 i = 0; i < operand->GetType()->GetCount(); ++i)
                {
                    elements.Add(ConstantElement(module, element, i, type->GetElement()));
                }
                continue;
            }
            elements.Add(element);
        }
        return module.GetComposite(type, elements.AsSpan());
    }
    case MirOp::Intrinsic:
    {
        // Integer Min and Max, as array indices are clamped with (§4.3): folding them lets an unrolled loop's
        // accesses become constant and its arrays registers.
        const MirIntrinsic intrinsic = instruction.GetIntrinsic();
        if ((intrinsic != MirIntrinsic::Min && intrinsic != MirIntrinsic::Max) || operands.Count() != 2 ||
            !instruction.GetType()->Is(MirTypeKind::Int))
        {
            return nullptr;
        }
        MirConstant *a = AsConstant(operands[0]);
        MirConstant *b = AsConstant(operands[1]);
        uint64 x = 0;
        uint64 y = 0;
        if (a == nullptr || b == nullptr || !ScalarBits(a, x) || !ScalarBits(b, y))
        {
            return nullptr;
        }
        const bool isSigned = instruction.GetType()->IsSigned();
        const bool less = isSigned ? static_cast<int64>(x) < static_cast<int64>(y) : x < y;
        return intrinsic == MirIntrinsic::Min ? (less ? a : b) : (less ? b : a);
    }
    case MirOp::Select:
    {
        MirConstant *condition = AsConstant(operands[0]);
        uint64 bits = 0;
        if (condition == nullptr || !operands[0]->GetType()->Is(MirTypeKind::Bool) || !ScalarBits(condition, bits))
        {
            return nullptr;
        }
        return AsConstant(operands[bits != 0 ? 1 : 2]);
    }
    case MirOp::VectorShuffle:
    {
        MirConstant *a = AsConstant(operands[0]);
        MirConstant *b = AsConstant(operands[1]);
        if (a == nullptr || b == nullptr)
        {
            return nullptr;
        }
        const uint32 firstCount = operands[0]->GetType()->GetCount();
        const MirType *scalar = operands[0]->GetType()->GetElement();
        List<MirConstant *> elements;
        for (const int64 component : instruction.GetLiterals())
        {
            const auto index = static_cast<uint32>(component);
            MirConstant *element = index < firstCount ? ConstantElement(module, a, index, scalar)
                                                      : ConstantElement(module, b, index - firstCount, scalar);
            if (element == nullptr)
            {
                return nullptr;
            }
            elements.Add(element);
        }
        return elements.Count() == 1 ? elements[0] : module.GetComposite(instruction.GetType(), elements.AsSpan());
    }
    default:
        return nullptr;
    }
}

bool IsPureOperation(const MirInstruction &instruction)
{
    switch (instruction.GetOp())
    {
    case MirOp::Variable:
    case MirOp::Load:
    case MirOp::Store:
    case MirOp::Call:
    case MirOp::Phi:
        return false;
    case MirOp::New:
    case MirOp::NewArray:
    case MirOp::Box:
    case MirOp::MakeDelegate:
    case MirOp::CombineDelegates:
        return false;
    case MirOp::FieldAddress:
    case MirOp::ElementAddress:
        return false;
    case MirOp::Unbox:
    case MirOp::NullCheck:
    case MirOp::CallVirtual:
    case MirOp::CallInterface:
    case MirOp::CallDelegate:
    case MirOp::Await:
    case MirOp::Yield:
        return false;
    case MirOp::Cast:
        return instruction.GetLiterals()[1] != 0;
    case MirOp::Intrinsic:
        if (HasSideEffects(instruction))
        {
            return false;
        }
        switch (instruction.GetIntrinsic())
        {
        case MirIntrinsic::WaveActiveBallot:
        case MirIntrinsic::WaveReadLaneAt:
        case MirIntrinsic::WaveReadFirstLane:
        case MirIntrinsic::WaveIsFirstLane:
        case MirIntrinsic::WaveActiveAllTrue:
        case MirIntrinsic::WaveActiveAnyTrue:
        case MirIntrinsic::WaveActiveAllEqual:
        case MirIntrinsic::WaveActiveSum:
        case MirIntrinsic::WaveActiveProduct:
        case MirIntrinsic::WaveActiveMin:
        case MirIntrinsic::WaveActiveMax:
        case MirIntrinsic::WaveActiveBitAnd:
        case MirIntrinsic::WaveActiveBitOr:
        case MirIntrinsic::WaveActiveBitXor:
        case MirIntrinsic::WaveActiveCountBits:
        case MirIntrinsic::WavePrefixSum:
        case MirIntrinsic::WavePrefixProduct:
        case MirIntrinsic::WavePrefixCountBits:
        case MirIntrinsic::Ddx:
        case MirIntrinsic::Ddy:
        case MirIntrinsic::DdxFine:
        case MirIntrinsic::DdyFine:
        case MirIntrinsic::DdxCoarse:
        case MirIntrinsic::DdyCoarse:
        case MirIntrinsic::Fwidth:
        case MirIntrinsic::Sample:
        case MirIntrinsic::SampleBias:
        case MirIntrinsic::SampleCmp:
        case MirIntrinsic::CalculateLod:
        case MirIntrinsic::RayQueryGet:
        case MirIntrinsic::HitObjectGet:
        case MirIntrinsic::TextureLoad:
            return false;
        default:
            return true;
        }
    default:
        return !instruction.IsTerminator();
    }
}

struct ValueKey
{
    const MirInstruction *Instruction;

    [[nodiscard]] uint64 GetHash() const
    {
        uint64 hash = HashCombine(static_cast<uint64>(Instruction->GetOp()),
                                  static_cast<uint64>(Instruction->GetIntrinsic()));
        hash = HashCombine(hash, HashInteger(reinterpret_cast<uintptr>(Instruction->GetType())));
        for (const MirValue *operand : Instruction->GetOperands())
        {
            hash = HashCombine(hash, HashInteger(reinterpret_cast<uintptr>(operand)));
        }
        for (const int64 literal : Instruction->GetLiterals())
        {
            hash = HashCombine(hash, HashInteger(static_cast<uint64>(literal)));
        }
        return hash;
    }

    bool operator==(const ValueKey &other) const
    {
        const MirInstruction *a = Instruction;
        const MirInstruction *b = other.Instruction;
        if (a->GetOp() != b->GetOp() || a->GetIntrinsic() != b->GetIntrinsic() || a->GetType() != b->GetType() ||
            a->GetOperands().Count() != b->GetOperands().Count() ||
            a->GetLiterals().Count() != b->GetLiterals().Count() || a->Precise != b->Precise ||
            a->Checked != b->Checked)
        {
            return false;
        }
        for (usize i = 0; i < a->GetOperands().Count(); ++i)
        {
            if (a->GetOperand(i) != b->GetOperand(i))
            {
                return false;
            }
        }
        for (usize i = 0; i < a->GetLiterals().Count(); ++i)
        {
            if (a->GetLiterals()[i] != b->GetLiterals()[i])
            {
                return false;
            }
        }
        return true;
    }
};
} // namespace

MirPreservedAnalyses MirConstantFoldPass::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    (void)analyses;
    MirModule &module = function.GetModule();
    bool changed = true;
    bool any = false;
    while (changed)
    {
        changed = false;
        for (MirBlock *block : function.GetBlocks())
        {
            MirInstruction *instruction = block->GetFirst();
            while (instruction != nullptr)
            {
                MirInstruction *next = instruction->GetNext();
                if (!instruction->Precise || !instruction->GetType()->GetScalar() ||
                    !instruction->GetType()->GetScalar()->Is(MirTypeKind::Float))
                {
                    if (MirConstant *folded = Fold(module, *instruction))
                    {
                        instruction->ReplaceAllUsesWith(folded);
                        instruction->Erase();
                        changed = true;
                        any = true;
                    }
                }
                instruction = next;
            }
        }
    }
    if (!any)
    {
        return MirPreservedAnalyses::All();
    }
    MirPreservedAnalyses preserved;
    preserved.Preserve(MirCfgAnalyses::Key);
    return preserved;
}

namespace
{
/// The value an extract reads straight from the construct or insert that made its composite, so the composite can
/// die; null when it reads a part that is not a whole operand.
MirValue *SimplifyComposite(const MirInstruction &instruction)
{
    if (instruction.GetOp() != MirOp::CompositeExtract || instruction.GetLiterals().IsEmpty())
    {
        return nullptr;
    }
    MirValue *composite = instruction.GetOperand(0);
    const Span<const int64> indices = instruction.GetLiterals();
    usize position = 0;
    while (position < indices.Count() && composite->GetValueKind() == MirValueKind::Instruction)
    {
        const auto *source = static_cast<const MirInstruction *>(composite);
        const int64 index = indices[position];
        if (source->GetOp() == MirOp::CompositeConstruct)
        {
            const bool wholeParts = !source->GetType()->Is(MirTypeKind::Vector) ||
                                    source->GetOperands().Count() == source->GetType()->GetCount();
            if (!wholeParts || index < 0 || static_cast<usize>(index) >= source->GetOperands().Count())
            {
                break;
            }
            composite = source->GetOperand(static_cast<usize>(index));
            ++position;
        }
        else if (source->GetOp() == MirOp::CompositeInsert && source->GetLiterals().Count() == 1)
        {
            composite = source->GetLiterals()[0] == index ? source->GetOperand(1) : source->GetOperand(0);
            position += source->GetLiterals()[0] == index ? 1 : 0;
        }
        else
        {
            break;
        }
    }
    return position == indices.Count() && composite->GetType() == instruction.GetType() ? composite : nullptr;
}
} // namespace

MirPreservedAnalyses MirCsePass::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    if (function.GetEntry() == nullptr)
    {
        return MirPreservedAnalyses::All();
    }
    const MirControlFlow &flow = analyses.GetResult<MirControlFlowAnalysis>(function);
    HashMap<ValueKey, MirInstruction *> available;
    List<ValueKey> added;
    struct Frame
    {
        MirBlock *Block;
        usize AddedBefore;
        usize NextChild;
        bool Processed;
    };
    List<Frame> stack;
    stack.Add({function.GetEntry(), 0, 0, false});
    bool changed = false;
    while (!stack.IsEmpty())
    {
        Frame &frame = stack.Last();
        if (!frame.Processed)
        {
            frame.Processed = true;
            frame.AddedBefore = added.Count();
            MirInstruction *instruction = frame.Block->GetFirst();
            while (instruction != nullptr)
            {
                MirInstruction *next = instruction->GetNext();
                if (instruction->GetOp() == MirOp::Phi && instruction->GetOperands().Count() > 0)
                {
                    MirValue *same = instruction->GetOperand(0);
                    bool trivial = true;
                    for (MirValue *operand : instruction->GetOperands())
                    {
                        trivial = trivial && (operand == same || operand == instruction);
                    }
                    if (trivial && same != instruction)
                    {
                        instruction->ReplaceAllUsesWith(same);
                        instruction->Erase();
                        changed = true;
                    }
                }
                else if (MirValue *simplified = SimplifyComposite(*instruction))
                {
                    instruction->ReplaceAllUsesWith(simplified);
                    instruction->Erase();
                    changed = true;
                }
                else if (IsPureOperation(*instruction) && !instruction->GetType()->Is(MirTypeKind::Void))
                {
                    const ValueKey key{instruction};
                    if (MirInstruction **existing = available.Find(key))
                    {
                        (*existing)->NonUniform = (*existing)->NonUniform || instruction->NonUniform;
                        (*existing)->Restrict = (*existing)->Restrict && instruction->Restrict;
                        instruction->ReplaceAllUsesWith(*existing);
                        instruction->Erase();
                        changed = true;
                    }
                    else
                    {
                        available.Add(key, instruction);
                        added.Add(key);
                    }
                }
                instruction = next;
            }
        }
        const Span<MirBlock *const> children = flow.GetDominatedChildren(frame.Block);
        if (frame.NextChild < children.Count())
        {
            MirBlock *child = children[frame.NextChild++];
            stack.Add({child, 0, 0, false});
            continue;
        }
        while (added.Count() > frame.AddedBefore)
        {
            available.Remove(added.Last());
            added.Pop();
        }
        stack.Pop();
    }
    if (!changed)
    {
        return MirPreservedAnalyses::All();
    }
    MirPreservedAnalyses preserved;
    preserved.Preserve(MirCfgAnalyses::Key);
    return preserved;
}

MirPreservedAnalyses MirDcePass::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    (void)analyses;
    HashSet<const MirInstruction *> live;
    List<MirInstruction *> work;
    for (MirBlock *block : function.GetBlocks())
    {
        for (MirInstruction *instruction : *block)
        {
            if (HasSideEffects(*instruction) && live.Add(instruction))
            {
                work.Add(instruction);
            }
        }
    }
    while (!work.IsEmpty())
    {
        MirInstruction *instruction = work.Pop();
        for (MirValue *operand : instruction->GetOperands())
        {
            if (operand->GetValueKind() == MirValueKind::Instruction)
            {
                auto *definition = static_cast<MirInstruction *>(operand);
                if (live.Add(definition))
                {
                    work.Add(definition);
                }
            }
        }
    }
    List<MirInstruction *> dead;
    for (MirBlock *block : function.GetBlocks())
    {
        for (MirInstruction *instruction : *block)
        {
            if (!live.Contains(instruction))
            {
                dead.Add(instruction);
            }
        }
    }
    if (dead.IsEmpty())
    {
        return MirPreservedAnalyses::All();
    }
    for (MirInstruction *instruction : dead)
    {
        instruction->DropOperands();
    }
    for (MirInstruction *instruction : dead)
    {
        instruction->GetParent()->Unlink(instruction);
    }
    MirPreservedAnalyses preserved;
    preserved.Preserve(MirCfgAnalyses::Key);
    return preserved;
}
} // namespace Marlin
