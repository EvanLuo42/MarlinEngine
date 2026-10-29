//! OS queries for well-known folders, environment variables, version and random bytes.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// A well-known user or system folder.
enum class KnownFolder : uint8
{
    /// The user's home (profile) directory.
    Home,
    /// The per-user local application data directory.
    LocalAppData,
    /// The per-user roaming application data directory.
    RoamingAppData,
    /// The user's documents directory.
    Documents,
    /// The user's saved-games directory.
    SavedGames,
    /// The temporary directory.
    Temp,
    /// The user's desktop directory.
    Desktop,
    /// The user's downloads directory.
    Downloads,
    /// The user's pictures directory.
    Pictures,
    /// The user's music directory.
    Music,
    /// The user's videos directory.
    Videos,
};

/// OS queries.
///
/// Functions that fill a buffer return the UTF-8 length without the terminator and write the terminator only when
/// the length is less than the capacity. Paths use `/`.
namespace PlatformSystem
{
/// Writes the path of `folder` into `buffer` and returns its UTF-8 length, or zero when it is unavailable.
///
/// The folder is created when the OS supports it.
CORE_API usize GetKnownFolder(KnownFolder folder, char *buffer, usize capacity);

/// Writes the value of `name` into `buffer` and returns its UTF-8 length, or zero when the variable is unset.
CORE_API usize GetEnvironmentVariable(const char *name, char *buffer, usize capacity);
/// Sets `name` to `value`, or removes it when `value` is null.
///
/// Returns whether the OS accepted the change.
CORE_API bool SetEnvironmentVariable(const char *name, const char *value);

/// Writes the OS version string, such as `Windows 10.0.26100`, and returns its length.
CORE_API usize GetOsVersion(char *buffer, usize capacity);
/// Writes the computer name and returns its length.
CORE_API usize GetComputerName(char *buffer, usize capacity);
/// Writes the user name and returns its length.
CORE_API usize GetUserName(char *buffer, usize capacity);
/// Writes the user's BCP 47 language tag, such as `en-US`, and returns its length.
CORE_API usize GetUserLanguage(char *buffer, usize capacity);

/// Fills `size` bytes at `buffer` from the OS cryptographic random source.
///
/// Failure is fatal.
CORE_API void GetSecureRandomBytes(void *buffer, usize size);
} // namespace PlatformSystem
} // namespace Marlin
