#include "LanguageIR/MirBuilder.h"
#include "LanguageIR/MirPasses.h"

namespace Marlin
{
namespace
{
/// Removes the incoming values of the phis of *block* that come from *predecessor*.
void RemoveIncoming(MirBlock *block, const MirBlock *predecessor)
{
    for (MirInstruction *phi : *block)
    {
        if (phi->GetOp() != MirOp::Phi)
        {
            break;
        }
        for (usize i = phi->GetTargets().Count(); i-- > 0;)
        {
            if (phi->GetTargets()[i] == predecessor)
            {
                phi->RemoveOperand(i);
            }
        }
    }
}

MirPreservedAnalyses CfgChanged(bool changed)
{
    return changed ? MirPreservedAnalyses::None() : MirPreservedAnalyses::All();
}
} // namespace

MirPreservedAnalyses MirDeadBranchPass::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    (void)analyses;
    MirBuilder builder(function.GetModule());
    bool changed = false;
    for (MirBlock *block : function.GetBlocks())
    {
        MirInstruction *terminator = block->GetTerminator();
        if (terminator == nullptr)
        {
            continue;
        }
        MirBlock *taken = nullptr;
        if (terminator->GetOp() == MirOp::CondBranch)
        {
            MirValue *condition = terminator->GetOperand(0);
            if (condition->GetValueKind() == MirValueKind::Constant &&
                static_cast<MirConstant *>(condition)->GetConstantKind() != MirConstantKind::Undef)
            {
                taken = terminator->GetTargets()[static_cast<MirConstant *>(condition)->GetBits() != 0 ? 0 : 1];
            }
            else if (terminator->GetTargets()[0] == terminator->GetTargets()[1])
            {
                taken = terminator->GetTargets()[0];
            }
        }
        else if (terminator->GetOp() == MirOp::Switch &&
                 terminator->GetOperand(0)->GetValueKind() == MirValueKind::Constant)
        {
            const auto *selector = static_cast<MirConstant *>(terminator->GetOperand(0));
            if (selector->GetConstantKind() != MirConstantKind::Undef)
            {
                taken = terminator->GetTargets()[0];
                const Span<const int64> cases = terminator->GetLiterals();
                for (usize i = 0; i < cases.Count(); ++i)
                {
                    if (cases[i] == selector->GetInt())
                    {
                        taken = terminator->GetTargets()[i + 1];
                    }
                }
            }
        }
        if (taken == nullptr)
        {
            continue;
        }
        List<MirBlock *> dropped;
        for (MirBlock *target : terminator->GetTargets())
        {
            if (target != taken && !dropped.Contains(target))
            {
                dropped.Add(target);
            }
        }
        for (MirBlock *target : dropped)
        {
            RemoveIncoming(target, block);
        }
        terminator->Erase();
        builder.SetInsertPoint(block);
        builder.Branch(taken);
        if (block->Merge.Kind == MirMergeKind::Selection)
        {
            block->Merge = {};
        }
        changed = true;
    }
    return CfgChanged(changed);
}

MirPreservedAnalyses MirSimplifyCfgPass::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    if (function.GetEntry() == nullptr)
    {
        return MirPreservedAnalyses::All();
    }
    const MirControlFlow &flow = analyses.GetResult<MirControlFlowAnalysis>(function);
    HashSet<const MirBlock *> structural;
    HashMap<const MirBlock *, MirBlock *> continueHeaders;
    for (MirBlock *block : function.GetBlocks())
    {
        if (flow.IsReachable(block) && block->Merge.Kind != MirMergeKind::None)
        {
            structural.Add(block->Merge.MergeBlock);
            if (block->Merge.ContinueBlock != nullptr)
            {
                structural.Add(block->Merge.ContinueBlock);
                continueHeaders.Add(block->Merge.ContinueBlock, block);
            }
        }
    }
    List<MirBlock *> unreachable;
    for (MirBlock *block : function.GetBlocks())
    {
        if (!flow.IsReachable(block))
        {
            unreachable.Add(block);
        }
    }
    if (unreachable.IsEmpty())
    {
        return MirPreservedAnalyses::All();
    }
    bool changed = false;
    for (MirBlock *block : unreachable)
    {
        for (MirBlock *successor : MirControlFlow::GetSuccessors(block))
        {
            RemoveIncoming(successor, block);
        }
    }
    MirBuilder builder(function.GetModule());
    for (MirBlock *block : unreachable)
    {
        if (structural.Contains(block))
        {
            MirBlock *const *header = continueHeaders.Find(block);
            const MirInstruction *only = block->GetFirst() == block->GetLast() ? block->GetFirst() : nullptr;
            const bool alreadyMinimal =
                only != nullptr && (header != nullptr ? only->GetOp() == MirOp::Branch && only->GetTargets()[0] == *header
                                                      : only->GetOp() == MirOp::Unreachable);
            if (alreadyMinimal)
            {
                continue;
            }
            while (MirInstruction *instruction = block->GetLast())
            {
                block->Unlink(instruction);
                instruction->DropOperands();
            }
            block->Merge = {};
            builder.SetInsertPoint(block);
            if (header != nullptr)
            {
                builder.Branch(*header);
            }
            else
            {
                builder.Terminate(MirOp::Unreachable);
            }
        }
        else
        {
            function.RemoveBlock(block);
        }
        changed = true;
    }
    return CfgChanged(changed);
}

MirPreservedAnalyses MirMergeReturnPass::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    List<MirBlock *> returns;
    for (MirBlock *block : function.GetBlocks())
    {
        const MirInstruction *terminator = block->GetTerminator();
        if (terminator != nullptr && terminator->GetOp() == MirOp::Return)
        {
            returns.Add(block);
        }
    }
    if (returns.Count() <= 1)
    {
        return MirPreservedAnalyses::All();
    }
    const MirControlFlow &flow = analyses.GetResult<MirControlFlowAnalysis>(function);
    for (MirBlock *header : function.GetBlocks())
    {
        if (header->Merge.Kind != MirMergeKind::Loop || !flow.IsReachable(header))
        {
            continue;
        }
        for (const MirBlock *block : returns)
        {
            if (flow.Dominates(header, block) && !flow.Dominates(header->Merge.MergeBlock, block))
            {
                return MirPreservedAnalyses::All();
            }
        }
    }

    MirModule &module = function.GetModule();
    MirBuilder builder(module);
    MirBlock *oldEntry = function.GetEntry();
    MirBlock *entry = function.AddBlock("return.entry");
    function.MoveBlock(entry, 0);
    MirInstruction *instruction = oldEntry->GetFirst();
    while (instruction != nullptr && instruction->GetOp() == MirOp::Variable)
    {
        MirInstruction *next = instruction->GetNext();
        oldEntry->Unlink(instruction);
        entry->Append(instruction);
        instruction = next;
    }
    const MirType *returnType = function.GetReturnType();
    MirInstruction *result = nullptr;
    builder.SetInsertPoint(entry);
    if (!returnType->Is(MirTypeKind::Void))
    {
        result = builder.Variable(returnType, "result");
    }
    MirBlock *header = function.AddBlock("return.header");
    function.MoveBlock(header, 1);
    MirBlock *latch = function.AddBlock("return.continue");
    MirBlock *exit = function.AddBlock("return.exit");
    builder.Branch(header);
    header->Merge = {MirMergeKind::Loop, exit, latch};
    builder.SetInsertPoint(header);
    builder.Branch(oldEntry);
    builder.SetInsertPoint(latch);
    builder.Branch(header);
    builder.SetInsertPoint(exit);
    builder.Return(result != nullptr ? builder.Load(result) : nullptr);

    for (MirBlock *block : returns)
    {
        MirInstruction *terminator = block->GetTerminator();
        MirValue *value = terminator->GetOperands().IsEmpty() ? nullptr : terminator->GetOperand(0);
        block->Unlink(terminator);
        builder.SetInsertPoint(block);
        if (value != nullptr)
        {
            builder.Store(result, value);
        }
        terminator->DropOperands();
        builder.Branch(exit);
    }
    return MirPreservedAnalyses::None();
}
} // namespace Marlin
