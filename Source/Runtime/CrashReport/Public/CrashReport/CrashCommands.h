#pragma once

#include "CoreTypes.h"
#include "String/String.h"
#include "CrashReport/CrashCommands.gen.h"

namespace Marlin
{
/// Console commands that test crash reporting.
MCLASS()
class CRASHREPORT_API CrashCommands
{
    MGENERATED_BODY()
public:
    virtual ~CrashCommands() = default;

    /// Crashes the calling thread on purpose: AccessViolation, Assert, Fatal, StackOverflow or Hang. Does nothing in
    /// Shipping.
    MFUNCTION(ConsoleCommand = "Debug.Crash")
    static void Crash(const String &kind);
};
} // namespace Marlin
