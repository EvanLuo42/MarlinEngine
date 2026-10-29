#include "RenderGraphRemote/RenderGraphRemote.h"

#include "Engine/Engine.h"
#include "Json/JsonSerializer.h"
#include "Json/JsonWriter.h"
#include "RHI/RhiSubsystem.h"
#include "Remote/RemoteServer.h"
#include "RenderGraph/RenderGraphExport.h"
#include "RenderGraph/RenderGraphSubsystem.h"
#include "String/StringBuilder.h"

namespace Marlin
{
void RenderGraphRemoteMethods::Graph(RemoteCall &call)
{
    RenderGraphSubsystem *graphs = Engine::GetSubsystem<RenderGraphSubsystem>();
    if (graphs == nullptr || graphs->GetDevice() == nullptr)
    {
        call.Fail("No render graphs run in this process"); // RECOVERY-DECISION D2
        return;
    }
    RemoteServer &server = call.GetServer();
    const RemoteDeferred deferred = call.Defer();
    graphs->RequestExport([&server, deferred](const RenderGraphExport &graph) {
        StringBuilder<4096> json;
        JsonWriter writer(json);
        Json::Write(writer, graph);
        server.Complete(deferred, json.AsView());
    });
}

bool RenderGraphRemoteMethods::CapturePasses(const String &first, const String &last)
{
    RenderGraphSubsystem *graphs = Engine::GetSubsystem<RenderGraphSubsystem>();
    const RhiSubsystem *rhi = Engine::GetSubsystem<RhiSubsystem>();
    if (graphs == nullptr || rhi == nullptr || rhi->GetCaptureTool() == nullptr)
    {
        return false;
    }
    graphs->RequestPassCapture(Name(first.AsView()), Name(last.AsView()));
    return true;
}
} // namespace Marlin
