#include "Net/NetListener.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Assert.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
constexpr usize TcpSlots = 8;
constexpr usize PipeSlots = 4;
} // namespace

NetListener::NetListener(NetLoop &loop, NetAcceptHandler onAccept) : _loop(loop), _onAccept(Move(onAccept))
{
}

NetListener::~NetListener()
{
    Close();
}

UniquePtr<NetListener> NetListener::ListenTcp(NetLoop &loop, uint16 port, bool allowRemote, NetAcceptHandler onAccept)
{
    UniquePtr<NetListener> listener(new NetListener(loop, Move(onAccept)));
    listener->_listener = PlatformSocket::Create(SocketProtocol::Tcp);
    const SocketAddress address =
        allowRemote ? PlatformSocket::AnyAddress(port) : PlatformSocket::LoopbackAddress(port);
    SocketAddress bound;
    if (!listener->_listener.IsValid() || !PlatformSocket::Bind(listener->_listener, address) ||
        !PlatformSocket::Listen(listener->_listener) || !PlatformSocket::GetLocalAddress(listener->_listener, bound) ||
        !loop.GetPort().Associate(listener->_listener))
    {
        return {};
    }
    listener->_port = PlatformSocket::GetPort(bound);
    const ScopeLock lock(listener->_lock);
    for (usize i = 0; i < TcpSlots; ++i)
    {
        Slot &slot = *listener->_slots.Add(MakeUnique<Slot>());
        slot.Operation.Context = static_cast<INetCompletion *>(listener.Get());
        if (!listener->Post(slot))
        {
            return {};
        }
    }
    return listener;
}

UniquePtr<NetListener> NetListener::ListenPipe(NetLoop &loop, StringView name, NetAcceptHandler onAccept)
{
    UniquePtr<NetListener> listener(new NetListener(loop, Move(onAccept)));
    listener->_pipeName = name;
    const ScopeLock lock(listener->_lock);
    for (usize i = 0; i < PipeSlots; ++i)
    {
        Slot &slot = *listener->_slots.Add(MakeUnique<Slot>());
        slot.Operation.Context = static_cast<INetCompletion *>(listener.Get());
        if (!listener->Post(slot))
        {
            return {};
        }
    }
    return listener;
}

bool NetListener::Post(Slot &slot)
{
    if (_closing)
    {
        return false;
    }
    IoPort &port = _loop.GetPort();
    bool started = false;
    if (_pipeName.IsEmpty())
    {
        slot.Accepted = PlatformSocket::Create(SocketProtocol::Tcp);
        started = slot.Accepted.IsValid() && port.Accept(_listener, slot.Accepted, slot.Addresses, slot.Operation);
        if (!started)
        {
            PlatformSocket::Close(slot.Accepted);
            slot.Accepted = {};
        }
    }
    else
    {
        slot.Pipe = PlatformPipe::CreateServer(_pipeName.CStr());
        started = slot.Pipe.IsValid() && port.Associate(slot.Pipe) && port.AcceptPipe(slot.Pipe, slot.Operation);
        if (!started)
        {
            PlatformPipe::Close(slot.Pipe);
            slot.Pipe = {};
        }
    }
    if (started)
    {
        _inFlight.FetchAdd(1, MemoryOrder::Relaxed);
    }
    return started;
}

void NetListener::Complete(IoOperation &operation)
{
    Slot *slot = nullptr;
    for (UniquePtr<Slot> &candidate : _slots)
    {
        if (&candidate->Operation == &operation)
        {
            slot = candidate.Get();
        }
    }
    MARLIN_ASSERT(slot != nullptr);
    SocketHandle accepted;
    PipeHandle pipe;
    {
        const ScopeLock lock(_lock);
        accepted = slot->Accepted;
        pipe = slot->Pipe;
        slot->Accepted = {};
        slot->Pipe = {};
    }
    if (operation.Succeeded)
    {
        Accepted(accepted, pipe, slot->Addresses);
    }
    else if (_pipeName.IsEmpty())
    {
        PlatformSocket::Close(accepted);
    }
    else
    {
        PlatformPipe::Close(pipe);
    }
    {
        const ScopeLock lock(_lock);
        (void)Post(*slot);
    }
    if (_inFlight.FetchSub(1, MemoryOrder::AcquireRelease) == 1)
    {
        const ScopeLock lock(_lock);
        if (_closing)
        {
            _idle.Set();
        }
    }
}

void NetListener::Accepted(SocketHandle accepted, PipeHandle pipe, const uint8 *addresses)
{
    SharedPtr<NetStream> stream;
    if (_pipeName.IsEmpty())
    {
        SocketAddress peer;
        if (!_loop.GetPort().FinishAccept(_listener, accepted, addresses, peer))
        {
            PlatformSocket::Close(accepted);
            return;
        }
        char text[64];
        const usize length = PlatformSocket::FormatAddress(peer, text, sizeof(text));
        stream = NetStream::FromSocket(
            _loop, accepted, Format("tcp {0}", StringView(text, length < sizeof(text) ? length : 0)).AsView());
    }
    else
    {
        stream = SharedPtr<NetStream>(new NetStream(_loop, {}, pipe, Format("pipe {0}", _pipeName).AsView()));
    }
    if (stream.IsValid() && _onAccept)
    {
        _onAccept(Move(stream));
    }
}

void NetListener::Close()
{
    MARLIN_ASSERT(!_loop.IsIoThread());
    {
        const ScopeLock lock(_lock);
        if (_closing)
        {
            return;
        }
        _closing = true;
        if (_inFlight.Load(MemoryOrder::Acquire) == 0)
        {
            _idle.Set();
        }
        if (_listener.IsValid())
        {
            PlatformSocket::Close(_listener);
        }
        for (UniquePtr<Slot> &slot : _slots)
        {
            if (slot->Pipe.IsValid())
            {
                _loop.GetPort().Cancel(slot->Pipe);
            }
        }
    }
    (void)_idle.Wait();
    _listener = {};
}
} // namespace Marlin
