#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"

namespace Marlin::Inflate
{
/// Decompresses a zlib stream (RFC 1950 around RFC 1951 deflate) into *output* and checks its Adler-32. A non-zero
/// *expected* is the exact size the data must inflate to; it is reserved up front and anything longer fails.
bool Zlib(Span<const uint8> input, List<uint8> &output, usize expected = 0);
} // namespace Marlin::Inflate
