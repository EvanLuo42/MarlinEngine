#include "HAL/PlatformIoPort.h"

#include "HAL/MicrosoftHeaders.h"
#include "Memory/MemoryOps.h"

#include <mswsock.h>

namespace Marlin
{
static_assert(sizeof(OVERLAPPED) <= sizeof(IoOperation::Platform));
static_assert(IoAcceptBufferSize >= 2 * (sizeof(sockaddr_in6) + 16));

namespace
{
struct SocketExtensions
{
    LPFN_ACCEPTEX AcceptEx = nullptr;
    LPFN_GETACCEPTEXSOCKADDRS GetAcceptExSockaddrs = nullptr;
    LPFN_CONNECTEX ConnectEx = nullptr;
    LPFN_TRANSMITFILE TransmitFile = nullptr;
};

template <typename TFunction> void LoadExtension(SOCKET socket, GUID guid, TFunction &function)
{
    DWORD bytes = 0;
    WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &function, sizeof(function), &bytes,
             nullptr, nullptr);
}

const SocketExtensions &GetExtensions()
{
    static const SocketExtensions extensions = [] {
        SocketExtensions result;
        const SOCKET socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (socket == INVALID_SOCKET)
        {
            return result;
        }
        LoadExtension(socket, WSAID_ACCEPTEX, result.AcceptEx);
        LoadExtension(socket, WSAID_GETACCEPTEXSOCKADDRS, result.GetAcceptExSockaddrs);
        LoadExtension(socket, WSAID_CONNECTEX, result.ConnectEx);
        LoadExtension(socket, WSAID_TRANSMITFILE, result.TransmitFile);
        closesocket(socket);
        return result;
    }();
    return extensions;
}

HANDLE ToPort(uintptr handle)
{
    return reinterpret_cast<HANDLE>(handle);
}

SOCKET ToSocket(SocketHandle socket)
{
    return static_cast<SOCKET>(socket.Value);
}

HANDLE ToFile(PipeHandle pipe)
{
    return reinterpret_cast<HANDLE>(pipe.Value);
}

OVERLAPPED *Begin(IoOperation &operation, IoOperationKind kind)
{
    MemZero(operation.Platform, sizeof(operation.Platform));
    operation.Kind = kind;
    operation.Bytes = 0;
    operation.Succeeded = false;
    return reinterpret_cast<OVERLAPPED *>(operation.Platform);
}

bool Started(int result)
{
    return result == 0 || WSAGetLastError() == WSA_IO_PENDING;
}

bool StartedFile(BOOL result)
{
    return result != 0 || GetLastError() == ERROR_IO_PENDING;
}

int Chunk(usize size)
{
    constexpr usize MaxChunk = 1u << 30;
    return static_cast<int>(size < MaxChunk ? size : MaxChunk);
}
} // namespace

IoPort::~IoPort()
{
    Shutdown();
}

bool IoPort::Initialize()
{
    (void)GetExtensions();
    const HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    _handle = reinterpret_cast<uintptr>(port);
    return port != nullptr;
}

void IoPort::Shutdown()
{
    if (_handle != 0)
    {
        CloseHandle(ToPort(_handle));
        _handle = 0;
    }
}

bool IoPort::Associate(SocketHandle socket)
{
    return CreateIoCompletionPort(reinterpret_cast<HANDLE>(ToSocket(socket)), ToPort(_handle), 0, 0) != nullptr;
}

bool IoPort::Associate(PipeHandle pipe)
{
    return CreateIoCompletionPort(ToFile(pipe), ToPort(_handle), 0, 0) != nullptr;
}

bool IoPort::Associate(FileHandle file)
{
    return CreateIoCompletionPort(reinterpret_cast<HANDLE>(file.Value), ToPort(_handle), 0, 0) != nullptr;
}

bool IoPort::Receive(SocketHandle socket, void *buffer, usize size, IoOperation &operation)
{
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::Receive);
    WSABUF part{static_cast<ULONG>(Chunk(size)), static_cast<CHAR *>(buffer)};
    DWORD flags = 0;
    return Started(WSARecv(ToSocket(socket), &part, 1, nullptr, &flags, overlapped, nullptr));
}

bool IoPort::Send(SocketHandle socket, const void *const *buffers, const usize *sizes, uint32 count,
                  IoOperation &operation)
{
    constexpr uint32 MaxParts = 64;
    if (count == 0 || count > MaxParts)
    {
        return false;
    }
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::Send);
    WSABUF parts[MaxParts];
    for (uint32 i = 0; i < count; ++i)
    {
        parts[i].len = static_cast<ULONG>(Chunk(sizes[i]));
        parts[i].buf = static_cast<CHAR *>(const_cast<void *>(buffers[i]));
    }
    return Started(WSASend(ToSocket(socket), parts, count, nullptr, 0, overlapped, nullptr));
}

bool IoPort::ReceiveFrom(SocketHandle socket, void *buffer, usize size, IoOperation &operation)
{
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::ReceiveFrom);
    WSABUF part{static_cast<ULONG>(Chunk(size)), static_cast<CHAR *>(buffer)};
    DWORD flags = 0;
    operation.Address.Length = sizeof(operation.Address.Storage);
    return Started(WSARecvFrom(ToSocket(socket), &part, 1, nullptr, &flags,
                               reinterpret_cast<sockaddr *>(operation.Address.Storage),
                               reinterpret_cast<INT *>(&operation.Address.Length), overlapped, nullptr));
}

bool IoPort::SendTo(SocketHandle socket, const void *data, usize size, const SocketAddress &address,
                    IoOperation &operation)
{
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::SendTo);
    WSABUF part{static_cast<ULONG>(Chunk(size)), static_cast<CHAR *>(const_cast<void *>(data))};
    return Started(WSASendTo(ToSocket(socket), &part, 1, nullptr, 0,
                             reinterpret_cast<const sockaddr *>(address.Storage), static_cast<int>(address.Length),
                             overlapped, nullptr));
}

bool IoPort::Accept(SocketHandle listener, SocketHandle accepted, void *addresses, IoOperation &operation)
{
    const SocketExtensions &extensions = GetExtensions();
    if (extensions.AcceptEx == nullptr)
    {
        return false;
    }
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::Accept);
    constexpr DWORD AddressSize = sizeof(sockaddr_in6) + 16;
    DWORD bytes = 0;
    return extensions.AcceptEx(ToSocket(listener), ToSocket(accepted), addresses, 0, AddressSize, AddressSize, &bytes,
                               overlapped) != 0 ||
           WSAGetLastError() == ERROR_IO_PENDING;
}

bool IoPort::FinishAccept(SocketHandle listener, SocketHandle accepted, const void *addresses, SocketAddress &peer)
{
    const SOCKET listenSocket = ToSocket(listener);
    if (setsockopt(ToSocket(accepted), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                   reinterpret_cast<const char *>(&listenSocket), sizeof(listenSocket)) != 0)
    {
        return false;
    }
    const SocketExtensions &extensions = GetExtensions();
    constexpr DWORD AddressSize = sizeof(sockaddr_in6) + 16;
    sockaddr *local = nullptr;
    sockaddr *remote = nullptr;
    INT localLength = 0;
    INT remoteLength = 0;
    extensions.GetAcceptExSockaddrs(const_cast<void *>(addresses), 0, AddressSize, AddressSize, &local, &localLength,
                                    &remote, &remoteLength);
    peer = {};
    if (remote != nullptr && remoteLength > 0 && static_cast<usize>(remoteLength) <= sizeof(peer.Storage))
    {
        MemCopy(peer.Storage, remote, static_cast<usize>(remoteLength));
        peer.Length = static_cast<uint32>(remoteLength);
    }
    return true;
}

bool IoPort::Connect(SocketHandle socket, const SocketAddress &address, IoOperation &operation)
{
    const SocketExtensions &extensions = GetExtensions();
    if (extensions.ConnectEx == nullptr)
    {
        return false;
    }
    const auto family = reinterpret_cast<const sockaddr *>(address.Storage)->sa_family;
    const SocketAddress local = PlatformSocket::AnyAddress(0, family == AF_INET6);
    if (bind(ToSocket(socket), reinterpret_cast<const sockaddr *>(local.Storage), static_cast<int>(local.Length)) != 0)
    {
        return false;
    }
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::Connect);
    return extensions.ConnectEx(ToSocket(socket), reinterpret_cast<const sockaddr *>(address.Storage),
                                static_cast<int>(address.Length), nullptr, 0, nullptr, overlapped) != 0 ||
           WSAGetLastError() == ERROR_IO_PENDING;
}

bool IoPort::FinishConnect(SocketHandle socket)
{
    return setsockopt(ToSocket(socket), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == 0;
}

bool IoPort::Receive(PipeHandle pipe, void *buffer, usize size, IoOperation &operation)
{
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::Receive);
    return StartedFile(ReadFile(ToFile(pipe), buffer, static_cast<DWORD>(Chunk(size)), nullptr, overlapped));
}

bool IoPort::Send(PipeHandle pipe, const void *data, usize size, IoOperation &operation)
{
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::Send);
    return StartedFile(WriteFile(ToFile(pipe), data, static_cast<DWORD>(Chunk(size)), nullptr, overlapped));
}

bool IoPort::AcceptPipe(PipeHandle pipe, IoOperation &operation)
{
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::AcceptPipe);
    if (ConnectNamedPipe(ToFile(pipe), overlapped) != 0)
    {
        return true;
    }
    switch (GetLastError())
    {
    case ERROR_IO_PENDING:
        return true;
    case ERROR_PIPE_CONNECTED:
        return PostQueuedCompletionStatus(ToPort(_handle), 0, 0, overlapped) != 0;
    default:
        return false;
    }
}

bool IoPort::Read(FileHandle file, uint64 offset, void *buffer, usize size, IoOperation &operation)
{
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::FileRead);
    overlapped->Offset = static_cast<DWORD>(offset);
    overlapped->OffsetHigh = static_cast<DWORD>(offset >> 32);
    return StartedFile(
        ReadFile(reinterpret_cast<HANDLE>(file.Value), buffer, static_cast<DWORD>(Chunk(size)), nullptr, overlapped));
}

bool IoPort::SendFile(SocketHandle socket, FileHandle file, uint64 offset, uint64 size, const void *head,
                      usize headSize, IoOperation &operation)
{
    const SocketExtensions &extensions = GetExtensions();
    if (extensions.TransmitFile == nullptr)
    {
        return false;
    }
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::FileSend);
    overlapped->Offset = static_cast<DWORD>(offset);
    overlapped->OffsetHigh = static_cast<DWORD>(offset >> 32);
    TRANSMIT_FILE_BUFFERS buffers{};
    buffers.Head = const_cast<void *>(head);
    buffers.HeadLength = static_cast<DWORD>(Chunk(headSize));
    // Zero bytes would send the rest of the file, so an empty region sends no file at all.
    const DWORD bytes = static_cast<DWORD>(Chunk(static_cast<usize>(size)));
    const HANDLE handle = bytes != 0 ? reinterpret_cast<HANDLE>(file.Value) : nullptr;
    return extensions.TransmitFile(ToSocket(socket), handle, bytes, 0, overlapped, headSize != 0 ? &buffers : nullptr,
                                   0) != 0 ||
           WSAGetLastError() == WSA_IO_PENDING;
}

void IoPort::Cancel(SocketHandle socket)
{
    CancelIoEx(reinterpret_cast<HANDLE>(ToSocket(socket)), nullptr);
}

void IoPort::Cancel(PipeHandle pipe)
{
    CancelIoEx(ToFile(pipe), nullptr);
}

bool IoPort::Post(IoOperation &operation)
{
    const uint32 bytes = operation.Bytes;
    OVERLAPPED *overlapped = Begin(operation, IoOperationKind::Posted);
    return PostQueuedCompletionStatus(ToPort(_handle), bytes, 0, overlapped) != 0;
}

uint32 IoPort::Wait(IoOperation **operations, uint32 maxCount, uint32 timeout)
{
    constexpr uint32 MaxEntries = 64;
    OVERLAPPED_ENTRY entries[MaxEntries];
    ULONG count = 0;
    const ULONG wanted = maxCount < MaxEntries ? maxCount : MaxEntries;
    if (GetQueuedCompletionStatusEx(ToPort(_handle), entries, wanted, &count, timeout == ~0u ? INFINITE : timeout,
                                    FALSE) == 0)
    {
        return 0;
    }
    for (ULONG i = 0; i < count; ++i)
    {
        auto *operation = reinterpret_cast<IoOperation *>(entries[i].lpOverlapped);
        const auto *overlapped = reinterpret_cast<const OVERLAPPED *>(operation->Platform);
        operation->Bytes = entries[i].dwNumberOfBytesTransferred;
        operation->Succeeded = overlapped->Internal == 0;
        operations[i] = operation;
    }
    return count;
}
} // namespace Marlin
