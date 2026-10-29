#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformSync.h"
#include "Json/JsonValue.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Net/NetListener.h"
#include "Net/NetLoop.h"
#include "Reflection/TypeInfo.h"
#include "Remote/RemoteConnection.h"
#include "String/String.h"
#include "Templates/Function.h"
#include "Remote/RemoteServer.gen.h"

MARLIN_DECLARE_LOG_CATEGORY(REMOTE_API, LogRemote, Info, Warning);

namespace Marlin
{
class JsonWriter;
class RemoteServer;
struct RemoteRequest;

/// Bytes sent beside a result instead of inside it, such as a screenshot; the client reads them by Attachment.
MSTRUCT()
struct RemoteAttachment
{
    MGENERATED_BODY()

    MPROPERTY()
    uint64 Attachment = 0;
    MPROPERTY()
    uint64 Size = 0;
};

/// A response a handler promised with RemoteCall::Defer; complete it once, from any thread.
struct RemoteDeferred
{
    uint32 ConnectionId = 0;
    /// The request id as JSON.
    String Id;
};

/// The call in progress. A Remote method that takes a `RemoteCall &` parameter can fail with a message, attach
/// bytes, or see which connection called; native handlers set their result through it.
MSTRUCT()
struct REMOTE_API RemoteCall
{
    MGENERATED_BODY()
public:
    RemoteCall() = default;
    ~RemoteCall() = default;
    MARLIN_NONCOPYABLE(RemoteCall);

    [[nodiscard]] const JsonValue &GetParams() const;
    [[nodiscard]] uint32 GetConnectionId() const;
    [[nodiscard]] RemoteServer &GetServer() const;

    void Fail(StringView message, int32 code = RemoteErrors::Failed);
    [[nodiscard]] bool HasFailed() const
    {
        return _errorCode != 0;
    }

    /// Sends *bytes* before the response.
    RemoteAttachment Attach(List<uint8> bytes);

    /// Answers later through RemoteServer::Complete, such as after a conversion; the handler returns at once.
    /// Inside a batch the deferred response arrives on its own, after the batch's.
    RemoteDeferred Defer();

    /// The result of a native handler: a value, or JSON text written by the handler.
    void SetResult(const JsonValue &result);
    void SetResultJson(StringView json);

private:
    friend class RemoteServer;
    friend struct RemoteDispatch;

    RemoteServer *_server = nullptr;
    RemoteConnection *_connection = nullptr;
    const JsonValue *_params = nullptr;
    String _id;
    String _result;
    String _errorMessage;
    int32 _errorCode = 0;
    bool _deferred = false;
};

using RemoteHandler = Function<void(RemoteCall &call)>;

/// Where a Remote method runs.
enum class RemoteThread : uint8
{
    /// Between frames, on the main thread: safe with engine state.
    Main,
    /// As a job as soon as it arrives, after the connection's earlier calls.
    Any,
};

struct RemoteServerDesc
{
    /// The named pipe to listen on, local to the machine; empty for none.
    String PipeName;
    /// Listen on TCP for stream framing and WebSocket, at Port; port zero takes any free one.
    bool ListenTcp = false;
    uint16 Port = 0;
    /// Listen on every interface instead of loopback; needs a token.
    bool AllowRemote = false;
    /// What TCP and WebSocket clients pass to Remote.Authenticate; pipes need none.
    String Token;
};

/// Serves JSON-RPC 2.0 (Documentation/Architecture/10): the `MFUNCTION(Remote)` methods of reflected types, named
/// "Category.Function", plus handlers added in code, and event streams clients subscribe to. Methods run between
/// frames unless marked `MFUNCTION(Remote = AnyThread)`; whoever owns the server calls ProcessPending each frame.
class REMOTE_API RemoteServer
{
public:
    RemoteServer(NetLoop &loop, RemoteServerDesc desc);
    ~RemoteServer();
    MARLIN_NONCOPYABLE(RemoteServer);

    /// Collects the reflected methods and starts listening; false if a listener could not start.
    bool Start();
    void Stop();

    /// Adds or replaces a native method.
    void AddMethod(StringView name, RemoteHandler handler, RemoteThread thread = RemoteThread::Main,
                   StringView description = {});
    /// Makes *stream* known to Remote.ListStreams; publishing works without it.
    void AddStream(StringView stream, StringView description);

    /// Runs the calls waiting for the main thread.
    void ProcessPending();

    /// Sends the result of a deferred call; nothing happens if its connection closed.
    void Complete(const RemoteDeferred &deferred, StringView resultJson);
    void Fail(const RemoteDeferred &deferred, StringView message, int32 code = RemoteErrors::Failed);

    /// Sends a notification to connection *id* alone, such as progress on one of its deferred calls; nothing happens
    /// if it closed. From any thread.
    void Notify(uint32 id, StringView method, StringView params);

    /// Sends a notification to the connections subscribed to *stream*; from any thread.
    void Publish(StringView stream, const JsonValue &params);
    /// The same with params as JSON text.
    void PublishJson(StringView stream, StringView params);
    /// One notification for each of *params*, in order, finding the subscribers once.
    void PublishJson(StringView stream, Span<const StringView> params);
    [[nodiscard]] bool HasSubscribers(StringView stream) const;

    /// The TCP port actually bound, zero if none.
    [[nodiscard]] uint16 GetPort() const
    {
        return _boundPort;
    }

    [[nodiscard]] StringView GetPipeName() const
    {
        return _desc.PipeName;
    }

    [[nodiscard]] usize GetConnectionCount() const;
    /// False once connection *id* has closed.
    [[nodiscard]] bool IsConnected(uint32 id) const;

private:
    friend struct RemoteDispatch;
    friend struct RemoteCall;

    struct HandlerBox final : RefCounted
    {
        explicit HandlerBox(RemoteHandler handler) : Handler(Move(handler))
        {
        }

        RemoteHandler Handler;
    };

    struct Method
    {
        SharedPtr<HandlerBox> Handler;
        RemoteThread Thread = RemoteThread::Main;
        String Description;
        const FunctionInfo *Function = nullptr;
    };

    struct Peer
    {
        SharedPtr<RemoteConnection> Connection;
        bool Authenticated = false;
        List<String> Subscriptions;
        uint64 NextAttachment = 1;
    };

    struct Pending
    {
        SharedPtr<RemoteConnection> Connection;
        JsonValue Request;
    };

    void Accept(SharedPtr<NetStream> stream, UniquePtr<IRemoteFraming> framing);
    void OnFrame(RemoteConnection &connection, RemoteFrame &frame);
    void OnClose(RemoteConnection &connection);
    /// Handles one request; writes the response to *output* unless it is a notification. False if none.
    bool Handle(RemoteConnection &connection, const JsonValue &request, JsonWriter &output);
    void Respond(RemoteConnection &connection, const JsonValue &request, RemoteThread thread);
    void AddBuiltins();
    void AddReflectedMethods();
    void WriteMethodList(JsonWriter &writer) const;
    SharedPtr<RemoteConnection> FindConnection(uint32 id) const;

    NetLoop &_loop;
    RemoteServerDesc _desc;
    mutable Mutex _lock;
    HashMap<String, Method> _methods;
    HashMap<String, String> _streams;
    HashMap<uint32, Peer> _peers;
    List<Pending> _pending;
    UniquePtr<NetListener> _pipeListener;
    UniquePtr<NetListener> _tcpListener;
    uint16 _boundPort = 0;
    bool _started = false;
};
} // namespace Marlin
