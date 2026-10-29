#include "Testing/Testing.h"

#include "Concurrency/ScopeLock.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformThread.h"
#include "Json/JsonValue.h"
#include "Net/NetLoop.h"
#include "Remote/RemoteClient.h"
#include "Remote/RemoteDiscovery.h"
#include "Remote/RemoteLogStream.h"
#include "Remote/RemoteServer.h"
#include "RemoteTestTypes.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "WebSocketHandshake.h"

namespace Marlin::Tests
{
int32 RemoteTestMethods::Calls = 0;

int32 RemoteTestMethods::Add(int32 left, int32 right)
{
    ++Calls;
    return left + right;
}

RemoteTestPoint RemoteTestMethods::Mirror(const RemoteTestPoint &point, int32 &sum)
{
    sum = point.X + point.Y;
    return {point.Y, point.X};
}

RemoteAttachment RemoteTestMethods::Bytes(uint32 count, RemoteCall &call)
{
    List<uint8> bytes;
    for (uint32 i = 0; i < count; ++i)
    {
        bytes.Add(static_cast<uint8>(i));
    }
    return call.Attach(Move(bytes));
}

void RemoteTestMethods::Refuse(RemoteCall &call)
{
    call.Fail("refused", -32050);
}

namespace
{
JsonValue Parse(StringView text)
{
    JsonValue value;
    JsonError error;
    MARLIN_REQUIRE(Json::Parse(text, value, error));
    return value;
}

/// Calls *method* and pumps both sides until the response arrives, as a main loop would.
RemoteResponse Pump(RemoteServer &server, RemoteClient &client, StringView method, const JsonValue &params)
{
    RemoteResponse response;
    bool done = false;
    client.Call(method, params, [&](RemoteResponse &result) {
        response = Move(result);
        done = true;
    });
    for (int32 i = 0; i < 2000 && !done; ++i)
    {
        server.ProcessPending();
        client.ProcessPending();
        PlatformThread::Sleep(1);
    }
    MARLIN_REQUIRE(done);
    return response;
}

String PipeName(StringView test)
{
    return Format("MarlinTests.{0}.{1}", PlatformProcess::CurrentId(), test);
}
} // namespace

MARLIN_TEST(Remote, WebSocketHandshake)
{
    uint8 digest[20];
    WebSocketHandshake::Sha1("abc", 3, digest);
    StringBuilder<64> hex;
    for (const uint8 byte : digest)
    {
        hex.Append("0123456789abcdef"[byte >> 4]).Append("0123456789abcdef"[byte & 15]);
    }
    MARLIN_EXPECT(hex.AsView() == "a9993e364706816aba3e25717850c26c9cd0d89d");
    StringBuilder<64> accept;
    WebSocketHandshake::AcceptKey("dGhlIHNhbXBsZSBub25jZQ==", accept);
    MARLIN_EXPECT(accept.AsView() == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

MARLIN_TEST(Remote, PipeCalls)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    RemoteServerDesc desc;
    desc.PipeName = PipeName("PipeCalls");
    RemoteServer server(loop, desc);
    MARLIN_REQUIRE(server.Start());
    server.AddMethod("Test.Echo", [](RemoteCall &call) { call.SetResult(call.GetParams()); }, RemoteThread::Any);
    RemoteClient client(loop);
    MARLIN_REQUIRE(client.ConnectPipe(desc.PipeName.AsView()));

    RemoteResponse response;
    MARLIN_REQUIRE(client.CallSync("Test.Echo", Parse(R"({"A":[1,2]})"), response));
    MARLIN_EXPECT(response.Result == Parse(R"({"A":[1,2]})"));

    RemoteTestMethods::Calls = 0;
    response = Pump(server, client, "Test.Add", Parse(R"({"Left": 40})"));
    MARLIN_EXPECT(response.Succeeded && response.Result.AsNumber() == 42 && RemoteTestMethods::Calls == 1);
    response = Pump(server, client, "Test.Add", Parse("[1, 5]"));
    MARLIN_EXPECT(response.Succeeded && response.Result.AsNumber() == 6);
    response = Pump(server, client, "Test.Add", Parse(R"({"Right": 1})"));
    MARLIN_EXPECT(response.ErrorCode == RemoteErrors::InvalidParams && response.ErrorMessage.AsView().Contains("Left"));
    response = Pump(server, client, "Test.Add", Parse(R"({"Left": "x"})"));
    MARLIN_EXPECT(response.ErrorCode == RemoteErrors::InvalidParams);

    MARLIN_REQUIRE(client.CallSync("Test.Mirror", Parse(R"({"Point": {"X": 3, "Y": 4}})"), response));
    MARLIN_EXPECT(response.Result == Parse(R"({"Result": {"X": 4, "Y": 3}, "Sum": 7})"));
    MARLIN_REQUIRE(client.CallSync("Test.Bytes", Parse(R"({"Count": 300})"), response));
    uint64 attachment = 0;
    MARLIN_REQUIRE(response.Result.Find("Attachment")->TryGetUnsigned(attachment));
    const List<uint8> *bytes = response.FindAttachment(attachment);
    MARLIN_REQUIRE(bytes != nullptr && bytes->Count() == 300);
    MARLIN_EXPECT((*bytes)[299] == static_cast<uint8>(299));
    MARLIN_EXPECT(!client.CallSync("Test.Refuse", JsonValue(), response));
    MARLIN_EXPECT(response.ErrorCode == -32050 && response.ErrorMessage == "refused");
    MARLIN_EXPECT(!client.CallSync("Test.Missing", JsonValue(), response));
    MARLIN_EXPECT(response.ErrorCode == RemoteErrors::MethodNotFound);

    MARLIN_REQUIRE(client.CallSync("Remote.ListMethods", JsonValue(), response));
    bool foundAdd = false;
    for (const JsonValue &method : response.Result.Find("Methods")->GetElements())
    {
        if (method.Find("Name")->AsString() == "Test.Add")
        {
            foundAdd = true;
            MARLIN_EXPECT(method.Find("Description")->AsString() == "Adds two numbers.");
            MARLIN_EXPECT(method.Find("params")->Find("required")->Count() == 1);
        }
    }
    MARLIN_EXPECT(foundAdd);
    MARLIN_EXPECT(server.GetConnectionCount() == 1);
    client.Disconnect();
    server.Stop();
}

MARLIN_TEST(Remote, StreamsAndNotifications)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    RemoteServerDesc desc;
    desc.PipeName = PipeName("Streams");
    RemoteServer server(loop, desc);
    MARLIN_REQUIRE(server.Start());
    RemoteClient client(loop);
    MARLIN_REQUIRE(client.ConnectPipe(desc.PipeName.AsView()));
    int32 received = 0;
    client.OnNotification(
        "Test.Stream", [&](const JsonValue &params) { received += static_cast<int32>(params.Find("N")->AsNumber()); });
    RemoteResponse response;
    MARLIN_EXPECT(!server.HasSubscribers("Test.Stream"));
    MARLIN_REQUIRE(client.CallSync("Remote.Subscribe", Parse(R"({"Stream": "Test.Stream"})"), response));
    MARLIN_EXPECT(server.HasSubscribers("Test.Stream"));
    server.PublishJson("Test.Stream", R"({"N": 5})");
    server.PublishJson("Other.Stream", R"({"N": 100})");
    for (int32 i = 0; i < 2000 && received == 0; ++i)
    {
        client.ProcessPending();
        PlatformThread::Sleep(1);
    }
    MARLIN_EXPECT(received == 5);
    client.Notify("Test.Ignored", JsonValue());
    MARLIN_REQUIRE(client.CallSync("Remote.Unsubscribe", Parse(R"({"Stream": "Test.Stream"})"), response));
    MARLIN_EXPECT(!server.HasSubscribers("Test.Stream"));
}

MARLIN_TEST(Remote, TcpAndWebSocket)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    RemoteServerDesc desc;
    desc.ListenTcp = true;
    desc.Token = "secret";
    RemoteServer server(loop, desc);
    MARLIN_REQUIRE(server.Start() && server.GetPort() != 0);

    RemoteClient tcp(loop);
    MARLIN_REQUIRE(tcp.ConnectTcp("127.0.0.1", server.GetPort()));
    RemoteResponse response;
    MARLIN_EXPECT(!tcp.CallSync("Remote.ListStreams", JsonValue(), response));
    MARLIN_EXPECT(response.ErrorCode == RemoteErrors::NotAuthenticated);
    MARLIN_EXPECT(!tcp.CallSync("Remote.Authenticate", Parse(R"({"Token": "wrong"})"), response));
    MARLIN_REQUIRE(tcp.CallSync("Remote.Authenticate", Parse(R"({"Token": "secret"})"), response));
    MARLIN_REQUIRE(tcp.CallSync("Test.Mirror", Parse(R"({"Point": {"X": 1, "Y": 2}})"), response));
    MARLIN_EXPECT(response.Result.Find("Sum")->AsNumber() == 3);

    RemoteClient web(loop);
    MARLIN_REQUIRE(web.ConnectWebSocket("127.0.0.1", server.GetPort()));
    MARLIN_REQUIRE(web.CallSync("Remote.Authenticate", Parse(R"({"Token": "secret"})"), response));
    MARLIN_REQUIRE(web.CallSync("Test.Bytes", Parse(R"({"Count": 70000})"), response));
    uint64 attachment = 0;
    MARLIN_REQUIRE(response.Result.Find("Attachment")->TryGetUnsigned(attachment));
    const List<uint8> *bytes = response.FindAttachment(attachment);
    MARLIN_EXPECT(bytes != nullptr && bytes->Count() == 70000 && (*bytes)[69999] == static_cast<uint8>(69999));
    response = Pump(server, web, "Test.Add", Parse("[2, 3]"));
    MARLIN_EXPECT(response.Result.AsNumber() == 5);
    MARLIN_EXPECT(server.GetConnectionCount() == 2);
}

MARLIN_TEST(Remote, Discovery)
{
    RemoteProcessInfo info;
    info.ProcessId = PlatformProcess::CurrentId();
    info.Target = "MarlinTests";
    info.Pipe = "Marlin.Test";
    MARLIN_REQUIRE(RemoteDiscovery::Register(info));
    List<RemoteProcessInfo> processes;
    RemoteDiscovery::List(processes);
    bool found = false;
    for (const RemoteProcessInfo &process : processes)
    {
        found = found || (process.ProcessId == info.ProcessId && process.Pipe == "Marlin.Test");
    }
    MARLIN_EXPECT(found);
    RemoteDiscovery::Unregister(info.ProcessId);
    processes.Clear();
    RemoteDiscovery::List(processes);
    for (const RemoteProcessInfo &process : processes)
    {
        MARLIN_EXPECT(process.ProcessId != info.ProcessId);
    }
}
} // namespace Marlin::Tests

namespace Marlin::Tests
{
MARLIN_TEST(Remote, DeferredResponses)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    RemoteServerDesc desc;
    desc.PipeName = Format("MarlinTests.{0}.Deferred", PlatformProcess::CurrentId());
    RemoteServer server(loop, desc);
    MARLIN_REQUIRE(server.Start());
    Mutex lock;
    List<RemoteDeferred> waiting;
    server.AddMethod(
        "Test.Later",
        [&](RemoteCall &call) {
            const ScopeLock scope(lock);
            waiting.Add(call.Defer());
        },
        RemoteThread::Any);
    RemoteClient client(loop);
    MARLIN_REQUIRE(client.ConnectPipe(desc.PipeName.AsView()));
    int32 answered = 0;
    client.Call("Test.Later", JsonValue(), [&](RemoteResponse &response) { answered += response.Succeeded ? 1 : 100; });
    client.Call("Test.Later", JsonValue(),
                [&](RemoteResponse &response) { answered += response.ErrorCode == -32077 ? 10 : 100; });
    for (int32 i = 0; i < 2000; ++i)
    {
        const ScopeLock scope(lock);
        if (waiting.Count() == 2)
        {
            break;
        }
        PlatformThread::Sleep(1);
    }
    MARLIN_REQUIRE(waiting.Count() == 2);
    server.Fail(waiting[1], "not today", -32077);
    server.Complete(waiting[0], "42");
    for (int32 i = 0; i < 2000 && answered != 11; ++i)
    {
        client.ProcessPending();
        PlatformThread::Sleep(1);
    }
    MARLIN_EXPECT(answered == 11);
}

MARLIN_DECLARE_LOG_CATEGORY(, LogRemoteTestOrigin, Info, Info);
MARLIN_DECLARE_LOG_CATEGORY(, LogRemoteTestRelay, Info, Info);
MARLIN_DEFINE_LOG_CATEGORY(LogRemoteTestOrigin);
MARLIN_DEFINE_LOG_CATEGORY(LogRemoteTestRelay);

namespace
{
/// Keeps the records of one category.
struct CategorySink final : ILogSink
{
    explicit CategorySink(const LogCategory &category) : Category(category)
    {
    }

    void Write(const LogRecord &record) override
    {
        if (record.Category == &Category)
        {
            Levels.Add(record.Level);
            Messages.Add(String(record.Message));
        }
    }

    const LogCategory &Category;
    List<LogLevel> Levels;
    List<String> Messages;
};
} // namespace

// A record of another process, as Log.Stream carries it, logs here with its source and category in front.
MARLIN_TEST(Remote, LogStreamEntriesRelog)
{
    LogRecord record;
    record.Category = &LogRemoteTestOrigin;
    record.Level = LogLevel::Warning;
    record.Message = "a \"quoted\" line";
    StringBuilder<256> text;
    RemoteLogStream::FormatEntry(text, record, "Worker 7");
    JsonValue entry;
    JsonError error;
    MARLIN_REQUIRE(Json::Parse(text.AsView(), entry, error));

    CategorySink sink(LogRemoteTestRelay);
    Log::AddSink(&sink);
    RemoteLogStream::Relog(LogRemoteTestRelay, entry, "Indexer");
    // Without a Source the default names it; Fatal ended the other process, so it is only an error here.
    record.Level = LogLevel::Fatal;
    text.Clear();
    RemoteLogStream::FormatEntry(text, record);
    MARLIN_REQUIRE(Json::Parse(text.AsView(), entry, error));
    RemoteLogStream::Relog(LogRemoteTestRelay, entry, "Indexer");
    Log::Flush();
    Log::RemoveSink(&sink);

    MARLIN_REQUIRE(sink.Messages.Count() == 2);
    MARLIN_EXPECT(sink.Levels[0] == LogLevel::Warning && sink.Levels[1] == LogLevel::Error);
    MARLIN_EXPECT(sink.Messages[0].AsView() == "[Worker 7] LogRemoteTestOrigin: a \"quoted\" line");
    MARLIN_EXPECT(sink.Messages[1].AsView() == "[Indexer] LogRemoteTestOrigin: a \"quoted\" line");
}
} // namespace Marlin::Tests
