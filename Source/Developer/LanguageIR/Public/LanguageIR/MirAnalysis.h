#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "LanguageIR/MirModule.h"

namespace Marlin
{
/// The predecessors, reverse post-order and immediate dominators of the blocks of a function; rebuild after
/// changing its control flow.
class LANGUAGEIR_API MirControlFlow
{
public:
    explicit MirControlFlow(const MirFunction &function);

    [[nodiscard]] Span<MirBlock *const> GetPredecessors(const MirBlock *block) const;
    [[nodiscard]] static Span<MirBlock *const> GetSuccessors(const MirBlock *block);

    /// The blocks reachable from the entry, each before its successors except along back edges.
    [[nodiscard]] Span<MirBlock *const> GetReversePostOrder() const
    {
        return _order.AsSpan();
    }

    [[nodiscard]] bool IsReachable(const MirBlock *block) const
    {
        return _index.Contains(block);
    }

    /// The immediate dominator of *block*; null for the entry and unreachable blocks.
    [[nodiscard]] MirBlock *GetImmediateDominator(const MirBlock *block) const;
    /// Whether *a* dominates *b*; every block dominates itself.
    [[nodiscard]] bool Dominates(const MirBlock *a, const MirBlock *b) const;
    /// The blocks *block* immediately dominates.
    [[nodiscard]] Span<MirBlock *const> GetDominatedChildren(const MirBlock *block) const;
    /// The dominance frontier of *block*.
    [[nodiscard]] Span<MirBlock *const> GetDominanceFrontier(const MirBlock *block) const;

private:
    HashMap<const MirBlock *, List<MirBlock *>> _predecessors;
    HashMap<const MirBlock *, List<MirBlock *>> _children;
    HashMap<const MirBlock *, List<MirBlock *>> _frontiers;
    HashMap<const MirBlock *, uint32> _index;
    List<MirBlock *> _order;
    List<uint32> _dominators;
};
} // namespace Marlin
