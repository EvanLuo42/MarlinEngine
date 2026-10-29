#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "Engine/Subsystem.h"
#include "Memory/UniquePtr.h"
#include "Remote/RemoteLogStream.h"
#include "Remote/RemoteServer.h"
#include "String/String.h"
#include "Remote/RemoteSubsystem.gen.h"

namespace Marlin
{
MSTRUCT()
struct EngineInfo
{
    MGENERATED_BODY()

    MPROPERTY()
    String Target;
    MPROPERTY()
    String EngineId;
    MPROPERTY()
    String EngineRoot;
    MPROPERTY()
    String Project;
    MPROPERTY()
    String Platform;
    MPROPERTY()
    String Configuration;
    MPROPERTY()
    uint32 ProcessId = 0;
    MPROPERTY()
    uint64 Frame = 0;
};

MSTRUCT()
struct ConsoleResult
{
    MGENERATED_BODY()

    MPROPERTY()
    bool Succeeded = false;
    MPROPERTY()
    String Output;
};

/// The Engine group of Remote methods.
MSTRUCT(Category = "Engine")
struct EngineRemoteMethods
{
    MGENERATED_BODY()

    /// Identifies the process: target, engine, project, platform and configuration.
    MFUNCTION(Remote = AnyThread)
    static EngineInfo Info();

    /// Ends the main loop with *exitCode*.
    MFUNCTION(Remote)
    static void Quit(int32 exitCode = 0);

    /// Runs a console line, such as "r.Shadow.Quality 2" or a command.
    MFUNCTION(Remote)
    static ConsoleResult ExecConsole(const String &line);

    /// The value of a console variable as text.
    MFUNCTION(Remote)
    static String GetVariable(const String &name, RemoteCall &call);
};

/// Serves Remote for the engine process: the pipe Marlin.<pid>, the TCP port of Engine.Remote if set, the process
/// file for discovery, and the Log.Stream stream. Not linked into Shipping builds.
MCLASS(Config = "Engine.Remote", Dependencies = "NetSubsystem, JobSubsystem")
class REMOTE_API RemoteSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// TCP port for tools on other machines, devkits and browsers; zero for none.
    MPROPERTY()
    static uint32 Port;
    /// Accept TCP connections from other machines; needs Token.
    MPROPERTY()
    static bool AllowRemote;
    /// What TCP and WebSocket clients pass to Remote.Authenticate.
    MPROPERTY()
    static String Token;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    [[nodiscard]] RemoteServer &GetServer() const
    {
        return *_server;
    }

private:
    UniquePtr<RemoteServer> _server;
    RemoteLogStream _sink;
    uint32 _processId = 0;
};
} // namespace Marlin
