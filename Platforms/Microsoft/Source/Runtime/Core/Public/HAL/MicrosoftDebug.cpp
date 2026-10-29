#include "HAL/PlatformDebug.h"

#include "HAL/MicrosoftHeaders.h"

namespace Marlin::PlatformDebug
{
namespace
{
bool IsContinuationByte(char c)
{
    const auto byte = static_cast<uint8>(c);
    return byte >= 0x80 && byte < 0xC0;
}
} // namespace

void Output(const char *text)
{
    const usize length = __builtin_strlen(text);
    const HANDLE standardError = GetStdHandle(STD_ERROR_HANDLE);
    if (standardError != nullptr && standardError != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(standardError, text, static_cast<DWORD>(length), &written, nullptr);
    }
    if (!IsDebuggerPresent())
    {
        return;
    }
    constexpr usize ChunkBytes = 510;
    wchar_t wide[ChunkBytes + 1];
    usize offset = 0;
    while (offset < length)
    {
        const usize limit = offset + ChunkBytes < length ? offset + ChunkBytes : length;
        usize end = limit;
        while (end < length && end > offset && IsContinuationByte(text[end]))
        {
            --end;
        }
        if (end == offset)
        {
            end = limit;
        }
        const int32 count = MultiByteToWideChar(CP_UTF8, 0, text + offset, static_cast<int32>(end - offset), wide,
                                                static_cast<int32>(ChunkBytes));
        wide[count > 0 ? count : 0] = L'\0';
        OutputDebugStringW(wide);
        offset = end;
    }
}

bool IsDebuggerAttached()
{
    return IsDebuggerPresent() != 0;
}
} // namespace Marlin::PlatformDebug