#pragma once

#include "Concurrency/Atomic.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "HAL/PlatformIoPort.h"
#include "HAL/PlatformSync.h"
#include "Memory/UniquePtr.h"
#include "Net/NetLoop.h"
#include "Templates/Function.h"

namespace Marlin
{
/// Gets each datagram and its sender on an IO thread; datagrams may arrive on several threads at once.
using NetDatagramHandler = Function<void(const SocketAddress &from, Span<const uint8> bytes)>;

/// The largest datagram received; bigger ones are truncated by the platform and dropped.
inline constexpr usize NetMaxDatagram = 65507;

/// A UDP endpoint with several receives in flight (Documentation/Architecture/14 §5), the base of the reliable UDP
/// that replication uses (05 §7).
class NET_API NetDatagramSocket final : private INetCompletion
{
public:
    ~NetDatagramSocket();
    MARLIN_NONCOPYABLE(NetDatagramSocket);

    /// Port zero takes any free port; *allowRemote* binds every interface instead of loopback.
    [[nodiscard]] static UniquePtr<NetDatagramSocket> Bind(NetLoop &loop, uint16 port, bool allowRemote,
                                                           NetDatagramHandler onReceive);

    [[nodiscard]] uint16 GetPort() const
    {
        return _port;
    }

    /// Sends a copy of *bytes*; from any thread. False if it could not start.
    bool SendTo(const SocketAddress &address, Span<const uint8> bytes);
    /// Stops receiving and waits for the operations in flight, so the handler never runs after it returns. Not from
    /// an IO thread.
    void Close();

private:
    struct Receive
    {
        IoOperation Operation;
        uint8 Buffer[NetMaxDatagram];
    };
    struct Send
    {
        IoOperation Operation;
        List<uint8> Bytes;
    };

    NetDatagramSocket(NetLoop &loop, NetDatagramHandler onReceive);

    void Complete(IoOperation &operation) override;
    bool Post(Receive &receive);
    void Done();
    /// Keeps a finished send for the next SendTo; needs _lock.
    void Recycle(Send *send);

    NetLoop &_loop;
    NetDatagramHandler _onReceive;
    SocketHandle _socket;
    uint16 _port = 0;
    List<UniquePtr<Receive>> _receives;
    List<Send *> _freeSends;
    Mutex _lock;
    Atomic<uint32> _inFlight{0};
    Event _idle{EventReset::Manual};
    bool _closing = false;
};
} // namespace Marlin
