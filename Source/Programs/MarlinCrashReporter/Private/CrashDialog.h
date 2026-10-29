#pragma once

#include "CoreTypes.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
struct CrashDialogDesc
{
    /// The product's display name, or the target's name without a product; names the dialog.
    StringView ProductName;
    /// What happened, in one sentence.
    StringView Headline;
    StringView ReportDirectory;
};

/// Asks whether to send the report, with a box for what the user was doing and a button that opens the report
/// folder. True to send; *description* gets what the user wrote.
bool AskToSendReport(const CrashDialogDesc &desc, String &description);
} // namespace Marlin
