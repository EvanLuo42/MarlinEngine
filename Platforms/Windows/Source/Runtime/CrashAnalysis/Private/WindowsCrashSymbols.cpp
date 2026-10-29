#include "CrashSymbols.h"

#include "String/StringBuilder.h"

#include "HAL/MicrosoftHeaders.h"

#include <dbghelp.h>

namespace Marlin::Private
{
namespace
{
/// dbghelp keys a session by a process handle; a dump has no process, so any unique value does.
const HANDLE SessionHandle = reinterpret_cast<HANDLE>(static_cast<uintptr>(0x4D43524Bu));
bool _loaded = false;

String Utf8(const wchar_t *text)
{
    char buffer[1024];
    const usize length = Microsoft::WideToUtf8Path(text, buffer, sizeof(buffer));
    return length < sizeof(buffer) ? String(buffer, length) : String();
}

StringView DirectoryOf(StringView path)
{
    const usize slash = path.LastIndexOf('/');
    return slash != StringView::NotFound ? path.Left(slash) : StringView();
}
} // namespace

void LoadSymbols(const Minidump &dump, const CrashAnalysisDesc &desc, Span<const String> images,
                 CrashAnalysisResult &result)
{
    const Span<const MinidumpModule> modules = dump.GetModules();
    StringBuilder<4096> searchPath;
    for (usize i = 0; i < modules.Count(); ++i)
    {
        const String debugFile =
            desc.SymbolStore.IsEmpty() ? String() : CrashAnalyzer::GetDebugFilePath(desc.SymbolStore, modules[i]);
        for (const StringView path : {debugFile.AsView(), i < images.Count() ? images[i].AsView() : StringView()})
        {
            const StringView directory = DirectoryOf(path);
            if (!directory.IsEmpty() && !searchPath.AsView().Contains(directory))
            {
                searchPath.Append(searchPath.IsEmpty() ? "" : ";").Append(directory);
            }
        }
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    const Microsoft::WidePath wideSearchPath(searchPath.CStr());
    _loaded = SymInitializeW(SessionHandle, wideSearchPath.Get(), FALSE) != 0;
    if (!_loaded)
    {
        return;
    }
    for (usize i = 0; i < modules.Count() && i < result.Modules.Count(); ++i)
    {
        const MinidumpModule &module = modules[i];
        const String &image = i < images.Count() && !images[i].IsEmpty() ? images[i] : module.Path;
        const Microsoft::WidePath wideImage(image.CStr());
        if (SymLoadModuleExW(SessionHandle, nullptr, wideImage.Get(), nullptr, module.Base, module.Size, nullptr, 0) ==
            0)
        {
            continue;
        }
        IMAGEHLP_MODULEW64 info = {};
        info.SizeOfStruct = sizeof(info);
        result.Modules[i].HasSymbols = SymGetModuleInfoW64(SessionHandle, module.Base, &info) &&
                                       (info.SymType == SymPdb || info.SymType == SymDia || info.SymType == SymCv);
    }
}

void Symbolize(uint64 address, CrashFrame &frame)
{
    if (!_loaded)
    {
        return;
    }
    constexpr usize MaxNameLength = 512;
    alignas(SYMBOL_INFOW) uint8 storage[sizeof(SYMBOL_INFOW) + MaxNameLength * sizeof(wchar_t)] = {};
    auto *symbol = reinterpret_cast<SYMBOL_INFOW *>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = MaxNameLength;
    DWORD64 displacement = 0;
    if (SymFromAddrW(SessionHandle, address, &displacement, symbol))
    {
        frame.Function = Utf8(symbol->Name);
        frame.Offset = displacement;
    }
    IMAGEHLP_LINEW64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddrW64(SessionHandle, address, &lineDisplacement, &line))
    {
        frame.File = Utf8(line.FileName);
        frame.Line = line.LineNumber;
    }
}

void UnloadSymbols()
{
    if (_loaded)
    {
        SymCleanup(SessionHandle);
        _loaded = false;
    }
}
} // namespace Marlin::Private
