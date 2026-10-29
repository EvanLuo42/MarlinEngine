#include "LanguageIR/MirAnalysis.h"

namespace Marlin
{
MirControlFlow::MirControlFlow(const MirFunction &function)
{
    MirBlock *entry = function.GetEntry();
    if (entry == nullptr)
    {
        return;
    }
    List<MirBlock *> postOrder;
    HashMap<const MirBlock *, bool> visited;
    struct Frame
    {
        MirBlock *Block;
        usize Next;
    };
    List<Frame> stack;
    stack.Add({entry, 0});
    visited.Add(entry, true);
    while (!stack.IsEmpty())
    {
        Frame &frame = stack.Last();
        const Span<MirBlock *const> successors = GetSuccessors(frame.Block);
        if (frame.Next < successors.Count())
        {
            MirBlock *successor = successors[frame.Next++];
            if (visited.TryAdd(successor, true))
            {
                stack.Add({successor, 0});
            }
            continue;
        }
        postOrder.Add(frame.Block);
        stack.Pop();
    }
    for (usize i = postOrder.Count(); i-- > 0;)
    {
        _index.Add(postOrder[i], static_cast<uint32>(_order.Count()));
        _order.Add(postOrder[i]);
    }
    for (MirBlock *block : _order)
    {
        for (MirBlock *successor : GetSuccessors(block))
        {
            List<MirBlock *> &predecessors = _predecessors.FindOrAdd(successor);
            if (!predecessors.Contains(block))
            {
                predecessors.Add(block);
            }
        }
    }

    constexpr uint32 Undefined = ~0u;
    _dominators.Resize(_order.Count(), Undefined);
    _dominators[0] = 0;
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (usize i = 1; i < _order.Count(); ++i)
        {
            uint32 dominator = Undefined;
            for (const MirBlock *predecessor : GetPredecessors(_order[i]))
            {
                uint32 other = *_index.Find(predecessor);
                if (_dominators[other] == Undefined)
                {
                    continue;
                }
                if (dominator == Undefined)
                {
                    dominator = other;
                    continue;
                }
                uint32 a = dominator;
                while (a != other)
                {
                    while (a > other)
                    {
                        a = _dominators[a];
                    }
                    while (other > a)
                    {
                        other = _dominators[other];
                    }
                }
                dominator = a;
            }
            if (_dominators[i] != dominator)
            {
                _dominators[i] = dominator;
                changed = true;
            }
        }
    }
    for (usize i = 1; i < _order.Count(); ++i)
    {
        _children.FindOrAdd(_order[_dominators[i]]).Add(_order[i]);
    }
    for (usize i = 0; i < _order.Count(); ++i)
    {
        const Span<MirBlock *const> predecessors = GetPredecessors(_order[i]);
        if (predecessors.Count() < 2)
        {
            continue;
        }
        for (const MirBlock *predecessor : predecessors)
        {
            uint32 runner = *_index.Find(predecessor);
            while (runner != _dominators[i])
            {
                List<MirBlock *> &frontier = _frontiers.FindOrAdd(_order[runner]);
                if (!frontier.Contains(_order[i]))
                {
                    frontier.Add(_order[i]);
                }
                runner = _dominators[runner];
            }
        }
    }
}

Span<MirBlock *const> MirControlFlow::GetPredecessors(const MirBlock *block) const
{
    const List<MirBlock *> *predecessors = _predecessors.Find(block);
    return predecessors != nullptr ? predecessors->AsSpan() : Span<MirBlock *const>();
}

Span<MirBlock *const> MirControlFlow::GetSuccessors(const MirBlock *block)
{
    const MirInstruction *terminator = block->GetTerminator();
    return terminator != nullptr ? terminator->GetTargets() : Span<MirBlock *const>();
}

MirBlock *MirControlFlow::GetImmediateDominator(const MirBlock *block) const
{
    const uint32 *index = _index.Find(block);
    return index == nullptr || *index == 0 ? nullptr : _order[_dominators[*index]];
}

bool MirControlFlow::Dominates(const MirBlock *a, const MirBlock *b) const
{
    const uint32 *indexA = _index.Find(a);
    const uint32 *indexB = _index.Find(b);
    if (indexA == nullptr || indexB == nullptr)
    {
        return false;
    }
    uint32 runner = *indexB;
    while (runner > *indexA)
    {
        runner = _dominators[runner];
    }
    return runner == *indexA;
}

Span<MirBlock *const> MirControlFlow::GetDominatedChildren(const MirBlock *block) const
{
    const List<MirBlock *> *children = _children.Find(block);
    return children != nullptr ? children->AsSpan() : Span<MirBlock *const>();
}

Span<MirBlock *const> MirControlFlow::GetDominanceFrontier(const MirBlock *block) const
{
    const List<MirBlock *> *frontier = _frontiers.Find(block);
    return frontier != nullptr ? frontier->AsSpan() : Span<MirBlock *const>();
}
} // namespace Marlin
