#include "Net/NetStream.h"

#include "Concurrency/ScopeLock.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
constexpr usize ReceiveChunk = 64 << 10;
constexpr usize MaxSendParts = 64;
/// Sends up to this size are appended to the last queued buffer, while it stays under MaxJoinedBuffer.
constexpr usize MaxJoinedSend = 4 << 10;
constexpr usize MaxJoinedBuffer = 64 << 10;

class ConnectWait final : public INetCompletion
{
public:
    ConnectWait()
    {
        Operation.Context = static_cast<INetCompletion *>(this);
    }

    void Complete(IoOperation & /*operation*/) override
    {
        Done.Set();
    }

    IoOperation Operation;
    Event Done{EventReset::Manual};
};
} // namespace

NetStream::NetStream(NetLoop &loop, SocketHandle socket, PipeHandle pipe, StringView description)
    : _loop(loop), _socket(socket), _pipe(pipe), _description(description)
{
    _receive.Context = static_cast<INetCompletion *>(this);
    _send.Context = static_cast<INetCompletion *>(this);
}

NetStream::~NetStream()
{
    const ScopeLock lock(_lock);
    CloseHandle();
}

SharedPtr<NetStream> NetStream::FromSocket(NetLoop &loop, SocketHandle socket, StringView description)
{
    if (!socket.IsValid() || !loop.GetPort().Associate(socket))
    {
        PlatformSocket::Close(socket);
        return {};
    }
    (void)PlatformSocket::SetNoDelay(socket, true);
    return SharedPtr<NetStream>(new NetStream(loop, socket, {}, description));
}

SharedPtr<NetStream> NetStream::FromPipe(NetLoop &loop, PipeHandle pipe, StringView description)
{
    if (!pipe.IsValid() || !loop.GetPort().Associate(pipe))
    {
        PlatformPipe::Close(pipe);
        return {};
    }
    return SharedPtr<NetStream>(new NetStream(loop, {}, pipe, description));
}

SharedPtr<NetStream> NetStream::ConnectTcp(NetLoop &loop, StringView host, uint16 port, uint32 timeout)
{
    const String name(host);
    SocketAddress address;
    if (!PlatformSocket::ParseAddress(name.CStr(), port, address) &&
        !PlatformSocket::ResolveAddress(name.CStr(), port, address))
    {
        return {};
    }
    const SocketHandle socket = PlatformSocket::Create(SocketProtocol::Tcp, PlatformSocket::IsIpv6(address));
    if (!socket.IsValid() || !loop.GetPort().Associate(socket))
    {
        PlatformSocket::Close(socket);
        return {};
    }
    ConnectWait wait;
    if (!loop.GetPort().Connect(socket, address, wait.Operation))
    {
        PlatformSocket::Close(socket);
        return {};
    }
    if (!wait.Done.Wait(timeout))
    {
        loop.GetPort().Cancel(socket);
        (void)wait.Done.Wait(~0u);
    }
    if (!wait.Operation.Succeeded || !loop.GetPort().FinishConnect(socket))
    {
        PlatformSocket::Close(socket);
        return {};
    }
    (void)PlatformSocket::SetNoDelay(socket, true);
    return SharedPtr<NetStream>(new NetStream(loop, socket, {}, Format("tcp {0}:{1}", host, port).AsView()));
}

SharedPtr<NetStream> NetStream::ConnectPipe(NetLoop &loop, StringView name, uint32 timeout)
{
    const String text(name);
    return FromPipe(loop, PlatformPipe::Connect(text.CStr(), timeout), Format("pipe {0}", name).AsView());
}

void NetStream::Start(NetStreamHandlers handlers)
{
    _handlers = Move(handlers);
    AddReference();
    PostReceive();
}

void NetStream::PostReceive()
{
    const usize chunk = _inputUsed / 2 > ReceiveChunk ? _inputUsed / 2 : ReceiveChunk;
    if (_input.Count() < _inputUsed + chunk)
    {
        _input.ResizeUninitialized(_inputUsed + chunk);
    }
    bool started = false;
    {
        const ScopeLock lock(_lock);
        if (!_closed)
        {
            uint8 *buffer = _input.Data() + _inputUsed;
            const usize size = _input.Count() - _inputUsed;
            started = _socket.IsValid() ? _loop.GetPort().Receive(_socket, buffer, size, _receive)
                                        : _loop.GetPort().Receive(_pipe, buffer, size, _receive);
            if (!started)
            {
                CloseHandle();
            }
        }
    }
    if (!started)
    {
        Finish();
    }
}

void NetStream::Complete(IoOperation &operation)
{
    if (&operation == &_receive)
    {
        Received(operation);
    }
    else
    {
        Sent(operation);
    }
}

void NetStream::Received(IoOperation &operation)
{
    if (!operation.Succeeded || operation.Bytes == 0)
    {
        {
            const ScopeLock lock(_lock);
            CloseHandle();
        }
        Finish();
        return;
    }
    _inputUsed += operation.Bytes;
    usize consumed =
        _handlers.OnReceive ? _handlers.OnReceive(*this, Span<const uint8>(_input.Data(), _inputUsed)) : _inputUsed;
    consumed = consumed < _inputUsed ? consumed : _inputUsed;
    if (consumed != 0)
    {
        MemMove(_input.Data(), _input.Data() + consumed, _inputUsed - consumed);
        _inputUsed -= consumed;
    }
    PostReceive();
}

void NetStream::Finish()
{
    {
        const ScopeLock lock(_lock);
        if (_finished)
        {
            return;
        }
        _finished = true;
    }
    if (_handlers.OnClose)
    {
        _handlers.OnClose(*this);
    }
    _handlers = {};
    if (ReleaseReference())
    {
        delete this;
    }
}

void NetStream::Send(Span<const uint8> bytes)
{
    if (bytes.IsEmpty())
    {
        return;
    }
    {
        // Small messages join the last queued buffer the stream owns, so a burst of them costs no allocation each.
        const ScopeLock lock(_lock);
        if (_closing || _closed)
        {
            return;
        }
        if (!_queued.IsEmpty() && bytes.Count() <= MaxJoinedSend)
        {
            SendPart &last = _queued.Last();
            if (last.Data == nullptr && !last.IsFile() && last.Owner.Get() == nullptr &&
                last.Bytes.Count() + bytes.Count() <= MaxJoinedBuffer)
            {
                last.Bytes.AddRange(bytes);
                last.Size += bytes.Count();
                _queuedBytes += bytes.Count();
                if (!_sendInFlight)
                {
                    PostSend();
                }
                return;
            }
        }
    }
    List<uint8> copy;
    copy.AddRange(bytes);
    Send(Move(copy));
}

void NetStream::Send(List<uint8> bytes)
{
    SendPart part;
    part.Size = bytes.Count();
    part.Bytes = Move(bytes);
    Queue(Move(part));
}

void NetStream::Send(SharedPtr<RefCounted> owner, Span<const uint8> bytes)
{
    SendPart part;
    part.Owner = Move(owner);
    part.Data = bytes.Data();
    part.Size = bytes.Count();
    Queue(Move(part));
}

bool NetStream::SendFile(SharedPtr<RefCounted> owner, FileHandle file, uint64 offset, uint64 size)
{
    if (!IsNetwork() || !file.IsValid())
    {
        return false;
    }
    SendPart part;
    part.Owner = Move(owner);
    part.File = file;
    part.Offset = offset;
    part.Size = size;
    Queue(Move(part));
    return true;
}

void NetStream::Queue(SendPart part)
{
    if (part.Size == 0)
    {
        return;
    }
    const ScopeLock lock(_lock);
    if (_closing || _closed)
    {
        return;
    }
    _queuedBytes += static_cast<usize>(part.Size);
    _queued.Add(Move(part));
    if (!_sendInFlight)
    {
        PostSend();
    }
}

void NetStream::PostSend()
{
    if (_socket.IsValid())
    {
        // Buffers go out together in one gather; a file region goes alone, or with the one buffer before it as the
        // head of its TransmitFile, which is how a reply's header travels with its value.
        usize parts = 0;
        while (parts < _queued.Count() && parts < MaxSendParts && !_queued[parts].IsFile())
        {
            ++parts;
        }
        if (parts <= 1 && parts < _queued.Count() && _queued[parts].IsFile())
        {
            ++parts;
        }
        for (usize i = 0; i < parts; ++i)
        {
            _sending.Add(Move(_queued[i]));
        }
        _queued.RemoveAt(0, parts);
    }
    else if (_queued.Count() == 1)
    {
        _sending.Add(Move(_queued[0]));
        _queued.Clear();
    }
    else
    {
        SendPart &joined = _sending.AddDefaulted();
        for (const SendPart &part : _queued)
        {
            joined.Bytes.AddRange(Span<const uint8>(part.Begin(), static_cast<usize>(part.Size)));
        }
        joined.Size = joined.Bytes.Count();
        _queued.Clear();
    }
    AddReference();
    _sendInFlight = true;
    bool started = false;
    if (const SendPart &last = _sending.Last(); last.IsFile())
    {
        const SendPart *head = _sending.Count() > 1 ? &_sending[0] : nullptr;
        started = _loop.GetPort().SendFile(_socket, last.File, last.Offset, last.Size,
                                           head != nullptr ? head->Begin() : nullptr,
                                           head != nullptr ? static_cast<usize>(head->Size) : 0, _send);
    }
    else
    {
        const void *buffers[MaxSendParts];
        usize sizes[MaxSendParts];
        for (usize i = 0; i < _sending.Count(); ++i)
        {
            buffers[i] = _sending[i].Begin();
            sizes[i] = static_cast<usize>(_sending[i].Size);
        }
        started = _socket.IsValid()
                      ? _loop.GetPort().Send(_socket, buffers, sizes, static_cast<uint32>(_sending.Count()), _send)
                      : _loop.GetPort().Send(_pipe, buffers[0], sizes[0], _send);
    }
    if (!started)
    {
        _sendInFlight = false;
        _sending.Clear();
        CloseHandle();
        (void)ReleaseReference();
    }
}

void NetStream::Sent(IoOperation &operation)
{
    {
        const ScopeLock lock(_lock);
        _sendInFlight = false;
        if (!operation.Succeeded)
        {
            _sending.Clear();
            _queued.Clear();
            _queuedBytes = 0;
            CloseHandle();
        }
        else
        {
            uint64 sent = operation.Bytes;
            _queuedBytes -= sent < _queuedBytes ? static_cast<usize>(sent) : _queuedBytes;
            // Finished parts release their owners; what a short send left goes back to the front of the queue, the
            // first part resuming at an offset.
            usize finished = 0;
            while (finished < _sending.Count() && sent >= _sending[finished].Size)
            {
                sent -= _sending[finished].Size;
                ++finished;
            }
            if (finished < _sending.Count())
            {
                _sending[finished].Offset += sent;
                _sending[finished].Size -= sent;
            }
            _sending.RemoveAt(0, finished);
            if (!_sending.IsEmpty())
            {
                for (SendPart &part : _queued)
                {
                    _sending.Add(Move(part));
                }
                _queued.Clear();
                Swap(_queued, _sending);
            }
            if (!_queued.IsEmpty() && !_closed)
            {
                PostSend();
            }
            else if (_closing)
            {
                CloseHandle(true);
            }
        }
    }
    if (ReleaseReference())
    {
        delete this;
    }
}

void NetStream::Close()
{
    const ScopeLock lock(_lock);
    _closing = true;
    if (!_sendInFlight && _queued.IsEmpty())
    {
        CloseHandle(true);
    }
}

void NetStream::Abort()
{
    const ScopeLock lock(_lock);
    _closing = true;
    _queued.Clear();
    _queuedBytes = 0;
    CloseHandle();
}

void NetStream::CloseHandle(bool graceful)
{
    if (_closed)
    {
        return;
    }
    _closed = true;
    if (_socket.IsValid())
    {
        // Without it, closing right after a TransmitFile completes can cut off what TCP has not delivered yet.
        if (graceful)
        {
            (void)PlatformSocket::ShutdownSend(_socket);
        }
        PlatformSocket::Close(_socket);
    }
    else
    {
        PlatformPipe::Close(_pipe);
    }
}

usize NetStream::GetQueuedBytes() const
{
    const ScopeLock lock(_lock);
    return _queuedBytes;
}

bool NetStream::IsOpen() const
{
    const ScopeLock lock(_lock);
    return !_closed;
}
} // namespace Marlin
