#include "Remote/RemoteServer.h"

#include "Concurrency/ScopeLock.h"
#include "Engine/Engine.h"
#include "Engine/Subsystem.h"
#include "HAL/PlatformTime.h"
#include "Json/JsonSerializer.h"
#include "Json/JsonWriter.h"
#include "Memory/Memory.h"
#include "Reflection/Reflection.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

MARLIN_DEFINE_LOG_CATEGORY(LogRemote);

namespace Marlin
{
namespace
{
constexpr uint64 HandshakeTimeout = 2000;
} // namespace

const JsonValue &RemoteCall::GetParams() const
{
    return *_params;
}

uint32 RemoteCall::GetConnectionId() const
{
    return _connection->GetId();
}

RemoteServer &RemoteCall::GetServer() const
{
    return *_server;
}

void RemoteCall::Fail(StringView message, int32 code)
{
    if (_errorCode == 0)
    {
        _errorCode = code;
        _errorMessage = message;
    }
}

RemoteAttachment RemoteCall::Attach(List<uint8> bytes)
{
    RemoteAttachment attachment;
    attachment.Size = bytes.Count();
    {
        const ScopeLock lock(_server->_lock);
        RemoteServer::Peer *peer = _server->_peers.Find(_connection->GetId());
        attachment.Attachment = peer != nullptr ? peer->NextAttachment++ : 0;
    }
    RemoteFrame frame;
    frame.IsAttachment = true;
    frame.AttachmentId = attachment.Attachment;
    frame.Bytes = Move(bytes);
    _connection->Send(Move(frame));
    return attachment;
}

RemoteDeferred RemoteCall::Defer()
{
    _deferred = true;
    return {_connection->GetId(), _id};
}

void RemoteCall::SetResult(const JsonValue &result)
{
    _result = result.ToString();
}

void RemoteCall::SetResultJson(StringView json)
{
    _result = json;
}

/// Calls reflected functions from JSON-RPC params.
struct RemoteDispatch
{
    /// Storage for one argument or the return value.
    struct Slot
    {
        const TypeInfo *Type = nullptr;
        void *Data = nullptr;
        bool Constructed = false;

        Slot() = default;
        MARLIN_NONCOPYABLE(Slot);

        ~Slot()
        {
            if (Constructed)
            {
                Type->GetOperations().Destruct(Data);
            }
            Memory::Free(Data);
        }

        void Allocate(const TypeInfo &type)
        {
            Type = &type;
            Data = Memory::Allocate(type.GetSize() != 0 ? type.GetSize() : 1, type.GetAlignment());
        }

        bool Construct()
        {
            if (Type->GetOperations().Construct == nullptr)
            {
                return false;
            }
            Type->GetOperations().Construct(Data);
            Constructed = true;
            return true;
        }
    };

    /// Parameters are camelCase in C++ and PascalCase in JSON, like fields.
    static String JsonName(StringView name)
    {
        String text(name);
        if (!text.IsEmpty())
        {
            text.Data()[0] = Ascii::ToUpper(text.Data()[0]);
        }
        return text;
    }

    static StringView Unquote(StringView text)
    {
        text = text.Trim();
        return text.Length() >= 2 && text.First() == '"' && text.Last() == '"' ? text.Substring(1, text.Length() - 2)
                                                                               : text;
    }

    static void Invoke(RemoteCall &call, const FunctionInfo &function)
    {
        void *object = nullptr;
        if (!function.IsStatic())
        {
            const SubsystemCollection *subsystems = Engine::GetSubsystems();
            Subsystem *subsystem = subsystems != nullptr ? subsystems->Find(function.GetOwner()) : nullptr;
            if (subsystem == nullptr)
            {
                call.Fail(Format("{0} is not running", function.GetOwner().GetName()));
                return;
            }
            object = TypeOf<Subsystem>().CastTo(subsystem, function.GetOwner());
        }
        const JsonValue &params = call.GetParams();
        if (!params.IsNull() && !params.IsObject() && !params.IsArray())
        {
            call.Fail("params must be an object or an array", RemoteErrors::InvalidParams);
            return;
        }
        const Span<const ParameterInfo> parameters = function.GetParameters();
        List<UniquePtr<Slot>> slots;
        List<void *> arguments;
        usize position = 0;
        for (const ParameterInfo &parameter : parameters)
        {
            const TypeInfo &type = parameter.Type();
            if (&type == &TypeOf<RemoteCall>())
            {
                arguments.Add(&call);
                continue;
            }
            UniquePtr<Slot> &slot = slots.Add(MakeUnique<Slot>());
            slot->Allocate(type);
            if (!slot->Construct())
            {
                call.Fail(Format("parameter {0} cannot be constructed", parameter.Name), RemoteErrors::InternalError);
                return;
            }
            arguments.Add(slot->Data);
            const JsonValue *value = params.IsObject() ? params.Find(JsonName(parameter.Name))
                                     : params.IsArray() && position < params.Count() ? &params.GetElements()[position]
                                                                                     : nullptr;
            ++position;
            if (value == nullptr)
            {
                if (!parameter.DefaultValue.IsEmpty())
                {
                    (void)Reflection::ParseValue(Unquote(parameter.DefaultValue), type, slot->Data);
                }
                else if (!parameter.IsOutput())
                {
                    call.Fail(Format("missing parameter {0}", JsonName(parameter.Name)), RemoteErrors::InvalidParams);
                    return;
                }
                continue;
            }
            StringBuilder<256> error;
            if (!Json::ReadValue(*value, type, slot->Data, error))
            {
                call.Fail(Format("parameter {0}: {1}", parameter.Name, error.AsView()), RemoteErrors::InvalidParams);
                return;
            }
        }
        if (params.IsArray() && params.Count() > position)
        {
            call.Fail(Format("{0} takes {1} parameters", function.GetName(), position), RemoteErrors::InvalidParams);
            return;
        }

        Slot result;
        const TypeInfo *returnType = function.GetReturnType();
        if (returnType != nullptr)
        {
            result.Allocate(*returnType);
        }
        function.Invoke(object, arguments.Data(), result.Data);
        result.Constructed = returnType != nullptr;
        if (call.HasFailed())
        {
            return;
        }

        StringBuilder<1024> text;
        JsonWriter writer(text);
        bool outputs = false;
        for (const ParameterInfo &parameter : parameters)
        {
            outputs = outputs || (parameter.IsOutput() && &parameter.Type() != &TypeOf<RemoteCall>());
        }
        if (!outputs)
        {
            returnType != nullptr ? Json::WriteValue(writer, *returnType, result.Data) : (void)writer.Null();
        }
        else
        {
            writer.BeginObject();
            if (returnType != nullptr)
            {
                writer.Key("Result");
                Json::WriteValue(writer, *returnType, result.Data);
            }
            usize index = 0;
            for (usize i = 0; i < parameters.Count(); ++i)
            {
                if (&parameters[i].Type() == &TypeOf<RemoteCall>())
                {
                    continue;
                }
                if (parameters[i].IsOutput())
                {
                    writer.Key(JsonName(parameters[i].Name));
                    Json::WriteValue(writer, parameters[i].Type(), slots[index]->Data);
                }
                ++index;
            }
            writer.EndObject();
        }
        call._result = text.AsView();
    }

    static void WriteSchema(JsonWriter &writer, JsonSchemaWriter &schema, const FunctionInfo &function)
    {
        writer.Key("params").BeginObject().Key("type").String("object").Key("properties").BeginObject();
        for (const ParameterInfo &parameter : function.GetParameters())
        {
            if (&parameter.Type() != &TypeOf<RemoteCall>() && !parameter.IsOutput())
            {
                writer.Key(JsonName(parameter.Name));
                schema.Write(writer, parameter.Type());
            }
        }
        writer.EndObject().Key("required").BeginArray();
        for (const ParameterInfo &parameter : function.GetParameters())
        {
            if (&parameter.Type() != &TypeOf<RemoteCall>() && !parameter.IsOutput() && parameter.DefaultValue.IsEmpty())
            {
                writer.String(JsonName(parameter.Name));
            }
        }
        writer.EndArray().EndObject();
        if (function.GetReturnType() != nullptr)
        {
            writer.Key("result");
            schema.Write(writer, *function.GetReturnType());
        }
    }
};

RemoteServer::RemoteServer(NetLoop &loop, RemoteServerDesc desc) : _loop(loop), _desc(Move(desc))
{
}

RemoteServer::~RemoteServer()
{
    Stop();
}

bool RemoteServer::Start()
{
    MARLIN_ASSERT(!_started);
    if (_desc.AllowRemote && _desc.Token.IsEmpty())
    {
        MARLIN_LOG(LogRemote, Error, "Remote connections from other machines need a token; not listening on TCP");
        _desc.AllowRemote = false;
    }
    AddBuiltins();
    AddReflectedMethods();
    _started = true;
    if (!_desc.PipeName.IsEmpty())
    {
        _pipeListener = NetListener::ListenPipe(_loop, _desc.PipeName.AsView(), [this](SharedPtr<NetStream> stream) {
            Accept(Move(stream), RemoteFramings::Stream());
        });
        if (_pipeListener == nullptr)
        {
            MARLIN_LOG(LogRemote, Error, "Remote cannot create pipe {0}", _desc.PipeName);
            return false;
        }
    }
    if (_desc.ListenTcp)
    {
        _tcpListener =
            NetListener::ListenTcp(_loop, _desc.Port, _desc.AllowRemote, [this](SharedPtr<NetStream> stream) {
                Accept(Move(stream), RemoteFramings::AcceptTcp());
            });
        if (_tcpListener == nullptr)
        {
            MARLIN_LOG(LogRemote, Error, "Remote cannot listen on TCP port {0}", _desc.Port);
            return false;
        }
        _boundPort = _tcpListener->GetPort();
    }
    return true;
}

void RemoteServer::Stop()
{
    if (!_started)
    {
        return;
    }
    _started = false;
    for (UniquePtr<NetListener> *listener : {&_pipeListener, &_tcpListener})
    {
        if (*listener != nullptr)
        {
            (*listener)->Close();
            listener->Reset();
        }
    }
    List<SharedPtr<RemoteConnection>> connections;
    {
        const ScopeLock lock(_lock);
        for (auto &entry : _peers)
        {
            connections.Add(entry.Value.Connection);
        }
        _pending.Clear();
    }
    for (const SharedPtr<RemoteConnection> &connection : connections)
    {
        connection->Close();
    }
    for (const SharedPtr<RemoteConnection> &connection : connections)
    {
        connection->WaitClosed();
    }
    const ScopeLock lock(_lock);
    _peers.Clear();
    _pending.Clear();
    _boundPort = 0;
    _started = false;
}

void RemoteServer::AddMethod(StringView name, RemoteHandler handler, RemoteThread thread, StringView description)
{
    const ScopeLock lock(_lock);
    Method &method = _methods.FindOrAdd(String(name));
    method.Handler = SharedPtr<HandlerBox>(new HandlerBox(Move(handler)));
    method.Thread = thread;
    method.Description = description;
    method.Function = nullptr;
}

void RemoteServer::AddStream(StringView stream, StringView description)
{
    const ScopeLock lock(_lock);
    _streams.FindOrAdd(String(stream)) = description;
}

void RemoteServer::AddReflectedMethods()
{
    List<const TypeInfo *> types;
    Reflection::ForEachType([&](const TypeInfo &type) { types.Add(&type); });
    for (const TypeInfo *type : types)
    {
        for (const FunctionInfo &function : type->GetOwnFunctions())
        {
            const Metadata &metadata = function.GetMetadata();
            if (!metadata.Has("Remote"))
            {
                continue;
            }
            if (!function.IsStatic() && !type->IsA(TypeOf<Subsystem>()))
            {
                MARLIN_LOG(LogRemote, Error,
                           "{0}::{1} is a Remote method but neither static nor a member of a subsystem",
                           type->GetName(), function.GetName());
                continue;
            }
            StringBuilder<128> name(metadata.GetOr("Category", type->GetMetadata().GetOr("Category", type->GetName())));
            name.Append('.').Append(function.GetName());
            const FunctionInfo *target = &function;
            const ScopeLock lock(_lock);
            Method &method = _methods.FindOrAdd(name.ToString());
            method.Function = target;
            method.Thread = metadata.HasFlag("Remote", "AnyThread") ? RemoteThread::Any : RemoteThread::Main;
            method.Description = metadata.GetOr("Tooltip", {});
            method.Handler = SharedPtr<HandlerBox>(
                new HandlerBox([target](RemoteCall &call) { RemoteDispatch::Invoke(call, *target); }));
        }
    }
}

void RemoteServer::AddBuiltins()
{
    AddMethod(
        "Remote.Authenticate",
        [this](RemoteCall &call) {
            const JsonValue *token = call.GetParams().Find("Token");
            const ScopeLock lock(_lock);
            Peer *peer = _peers.Find(call.GetConnectionId());
            if (peer == nullptr || token == nullptr || _desc.Token.IsEmpty() || token->AsString() != _desc.Token)
            {
                call.Fail("authentication failed", RemoteErrors::NotAuthenticated);
                return;
            }
            peer->Authenticated = true;
            call.SetResult(JsonValue::MakeBool(true));
        },
        RemoteThread::Any, "Authenticates a TCP or WebSocket connection with the server's token.");
    AddMethod(
        "Remote.Subscribe",
        [this](RemoteCall &call) {
            const JsonValue *stream = call.GetParams().Find("Stream");
            if (stream == nullptr || !stream->IsString())
            {
                call.Fail("Stream is required", RemoteErrors::InvalidParams);
                return;
            }
            const ScopeLock lock(_lock);
            Peer *peer = _peers.Find(call.GetConnectionId());
            if (peer != nullptr && !peer->Subscriptions.Contains(stream->AsString()))
            {
                peer->Subscriptions.Add(String(stream->AsString()));
            }
            call.SetResult(JsonValue::MakeBool(true));
        },
        RemoteThread::Any, "Receives the notifications of a stream, such as Log.Stream.");
    AddMethod(
        "Remote.Unsubscribe",
        [this](RemoteCall &call) {
            const JsonValue *stream = call.GetParams().Find("Stream");
            const ScopeLock lock(_lock);
            Peer *peer = _peers.Find(call.GetConnectionId());
            if (peer != nullptr && stream != nullptr)
            {
                peer->Subscriptions.Remove(String(stream->AsString()));
            }
            call.SetResult(JsonValue::MakeBool(true));
        },
        RemoteThread::Any, "Stops the notifications of a stream.");
    AddMethod(
        "Remote.ListMethods",
        [this](RemoteCall &call) {
            StringBuilder<16384> text;
            JsonWriter writer(text);
            WriteMethodList(writer);
            call.SetResultJson(text.AsView());
        },
        RemoteThread::Any, "Lists the methods with JSON Schemas of their params and results.");
    AddMethod(
        "Remote.ListStreams",
        [this](RemoteCall &call) {
            JsonValue streams = JsonValue::MakeArray();
            const ScopeLock lock(_lock);
            for (const auto &entry : _streams)
            {
                JsonValue stream = JsonValue::MakeObject();
                stream.Set("Name", JsonValue::MakeString(entry.Key));
                stream.Set("Description", JsonValue::MakeString(entry.Value));
                streams.Add(Move(stream));
            }
            call.SetResult(Move(streams));
        },
        RemoteThread::Any, "Lists the streams Remote.Subscribe accepts.");
}

void RemoteServer::WriteMethodList(JsonWriter &writer) const
{
    JsonSchemaWriter schema;
    writer.BeginObject().Key("Methods").BeginArray();
    const ScopeLock lock(_lock);
    for (const auto &entry : _methods)
    {
        writer.BeginObject().Key("Name").String(entry.Key);
        writer.Key("Description").String(entry.Value.Description);
        writer.Key("AnyThread").Bool(entry.Value.Thread == RemoteThread::Any);
        if (entry.Value.Function != nullptr)
        {
            RemoteDispatch::WriteSchema(writer, schema, *entry.Value.Function);
        }
        writer.EndObject();
    }
    writer.EndArray();
    schema.WriteDefinitions(writer);
    writer.EndObject();
}

void RemoteServer::Accept(SharedPtr<NetStream> stream, UniquePtr<IRemoteFraming> framing)
{
    const bool network = stream->IsNetwork();
    const SharedPtr<RemoteConnection> connection(new RemoteConnection(
        Move(stream), Move(framing), [this](RemoteConnection &from, RemoteFrame &frame) { OnFrame(from, frame); },
        [this](RemoteConnection &from) { OnClose(from); }));
    {
        const ScopeLock lock(_lock);
        Peer &peer = _peers.FindOrAdd(connection->GetId());
        peer.Connection = connection;
        peer.Authenticated = !network || _desc.Token.IsEmpty();
    }
    MARLIN_LOG(LogRemote, Info, "Remote connection {0} from {1}", connection->GetId(), connection->Describe());
    connection->Start();
}

void RemoteServer::OnClose(RemoteConnection &connection)
{
    const ScopeLock lock(_lock);
    _peers.Remove(connection.GetId());
}

SharedPtr<RemoteConnection> RemoteServer::FindConnection(uint32 id) const
{
    const ScopeLock lock(_lock);
    const Peer *peer = _peers.Find(id);
    return peer != nullptr ? peer->Connection : SharedPtr<RemoteConnection>();
}

namespace
{
void WriteError(JsonWriter &writer, const JsonValue *id, int32 code, StringView message)
{
    writer.BeginObject().Key("jsonrpc").String("2.0").Key("id");
    id != nullptr ? (void)writer.Value(*id) : (void)writer.Null();
    writer.Key("error").BeginObject().Key("code").Integer(code).Key("message").String(message).EndObject();
    writer.EndObject();
}
} // namespace

void RemoteServer::OnFrame(RemoteConnection &connection, RemoteFrame &frame)
{
    if (frame.IsAttachment)
    {
        return;
    }
    JsonValue request;
    JsonError error;
    if (!Json::Parse(frame.AsText(), request, error))
    {
        StringBuilder<256> text;
        JsonWriter writer(text);
        WriteError(writer, nullptr, RemoteErrors::ParseError,
                   Format("{0} at {1}:{2}", error.Message, error.Line, error.Column));
        connection.SendText(text.AsView());
        return;
    }
    RemoteThread thread = RemoteThread::Any;
    const auto visit = [&](const JsonValue &message) {
        const JsonValue *method = message.IsObject() ? message.Find("method") : nullptr;
        const ScopeLock lock(_lock);
        const Method *found = method != nullptr ? _methods.Find(String(method->AsString())) : nullptr;
        if (found != nullptr && found->Thread == RemoteThread::Main)
        {
            thread = RemoteThread::Main;
        }
    };
    if (request.IsArray())
    {
        for (const JsonValue &message : request.GetElements())
        {
            visit(message);
        }
    }
    else
    {
        visit(request);
    }
    SharedPtr<RemoteConnection> target;
    {
        const ScopeLock lock(_lock);
        const Peer *peer = _peers.Find(connection.GetId());
        if (peer == nullptr)
        {
            return;
        }
        if (thread == RemoteThread::Main)
        {
            _pending.Add({peer->Connection, Move(request)});
            return;
        }
        target = peer->Connection;
    }
    connection.Dispatch([this, target, request = Move(request)] { Respond(*target, request, RemoteThread::Any); });
}

void RemoteServer::Respond(RemoteConnection &connection, const JsonValue &request, RemoteThread /*thread*/)
{
    StringBuilder<1024> text;
    JsonWriter writer(text);
    if (request.IsArray())
    {
        if (request.Count() == 0)
        {
            WriteError(writer, nullptr, RemoteErrors::InvalidRequest, "an empty batch");
            connection.SendText(text.AsView());
            return;
        }
        writer.BeginArray();
        bool any = false;
        for (const JsonValue &message : request.GetElements())
        {
            any = Handle(connection, message, writer) || any;
        }
        writer.EndArray();
        if (any)
        {
            connection.SendText(text.AsView());
        }
        return;
    }
    if (Handle(connection, request, writer))
    {
        connection.SendText(text.AsView());
    }
}

bool RemoteServer::Handle(RemoteConnection &connection, const JsonValue &request, JsonWriter &output)
{
    const JsonValue *id = request.IsObject() ? request.Find("id") : nullptr;
    const JsonValue *version = request.IsObject() ? request.Find("jsonrpc") : nullptr;
    const JsonValue *method = request.IsObject() ? request.Find("method") : nullptr;
    if (version == nullptr || version->AsString() != "2.0" || method == nullptr || !method->IsString() ||
        (id != nullptr && !id->IsString() && !id->IsNumber() && !id->IsNull()))
    {
        WriteError(output, id, RemoteErrors::InvalidRequest, "not a JSON-RPC 2.0 request");
        return true;
    }
    SharedPtr<HandlerBox> handler;
    bool authenticated = false;
    {
        const ScopeLock lock(_lock);
        const Method *found = _methods.Find(String(method->AsString()));
        if (found != nullptr)
        {
            handler = found->Handler;
        }
        const Peer *peer = _peers.Find(connection.GetId());
        authenticated = peer != nullptr && peer->Authenticated;
    }
    if (!authenticated && method->AsString() != "Remote.Authenticate")
    {
        if (id != nullptr)
        {
            WriteError(output, id, RemoteErrors::NotAuthenticated, "call Remote.Authenticate first");
        }
        return id != nullptr;
    }
    if (!handler.IsValid())
    {
        if (id != nullptr)
        {
            WriteError(output, id, RemoteErrors::MethodNotFound, Format("no method {0}", method->AsString()));
        }
        return id != nullptr;
    }
    static const JsonValue noParams;
    const JsonValue *params = request.Find("params");
    RemoteCall call;
    call._server = this;
    call._connection = &connection;
    call._params = params != nullptr ? params : &noParams;
    call._id = id != nullptr ? id->ToString() : String("null");
    handler->Handler(call);
    if (id == nullptr || (call._deferred && !call.HasFailed()))
    {
        return false;
    }
    if (call.HasFailed())
    {
        WriteError(output, id, call._errorCode, call._errorMessage);
        return true;
    }
    output.BeginObject().Key("jsonrpc").String("2.0").Key("id").Value(*id).Key("result");
    output.Raw(call._result.IsEmpty() ? StringView("null") : call._result.AsView());
    output.EndObject();
    return true;
}

void RemoteServer::Complete(const RemoteDeferred &deferred, StringView resultJson)
{
    const SharedPtr<RemoteConnection> connection = FindConnection(deferred.ConnectionId);
    if (!connection.IsValid())
    {
        return;
    }
    StringBuilder<1024> text;
    JsonWriter writer(text);
    writer.BeginObject().Key("jsonrpc").String("2.0").Key("id").Raw(deferred.Id).Key("result");
    writer.Raw(resultJson.IsEmpty() ? StringView("null") : resultJson).EndObject();
    connection->SendText(text.AsView());
}

void RemoteServer::Fail(const RemoteDeferred &deferred, StringView message, int32 code)
{
    const SharedPtr<RemoteConnection> connection = FindConnection(deferred.ConnectionId);
    if (!connection.IsValid())
    {
        return;
    }
    StringBuilder<512> text;
    JsonWriter writer(text);
    writer.BeginObject().Key("jsonrpc").String("2.0").Key("id").Raw(deferred.Id).Key("error").BeginObject();
    writer.Key("code").Integer(code).Key("message").String(message).EndObject().EndObject();
    connection->SendText(text.AsView());
}

void RemoteServer::ProcessPending()
{
    List<Pending> pending;
    List<SharedPtr<RemoteConnection>> stalled;
    {
        const ScopeLock lock(_lock);
        pending = Move(_pending);
        _pending.Clear();
        const uint64 now = PlatformTime::Ticks() * 1000 / PlatformTime::TicksPerSecond();
        for (const auto &entry : _peers)
        {
            if (!entry.Value.Connection->IsReady() && now - entry.Value.Connection->GetStartTime() > HandshakeTimeout)
            {
                stalled.Add(entry.Value.Connection);
            }
        }
    }
    for (const Pending &entry : pending)
    {
        if (entry.Connection->IsOpen())
        {
            Respond(*entry.Connection, entry.Request, RemoteThread::Main);
        }
    }
    for (const SharedPtr<RemoteConnection> &connection : stalled)
    {
        connection->Close();
    }
}

void RemoteServer::Publish(StringView stream, const JsonValue &params)
{
    PublishJson(stream, params.ToString().AsView());
}

void RemoteServer::Notify(uint32 id, StringView method, StringView params)
{
    const SharedPtr<RemoteConnection> connection = FindConnection(id);
    if (!connection.IsValid())
    {
        return;
    }
    StringBuilder<512> text;
    JsonWriter writer(text);
    writer.BeginObject().Key("jsonrpc").String("2.0").Key("method").String(method).Key("params").Raw(params);
    writer.EndObject();
    connection->SendText(text.AsView());
}

void RemoteServer::PublishJson(StringView stream, StringView params)
{
    PublishJson(stream, Span<const StringView>(&params, 1));
}

void RemoteServer::PublishJson(StringView stream, Span<const StringView> params)
{
    List<SharedPtr<RemoteConnection>, InlineAllocator<4>> targets;
    {
        const ScopeLock lock(_lock);
        for (const auto &entry : _peers)
        {
            if (entry.Value.Subscriptions.Contains(stream))
            {
                targets.Add(entry.Value.Connection);
            }
        }
    }
    if (targets.IsEmpty())
    {
        return;
    }
    StringBuilder<512> text;
    for (const StringView entry : params)
    {
        text.Clear();
        JsonWriter writer(text);
        writer.BeginObject().Key("jsonrpc").String("2.0").Key("method").String(stream).Key("params").Raw(entry);
        writer.EndObject();
        for (const SharedPtr<RemoteConnection> &connection : targets)
        {
            connection->SendText(text.AsView());
        }
    }
}

bool RemoteServer::HasSubscribers(StringView stream) const
{
    const ScopeLock lock(_lock);
    for (const auto &entry : _peers)
    {
        if (entry.Value.Subscriptions.Contains(stream))
        {
            return true;
        }
    }
    return false;
}

bool RemoteServer::IsConnected(uint32 id) const
{
    const SharedPtr<RemoteConnection> connection = FindConnection(id);
    return connection.IsValid() && connection->IsOpen();
}

usize RemoteServer::GetConnectionCount() const
{
    const ScopeLock lock(_lock);
    return _peers.Count();
}
} // namespace Marlin
