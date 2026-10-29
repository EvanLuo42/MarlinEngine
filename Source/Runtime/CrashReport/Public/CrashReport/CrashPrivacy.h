#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
/// Text to replace in a crash report, matched ignoring case.
struct CrashScrubPattern
{
    String Text;
    StringView Placeholder;
    /// Only where no letter or digit touches either end, for names that may be part of other words.
    bool WholeWord = false;
};

/// Removes what identifies the user from crash reports before they leave the machine: the user, computer and home
/// folder names, and the values of command-line switches.
namespace CrashPrivacy
{
/// The home folder, user name and computer name of this machine, each in the forms paths and network names use.
/// Names shorter than three characters are left out, as they would match too much.
[[nodiscard]] CRASHREPORT_API List<CrashScrubPattern> MakeLocalPatterns();
/// *text* with every match of *patterns* replaced by its placeholder; earlier patterns win.
[[nodiscard]] CRASHREPORT_API String Scrub(StringView text, Span<const CrashScrubPattern> patterns);
/// *commandLine* without the executable, with the values of switches not in *kept* replaced by `<removed>` and the
/// arguments that follow such switches dropped.
[[nodiscard]] CRASHREPORT_API String FilterCommandLine(StringView commandLine, Span<const StringView> kept);
} // namespace CrashPrivacy
} // namespace Marlin
