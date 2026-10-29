//! Process-wide platform initialization and shutdown.

#pragma once

#include "CoreTypes.h"

namespace Marlin::Platform
{
/// Prepares the process for the engine: verifies that the CPU supports the instruction set the build
/// requires, switches the console to UTF-8, starts the socket layer, suppresses the OS error dialogs, and
/// reserves stack for the crash handler.
///
/// Called once by `Engine::Startup` before subsystems are created. Exits the process when the CPU lacks a
/// required feature.
///
/// # Thread safety
/// Must run before any other engine thread uses platform services.
CORE_API void Initialize();
/// Releases the process-wide state set up by [Initialize]: stops the socket layer and restores the previous
/// console code pages.
///
/// Called once by `Engine::Shutdown`, after the log is flushed and the crash handler is stopped.
CORE_API void Shutdown();
} // namespace Marlin::Platform
