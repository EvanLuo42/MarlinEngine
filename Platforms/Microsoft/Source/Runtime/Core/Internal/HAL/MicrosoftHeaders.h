#pragma once

#include "CoreTypes.h"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#undef CreateDirectory
#undef GetComputerName
#undef GetCurrentDirectory
#undef GetEnvironmentVariable
#undef GetUserName
#undef SetEnvironmentVariable
#undef Yield

namespace Marlin::Microsoft
{
/// Same buffer convention as PlatformProcess.
CORE_API usize Utf8ToWide(const char *text, wchar_t *buffer, usize capacity);
/// Same buffer convention as PlatformProcess.
CORE_API usize WideToUtf8(const wchar_t *text, char *buffer, usize capacity);
/// Like WideToUtf8, with '\' turned into '/'.
CORE_API usize WideToUtf8Path(const wchar_t *text, char *buffer, usize capacity);

/// FILETIME is 100 ns since 1601-01-01; DateTime ticks count from 0001-01-01.
constexpr int64 FileTimeEpochTicks = 504911232000000000;

inline int64 FileTimeToTicks(FILETIME time)
{
    return static_cast<int64>((static_cast<uint64>(time.dwHighDateTime) << 32) | time.dwLowDateTime) +
           FileTimeEpochTicks;
}

inline FILETIME TicksToFileTime(int64 ticks)
{
    const auto value = static_cast<uint64>(ticks - FileTimeEpochTicks);
    return {.dwLowDateTime = static_cast<DWORD>(value), .dwHighDateTime = static_cast<DWORD>(value >> 32)};
}

/// A UTF-8 path as a UTF-16 Win32 path; long absolute paths get the \\?\ prefix.
class CORE_API WidePath
{
public:
    /// *extended* adds the prefix whatever the length, for paths that will be extended.
    explicit WidePath(const char *path, bool extended = false);
    ~WidePath();
    MARLIN_NONCOPYABLE(WidePath);

    /// Null if the path is not valid UTF-8.
    const wchar_t *Get() const
    {
        return _text;
    }

private:
    static constexpr usize InlineCapacity = MAX_PATH;

    wchar_t _inlineBuffer[InlineCapacity] = {};
    wchar_t *_heap = nullptr;
    const wchar_t *_text = nullptr;
};

/// A UTF-8 object name as a session-local kernel object name.
class CORE_API WideObjectName
{
public:
    explicit WideObjectName(const char *name);
    ~WideObjectName() = default;
    MARLIN_NONCOPYABLE(WideObjectName);

    /// Null for a null or invalid name.
    const wchar_t *Get() const
    {
        return _valid ? _text : nullptr;
    }

private:
    wchar_t _text[MAX_PATH] = {};
    bool _valid = false;
};
} // namespace Marlin::Microsoft
