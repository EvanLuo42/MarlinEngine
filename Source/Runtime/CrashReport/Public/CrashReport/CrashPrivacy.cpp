#include "CrashReport/CrashPrivacy.h"

#include "HAL/PlatformSystem.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Templates/Algorithm.h"

namespace Marlin::CrashPrivacy
{
namespace
{
constexpr usize MinNameLength = 3;

bool MatchesAt(StringView text, usize at, StringView pattern)
{
    if (pattern.IsEmpty() || text.Length() - at < pattern.Length())
    {
        return false;
    }
    for (usize i = 0; i < pattern.Length(); ++i)
    {
        if (Ascii::ToLower(text[at + i]) != Ascii::ToLower(pattern[i]))
        {
            return false;
        }
    }
    return true;
}

bool IsWordAt(StringView text, usize at, usize length)
{
    const bool before = at > 0 && Ascii::IsLetterOrDigit(text[at - 1]);
    const bool after = at + length < text.Length() && Ascii::IsLetterOrDigit(text[at + length]);
    return !before && !after;
}

String Query(usize (*query)(char *, usize))
{
    char buffer[256];
    const usize length = query(buffer, sizeof(buffer));
    return length < sizeof(buffer) ? String(buffer, length) : String();
}

String HomeFolder()
{
    char buffer[1024];
    const usize length = PlatformSystem::GetKnownFolder(KnownFolder::Home, buffer, sizeof(buffer));
    return length < sizeof(buffer) ? String(buffer, length) : String();
}

/// The name of switch *token*, such as Map for `-Map`, `-Map=Town` or `"-Map=Town Hall"`; empty if it is no switch.
StringView SwitchName(StringView token, bool &hasValue)
{
    const usize start = token.StartsWith('"') ? 1 : 0;
    if (token.Length() <= start + 1 || token[start] != '-')
    {
        return {};
    }
    const StringView rest = token.Substring(start + 1);
    const usize end = rest.IndexOfAny("=:\"");
    hasValue = end != StringView::NotFound && rest[end] != '"';
    return rest.Left(end);
}

bool IsKept(StringView name, Span<const StringView> kept)
{
    for (const StringView candidate : kept)
    {
        if (name.EqualsIgnoreCase(candidate))
        {
            return true;
        }
    }
    return false;
}
} // namespace

List<CrashScrubPattern> MakeLocalPatterns()
{
    List<CrashScrubPattern> patterns;
    const String home = HomeFolder();
    if (home.Length() >= MinNameLength)
    {
        String backslashes = home;
        backslashes.Replace('/', '\\');
        patterns.Add({.Text = home, .Placeholder = "<Home>"});
        if (backslashes != home)
        {
            patterns.Add({.Text = Move(backslashes), .Placeholder = "<Home>"});
        }
    }
    // RECOVERY-DECISION D14: the computer name is matched only in its UNC, URL and user@host forms.
    const String computer = Query(&PlatformSystem::GetComputerName);
    if (computer.Length() >= MinNameLength)
    {
        patterns.Add({.Text = Format("\\\\{0}", computer), .Placeholder = "\\\\<Computer>", .WholeWord = true});
        patterns.Add({.Text = Format("//{0}", computer), .Placeholder = "//<Computer>", .WholeWord = true});
        patterns.Add({.Text = Format("@{0}", computer), .Placeholder = "@<Computer>", .WholeWord = true});
    }
    const String user = Query(&PlatformSystem::GetUserName);
    if (user.Length() >= MinNameLength)
    {
        patterns.Add({.Text = user, .Placeholder = "<User>", .WholeWord = true});
    }
    return patterns;
}

String Scrub(StringView text, Span<const CrashScrubPattern> patterns)
{
    StringBuilder<1024> output;
    usize i = 0;
    while (i < text.Length())
    {
        bool replaced = false;
        for (const CrashScrubPattern &pattern : patterns)
        {
            const usize length = pattern.Text.Length();
            if (MatchesAt(text, i, pattern.Text) && (!pattern.WholeWord || IsWordAt(text, i, length)))
            {
                output.Append(pattern.Placeholder);
                i += length;
                replaced = true;
                break;
            }
        }
        if (!replaced)
        {
            output.Append(text[i]);
            ++i;
        }
    }
    return output.ToString();
}

String FilterCommandLine(StringView commandLine, Span<const StringView> kept)
{
    StringBuilder<1024> output;
    bool keepNext = false;
    usize i = 0;
    while (i < commandLine.Length())
    {
        while (i < commandLine.Length() && Ascii::IsWhiteSpace(commandLine[i]))
        {
            ++i;
        }
        const usize start = i;
        bool quoted = false;
        while (i < commandLine.Length() && (quoted || !Ascii::IsWhiteSpace(commandLine[i])))
        {
            quoted = commandLine[i] == '"' ? !quoted : quoted;
            ++i;
        }
        const StringView token = commandLine.Substring(start, i - start);
        if (token.IsEmpty())
        {
            break;
        }
        bool hasValue = false;
        const StringView name = SwitchName(token, hasValue);
        StringView piece;
        bool removed = false;
        if (name.IsEmpty())
        {
            piece = keepNext ? token : StringView();
            keepNext = false;
        }
        else if (IsKept(name, kept))
        {
            piece = token;
            keepNext = !hasValue;
        }
        else
        {
            piece = hasValue ? token.Left(static_cast<usize>(name.Data() - token.Data()) + name.Length()) : token;
            removed = hasValue;
            keepNext = false;
        }
        if (piece.IsEmpty())
        {
            continue;
        }
        if (!output.IsEmpty())
        {
            output.Append(' ');
        }
        output.Append(piece);
        if (removed)
        {
            output.Append("=<removed>");
        }
    }
    return output.ToString();
}
} // namespace Marlin::CrashPrivacy
