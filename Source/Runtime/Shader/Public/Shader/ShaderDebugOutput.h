#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
/// The debug output buffer that shaders compiled with debug output append `Debug.Print` and `Debug.Assert` records
/// to (MarlinLanguage.md §9.11), in 32-bit words: the words appended, the words the records may take, the records,
/// then scratch words that records which do not fit are written to instead. The compiler's side is
/// MirExpandDebugPrintPass; the two must agree.
inline constexpr uint32 ShaderDebugCount = 0;
inline constexpr uint32 ShaderDebugCapacity = 1;
inline constexpr uint32 ShaderDebugHeaderWords = 2;
inline constexpr uint32 ShaderDebugScratchWords = 64;

/// What a record names its format by: the low 32 bits of the XXH3-64 of the format's text.
[[nodiscard]] SHADER_API uint32 GetShaderDebugFormatHash(StringView format);

/// One message of a debug output buffer.
struct ShaderDebugMessage
{
    String Text;
    /// A failed `Debug.Assert`, rather than a `Debug.Print`.
    bool Assertion = false;
};

/// Formats the records of *words*, the contents of a debug output buffer, with the formats *formats* holds by their
/// hash, into *messages* in the order they were appended. Returns the words that did not fit; a malformed record
/// ends the reading.
SHADER_API uint32 ReadShaderDebugOutput(Span<const uint32> words, const HashMap<uint32, String> &formats,
                                        List<ShaderDebugMessage> &messages);
} // namespace Marlin
