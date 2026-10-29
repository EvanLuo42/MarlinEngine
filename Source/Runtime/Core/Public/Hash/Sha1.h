//! SHA-1 (FIPS 180-4) for formats that name it, such as name-based Guids.
//!
//! It is not a secure hash; do not use it for authentication or signatures.

#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"

namespace Marlin
{
/// An incremental SHA-1 hasher.
///
/// Feed data with `Update`, then call `Finish` once. The hasher must be reset before it is used for another message.
///
/// # Thread safety
/// Not thread-safe; use one instance per thread.
class CORE_API Sha1
{
public:
    /// The digest size in bytes.
    static constexpr usize DigestSize = 20;

    /// Creates a hasher in the initial state.
    Sha1();

    /// Adds `data` to the message.
    void Update(Span<const uint8> data);
    /// Writes the digest and ends the message.
    ///
    /// Call `Reset` before reusing the hasher.
    void Finish(uint8 (&digest)[DigestSize]);
    /// Returns the hasher to its initial state.
    void Reset();

    /// Hashes `data` in one call and writes the digest.
    static void Hash(Span<const uint8> data, uint8 (&digest)[DigestSize]);

private:
    void Compress(const uint8 *block);

    uint32 _state[5];
    uint8 _buffer[64];
    uint64 _length = 0;
};
} // namespace Marlin
