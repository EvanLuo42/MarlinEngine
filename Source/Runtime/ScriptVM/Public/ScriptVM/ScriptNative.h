#pragma once

#include "Containers/HashMap.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"
#include "ScriptVM/ScriptTypes.h"
#include "Templates/TypeTraits.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// The text of a script string, which is UTF-8 (MarlinScript §9.8); empty for null.
[[nodiscard]] inline StringView GetScriptString(const ScriptArray *string)
{
    if (string == nullptr)
    {
        return {};
    }
    return StringView(reinterpret_cast<const char *>(string) + string->Type->DataOffset, string->Length);
}

/// A new string object of *domain* holding *text*.
[[nodiscard]] SCRIPTVM_API ScriptArray *NewScriptString(ScriptDomain &domain, StringView text);

/// What a native function sees of its call (MarlinScript §7.1): the parameter registers, the result register at the
/// start of the window, and the domain. References read here stay valid until the native allocates or calls a
/// script; read them again after that.
class SCRIPTVM_API ScriptCallContext
{
public:
    ScriptCallContext(ScriptDomain &domain, const ScriptFunctionInfo &function, uint8 *window)
        : _domain(&domain), _function(&function), _window(window)
    {
    }

    [[nodiscard]] ScriptDomain &GetDomain() const
    {
        return *_domain;
    }

    [[nodiscard]] const ScriptFunctionInfo &GetFunction() const
    {
        return *_function;
    }

    /// The first register of parameter *index*.
    [[nodiscard]] uint8 *GetParameter(uint32 index) const
    {
        return _window + _function->ParameterOffsets[index] * ScriptSlotBytes;
    }

    /// The first register of the result, which overlaps the first parameter.
    [[nodiscard]] uint8 *GetResult() const
    {
        return _window;
    }

    template <typename T> [[nodiscard]] T Get(uint32 index) const;
    template <typename T> void Return(const T &value);

    /// Ends the call with a fault (MarlinScript §8); the native should return right after.
    void Fault(ScriptFault kind, StringView message);

    [[nodiscard]] bool HasFaulted() const
    {
        return _faulted;
    }

    [[nodiscard]] ScriptFault GetFaultKind() const
    {
        return _faultKind;
    }

    [[nodiscard]] StringView GetFaultMessage() const
    {
        return _faultMessage.AsView();
    }

private:
    ScriptDomain *_domain;
    const ScriptFunctionInfo *_function;
    uint8 *_window;
    bool _faulted = false;
    ScriptFault _faultKind = ScriptFault::Native;
    String _faultMessage;
};

/// Converts between registers and the C++ types of native signatures and ScriptFunction calls.
template <typename T> struct ScriptMarshal;

template <typename T>
    requires Arithmetic<T>
struct ScriptMarshal<T>
{
    /// Registers hold integers narrower than 32 bits extended to 32.
    using Register = Conditional<(sizeof(T) < 4), Conditional<IsSigned<T>, int32, uint32>, T>;

    static T Read(const uint8 *slot)
    {
        Register value;
        MemCopy(&value, slot, sizeof(value));
        return static_cast<T>(value);
    }

    static void Write(ScriptDomain &, uint8 *slot, T value)
    {
        const Register wide = static_cast<Register>(value);
        MemCopy(slot, &wide, sizeof(wide));
    }
};

template <> struct ScriptMarshal<bool>
{
    static bool Read(const uint8 *slot)
    {
        uint32 value;
        MemCopy(&value, slot, sizeof(value));
        return value != 0;
    }

    static void Write(ScriptDomain &, uint8 *slot, bool value)
    {
        const uint32 wide = value ? 1 : 0;
        MemCopy(slot, &wide, sizeof(wide));
    }
};

/// A reference to an object, or the address an `out` or `ref` parameter points at.
template <typename T> struct ScriptMarshal<T *>
{
    static T *Read(const uint8 *slot)
    {
        T *value;
        MemCopy(&value, slot, sizeof(value));
        return value;
    }

    static void Write(ScriptDomain &, uint8 *slot, T *value)
    {
        MemCopy(slot, &value, sizeof(value));
    }
};

template <typename T> struct ScriptMarshal<const T *>
{
    static const T *Read(const uint8 *slot)
    {
        const T *value;
        MemCopy(&value, slot, sizeof(value));
        return value;
    }

    static void Write(ScriptDomain &, uint8 *slot, const T *value)
    {
        MemCopy(slot, &value, sizeof(value));
    }
};

/// A string, read as its text and written as a new string object.
template <> struct ScriptMarshal<StringView>
{
    static StringView Read(const uint8 *slot)
    {
        return GetScriptString(ScriptMarshal<ScriptArray *>::Read(slot));
    }

    static void Write(ScriptDomain &domain, uint8 *slot, StringView value)
    {
        ScriptMarshal<ScriptArray *>::Write(domain, slot, NewScriptString(domain, value));
    }
};

template <> struct ScriptMarshal<String>
{
    static String Read(const uint8 *slot)
    {
        return String(ScriptMarshal<StringView>::Read(slot));
    }

    static void Write(ScriptDomain &domain, uint8 *slot, const String &value)
    {
        ScriptMarshal<StringView>::Write(domain, slot, value.AsView());
    }
};

template <typename T> T ScriptCallContext::Get(uint32 index) const
{
    return ScriptMarshal<T>::Read(GetParameter(index));
}

template <typename T> void ScriptCallContext::Return(const T &value)
{
    ScriptMarshal<T>::Write(*_domain, GetResult(), value);
}

/// A registered native: the thunk the VM calls and what it calls it with.
struct ScriptNativeEntry
{
    ScriptNativeThunk Thunk = nullptr;
    const void *Target = nullptr;
};

namespace Private
{
template <typename TResult, typename... TArgs, usize... TIndices>
void CallScriptNative(ScriptCallContext &context, const void *target, IndexSequence<TIndices...>)
{
    const auto function = BitCast<TResult (*)(TArgs...)>(target);
    if constexpr (IsSame<TResult, void>)
    {
        function(context.Get<Decay<TArgs>>(static_cast<uint32>(TIndices))...);
    }
    else
    {
        const TResult result = function(context.Get<Decay<TArgs>>(static_cast<uint32>(TIndices))...);
        if (!context.HasFaulted())
        {
            context.Return(result);
        }
    }
}

template <typename TResult, typename... TArgs, usize... TIndices>
void CallScriptNativeWithContext(ScriptCallContext &context, const void *target, IndexSequence<TIndices...>)
{
    const auto function = BitCast<TResult (*)(ScriptCallContext &, TArgs...)>(target);
    if constexpr (IsSame<TResult, void>)
    {
        function(context, context.Get<Decay<TArgs>>(static_cast<uint32>(TIndices))...);
    }
    else
    {
        const TResult result = function(context, context.Get<Decay<TArgs>>(static_cast<uint32>(TIndices))...);
        if (!context.HasFaulted())
        {
            context.Return(result);
        }
    }
}

template <typename TResult, typename... TArgs> void ScriptNativeThunkOf(ScriptCallContext &context, const void *target)
{
    CallScriptNative<TResult, TArgs...>(context, target, MakeIndexSequence<sizeof...(TArgs)>{});
}

template <typename TResult, typename... TArgs>
void ScriptNativeThunkWithContextOf(ScriptCallContext &context, const void *target)
{
    CallScriptNativeWithContext<TResult, TArgs...>(context, target, MakeIndexSequence<sizeof...(TArgs)>{});
}
} // namespace Private

/// The native functions scripts link to by name (MarlinScript §7.1). A plain C++ function is registered with its
/// signature; one taking a ScriptCallContext first may also fault and reach the domain.
class SCRIPTVM_API ScriptNativeRegistry
{
public:
    /// Registers *thunk*, called with *target*; a name registered before is replaced.
    void Register(StringView name, ScriptNativeThunk thunk, const void *target = nullptr);

    template <typename TResult, typename... TArgs> void Register(StringView name, TResult (*function)(TArgs...))
    {
        Register(name, &Private::ScriptNativeThunkOf<TResult, TArgs...>, BitCast<const void *>(function));
    }

    template <typename TResult, typename... TArgs>
    void Register(StringView name, TResult (*function)(ScriptCallContext &, TArgs...))
    {
        Register(name, &Private::ScriptNativeThunkWithContextOf<TResult, TArgs...>, BitCast<const void *>(function));
    }

    [[nodiscard]] const ScriptNativeEntry *Find(StringView name) const
    {
        return _entries.Find(name);
    }

private:
    HashMap<String, ScriptNativeEntry> _entries;
};
} // namespace Marlin
