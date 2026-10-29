#include "AssetPipeline/AssetIndexService.h"
#include "AssetPipeline/AssetScanner.h"
#include "Config/Config.h"
#include "Engine/Engine.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Net/NetLoop.h"
#include "Net/NetSubsystem.h"
#include "Shader/GlobalShader.h"
#include "ShaderPipeline/ShaderCompiler.h"
#include "String/Format.h"
#include "Vfs/Vfs.h"

int main(int /*argc*/, const char *const * /*argv*/)
{
    using namespace Marlin;
    if (!ProgramHost::Initialize("MarlinAssetIndexer",
                                 ProgramFeatures::Vfs | ProgramFeatures::Project | ProgramFeatures::Config))
    {
        return 1;
    }
    const EngineEnvironment &environment = Engine::GetEnvironment();
    if (!environment.HasProject())
    {
        MARLIN_LOG(LogAssetIndex, Error, "MarlinAssetIndexer needs -Project=<path to .mproject>");
        ProgramHost::Shutdown();
        return 1;
    }

    AssetIndexServiceDesc desc;
    desc.StoreDirectory = Format("{0}/Intermediate/AssetIndex", environment.ProjectRoot);
    desc.PipeName = AssetIndexer::PipeNameFor(environment.ProjectRoot.AsView());
    desc.ProjectFile = environment.ProjectFile;
    uint32 converters = 0;
    desc.MaxConverters = Config::TryGet("Engine.Converter.Workers", converters) ? converters : 4;
    StringView seed;
    if (CommandLine::TryGetValue("Seed", seed))
    {
        desc.Seed = seed;
    }
    desc.Folders = GetProjectContentFolders();

    (void)PlatformFile::CreateDirectory(Format("{0}/Intermediate", environment.ProjectRoot).CStr());
    (void)PlatformFile::CreateDirectory(desc.StoreDirectory.CStr());
    const String lockPath = Format("{0}/Indexer.lock", desc.StoreDirectory);
    const FileHandle lock = PlatformFile::Open(lockPath.CStr(), FileAccess::Write, FileMode::CreateAlways,
                                               FileFlags::Exclusive | FileFlags::DeleteOnClose);
    if (!lock.IsValid())
    {
        MARLIN_LOG(LogAssetIndex, Info, "Another MarlinAssetIndexer serves {0}", environment.ProjectName);
        ProgramHost::Shutdown();
        return 0;
    }

    int32 exitCode = 0;
    {
        NetLoop loop;
        (void)loop.Start(NetSubsystem::Threads);
        AssetIndexService service(loop, Move(desc));
        if (service.Start())
        {
            MARLIN_LOG(LogAssetIndex, Info, "MarlinAssetIndexer serves {0} on {1}", environment.ProjectName,
                       service.GetServer().GetPipeName());
            // The warm step (MarlinLanguage.md §1.6): compile the global shaders for this machine in the background,
            // so the first Shader.Locate of the editor or game finds them in the DDC.
            List<String> entryPoints;
            String errors;
            if (GetGlobalShaderEntryPoints(entryPoints, errors))
            {
                List<ShaderLocateParams> shaders;
                for (const String &entryPoint : entryPoints)
                {
                    ShaderLocateParams &shader = shaders.AddDefaulted();
                    shader.EntryPoint = entryPoint;
                    shader.Uuid = GetGlobalShaderGuid(entryPoint.AsView());
                    shader.Target.Platform = environment.Platform;
                    shader.Urgent = false;
                }
                service.WarmShaders(shaders.AsSpan());
            }
            else
            {
                MARLIN_LOG(LogAssetIndex, Warning, "Global shaders are not warmed: {0}", errors);
            }
            // Started for a client (-ExitWhenIdle), it leaves once no client has needed it for a while, so it holds
            // no files after the editor or game closes; started by hand, it stays.
            const bool exitWhenIdle = CommandLine::HasSwitch("ExitWhenIdle");
            float64 idleSeconds = 10;
            (void)Config::TryGet("Engine.AssetIndex.IdleExitSeconds", idleSeconds);
            float64 idleSince = -1;
            while (!Engine::IsExitRequested())
            {
                service.Tick();
                Log::Flush();
                if (exitWhenIdle)
                {
                    const float64 now = PlatformTime::Seconds();
                    if (!service.IsIdle())
                    {
                        idleSince = -1;
                    }
                    else if (idleSince < 0)
                    {
                        idleSince = now;
                    }
                    else if (now - idleSince >= idleSeconds)
                    {
                        MARLIN_LOG(LogAssetIndex, Info, "No client for {0} s; MarlinAssetIndexer exits", idleSeconds);
                        break;
                    }
                }
                PlatformThread::Sleep(5);
            }
        }
        else
        {
            exitCode = 1;
        }
        service.Stop();
    }
    PlatformFile::Close(lock);
    ProgramHost::Shutdown();
    return exitCode;
}
