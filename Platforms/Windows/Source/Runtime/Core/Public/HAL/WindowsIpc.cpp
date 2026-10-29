#include "HAL/PlatformIpc.h"

#include "HAL/MicrosoftHeaders.h"

namespace Marlin
{
namespace
{
constexpr DWORD PipeBufferSize = 64 * 1024;

HANDLE PipeFile(PipeHandle pipe)
{
    return reinterpret_cast<HANDLE>(pipe.Value);
}

HANDLE PipeEvent(PipeHandle pipe)
{
    return reinterpret_cast<HANDLE>(pipe.Event);
}

class PipeName
{
public:
    explicit PipeName(const char *name)
    {
        constexpr wchar_t Prefix[] = L"\\\\.\\pipe\\";
        constexpr usize PrefixLength = (sizeof(Prefix) / sizeof(wchar_t)) - 1;
        for (usize i = 0; i < PrefixLength; ++i)
        {
            _text[i] = Prefix[i];
        }
        const usize capacity = MaxLength - PrefixLength;
        const usize length = Microsoft::Utf8ToWide(name, _text + PrefixLength, capacity);
        _valid = length > 0 && length < capacity;
    }

    const wchar_t *Get() const
    {
        return _valid ? _text : nullptr;
    }

private:
    static constexpr usize MaxLength = 256;

    wchar_t _text[MaxLength] = {};
    bool _valid = false;
};

PipeHandle MakePipe(HANDLE file)
{
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr)
    {
        CloseHandle(file);
        return {};
    }
    return {.Value = reinterpret_cast<uintptr>(file), .Event = reinterpret_cast<uintptr>(event)};
}

bool Transfer(PipeHandle pipe, void *readBuffer, const void *writeData, DWORD size, DWORD &transferred)
{
    transferred = 0;
    if (pipe.Event == 0)
    {
        return writeData != nullptr ? WriteFile(PipeFile(pipe), writeData, size, &transferred, nullptr) != 0
                                    : ReadFile(PipeFile(pipe), readBuffer, size, &transferred, nullptr) != 0;
    }
    OVERLAPPED overlapped = {};
    overlapped.hEvent = PipeEvent(pipe);
    const BOOL done = writeData != nullptr ? WriteFile(PipeFile(pipe), writeData, size, nullptr, &overlapped)
                                           : ReadFile(PipeFile(pipe), readBuffer, size, nullptr, &overlapped);
    if (!done && GetLastError() != ERROR_IO_PENDING)
    {
        return false;
    }
    return GetOverlappedResult(PipeFile(pipe), &overlapped, &transferred, TRUE) != 0;
}
} // namespace

namespace PlatformPipe
{
bool Read(PipeHandle pipe, void *buffer, usize size, usize &bytesRead)
{
    constexpr usize MaxChunk = 1u << 30;
    DWORD transferred = 0;
    const bool succeeded =
        Transfer(pipe, buffer, nullptr, static_cast<DWORD>(size < MaxChunk ? size : MaxChunk), transferred);
    bytesRead = transferred;
    return succeeded && (transferred > 0 || size == 0);
}

bool Write(PipeHandle pipe, const void *data, usize size)
{
    const auto *source = static_cast<const uint8 *>(data);
    usize written = 0;
    while (written < size)
    {
        constexpr usize MaxChunk = 1u << 30;
        const usize remaining = size - written;
        DWORD transferred = 0;
        if (!Transfer(pipe, nullptr, source + written, static_cast<DWORD>(remaining < MaxChunk ? remaining : MaxChunk),
                      transferred))
        {
            return false;
        }
        written += transferred;
    }
    return true;
}

bool Peek(PipeHandle pipe, usize &bytesAvailable)
{
    DWORD available = 0;
    const bool open = PeekNamedPipe(PipeFile(pipe), nullptr, 0, nullptr, &available, nullptr) != 0;
    bytesAvailable = open ? available : 0;
    return open;
}

void Close(PipeHandle pipe)
{
    if (pipe.Value != 0)
    {
        CloseHandle(PipeFile(pipe));
    }
    if (pipe.Event != 0)
    {
        CloseHandle(PipeEvent(pipe));
    }
}

PipeHandle CreateServer(const char *name)
{
    const PipeName pipeName(name);
    if (pipeName.Get() == nullptr)
    {
        return {};
    }
    const HANDLE file = CreateNamedPipeW(pipeName.Get(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                         PIPE_UNLIMITED_INSTANCES, PipeBufferSize, PipeBufferSize, 0, nullptr);
    return file == INVALID_HANDLE_VALUE ? PipeHandle{} : MakePipe(file);
}

bool WaitForClient(PipeHandle server, uint32 timeout)
{
    OVERLAPPED overlapped = {};
    overlapped.hEvent = PipeEvent(server);
    if (ConnectNamedPipe(PipeFile(server), &overlapped))
    {
        return true;
    }
    switch (GetLastError())
    {
    case ERROR_PIPE_CONNECTED:
        return true;
    case ERROR_IO_PENDING:
        break;
    default:
        return false;
    }
    if (WaitForSingleObject(overlapped.hEvent, timeout) != WAIT_OBJECT_0)
    {
        CancelIoEx(PipeFile(server), &overlapped);
    }
    DWORD transferred = 0;
    return GetOverlappedResult(PipeFile(server), &overlapped, &transferred, TRUE) != 0;
}

void Disconnect(PipeHandle server)
{
    DisconnectNamedPipe(PipeFile(server));
}

PipeHandle Connect(const char *name, uint32 timeout)
{
    const PipeName pipeName(name);
    if (pipeName.Get() == nullptr)
    {
        return {};
    }
    const ULONGLONG deadline = GetTickCount64() + timeout;
    while (true)
    {
        const HANDLE file = CreateFileW(pipeName.Get(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_OVERLAPPED, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            return MakePipe(file);
        }
        const DWORD error = GetLastError();
        const ULONGLONG now = GetTickCount64();
        if ((error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) || now >= deadline)
        {
            return {};
        }
        const auto remaining = static_cast<DWORD>(deadline - now);
        if (error == ERROR_PIPE_BUSY)
        {
            WaitNamedPipeW(pipeName.Get(), remaining);
        }
        else
        {
            Sleep(remaining < 10 ? remaining : 10);
        }
    }
}
} // namespace PlatformPipe

namespace PlatformSharedMemory
{
namespace
{
bool MapRegion(HANDLE mapping, usize size, SharedMemory &memory)
{
    void *data = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (data == nullptr)
    {
        CloseHandle(mapping);
        return false;
    }
    memory = {.Data = data, .Size = size, .Handle = reinterpret_cast<uintptr>(mapping)};
    return true;
}
} // namespace

bool Create(const char *name, usize size, SharedMemory &memory)
{
    memory = {};
    const Microsoft::WideObjectName wideName(name);
    if (name != nullptr && wideName.Get() == nullptr)
    {
        return false;
    }
    const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                              static_cast<DWORD>(static_cast<uint64>(size) >> 32),
                                              static_cast<DWORD>(size), wideName.Get());
    return mapping != nullptr && MapRegion(mapping, size, memory);
}

bool Open(const char *name, usize size, SharedMemory &memory)
{
    memory = {};
    const Microsoft::WideObjectName wideName(name);
    if (wideName.Get() == nullptr)
    {
        return false;
    }
    const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wideName.Get());
    return mapping != nullptr && MapRegion(mapping, size, memory);
}

void Close(SharedMemory &memory)
{
    if (memory.Data != nullptr)
    {
        UnmapViewOfFile(memory.Data);
        CloseHandle(reinterpret_cast<HANDLE>(memory.Handle));
    }
    memory = {};
}
} // namespace PlatformSharedMemory
} // namespace Marlin
