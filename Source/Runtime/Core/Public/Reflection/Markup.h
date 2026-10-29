//! The reflection markup macros read by MarlinHeaderTool.
//!
//! They produce no code themselves; `marlin generate` expands them into the `.gen.h` and `.gen.cpp` of the header.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
class TypeInfo;

namespace Private
{
/// Generated code specializes this for each reflected type; MGENERATED_BODY makes it a friend.
template <typename T> struct TypeRegistration;
} // namespace Private
} // namespace Marlin

/// Marks a value type for reflection.
///
/// Arguments are metadata, written as `Key` or `Key=Value`.
#define MSTRUCT(...)
/// Marks a polymorphic class for reflection; its objects know their type through `GetType`.
///
/// Arguments are metadata, written as `Key` or `Key=Value`.
#define MCLASS(...)
/// Marks an abstract base that classes implement.
///
/// Arguments are metadata, written as `Key` or `Key=Value`.
#define MINTERFACE(...)
/// Marks an enum for reflection.
///
/// It needs a fixed underlying type unless it is scoped. Arguments are metadata, written as `Key` or `Key=Value`.
#define MENUM(...)
/// Marks a field for reflection; a static one is a global, such as a console variable.
///
/// Arguments are metadata, written as `Key` or `Key=Value`.
#define MPROPERTY(...)
/// Marks a method for reflection, callable through `FunctionInfo::Invoke`.
///
/// Arguments are metadata, written as `Key` or `Key=Value`.
#define MFUNCTION(...)

/// Opens every reflected struct, class and interface; it must be the first thing in the body.
///
/// The header's generated `.gen.h`, its last include, defines what it expands to.
#define MGENERATED_BODY() MARLIN_CONCAT(MARLIN_CONCAT(MARLIN_GENERATED_, MARLIN_FILE_ID), MARLIN_CONCAT(_, __LINE__))

/// Declares `GetType` without `override`, which the generated body cannot know.
///
/// The pragmas keep a class that marks its other overrides from warning about it.
#define MARLIN_DYNAMIC_TYPE()                                                                                          \
    _Pragma("clang diagnostic push")                                                                                   \
        _Pragma("clang diagnostic ignored \"-Winconsistent-missing-override\"") virtual const ::Marlin::TypeInfo &     \
        GetType() const                                                                                                \
    {                                                                                                                  \
        return StaticType();                                                                                           \
    }                                                                                                                  \
    _Pragma("clang diagnostic pop")
