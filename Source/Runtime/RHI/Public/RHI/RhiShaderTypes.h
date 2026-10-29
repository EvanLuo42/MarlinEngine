#pragma once

#include "CoreTypes.h"
#include "Reflection/TypeOf.h"
#include "RHI/RhiShaderTypes.gen.h"

namespace Marlin
{
/// A shader's `Span<T>` (MarlinLanguage.md §4.5) as C++ writes it into [Uniform] arguments or GPU memory: the device
/// address of the first element and the number of elements there. A shader's `Ptr<T>` is a plain uint64 address.
MSTRUCT()
struct alignas(8) ShaderSpan
{
    MGENERATED_BODY()

    MPROPERTY()
    uint64 Pointer = 0;
    MPROPERTY()
    uint32 Length = 0;
};
static_assert(sizeof(ShaderSpan) == 16, "a Span is 16 bytes in GPU memory (MarlinLanguage.md §10)");
} // namespace Marlin
