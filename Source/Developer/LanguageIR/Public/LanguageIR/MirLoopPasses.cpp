#include "LanguageIR/MirAnalysis.h"
#include "LanguageIR/MirPasses.h"

namespace Marlin
{
namespace
{
struct UnrollableLoop
{
    MirBlock *Header = nullptr;
    MirBlock *Check = nullptr;
    MirBlock *Body = nullptr;
    MirBlock *Latch = nullptr;
    MirBlock *Merge = nullptr;
    MirBlock *Preheader = nullptr;
    List<MirBlock *> Blocks;
    uint32 TripCount = 0;
};

int64 Wrap(int64 value, uint32 width, bool isSigned)
{
    if (width >= 64)
    {
        return value;
    }
    const uint64 mask = (uint64{1} << width) - 1;
    const uint64 bits = static_cast<uint64>(value) & mask;
    if (isSigned && (bits >> (width - 1)) != 0)
    {
        return static_cast<int64>(bits | ~mask);
    }
    return static_cast<int64>(bits);
}

bool Compare(MirOp op, int64 left, int64 right, bool isSigned)
{
    const auto uleft = static_cast<uint64>(left);
    const auto uright = static_cast<uint64>(right);
    switch (op)
    {
    case MirOp::Equal:
        return left == right;
    case MirOp::NotEqual:
        return left != right;
    case MirOp::Less:
        return isSigned ? left < right : uleft < uright;
    case MirOp::LessEqual:
        return isSigned ? left <= right : uleft <= uright;
    case MirOp::Greater:
        return isSigned ? left > right : uleft > uright;
    case MirOp::GreaterEqual:
        return isSigned ? left >= right : uleft >= uright;
    default:
        return false;
    }
}

MirConstant *AsIntConstant(MirValue *value)
{
    if (value->GetValueKind() != MirValueKind::Constant || !value->GetType()->Is(MirTypeKind::Int))
    {
        return nullptr;
    }
    auto *constant = static_cast<MirConstant *>(value);
    return constant->GetConstantKind() == MirConstantKind::Scalar ? constant : nullptr;
}

MirValue *IncomingFrom(MirInstruction *phi, const MirBlock *block)
{
    for (usize i = 0; i < phi->GetTargets().Count(); ++i)
    {
        if (phi->GetTargets()[i] == block)
        {
            return phi->GetOperand(i);
        }
    }
    return nullptr;
}

/// The number of times the condition of *loop* enters the body, or false if it is not a constant within the limit.
bool CountTrips(UnrollableLoop &loop)
{
    MirInstruction *branch = loop.Check->GetTerminator();
    MirValue *condition = branch->GetOperand(0);
    if (condition->GetValueKind() != MirValueKind::Instruction)
    {
        return false;
    }
    auto *compare = static_cast<MirInstruction *>(condition);
    if (compare->GetOperands().Count() != 2)
    {
        return false;
    }
    MirInstruction *phi = nullptr;
    MirConstant *bound = nullptr;
    bool phiOnLeft = true;
    for (usize side = 0; side < 2; ++side)
    {
        MirValue *operand = compare->GetOperand(side);
        if (operand->GetValueKind() == MirValueKind::Instruction &&
            static_cast<MirInstruction *>(operand)->GetOp() == MirOp::Phi &&
            static_cast<MirInstruction *>(operand)->GetParent() == loop.Header)
        {
            phi = static_cast<MirInstruction *>(operand);
            phiOnLeft = side == 0;
            bound = AsIntConstant(compare->GetOperand(1 - side));
        }
    }
    if (phi == nullptr || bound == nullptr || !phi->GetType()->Is(MirTypeKind::Int))
    {
        return false;
    }
    MirConstant *start = AsIntConstant(IncomingFrom(phi, loop.Preheader));
    MirValue *next = IncomingFrom(phi, loop.Latch);
    if (start == nullptr || next == nullptr || next->GetValueKind() != MirValueKind::Instruction)
    {
        return false;
    }
    auto *step = static_cast<MirInstruction *>(next);
    if ((step->GetOp() != MirOp::Add && step->GetOp() != MirOp::Sub) || step->GetOperands().Count() != 2)
    {
        return false;
    }
    MirConstant *amount = nullptr;
    if (step->GetOperand(0) == phi)
    {
        amount = AsIntConstant(step->GetOperand(1));
    }
    else if (step->GetOp() == MirOp::Add && step->GetOperand(1) == phi)
    {
        amount = AsIntConstant(step->GetOperand(0));
    }
    if (amount == nullptr)
    {
        return false;
    }
    const uint32 width = phi->GetType()->GetWidth();
    const bool isSigned = phi->GetType()->IsSigned();
    const int64 delta = step->GetOp() == MirOp::Add ? amount->GetInt() : -amount->GetInt();
    const bool bodyOnTrue = branch->GetTargets()[0] == loop.Body;
    int64 value = start->GetInt();
    uint32 trips = 0;
    while (true)
    {
        const bool result = phiOnLeft ? Compare(compare->GetOp(), value, bound->GetInt(), isSigned)
                                      : Compare(compare->GetOp(), bound->GetInt(), value, isSigned);
        if (result != bodyOnTrue)
        {
            break;
        }
        if (++trips > MirLoopUnrollPass::MaxTripCount)
        {
            return false;
        }
        value = Wrap(value + delta, width, isSigned);
    }
    loop.TripCount = trips;
    return true;
}

/// Whether the loop *header* starts has the shape the pass unrolls; fills *loop* if so.
bool Analyze(MirBlock *header, const MirControlFlow &flow, UnrollableLoop &loop)
{
    loop.Header = header;
    loop.Latch = header->Merge.ContinueBlock;
    loop.Merge = header->Merge.MergeBlock;
    MirInstruction *entry = header->GetTerminator();
    if (entry == nullptr || entry->GetOp() != MirOp::Branch || entry->GetTargets()[0] == header)
    {
        return false;
    }
    loop.Check = entry->GetTargets()[0];
    MirInstruction *branch = loop.Check->GetTerminator();
    if (branch == nullptr || branch->GetOp() != MirOp::CondBranch || loop.Check->Merge.Kind != MirMergeKind::None)
    {
        return false;
    }
    const Span<MirBlock *const> targets = branch->GetTargets();
    if (targets[0] == loop.Merge && targets[1] != loop.Merge)
    {
        loop.Body = targets[1];
    }
    else if (targets[1] == loop.Merge && targets[0] != loop.Merge)
    {
        loop.Body = targets[0];
    }
    else
    {
        return false;
    }
    MirInstruction *back = loop.Latch->GetTerminator();
    if (back == nullptr || back->GetOp() != MirOp::Branch || back->GetTargets()[0] != header)
    {
        return false;
    }
    const Span<MirBlock *const> predecessors = flow.GetPredecessors(header);
    if (predecessors.Count() != 2)
    {
        return false;
    }
    loop.Preheader = predecessors[0] == loop.Latch ? predecessors[1] : predecessors[0];
    if (loop.Preheader == loop.Latch)
    {
        return false;
    }
    loop.Blocks.Clear();
    loop.Blocks.Add(header);
    for (usize i = 0; i < loop.Blocks.Count(); ++i)
    {
        for (MirBlock *successor : MirControlFlow::GetSuccessors(loop.Blocks[i]))
        {
            if (successor == header || loop.Blocks.Contains(successor))
            {
                continue;
            }
            if (successor == loop.Merge)
            {
                if (loop.Blocks[i] != loop.Check)
                {
                    return false;
                }
                continue;
            }
            if (!flow.Dominates(header, successor))
            {
                return false;
            }
            loop.Blocks.Add(successor);
        }
        // A nested construct's merge and continue blocks are inside too, even when nothing reaches them (an inlined
        // callee's return loop never continues): its clone names their clones.
        const MirMerge &merge = loop.Blocks[i]->Merge;
        if (loop.Blocks[i] != header && merge.Kind != MirMergeKind::None)
        {
            for (MirBlock *declared : {merge.MergeBlock, merge.ContinueBlock})
            {
                if (declared == nullptr || declared == header || loop.Blocks.Contains(declared))
                {
                    continue;
                }
                if (declared == loop.Merge || (flow.IsReachable(declared) && !flow.Dominates(header, declared)))
                {
                    return false;
                }
                loop.Blocks.Add(declared);
            }
        }
    }
    if (!loop.Blocks.Contains(loop.Latch))
    {
        return false;
    }
    return CountTrips(loop);
}

usize IndexOf(const MirFunction &function, const MirBlock *block)
{
    const Span<MirBlock *const> blocks = function.GetBlocks();
    for (usize i = 0; i < blocks.Count(); ++i)
    {
        if (blocks[i] == block)
        {
            return i;
        }
    }
    return blocks.Count();
}

MirValue *Mapped(const HashMap<const MirValue *, MirValue *> &values, MirValue *value)
{
    MirValue *const *found = values.Find(value);
    return found != nullptr ? *found : value;
}

void Unroll(MirFunction &function, UnrollableLoop &loop)
{
    MirModule &module = function.GetModule();
    Arena &arena = module.GetArena();
    MirBlock *firstHeader = nullptr;
    MirInstruction *pendingBack = nullptr;
    HashMap<const MirValue *, MirValue *> previous;
    for (uint32 copy = 0; copy <= loop.TripCount; ++copy)
    {
        const bool final = copy == loop.TripCount;
        HashMap<const MirValue *, MirValue *> values;
        for (MirInstruction *phi : *loop.Header)
        {
            if (phi->GetOp() != MirOp::Phi)
            {
                break;
            }
            values.Add(phi, copy == 0 ? IncomingFrom(phi, loop.Preheader)
                                      : Mapped(previous, IncomingFrom(phi, loop.Latch)));
        }
        List<MirBlock *> sources;
        if (final)
        {
            sources.Add(loop.Header);
            sources.Add(loop.Check);
        }
        else
        {
            sources.AddRange(loop.Blocks.AsSpan());
        }
        HashMap<const MirBlock *, MirBlock *> blocks;
        for (MirBlock *source : sources)
        {
            MirBlock *clone = function.AddBlock(source->GetName());
            function.MoveBlock(clone, IndexOf(function, loop.Merge));
            blocks.Add(source, clone);
        }
        List<Pair<MirInstruction *, MirInstruction *>> pending;
        MirInstruction *backEdge = nullptr;
        for (MirBlock *source : sources)
        {
            MirBlock *clone = *blocks.Find(source);
            if (source != loop.Header && source->Merge.Kind != MirMergeKind::None)
            {
                clone->Merge = source->Merge;
                clone->Merge.MergeBlock = *blocks.Find(source->Merge.MergeBlock);
                if (source->Merge.ContinueBlock != nullptr)
                {
                    clone->Merge.ContinueBlock = *blocks.Find(source->Merge.ContinueBlock);
                }
            }
            for (MirInstruction *original : *source)
            {
                if (original->GetOp() == MirOp::Phi && source == loop.Header)
                {
                    continue;
                }
                MirInstruction *instruction = nullptr;
                if (source == loop.Check && original == loop.Check->GetTerminator())
                {
                    instruction = arena.New<MirInstruction>(MirOp::Branch, original->GetType(), arena);
                    instruction->AddTarget(final ? loop.Merge : *blocks.Find(loop.Body));
                }
                else if (source == loop.Latch && original == loop.Latch->GetTerminator())
                {
                    instruction = arena.New<MirInstruction>(MirOp::Branch, original->GetType(), arena);
                    instruction->AddTarget(loop.Header);
                    backEdge = instruction;
                }
                else
                {
                    instruction = arena.New<MirInstruction>(original->GetOp(), original->GetType(), arena);
                    instruction->SetIntrinsic(original->GetIntrinsic());
                    instruction->SetName(original->GetName());
                    instruction->NonUniform = original->NonUniform;
                    instruction->Restrict = original->Restrict;
                    instruction->Precise = original->Precise;
                    for (const int64 literal : original->GetLiterals())
                    {
                        instruction->AddLiteral(literal);
                    }
                    for (MirBlock *target : original->GetTargets())
                    {
                        instruction->AddTarget(*blocks.Find(target));
                    }
                    pending.Add({original, instruction});
                }
                instruction->Location = original->Location;
                clone->Append(instruction);
                values.Add(original, instruction);
            }
        }
        for (const auto &[original, instruction] : pending)
        {
            for (MirValue *operand : original->GetOperands())
            {
                instruction->AddOperand(Mapped(values, operand));
            }
        }
        MirBlock *header = *blocks.Find(loop.Header);
        if (pendingBack != nullptr)
        {
            pendingBack->SetTarget(0, header);
        }
        else
        {
            firstHeader = header;
        }
        pendingBack = backEdge;
        if (final)
        {
            for (MirBlock *source : sources)
            {
                for (MirInstruction *original : *source)
                {
                    const List<MirUse> uses(original->GetUses());
                    for (const MirUse &use : uses)
                    {
                        if (!loop.Blocks.Contains(use.User->GetParent()))
                        {
                            use.User->SetOperand(use.Index, Mapped(values, original));
                        }
                    }
                }
            }
            for (MirInstruction *phi : *loop.Merge)
            {
                if (phi->GetOp() != MirOp::Phi)
                {
                    break;
                }
                for (usize i = 0; i < phi->GetTargets().Count(); ++i)
                {
                    if (phi->GetTargets()[i] == loop.Check)
                    {
                        phi->SetTarget(i, *blocks.Find(loop.Check));
                    }
                }
            }
        }
        previous = Move(values);
    }
    MirInstruction *enter = loop.Preheader->GetTerminator();
    for (usize i = 0; i < enter->GetTargets().Count(); ++i)
    {
        if (enter->GetTargets()[i] == loop.Header)
        {
            enter->SetTarget(i, firstHeader);
        }
    }
    for (MirBlock *block : loop.Blocks)
    {
        for (MirInstruction *instruction : *block)
        {
            instruction->DropOperands();
        }
    }
    for (MirBlock *block : loop.Blocks)
    {
        function.RemoveBlock(block);
    }
}
} // namespace

bool GetConstantTripCount(MirBlock *header, const MirControlFlow &flow, uint32 &tripCount)
{
    UnrollableLoop loop;
    if (header->Merge.Kind != MirMergeKind::Loop || !Analyze(header, flow, loop))
    {
        return false;
    }
    tripCount = loop.TripCount;
    return true;
}

MirPreservedAnalyses MirLoopUnrollPass::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    (void)analyses;
    bool changed = false;
    bool progress = function.GetEntry() != nullptr;
    while (progress)
    {
        progress = false;
        const MirControlFlow flow(function);
        for (MirBlock *block : flow.GetReversePostOrder())
        {
            if (block->Merge.Kind != MirMergeKind::Loop || block->Merge.Control != MirLoopControl::Unroll ||
                block->Merge.UnrollCount != 0)
            {
                continue;
            }
            UnrollableLoop loop;
            if (Analyze(block, flow, loop))
            {
                Unroll(function, loop);
                progress = true;
                changed = true;
                break;
            }
        }
    }
    return changed ? MirPreservedAnalyses::None() : MirPreservedAnalyses::All();
}
} // namespace Marlin
