#include "Remote/RemoteClient.h"

#include "Concurrency/ScopeLock.h"
#include "HAL/PlatformTime.h"
#include "Json/JsonWriter.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
RemoteClient::RemoteClient(NetLoop &loop) : _loop(loop)
{
}

RemoteClient::~RemoteClient()
{
    Disconnect();
}

bool RemoteClient::ConnectPipe(StringView name, uint32 timeout)
{
    Disconnect();
    _pipeName.Clear();
    SharedPtr<NetStream> stream = NetStream::ConnectPipe(_loop, name, timeout);
    if (!stream.IsValid() || !Attach(Move(stream), RemoteFramings::Stream(), {}, timeout))
    {
        return false;
    }
    _pipeName = String(name);
    return true;
}

bool RemoteClient::ConnectTcp(StringView host, uint16 port, uint32 timeout)
{
    Disconnect();
    _pipeName.Clear();
    SharedPtr<NetStream> stream = NetStream::ConnectTcp(_loop, host, port, timeout);
    return stream.IsValid() && Attach(Move(stream), RemoteFramings::Stream(), {}, timeout);
}

bool RemoteClient::ConnectWebSocket(StringView host, uint16 port, uint32 timeout)
{
    Disconnect();
    _pipeName.Clear();
    SharedPtr<NetStream> stream = NetStream::ConnectTcp(_loop, host, port, timeout);
    if (!stream.IsValid())
    {
        return false;
    }
    List<uint8> request;
    UniquePtr<IRemoteFraming> framing =
        RemoteFramings::WebSocketClient(Format("{0}:{1}", host, port).AsView(), "/", request);
    return Attach(Move(stream), Move(framing), Move(request), timeout);
}

bool RemoteClient::Attach(SharedPtr<NetStream> stream, UniquePtr<IRemoteFraming> framing, List<uint8> request,
                          uint32 timeout)
{
    const SharedPtr<RemoteConnection> connection(new RemoteConnection(
        Move(stream), Move(framing), [this](RemoteConnection &, RemoteFrame &frame) { OnFrame(frame); },
        [this](RemoteConnection &) { OnClose(); }));
    {
        const ScopeLock lock(_lock);
        _connection = connection;
    }
    if (!request.IsEmpty())
    {
        connection->SendRaw(Move(request));
    }
    connection->Start();
    if (!connection->WaitReady(timeout))
    {
        Disconnect();
        return false;
    }
    return true;
}

void RemoteClient::Disconnect()
{
    SharedPtr<RemoteConnection> connection;
    {
        const ScopeLock lock(_lock);
        connection = Move(_connection);
        _connection = {};
    }
    if (connection.IsValid())
    {
        connection->Close();
        if (!_loop.IsIoThread())
        {
            connection->WaitClosed();
        }
    }
    OnClose();
}

SharedPtr<RemoteConnection> RemoteClient::GetConnection() const
{
    const ScopeLock lock(_lock);
    return _connection;
}

bool RemoteClient::IsConnected() const
{
    const SharedPtr<RemoteConnection> connection = GetConnection();
    return connection.IsValid() && connection->IsOpen();
}

void RemoteClient::CollectAttachments(const JsonValue &value, List<uint64> &ids)
{
    if (value.IsObject())
    {
        const JsonValue *attachment = value.Find("Attachment");
        uint64 id = 0;
        if (attachment != nullptr && value.Find("Size") != nullptr && attachment->TryGetUnsigned(id))
        {
            ids.Add(id);
        }
        for (const JsonMember &member : value.GetMembers())
        {
            CollectAttachments(member.Value, ids);
        }
    }
    else if (value.IsArray())
    {
        for (const JsonValue &element : value.GetElements())
        {
            CollectAttachments(element, ids);
        }
    }
}

void RemoteClient::OnFrame(RemoteFrame &frame)
{
    if (frame.IsAttachment)
    {
        const ScopeLock lock(_lock);
        _attachments.FindOrAdd(frame.AttachmentId) = Move(frame.Bytes);
        return;
    }
    JsonValue message;
    JsonError error;
    if (!Json::Parse(frame.AsText(), message, error))
    {
        return;
    }
    List<Ready> immediate;
    const auto handle = [&](const JsonValue &entry) {
        const JsonValue *id = entry.Find("id");
        const JsonValue *method = entry.Find("method");
        if (method != nullptr && id == nullptr)
        {
            const JsonValue *params = entry.Find("params");
            const ScopeLock lock(_lock);
            _notifications.Add({String(method->AsString()), params != nullptr ? *params : JsonValue()});
            return;
        }
        uint64 key = 0;
        if (id == nullptr || !id->TryGetUnsigned(key))
        {
            return;
        }
        RemoteResponse response;
        const JsonValue *result = entry.Find("result");
        const JsonValue *failure = entry.Find("error");
        response.Succeeded = result != nullptr && failure == nullptr;
        if (result != nullptr)
        {
            response.Result = *result;
        }
        if (failure != nullptr)
        {
            int64 code = 0;
            (void)failure->Find("code")->TryGetInteger(code);
            response.ErrorCode = static_cast<int32>(code);
            const JsonValue *text = failure->Find("message");
            response.ErrorMessage = text != nullptr ? text->AsString() : StringView();
        }
        List<uint64> ids;
        CollectAttachments(response.Result, ids);
        const ScopeLock lock(_lock);
        for (const uint64 attachment : ids)
        {
            if (List<uint8> *bytes = _attachments.Find(attachment))
            {
                response.Attachments.FindOrAdd(attachment) = Move(*bytes);
                _attachments.Remove(attachment);
            }
        }
        Waiting *waiting = _waiting.Find(key);
        if (waiting == nullptr)
        {
            return;
        }
        if (waiting->Done != nullptr)
        {
            *waiting->Response = Move(response);
            waiting->Done->Set();
        }
        else if (waiting->Immediate)
        {
            immediate.Add({Move(waiting->Handler), Move(response)});
        }
        else
        {
            _ready.Add({Move(waiting->Handler), Move(response)});
        }
        _waiting.Remove(key);
    };
    if (message.IsArray())
    {
        for (const JsonValue &entry : message.GetElements())
        {
            handle(entry);
        }
    }
    else if (message.IsObject())
    {
        handle(message);
    }
    for (Ready &entry : immediate)
    {
        entry.Handler(entry.Response);
    }
}

void RemoteClient::OnClose()
{
    List<Ready> immediate;
    {
        const ScopeLock lock(_lock);
        for (auto &entry : _waiting)
        {
            RemoteResponse response;
            response.ErrorCode = RemoteErrors::ConnectionLost;
            response.ErrorMessage = "the connection closed";
            if (entry.Value.Done != nullptr)
            {
                *entry.Value.Response = Move(response);
                entry.Value.Done->Set();
            }
            else
            {
                (entry.Value.Immediate ? immediate : _ready).Add({Move(entry.Value.Handler), Move(response)});
            }
        }
        _waiting.Clear();
    }
    for (Ready &entry : immediate)
    {
        entry.Handler(entry.Response);
    }
}

uint64 RemoteClient::Send(StringView method, const JsonValue &params, Waiting waiting)
{
    uint64 id = 0;
    {
        const ScopeLock lock(_lock);
        id = _nextId++;
        _waiting.FindOrAdd(id) = Move(waiting);
    }
    StringBuilder<512> text;
    JsonWriter writer(text);
    writer.BeginObject().Key("jsonrpc").String("2.0").Key("id").Unsigned(id).Key("method").String(method);
    if (!params.IsNull())
    {
        writer.Key("params").Value(params);
    }
    writer.EndObject();
    const SharedPtr<RemoteConnection> connection = GetConnection();
    if (!connection.IsValid() || !connection->IsOpen())
    {
        OnClose();
        return id;
    }
    connection->SendText(text.AsView());
    return id;
}

void RemoteClient::Call(StringView method, const JsonValue &params, RemoteResponseHandler handler)
{
    Waiting waiting;
    waiting.Handler = Move(handler);
    (void)Send(method, params, Move(waiting));
}

void RemoteClient::CallAsync(StringView method, const JsonValue &params, RemoteResponseHandler handler)
{
    Waiting waiting;
    waiting.Handler = Move(handler);
    waiting.Immediate = true;
    (void)Send(method, params, Move(waiting));
}

bool RemoteClient::CallSync(StringView method, const JsonValue &params, RemoteResponse &response, uint32 timeout)
{
    Event done(EventReset::Manual);
    Waiting waiting;
    waiting.Done = &done;
    waiting.Response = &response;
    const uint64 id = Send(method, params, Move(waiting));
    if (!done.Wait(timeout))
    {
        const ScopeLock lock(_lock);
        if (_waiting.Remove(id))
        {
            response = {};
            response.ErrorCode = RemoteErrors::ConnectionLost;
            response.ErrorMessage = "timed out";
            return false;
        }
    }
    return response.Succeeded;
}

void RemoteClient::Notify(StringView method, const JsonValue &params)
{
    const SharedPtr<RemoteConnection> connection = GetConnection();
    if (!connection.IsValid() || !connection->IsOpen())
    {
        return;
    }
    StringBuilder<512> text;
    JsonWriter writer(text);
    writer.BeginObject().Key("jsonrpc").String("2.0").Key("method").String(method);
    if (!params.IsNull())
    {
        writer.Key("params").Value(params);
    }
    writer.EndObject();
    connection->SendText(text.AsView());
}

void RemoteClient::NotifyJson(StringView method, StringView params)
{
    const SharedPtr<RemoteConnection> connection = GetConnection();
    if (!connection.IsValid() || !connection->IsOpen())
    {
        return;
    }
    StringBuilder<512> text;
    JsonWriter writer(text);
    writer.BeginObject().Key("jsonrpc").String("2.0").Key("method").String(method).Key("params").Raw(params);
    writer.EndObject();
    connection->SendText(text.AsView());
}

void RemoteClient::OnNotification(StringView method, RemoteNotificationHandler handler)
{
    const ScopeLock lock(_lock);
    _handlers.FindOrAdd(String(method)) = SharedPtr<HandlerBox>(new HandlerBox(Move(handler)));
}

void RemoteClient::ProcessPending()
{
    List<Ready> ready;
    List<ReadyNotification> notifications;
    {
        const ScopeLock lock(_lock);
        ready = Move(_ready);
        _ready.Clear();
        notifications = Move(_notifications);
        _notifications.Clear();
    }
    for (Ready &entry : ready)
    {
        if (entry.Handler)
        {
            entry.Handler(entry.Response);
        }
    }
    for (const ReadyNotification &notification : notifications)
    {
        SharedPtr<HandlerBox> handler;
        {
            const ScopeLock lock(_lock);
            if (const SharedPtr<HandlerBox> *found = _handlers.Find(notification.Method))
            {
                handler = *found;
            }
        }
        if (handler.IsValid())
        {
            handler->Handler(notification.Params);
        }
    }
}
} // namespace Marlin
