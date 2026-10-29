#include "HAL/PlatformCrash.h"
#include "HAL/PlatformStackTrace.h"
#include "HAL/PlatformSync.h"

#include "HAL/MicrosoftHeaders.h"

#include <dbghelp.h>
#include <signal.h>
#include <stdlib.h>

namespace Marlin
{
namespace
{
CrashHandler _crashHandler = nullptr;
Mutex _symbolMutex;
bool _symbolsInitialized = false;

LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS *exception)
{
    const CrashHandler handler = _crashHandler;
    if (handler != nullptr)
    {
        const CrashInfo info = {
            .ExceptionCode = exception->ExceptionRecord->ExceptionCode,
            .Address = exception->ExceptionRecord->ExceptionAddress,
            .ThreadId = GetCurrentThreadId(),
            .PlatformContext = exception,
            .Failure = PlatformCrash::GetNotedFailure(),
        };
        handler(info);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void RaiseFailure(const char *kind)
{
    if (PlatformCrash::GetNotedFailure() == nullptr)
    {
        PlatformCrash::NoteFailure(kind, kind, nullptr, 0);
    }
    RaiseException(PlatformCrash::FailureExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    MARLIN_TRAP();
}

void __cdecl OnPureCall()
{
    RaiseFailure("Pure virtual call");
}

void __cdecl OnInvalidParameter(const wchar_t * /*expression*/, const wchar_t * /*function*/, const wchar_t * /*file*/,
                                unsigned int /*line*/, uintptr_t /*reserved*/)
{
    RaiseFailure("Invalid runtime library parameter");
}

void __cdecl OnAbort(int /*signal*/)
{
    RaiseFailure("Abort");
}

struct SnapshotJob
{
    HANDLE Process;
    DWORD ProcessId;
    HANDLE File;
    MINIDUMP_TYPE Type;
    bool Succeeded;
};

DWORD WINAPI WriteSnapshot(LPVOID parameter)
{
    auto *job = static_cast<SnapshotJob *>(parameter);
    job->Succeeded =
        MiniDumpWriteDump(job->Process, job->ProcessId, job->File, job->Type, nullptr, nullptr, nullptr) != 0;
    return 0;
}

bool EnsureSymbols()
{
    if (!_symbolsInitialized)
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
        _symbolsInitialized = SymInitializeW(GetCurrentProcess(), nullptr, TRUE) != 0;
    }
    return _symbolsInitialized;
}

void CopyName(const wchar_t *source, char *target, usize capacity)
{
    if (Microsoft::WideToUtf8(source, target, capacity) >= capacity)
    {
        target[0] = '\0';
    }
}
} // namespace

namespace PlatformStackTrace
{
MARLIN_NOINLINE uint32 Capture(void **frames, uint32 maxCount, uint32 skip)
{
    return RtlCaptureStackBackTrace(skip + 1, maxCount, frames, nullptr);
}

bool Symbolize(const void *address, StackFrameSymbol &symbol)
{
    symbol = {};
    const ScopeLock<Mutex> lock(_symbolMutex);
    if (!EnsureSymbols())
    {
        return false;
    }
    const HANDLE process = GetCurrentProcess();
    const auto location = reinterpret_cast<DWORD64>(address);
    if (SymGetModuleBase64(process, location) == 0)
    {
        SymRefreshModuleList(process);
    }

    constexpr usize MaxNameLength = 256;
    alignas(SYMBOL_INFOW) uint8 storage[sizeof(SYMBOL_INFOW) + MaxNameLength * sizeof(wchar_t)] = {};
    auto *info = reinterpret_cast<SYMBOL_INFOW *>(storage);
    info->SizeOfStruct = sizeof(SYMBOL_INFOW);
    info->MaxNameLen = MaxNameLength;
    DWORD64 displacement = 0;
    const bool found = SymFromAddrW(process, location, &displacement, info) != 0;
    if (found)
    {
        CopyName(info->Name, symbol.Function, sizeof(symbol.Function));
    }

    IMAGEHLP_LINEW64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddrW64(process, location, &lineDisplacement, &line))
    {
        CopyName(line.FileName, symbol.File, sizeof(symbol.File));
        symbol.Line = line.LineNumber;
    }

    IMAGEHLP_MODULEW64 module = {};
    module.SizeOfStruct = sizeof(module);
    if (SymGetModuleInfoW64(process, location, &module))
    {
        CopyName(module.ModuleName, symbol.Module, sizeof(symbol.Module));
    }
    return found;
}
} // namespace PlatformStackTrace

namespace PlatformCrash
{
void SetHandler(CrashHandler handler)
{
    _crashHandler = handler;
    SetUnhandledExceptionFilter(handler != nullptr ? UnhandledExceptionHandler : nullptr);
    if (handler != nullptr)
    {
        _set_purecall_handler(OnPureCall);
        _set_invalid_parameter_handler(OnInvalidParameter);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        signal(SIGABRT, OnAbort);
    }
    else
    {
        _set_purecall_handler(nullptr);
        _set_invalid_parameter_handler(nullptr);
        signal(SIGABRT, SIG_DFL);
    }
}

void RestoreHandler()
{
    if (_crashHandler != nullptr)
    {
        const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(UnhandledExceptionHandler);
        if (previous != UnhandledExceptionHandler)
        {
            SetHandler(_crashHandler);
        }
    }
}

bool WriteMinidump(const char *path, uint32 processId, const CrashInfo *info)
{
    const Microsoft::WidePath widePath(path);
    if (widePath.Get() == nullptr)
    {
        return false;
    }
    const bool isSelf = processId == GetCurrentProcessId();
    const HANDLE process =
        isSelf ? GetCurrentProcess() : OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (process == nullptr)
    {
        return false;
    }
    const HANDLE file =
        CreateFileW(widePath.Get(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        if (!isSelf)
        {
            CloseHandle(process);
        }
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception = {};
    if (info != nullptr)
    {
        exception.ThreadId = info->ThreadId;
        exception.ExceptionPointers = static_cast<EXCEPTION_POINTERS *>(info->PlatformContext);
        exception.ClientPointers = isSelf ? FALSE : TRUE;
    }
    const auto type = static_cast<MINIDUMP_TYPE>(
        static_cast<uint32>(MiniDumpWithDataSegs) | static_cast<uint32>(MiniDumpWithIndirectlyReferencedMemory) |
        static_cast<uint32>(MiniDumpWithThreadInfo) | static_cast<uint32>(MiniDumpWithUnloadedModules) |
        static_cast<uint32>(MiniDumpWithHandleData));
    const bool locked = isSelf ? _symbolMutex.TryLock() : (_symbolMutex.Lock(), true);
    bool succeeded = false;
    if (isSelf && info == nullptr)
    {
        // A snapshot of this process: MiniDumpWriteDump cannot capture the stack of the thread calling it, so a
        // helper thread writes the dump while this one waits.
        SnapshotJob job = {process, processId, file, type, false};
        const HANDLE thread = CreateThread(nullptr, 0, WriteSnapshot, &job, 0, nullptr);
        if (thread != nullptr)
        {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
            succeeded = job.Succeeded;
        }
    }
    else
    {
        succeeded = MiniDumpWriteDump(process, processId, file, type, info != nullptr ? &exception : nullptr, nullptr,
                                      nullptr) != 0;
    }
    if (locked)
    {
        _symbolMutex.Unlock();
    }
    CloseHandle(file);
    if (!isSelf)
    {
        CloseHandle(process);
    }
    return succeeded;
}
} // namespace PlatformCrash
} // namespace Marlin
