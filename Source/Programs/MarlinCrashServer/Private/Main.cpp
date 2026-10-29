#include "Config/Config.h"
#include "CrashServer/CrashServer.h"
#include "Diagnostics/Log.h"
#include "Engine/Engine.h"
#include "HAL/PlatformThread.h"
#include "Net/NetLoop.h"
#include "Net/NetSubsystem.h"
#include "String/Format.h"

int main(int /*argc*/, const char *const * /*argv*/)
{
    using namespace Marlin;
    if (!ProgramHost::Initialize("MarlinCrashServer", ProgramFeatures::Config))
    {
        return 1;
    }
    CrashServerDesc desc;
    desc.Port = Config::GetOr<uint16>("Engine.CrashServer.Port", CrashServerDefaultPort);
    // RECOVERY-DECISION D7: crash server setting chosen (not recovered).
    desc.AllowRemote = Config::GetOr<bool>("Engine.CrashServer.AllowRemote", false);
    desc.RetentionDays = Config::GetOr<uint32>("Engine.CrashServer.RetentionDays", desc.RetentionDays);
    if (!Config::TryGet("Engine.CrashServer.DataDirectory", desc.DataDirectory) || desc.DataDirectory.IsEmpty())
    {
        desc.DataDirectory = Format("{0}/Saved/CrashServer", Engine::GetEnvironment().EngineRoot);
    }
    (void)Config::TryGet("Engine.CrashServer.SymbolStore", desc.SymbolStore);

    int32 exitCode = 0;
    NetLoop loop;
    if (loop.Start(NetSubsystem::Threads))
    {
        CrashServer server(loop, Move(desc));
        if (server.Start())
        {
            MARLIN_LOG(LogCore, Info, "MarlinCrashServer listens on port {0}", server.GetPort());
            while (!Engine::IsExitRequested())
            {
                server.Tick();
                PlatformThread::Sleep(50);
            }
        }
        else
        {
            exitCode = 1;
        }
        server.Stop();
    }
    else
    {
        exitCode = 1;
    }
    loop.Stop();
    ProgramHost::Shutdown();
    return exitCode;
}
