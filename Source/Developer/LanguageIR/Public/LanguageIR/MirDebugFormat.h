#pragma once

#include "Containers/List.h"
#include "String/String.h"

namespace Marlin
{
/// A piece of a `Debug.Print` format (§9.11): literal text, or a reference to an argument.
struct MirFormatSegment
{
    /// The text of a literal segment, braces unescaped.
    StringView Text;
    /// The argument a reference prints; ~0u for literal text.
    uint32 Argument = ~0u;
    /// `{n:x}`: an integer printed in hexadecimal.
    bool Hexadecimal = false;
};

/// Splits *format* into *segments*; false with *error* set if a brace is unmatched or a reference is malformed.
/// Escaped braces (`{{`, `}}`) become literal segments of their own.
LANGUAGEIR_API bool ParseMirDebugFormat(StringView format, List<MirFormatSegment> &segments, String &error);
} // namespace Marlin
