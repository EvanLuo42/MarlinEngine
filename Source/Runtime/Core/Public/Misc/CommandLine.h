//! The process command line and its switches, split with Windows quoting rules.
//!
//! The argument list is built from the OS command line on first use and can be replaced for tools and tests.

#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "String/Number.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
/// The process command line without the executable, split into arguments on first use.
///
/// A switch is `-Name`, `-Name=Value` or `-Name:Value`; two leading dashes are accepted, and names are matched
/// ignoring ASCII case. `-Config:Section.Key=Value` is the switch `Config` with the value `Section.Key=Value`.
/// Other arguments are positional.
///
/// # Thread safety
/// The state is built on first use and shared. Reading is safe from any thread, but [`Set`] must be called before
/// other threads read the command line.
class CORE_API CommandLine
{
public:
    /// Replaces the command line, for tools and tests; `text` excludes the executable.
    ///
    /// Splits `text` immediately and invalidates any span returned by [`GetArguments`]. Call it before other threads
    /// read the command line.
    static void Set(StringView text);

    /// Returns the command line text, valid until the next [`Set`].
    ///
    /// When the state is built from the OS command line, this is the parsed arguments joined by single spaces, so
    /// the original quoting is not preserved; after `Set`, it is exactly the text that was passed.
    [[nodiscard]] static StringView Get();

    /// Returns every argument, with quotes removed.
    ///
    /// The span is valid until the next [`Set`].
    [[nodiscard]] static Span<const String> GetArguments();

    /// Returns whether any argument is the switch `name`, with or without a value.
    [[nodiscard]] static bool HasSwitch(StringView name);

    /// Returns the value of the last `name` switch that has a non-empty value.
    ///
    /// The value is everything after the first `=` or `:`. Returns false, leaving `value` unchanged, when no switch
    /// provides a value.
    [[nodiscard]] static bool TryGetValue(StringView name, StringView &value);

    /// Returns the value of the last `name` switch parsed as a number.
    ///
    /// Returns false when the switch is absent or the value does not parse; `value` is unchanged in either case.
    template <typename T>
        requires(Arithmetic<T>)
    [[nodiscard]] static bool TryGetValue(StringView name, T &value)
    {
        StringView text;
        return TryGetValue(name, text) && TryParse(text, value);
    }

    /// Calls `visitor` with the value of every `name` switch that has a non-empty value, in argument order.
    template <typename TVisitor> static void ForEachValue(StringView name, TVisitor visitor)
    {
        for (const String &argument : GetArguments())
        {
            StringView value;
            if (MatchSwitch(argument, name, value) && !value.IsEmpty())
            {
                visitor(value);
            }
        }
    }

    /// Splits `text` by Windows rules and appends the arguments to `arguments`.
    ///
    /// White space separates arguments, double quotes group text, an odd number of backslashes escapes a following
    /// quote (each pair becomes one backslash), and `""` inside quotes is a literal quote. `arguments` is not
    /// cleared first.
    static void Split(StringView text, List<String> &arguments);

    /// Returns whether `argument` is the switch `name`, storing what follows `=` or `:` in `value`.
    ///
    /// A leading `-` or `--` is accepted, and the name is matched ignoring ASCII case. `value` becomes empty when
    /// the switch has no separator and is left unchanged when the function returns false.
    static bool MatchSwitch(StringView argument, StringView name, StringView &value);
};
} // namespace Marlin
