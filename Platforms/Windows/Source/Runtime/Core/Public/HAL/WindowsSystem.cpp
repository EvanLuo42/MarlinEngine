#include "HAL/PlatformSystem.h"

#include "Diagnostics/Assert.h"
#include "HAL/MicrosoftHeaders.h"
#include "Memory/MemoryOps.h"

#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>

namespace Marlin::PlatformSystem
{
namespace
{
usize AppendText(char *text, usize length, usize capacity, const char *suffix)
{
    for (; *suffix != '\0' && length + 1 < capacity; ++suffix)
    {
        text[length++] = *suffix;
    }
    text[length] = '\0';
    return length;
}

usize AppendNumber(char *text, usize length, usize capacity, uint32 value)
{
    char digits[10];
    usize count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0 && length + 1 < capacity)
    {
        text[length++] = digits[--count];
    }
    text[length] = '\0';
    return length;
}

usize CopyResult(const char *text, usize length, char *buffer, usize capacity)
{
    if (length < capacity)
    {
        MemCopy(buffer, text, length + 1);
    }
    return length;
}
} // namespace

usize GetKnownFolder(KnownFolder folder, char *buffer, usize capacity)
{
    if (folder == KnownFolder::Temp)
    {
        wchar_t temp[MAX_PATH + 1];
        const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
        if (length == 0 || length > MAX_PATH)
        {
            return 0;
        }
        if (length > 3 && temp[length - 1] == L'\\')
        {
            temp[length - 1] = L'\0';
        }
        return Microsoft::WideToUtf8Path(temp, buffer, capacity);
    }

    const KNOWNFOLDERID *id = nullptr;
    switch (folder)
    {
    case KnownFolder::Home:
        id = &FOLDERID_Profile;
        break;
    case KnownFolder::LocalAppData:
        id = &FOLDERID_LocalAppData;
        break;
    case KnownFolder::RoamingAppData:
        id = &FOLDERID_RoamingAppData;
        break;
    case KnownFolder::Documents:
        id = &FOLDERID_Documents;
        break;
    case KnownFolder::SavedGames:
        id = &FOLDERID_SavedGames;
        break;
    case KnownFolder::Desktop:
        id = &FOLDERID_Desktop;
        break;
    case KnownFolder::Downloads:
        id = &FOLDERID_Downloads;
        break;
    case KnownFolder::Pictures:
        id = &FOLDERID_Pictures;
        break;
    case KnownFolder::Music:
        id = &FOLDERID_Music;
        break;
    case KnownFolder::Videos:
        id = &FOLDERID_Videos;
        break;
    case KnownFolder::Temp:
        return 0;
    }
    wchar_t *path = nullptr;
    if (FAILED(SHGetKnownFolderPath(*id, KF_FLAG_CREATE, nullptr, &path)))
    {
        CoTaskMemFree(path);
        return 0;
    }
    const usize length = Microsoft::WideToUtf8Path(path, buffer, capacity);
    CoTaskMemFree(path);
    return length;
}

usize GetEnvironmentVariable(const char *name, char *buffer, usize capacity)
{
    wchar_t wideName[256];
    if (Microsoft::Utf8ToWide(name, wideName, 256) >= 256)
    {
        return 0;
    }
    const DWORD wideCapacity = GetEnvironmentVariableW(wideName, nullptr, 0);
    if (wideCapacity == 0)
    {
        return 0;
    }
    auto *value = static_cast<wchar_t *>(HeapAlloc(GetProcessHeap(), 0, wideCapacity * sizeof(wchar_t)));
    if (value == nullptr)
    {
        return 0;
    }
    const DWORD length = GetEnvironmentVariableW(wideName, value, wideCapacity);
    const usize result = length > 0 && length < wideCapacity ? Microsoft::WideToUtf8(value, buffer, capacity) : 0;
    HeapFree(GetProcessHeap(), 0, value);
    return result;
}

bool SetEnvironmentVariable(const char *name, const char *value)
{
    wchar_t wideName[256];
    if (Microsoft::Utf8ToWide(name, wideName, 256) >= 256)
    {
        return false;
    }
    if (value == nullptr)
    {
        return SetEnvironmentVariableW(wideName, nullptr) != 0;
    }
    const usize length = Microsoft::Utf8ToWide(value, nullptr, 0);
    auto *wideValue = static_cast<wchar_t *>(HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(wchar_t)));
    if (wideValue == nullptr)
    {
        return false;
    }
    Microsoft::Utf8ToWide(value, wideValue, length + 1);
    const bool succeeded = SetEnvironmentVariableW(wideName, wideValue) != 0;
    HeapFree(GetProcessHeap(), 0, wideValue);
    return succeeded;
}

usize GetOsVersion(char *buffer, usize capacity)
{
    using RtlGetVersionFunction = LONG(WINAPI *)(OSVERSIONINFOW *);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtlGetVersion =
        ntdll != nullptr ? reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtlGetVersion == nullptr || rtlGetVersion(&version) != 0)
    {
        return 0;
    }
    char text[64];
    usize length = AppendText(text, 0, sizeof(text), "Windows ");
    length = AppendNumber(text, length, sizeof(text), version.dwMajorVersion);
    length = AppendText(text, length, sizeof(text), ".");
    length = AppendNumber(text, length, sizeof(text), version.dwMinorVersion);
    length = AppendText(text, length, sizeof(text), ".");
    length = AppendNumber(text, length, sizeof(text), version.dwBuildNumber);
    return CopyResult(text, length, buffer, capacity);
}

usize GetComputerName(char *buffer, usize capacity)
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(name, &size) ? Microsoft::WideToUtf8(name, buffer, capacity) : 0;
}

usize GetUserName(char *buffer, usize capacity)
{
    wchar_t name[257];
    DWORD size = 257;
    return GetUserNameW(name, &size) ? Microsoft::WideToUtf8(name, buffer, capacity) : 0;
}

usize GetUserLanguage(char *buffer, usize capacity)
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    return GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0 ? Microsoft::WideToUtf8(name, buffer, capacity)
                                                                      : 0;
}

void GetSecureRandomBytes(void *buffer, usize size)
{
    auto *bytes = static_cast<UCHAR *>(buffer);
    while (size != 0)
    {
        const ULONG chunk = size < 0x40000000 ? static_cast<ULONG>(size) : 0x40000000;
        const LONG status = BCryptGenRandom(nullptr, bytes, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        MARLIN_VERIFY(status >= 0);
        bytes += chunk;
        size -= chunk;
    }
}
} // namespace Marlin::PlatformSystem
