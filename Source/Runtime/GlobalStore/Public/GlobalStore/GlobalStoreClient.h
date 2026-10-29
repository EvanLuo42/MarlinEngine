#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "GlobalStore/GlobalStoreProtocol.h"
#include "HAL/PlatformSync.h"
#include "Memory/SharedPtr.h"
#include "Net/NetLoop.h"
#include "Net/NetStream.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
/// The answer to one request: Missing for an absent key, Error if the connection failed first. For a Get, Bytes holds
/// the part asked for and TotalSize the size of the whole value.
struct GlobalStoreReply
{
    GlobalStoreStatus Status = GlobalStoreStatus::Error;
    uint64 TotalSize = 0;
    List<uint8> Bytes;
};

using GlobalStoreCallback = Function<void(GlobalStoreReply &reply)>;

/// One connection to a GlobalStore server over Net (Documentation/Architecture/14 §6). Requests are pipelined: each
/// goes out at once and its callback runs on an IO thread when its answer arrives, in request order. The blocking
/// calls wait for those callbacks and must not be made on an IO thread. Safe to share between threads.
class GLOBALSTORE_API GlobalStoreClient
{
public:
    /// The bytes a framed PutAsync leaves at the front of its buffer for the request header.
    static constexpr usize PutHeaderSize = sizeof(GlobalStoreRequest);

    explicit GlobalStoreClient(NetLoop &loop);
    ~GlobalStoreClient();
    MARLIN_NONCOPYABLE(GlobalStoreClient);

    bool Connect(StringView host, uint16 port, uint32 timeout = 2000);
    /// Connects to the GlobalStore on this machine, starting MarlinGlobalStore from *executableDirectory* (hidden, with
    /// -ExitWhenIdle) when none answers on *port* and waiting up to *timeout* ms for it (06 §3.3).
    bool ConnectLocal(StringView executableDirectory, uint16 port, uint32 timeout = 5000);
    void Disconnect();
    [[nodiscard]] bool IsConnected() const;

    /// Reads *size* bytes from *offset*, or the rest of the value.
    void GetAsync(const Hash128 &key, uint64 offset, uint64 size, GlobalStoreCallback done);
    void PutAsync(const Hash128 &key, Span<const uint8> value, GlobalStoreCallback done);
    /// PutAsync without copying the value: *framed* is PutHeaderSize bytes this fills in, then the value, and is
    /// sent as is.
    void PutAsync(const Hash128 &key, List<uint8> framed, GlobalStoreCallback done);
    void HasAsync(const Hash128 &key, GlobalStoreCallback done);

    /// False if the key is missing.
    bool Get(const Hash128 &key, List<uint8> &value, uint64 offset = 0, uint64 size = GlobalStoreWholeValue);
    bool Put(const Hash128 &key, Span<const uint8> value);
    /// True if the key exists; *size* is its size.
    bool Has(const Hash128 &key, uint64 *size = nullptr);
    /// Reads many whole values in one pipelined batch; false if the connection failed.
    bool GetMany(Span<const Hash128> keys, List<List<uint8>> &values, List<bool> &found);
    /// GetMany of the same part of every value: *size* bytes from *offset*, such as each converted result's header.
    bool GetMany(Span<const Hash128> keys, uint64 offset, uint64 size, List<List<uint8>> &values, List<bool> &found);
    bool HasMany(Span<const Hash128> keys, List<bool> &found);

private:
    struct Connection;

    void Request(GlobalStoreOp op, const Hash128 &key, uint64 offset, uint64 size, Span<const uint8> payload,
                 GlobalStoreCallback done);
    /// Sends *bytes*, whose first PutHeaderSize bytes this fills with the request header.
    void Send(GlobalStoreOp op, const Hash128 &key, uint64 offset, uint64 size, List<uint8> bytes,
              GlobalStoreCallback done);

    NetLoop &_loop;
    mutable Mutex _lock;
    SharedPtr<Connection> _connection;
};
} // namespace Marlin
