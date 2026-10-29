#pragma once

#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "String/StringView.h"

namespace Marlin
{
class MarlinTextObject;
struct MarlinTextValue;
class StringWriter;

/// The canonical form of a source object, for hashing: links by Guid only, numbers and strings normalized, no
/// comments or layout, so moving a file or reformatting it does not invalidate its conversions. The scanner stores
/// the hash of every object in the index, so dependency checks never read a source file.
namespace AssetCanonical
{
ASSETPIPELINE_API void Write(const MarlinTextObject &object, StringWriter &output);
[[nodiscard]] ASSETPIPELINE_API Hash128 Hash(const MarlinTextObject &object);
/// Writes *text* as length:text, which no text can be confused with.
ASSETPIPELINE_API void WriteText(StringWriter &output, StringView text);
ASSETPIPELINE_API void WriteNumber(StringWriter &output, uint64 value);
ASSETPIPELINE_API void WriteValue(StringWriter &output, const MarlinTextValue &value);
} // namespace AssetCanonical
} // namespace Marlin
