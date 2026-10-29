#include "HAL/MicrosoftHeaders.h"

namespace Marlin::Microsoft
{
namespace
{
constexpr wchar_t LongPathPrefix[] = L"\\\\?\\";
constexpr wchar_t LongUncPrefix[] = L"\\\\?\\UNC\\";
constexpr wchar_t ObjectNamePrefix[] = L"Local\\";

void ToBackslashes(wchar_t *text)
{
    for (; *text != L'\0'; ++text)
    {
        if (*text == L'/')
        {
            *text = L'\\';
        }
    }
}

usize WideLength(const wchar_t *text)
{
    usize length = 0;
    while (text[length] != L'\0')
    {
        ++length;
    }
    return length;
}
} // namespace

usize Utf8ToWide(const char *text, wchar_t *buffer, usize capacity)
{
    const int32 required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (required <= 0)
    {
        return 0;
    }
    if (static_cast<usize>(required) <= capacity)
    {
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, buffer, required);
    }
    return static_cast<usize>(required) - 1;
}

usize WideToUtf8(const wchar_t *text, char *buffer, usize capacity)
{
    const int32 required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        return 0;
    }
    if (static_cast<usize>(required) <= capacity)
    {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, required, nullptr, nullptr);
    }
    return static_cast<usize>(required) - 1;
}

usize WideToUtf8Path(const wchar_t *text, char *buffer, usize capacity)
{
    if (text[0] == L'\\' && text[1] == L'\\' && text[2] == L'?' && text[3] == L'\\')
    {
        text += 4;
    }
    const usize length = WideToUtf8(text, buffer, capacity);
    if (length < capacity)
    {
        for (usize i = 0; i < length; ++i)
        {
            if (buffer[i] == '\\')
            {
                buffer[i] = '/';
            }
        }
    }
    return length;
}

WidePath::WidePath(const char *path, bool extended)
{
    if (path == nullptr)
    {
        return;
    }
    const usize length = Utf8ToWide(path, _inlineBuffer, InlineCapacity);
    if (length == 0 && path[0] != '\0')
    {
        return;
    }
    wchar_t *text = _inlineBuffer;
    if (length >= InlineCapacity)
    {
        _heap = static_cast<wchar_t *>(HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(wchar_t)));
        if (_heap == nullptr)
        {
            return;
        }
        Utf8ToWide(path, _heap, length + 1);
        text = _heap;
    }
    ToBackslashes(text);
    _text = text;
    if (!extended && length < InlineCapacity - 12)
    {
        return;
    }

    if (text[0] == L'\\' && text[1] == L'\\' && text[2] == L'?' && text[3] == L'\\')
    {
        return;
    }
    const HANDLE heap = GetProcessHeap();
    const DWORD fullCapacity = GetFullPathNameW(text, 0, nullptr, nullptr);
    auto *full = static_cast<wchar_t *>(HeapAlloc(heap, 0, fullCapacity * sizeof(wchar_t)));
    if (fullCapacity == 0 || full == nullptr || GetFullPathNameW(text, fullCapacity, full, nullptr) == 0)
    {
        // No full path, no path: handing on the raw text would let an empty one reach "\*", the drive's root.
        HeapFree(heap, 0, full);
        _text = nullptr;
        return;
    }
    const bool isUnc = full[0] == L'\\' && full[1] == L'\\';
    const wchar_t *prefix = isUnc ? LongUncPrefix : LongPathPrefix;
    const wchar_t *rest = isUnc ? full + 2 : full;
    const usize prefixLength = WideLength(prefix);
    const usize restLength = WideLength(rest);
    auto *result = static_cast<wchar_t *>(HeapAlloc(heap, 0, (prefixLength + restLength + 1) * sizeof(wchar_t)));
    if (result == nullptr)
    {
        HeapFree(heap, 0, full);
        _text = nullptr;
        return;
    }
    for (usize i = 0; i < prefixLength; ++i)
    {
        result[i] = prefix[i];
    }
    for (usize i = 0; i <= restLength; ++i)
    {
        result[prefixLength + i] = rest[i];
    }
    HeapFree(heap, 0, full);
    if (_heap != nullptr)
    {
        HeapFree(heap, 0, _heap);
    }
    _heap = result;
    _text = result;
}

WidePath::~WidePath()
{
    if (_heap != nullptr)
    {
        HeapFree(GetProcessHeap(), 0, _heap);
    }
}

WideObjectName::WideObjectName(const char *name)
{
    if (name == nullptr)
    {
        return;
    }
    const usize prefixLength = WideLength(ObjectNamePrefix);
    for (usize i = 0; i < prefixLength; ++i)
    {
        _text[i] = ObjectNamePrefix[i];
    }
    const usize capacity = MAX_PATH - prefixLength;
    const usize length = Utf8ToWide(name, _text + prefixLength, capacity);
    _valid = length < capacity && (length > 0 || name[0] == '\0');
}
} // namespace Marlin::Microsoft
