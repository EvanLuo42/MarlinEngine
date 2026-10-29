#include "HAL/PlatformLibrary.h"

#include "HAL/MicrosoftHeaders.h"

namespace Marlin::PlatformLibrary
{
LibraryHandle Load(const char *path)
{
    const Microsoft::WidePath widePath(path);
    const wchar_t *text = widePath.Get();
    if (text == nullptr)
    {
        return {};
    }
    const bool isAbsolute = (text[0] != L'\0' && text[1] == L':') || (text[0] == L'\\' && text[1] == L'\\');
    const DWORD flags = isAbsolute ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS : 0;
    return {LoadLibraryExW(text, nullptr, flags)};
}

void Unload(LibraryHandle library)
{
    if (library.IsValid())
    {
        FreeLibrary(static_cast<HMODULE>(library.Value));
    }
}

void *FindSymbol(LibraryHandle library, const char *name)
{
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(library.Value), name));
}
} // namespace Marlin::PlatformLibrary
