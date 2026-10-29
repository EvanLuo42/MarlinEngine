#include "LanguageIR/MirBuilder.h"
#include "LanguageIR/MirPasses.h"
#include "Templates/Utility.h"

namespace Marlin
{
namespace
{
/// Whether *function* has a custom derivative that MirAutodiffPass has not used yet: calls to it stay calls until
/// then, so differentiation sees them (§9.12).
bool HasPendingDerivative(const MirFunction &function)
{
    return function.ForwardDerivative != nullptr || function.ForwardDerivative2 != nullptr ||
           function.BackwardDerivative != nullptr;
}

bool IsInlinable(const MirFunction &callee)
{
    if (!callee.AlwaysInline || callee.GetEntry() == nullptr || HasPendingDerivative(callee))
    {
        return false;
    }
    usize returns = 0;
    for (const MirBlock *block : callee.GetBlocks())
    {
        const MirInstruction *terminator = block->GetTerminator();
        returns += terminator != nullptr && terminator->GetOp() == MirOp::Return ? 1 : 0;
    }
    return returns == 1;
}

void InlineCall(MirFunction &caller, MirInstruction *call)
{
    MirModule &module = caller.GetModule();
    Arena &arena = module.GetArena();
    auto *callee = static_cast<MirFunction *>(call->GetOperand(0));
    MirBlock *block = call->GetParent();

    MirBlock *after = caller.AddBlock("inline.after");
    usize position = 0;
    while (caller.GetBlocks()[position] != block)
    {
        ++position;
    }
    caller.MoveBlock(after, position + 1);
    while (MirInstruction *moved = call->GetNext())
    {
        block->Unlink(moved);
        after->Append(moved);
    }
    after->Merge = block->Merge;
    block->Merge = {};
    for (MirBlock *successor : MirControlFlow::GetSuccessors(after))
    {
        for (MirInstruction *phi : *successor)
        {
            if (phi->GetOp() != MirOp::Phi)
            {
                break;
            }
            for (usize i = 0; i < phi->GetTargets().Count(); ++i)
            {
                if (phi->GetTargets()[i] == block)
                {
                    phi->SetTarget(i, after);
                }
            }
        }
    }

    HashMap<const MirValue *, MirValue *> values;
    for (usize i = 0; i < callee->GetArguments().Count(); ++i)
    {
        values.Add(callee->GetArguments()[i], call->GetOperand(i + 1));
    }
    HashMap<const MirBlock *, MirBlock *> blocks;
    List<MirBlock *> clones;
    for (usize i = 0; i < callee->GetBlocks().Count(); ++i)
    {
        MirBlock *clone = caller.AddBlock(callee->GetBlocks()[i]->GetName());
        caller.MoveBlock(clone, position + 1 + i);
        blocks.Add(callee->GetBlocks()[i], clone);
        clones.Add(clone);
    }
    MirBlock *callerEntry = caller.GetEntry();
    MirInstruction *firstNonVariable = callerEntry->GetFirst();
    while (firstNonVariable != nullptr && firstNonVariable->GetOp() == MirOp::Variable)
    {
        firstNonVariable = firstNonVariable->GetNext();
    }
    List<Pair<MirInstruction *, MirInstruction *>> pending;
    MirValue *returned = nullptr;
    for (usize i = 0; i < callee->GetBlocks().Count(); ++i)
    {
        const MirBlock *source = callee->GetBlocks()[i];
        MirBlock *clone = clones[i];
        if (source->Merge.Kind != MirMergeKind::None)
        {
            clone->Merge.Kind = source->Merge.Kind;
            clone->Merge.MergeBlock = *blocks.Find(source->Merge.MergeBlock);
            clone->Merge.ContinueBlock =
                source->Merge.ContinueBlock != nullptr ? *blocks.Find(source->Merge.ContinueBlock) : nullptr;
            clone->Merge.Control = source->Merge.Control;
            clone->Merge.UnrollCount = source->Merge.UnrollCount;
            clone->Merge.MaxIterations = source->Merge.MaxIterations;
        }
        for (MirInstruction *original : *source)
        {
            if (original->GetOp() == MirOp::Return)
            {
                returned = original->GetOperands().IsEmpty() ? nullptr : original->GetOperand(0);
                MirInstruction *branch = arena.New<MirInstruction>(MirOp::Branch, module.GetTypes().GetVoid(), arena);
                branch->Location = original->Location;
                branch->AddTarget(after);
                clone->Append(branch);
                continue;
            }
            MirInstruction *copy = arena.New<MirInstruction>(original->GetOp(), original->GetType(), arena);
            copy->SetIntrinsic(original->GetIntrinsic());
            copy->SetName(original->GetName());
            copy->Location = original->Location;
            copy->NonUniform = original->NonUniform;
            copy->Restrict = original->Restrict;
            copy->Precise = original->Precise;
            copy->Checked = original->Checked;
            for (const int64 literal : original->GetLiterals())
            {
                copy->AddLiteral(literal);
            }
            for (MirBlock *target : original->GetTargets())
            {
                copy->AddTarget(*blocks.Find(target));
            }
            if (original->GetOp() == MirOp::Variable)
            {
                callerEntry->InsertBefore(copy, firstNonVariable);
            }
            else
            {
                clone->Append(copy);
            }
            values.Add(original, copy);
            pending.Add({original, copy});
        }
    }
    for (const auto &[original, copy] : pending)
    {
        for (MirValue *operand : original->GetOperands())
        {
            MirValue **mapped = values.Find(operand);
            copy->AddOperand(mapped != nullptr ? *mapped : operand);
        }
    }
    if (returned != nullptr)
    {
        MirValue **mapped = values.Find(returned);
        call->ReplaceAllUsesWith(mapped != nullptr ? *mapped : returned);
    }
    call->Erase();
    MirInstruction *jump = arena.New<MirInstruction>(MirOp::Branch, module.GetTypes().GetVoid(), arena);
    jump->AddTarget(clones[0]);
    block->Append(jump);
}
} // namespace

MirPreservedAnalyses MirInlinePass::Run(MirModule &module, MirModuleAnalysisManager &analyses)
{
    bool changed = false;
    bool progress = true;
    while (progress)
    {
        progress = false;
        for (MirFunction *function : module.GetFunctions())
        {
            for (usize b = 0; b < function->GetBlocks().Count(); ++b)
            {
                for (MirInstruction *instruction : *function->GetBlocks()[b])
                {
                    if (instruction->GetOp() != MirOp::Call)
                    {
                        continue;
                    }
                    auto *callee = static_cast<MirFunction *>(instruction->GetOperand(0));
                    if (callee == function || !IsInlinable(*callee))
                    {
                        continue;
                    }
                    bool calleeCalls = false;
                    for (const MirBlock *calleeBlock : callee->GetBlocks())
                    {
                        for (const MirInstruction *inner : *calleeBlock)
                        {
                            // A call held for its custom derivative is inlined later; the callee goes in with it.
                            calleeCalls = calleeCalls ||
                                          (inner->GetOp() == MirOp::Call &&
                                           !HasPendingDerivative(*static_cast<MirFunction *>(inner->GetOperand(0))));
                        }
                    }
                    if (calleeCalls)
                    {
                        continue;
                    }
                    analyses.GetFunctionAnalyses().Clear(*function);
                    InlineCall(*function, instruction);
                    progress = true;
                    changed = true;
                    break;
                }
            }
        }
    }
    bool removed = true;
    while (removed)
    {
        removed = false;
        List<MirFunction *> dead;
        for (MirFunction *function : module.GetFunctions())
        {
            bool kept = false;
            for (const MirEntryPoint *entry : module.GetEntryPoints())
            {
                kept = kept || entry->Function == function;
            }
            // Custom derivatives are kept for MirAutodiffPass while the functions naming them are.
            for (const MirFunction *other : module.GetFunctions())
            {
                kept = kept || other->ForwardDerivative == function || other->ForwardDerivative2 == function ||
                       other->BackwardDerivative == function;
            }
            // RECOVERY-DECISION lang-ir-D6: only dead AlwaysInline functions go, so script functions the VM calls stay.
            if (!kept && !function->HasUses() && function->AlwaysInline)
            {
                dead.Add(function);
            }
        }
        for (MirFunction *function : dead)
        {
            analyses.GetFunctionAnalyses().Clear(*function);
            module.RemoveFunction(function);
            removed = true;
            changed = true;
        }
    }
    return changed ? MirPreservedAnalyses::None() : MirPreservedAnalyses::All();
}
} // namespace Marlin
