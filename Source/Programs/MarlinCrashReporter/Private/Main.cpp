#include "CrashAnalysis/CrashAnalysis.h"
#include "CrashWatcher.h"
#include "Engine/Engine.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
/// Prints the analysis of a minidump, for looking at a report by hand; symbols come from a symbol store when
/// one is given.
int32 AnalyzeDump(StringView dump)
{
    CrashAnalysisDesc desc;
    desc.Minidump = dump;
    StringView store;
    // RECOVERY-DECISION D10: reporter switch names -Analyze= and -SymbolStore= chosen.
    if (CommandLine::TryGetValue("SymbolStore", store))
    {
        desc.SymbolStore = store;
    }
    CrashAnalysisResult result;
    String error;
    StringBuilder<16384> text;
    const bool analyzed = CrashAnalyzer::Analyze(desc, result, error);
    if (analyzed)
    {
        CrashAnalyzer::WriteText(text, result);
    }
    else
    {
        text.Append(error).Append('\n');
    }
    PlatformProcess::WriteOutput(text.Data(), text.Length());
    return analyzed ? 0 : 1;
}
} // namespace
} // namespace Marlin

int main(int /*argc*/, const char *const * /*argv*/)
{
    using namespace Marlin;
    if (!ProgramHost::Initialize("MarlinCrashReporter"))
    {
        return 1;
    }
    int32 exitCode = 0;
    StringView dump;
    uint32 processId = 0;
    // RECOVERY-DECISION D10: reporter switch names -Analyze= and -SymbolStore= chosen.
    if (CommandLine::TryGetValue("Analyze", dump))
    {
        exitCode = AnalyzeDump(dump);
    }
    else if (CommandLine::TryGetValue("Pid", processId))
    {
        CrashWatcher watcher;
        if (watcher.Attach(processId))
        {
            watcher.Run();
        }
        else
        {
            exitCode = 1;
        }
    }
    else
    {
        exitCode = 1;
    }
    ProgramHost::Shutdown();
    return exitCode;
}
