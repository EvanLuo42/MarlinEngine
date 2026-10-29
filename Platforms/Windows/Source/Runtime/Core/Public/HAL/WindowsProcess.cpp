#include "HAL/PlatformProcess.h"

#include "HAL/MicrosoftHeaders.h"

namespace Marlin::PlatformProcess
{
namespace
{
HANDLE ToHandle(ProcessHandle process)
{
    return reinterpret_cast<HANDLE>(process.Value);
}

HANDLE KillOnExitJob()
{
    static const HANDLE job = [] {
        const HANDLE created = CreateJobObjectW(nullptr, nullptr);
        if (created != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }
        return created;
    }();
    return job;
}

void CloseIfOpen(HANDLE handle)
{
    if (handle != nullptr)
    {
        CloseHandle(handle);
    }
}

HANDLE InheritableStdHandle(DWORD which)
{
    const HANDLE handle = GetStdHandle(which);
    DWORD flags = 0;
    const bool inheritable = handle != nullptr && handle != INVALID_HANDLE_VALUE &&
                             GetHandleInformation(handle, &flags) && (flags & HANDLE_FLAG_INHERIT) != 0;
    return inheritable ? handle : nullptr;
}

bool CreatePipePair(HANDLE &parentEnd, HANDLE &childEnd, bool childReads)
{
    SECURITY_ATTRIBUTES security = {
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &security, 0))
    {
        return false;
    }
    parentEnd = childReads ? write : read;
    childEnd = childReads ? read : write;
    SetHandleInformation(parentEnd, HANDLE_FLAG_INHERIT, 0);
    return true;
}

wchar_t *BuildCommandLine(const char *executable, const char *arguments)
{
    const Microsoft::WidePath wideExecutable(executable);
    if (wideExecutable.Get() == nullptr)
    {
        return nullptr;
    }
    usize executableLength = 0;
    while (wideExecutable.Get()[executableLength] != L'\0')
    {
        ++executableLength;
    }
    const usize argumentsLength = arguments != nullptr ? Microsoft::Utf8ToWide(arguments, nullptr, 0) : 0;
    const usize capacity = executableLength + argumentsLength + 4;
    auto *commandLine = static_cast<wchar_t *>(HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(wchar_t)));
    if (commandLine == nullptr)
    {
        return nullptr;
    }
    usize length = 0;
    commandLine[length++] = L'"';
    for (usize i = 0; i < executableLength; ++i)
    {
        commandLine[length++] = wideExecutable.Get()[i];
    }
    commandLine[length++] = L'"';
    if (argumentsLength > 0)
    {
        commandLine[length++] = L' ';
        Microsoft::Utf8ToWide(arguments, commandLine + length, argumentsLength + 1);
        length += argumentsLength;
    }
    commandLine[length] = L'\0';
    return commandLine;
}
} // namespace

usize ExecutablePath(char *buffer, usize capacity)
{
    const HANDLE heap = GetProcessHeap();
    DWORD wideCapacity = MAX_PATH;
    while (true)
    {
        auto *wide = static_cast<wchar_t *>(HeapAlloc(heap, 0, wideCapacity * sizeof(wchar_t)));
        if (wide == nullptr)
        {
            return 0;
        }
        const DWORD length = GetModuleFileNameW(nullptr, wide, wideCapacity);
        if (length == 0 || length < wideCapacity)
        {
            const usize result = length == 0 ? 0 : Microsoft::WideToUtf8Path(wide, buffer, capacity);
            HeapFree(heap, 0, wide);
            return result;
        }
        HeapFree(heap, 0, wide);
        if (wideCapacity >= 32768)
        {
            return 0;
        }
        wideCapacity *= 2;
    }
}

usize CommandLine(char *buffer, usize capacity)
{
    return Microsoft::WideToUtf8(GetCommandLineW(), buffer, capacity);
}

void WriteOutput(const char *text, usize length)
{
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    while (length > 0 && output != nullptr && output != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(length < 0x40000000 ? length : 0x40000000);
        if (WriteFile(output, text, chunk, &written, nullptr) == 0 || written == 0)
        {
            return;
        }
        text += written;
        length -= written;
    }
}

uint32 CurrentId()
{
    return GetCurrentProcessId();
}

void Exit(int32 exitCode)
{
    ExitProcess(static_cast<UINT>(exitCode));
}

bool Create(const ProcessDesc &desc, ProcessHandle &process, PipeHandle *input, PipeHandle *output)
{
    process = {};
    const bool redirectInput = HasAnyFlags(desc.Flags, ProcessFlags::RedirectInput);
    const bool redirectOutput = HasAnyFlags(desc.Flags, ProcessFlags::RedirectOutput);
    const Microsoft::WidePath workingDirectory(desc.WorkingDirectory);
    if (desc.WorkingDirectory != nullptr && workingDirectory.Get() == nullptr)
    {
        return false;
    }
    wchar_t *commandLine = BuildCommandLine(desc.Executable, desc.Arguments);
    if (commandLine == nullptr)
    {
        return false;
    }

    HANDLE inputParent = nullptr;
    HANDLE inputChild = nullptr;
    HANDLE outputParent = nullptr;
    HANDLE outputChild = nullptr;
    bool succeeded = (!redirectInput || CreatePipePair(inputParent, inputChild, true)) &&
                     (!redirectOutput || CreatePipePair(outputParent, outputChild, false));

    HANDLE inherited[3] = {};
    DWORD inheritedCount = 0;
    const auto inherit = [&](HANDLE handle) {
        for (DWORD i = 0; i < inheritedCount; ++i)
        {
            if (inherited[i] == handle)
            {
                return handle;
            }
        }
        if (handle != nullptr)
        {
            inherited[inheritedCount++] = handle;
        }
        return handle;
    };
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
    if (succeeded && (redirectInput || redirectOutput))
    {
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = inherit(redirectInput ? inputChild : InheritableStdHandle(STD_INPUT_HANDLE));
        startup.StartupInfo.hStdOutput =
            inherit(redirectOutput ? outputChild : InheritableStdHandle(STD_OUTPUT_HANDLE));
        startup.StartupInfo.hStdError = inherit(redirectOutput ? outputChild : InheritableStdHandle(STD_ERROR_HANDLE));
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
        succeeded =
            attributes != nullptr && InitializeProcThreadAttributeList(attributes, 1, 0, &size) &&
            UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, static_cast<void *>(inherited),
                                      inheritedCount * sizeof(HANDLE), nullptr, nullptr);
        startup.lpAttributeList = attributes;
    }

    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
    if (HasAnyFlags(desc.Flags, ProcessFlags::Hidden))
    {
        flags |= CREATE_NO_WINDOW;
    }
    if (HasAnyFlags(desc.Flags, ProcessFlags::Detached))
    {
        flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
    }
    PROCESS_INFORMATION info = {};
    if (succeeded)
    {
        succeeded = CreateProcessW(nullptr, commandLine, nullptr, nullptr, inheritedCount > 0 ? TRUE : FALSE, flags,
                                   nullptr, workingDirectory.Get(), &startup.StartupInfo, &info) != 0;
        if (!succeeded && (flags & CREATE_BREAKAWAY_FROM_JOB) != 0 && GetLastError() == ERROR_ACCESS_DENIED)
        {
            flags &= ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB);
            succeeded = CreateProcessW(nullptr, commandLine, nullptr, nullptr, inheritedCount > 0 ? TRUE : FALSE, flags,
                                       nullptr, workingDirectory.Get(), &startup.StartupInfo, &info) != 0;
        }
    }
    if (succeeded)
    {
        if (HasAnyFlags(desc.Flags, ProcessFlags::KillOnParentExit) && KillOnExitJob() != nullptr)
        {
            AssignProcessToJobObject(KillOnExitJob(), info.hProcess);
        }
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        process = {.Value = reinterpret_cast<uintptr>(info.hProcess), .Id = info.dwProcessId};
    }

    if (attributes != nullptr)
    {
        DeleteProcThreadAttributeList(attributes);
        HeapFree(GetProcessHeap(), 0, attributes);
    }
    HeapFree(GetProcessHeap(), 0, commandLine);
    CloseIfOpen(inputChild);
    CloseIfOpen(outputChild);
    if (!succeeded)
    {
        CloseIfOpen(inputParent);
        CloseIfOpen(outputParent);
        return false;
    }
    if (input != nullptr)
    {
        *input = redirectInput ? PipeHandle{.Value = reinterpret_cast<uintptr>(inputParent)} : PipeHandle{};
    }
    else if (inputParent != nullptr)
    {
        CloseHandle(inputParent);
    }
    if (output != nullptr)
    {
        *output = redirectOutput ? PipeHandle{.Value = reinterpret_cast<uintptr>(outputParent)} : PipeHandle{};
    }
    else if (outputParent != nullptr)
    {
        CloseHandle(outputParent);
    }
    return true;
}

bool Open(uint32 processId, ProcessHandle &process)
{
    const HANDLE handle =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, processId);
    if (handle == nullptr)
    {
        return false;
    }
    process = ProcessHandle{.Value = reinterpret_cast<uintptr>(handle), .Id = processId};
    return true;
}

bool Wait(ProcessHandle process, uint32 timeout)
{
    return WaitForSingleObject(ToHandle(process), timeout) == WAIT_OBJECT_0;
}

bool IsRunning(ProcessHandle process)
{
    return WaitForSingleObject(ToHandle(process), 0) == WAIT_TIMEOUT;
}

bool IsRunning(uint32 processId)
{
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr)
    {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}

bool GetExitCode(ProcessHandle process, int32 &exitCode)
{
    DWORD code = 0;
    if (IsRunning(process) || !GetExitCodeProcess(ToHandle(process), &code))
    {
        return false;
    }
    exitCode = static_cast<int32>(code);
    return true;
}

void Terminate(ProcessHandle process, int32 exitCode)
{
    TerminateProcess(ToHandle(process), static_cast<UINT>(exitCode));
}

void Close(ProcessHandle process)
{
    if (process.IsValid())
    {
        CloseHandle(ToHandle(process));
    }
}
} // namespace Marlin::PlatformProcess
