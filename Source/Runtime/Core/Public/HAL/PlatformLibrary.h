//! Loading dynamic libraries and resolving their exported symbols.
//!
//! A library stays loaded until `Unload`; a symbol address from `FindSymbol` is valid only while it is loaded.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// A loaded dynamic library.
struct LibraryHandle
{
    /// The OS module handle, or null for an invalid handle.
    void *Value = nullptr;

    /// Returns whether the handle refers to a loaded library.
    bool IsValid() const
    {
        return Value != nullptr;
    }
};

/// Loads dynamic libraries and resolves their symbols at run time.
namespace PlatformLibrary
{
/// Loads the dynamic library at `path`, a UTF-8 path.
///
/// An absolute `path` also searches its own directory for the library's dependencies. A relative path uses the
/// default library search order.
///
/// # Errors
/// Returns an invalid handle when the file is missing, `path` is not valid UTF-8, or a dependency cannot be
/// loaded.
///
/// # Examples
///
/// ```cpp
/// using GetInstanceProcAddr = void *(*)(void *, const char *);
/// const LibraryHandle library = PlatformLibrary::Load("vulkan-1.dll");
/// if (library.IsValid())
/// {
///     auto getInstanceProcAddr =
///         reinterpret_cast<GetInstanceProcAddr>(PlatformLibrary::FindSymbol(library, "vkGetInstanceProcAddr"));
///     PlatformLibrary::Unload(library);
/// }
/// ```
CORE_API LibraryHandle Load(const char *path);
/// Unloads `library` and releases its OS module. An invalid handle is ignored.
CORE_API void Unload(LibraryHandle library);
/// Returns the address of the export named `name`, or null when the library does not export it.
///
/// # Preconditions
/// `library` is valid and `name` is the exact export name; a C++ function is found only under the name it was
/// exported with, which for a C-linkage function is its plain name.
CORE_API void *FindSymbol(LibraryHandle library, const char *name);
} // namespace PlatformLibrary
} // namespace Marlin
