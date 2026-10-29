#pragma once

#include "Concurrency/Atomic.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformSync.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Net/NetStream.h"
#include "Remote/RemoteFraming.h"
#include "Templates/Function.h"

namespace Marlin
{
class RemoteConnection;

using RemoteFrameHandler = Function<void(RemoteConnection &connection, RemoteFrame &frame)>;
using RemoteCloseHandler = Function<void(RemoteConnection &connection)>;

/// A NetStream and the framing on it (Documentation/Architecture/14 §6): each frame received goes to the frame
/// handler on an IO thread. Work that must not hold up the IO thread is dispatched to the job system, one job at a
/// time per connection so its requests keep their order. Shared by its owner and the calls in flight on it.
class REMOTE_API RemoteConnection final : public RefCounted
{
public:
    RemoteConnection(SharedPtr<NetStream> stream, UniquePtr<IRemoteFraming> framing, RemoteFrameHandler onFrame,
                     RemoteCloseHandler onClose);
    ~RemoteConnection() override;
    MARLIN_NONCOPYABLE(RemoteConnection);

    void Start();
    /// Queues *frame*; from any thread. Dropped once the connection has closed.
    void Send(const RemoteFrame &frame);
    void SendText(StringView text);
    /// Sends raw bytes, such as a handshake request, before any frame.
    void SendRaw(List<uint8> bytes);
    /// Announces the close where the protocol can, sends what is queued, then closes.
    void Close();
    /// Waits until the close handler has run and the dispatched work is done. Not from an IO thread.
    void WaitClosed();
    /// Runs *work* on the job system after the work dispatched before it.
    void Dispatch(Function<void()> work);
    /// Waits up to *timeout* milliseconds for the framing's handshake; false if it failed or timed out.
    bool WaitReady(uint32 timeout);

    [[nodiscard]] bool IsOpen() const
    {
        return _open.Load(MemoryOrder::Acquire);
    }

    [[nodiscard]] bool IsReady() const;

    [[nodiscard]] uint32 GetId() const
    {
        return _id;
    }

    [[nodiscard]] bool IsNetwork() const
    {
        return _stream->IsNetwork();
    }

    [[nodiscard]] StringView Describe() const
    {
        return _stream->Describe();
    }

    /// When the connection was accepted or made, in milliseconds of PlatformTime.
    [[nodiscard]] uint64 GetStartTime() const
    {
        return _startTime;
    }

private:
    static void RunDispatched(void *argument);
    usize Receive(Span<const uint8> bytes);
    void Closed();

    SharedPtr<NetStream> _stream;
    UniquePtr<IRemoteFraming> _framing;
    RemoteFrameHandler _onFrame;
    RemoteCloseHandler _onClose;
    mutable Mutex _framingLock;
    Mutex _dispatchLock;
    List<Function<void()>> _dispatched;
    /// The batch RunDispatched is running.
    List<Function<void()>> _running;
    bool _dispatching = false;
    Event _ready{EventReset::Manual};
    Event _closed{EventReset::Manual};
    Event _idle{EventReset::Manual};
    uint64 _startTime = 0;
    uint32 _id;
    Atomic<bool> _open{true};
};
} // namespace Marlin
