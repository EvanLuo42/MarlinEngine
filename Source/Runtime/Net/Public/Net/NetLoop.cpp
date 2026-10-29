#include "Net/NetLoop.h"

namespace Marlin
{
namespace
{
class PostedWork final : public INetCompletion
{
public:
    explicit PostedWork(Function<void()> work) : _work(Move(work))
    {
        Operation.Context = static_cast<INetCompletion *>(this);
    }

    void Complete(IoOperation & /*operation*/) override
    {
        _work();
        delete this;
    }

    IoOperation Operation;

private:
    Function<void()> _work;
};
} // namespace

NetLoop::~NetLoop()
{
    Stop();
}

bool NetLoop::Start(uint32 threads)
{
    if (IsRunning() || !_port.Initialize())
    {
        return false;
    }
    const uint32 count = threads != 0 ? threads : 1;
    _threadIds.Resize(count);
    _registering.Store(0);
    _started.Store(0);
    for (uint32 i = 0; i < count; ++i)
    {
        ThreadDesc desc;
        desc.Name = "Marlin Net";
        desc.Entry = &ThreadMain;
        desc.Argument = this;
        desc.Priority = ThreadPriority::AboveNormal;
        const ThreadHandle thread = PlatformThread::Create(desc);
        if (!thread.IsValid())
        {
            Stop();
            return false;
        }
        _threads.Add(thread);
    }
    while (_started.Load(MemoryOrder::Acquire) != count)
    {
        PlatformThread::YieldThread();
    }
    return true;
}

void NetLoop::Stop()
{
    if (_threads.IsEmpty())
    {
        _port.Shutdown();
        return;
    }
    List<IoOperation> stops;
    stops.Resize(_threads.Count());
    for (IoOperation &stop : stops)
    {
        (void)_port.Post(stop);
    }
    for (const ThreadHandle &thread : _threads)
    {
        PlatformThread::Join(thread);
    }
    _threads.Clear();
    _threadIds.Clear();
    _port.Shutdown();
}

void NetLoop::Post(Function<void()> work)
{
    auto *posted = new PostedWork(Move(work));
    if (!_port.Post(posted->Operation))
    {
        posted->Complete(posted->Operation);
    }
}

bool NetLoop::IsIoThread() const
{
    const uint32 current = PlatformThread::CurrentId();
    for (const uint32 id : _threadIds)
    {
        if (id == current)
        {
            return true;
        }
    }
    return false;
}

void NetLoop::ThreadMain(void *argument)
{
    static_cast<NetLoop *>(argument)->Run();
}

void NetLoop::Run()
{
    _threadIds[_registering.FetchAdd(1, MemoryOrder::Relaxed)] = PlatformThread::CurrentId();
    _started.FetchAdd(1, MemoryOrder::Release);
    IoOperation *completed[64];
    bool stopping = false;
    while (!stopping)
    {
        const uint32 count = _port.Wait(completed, 64, ~0u);
        for (uint32 i = 0; i < count; ++i)
        {
            IoOperation &operation = *completed[i];
            if (operation.Context != nullptr)
            {
                static_cast<INetCompletion *>(operation.Context)->Complete(operation);
            }
            else if (stopping)
            {
                (void)_port.Post(operation);
            }
            else
            {
                stopping = true;
            }
        }
    }
}
} // namespace Marlin
