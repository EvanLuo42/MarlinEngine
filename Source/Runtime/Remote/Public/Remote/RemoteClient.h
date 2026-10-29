#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformSync.h"
#include "Json/JsonValue.h"
#include "Memory/SharedPtr.h"
#include "Net/NetLoop.h"
#include "Remote/RemoteConnection.h"
#include "String/String.h"
#include "Templates/Function.h"

namespace Marlin
{
/// The outcome of a call: a result, or a JSON-RPC error, with the attachments the result refers to.
struct RemoteResponse
{
    bool Succeeded = false;
    JsonValue Result;
    int32 ErrorCode = 0;
    String ErrorMessage;
    HashMap<uint64, List<uint8>> Attachments;

    /// The bytes of a RemoteAttachment in the result, or null.
    [[nodiscard]] const List<uint8> *FindAttachment(uint64 id) const
    {
        return Attachments.Find(id);
    }
};

using RemoteResponseHandler = Function<void(RemoteResponse &response)>;
using RemoteNotificationHandler = Function<void(const JsonValue &params)>;

/// Calls a RemoteServer: an engine process, the asset indexer or any other program that serves one. Handlers of
/// asynchronous calls and notifications run in ProcessPending, on the thread that calls it.
class REMOTE_API RemoteClient
{
public:
    explicit RemoteClient(NetLoop &loop);
    ~RemoteClient();
    MARLIN_NONCOPYABLE(RemoteClient);

    bool ConnectPipe(StringView name, uint32 timeout = 2000);
    bool ConnectTcp(StringView host, uint16 port, uint32 timeout = 2000);
    bool ConnectWebSocket(StringView host, uint16 port, uint32 timeout = 2000);
    void Disconnect();
    [[nodiscard]] bool IsConnected() const;
    /// The pipe of the last ConnectPipe that succeeded; empty after connecting another way.
    [[nodiscard]] StringView GetPipeName() const
    {
        return _pipeName.AsView();
    }

    void Call(StringView method, const JsonValue &params, RemoteResponseHandler handler);
    /// Like Call, but *handler* runs on an IO thread as soon as the response arrives, without ProcessPending; it must
    /// be short. For callers with no thread of their own to pump, such as an asset store.
    void CallAsync(StringView method, const JsonValue &params, RemoteResponseHandler handler);
    /// Blocks for the response; false on timeout or a lost connection, with the reason in *response*.
    bool CallSync(StringView method, const JsonValue &params, RemoteResponse &response, uint32 timeout = 10000);
    /// Sends a notification, which gets no response.
    void Notify(StringView method, const JsonValue &params);
    /// Notify with *params* already written as JSON text.
    void NotifyJson(StringView method, StringView params);
    /// Handles the notifications of *method*, such as a stream subscribed with Remote.Subscribe.
    void OnNotification(StringView method, RemoteNotificationHandler handler);

    void ProcessPending();

private:
    struct Waiting
    {
        RemoteResponseHandler Handler;
        Event *Done = nullptr;
        RemoteResponse *Response = nullptr;
        /// Run the handler on the IO thread as soon as the response arrives.
        bool Immediate = false;
    };

    struct Ready
    {
        RemoteResponseHandler Handler;
        RemoteResponse Response;
    };

    struct HandlerBox final : RefCounted
    {
        explicit HandlerBox(RemoteNotificationHandler handler) : Handler(Move(handler))
        {
        }

        RemoteNotificationHandler Handler;
    };

    struct ReadyNotification
    {
        String Method;
        JsonValue Params;
    };

    bool Attach(SharedPtr<NetStream> stream, UniquePtr<IRemoteFraming> framing, List<uint8> request, uint32 timeout);
    void OnFrame(RemoteFrame &frame);
    void OnClose();
    uint64 Send(StringView method, const JsonValue &params, Waiting waiting);
    static void CollectAttachments(const JsonValue &value, List<uint64> &ids);
    [[nodiscard]] SharedPtr<RemoteConnection> GetConnection() const;

    NetLoop &_loop;
    SharedPtr<RemoteConnection> _connection;
    mutable Mutex _lock;
    uint64 _nextId = 1;
    HashMap<uint64, Waiting> _waiting;
    HashMap<uint64, List<uint8>> _attachments;
    List<Ready> _ready;
    List<ReadyNotification> _notifications;
    HashMap<String, SharedPtr<HandlerBox>> _handlers;
    String _pipeName;
};
} // namespace Marlin
