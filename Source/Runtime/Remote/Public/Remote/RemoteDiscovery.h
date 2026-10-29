#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "String/String.h"
#include "Remote/RemoteDiscovery.gen.h"

namespace Marlin
{
/// What a process that serves Remote writes about itself, so tools such as MarlinMcp can find it.
MSTRUCT()
struct RemoteProcessInfo
{
    MGENERATED_BODY()

    MPROPERTY()
    uint32 ProcessId = 0;
    MPROPERTY()
    String Target;
    MPROPERTY()
    String EngineId;
    MPROPERTY()
    String Project;
    /// The named pipe to connect to.
    MPROPERTY()
    String Pipe;
    /// The TCP port, zero if it listens on none.
    MPROPERTY()
    uint32 Port = 0;
};

/// Process files live in <OS temp>/Marlin/Processes/<pid>.mtext, shared by every engine of the user.
namespace RemoteDiscovery
{
[[nodiscard]] REMOTE_API String GetDirectory();
REMOTE_API bool Register(const RemoteProcessInfo &info);
REMOTE_API void Unregister(uint32 processId);
/// The processes that serve Remote; files of processes that are gone are removed.
REMOTE_API void List(List<RemoteProcessInfo> &processes);
} // namespace RemoteDiscovery
} // namespace Marlin
