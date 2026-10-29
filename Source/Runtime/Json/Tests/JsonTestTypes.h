#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "Templates/Optional.h"
#include "JsonTestTypes.gen.h"

namespace Marlin::Tests
{
MENUM()
enum class JsonTestMode : uint8
{
    Windowed,
    Fullscreen,
    Borderless,
};

MSTRUCT(Tuple)
struct JsonTestSize
{
    MGENERATED_BODY()

    MPROPERTY()
    int32 Width = 0;
    MPROPERTY()
    int32 Height = 0;
};

MSTRUCT()
struct JsonTestSettings
{
    MGENERATED_BODY()

    /// The display name.
    MPROPERTY()
    String Name;
    MPROPERTY()
    JsonTestSize Size;
    MPROPERTY()
    JsonTestMode Mode = JsonTestMode::Windowed;
    MPROPERTY()
    uint64 Id = 0;
    MPROPERTY()
    List<String> Tags;
    MPROPERTY()
    HashMap<String, float32> Scales;
    MPROPERTY()
    HashMap<int32, String> ByIndex;
    MPROPERTY()
    Guid Owner;
    MPROPERTY()
    Optional<int32> Fallback;
    MPROPERTY(Transient)
    int32 Cached = 0;
};
} // namespace Marlin::Tests
