#include "Remote/RemoteConnection.h"

#include "Concurrency/Jobs.h"
#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Assert.h"
#include "HAL/PlatformTime.h"

namespace Marlin
{
namespace
{
Atomic<uint32> _nextConnectionId{1};

uint64 Milliseconds()
{
    return PlatformTime::Ticks() * 1000 / PlatformTime::TicksPerSecond();
}
} // namespace

RemoteConnection::RemoteConnection(SharedPtr<NetStream> stream, UniquePtr<IRemoteFraming> framing,
                                   RemoteFrameHandler onFrame, RemoteCloseHandler onClose)
    : _stream(Move(stream)), _framing(Move(framing)), _onFrame(Move(onFrame)), _onClose(Move(onClose)),
      _startTime(Milliseconds()), _id(_nextConnectionId.FetchAdd(1))
{
    _idle.Set();
}

RemoteConnection::~RemoteConnection() = default;

void RemoteConnection::Start()
{
    if (IsReady())
    {
        _ready.Set();
    }
    const SharedPtr<RemoteConnection> self(this);
    NetStreamHandlers handlers;
    handlers.OnReceive = [self](NetStream & /*stream*/, Span<const uint8> bytes) { return self->Receive(bytes); };
    handlers.OnClose = [self](NetStream & /*stream*/) { self->Closed(); };
    _stream->Start(Move(handlers));
}

usize RemoteConnection::Receive(Span<const uint8> bytes)
{
    List<RemoteFrame> frames;
    List<uint8> reply;
    usize consumed = 0;
    bool ok = false;
    bool ready = false;
    {
        const ScopeLock lock(_framingLock);
        ok = _framing->Decode(bytes, consumed, frames, reply);
        ready = _framing->IsReady();
    }
    if (!reply.IsEmpty())
    {
        _stream->Send(Move(reply));
    }
    if (ready)
    {
        _ready.Set();
    }
    for (RemoteFrame &frame : frames)
    {
        _onFrame(*this, frame);
    }
    if (!ok)
    {
        _stream->Close();
        return bytes.Count();
    }
    return consumed;
}

void RemoteConnection::Closed()
{
    _open.Store(false, MemoryOrder::Release);
    _ready.Set();
    if (_onClose)
    {
        _onClose(*this);
    }
    _closed.Set();
}

void RemoteConnection::Send(const RemoteFrame &frame)
{
    if (!IsOpen())
    {
        return;
    }
    List<uint8> bytes;
    const ScopeLock lock(_framingLock);
    _framing->Encode(frame, bytes);
    _stream->Send(Move(bytes));
}

void RemoteConnection::SendText(StringView text)
{
    Send(RemoteFrame::FromText(text));
}

void RemoteConnection::SendRaw(List<uint8> bytes)
{
    _stream->Send(Move(bytes));
}

void RemoteConnection::Close()
{
    List<uint8> bytes;
    {
        const ScopeLock lock(_framingLock);
        if (_framing->IsReady())
        {
            _framing->EncodeClose(bytes);
        }
    }
    if (!bytes.IsEmpty())
    {
        _stream->Send(Move(bytes));
    }
    _stream->Close();
}

void RemoteConnection::WaitClosed()
{
    MARLIN_ASSERT(!_stream->GetLoop().IsIoThread());
    (void)_closed.Wait();
    (void)_idle.Wait();
}

bool RemoteConnection::WaitReady(uint32 timeout)
{
    (void)_ready.Wait(timeout);
    return IsOpen() && IsReady();
}

bool RemoteConnection::IsReady() const
{
    const ScopeLock lock(_framingLock);
    return _framing->IsReady();
}

void RemoteConnection::Dispatch(Function<void()> work)
{
    {
        const ScopeLock lock(_dispatchLock);
        _dispatched.Add(Move(work));
        if (_dispatching)
        {
            return;
        }
        _dispatching = true;
        _idle.Reset();
    }
    AddReference();
    JobDecl job;
    job.Entry = &RunDispatched;
    job.Argument = this;
    job.LargeStack = true;
    Jobs::Run(job);
}

void RemoteConnection::RunDispatched(void *argument)
{
    auto *self = static_cast<RemoteConnection *>(argument);
    while (true)
    {
        // Takes everything queued at once and runs it in order; only the one dispatching job touches _running.
        {
            const ScopeLock lock(self->_dispatchLock);
            if (self->_dispatched.IsEmpty())
            {
                self->_dispatching = false;
                self->_idle.Set();
                break;
            }
            Swap(self->_running, self->_dispatched);
        }
        for (Function<void()> &work : self->_running)
        {
            work();
        }
        self->_running.Clear();
    }
    if (self->ReleaseReference())
    {
        delete self;
    }
}
} // namespace Marlin
