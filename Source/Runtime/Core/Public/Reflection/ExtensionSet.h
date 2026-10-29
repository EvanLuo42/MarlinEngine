//! Extension points: the implementations of an abstract base, chosen at build time from the reflected subclasses.

#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Reflection/TypeInfo.h"
#include "Reflection/TypeOf.h"
#include "String/String.h"
#include "Reflection/ExtensionSet.gen.h"

namespace Marlin
{
/// The context an extension's `ShouldCreate` sees and `ExtensionSet::Build` filters with.
MSTRUCT()
struct CORE_API ExtensionContext
{
    MGENERATED_BODY()

    /// Type names to leave out, from the owner's config under `Engine.Extensions.Disabled`.
    Span<const String> Disabled;
};

namespace Private
{
/// One implementation as `BuildExtensions` found it.
struct ExtensionEntry
{
    /// The implementation's reflected type.
    const TypeInfo *Type = nullptr;
    /// The object as its own type, for deleting.
    void *Raw = nullptr;
    /// The object as the extension point.
    void *Object = nullptr;
    /// The index of the entry this one replaces through `MCLASS(Replaces = "Name")`, or -1.
    int32 Replaces = -1;
    /// Whether another entry replaces this one.
    bool Replaced = false;
};

/// Finds, filters, orders and creates the implementations of `point` into `entries`.
CORE_API void BuildExtensions(const TypeInfo &point, const ExtensionContext &context, List<ExtensionEntry> &entries);
/// Destroys the objects in `entries` and clears the list.
CORE_API void DestroyExtensions(List<ExtensionEntry> &entries);
} // namespace Private

/// The implementations of an extension point `T`, an abstract `MCLASS(Abstract, ExtensionPoint)`.
///
/// `Build` finds every non-abstract subclass, asks its `ShouldCreate`, drops disabled ones, sorts by
/// `MCLASS(Priority = N)` (higher first, then by name) and creates them. An implementation marked
/// `MCLASS(Replaces = "Other")` takes Other's place: Other no longer appears when iterating, and `GetReplaced` hands
/// it to the replacer to wrap. Immutable once built, so any thread may read it.
template <typename T> class ExtensionSet
{
public:
    ExtensionSet() = default;
    /// Destroys every created implementation.
    ~ExtensionSet()
    {
        Private::DestroyExtensions(_entries);
    }
    MARLIN_NONCOPYABLE(ExtensionSet);

    /// Destroys the previous set and builds a new one from the implementations linked into the process.
    void Build(const ExtensionContext &context = {})
    {
        Private::DestroyExtensions(_entries);
        Private::BuildExtensions(TypeOf<T>(), context, _entries);
        _active.Clear();
        _replaced.Clear();
        for (const Private::ExtensionEntry &entry : _entries)
        {
            if (!entry.Replaced)
            {
                _active.Add(static_cast<T *>(entry.Object));
            }
            if (entry.Replaces >= 0)
            {
                _replaced.Add({static_cast<const T *>(entry.Object),
                               static_cast<T *>(_entries[static_cast<usize>(entry.Replaces)].Object)});
            }
        }
    }

    /// Returns the number of active implementations.
    [[nodiscard]] usize Count() const
    {
        return _active.Count();
    }

    /// Returns the first active implementation.
    [[nodiscard]] T *const *begin() const
    {
        return _active.begin();
    }

    /// Returns one past the last active implementation.
    [[nodiscard]] T *const *end() const
    {
        return _active.end();
    }

    /// Returns the implementation `replacer` replaces, or null.
    [[nodiscard]] T *GetReplaced(const T &replacer) const
    {
        for (const Replacement &replacement : _replaced)
        {
            if (replacement.Replacer == &replacer)
            {
                return replacement.Replaced;
            }
        }
        return nullptr;
    }

    /// Returns the reflected type of `extension`.
    [[nodiscard]] const TypeInfo &GetType(const T &extension) const
    {
        return extension.GetType();
    }

private:
    struct Replacement
    {
        const T *Replacer;
        T *Replaced;
    };

    List<Private::ExtensionEntry> _entries;
    List<T *> _active;
    /// Only the few replacing implementations, so GetReplaced scans a handful at most.
    List<Replacement> _replaced;
};
} // namespace Marlin
