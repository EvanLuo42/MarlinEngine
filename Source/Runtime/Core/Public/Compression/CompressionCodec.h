//! Block compression codecs and the per-process set of them.
//!
//! Codecs are stateless extension points chosen by name; the concrete codecs live in their own modules.

#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Reflection/ExtensionSet.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Compression/CompressionCodec.gen.h"

namespace Marlin
{
/// A block compressor chosen by name when data is written and when it is read.
///
/// Packages name the codec their blocks use, and a process decompresses with whatever codec of that name is linked.
/// `Lz4` and `Zstd` live in their own modules; a hardware decoder can be added the same way. Implementations are
/// stateless and safe to call from any thread. The engine owns the process's [`CompressionCodecSet`].
MCLASS(Abstract, ExtensionPoint)
class CORE_API CompressionCodec
{
    MGENERATED_BODY()
public:
    CompressionCodec() = default;
    virtual ~CompressionCodec() = default;
    MARLIN_NONCOPYABLE(CompressionCodec);

    /// Returns the codec's name, such as `Lz4`.
    ///
    /// The name is written into packages, so it must not change.
    [[nodiscard]] virtual StringView GetName() const = 0;
    /// Appends `input` compressed to `output` at `level`.
    ///
    /// `level` 0 means the codec's default; higher values produce smaller output more slowly. Returns false when the
    /// data does not benefit from compression, in which case the caller stores it as is.
    virtual bool Compress(Span<const uint8> input, int32 level, List<uint8> &output) const = 0;
    /// Decompresses exactly `output.Count()` bytes from `input`.
    ///
    /// Returns false on corrupt data or when the output size does not match.
    virtual bool Decompress(Span<const uint8> input, Span<uint8> output) const = 0;
};

/// The codecs linked into the process, indexed by name.
///
/// Immutable once built. Lookup is case-insensitive; if two codecs have the same name, the first one in extension
/// order wins.
class CORE_API CompressionCodecSet
{
public:
    CompressionCodecSet() = default;
    MARLIN_NONCOPYABLE(CompressionCodecSet);

    /// Builds or rebuilds the set from the codecs linked into the process.
    ///
    /// `context` decides which extensions are created. Calling it again replaces the previous set.
    void Build(const ExtensionContext &context);
    /// Returns the codec named `name`, case-insensitively, or null.
    ///
    /// # Thread safety
    /// Safe to call from any thread once the set is built.
    [[nodiscard]] const CompressionCodec *Find(StringView name) const;

private:
    ExtensionSet<CompressionCodec> _codecs;
    HashMap<String, CompressionCodec *> _byName;
};
} // namespace Marlin
