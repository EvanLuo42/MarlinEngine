//! Support templates that MarlinHeaderTool's generated `.gen.cpp` files are written against.
//!
//! Nothing else should include this header; it is not part of the reflection API.

#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Memory/MemoryOps.h"
#include "Reflection/Reflection.h"
#include "Templates/TypeTraits.h"
#include "Templates/Utility.h"

/// What MarlinHeaderTool's .gen.cpp files are written against; nothing else should include this.
namespace Marlin::Private
{
template <typename TDerived, typename TBase> consteval TypeGetter ParentOf()
{
    if constexpr (ReflectedType<TBase> && !ReflectedInterface<TBase>)
    {
        return &TypeOf<TBase>;
    }
    else
    {
        return nullptr;
    }
}

template <typename TDerived, typename TBase> consteval auto ToParentOf() -> void *(*)(void *)
{
    if constexpr (ReflectedType<TBase> && !ReflectedInterface<TBase>)
    {
        return [](void *object) -> void * { return static_cast<TBase *>(static_cast<TDerived *>(object)); };
    }
    else
    {
        return nullptr;
    }
}

/// An entry for each base that is a reflected interface; other bases give an empty entry, which is skipped.
template <typename TDerived, typename TBase> consteval InterfaceEntry InterfaceOf()
{
    if constexpr (ReflectedInterface<TBase>)
    {
        return {
            &TypeOf<TBase>,
            [](void *object) -> void * { return static_cast<TBase *>(static_cast<TDerived *>(object)); },
        };
    }
    else
    {
        return {nullptr, nullptr};
    }
}

template <typename T> struct FunctionTraits;

template <typename TResult, typename... TArguments> struct FunctionTraitsBase
{
    using Result = TResult;
    template <usize TIndex> using Argument = TypeAt<TIndex, TArguments...>;
    static constexpr usize ArgumentCount = sizeof...(TArguments);
};

template <typename TResult, typename... TArguments>
struct FunctionTraits<TResult (*)(TArguments...)> : FunctionTraitsBase<TResult, TArguments...>
{
    using Class = void;
    static constexpr bool IsStatic = true;
    static constexpr bool IsConst = false;
};

template <typename TResult, typename... TArguments>
struct FunctionTraits<TResult (*)(TArguments...) noexcept> : FunctionTraits<TResult (*)(TArguments...)>
{
};

template <typename TClass, typename TResult, typename... TArguments>
struct FunctionTraits<TResult (TClass::*)(TArguments...)> : FunctionTraitsBase<TResult, TArguments...>
{
    using Class = TClass;
    static constexpr bool IsStatic = false;
    static constexpr bool IsConst = false;
};

template <typename TClass, typename TResult, typename... TArguments>
struct FunctionTraits<TResult (TClass::*)(TArguments...) const> : FunctionTraitsBase<TResult, TArguments...>
{
    using Class = const TClass;
    static constexpr bool IsStatic = false;
    static constexpr bool IsConst = true;
};

template <typename TClass, typename TResult, typename... TArguments>
struct FunctionTraits<TResult (TClass::*)(TArguments...) noexcept> : FunctionTraits<TResult (TClass::*)(TArguments...)>
{
};

template <typename TClass, typename TResult, typename... TArguments>
struct FunctionTraits<TResult (TClass::*)(TArguments...) const noexcept>
    : FunctionTraits<TResult (TClass::*)(TArguments...) const>
{
};

template <auto TFunction> using FunctionTraitsOf = FunctionTraits<decltype(TFunction)>;

template <auto TFunction> consteval TypeGetter ReturnTypeOf()
{
    using TResult = FunctionTraitsOf<TFunction>::Result;
    if constexpr (IsVoid<TResult>)
    {
        return nullptr;
    }
    else
    {
        return &TypeOf<RemoveCvRef<TResult>>;
    }
}

template <auto TFunction, usize TIndex> consteval ParameterInfo ParameterOf(StringView name, StringView defaultValue)
{
    using TArgument = FunctionTraitsOf<TFunction>::template Argument<TIndex>;
    ParameterFlags flags = ParameterFlags::None;
    if constexpr (IsReference<TArgument>)
    {
        flags = IsConst<RemoveReference<TArgument>> ? ParameterFlags::Reference | ParameterFlags::Const
                                                    : ParameterFlags::Reference;
    }
    return {name, &TypeOf<RemoveCvRef<TArgument>>, flags, defaultValue};
}

template <auto TFunction> struct Invoker
{
    using TTraits = FunctionTraitsOf<TFunction>;

    static void Invoke(void *object, void *const *arguments, void *result)
    {
        InvokeWith(object, arguments, result, MakeIndexSequence<TTraits::ArgumentCount>());
    }

private:
    template <usize TIndex> static decltype(auto) ArgumentAt(void *const *arguments)
    {
        using TArgument = TTraits::template Argument<TIndex>;
        return static_cast<TArgument>(*static_cast<RemoveCvRef<TArgument> *>(arguments[TIndex]));
    }

    template <usize... TIndices>
    static void InvokeWith([[maybe_unused]] void *object, [[maybe_unused]] void *const *arguments,
                           [[maybe_unused]] void *result, IndexSequence<TIndices...> /*indices*/)
    {
        auto call = [&]() -> decltype(auto) {
            if constexpr (TTraits::IsStatic)
            {
                return TFunction(ArgumentAt<TIndices>(arguments)...);
            }
            else
            {
                return (static_cast<TTraits::Class *>(object)->*TFunction)(ArgumentAt<TIndices>(arguments)...);
            }
        };
        using TResult = TTraits::Result;
        if constexpr (IsVoid<TResult>)
        {
            call();
        }
        else
        {
            ConstructAt(static_cast<RemoveCvRef<TResult> *>(result), call());
        }
    }
};

/// Collects a module's types for Register_<Module>.
class CORE_API ModuleRegistrar
{
public:
    explicit ModuleRegistrar(StringView module);
    ~ModuleRegistrar() = default;
    MARLIN_NONCOPYABLE(ModuleRegistrar);

    void Add(TypeInfo &type);
    /// Registers the collected types, then links them to their parents and registers their console variables.
    void Commit();

private:
    StringView _module;
    List<TypeInfo *> _types;
};
} // namespace Marlin::Private
