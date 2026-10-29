#pragma once

#include "CoreTypes.h"
#include "CrashReport/CrashProtocol.h"
#include "String/StringView.h"

namespace Marlin
{
/// Uploads the report folder *directory* to the MarlinCrashServer at *host*:*port* and waits for its answer; false if
/// the server could not be reached or did not answer.
bool UploadReport(StringView directory, StringView host, uint16 port, CrashUploadReply &reply);
} // namespace Marlin
