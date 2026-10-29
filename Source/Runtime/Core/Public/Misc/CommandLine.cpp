#include "Misc/CommandLine.h"

#include "HAL/PlatformProcess.h"
#include "Memory/MemoryOps.h"

namespace Marlin
{
namespace
{
struct CommandLineState
{
    String Text;
    List<String> Arguments;
};

/// Built on first use and never destroyed.
CommandLineState &State()
{
    alignas(CommandLineState) static uint8 storage[sizeof(CommandLineState)];
    static CommandLineState *state = [] {
        auto *created = ConstructAt(reinterpret_cast<CommandLineState *>(storage));
        String full;
        const usize length = PlatformProcess::CommandLine(nullptr, 0);
        PlatformProcess::CommandLine(full.ResizeUninitialized(length), length + 1);
        List<String> arguments;
        CommandLine::Split(full, arguments);
        if (!arguments.IsEmpty())
        {
            arguments.RemoveAt(0);
        }
        for (const String &argument : arguments)
        {
            created->Text.Append(created->Text.IsEmpty() ? "" : " ").Append(argument);
        }
        created->Arguments = Move(arguments);
        return created;
    }();
    return *state;
}
} // namespace

void CommandLine::Set(StringView text)
{
    CommandLineState &state = State();
    state.Text = text;
    state.Arguments.Clear();
    Split(text, state.Arguments);
}

StringView CommandLine::Get()
{
    return State().Text;
}

Span<const String> CommandLine::GetArguments()
{
    return State().Arguments.AsSpan();
}

bool CommandLine::HasSwitch(StringView name)
{
    StringView value;
    for (const String &argument : GetArguments())
    {
        if (MatchSwitch(argument, name, value))
        {
            return true;
        }
    }
    return false;
}

bool CommandLine::TryGetValue(StringView name, StringView &value)
{
    bool found = false;
    ForEachValue(name, [&](StringView text) {
        value = text;
        found = true;
    });
    return found;
}

bool CommandLine::MatchSwitch(StringView argument, StringView name, StringView &value)
{
    if (!argument.StartsWith('-'))
    {
        return false;
    }
    argument = argument.RemoveStart(argument.StartsWith("--") ? 2 : 1);
    if (argument.Length() < name.Length() || !argument.Left(name.Length()).EqualsIgnoreCase(name))
    {
        return false;
    }
    const StringView rest = argument.RemoveStart(name.Length());
    if (rest.IsEmpty())
    {
        value = {};
        return true;
    }
    if (rest[0] != '=' && rest[0] != ':')
    {
        return false;
    }
    value = rest.RemoveStart(1);
    return true;
}

void CommandLine::Split(StringView text, List<String> &arguments)
{
    usize index = 0;
    while (index < text.Length())
    {
        while (index < text.Length() && Ascii::IsWhiteSpace(text[index]))
        {
            ++index;
        }
        if (index == text.Length())
        {
            break;
        }
        String argument;
        bool quoted = false;
        while (index < text.Length() && (quoted || !Ascii::IsWhiteSpace(text[index])))
        {
            const char c = text[index];
            if (c == '\\')
            {
                usize slashes = 0;
                while (index < text.Length() && text[index] == '\\')
                {
                    ++slashes;
                    ++index;
                }
                if (index < text.Length() && text[index] == '"')
                {
                    argument.Append(slashes / 2, '\\');
                    if (slashes % 2 != 0)
                    {
                        argument.Append('"');
                        ++index;
                    }
                }
                else
                {
                    argument.Append(slashes, '\\');
                }
                continue;
            }
            if (c == '"')
            {
                if (quoted && index + 1 < text.Length() && text[index + 1] == '"')
                {
                    argument.Append('"');
                    index += 2;
                    continue;
                }
                quoted = !quoted;
                ++index;
                continue;
            }
            argument.Append(c);
            ++index;
        }
        arguments.Add(Move(argument));
    }
}
} // namespace Marlin
