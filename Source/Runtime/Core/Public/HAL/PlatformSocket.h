//! TCP and UDP sockets: addresses, handles, blocking and non-blocking I/O, options and polling.
//!
//! Functions report failure with false, a negative byte count or an invalid handle; `LastError` holds the cause for the
//! calling thread. A single send or receive call moves at most 1 GiB.

#pragma once

#include "CoreTypes.h"
#include "EnumFlags.h"

namespace Marlin
{
/// A socket's transport protocol.
enum class SocketProtocol : uint8
{
    /// A reliable, ordered byte stream (TCP).
    Tcp,
    /// Unreliable, unordered datagrams (UDP).
    Udp,
};

/// A socket failure, as returned by `PlatformSocket::LastError`.
enum class SocketError : uint8
{
    /// No error.
    None,
    /// The call would block, or an operation is still in progress.
    WouldBlock,
    /// The peer refused the connection.
    ConnectionRefused,
    /// The peer or the network reset the connection.
    ConnectionReset,
    /// The local network stack aborted the connection.
    ConnectionAborted,
    /// The socket is not connected, or is shut down in the direction used.
    NotConnected,
    /// The operation timed out.
    TimedOut,
    /// The address is already in use.
    AddressInUse,
    /// The address is not available on this machine.
    AddressUnavailable,
    /// The host or network cannot be reached.
    HostUnreachable,
    /// A datagram is larger than the socket or buffer allows.
    MessageTooLong,
    /// A failure with no dedicated value.
    Other,
};

/// Events requested from, or reported by, `PlatformSocket::Poll`; combined with `|`.
enum class SocketEvents : uint8
{
    /// No events.
    None = 0,
    /// The socket has data to read, or its peer closed the connection.
    Readable = 1 << 0,
    /// The socket can accept more data.
    Writable = 1 << 1,
    /// Reported only; also set for a closed connection.
    Error = 1 << 2,
};
MARLIN_ENUM_FLAGS(SocketEvents)

/// A native socket handle; `InvalidValue` means no socket.
struct SocketHandle
{
    /// The value an invalid handle has.
    static constexpr uintptr InvalidValue = ~static_cast<uintptr>(0);

    /// The native socket; `InvalidValue` when invalid.
    uintptr Value = InvalidValue;

    /// Returns true when the handle refers to a socket.
    bool IsValid() const
    {
        return Value != InvalidValue;
    }
};

/// An IPv4 or IPv6 address and port, in the platform's `sockaddr` form.
struct SocketAddress
{
    /// The address bytes; the family and port are encoded as the platform expects.
    alignas(8) uint8 Storage[28] = {};
    /// The number of bytes used in `Storage`.
    uint32 Length = 0;
};

/// One socket in a `PlatformSocket::Poll` call: the events requested and those found ready.
struct SocketPollEntry
{
    /// The socket to watch.
    SocketHandle Socket;
    /// The events to wait for; `SocketEvents::Error` cannot be requested.
    SocketEvents Events = SocketEvents::None;
    /// The events that are ready, filled by `Poll`.
    SocketEvents Ready = SocketEvents::None;
};

namespace PlatformSocket
{
/// Parses an IPv4 or IPv6 literal into `address` with the given `port`.
///
/// Does not resolve host names and does not accept brackets around an IPv6 literal.
///
/// # Errors
/// Returns false and clears `address` when `host` is not a numeric address.
CORE_API bool ParseAddress(const char *host, uint16 port, SocketAddress &address);
/// Resolves a host name and port into `address`, preferring IPv4.
///
/// Blocks until the system resolver returns. An IPv4 result is chosen whenever the resolver returns one.
///
/// # Errors
/// Returns false and clears `address` when the name cannot be resolved.
CORE_API bool ResolveAddress(const char *host, uint16 port, SocketAddress &address);
/// Returns the wildcard address for `port`: all IPv4 or all IPv6 interfaces.
CORE_API SocketAddress AnyAddress(uint16 port, bool ipv6 = false);
/// Returns the loopback address for `port` (`127.0.0.1` or `::1`).
CORE_API SocketAddress LoopbackAddress(uint16 port, bool ipv6 = false);
/// Returns the port of `address` in host byte order.
CORE_API uint16 GetPort(const SocketAddress &address);
/// Returns true when `address` is an IPv6 address.
CORE_API bool IsIpv6(const SocketAddress &address);
/// Writes `host:port` to `buffer`, bracketing the host for IPv6, and returns the UTF-8 length without terminator.
///
/// The terminated string is written only when the length is less than `capacity`.
///
/// # Errors
/// Returns zero when the address cannot be formatted.
CORE_API usize FormatAddress(const SocketAddress &address, char *buffer, usize capacity);

/// Creates a socket of `protocol`, IPv4 by default or IPv6 when `ipv6` is true.
///
/// The socket is created for overlapped I/O, so it can be associated with a completion port; blocking calls still work.
/// An IPv6 socket is dual-stack and accepts IPv4 peers too. A UDP socket does not report ICMP port-unreachable errors.
///
/// # Errors
/// Returns an invalid handle when the socket cannot be created.
CORE_API SocketHandle Create(SocketProtocol protocol, bool ipv6 = false);
/// Closes `socket`; does nothing for an invalid handle.
CORE_API void Close(SocketHandle socket);
/// Ends sending on `socket`: what was already sent still arrives, then the peer reads the end of the stream.
///
/// Receiving is unaffected.
///
/// # Errors
/// Returns false when the socket is invalid or the call fails.
CORE_API bool ShutdownSend(SocketHandle socket);

/// Binds `socket` to the local `address`.
///
/// # Errors
/// Returns false when the address is in use or cannot be bound.
CORE_API bool Bind(SocketHandle socket, const SocketAddress &address);
/// Starts listening on a bound TCP socket, with room for `backlog` pending connections.
///
/// # Errors
/// Returns false when the socket cannot listen.
CORE_API bool Listen(SocketHandle socket, uint32 backlog = 128);
/// Accepts a pending connection on `listener` and writes the peer's address to `peer` when it is not null.
///
/// # Errors
/// Returns an invalid handle on failure. A non-blocking listener with no pending connection fails and `LastError`
/// reports `SocketError::WouldBlock`.
CORE_API SocketHandle Accept(SocketHandle listener, SocketAddress *peer = nullptr);
/// Starts a connection from `socket` to `address`.
///
/// A blocking socket returns once the connection is established or has failed. A non-blocking socket fails with
/// `SocketError::WouldBlock` and becomes writable once the connection completes.
///
/// # Errors
/// Returns false when the connection cannot be started or fails.
CORE_API bool Connect(SocketHandle socket, const SocketAddress &address);

/// Sends up to `size` bytes on a connected socket and returns the number sent, or -1 on error.
///
/// A stream send can accept fewer bytes than requested.
///
/// # Errors
/// Returns -1 on failure; `LastError` holds the cause.
CORE_API isize Send(SocketHandle socket, const void *data, usize size);
/// Receives up to `size` bytes from a connected socket and returns the number received, or -1 on error.
///
/// On a stream socket, returns 0 once the peer has closed the connection.
///
/// # Errors
/// Returns -1 on failure; `LastError` holds the cause.
CORE_API isize Receive(SocketHandle socket, void *buffer, usize size);
/// Sends one datagram of `size` bytes to `address` and returns the number sent, or -1 on error.
///
/// # Errors
/// Returns -1 on failure; `LastError` holds the cause.
CORE_API isize SendTo(SocketHandle socket, const void *data, usize size, const SocketAddress &address);
/// Receives one datagram into `buffer`, writes the sender to `from`, and returns its size or -1 on error.
///
/// A return value of 0 is a valid empty datagram.
///
/// # Errors
/// Returns -1 on failure; `LastError` holds the cause.
CORE_API isize ReceiveFrom(SocketHandle socket, void *buffer, usize size, SocketAddress &from);

/// Sets whether `socket` blocks; a non-blocking call that would block fails with `SocketError::WouldBlock`.
///
/// # Errors
/// Returns false when the mode cannot be changed.
CORE_API bool SetNonBlocking(SocketHandle socket, bool nonBlocking);
/// Enables or disables the Nagle algorithm on a TCP socket.
///
/// # Errors
/// Returns false when the option cannot be set, including on a non-TCP socket.
CORE_API bool SetNoDelay(SocketHandle socket, bool noDelay);
/// Allows the socket to bind to a local port that another socket is still using.
///
/// # Errors
/// Returns false when the option cannot be set.
CORE_API bool SetReuseAddress(SocketHandle socket, bool reuse);
/// Allows sending broadcasts from a UDP socket.
///
/// # Errors
/// Returns false when the option cannot be set.
CORE_API bool SetBroadcast(SocketHandle socket, bool broadcast);
/// Sets the send and receive buffer sizes in bytes.
///
/// # Errors
/// Returns false when either size cannot be set.
CORE_API bool SetBufferSizes(SocketHandle socket, uint32 sendSize, uint32 receiveSize);
/// Writes the socket's local address to `address`.
///
/// # Errors
/// Returns false when the address cannot be read.
CORE_API bool GetLocalAddress(SocketHandle socket, SocketAddress &address);

/// Waits for events on up to 64 sockets and fills each entry's `Ready`.
///
/// Returns the number of entries with a ready event, 0 on timeout, or -1 on error. `timeout` is in milliseconds;
/// `0xFFFFFFFF` waits indefinitely. `entries[i].Events` is the set to wait for.
///
/// # Errors
/// Returns -1 when the call fails, including when `count` exceeds 64.
///
/// # Examples
///
/// ```cpp
/// SocketPollEntry entry;
/// entry.Socket = listener;
/// entry.Events = SocketEvents::Readable;
/// const int32 ready = PlatformSocket::Poll(&entry, 1, 0); // -> 1 when a connection is pending.
/// ```
CORE_API int32 Poll(SocketPollEntry *entries, uint32 count, uint32 timeout);
/// Returns the error of the calling thread's last failed socket call.
///
/// The value is per thread and is not cleared by a successful call.
CORE_API SocketError LastError();
} // namespace PlatformSocket
} // namespace Marlin
