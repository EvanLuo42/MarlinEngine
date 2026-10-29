//! Fixed-width integer, floating-point and pointer-sized type aliases.
//!
//! The aliases name compiler-provided types, so engine code never includes `<cstdint>` or a platform SDK header
//! for them. Static assertions at the end of the header fix the sizes the rest of the engine assumes.

#pragma once

#include "HAL/Compiler.h"

namespace Marlin
{
/// A signed 8-bit integer.
using int8 = __INT8_TYPE__;
/// A signed 16-bit integer.
using int16 = __INT16_TYPE__;
/// A signed 32-bit integer.
using int32 = __INT32_TYPE__;
/// A signed 64-bit integer.
using int64 = __INT64_TYPE__;
/// An unsigned 8-bit integer.
using uint8 = __UINT8_TYPE__;
/// An unsigned 16-bit integer.
using uint16 = __UINT16_TYPE__;
/// An unsigned 32-bit integer.
using uint32 = __UINT32_TYPE__;
/// An unsigned 64-bit integer.
using uint64 = __UINT64_TYPE__;

/// A 32-bit floating-point value, aliasing `float`.
using float32 = float;
/// A 64-bit floating-point value, aliasing `double`.
using float64 = double;

/// An unsigned integer with the size of a pointer, for object sizes and indices.
using usize = __SIZE_TYPE__;
/// A signed integer with the size of a pointer, for differences and offsets.
using isize = __PTRDIFF_TYPE__;

/// A signed integer with the size of a pointer, for a pointer's value as an integer.
using intptr = __INTPTR_TYPE__;
/// An unsigned integer with the size of a pointer, for a pointer's value as an integer.
using uintptr = __UINTPTR_TYPE__;

static_assert(sizeof(int8) == 1 && sizeof(int16) == 2 && sizeof(int32) == 4 && sizeof(int64) == 8);
static_assert(sizeof(float32) == 4 && sizeof(float64) == 8);
static_assert(sizeof(usize) == sizeof(void *) && sizeof(uintptr) == sizeof(void *));
} // namespace Marlin
