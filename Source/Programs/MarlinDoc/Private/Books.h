#pragma once

#include "Model.h"

namespace Marlin::Doc
{
/// Reads the book in *directory* from its Summary.md (Documentation/Architecture/16 §2); pages Summary.md does not
/// list are reported.
bool ReadBook(StringView directory, StringView root, Book &book, Reporter &reporter);

/// The headings of *markdown* outside code blocks, with their anchors; repeated anchors get `-1`, `-2`….
List<Heading> ReadHeadings(StringView markdown);
} // namespace Marlin::Doc
