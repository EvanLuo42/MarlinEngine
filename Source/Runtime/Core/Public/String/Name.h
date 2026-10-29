//! Interned names: 32-bit indices into a process-wide, append-only string table.
//!
//! A name compares, hashes and copies in constant time, and its text lives for the life of the process.

#pragma once

#include "CoreTypes.h"
#include "Hash/Hash.h"
#include "String/StringView.h"

namespace Marlin
{
class StringWriter;

/// An interned string: a 32-bit index into a process-wide table.
///
/// Names are compared and hashed by index, so both are O(1). The table is case-sensitive and its entries are never
/// freed, which makes the text of every [`ToView`] valid for the life of the process. Use names for type names,
/// component names and config keys, not for arbitrary text.
///
/// # Thread safety
/// Any thread may construct, find and read names. Adding a name takes a per-shard lock, and the table is built on
/// first use.
class CORE_API Name
{
public:
    /// Creates `None`, the empty name.
    constexpr Name() = default;

    /// Interns `text`, adding it when it is new; empty text gives `None`.
    explicit Name(StringView text);

    /// Returns the name of `text` if it was interned before, or `None`; never adds.
    [[nodiscard]] static Name Find(StringView text);

    /// Returns the name at `index`, which must come from [`GetIndex`] on a name in the same table.
    ///
    /// # Preconditions
    /// `index` is 0 (`None`) or a value returned by `GetIndex`. Any other value is not checked and makes
    /// [`ToView`] read outside the table.
    [[nodiscard]] static constexpr Name FromIndex(uint32 index)
    {
        Name name;
        name._index = index;
        return name;
    }

    /// Returns whether this is `None`, the empty name.
    [[nodiscard]] constexpr bool IsNone() const
    {
        return _index == 0;
    }

    /// Returns the 32-bit table index; `None` has index 0.
    [[nodiscard]] constexpr uint32 GetIndex() const
    {
        return _index;
    }

    /// Returns the interned text, valid for the life of the process.
    ///
    /// `None` returns an empty view.
    [[nodiscard]] StringView ToView() const;

    /// Returns a hash of the table index, not of the characters.
    ///
    /// Equal names hash equally, so a `Name` and a `StringView` with the same text hash differently.
    [[nodiscard]] constexpr uint64 GetHash() const
    {
        return HashInteger(_index);
    }

    /// Writes the text, ignoring `spec`; this makes a `Name` usable as a format argument.
    void FormatTo(StringWriter &output, StringView spec) const;

    /// Compares the indices, which is the same as comparing the text because names are interned.
    constexpr bool operator==(const Name &) const = default;

private:
    uint32 _index = 0;
};
} // namespace Marlin
