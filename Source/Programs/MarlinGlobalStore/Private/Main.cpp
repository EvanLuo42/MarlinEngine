#include "Config/Config.h"
#include "Engine/Engine.h"
#include "GlobalStoreServer/GlobalStoreServer.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Net/NetLoop.h"
#include "Net/NetSubsystem.h"
#include "String/Format.h"

int main(int /*argc*/, const char *const * /*argv*/)
{
    using namespace Marlin;
    if (!ProgramHost::Initialize("MarlinGlobalStore", ProgramFeatures::Config))
    {
        return 1;
    }
    GlobalStoreServerDesc desc;
    desc.Port = Config::GetOr<uint16>("Engine.GlobalStore.Port", GlobalStoreDefaultPort);
    desc.AllowRemote = Config::GetOr<bool>("Engine.GlobalStore.AllowRemote", false);
    if (!Config::TryGet("Engine.GlobalStore.DataDirectory", desc.DataDirectory) || desc.DataDirectory.IsEmpty())
    {
        desc.DataDirectory = Format("{0}/Saved/GlobalStore", Engine::GetEnvironment().EngineRoot);
    }
    desc.MemoryBudget = Config::GetOr<uint64>("Engine.GlobalStore.MemoryBudget", desc.MemoryBudget);
    (void)Config::TryGet("Engine.GlobalStore.Upstream.Host", desc.UpstreamHost);
    desc.UpstreamPort = Config::GetOr<uint16>("Engine.GlobalStore.Upstream.Port", GlobalStoreDefaultPort);

    int32 exitCode = 0;
    NetLoop loop;
    if (loop.Start(NetSubsystem::Threads))
    {
        GlobalStoreServer server(loop, Move(desc));
        if (server.Start())
        {
            MARLIN_LOG(LogCore, Info, "MarlinGlobalStore listens on port {0}", server.GetPort());
            // Started for a client (-ExitWhenIdle), it leaves once nothing has been connected for a while, so it holds
            // no engine files after the editor and indexer close (06 §3.3); started by hand, it stays.
            const bool exitWhenIdle = CommandLine::HasSwitch("ExitWhenIdle");
            const float64 idleSeconds = Config::GetOr<float64>("Engine.GlobalStore.IdleExitSeconds", 10);
            float64 idleSince = -1;
            while (!Engine::IsExitRequested())
            {
                server.Tick();
                if (exitWhenIdle)
                {
                    const float64 now = PlatformTime::Seconds();
                    if (server.GetConnectionCount() != 0)
                    {
                        idleSince = -1;
                    }
                    else if (idleSince < 0)
                    {
                        idleSince = now;
                    }
                    else if (now - idleSince >= idleSeconds)
                    {
                        MARLIN_LOG(LogCore, Info, "No client for {0} s; MarlinGlobalStore exits", idleSeconds);
                        break;
                    }
                }
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
