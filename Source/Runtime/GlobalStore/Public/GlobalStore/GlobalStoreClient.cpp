#include "GlobalStore/GlobalStoreClient.h"

#include "Concurrency/Atomic.h"
#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Assert.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformThread.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"

namespace Marlin
{
struct GlobalStoreClient::Connection : RefCounted
{
    SharedPtr<NetStream> Stream;
    Mutex Lock;
    List<GlobalStoreCallback> Pending;
    usize Head = 0;
    uint32 NextId = 1;
    bool Closed = false;

    usize Receive(NetStream &stream, Span<const uint8> bytes)
    {
        usize consumed = 0;
        while (bytes.Count() - consumed >= sizeof(GlobalStoreResponse))
        {
            GlobalStoreResponse response;
            MemCopy(&response, bytes.Data() + consumed, sizeof(response));
            if (response.Magic != GlobalStoreResponseMagic || response.Size > GlobalStoreMaxValue)
            {
                stream.Abort();
                return bytes.Count();
            }
            if (bytes.Count() - consumed - sizeof(response) < response.Size)
            {
                break;
            }
            GlobalStoreReply reply;
            reply.Status = response.Status;
            reply.TotalSize = response.TotalSize;
            reply.Bytes.AddRange(bytes.Slice(consumed + sizeof(response), static_cast<usize>(response.Size)));
            consumed += sizeof(response) + static_cast<usize>(response.Size);
            GlobalStoreCallback done = Pop();
            if (done)
            {
                done(reply);
            }
        }
        return consumed;
    }

    GlobalStoreCallback Pop()
    {
        const ScopeLock lock(Lock);
        if (Head == Pending.Count())
        {
            return {};
        }
        GlobalStoreCallback done = Move(Pending[Head++]);
        if (Head == Pending.Count())
        {
            Pending.Clear();
            Head = 0;
        }
        else if (Head > 1024 && Head * 2 > Pending.Count())
        {
            Pending.RemoveAt(0, Head);
            Head = 0;
        }
        return done;
    }

    void Fail()
    {
        List<GlobalStoreCallback> failed;
        {
            const ScopeLock lock(Lock);
            Closed = true;
            for (usize i = Head; i < Pending.Count(); ++i)
            {
                failed.Add(Move(Pending[i]));
            }
            Pending.Clear();
            Head = 0;
        }
        for (GlobalStoreCallback &done : failed)
        {
            GlobalStoreReply reply;
            done(reply);
        }
    }
};

namespace
{
struct Waiter
{
    Event Done{EventReset::Manual};
    GlobalStoreReply Reply;
};
} // namespace

GlobalStoreClient::GlobalStoreClient(NetLoop &loop) : _loop(loop)
{
}

GlobalStoreClient::~GlobalStoreClient()
{
    Disconnect();
}

bool GlobalStoreClient::Connect(StringView host, uint16 port, uint32 timeout)
{
    Disconnect();
    SharedPtr<NetStream> stream = NetStream::ConnectTcp(_loop, host, port, timeout);
    if (!stream.IsValid())
    {
        return false;
    }
    SharedPtr<Connection> connection(new Connection());
    connection->Stream = stream;
    NetStreamHandlers handlers;
    handlers.OnReceive = [connection](NetStream &self, Span<const uint8> bytes) {
        return connection->Receive(self, bytes);
    };
    handlers.OnClose = [connection](NetStream & /*self*/) { connection->Fail(); };
    {
        const ScopeLock lock(_lock);
        _connection = connection;
    }
    stream->Start(Move(handlers));
    return true;
}

bool GlobalStoreClient::ConnectLocal(StringView executableDirectory, uint16 port, uint32 timeout)
{
    constexpr StringView LocalHost = "127.0.0.1";
    // A running store answers on loopback at once; Windows takes about 2 s to refuse a port nobody listens on.
    constexpr uint32 ProbeMilliseconds = 200;
    if (Connect(LocalHost, port, ProbeMilliseconds))
    {
        return true;
    }
    // Another process may be starting it at the same moment; its lock lets only one of them serve.
    const String executable =
        Format("{0}/MarlinGlobalStore{1}", executableDirectory, MARLIN_PLATFORM_WINDOWS ? ".exe" : "");
    ProcessDesc desc;
    desc.Executable = executable.CStr();
    desc.Arguments = "-ExitWhenIdle";
    desc.Flags = ProcessFlags::Hidden;
    ProcessHandle process;
    if (!PlatformProcess::Create(desc, process))
    {
        return false;
    }
    PlatformProcess::Close(process);
    for (uint32 waited = 0; waited < timeout; waited += 50)
    {
        if (Connect(LocalHost, port, 200))
        {
            return true;
        }
        PlatformThread::Sleep(50);
    }
    return false;
}

void GlobalStoreClient::Disconnect()
{
    SharedPtr<Connection> connection;
    {
        const ScopeLock lock(_lock);
        connection = Move(_connection);
        _connection = {};
    }
    if (connection.IsValid())
    {
        connection->Stream->Abort();
    }
}

bool GlobalStoreClient::IsConnected() const
{
    const ScopeLock lock(_lock);
    if (!_connection.IsValid())
    {
        return false;
    }
    const ScopeLock connectionLock(_connection->Lock);
    return !_connection->Closed;
}

void GlobalStoreClient::Request(GlobalStoreOp op, const Hash128 &key, uint64 offset, uint64 size,
                                Span<const uint8> payload, GlobalStoreCallback done)
{
    List<uint8> bytes;
    bytes.Reserve(PutHeaderSize + payload.Count());
    bytes.ResizeUninitialized(PutHeaderSize);
    bytes.AddRange(payload);
    Send(op, key, offset, size, Move(bytes), Move(done));
}

void GlobalStoreClient::Send(GlobalStoreOp op, const Hash128 &key, uint64 offset, uint64 size, List<uint8> bytes,
                             GlobalStoreCallback done)
{
    MARLIN_ASSERT(bytes.Count() >= PutHeaderSize);
    SharedPtr<Connection> connection;
    {
        const ScopeLock lock(_lock);
        connection = _connection;
    }
    if (connection.IsValid())
    {
        const ScopeLock lock(connection->Lock);
        if (!connection->Closed)
        {
            GlobalStoreRequest request;
            request.Op = op;
            request.Id = connection->NextId++;
            request.Key = key;
            request.Offset = offset;
            request.Size = op == GlobalStoreOp::Put ? bytes.Count() - PutHeaderSize : size;
            MemCopy(bytes.Data(), &request, sizeof(request));
            connection->Pending.Add(Move(done));
            connection->Stream->Send(Move(bytes));
            return;
        }
    }
    GlobalStoreReply reply;
    done(reply);
}

void GlobalStoreClient::GetAsync(const Hash128 &key, uint64 offset, uint64 size, GlobalStoreCallback done)
{
    Request(GlobalStoreOp::Get, key, offset, size, {}, Move(done));
}

void GlobalStoreClient::PutAsync(const Hash128 &key, Span<const uint8> value, GlobalStoreCallback done)
{
    if (value.Count() > GlobalStoreMaxValue)
    {
        GlobalStoreReply reply;
        done(reply);
        return;
    }
    Request(GlobalStoreOp::Put, key, 0, 0, value, Move(done));
}

void GlobalStoreClient::PutAsync(const Hash128 &key, List<uint8> framed, GlobalStoreCallback done)
{
    if (framed.Count() < PutHeaderSize || framed.Count() - PutHeaderSize > GlobalStoreMaxValue)
    {
        GlobalStoreReply reply;
        done(reply);
        return;
    }
    Send(GlobalStoreOp::Put, key, 0, 0, Move(framed), Move(done));
}

void GlobalStoreClient::HasAsync(const Hash128 &key, GlobalStoreCallback done)
{
    Request(GlobalStoreOp::Has, key, 0, 0, {}, Move(done));
}

bool GlobalStoreClient::Get(const Hash128 &key, List<uint8> &value, uint64 offset, uint64 size)
{
    MARLIN_ASSERT(!_loop.IsIoThread());
    Waiter waiter;
    GetAsync(key, offset, size, [&waiter](GlobalStoreReply &reply) {
        waiter.Reply = Move(reply);
        waiter.Done.Set();
    });
    (void)waiter.Done.Wait();
    value = Move(waiter.Reply.Bytes);
    return waiter.Reply.Status == GlobalStoreStatus::Ok;
}

bool GlobalStoreClient::Put(const Hash128 &key, Span<const uint8> value)
{
    MARLIN_ASSERT(!_loop.IsIoThread());
    Waiter waiter;
    PutAsync(key, value, [&waiter](GlobalStoreReply &reply) {
        waiter.Reply.Status = reply.Status;
        waiter.Done.Set();
    });
    (void)waiter.Done.Wait();
    return waiter.Reply.Status == GlobalStoreStatus::Ok;
}

bool GlobalStoreClient::Has(const Hash128 &key, uint64 *size)
{
    MARLIN_ASSERT(!_loop.IsIoThread());
    Waiter waiter;
    HasAsync(key, [&waiter](GlobalStoreReply &reply) {
        waiter.Reply.Status = reply.Status;
        waiter.Reply.TotalSize = reply.TotalSize;
        waiter.Done.Set();
    });
    (void)waiter.Done.Wait();
    if (size != nullptr)
    {
        *size = waiter.Reply.TotalSize;
    }
    return waiter.Reply.Status == GlobalStoreStatus::Ok;
}

bool GlobalStoreClient::GetMany(Span<const Hash128> keys, List<List<uint8>> &values, List<bool> &found)
{
    return GetMany(keys, 0, GlobalStoreWholeValue, values, found);
}

bool GlobalStoreClient::GetMany(Span<const Hash128> keys, uint64 offset, uint64 size, List<List<uint8>> &values,
                                List<bool> &found)
{
    MARLIN_ASSERT(!_loop.IsIoThread());
    values.Clear();
    values.Resize(keys.Count());
    found.Clear();
    found.Resize(keys.Count());
    if (keys.IsEmpty())
    {
        return IsConnected();
    }
    Event done{EventReset::Manual};
    Atomic<usize> remaining{keys.Count()};
    Atomic<bool> failed{false};
    for (usize i = 0; i < keys.Count(); ++i)
    {
        GetAsync(keys[i], offset, size, [&, i](GlobalStoreReply &reply) {
            found[i] = reply.Status == GlobalStoreStatus::Ok;
            values[i] = Move(reply.Bytes);
            if (reply.Status == GlobalStoreStatus::Error)
            {
                failed.Store(true);
            }
            if (remaining.FetchSub(1) == 1)
            {
                done.Set();
            }
        });
    }
    (void)done.Wait();
    return !failed.Load();
}

bool GlobalStoreClient::HasMany(Span<const Hash128> keys, List<bool> &found)
{
    MARLIN_ASSERT(!_loop.IsIoThread());
    found.Clear();
    found.Resize(keys.Count());
    if (keys.IsEmpty())
    {
        return IsConnected();
    }
    Event done{EventReset::Manual};
    Atomic<usize> remaining{keys.Count()};
    Atomic<bool> failed{false};
    for (usize i = 0; i < keys.Count(); ++i)
    {
        HasAsync(keys[i], [&, i](GlobalStoreReply &reply) {
            found[i] = reply.Status == GlobalStoreStatus::Ok;
            if (reply.Status == GlobalStoreStatus::Error)
            {
                failed.Store(true);
            }
            if (remaining.FetchSub(1) == 1)
            {
                done.Set();
            }
        });
    }
    (void)done.Wait();
    return !failed.Load();
}
} // namespace Marlin
