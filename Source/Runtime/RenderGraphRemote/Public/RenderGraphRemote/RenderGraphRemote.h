#pragma once

#include "CoreTypes.h"
#include "Reflection/TypeOf.h"
#include "Remote/RemoteServer.h"
#include "String/String.h"
#include "RenderGraphRemote/RenderGraphRemote.gen.h"

namespace Marlin
{
/// The Render group of Remote methods RenderGraph serves (Documentation/Architecture/10).
MSTRUCT(Category = "Render")
struct RenderGraphRemoteMethods
{
    MGENERATED_BODY()

    /// The next graph that executes, compiled, as a RenderGraphExport: passes with their accesses, barriers and
    /// waits, resources with their placement, and submissions. The response comes once that graph has executed.
    MFUNCTION(Remote)
    static void Graph(RemoteCall &call);

    /// Captures the next graph with the injected capture tool, from the first pass named *first* through the next
    /// pass named *last*; false if there is no capture tool.
    MFUNCTION(Remote)
    static bool CapturePasses(const String &first, const String &last);
};
} // namespace Marlin
