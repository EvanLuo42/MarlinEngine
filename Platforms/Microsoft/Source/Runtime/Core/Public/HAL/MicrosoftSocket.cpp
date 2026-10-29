#include "HAL/PlatformSocket.h"

#include "HAL/MicrosoftHeaders.h"
#include "Memory/MemoryOps.h"

#include <mswsock.h>

namespace Marlin::PlatformSocket
{
static_assert(sizeof(sockaddr_in6) <= sizeof(SocketAddress::Storage));
static_assert(SocketHandle::InvalidValue == INVALID_SOCKET);

namespace
{
SOCKET ToSocket(SocketHandle socket)
{
    return static_cast<SOCKET>(socket.Value);
}

const sockaddr *ToSockaddr(const SocketAddress &address)
{
    return reinterpret_cast<const sockaddr *>(address.Storage);
}

sockaddr *ToSockaddr(SocketAddress &address)
{
    return reinterpret_cast<sockaddr *>(address.Storage);
}

int SocketLength(usize size)
{
    constexpr usize MaxChunk = 1u << 30;
    return static_cast<int>(size < MaxChunk ? size : MaxChunk);
}

bool SetOption(SocketHandle socket, int level, int option, int value)
{
    return setsockopt(ToSocket(socket), level, option, reinterpret_cast<const char *>(&value), sizeof(value)) == 0;
}

SocketAddress MakeAddress(uint16 port, bool ipv6, bool loopback)
{
    SocketAddress address;
    if (ipv6)
    {
        auto *ip = reinterpret_cast<sockaddr_in6 *>(address.Storage);
        ip->sin6_family = AF_INET6;
        ip->sin6_port = htons(port);
        ip->sin6_addr = loopback ? in6addr_loopback : in6addr_any;
        address.Length = sizeof(sockaddr_in6);
    }
    else
    {
        auto *ip = reinterpret_cast<sockaddr_in *>(address.Storage);
        ip->sin_family = AF_INET;
        ip->sin_port = htons(port);
        ip->sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
        address.Length = sizeof(sockaddr_in);
    }
    return address;
}
} // namespace

bool ParseAddress(const char *host, uint16 port, SocketAddress &address)
{
    address = {};
    auto *ip4 = reinterpret_cast<sockaddr_in *>(address.Storage);
    if (inet_pton(AF_INET, host, &ip4->sin_addr) == 1)
    {
        ip4->sin_family = AF_INET;
        ip4->sin_port = htons(port);
        address.Length = sizeof(sockaddr_in);
        return true;
    }
    auto *ip6 = reinterpret_cast<sockaddr_in6 *>(address.Storage);
    if (inet_pton(AF_INET6, host, &ip6->sin6_addr) == 1)
    {
        ip6->sin6_family = AF_INET6;
        ip6->sin6_port = htons(port);
        address.Length = sizeof(sockaddr_in6);
        return true;
    }
    address = {};
    return false;
}

bool ResolveAddress(const char *host, uint16 port, SocketAddress &address)
{
    address = {};
    ADDRINFOA hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOA *results = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &results) != 0)
    {
        return false;
    }
    const ADDRINFOA *chosen = nullptr;
    for (const ADDRINFOA *result = results; result != nullptr; result = result->ai_next)
    {
        if (chosen == nullptr || (result->ai_family == AF_INET && chosen->ai_family != AF_INET))
        {
            chosen = result;
        }
    }
    const bool found = chosen != nullptr && chosen->ai_addrlen <= sizeof(address.Storage);
    if (found)
    {
        MemCopy(address.Storage, chosen->ai_addr, chosen->ai_addrlen);
        address.Length = static_cast<uint32>(chosen->ai_addrlen);
        if (chosen->ai_family == AF_INET)
        {
            reinterpret_cast<sockaddr_in *>(address.Storage)->sin_port = htons(port);
        }
        else
        {
            reinterpret_cast<sockaddr_in6 *>(address.Storage)->sin6_port = htons(port);
        }
    }
    freeaddrinfo(results);
    return found;
}

SocketAddress AnyAddress(uint16 port, bool ipv6)
{
    return MakeAddress(port, ipv6, false);
}

SocketAddress LoopbackAddress(uint16 port, bool ipv6)
{
    return MakeAddress(port, ipv6, true);
}

uint16 GetPort(const SocketAddress &address)
{
    if (ToSockaddr(address)->sa_family == AF_INET6)
    {
        return ntohs(reinterpret_cast<const sockaddr_in6 *>(address.Storage)->sin6_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in *>(address.Storage)->sin_port);
}

bool IsIpv6(const SocketAddress &address)
{
    return ToSockaddr(address)->sa_family == AF_INET6;
}

usize FormatAddress(const SocketAddress &address, char *buffer, usize capacity)
{
    char host[INET6_ADDRSTRLEN] = {};
    const bool ipv6 = ToSockaddr(address)->sa_family == AF_INET6;
    const void *raw =
        ipv6 ? static_cast<const void *>(&reinterpret_cast<const sockaddr_in6 *>(address.Storage)->sin6_addr)
             : static_cast<const void *>(&reinterpret_cast<const sockaddr_in *>(address.Storage)->sin_addr);
    if (inet_ntop(ipv6 ? AF_INET6 : AF_INET, raw, host, sizeof(host)) == nullptr)
    {
        return 0;
    }

    char text[INET6_ADDRSTRLEN + 8];
    usize length = 0;
    if (ipv6)
    {
        text[length++] = '[';
    }
    for (const char *c = host; *c != '\0'; ++c)
    {
        text[length++] = *c;
    }
    if (ipv6)
    {
        text[length++] = ']';
    }
    text[length++] = ':';
    char digits[5];
    usize digitCount = 0;
    uint32 port = GetPort(address);
    do
    {
        digits[digitCount++] = static_cast<char>('0' + port % 10);
        port /= 10;
    } while (port != 0);
    while (digitCount > 0)
    {
        text[length++] = digits[--digitCount];
    }
    text[length] = '\0';
    if (length < capacity)
    {
        MemCopy(buffer, text, length + 1);
    }
    return length;
}

SocketHandle Create(SocketProtocol protocol, bool ipv6)
{
    const int type = protocol == SocketProtocol::Tcp ? SOCK_STREAM : SOCK_DGRAM;
    const int ipProtocol = protocol == SocketProtocol::Tcp ? IPPROTO_TCP : IPPROTO_UDP;
    const SOCKET socket = WSASocketW(ipv6 ? AF_INET6 : AF_INET, type, ipProtocol, nullptr, 0,
                                     WSA_FLAG_NO_HANDLE_INHERIT | WSA_FLAG_OVERLAPPED);
    if (socket != INVALID_SOCKET && ipv6)
    {
        const int dualStack = 0;
        setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&dualStack), sizeof(dualStack));
    }
    if (socket != INVALID_SOCKET && protocol == SocketProtocol::Udp)
    {
        BOOL reportReset = FALSE;
        DWORD bytes = 0;
        WSAIoctl(socket, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &bytes, nullptr, nullptr);
    }
    return {static_cast<uintptr>(socket)};
}

void Close(SocketHandle socket)
{
    if (socket.IsValid())
    {
        closesocket(ToSocket(socket));
    }
}

bool ShutdownSend(SocketHandle socket)
{
    return socket.IsValid() && shutdown(ToSocket(socket), SD_SEND) == 0;
}

bool Bind(SocketHandle socket, const SocketAddress &address)
{
    return bind(ToSocket(socket), ToSockaddr(address), static_cast<int>(address.Length)) == 0;
}

bool Listen(SocketHandle socket, uint32 backlog)
{
    return listen(ToSocket(socket), static_cast<int>(backlog)) == 0;
}

SocketHandle Accept(SocketHandle listener, SocketAddress *peer)
{
    SocketAddress address;
    int length = sizeof(address.Storage);
    const SOCKET socket = accept(ToSocket(listener), ToSockaddr(address), &length);
    if (socket != INVALID_SOCKET && peer != nullptr)
    {
        address.Length = static_cast<uint32>(length);
        *peer = address;
    }
    return {static_cast<uintptr>(socket)};
}

bool Connect(SocketHandle socket, const SocketAddress &address)
{
    return connect(ToSocket(socket), ToSockaddr(address), static_cast<int>(address.Length)) == 0;
}

isize Send(SocketHandle socket, const void *data, usize size)
{
    const int sent = send(ToSocket(socket), static_cast<const char *>(data), SocketLength(size), 0);
    return sent == SOCKET_ERROR ? -1 : sent;
}

isize Receive(SocketHandle socket, void *buffer, usize size)
{
    const int received = recv(ToSocket(socket), static_cast<char *>(buffer), SocketLength(size), 0);
    return received == SOCKET_ERROR ? -1 : received;
}

isize SendTo(SocketHandle socket, const void *data, usize size, const SocketAddress &address)
{
    const int sent = sendto(ToSocket(socket), static_cast<const char *>(data), SocketLength(size), 0,
                            ToSockaddr(address), static_cast<int>(address.Length));
    return sent == SOCKET_ERROR ? -1 : sent;
}

isize ReceiveFrom(SocketHandle socket, void *buffer, usize size, SocketAddress &from)
{
    from = {};
    int length = sizeof(from.Storage);
    const int received =
        recvfrom(ToSocket(socket), static_cast<char *>(buffer), SocketLength(size), 0, ToSockaddr(from), &length);
    if (received == SOCKET_ERROR)
    {
        return -1;
    }
    from.Length = static_cast<uint32>(length);
    return received;
}

bool SetNonBlocking(SocketHandle socket, bool nonBlocking)
{
    u_long mode = nonBlocking ? 1 : 0;
    return ioctlsocket(ToSocket(socket), static_cast<long>(FIONBIO), &mode) == 0;
}

bool SetNoDelay(SocketHandle socket, bool noDelay)
{
    return SetOption(socket, IPPROTO_TCP, TCP_NODELAY, noDelay ? 1 : 0);
}

bool SetReuseAddress(SocketHandle socket, bool reuse)
{
    return SetOption(socket, SOL_SOCKET, SO_REUSEADDR, reuse ? 1 : 0);
}

bool SetBroadcast(SocketHandle socket, bool broadcast)
{
    return SetOption(socket, SOL_SOCKET, SO_BROADCAST, broadcast ? 1 : 0);
}

bool SetBufferSizes(SocketHandle socket, uint32 sendSize, uint32 receiveSize)
{
    return SetOption(socket, SOL_SOCKET, SO_SNDBUF, static_cast<int>(sendSize)) &&
           SetOption(socket, SOL_SOCKET, SO_RCVBUF, static_cast<int>(receiveSize));
}

bool GetLocalAddress(SocketHandle socket, SocketAddress &address)
{
    address = {};
    int length = sizeof(address.Storage);
    if (getsockname(ToSocket(socket), ToSockaddr(address), &length) != 0)
    {
        return false;
    }
    address.Length = static_cast<uint32>(length);
    return true;
}

int32 Poll(SocketPollEntry *entries, uint32 count, uint32 timeout)
{
    constexpr uint32 MaxEntries = 64;
    if (count > MaxEntries)
    {
        WSASetLastError(WSAEINVAL);
        return -1;
    }
    WSAPOLLFD fds[MaxEntries];
    for (uint32 i = 0; i < count; ++i)
    {
        fds[i].fd = ToSocket(entries[i].Socket);
        uint32 requested = 0;
        if (HasAnyFlags(entries[i].Events, SocketEvents::Readable))
        {
            requested |= static_cast<uint32>(POLLRDNORM);
        }
        if (HasAnyFlags(entries[i].Events, SocketEvents::Writable))
        {
            requested |= static_cast<uint32>(POLLWRNORM);
        }
        fds[i].events = static_cast<SHORT>(requested);
        fds[i].revents = 0;
    }
    const int ready = WSAPoll(fds, count, timeout == 0xFFFFFFFFu ? -1 : static_cast<INT>(timeout));
    if (ready == SOCKET_ERROR)
    {
        return -1;
    }
    for (uint32 i = 0; i < count; ++i)
    {
        const auto events = static_cast<uint32>(static_cast<uint16>(fds[i].revents));
        SocketEvents result = SocketEvents::None;
        if ((events & (static_cast<uint32>(POLLRDNORM) | static_cast<uint32>(POLLHUP))) != 0)
        {
            result |= SocketEvents::Readable;
        }
        if ((events & static_cast<uint32>(POLLWRNORM)) != 0)
        {
            result |= SocketEvents::Writable;
        }
        if ((events & (static_cast<uint32>(POLLERR) | static_cast<uint32>(POLLHUP) | static_cast<uint32>(POLLNVAL))) !=
            0)
        {
            result |= SocketEvents::Error;
        }
        entries[i].Ready = result;
    }
    return ready;
}

SocketError LastError()
{
    switch (WSAGetLastError())
    {
    case 0:
        return SocketError::None;
    case WSAEWOULDBLOCK:
    case WSAEINPROGRESS:
    case WSAEALREADY:
        return SocketError::WouldBlock;
    case WSAECONNREFUSED:
        return SocketError::ConnectionRefused;
    case WSAECONNRESET:
    case WSAENETRESET:
        return SocketError::ConnectionReset;
    case WSAECONNABORTED:
        return SocketError::ConnectionAborted;
    case WSAENOTCONN:
    case WSAESHUTDOWN:
        return SocketError::NotConnected;
    case WSAETIMEDOUT:
        return SocketError::TimedOut;
    case WSAEADDRINUSE:
        return SocketError::AddressInUse;
    case WSAEADDRNOTAVAIL:
        return SocketError::AddressUnavailable;
    case WSAEHOSTUNREACH:
    case WSAENETUNREACH:
        return SocketError::HostUnreachable;
    case WSAEMSGSIZE:
        return SocketError::MessageTooLong;
    default:
        return SocketError::Other;
    }
}
} // namespace Marlin::PlatformSocket
