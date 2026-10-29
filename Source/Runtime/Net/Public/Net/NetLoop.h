#pragma once

#include "Concurrency/Atomic.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformIoPort.h"
#include "HAL/PlatformThread.h"
#include "Templates/Function.h"

namespace Marlin
{
/// What owns operations on a NetLoop's port: each completed operation whose Context points at it comes back here,
/// on an IO thread.
class NET_API INetCompletion
{
public:
    virtual void Complete(IoOperation &operation) = 0;

protected:
    INetCompletion() = default;
    ~INetCompletion() = default;
};

/// A completion port and the dedicated IO threads that wait on it (Documentation/Architecture/14 §3). Every Net
/// callback runs on these threads and must be short; heavy work goes to the job system. The engine's loop belongs
/// to NetSubsystem; a program without subsystems makes its own.
class NET_API NetLoop
{
public:
    NetLoop() = default;
    ~NetLoop();
    MARLIN_NONCOPYABLE(NetLoop);

    /// *threads* IO threads, at least one.
    bool Start(uint32 threads);
    /// Waits for the threads; everything on the loop must be closed first.
    void Stop();

    [[nodiscard]] bool IsRunning() const
    {
        return !_threads.IsEmpty();
    }

    [[nodiscard]] IoPort &GetPort()
    {
        return _port;
    }

    /// Runs *work* on an IO thread soon; from any thread.
    void Post(Function<void()> work);
    /// True on one of this loop's IO threads.
    [[nodiscard]] bool IsIoThread() const;

private:
    static void ThreadMain(void *argument);
    void Run();

    IoPort _port;
    List<ThreadHandle> _threads;
    List<uint32> _threadIds;
    Atomic<uint32> _registering{0};
    Atomic<uint32> _started{0};
};
} // namespace Marlin
