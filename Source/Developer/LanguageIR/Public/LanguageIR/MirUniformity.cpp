#include "LanguageIR/MirPasses.h"

namespace Marlin
{
namespace
{
bool IsDivergentSource(const MirInstruction &instruction)
{
    switch (instruction.GetOp())
    {
    case MirOp::Load:
    {
        const MirValue *pointer = instruction.GetOperand(0);
        const MirAddressSpace space = pointer->GetType()->GetAddressSpace();
        return space == MirAddressSpace::Input || space == MirAddressSpace::GroupShared ||
               space == MirAddressSpace::IncomingRayPayload || space == MirAddressSpace::HitAttribute ||
               space == MirAddressSpace::IncomingCallableData || space == MirAddressSpace::TaskPayload;
    }
    case MirOp::Call:
        return true;
    case MirOp::Intrinsic:
        switch (instruction.GetIntrinsic())
        {
        case MirIntrinsic::AtomicLoad:
        case MirIntrinsic::AtomicExchange:
        case MirIntrinsic::AtomicCompareExchange:
        case MirIntrinsic::AtomicAdd:
        case MirIntrinsic::AtomicMin:
        case MirIntrinsic::AtomicMax:
        case MirIntrinsic::AtomicAnd:
        case MirIntrinsic::AtomicOr:
        case MirIntrinsic::AtomicXor:
        case MirIntrinsic::WaveIsFirstLane:
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
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}
} // namespace

MirUniformityAnalysis::Result MirUniformityAnalysis::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    (void)analyses;
    Result result;
    for (const MirArgument *argument : function.GetArguments())
    {
        result.NonUniform.Add(argument);
    }
    HashSet<const MirValue *> divergentVariables;
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (MirBlock *block : function.GetBlocks())
        {
            for (MirInstruction *instruction : *block)
            {
                bool divergent = result.NonUniform.Contains(instruction);
                if (!divergent)
                {
                    divergent = IsDivergentSource(*instruction);
                    for (const MirValue *operand : instruction->GetOperands())
                    {
                        divergent = divergent || result.NonUniform.Contains(operand);
                    }
                    if (instruction->GetOp() == MirOp::Load)
                    {
                        const MirValue *root = instruction->GetOperand(0);
                        while (root->GetValueKind() == MirValueKind::Instruction &&
                               static_cast<const MirInstruction *>(root)->GetOp() == MirOp::AccessChain)
                        {
                            root = static_cast<const MirInstruction *>(root)->GetOperand(0);
                        }
                        divergent = divergent || divergentVariables.Contains(root);
                    }
                    if (divergent && !instruction->GetType()->Is(MirTypeKind::Void))
                    {
                        result.NonUniform.Add(instruction);
                        changed = true;
                    }
                }
                if (instruction->GetOp() == MirOp::Store && result.NonUniform.Contains(instruction->GetOperand(1)))
                {
                    const MirValue *root = instruction->GetOperand(0);
                    while (root->GetValueKind() == MirValueKind::Instruction &&
                           static_cast<const MirInstruction *>(root)->GetOp() == MirOp::AccessChain)
                    {
                        root = static_cast<const MirInstruction *>(root)->GetOperand(0);
                    }
                    changed = divergentVariables.Add(root) || changed;
                }
            }
            const MirInstruction *terminator = block->GetTerminator();
            if (block->Merge.Kind != MirMergeKind::None && terminator != nullptr &&
                !terminator->GetOperands().IsEmpty() && result.NonUniform.Contains(terminator->GetOperand(0)))
            {
                for (MirInstruction *phi : *block->Merge.MergeBlock)
                {
                    if (phi->GetOp() != MirOp::Phi)
                    {
                        break;
                    }
                    changed = result.NonUniform.Add(phi) || changed;
                }
            }
        }
    }
    return result;
}

MirPreservedAnalyses MirNonUniformPass::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    const MirUniformityAnalysis::Result &uniformity = analyses.GetResult<MirUniformityAnalysis>(function);
    bool changed = false;
    for (MirBlock *block : function.GetBlocks())
    {
        for (MirInstruction *instruction : *block)
        {
            if (instruction->NonUniform)
            {
                continue;
            }
            const bool takesHandle = instruction->GetOp() == MirOp::Intrinsic || instruction->GetOp() == MirOp::BufferPointer;
            if (!takesHandle)
            {
                continue;
            }
            for (const MirValue *operand : instruction->GetOperands())
            {
                if (operand->GetType()->IsHandle() && uniformity.IsNonUniform(operand))
                {
                    instruction->NonUniform = true;
                    changed = true;
                    break;
                }
            }
        }
    }
    (void)changed;
    return MirPreservedAnalyses::All();
}
} // namespace Marlin
