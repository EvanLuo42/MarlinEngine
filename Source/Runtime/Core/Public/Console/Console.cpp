#include "Console/Console.h"

#include "Containers/HashMap.h"
#include "Diagnostics/AssertFormat.h"
#include "HAL/PlatformSync.h"
#include "Memory/Memory.h"
#include "Memory/UniquePtr.h"
#include "Reflection/Reflection.h"
#include "String/Ascii.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
uint64 HashIgnoringCase(StringView name)
{
    StringBuilder<128> lower;
    for (const char c : name)
    {
        lower.Append(Ascii::ToLower(c));
    }
    return lower.AsView().GetHash();
}

class ConsoleRegistry
{
public:
    /// Built on first use and never destroyed, so variables stay valid through static destruction.
    static ConsoleRegistry &Get()
    {
        alignas(ConsoleRegistry) static uint8 storage[sizeof(ConsoleRegistry)];
        static ConsoleRegistry *registry = ConstructAt(reinterpret_cast<ConsoleRegistry *>(storage));
        return *registry;
    }

    void Register(const TypeInfo &type)
    {
        for (const FieldInfo &field : type.GetOwnFields())
        {
            const Optional<StringView> name = field.GetMetadata().Get("ConsoleVariable");
            if (!name.HasValue())
            {
                continue;
            }
            if (!field.IsStatic() || name.Value().IsEmpty())
            {
                MARLIN_FATALF("{0}.{1}: a console variable must be a static field with a name", type.GetFullName(),
                              field.GetName());
            }
            const ScopeLock lock(_lock);
            AddUnique(name.Value());
            _variables.Add(HashIgnoringCase(name.Value()), MakeUnique<ConsoleVariable>(name.Value(), field));
        }
        for (const FunctionInfo &function : type.GetOwnFunctions())
        {
            const Optional<StringView> name = function.GetMetadata().Get("ConsoleCommand");
            if (!name.HasValue())
            {
                continue;
            }
            if (!function.IsStatic() || name.Value().IsEmpty())
            {
                MARLIN_FATALF("{0}.{1}: a console command must be a static function with a name", type.GetFullName(),
                              function.GetName());
            }
            const ScopeLock lock(_lock);
            AddUnique(name.Value());
            _commands.Add(HashIgnoringCase(name.Value()), MakeUnique<ConsoleCommand>(name.Value(), function));
        }
    }

    ConsoleVariable *FindVariable(StringView name)
    {
        const ScopeLock lock(_lock);
        UniquePtr<ConsoleVariable> *variable = _variables.Find(HashIgnoringCase(name));
        return variable != nullptr && (*variable)->GetName().EqualsIgnoreCase(name) ? variable->Get() : nullptr;
    }

    const ConsoleCommand *FindCommand(StringView name)
    {
        const ScopeLock lock(_lock);
        UniquePtr<ConsoleCommand> *command = _commands.Find(HashIgnoringCase(name));
        return command != nullptr && (*command)->GetName().EqualsIgnoreCase(name) ? command->Get() : nullptr;
    }

    void ForEachVariable(FunctionRef<void(ConsoleVariable &)> visitor)
    {
        List<ConsoleVariable *> variables;
        {
            const ScopeLock lock(_lock);
            for (const auto &entry : _variables)
            {
                variables.Add(entry.Value.Get());
            }
        }
        for (ConsoleVariable *variable : variables)
        {
            visitor(*variable);
        }
    }

    void ForEachCommand(FunctionRef<void(const ConsoleCommand &)> visitor)
    {
        List<const ConsoleCommand *> commands;
        {
            const ScopeLock lock(_lock);
            for (const auto &entry : _commands)
            {
                commands.Add(entry.Value.Get());
            }
        }
        for (const ConsoleCommand *command : commands)
        {
            visitor(*command);
        }
    }

private:
    void AddUnique(StringView name) const
    {
        const uint64 hash = HashIgnoringCase(name);
        if (_variables.Contains(hash) || _commands.Contains(hash))
        {
            MARLIN_FATALF("Console name {0} is registered twice", name);
        }
    }

    Mutex _lock;
    HashMap<uint64, UniquePtr<ConsoleVariable>> _variables;
    HashMap<uint64, UniquePtr<ConsoleCommand>> _commands;
};

/// Splits on spaces; a "quoted" argument keeps its spaces and drops the quotes.
List<StringView> SplitArguments(StringView text)
{
    List<StringView> arguments;
    usize i = 0;
    while (i < text.Length())
    {
        while (i < text.Length() && Ascii::IsWhiteSpace(text[i]))
        {
            ++i;
        }
        if (i == text.Length())
        {
            break;
        }
        if (text[i] == '"')
        {
            const usize start = ++i;
            while (i < text.Length() && text[i] != '"')
            {
                ++i;
            }
            arguments.Add(text.Substring(start, i - start));
            ++i;
            continue;
        }
        const usize start = i;
        while (i < text.Length() && !Ascii::IsWhiteSpace(text[i]))
        {
            ++i;
        }
        arguments.Add(text.Substring(start, i - start));
    }
    return arguments;
}

/// A value of a reflected type in heap storage.
class TemporaryValue
{
public:
    explicit TemporaryValue(const TypeInfo &type)
        : _type(type), _storage(Memory::Allocate(type.GetSize() != 0 ? type.GetSize() : 1, type.GetAlignment()))
    {
    }

    ~TemporaryValue()
    {
        if (_constructed)
        {
            _type.GetOperations().Destruct(_storage);
        }
        Memory::Free(_storage);
    }

    MARLIN_NONCOPYABLE(TemporaryValue);

    bool Construct()
    {
        if (_type.GetOperations().Construct == nullptr || _type.GetOperations().Destruct == nullptr)
        {
            return false;
        }
        _type.GetOperations().Construct(_storage);
        _constructed = true;
        return true;
    }

    void MarkConstructed()
    {
        _constructed = true;
    }

    [[nodiscard]] void *Get() const
    {
        return _storage;
    }

private:
    const TypeInfo &_type;
    void *_storage;
    bool _constructed = false;
};
} // namespace

namespace Private
{
void RegisterConsoleObjects(const TypeInfo &type)
{
    if (type.HasFields())
    {
        ConsoleRegistry::Get().Register(type);
    }
}
} // namespace Private

ConsoleVariable::ConsoleVariable(StringView name, const FieldInfo &field) : _name(name), _field(field)
{
    StringBuilder<64> value;
    FormatValue(value);
    _default = value.AsView();
}

StringView ConsoleVariable::GetHelp() const
{
    return _field.GetMetadata().GetOr("Tooltip", {});
}

bool ConsoleVariable::HasFlag(StringView flag) const
{
    return _field.GetMetadata().HasFlag("Flags", flag);
}

void ConsoleVariable::FormatValue(StringWriter &output) const
{
    Reflection::FormatValue(output, GetType(), GetValue());
}

String ConsoleVariable::ToString() const
{
    StringBuilder<64> value;
    FormatValue(value);
    return value.ToString();
}

bool ConsoleVariable::Set(StringView text, ConsoleSetBy setBy)
{
    if (setBy < _setBy || !Reflection::ParseValue(text, GetType(), GetValue()))
    {
        return false;
    }
    _setBy = setBy;
    NotifyChanged();
    return true;
}

void ConsoleVariable::Reset()
{
    (void)Reflection::ParseValue(_default, GetType(), GetValue());
    _setBy = ConsoleSetBy::Default;
    NotifyChanged();
}

void ConsoleVariable::AddChangedHandler(ChangedHandler handler)
{
    _handlers.Add(Move(handler));
}

void ConsoleVariable::NotifyChanged()
{
    for (const ChangedHandler &handler : _handlers)
    {
        handler(*this);
    }
}

ConsoleCommand::ConsoleCommand(StringView name, const FunctionInfo &function) : _name(name), _function(function)
{
}

StringView ConsoleCommand::GetHelp() const
{
    return _function.GetMetadata().GetOr("Tooltip", {});
}

bool ConsoleCommand::HasFlag(StringView flag) const
{
    return _function.GetMetadata().HasFlag("Flags", flag);
}

bool ConsoleCommand::Execute(StringView arguments, StringWriter &output) const
{
    const Span<const ParameterInfo> parameters = _function.GetParameters();
    const List<StringView> values = SplitArguments(arguments);
    if (values.Count() != parameters.Count())
    {
        output.Append(_name).Append(" takes ");
        for (usize i = 0; i < parameters.Count(); ++i)
        {
            output.Append(i == 0 ? "" : " ").Append(parameters[i].Name);
        }
        output.Append(parameters.IsEmpty() ? "no arguments" : "");
        return false;
    }

    List<UniquePtr<TemporaryValue>> storage;
    List<void *> pointers;
    for (usize i = 0; i < parameters.Count(); ++i)
    {
        const TypeInfo &type = parameters[i].Type();
        UniquePtr<TemporaryValue> &value = storage.Add(MakeUnique<TemporaryValue>(type));
        if (!value->Construct() || !Reflection::ParseValue(values[i], type, value->Get()))
        {
            output.Append(parameters[i].Name)
                .Append(": '")
                .Append(values[i])
                .Append("' is not a ")
                .Append(type.GetName());
            return false;
        }
        pointers.Add(value->Get());
    }

    const TypeInfo *returnType = _function.GetReturnType();
    TemporaryValue result(returnType != nullptr ? *returnType : TypeOf<bool>());
    _function.Invoke(nullptr, pointers.Data(), result.Get());
    if (returnType != nullptr)
    {
        result.MarkConstructed();
        Reflection::FormatValue(output, *returnType, result.Get());
    }
    return true;
}

namespace Console
{
ConsoleVariable *FindVariable(StringView name)
{
    return ConsoleRegistry::Get().FindVariable(name);
}

const ConsoleCommand *FindCommand(StringView name)
{
    return ConsoleRegistry::Get().FindCommand(name);
}

void ForEachVariable(FunctionRef<void(ConsoleVariable &)> visitor)
{
    ConsoleRegistry::Get().ForEachVariable(visitor);
}

void ForEachCommand(FunctionRef<void(const ConsoleCommand &)> visitor)
{
    ConsoleRegistry::Get().ForEachCommand(visitor);
}

bool Execute(StringView line, StringWriter &output, ConsoleSetBy setBy)
{
    line = line.Trim();
    StringView name = line;
    StringView arguments;
    const usize space = line.IndexOfAny(" \t");
    if (space != StringView::NotFound)
    {
        name = line.Left(space);
        arguments = line.RemoveStart(space).Trim();
    }
    if (ConsoleVariable *variable = FindVariable(name))
    {
        if (arguments.IsEmpty())
        {
            output.Append(variable->GetName()).Append(" = ");
            variable->FormatValue(output);
            return true;
        }
        if (arguments.Length() >= 2 && arguments.First() == '"' && arguments.Last() == '"')
        {
            arguments = arguments.Substring(1, arguments.Length() - 2);
        }
        if (variable->HasFlag("ReadOnly") && setBy == ConsoleSetBy::Console)
        {
            output.Append(variable->GetName()).Append(" is read-only");
            return false;
        }
        if (MARLIN_BUILD_SHIPPING && variable->HasFlag("Cheat") && setBy == ConsoleSetBy::Console)
        {
            output.Append(variable->GetName()).Append(" is a cheat");
            return false;
        }
        if (!variable->Set(arguments, setBy))
        {
            output.Append(variable->GetName()).Append(": cannot set '").Append(arguments).Append('\'');
            return false;
        }
        output.Append(variable->GetName()).Append(" = ");
        variable->FormatValue(output);
        return true;
    }
    if (const ConsoleCommand *command = FindCommand(name))
    {
        if (MARLIN_BUILD_SHIPPING && command->HasFlag("Cheat") && setBy == ConsoleSetBy::Console)
        {
            output.Append(command->GetName()).Append(" is a cheat");
            return false;
        }
        return command->Execute(arguments, output);
    }
    output.Append("Unknown console variable or command '").Append(name).Append('\'');
    return false;
}
} // namespace Console
} // namespace Marlin
