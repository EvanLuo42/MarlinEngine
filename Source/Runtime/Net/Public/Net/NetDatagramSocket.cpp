#include "Net/NetDatagramSocket.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Assert.h"

namespace Marlin
{
namespace
{
constexpr usize ReceiveSlots = 8;
/// Finished sends kept for reuse, with their buffers.
constexpr usize MaxFreeSends = 64;
} // namespace

NetDatagramSocket::NetDatagramSocket(NetLoop &loop, NetDatagramHandler onReceive)
    : _loop(loop), _onReceive(Move(onReceive))
{
}

NetDatagramSocket::~NetDatagramSocket()
{
    Close();
    for (Send *send : _freeSends)
    {
        delete send;
    }
}

UniquePtr<NetDatagramSocket> NetDatagramSocket::Bind(NetLoop &loop, uint16 port, bool allowRemote,
                                                     NetDatagramHandler onReceive)
{
    UniquePtr<NetDatagramSocket> socket(new NetDatagramSocket(loop, Move(onReceive)));
    socket->_socket = PlatformSocket::Create(SocketProtocol::Udp);
    const SocketAddress address =
        allowRemote ? PlatformSocket::AnyAddress(port) : PlatformSocket::LoopbackAddress(port);
    SocketAddress bound;
    if (!socket->_socket.IsValid() || !PlatformSocket::Bind(socket->_socket, address) ||
        !PlatformSocket::GetLocalAddress(socket->_socket, bound) || !loop.GetPort().Associate(socket->_socket))
    {
        return {};
    }
    (void)PlatformSocket::SetBufferSizes(socket->_socket, 4 << 20, 4 << 20);
    socket->_port = PlatformSocket::GetPort(bound);
    const ScopeLock lock(socket->_lock);
    for (usize i = 0; i < ReceiveSlots; ++i)
    {
        Receive &receive = *socket->_receives.Add(MakeUnique<Receive>());
        receive.Operation.Context = static_cast<INetCompletion *>(socket.Get());
        if (!socket->Post(receive))
        {
            return {};
        }
    }
    return socket;
}

bool NetDatagramSocket::Post(Receive &receive)
{
    if (_closing)
    {
        return false;
    }
    if (!_loop.GetPort().ReceiveFrom(_socket, receive.Buffer, sizeof(receive.Buffer), receive.Operation))
    {
        return false;
    }
    _inFlight.FetchAdd(1, MemoryOrder::Relaxed);
    return true;
}

bool NetDatagramSocket::SendTo(const SocketAddress &address, Span<const uint8> bytes)
{
    Send *send = nullptr;
    {
        const ScopeLock lock(_lock);
        send = !_freeSends.IsEmpty() ? _freeSends.Pop() : nullptr;
    }
    if (send == nullptr)
    {
        send = new Send();
        send->Operation.Context = static_cast<INetCompletion *>(this);
    }
    send->Bytes.Clear();
    send->Bytes.AddRange(bytes);
    const ScopeLock lock(_lock);
    if (_closing || !_loop.GetPort().SendTo(_socket, send->Bytes.Data(), send->Bytes.Count(), address, send->Operation))
    {
        Recycle(send);
        return false;
    }
    _inFlight.FetchAdd(1, MemoryOrder::Relaxed);
    return true;
}

void NetDatagramSocket::Recycle(Send *send)
{
    if (_freeSends.Count() < MaxFreeSends)
    {
        _freeSends.Add(send);
        return;
    }
    delete send;
}

void NetDatagramSocket::Complete(IoOperation &operation)
{
    if (operation.Kind == IoOperationKind::SendTo)
    {
        {
            const ScopeLock lock(_lock);
            Recycle(reinterpret_cast<Send *>(&operation));
        }
        Done();
        return;
    }
    auto &receive = *reinterpret_cast<Receive *>(&operation);
    if (operation.Succeeded && _onReceive)
    {
        _onReceive(operation.Address, Span<const uint8>(receive.Buffer, operation.Bytes));
    }
    {
        const ScopeLock lock(_lock);
        (void)Post(receive);
    }
    Done();
}

void NetDatagramSocket::Done()
{
    if (_inFlight.FetchSub(1, MemoryOrder::AcquireRelease) == 1)
    {
        const ScopeLock lock(_lock);
        if (_closing)
        {
            _idle.Set();
        }
    }
}

void NetDatagramSocket::Close()
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
        if (_socket.IsValid())
        {
            PlatformSocket::Close(_socket);
        }
    }
    (void)_idle.Wait();
    _socket = {};
}
} // namespace Marlin
