#include "CrashReport/CrashReportInfo.h"

#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Reflection/TypeOf.h"
#include "String/StringBuilder.h"

namespace Marlin::CrashReport
{
String WriteInfo(const CrashReportInfo &info)
{
    StringBuilder<2048> text;
    MarlinTextSerializer::WriteObject(text, TypeOf<CrashReportInfo>(), &info);
    text.Append('\n');
    return text.ToString();
}

bool ReadInfo(StringView text, CrashReportInfo &info, String &errors)
{
    MarlinTextDocument document;
    MarlinTextDiagnostics diagnostics;
    if (document.Parse(text, diagnostics))
    {
        for (usize i = 0; i < document.GetObjectCount(); ++i)
        {
            const MarlinTextObject *object = document.GetObject(i, diagnostics);
            if (object != nullptr && object->GetTypeName() == "CrashReportInfo")
            {
                const bool read =
                    MarlinTextSerializer::ReadObject(*object, TypeOf<CrashReportInfo>(), &info, diagnostics);
                errors = read ? String() : diagnostics.ToString();
                return read;
            }
        }
    }
    errors = diagnostics.HasErrors() ? diagnostics.ToString() : String("No CrashReportInfo object");
    return false;
}

StringView GetKindName(CrashKind kind)
{
    switch (kind)
    {
    case CrashKind::Exception:
        return "Exception";
    case CrashKind::Failure:
        return "Failure";
    case CrashKind::Hang:
        return "Hang";
    case CrashKind::AbnormalExit:
        return "AbnormalExit";
    }
    return "Unknown";
}
} // namespace Marlin::CrashReport
