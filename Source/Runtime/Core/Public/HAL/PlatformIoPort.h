//! A completion port for asynchronous socket, pipe and file I/O.
//!
//! Operations start on handles associated with the port and complete into caller-owned `IoOperation` values, which
//! `Wait` hands back with the byte count and result. On Windows the port is an I/O completion port, and the extension
//! functions `AcceptEx`, `ConnectEx`, `GetAcceptExSockaddrs` and `TransmitFile` are loaded at runtime.

#pragma once

#include "CoreTypes.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformIpc.h"
#include "HAL/PlatformSocket.h"

namespace Marlin
{
/// The operation an `IoOperation` represents.
enum class IoOperationKind : uint8
{
    /// A socket receive.
    Receive,
    /// A socket send.
    Send,
    /// A socket accept.
    Accept,
    /// A socket connect.
    Connect,
    /// A wait for a pipe client.
    AcceptPipe,
    /// A datagram receive.
    ReceiveFrom,
    /// A datagram send.
    SendTo,
    /// A completion posted by `Post`.
    Posted,
    /// A file read.
    FileRead,
    /// A file-to-socket send.
    FileSend,
};

/// One asynchronous socket, pipe or file operation.
///
/// The caller owns the operation and keeps it alive and in place from the call that starts it until `Wait` hands it
/// back.
///
/// # Safety
/// The platform uses the operation's address as its completion identity. Moving, copying or destroying the operation
/// while it is in flight leaves the completion pointing at invalid memory.
struct IoOperation
{
    /// The platform's overlapped structure; first, so a completion finds its operation.
    alignas(8) uint8 Platform[32] = {};
    /// A caller-owned pointer that the port never changes.
    void *Context = nullptr;
    /// The bytes transferred, set by `Wait`.
    uint32 Bytes = 0;
    /// True when the operation succeeded, set by `Wait`.
    bool Succeeded = false;
    /// The kind of operation, set when it starts or is posted.
    IoOperationKind Kind = IoOperationKind::Posted;
    /// The sender of a `ReceiveFrom`, set by the platform on completion.
    SocketAddress Address;
};

/// The number of bytes `Accept` needs for the two addresses it writes.
inline constexpr usize IoAcceptBufferSize = 2 * (28 + 16);

/// A completion queue for sockets, pipes and files: an I/O completion port on Windows.
///
/// Operations started on a handle associated with the port complete there, and any number of threads may wait on it.
/// Every start function returns false when the operation failed to start; such an operation never completes. Buffers
/// passed to a start function stay owned by the caller and must remain valid until the operation completes. A single
/// buffer is limited to 1 GiB per operation.
///
/// # Preconditions
/// `Initialize` is called once and succeeds before `Associate`, a start function or `Wait` is used; those functions do
/// not check the port. `Shutdown` and the destructor are safe on a fresh port.
///
/// # Thread safety
/// `Wait` and `Post` are safe from several threads. Starting an operation is safe while other threads wait; an
/// operation must not be reused before it completes.
///
/// # Examples
///
/// ```cpp
/// IoPort port;
/// if (port.Initialize() && port.Associate(socket))
/// {
///     char buffer[1024];
///     IoOperation operation;
///     if (port.Receive(socket, buffer, sizeof(buffer), operation))
///     {
///         IoOperation *completed = nullptr;
///         const uint32 count = port.Wait(&completed, 1, ~0u); // -> 1 once the receive completes.
///     }
/// }
/// ```
class CORE_API IoPort
{
public:
    /// Creates a port that is not initialized.
    IoPort() = default;
    /// Shuts the port down.
    ~IoPort();
    MARLIN_NONCOPYABLE(IoPort);

    /// Creates the completion port.
    ///
    /// # Errors
    /// Returns false when the port cannot be created.
    bool Initialize();
    /// Closes the port and releases its handle.
    ///
    /// # Preconditions
    /// No operation is in flight; the port does not cancel or wait for them.
    void Shutdown();

    /// Associates `socket` with the port so its operations complete here.
    ///
    /// # Errors
    /// Returns false when the socket cannot be associated.
    bool Associate(SocketHandle socket);
    /// Associates `pipe` with the port so its operations complete here.
    ///
    /// # Errors
    /// Returns false when the pipe cannot be associated.
    bool Associate(PipeHandle pipe);
    /// Associates `file` with the port so its reads complete here.
    ///
    /// # Preconditions
    /// `file` was opened with `FileFlags::Async`.
    ///
    /// # Errors
    /// Returns false when the file cannot be associated.
    bool Associate(FileHandle file);

    /// Starts a receive from `socket` into `buffer` of up to `size` bytes.
    ///
    /// A completed receive of zero bytes means the peer closed the connection.
    ///
    /// # Errors
    /// Returns false when the receive cannot start; it then never completes.
    bool Receive(SocketHandle socket, void *buffer, usize size, IoOperation &operation);
    /// Sends the `count` buffers in order as one operation.
    ///
    /// `count` must be between 1 and 64; `operation.Bytes` is the total sent.
    ///
    /// # Errors
    /// Returns false when `count` is out of range or the send cannot start.
    bool Send(SocketHandle socket, const void *const *buffers, const usize *sizes, uint32 count,
              IoOperation &operation);
    /// Starts a receive of one datagram, writing the sender to `operation.Address` on completion.
    ///
    /// # Errors
    /// Returns false when the receive cannot start.
    bool ReceiveFrom(SocketHandle socket, void *buffer, usize size, IoOperation &operation);
    /// Starts a send of one datagram to `address`.
    ///
    /// # Errors
    /// Returns false when the send cannot start.
    bool SendTo(SocketHandle socket, const void *data, usize size, const SocketAddress &address,
                IoOperation &operation);
    /// Accepts a connection on `listener` into `accepted`, a fresh unconnected TCP socket of the same family.
    ///
    /// `addresses` holds `IoAcceptBufferSize` bytes until completion. Call `FinishAccept` after the operation succeeds
    /// and before using `accepted`.
    ///
    /// # Errors
    /// Returns false when the accept cannot start, including when the platform lacks `AcceptEx`.
    bool Accept(SocketHandle listener, SocketHandle accepted, void *addresses, IoOperation &operation);
    /// Makes `accepted` usable after its `Accept` operation succeeded and writes the peer address to `peer`.
    ///
    /// # Errors
    /// Returns false when the accepted socket's context cannot be updated.
    bool FinishAccept(SocketHandle listener, SocketHandle accepted, const void *addresses, SocketAddress &peer);
    /// Starts connecting `socket`, which must be associated and unbound, to `address`.
    ///
    /// Call `FinishConnect` after the operation succeeds and before using the socket.
    ///
    /// # Errors
    /// Returns false when the connect cannot start, including when the platform lacks `ConnectEx`.
    bool Connect(SocketHandle socket, const SocketAddress &address, IoOperation &operation);
    /// Makes `socket` usable after its `Connect` operation succeeded.
    ///
    /// # Errors
    /// Returns false when the connection context cannot be updated.
    bool FinishConnect(SocketHandle socket);

    /// Starts a read from a pipe instance.
    ///
    /// # Errors
    /// Returns false when the read cannot start.
    bool Receive(PipeHandle pipe, void *buffer, usize size, IoOperation &operation);
    /// Starts a write to a pipe instance.
    ///
    /// # Errors
    /// Returns false when the write cannot start.
    bool Send(PipeHandle pipe, const void *data, usize size, IoOperation &operation);
    /// Waits for a client on a pipe server instance.
    ///
    /// If a client is already connected, the operation completes at once.
    ///
    /// # Errors
    /// Returns false when the wait cannot start.
    bool AcceptPipe(PipeHandle pipe, IoOperation &operation);

    /// Starts a read of up to `size` bytes at `offset` of an associated file.
    ///
    /// # Errors
    /// Returns false when the read cannot start, including a read at or past the end of the file; such an operation
    /// never completes.
    bool Read(FileHandle file, uint64 offset, void *buffer, usize size, IoOperation &operation);
    /// Sends `head`, then `size` bytes at `offset` of `file`, from the kernel's file cache straight to the socket.
    ///
    /// Uses `TransmitFile` on Windows and needs a connected TCP socket. `file` is opened with `FileFlags::Async` and
    /// need not be associated. A `size` of zero sends only `head`. `operation.Bytes` counts the head too, and, as with
    /// any send, the operation may stop short of a large `size`; at most 1 GiB of file data is sent per call.
    ///
    /// # Errors
    /// Returns false when the send cannot start, including when the platform lacks `TransmitFile`.
    bool SendFile(SocketHandle socket, FileHandle file, uint64 offset, uint64 size, const void *head, usize headSize,
                  IoOperation &operation);

    /// Cancels every operation in flight on `socket`; they complete as failed.
    void Cancel(SocketHandle socket);
    /// Cancels every operation in flight on `pipe`; they complete as failed.
    void Cancel(PipeHandle pipe);

    /// Completes `operation` at once with its current `Bytes`, to hand work to a waiting thread.
    ///
    /// The completion has `IoOperationKind::Posted` and succeeds.
    ///
    /// # Errors
    /// Returns false when the completion cannot be posted.
    bool Post(IoOperation &operation);
    /// Writes up to `maxCount` completed operations, at most 64, and returns how many.
    ///
    /// Waits up to `timeout` milliseconds for the first completion; 0 returns at once and `~0u` waits indefinitely.
    /// Each written pointer refers to the caller's operation, with `Bytes` and `Succeeded` set.
    ///
    /// # Errors
    /// Returns 0 on timeout or when the port fails; the two cases are not distinguished.
    uint32 Wait(IoOperation **operations, uint32 maxCount, uint32 timeout);

private:
    uintptr _handle = 0;
};
} // namespace Marlin
