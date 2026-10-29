#include "Remote/RemoteSubsystem.h"

#include "Console/Console.h"
#include "Diagnostics/Assert.h"
#include "Engine/Engine.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Json/JsonWriter.h"
#include "Net/NetSubsystem.h"
#include "Remote/RemoteDiscovery.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
uint32 RemoteSubsystem::Port = 0;
bool RemoteSubsystem::AllowRemote = false;
String RemoteSubsystem::Token;

namespace
{
Atomic<uint64> _currentFrame{0};
} // namespace

EngineInfo EngineRemoteMethods::Info()
{
    const EngineEnvironment &environment = Engine::GetEnvironment();
    EngineInfo info;
    info.Target = environment.TargetName;
    info.EngineId = environment.EngineId;
    info.EngineRoot = environment.EngineRoot;
    info.Project = environment.ProjectName;
    info.Platform = environment.Platform;
    info.Configuration = environment.Configuration;
    info.ProcessId = PlatformProcess::CurrentId();
    info.Frame = _currentFrame.Load(MemoryOrder::Relaxed);
    return info;
}

void EngineRemoteMethods::Quit(int32 exitCode)
{
    Engine::RequestExit(exitCode);
}

ConsoleResult EngineRemoteMethods::ExecConsole(const String &line)
{
    ConsoleResult result;
    StringBuilder<1024> output;
    result.Succeeded = Console::Execute(line.AsView(), output, ConsoleSetBy::Console);
    result.Output = output.AsView();
    return result;
}

String EngineRemoteMethods::GetVariable(const String &name, RemoteCall &call)
{
    const ConsoleVariable *variable = Console::FindVariable(name.AsView());
    if (variable == nullptr)
    {
        call.Fail(Format("no console variable {0}", name));
        return {};
    }
    StringBuilder<256> value;
    variable->FormatValue(value);
    return value.ToString();
}

void RemoteSubsystem::Initialize(SubsystemContext &context)
{
    NetSubsystem *net = context.Collection->Get<NetSubsystem>();
    MARLIN_ASSERT(net != nullptr);
    _processId = PlatformProcess::CurrentId();
    RemoteServerDesc desc;
    desc.PipeName = Format("Marlin.{0}", _processId);
    desc.ListenTcp = Port != 0 && Port <= 0xFFFF;
    desc.Port = static_cast<uint16>(desc.ListenTcp ? Port : 0);
    desc.AllowRemote = AllowRemote;
    desc.Token = Token;
    _server = MakeUnique<RemoteServer>(net->GetLoop(), Move(desc));
    _server->AddStream(RemoteLogStream::Stream,
                       "Every log record: Category, Level, Message, Frame, Thread, Seconds, File, Line.");
    if (!_server->Start())
    {
        MARLIN_LOG(LogRemote, Warning, "Remote is only partly listening; see the errors above");
    }
    _sink.Server = _server.Get();
    Log::AddSink(&_sink);

    const EngineEnvironment &environment = Engine::GetEnvironment();
    RemoteProcessInfo info;
    info.ProcessId = _processId;
    info.Target = environment.TargetName;
    info.EngineId = environment.EngineId;
    info.Project = environment.ProjectName;
    info.Pipe = _server->GetPipeName();
    info.Port = _server->GetPort();
    if (!RemoteDiscovery::Register(info))
    {
        MARLIN_LOG(LogRemote, Warning, "Remote could not write its process file to {0}",
                   RemoteDiscovery::GetDirectory());
    }
}

void RemoteSubsystem::Deinitialize()
{
    RemoteDiscovery::Unregister(_processId);
    Log::RemoveSink(&_sink);
    _sink.Server = nullptr;
    _server->Stop();
    _server.Reset();
}

void RemoteSubsystem::BeginFrame(const FrameTime &time)
{
    _currentFrame.Store(time.Frame, MemoryOrder::Relaxed);
    _server->ProcessPending();
}
} // namespace Marlin
