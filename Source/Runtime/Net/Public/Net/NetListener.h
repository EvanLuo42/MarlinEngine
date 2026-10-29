#pragma once

#include "Concurrency/Atomic.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformIoPort.h"
#include "HAL/PlatformSync.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Net/NetLoop.h"
#include "Net/NetStream.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
/// Gets each accepted stream, not yet started, on an IO thread.
using NetAcceptHandler = Function<void(SharedPtr<NetStream> stream)>;

/// Accepts TCP connections or local pipe clients (Documentation/Architecture/14 §4), with several accepts in flight so
/// a burst of connections never waits for a thread.
class NET_API NetListener final : private INetCompletion
{
public:
    ~NetListener();
    MARLIN_NONCOPYABLE(NetListener);

    /// Port zero takes any free port; *allowRemote* listens on every interface instead of loopback.
    [[nodiscard]] static UniquePtr<NetListener> ListenTcp(NetLoop &loop, uint16 port, bool allowRemote,
                                                          NetAcceptHandler onAccept);
    [[nodiscard]] static UniquePtr<NetListener> ListenPipe(NetLoop &loop, StringView name, NetAcceptHandler onAccept);

    [[nodiscard]] uint16 GetPort() const
    {
        return _port;
    }

    /// Stops accepting and waits for the accepts in flight, so OnAccept never runs after it returns. Not from an IO
    /// thread.
    void Close();

private:
    struct Slot
    {
        IoOperation Operation;
        SocketHandle Accepted;
        PipeHandle Pipe;
        uint8 Addresses[IoAcceptBufferSize] = {};
    };

    NetListener(NetLoop &loop, NetAcceptHandler onAccept);

    void Complete(IoOperation &operation) override;
    /// Starts an accept on *slot*; needs _lock.
    bool Post(Slot &slot);
    void Accepted(SocketHandle accepted, PipeHandle pipe, const uint8 *addresses);

    NetLoop &_loop;
    NetAcceptHandler _onAccept;
    SocketHandle _listener;
    String _pipeName;
    uint16 _port = 0;
    List<UniquePtr<Slot>> _slots;
    Mutex _lock;
    Atomic<uint32> _inFlight{0};
    Event _idle{EventReset::Manual};
    bool _closing = false;
};
} // namespace Marlin
