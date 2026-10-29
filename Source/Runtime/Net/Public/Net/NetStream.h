#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "HAL/PlatformIoPort.h"
#include "HAL/PlatformSync.h"
#include "Memory/SharedPtr.h"
#include "Net/NetLoop.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
class NetStream;

struct NetStreamHandlers
{
    /// The bytes received and not yet consumed; returns how many it consumed, the rest wait for more.
    Function<usize(NetStream &stream, Span<const uint8> bytes)> OnReceive;
    /// Once, after the last OnReceive, when the stream closed for any reason.
    Function<void(NetStream &stream)> OnClose;
};

/// A byte stream over TCP or a local pipe (Documentation/Architecture/14 §4). One receive is in flight at a time, so
/// a stream's handlers never run concurrently; sends from any thread are queued and go out together.
class NET_API NetStream final : public RefCounted, private INetCompletion
{
public:
    ~NetStream() override;
    MARLIN_NONCOPYABLE(NetStream);

    /// Connects in the calling thread; null on failure or after *timeout* milliseconds.
    [[nodiscard]] static SharedPtr<NetStream> ConnectTcp(NetLoop &loop, StringView host, uint16 port, uint32 timeout);
    [[nodiscard]] static SharedPtr<NetStream> ConnectPipe(NetLoop &loop, StringView name, uint32 timeout);
    /// Wraps a connected socket or pipe; the stream owns it.
    [[nodiscard]] static SharedPtr<NetStream> FromSocket(NetLoop &loop, SocketHandle socket, StringView description);
    [[nodiscard]] static SharedPtr<NetStream> FromPipe(NetLoop &loop, PipeHandle pipe, StringView description);

    /// Starts receiving; the handlers run on IO threads from now on.
    void Start(NetStreamHandlers handlers);
    /// Queues a copy of *bytes*; from any thread. Dropped once the stream is closing.
    void Send(Span<const uint8> bytes);
    void Send(List<uint8> bytes);
    /// Queues *bytes* without copying them; *owner* keeps them alive and unchanged until they are sent or dropped.
    /// A pipe copies them when it joins several buffers into one write.
    void Send(SharedPtr<RefCounted> owner, Span<const uint8> bytes);
    /// Queues *size* bytes at *offset* of *file*, which the kernel sends from its file cache (TransmitFile); *owner*
    /// keeps the file open until then. *file* must be opened with FileFlags::Async. TCP only: false on a pipe, which
    /// sends nothing.
    bool SendFile(SharedPtr<RefCounted> owner, FileHandle file, uint64 offset, uint64 size);
    /// Sends what is queued, then closes.
    void Close();
    /// Closes now, dropping what is queued.
    void Abort();

    [[nodiscard]] usize GetQueuedBytes() const;
    [[nodiscard]] bool IsOpen() const;
    /// True for TCP, which reaches beyond this machine's user.
    [[nodiscard]] bool IsNetwork() const
    {
        return _socket.IsValid();
    }

    /// Such as "tcp 127.0.0.1:50000" or "pipe Marlin.1234", for logs.
    [[nodiscard]] StringView Describe() const
    {
        return _description;
    }

    [[nodiscard]] NetLoop &GetLoop() const
    {
        return _loop;
    }

private:
    friend class NetListener;

    /// One queued send: bytes the queue owns, bytes an owner keeps alive, or a region of a file.
    struct SendPart
    {
        List<uint8> Bytes;
        SharedPtr<RefCounted> Owner;
        /// Referenced bytes; null when the part owns Bytes or is a file region.
        const uint8 *Data = nullptr;
        FileHandle File;
        /// What is not sent yet: from Offset into Bytes, Data or File, Size bytes.
        uint64 Offset = 0;
        uint64 Size = 0;

        [[nodiscard]] bool IsFile() const
        {
            return File.IsValid();
        }

        [[nodiscard]] const uint8 *Begin() const
        {
            return (Data != nullptr ? Data : Bytes.Data()) + Offset;
        }
    };

    NetStream(NetLoop &loop, SocketHandle socket, PipeHandle pipe, StringView description);

    /// Queues *part* and starts a send if none is in flight; takes _lock.
    void Queue(SendPart part);
    void Complete(IoOperation &operation) override;
    void PostReceive();
    void Received(IoOperation &operation);
    void Sent(IoOperation &operation);
    /// Starts a send of the queue; needs _lock.
    void PostSend();
    /// Closes the handle once, ending sending first when *graceful* so queued data still arrives; needs _lock.
    void CloseHandle(bool graceful = false);
    void Finish();

    NetLoop &_loop;
    SocketHandle _socket;
    PipeHandle _pipe;
    String _description;
    NetStreamHandlers _handlers;

    IoOperation _receive;
    List<uint8> _input;
    usize _inputUsed = 0;

    mutable Mutex _lock;
    IoOperation _send;
    List<SendPart> _queued;
    List<SendPart> _sending;
    usize _queuedBytes = 0;
    bool _sendInFlight = false;
    bool _closing = false;
    bool _closed = false;
    bool _finished = false;
};
} // namespace Marlin
