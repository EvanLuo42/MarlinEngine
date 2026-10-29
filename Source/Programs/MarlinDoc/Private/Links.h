#pragma once

#include "Model.h"
#include "Templates/Function.h"

namespace Marlin::Doc
{
enum class LinkKind : uint8
{
    /// `` [`Name`] `` or `` [text](`Name`) ``.
    Symbol,
    /// `[text](manual:Book/Page#anchor)`.
    Manual,
    /// `[text](Other.md#anchor)` between the pages of the books.
    Page,
};

/// A link of a Markdown text: [Begin, End) is all of it, *Text* what it shows and *Target* what it names, without
/// backticks.
struct LinkOccurrence
{
    LinkKind Kind = LinkKind::Symbol;
    usize Begin = 0;
    usize End = 0;
    StringView Text;
    StringView Target;
};

/// Calls *visitor* with each link of *markdown* outside code; Page links only if *pageLinks*.
void ForEachLink(StringView markdown, bool pageLinks, FunctionRef<void(const LinkOccurrence &)> visitor);

/// Resolves every link of the docs, books and module docs, the names in signatures, bases and extended types, and
/// fills in the reverse relations (Documentation/Architecture/16 §3.4).
void ResolveLinks(Project &project, Reporter &reporter);

/// The canonical form of a qualified name: `.` between the parts, generic arguments left out.
String CanonicalName(StringView name);
} // namespace Marlin::Doc
