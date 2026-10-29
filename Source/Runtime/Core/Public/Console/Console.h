//! The developer console: reflected variables and commands, the registry, and line execution.
//!
//! Variables and commands are declared with `MPROPERTY(ConsoleVariable=...)` and `MFUNCTION(ConsoleCommand=...)` on
//! static members of a reflected type.

#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Reflection/TypeInfo.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
class StringWriter;

/// Where a console variable's value came from.
///
/// A value replaces one from the same or a lower priority only; the enumerators are ordered from lowest to highest
/// priority.
enum class ConsoleSetBy : uint8
{
    /// The value from the field's initializer.
    Default,
    /// Set from the project's config.
    Config,
    /// Set from the command line.
    CommandLine,
    /// Set by engine code.
    Code,
    /// Set from the console.
    Console,
};

/// A static reflected field exposed as a console variable.
///
/// The field is marked with `MPROPERTY(ConsoleVariable="r.Shadow.Quality", Flags=Cheat|Scalability)`. Set it on the
/// game thread between frames; readers on other threads see the change at their own pace.
///
/// # Thread safety
/// `Set` and `Reset` are not synchronized with readers of the field.
class CORE_API ConsoleVariable
{
public:
    /// Called after the variable's value changes.
    using ChangedHandler = Function<void(const ConsoleVariable &)>;

    /// Creates a variable for `field`; `name` must outlive it.
    ///
    /// # Preconditions
    /// `field` is a static reflected field. The registry checks this when it registers the type.
    ConsoleVariable(StringView name, const FieldInfo &field);
    ~ConsoleVariable() = default;
    MARLIN_NONCOPYABLE(ConsoleVariable);

    /// Returns the name the variable was registered under.
    [[nodiscard]] StringView GetName() const
    {
        return _name;
    }

    /// Returns the reflected field.
    [[nodiscard]] const FieldInfo &GetField() const
    {
        return _field;
    }

    /// Returns the field's type.
    [[nodiscard]] const TypeInfo &GetType() const
    {
        return _field.GetType();
    }

    /// Returns the field's `Tooltip` metadata, which its first `///` paragraph provides.
    [[nodiscard]] StringView GetHelp() const;
    /// Returns whether the field's `Flags` metadata contains `flag`.
    [[nodiscard]] bool HasFlag(StringView flag) const;

    /// Returns where the current value came from.
    [[nodiscard]] ConsoleSetBy GetSetBy() const
    {
        return _setBy;
    }

    /// Returns a pointer to the field's storage.
    [[nodiscard]] void *GetValue() const
    {
        return _field.GetValue(static_cast<void *>(nullptr));
    }

    /// Formats the current value into `output`.
    void FormatValue(StringWriter &output) const;
    /// Returns the current value formatted as text.
    [[nodiscard]] String ToString() const;

    /// Parses `text` into the variable.
    ///
    /// Returns false if `text` is not a valid value or the current value came from a higher-priority source. On
    /// success, notifies the changed handlers.
    bool Set(StringView text, ConsoleSetBy setBy);
    /// Restores the value the variable had when it was registered and notifies the changed handlers.
    void Reset();

    /// Registers a handler called after every change; it stays registered for the variable's lifetime.
    void AddChangedHandler(ChangedHandler handler);

private:
    void NotifyChanged();

    StringView _name;
    const FieldInfo &_field;
    String _default;
    ConsoleSetBy _setBy = ConsoleSetBy::Default;
    List<ChangedHandler> _handlers;
};

/// A static reflected function exposed as a console command.
///
/// The function is marked with `MFUNCTION(ConsoleCommand="r.Recompile")`; its arguments are parsed from the command
/// line.
class CORE_API ConsoleCommand
{
public:
    /// Creates a command for `function`; `name` must outlive it.
    ///
    /// # Preconditions
    /// `function` is a static reflected function. The registry checks this when it registers the type.
    ConsoleCommand(StringView name, const FunctionInfo &function);
    ~ConsoleCommand() = default;
    MARLIN_NONCOPYABLE(ConsoleCommand);

    /// Returns the name the command was registered under.
    [[nodiscard]] StringView GetName() const
    {
        return _name;
    }

    /// Returns the reflected function.
    [[nodiscard]] const FunctionInfo &GetFunction() const
    {
        return _function;
    }

    /// Returns the function's `Tooltip` metadata, which its first `///` paragraph provides.
    [[nodiscard]] StringView GetHelp() const;
    /// Returns whether the function's `Flags` metadata contains `flag`.
    [[nodiscard]] bool HasFlag(StringView flag) const;

    /// Parses `arguments` and calls the function, writing its result or the error to `output`.
    ///
    /// Arguments are separated by spaces; a quoted argument keeps its spaces and drops the quotes. Returns false if
    /// the argument count or one of the values does not match a parameter.
    bool Execute(StringView arguments, StringWriter &output) const;

private:
    StringView _name;
    const FunctionInfo &_function;
};

namespace Private
{
/// Registers the console variables and commands of a reflected type; called as its module registers.
void RegisterConsoleObjects(const TypeInfo &type);
} // namespace Private

/// The process console registry.
///
/// Names are matched ignoring case.
namespace Console
{
/// Returns the variable named `name`, ignoring case, or null.
[[nodiscard]] CORE_API ConsoleVariable *FindVariable(StringView name);
/// Returns the command named `name`, ignoring case, or null.
[[nodiscard]] CORE_API const ConsoleCommand *FindCommand(StringView name);
/// Calls `visitor` for each registered variable.
///
/// The visitor runs without the registry lock.
CORE_API void ForEachVariable(FunctionRef<void(ConsoleVariable &)> visitor);
/// Calls `visitor` for each registered command.
///
/// The visitor runs without the registry lock.
CORE_API void ForEachCommand(FunctionRef<void(const ConsoleCommand &)> visitor);

/// Runs one console line.
///
/// `name` shows a variable, `name value` sets it, and `command arguments` calls a command. Writes the outcome to
/// `output`; returns false if the line named nothing or failed. `setBy` records where the value came from; the
/// `ReadOnly` and `Cheat` restrictions apply only when it is [`ConsoleSetBy::Console`], and a `Cheat` command is
/// also rejected in Shipping.
CORE_API bool Execute(StringView line, StringWriter &output, ConsoleSetBy setBy = ConsoleSetBy::Console);
} // namespace Console
} // namespace Marlin
