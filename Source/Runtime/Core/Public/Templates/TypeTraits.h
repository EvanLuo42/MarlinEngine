//! Type traits and concepts used instead of `<type_traits>`.
//!
//! Each trait maps to a clang builtin or is composed from one, so the header needs no standard library.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// The type `T` with its top-level `const` removed.
template <typename T> using RemoveConst = __remove_const(T);
/// The type `T` with its top-level `volatile` removed.
template <typename T> using RemoveVolatile = __remove_volatile(T);
/// The type `T` with its top-level `const` and `volatile` removed.
template <typename T> using RemoveCv = __remove_cv(T);
/// The type `T` with a reference removed; `T` itself when it is not a reference.
template <typename T> using RemoveReference = __remove_reference_t(T);
/// The type `T` with references and cv-qualifiers removed.
template <typename T> using RemoveCvRef = __remove_cvref(T);
/// The type `T` with one pointer level removed; `T` itself when it is not a pointer.
template <typename T> using RemovePointer = __remove_pointer(T);
/// The element type of the array `T`; `T` itself when it is not an array.
template <typename T> using RemoveExtent = __remove_extent(T);
/// The element type after removing every array extent from `T`.
template <typename T> using RemoveAllExtents = __remove_all_extents(T);
/// A pointer to `T`; a reference type yields a pointer to the referenced type.
template <typename T> using AddPointer = __add_pointer(T);
/// An lvalue reference to `T`, or `T` when it cannot be referenced, such as `void`.
template <typename T> using AddLvalueReference = __add_lvalue_reference(T);
/// An rvalue reference to `T`, or `T` when it cannot be referenced, such as `void`.
template <typename T> using AddRvalueReference = __add_rvalue_reference(T);
/// `T` with top-level `const` added.
template <typename T> using AddConst = const T;
/// The type an argument of type `T` takes when passed by value: references removed, arrays and functions
/// decayed to pointers, and cv-qualifiers dropped.
template <typename T> using Decay = __decay(T);
/// The underlying integer type of the enum `T`.
template <typename T> using UnderlyingType = __underlying_type(T);
/// The signed integer type with the same width as the integral `T`.
template <typename T> using MakeSigned = __make_signed(T);
/// The unsigned integer type with the same width as the integral `T`.
template <typename T> using MakeUnsigned = __make_unsigned(T);
/// The `TIndex`th type in `TTypes...`; `TIndex` must be in range.
template <usize TIndex, typename... TTypes> using TypeAt = __type_pack_element<TIndex, TTypes...>;

namespace Private
{
template <bool TCondition, typename TTrue, typename TFalse> struct ConditionalType
{
    using Type = TTrue;
};

template <typename TTrue, typename TFalse> struct ConditionalType<false, TTrue, TFalse>
{
    using Type = TFalse;
};

template <typename T> struct TypeIdentity
{
    using Type = T;
};

template <typename T> AddRvalueReference<T> DeclvalType() noexcept;
} // namespace Private

/// `TTrue` when `TCondition` is true, `TFalse` otherwise.
template <bool TCondition, typename TTrue, typename TFalse>
using Conditional = Private::ConditionalType<TCondition, TTrue, TFalse>::Type;
/// An alias that is exactly `T`, but blocks template argument deduction of `T` through a parameter.
///
/// Use it to make a trailing parameter non-deduced, so the caller must supply the type or value explicitly.
template <typename T> using TypeIdentity = Private::TypeIdentity<T>::Type;

/// Whether `T` and `TOther` are the same type.
template <typename T, typename TOther> inline constexpr bool IsSame = __is_same(T, TOther);
/// Whether `T` is `void`.
template <typename T> inline constexpr bool IsVoid = __is_void(T);
/// Whether `T` is const-qualified.
template <typename T> inline constexpr bool IsConst = __is_const(T);
/// Whether `T` is volatile-qualified.
template <typename T> inline constexpr bool IsVolatile = __is_volatile(T);
/// Whether `T` is an lvalue or rvalue reference.
template <typename T> inline constexpr bool IsReference = __is_reference(T);
/// Whether `T` is an lvalue reference.
template <typename T> inline constexpr bool IsLvalueReference = __is_lvalue_reference(T);
/// Whether `T` is an rvalue reference.
template <typename T> inline constexpr bool IsRvalueReference = __is_rvalue_reference(T);
/// Whether `T` is an object or function pointer; member pointers do not count.
template <typename T> inline constexpr bool IsPointer = __is_pointer(T);
/// Whether `T` is a pointer to a data member or to a member function.
template <typename T> inline constexpr bool IsMemberPointer = __is_member_pointer(T);
/// Whether `T` is an array type.
template <typename T> inline constexpr bool IsArray = __is_array(T);
/// Whether `T` is a function type.
template <typename T> inline constexpr bool IsFunction = __is_function(T);
/// Whether `T` is an enum type.
template <typename T> inline constexpr bool IsEnum = __is_enum(T);
/// Whether `T` is a scoped enum, declared with `enum class` or `enum struct`.
template <typename T> inline constexpr bool IsScopedEnum = __is_scoped_enum(T);
/// Whether `T` is a non-union class type.
template <typename T> inline constexpr bool IsClass = __is_class(T);
/// Whether `T` is a union type.
template <typename T> inline constexpr bool IsUnion = __is_union(T);
/// Whether `T` is `bool`, a character type or an integer type.
template <typename T> inline constexpr bool IsIntegral = __is_integral(T);
/// Whether `T` is a floating-point type.
template <typename T> inline constexpr bool IsFloatingPoint = __is_floating_point(T);
/// Whether `T` is an integral or floating-point type.
template <typename T> inline constexpr bool IsArithmetic = __is_arithmetic(T);
/// Whether `T` is arithmetic, an enum, a pointer, a member pointer or `decltype(nullptr)`.
template <typename T> inline constexpr bool IsScalar = __is_scalar(T);
/// Whether `T` is a signed arithmetic type.
template <typename T> inline constexpr bool IsSigned = __is_signed(T);
/// Whether `T` is an unsigned arithmetic type.
template <typename T> inline constexpr bool IsUnsigned = __is_unsigned(T);
/// Whether `T` is an empty class: a non-union class with no non-static data members, no virtual functions or
/// virtual bases, and no non-empty base classes.
template <typename T> inline constexpr bool IsEmpty = __is_empty(T);
/// Whether `T` is a class that cannot be derived from.
template <typename T> inline constexpr bool IsFinal = __is_final(T);
/// Whether `T` has at least one pure virtual function.
template <typename T> inline constexpr bool IsAbstract = __is_abstract(T);
/// Whether `T` declares or inherits a virtual function.
template <typename T> inline constexpr bool IsPolymorphic = __is_polymorphic(T);
/// Whether `T` is an aggregate.
template <typename T> inline constexpr bool IsAggregate = __is_aggregate(T);
/// Whether `T` has standard layout.
template <typename T> inline constexpr bool IsStandardLayout = __is_standard_layout(T);
/// Whether `T` is trivially copyable: its copy and move operations are trivial and its destructor is trivial.
template <typename T> inline constexpr bool IsTriviallyCopyable = __is_trivially_copyable(T);
/// Whether `T` has a trivial destructor.
template <typename T> inline constexpr bool IsTriviallyDestructible = __is_trivially_destructible(T);
/// Whether default-constructing `T` runs no user code.
template <typename T> inline constexpr bool IsTriviallyDefaultConstructible = __is_trivially_constructible(T);
/// Whether every distinct value of `T` has a distinct object representation, so `memcmp` equality is value
/// equality.
template <typename T> inline constexpr bool HasUniqueObjectRepresentations = __has_unique_object_representations(T);
/// Whether `T` can be constructed from `TArgs...`.
template <typename T, typename... TArgs> inline constexpr bool IsConstructible = __is_constructible(T, TArgs...);
/// Whether that construction is trivial.
template <typename T, typename... TArgs>
inline constexpr bool IsTriviallyConstructible = __is_trivially_constructible(T, TArgs...);
/// Whether a `TValue` can be assigned to a `T`.
template <typename T, typename TValue> inline constexpr bool IsAssignable = __is_assignable(T, TValue);
/// Whether a `TFrom` converts implicitly to a `TTo`.
template <typename TFrom, typename TTo> inline constexpr bool IsConvertible = __is_convertible(TFrom, TTo);
/// Whether `TDerived` is `TBase` or derives from it, regardless of access.
template <typename TBase, typename TDerived> inline constexpr bool IsBaseOf = __is_base_of(TBase, TDerived);

/// Whether moving a `T` to a new address and forgetting the old one equals copying its bytes.
///
/// Containers can then relocate elements with `memmove`. It defaults to `IsTriviallyCopyable`; specialize it for
/// types that own memory but hold no pointers into themselves, such as `List` and `String`.
template <typename T> inline constexpr bool IsTriviallyRelocatable = __is_trivially_copyable(T);

/// Whether a zero-filled `T` is a valid default-constructed `T`.
///
/// True for trivially default-constructible types and for pointers. `DefaultConstructRange` uses it to zero
/// memory instead of calling a constructor.
template <typename T> inline constexpr bool IsZeroConstructible = __is_trivially_constructible(T) || IsPointer<T>;

/// A constraint that accepts integral types.
template <typename T>
concept Integral = IsIntegral<T>;
/// A constraint that accepts signed integral types.
template <typename T>
concept SignedIntegral = IsIntegral<T> && IsSigned<T>;
/// A constraint that accepts unsigned integral types; `bool` qualifies because it is integral and not signed.
template <typename T>
concept UnsignedIntegral = IsIntegral<T> && !IsSigned<T>;
/// A constraint that accepts floating-point types.
template <typename T>
concept FloatingPoint = IsFloatingPoint<T>;
/// A constraint that accepts arithmetic types.
template <typename T>
concept Arithmetic = IsArithmetic<T>;
/// A constraint that accepts enum types.
template <typename T>
concept Enum = IsEnum<T>;
/// A constraint that accepts object and function pointer types.
template <typename T>
concept Pointer = IsPointer<T>;
/// A constraint that accepts two identical types.
template <typename T, typename TOther>
concept SameAs = IsSame<T, TOther> && IsSame<TOther, T>;
/// A constraint that accepts a `TFrom` implicitly convertible to a `TTo`.
template <typename TFrom, typename TTo>
concept ConvertibleTo = IsConvertible<TFrom, TTo>;
/// A constraint that accepts a class derived from `TBase`, publicly and without ambiguity.
template <typename TDerived, typename TBase>
concept DerivedFrom = IsBaseOf<TBase, TDerived> && IsConvertible<const volatile TDerived *, const volatile TBase *>;
/// A constraint that accepts a `T` constructible from `TArgs...`.
template <typename T, typename... TArgs>
concept ConstructibleFrom = IsConstructible<T, TArgs...>;
/// A constraint that accepts a default-constructible `T`.
template <typename T>
concept DefaultConstructible = IsConstructible<T>;
/// A constraint that accepts a `T` constructible from a `const T &`.
template <typename T>
concept CopyConstructible = IsConstructible<T, const T &>;
/// A constraint that accepts a `T` constructible from a `T &&`.
template <typename T>
concept MoveConstructible = IsConstructible<T, T &&>;
/// A constraint that accepts types whose `==` returns something convertible to `bool`.
template <typename T>
concept EqualityComparable = requires(const T &a, const T &b) {
    { a == b } -> ConvertibleTo<bool>;
};
/// A constraint that accepts types whose `<` returns something convertible to `bool`.
template <typename T>
concept LessThanComparable = requires(const T &a, const T &b) {
    { a < b } -> ConvertibleTo<bool>;
};

/// A constraint that accepts a `TFunction` callable with `TArgs...`.
template <typename TFunction, typename... TArgs>
concept Invocable = requires(TFunction &&function, TArgs &&...args) {
    static_cast<TFunction &&>(function)(static_cast<TArgs &&>(args)...);
};

/// The type produced by calling a `TFunction` with `TArgs...`.
template <typename TFunction, typename... TArgs>
using InvokeResult = decltype(Private::DeclvalType<TFunction>()(Private::DeclvalType<TArgs>()...));

/// A constraint that accepts a `TFunction` callable with `TArgs...` whose result converts to `TResult`.
template <typename TFunction, typename TResult, typename... TArgs>
concept InvocableReturning =
    Invocable<TFunction, TArgs...> && ConvertibleTo<InvokeResult<TFunction, TArgs...>, TResult>;
} // namespace Marlin
