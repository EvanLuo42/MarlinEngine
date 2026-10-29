//! Maps C++ types and supported containers to their `TypeInfo`.

#pragma once

#include "Containers/Array.h"
#include "Containers/FrozenArray.h"
#include "Containers/HashMap.h"
#include "Containers/HashSet.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Hash/Hash.h"
#include "Memory/MemoryOps.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Reflection/TypeInfo.h"
#include "String/Name.h"
#include "String/String.h"
#include "Templates/Optional.h"
#include "Templates/TypeTraits.h"

namespace Marlin
{
namespace Private
{
template <typename T>
concept ReflectedType = requires { typename T::MarlinReflectedType; } && IsSame<typename T::MarlinReflectedType, T>;

template <typename T>
concept ReflectedInterface =
    ReflectedType<T> && requires { typename T::MarlinInterfaceType; } && IsSame<typename T::MarlinInterfaceType, T>;

template <typename T>
concept ReflectedEnum = IsEnum<T> && requires(T *tag) {
    { MarlinStaticType(tag) } -> SameAs<const TypeInfo &>;
};

CORE_API const TypeInfo &GetBuiltinType(const bool *tag);
CORE_API const TypeInfo &GetBuiltinType(const int8 *tag);
CORE_API const TypeInfo &GetBuiltinType(const int16 *tag);
CORE_API const TypeInfo &GetBuiltinType(const int32 *tag);
CORE_API const TypeInfo &GetBuiltinType(const int64 *tag);
CORE_API const TypeInfo &GetBuiltinType(const uint8 *tag);
CORE_API const TypeInfo &GetBuiltinType(const uint16 *tag);
CORE_API const TypeInfo &GetBuiltinType(const uint32 *tag);
CORE_API const TypeInfo &GetBuiltinType(const uint64 *tag);
CORE_API const TypeInfo &GetBuiltinType(const float32 *tag);
CORE_API const TypeInfo &GetBuiltinType(const float64 *tag);
CORE_API const TypeInfo &GetBuiltinType(const String *tag);
CORE_API const TypeInfo &GetBuiltinType(const StringView *tag);
CORE_API const TypeInfo &GetBuiltinType(const Name *tag);

template <typename T>
concept BuiltinType = requires(const T *tag) { GetBuiltinType(tag); };

/// Returns the canonical `TypeInfo` of a type described at run time, creating it on first use; `key` tells C++ types
/// apart, so every module that instantiates a container shares one TypeInfo for it.
CORE_API const TypeInfo &InternType(const TypeDescription &description, uint64 key);

/// A hash of T's spelling, the same in every module.
template <typename T> consteval uint64 TypeKey()
{
    return RapidHash(__PRETTY_FUNCTION__, sizeof(__PRETTY_FUNCTION__) - 1);
}

template <typename T> struct ContainerType
{
    static_assert(!IsSame<T, T>, "Type is not reflected: mark it MSTRUCT, MCLASS or MENUM, or specialize "
                                 "Marlin::Private::ContainerType for it.");
};
} // namespace Private

/// Returns the `TypeInfo` of `T`.
///
/// `T` may be a reflected struct, class, interface or enum, a builtin, or one of the supported containers (`List`,
/// `Array`, `HashMap`, `HashSet`, `Optional`, `UniquePtr`, `SharedPtr`, `FrozenArray`, `FrozenString`). An unsupported
/// type fails to compile with a static assertion.
///
/// # Examples
///
/// ```cpp
/// const TypeInfo &type = TypeOf<int32>();
/// bool isInt = type.GetKind() == TypeKind::Int32; // -> true
/// ```
template <typename T> [[nodiscard]] const TypeInfo &TypeOf()
{
    using TType = RemoveCv<T>;
    if constexpr (Private::ReflectedType<TType>)
    {
        return TType::StaticType();
    }
    else if constexpr (Private::ReflectedEnum<TType>)
    {
        return MarlinStaticType(static_cast<TType *>(nullptr));
    }
    else if constexpr (Private::BuiltinType<TType>)
    {
        return Private::GetBuiltinType(static_cast<const TType *>(nullptr));
    }
    else
    {
        return Private::ContainerType<TType>::Get();
    }
}

namespace Private
{
template <typename T> struct OperationsFor
{
    static constexpr TypeOperations Value{
        .Construct =
            [] {
                if constexpr (IsConstructible<T> && !IsAbstract<T>)
                {
                    return [](void *target) { ConstructAt(static_cast<T *>(target)); };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .Destruct =
            [] {
                if constexpr (!IsAbstract<T>)
                {
                    return [](void *target) { DestroyAt(static_cast<T *>(target)); };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .CopyConstruct =
            [] {
                if constexpr (IsConstructible<T, const T &> && !IsAbstract<T>)
                {
                    return [](void *target, const void *source) {
                        ConstructAt(static_cast<T *>(target), *static_cast<const T *>(source));
                    };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .MoveConstruct =
            [] {
                if constexpr (IsConstructible<T, T &&> && !IsAbstract<T>)
                {
                    return [](void *target, void *source) {
                        ConstructAt(static_cast<T *>(target), Move(*static_cast<T *>(source)));
                    };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .CopyAssign =
            [] {
                if constexpr (IsAssignable<T &, const T &>)
                {
                    return [](void *target, const void *source) {
                        *static_cast<T *>(target) = *static_cast<const T *>(source);
                    };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .MoveAssign =
            [] {
                if constexpr (IsAssignable<T &, T &&>)
                {
                    return
                        [](void *target, void *source) { *static_cast<T *>(target) = Move(*static_cast<T *>(source)); };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .Equals =
            [] {
                if constexpr (requires(const T &a, const T &b) {
                                  { a == b } -> ConvertibleTo<bool>;
                              })
                {
                    return [](const void *a, const void *b) -> bool {
                        return *static_cast<const T *>(a) == *static_cast<const T *>(b);
                    };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .Hash =
            [] {
                if constexpr (Hashable<T>)
                {
                    return [](const void *value) -> uint64 { return Hasher<T>::Get(*static_cast<const T *>(value)); };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .Format =
            [] {
                if constexpr (requires(const T &value, StringWriter &output) { value.FormatTo(output, StringView()); })
                {
                    return [](StringWriter &output, const void *value) {
                        static_cast<const T *>(value)->FormatTo(output, StringView());
                    };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .Parse =
            [] {
                if constexpr (requires(StringView text, T &value) {
                                  { T::TryParse(text, value) } -> ConvertibleTo<bool>;
                              })
                {
                    return [](StringView text, void *value) -> bool {
                        return T::TryParse(text, *static_cast<T *>(value));
                    };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .New =
            [] {
                if constexpr (IsConstructible<T> && !IsAbstract<T>)
                {
                    return []() -> void * { return new T(); };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .Delete =
            [] {
                if constexpr (!IsAbstract<T> || __has_virtual_destructor(T))
                {
                    return [](void *object) { delete static_cast<T *>(object); };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .GetDynamicType =
            [] {
                if constexpr (requires(const T &value) {
                                  { value.GetType() } -> SameAs<const TypeInfo &>;
                              })
                {
                    return [](const void *object) -> const TypeInfo & {
                        return static_cast<const T *>(object)->GetType();
                    };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .TriviallyCopyable = IsTriviallyCopyable<T>,
        .TriviallyDestructible = IsTriviallyDestructible<T>,
    };
};

/// Arrays apply their element's operations to each element.
template <typename T, usize TCount> struct OperationsFor<T[TCount]>
{
    using TArray = T[TCount];

    static constexpr TypeOperations Value{
        .Construct = [](void *target) { DefaultConstructRange(*static_cast<TArray *>(target), TCount); },
        .Destruct = [](void *target) { DestroyRange(*static_cast<TArray *>(target), TCount); },
        .CopyConstruct =
            [](void *target, const void *source) {
                CopyConstructRange(*static_cast<TArray *>(target), *static_cast<const TArray *>(source), TCount);
            },
        .MoveConstruct =
            [](void *target, void *source) {
                MoveConstructRange(*static_cast<TArray *>(target), *static_cast<TArray *>(source), TCount);
            },
        .CopyAssign =
            [] {
                if constexpr (IsAssignable<T &, const T &>)
                {
                    return [](void *target, const void *source) {
                        for (usize i = 0; i < TCount; ++i)
                        {
                            (*static_cast<TArray *>(target))[i] = (*static_cast<const TArray *>(source))[i];
                        }
                    };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .MoveAssign = nullptr,
        .Equals =
            [] {
                if constexpr (requires(const T &a, const T &b) {
                                  { a == b } -> ConvertibleTo<bool>;
                              })
                {
                    return [](const void *a, const void *b) -> bool {
                        for (usize i = 0; i < TCount; ++i)
                        {
                            if (!((*static_cast<const TArray *>(a))[i] == (*static_cast<const TArray *>(b))[i]))
                            {
                                return false;
                            }
                        }
                        return true;
                    };
                }
                else
                {
                    return nullptr;
                }
            }(),
        .Hash = nullptr,
        .Format = nullptr,
        .Parse = nullptr,
        .New = nullptr,
        .Delete = nullptr,
        .GetDynamicType = nullptr,
        .TriviallyCopyable = IsTriviallyCopyable<T>,
        .TriviallyDestructible = IsTriviallyDestructible<T>,
    };
};

template <typename T> constexpr TypeDescription DescribeContainer(StringView name, TypeKind kind)
{
    return {
        .Name = name,
        .FullName = name,
        .Kind = kind,
        .Size = sizeof(T),
        .Alignment = alignof(T),
        .Operations = &OperationsFor<T>::Value,
    };
}

template <typename TContainer, typename TDescriber> const TypeInfo &InternContainer()
{
    static const TypeInfo &type = InternType(TDescriber::Describe(), TypeKey<TContainer>());
    return type;
}

template <typename T, typename TAllocator> struct ContainerType<List<T, TAllocator>>
{
    using TList = List<T, TAllocator>;

    static constexpr ListOperations Operations{
        .Count = [](const void *list) { return static_cast<const TList *>(list)->Count(); },
        .At = [](void *list, usize index) -> void * { return &(*static_cast<TList *>(list))[index]; },
        .Resize = [](void *list, usize count) { static_cast<TList *>(list)->Resize(count); },
        .RemoveAt = [](void *list, usize index) { static_cast<TList *>(list)->RemoveAt(index); },
    };

    static constexpr TypeDescription Describe()
    {
        TypeDescription description = DescribeContainer<TList>("List", TypeKind::List);
        description.Element = &TypeOf<T>;
        description.ContainerOperations = &Operations;
        return description;
    }

    static const TypeInfo &Get()
    {
        return InternContainer<TList, ContainerType>();
    }
};

template <typename T, usize TCount> struct FixedArrayType
{
    using TArray = T[TCount];

    static constexpr TypeDescription Describe()
    {
        TypeDescription description = DescribeContainer<TArray>("Array", TypeKind::FixedArray);
        description.Element = &TypeOf<T>;
        description.FixedCount = static_cast<uint32>(TCount);
        return description;
    }
};

template <typename T, usize TCount> struct ContainerType<T[TCount]>
{
    static const TypeInfo &Get()
    {
        return InternContainer<T[TCount], FixedArrayType<T, TCount>>();
    }
};

template <typename T, usize TCount> struct ContainerType<Array<T, TCount>>
{
    static_assert(sizeof(Array<T, TCount>) == sizeof(T[TCount]));

    static const TypeInfo &Get()
    {
        return InternContainer<T[TCount], FixedArrayType<T, TCount>>();
    }
};

template <typename TKey, typename TValue> struct ContainerType<HashMap<TKey, TValue>>
{
    using TMap = HashMap<TKey, TValue>;

    static constexpr MapOperations Operations{
        .Count = [](const void *map) { return static_cast<const TMap *>(map)->Count(); },
        .Clear = [](void *map) { static_cast<TMap *>(map)->Clear(); },
        .FindOrAdd = [](void *map, const void *key) -> void * {
            return &static_cast<TMap *>(map)->FindOrAdd(*static_cast<const TKey *>(key));
        },
        .Find = [](void *map, const void *key) -> void * {
            return static_cast<TMap *>(map)->Find(*static_cast<const TKey *>(key));
        },
        .Remove = [](void *map,
                     const void *key) { return static_cast<TMap *>(map)->Remove(*static_cast<const TKey *>(key)); },
        .ForEach =
            [](const void *map, void (*visitor)(const void *key, const void *value, void *context), void *context) {
                for (const auto &entry : *static_cast<const TMap *>(map))
                {
                    visitor(&entry.Key, &entry.Value, context);
                }
            },
    };

    static constexpr TypeDescription Describe()
    {
        TypeDescription description = DescribeContainer<TMap>("Map", TypeKind::Map);
        description.Key = &TypeOf<TKey>;
        description.Element = &TypeOf<TValue>;
        description.ContainerOperations = &Operations;
        return description;
    }

    static const TypeInfo &Get()
    {
        return InternContainer<TMap, ContainerType>();
    }
};

template <typename TKey> struct ContainerType<HashSet<TKey>>
{
    using TSet = HashSet<TKey>;

    static constexpr SetOperations Operations{
        .Count = [](const void *set) { return static_cast<const TSet *>(set)->Count(); },
        .Clear = [](void *set) { static_cast<TSet *>(set)->Clear(); },
        .Add = [](void *set,
                  const void *key) { return static_cast<TSet *>(set)->Add(*static_cast<const TKey *>(key)); },
        .Contains =
            [](const void *set, const void *key) {
                return static_cast<const TSet *>(set)->Contains(*static_cast<const TKey *>(key));
            },
        .Remove = [](void *set,
                     const void *key) { return static_cast<TSet *>(set)->Remove(*static_cast<const TKey *>(key)); },
        .ForEach =
            [](const void *set, void (*visitor)(const void *key, void *context), void *context) {
                for (const TKey &key : *static_cast<const TSet *>(set))
                {
                    visitor(&key, context);
                }
            },
    };

    static constexpr TypeDescription Describe()
    {
        TypeDescription description = DescribeContainer<TSet>("Set", TypeKind::Set);
        description.Key = &TypeOf<TKey>;
        description.Element = &TypeOf<TKey>;
        description.ContainerOperations = &Operations;
        return description;
    }

    static const TypeInfo &Get()
    {
        return InternContainer<TSet, ContainerType>();
    }
};

template <typename T> struct ContainerType<Optional<T>>
{
    using TOptional = Optional<T>;

    static constexpr OptionalOperations Operations{
        .HasValue = [](const void *optional) { return static_cast<const TOptional *>(optional)->HasValue(); },
        .Get = [](void *optional) -> void * { return static_cast<TOptional *>(optional)->TryGet(); },
        .Emplace = [](void *optional) -> void * { return &static_cast<TOptional *>(optional)->Emplace(); },
        .Reset = [](void *optional) { static_cast<TOptional *>(optional)->Reset(); },
    };

    static constexpr TypeDescription Describe()
    {
        TypeDescription description = DescribeContainer<TOptional>("Optional", TypeKind::Optional);
        description.Element = &TypeOf<T>;
        description.ContainerOperations = &Operations;
        return description;
    }

    static const TypeInfo &Get()
    {
        return InternContainer<TOptional, ContainerType>();
    }
};

template <typename T> struct ContainerType<UniquePtr<T>>
{
    using TPointer = UniquePtr<T>;

    static constexpr PointerOperations Operations{
        .Get = [](const void *pointer) -> void * { return static_cast<const TPointer *>(pointer)->Get(); },
        .Reset = [](void *pointer, void *object) { static_cast<TPointer *>(pointer)->Reset(static_cast<T *>(object)); },
    };

    static constexpr TypeDescription Describe()
    {
        TypeDescription description = DescribeContainer<TPointer>("UniquePtr", TypeKind::UniquePtr);
        description.Element = &TypeOf<T>;
        description.ContainerOperations = &Operations;
        return description;
    }

    static const TypeInfo &Get()
    {
        return InternContainer<TPointer, ContainerType>();
    }
};

template <typename T> struct ContainerType<SharedPtr<T>>
{
    using TPointer = SharedPtr<T>;

    static constexpr PointerOperations Operations{
        .Get = [](const void *pointer) -> void * { return static_cast<const TPointer *>(pointer)->Get(); },
        .Reset = [](void *pointer,
                    void *object) { *static_cast<TPointer *>(pointer) = TPointer(static_cast<T *>(object)); },
    };

    static constexpr TypeDescription Describe()
    {
        TypeDescription description = DescribeContainer<TPointer>("SharedPtr", TypeKind::SharedPtr);
        description.Element = &TypeOf<T>;
        description.ContainerOperations = &Operations;
        return description;
    }

    static const TypeInfo &Get()
    {
        return InternContainer<TPointer, ContainerType>();
    }
};
template <typename T> struct ContainerType<FrozenArray<T>>
{
    using TArray = FrozenArray<T>;

    static constexpr ListOperations Operations{
        .Count = [](const void *list) { return static_cast<const TArray *>(list)->Count(); },
        .At = [](void *list, usize index) -> void * { return &(*static_cast<TArray *>(list))[index]; },
        .Resize = [](void *list, usize count) { static_cast<TArray *>(list)->Resize(count); },
        .RemoveAt = [](void *list, usize index) { static_cast<TArray *>(list)->RemoveAt(index); },
    };

    static constexpr TypeDescription Describe()
    {
        TypeDescription description = DescribeContainer<TArray>("FrozenArray", TypeKind::List);
        description.Element = &TypeOf<T>;
        description.ContainerOperations = &Operations;
        description.Extension = &FrozenArrayLayout;
        return description;
    }

    static const TypeInfo &Get()
    {
        return InternContainer<TArray, ContainerType>();
    }
};

template <> struct ContainerType<FrozenString>
{
    static constexpr TypeDescription Describe()
    {
        TypeDescription description = DescribeContainer<FrozenString>("FrozenString", TypeKind::Opaque);
        description.Extension = &FrozenStringLayout;
        return description;
    }

    static const TypeInfo &Get()
    {
        return InternContainer<FrozenString, ContainerType>();
    }
};
} // namespace Private

/// Casts a reflected object to a derived class or an implemented interface, or returns null.
///
/// A cast to a base class is a static conversion and always succeeds for a non-null object. Downcasts and interface
/// casts use the object's dynamic type.
template <typename TTarget, typename TSource> [[nodiscard]] TTarget *Cast(TSource *object)
{
    if constexpr (IsBaseOf<TTarget, TSource>)
    {
        return object;
    }
    else
    {
        if (object == nullptr)
        {
            return nullptr;
        }
        void *target = object->GetType().CastTo(Private::RemoveConst(object), TypeOf<TTarget>());
        return static_cast<TTarget *>(target);
    }
}
} // namespace Marlin
