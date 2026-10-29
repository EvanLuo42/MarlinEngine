#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "String/String.h"
#include "ConfigTestTypes.gen.h"

namespace Marlin::Tests
{
MSTRUCT(Config = "Test.Renderer")
struct ConfigTestRenderer
{
    MGENERATED_BODY()

    MPROPERTY()
    static int32 ShadowQuality;
    MPROPERTY()
    static float32 Gamma;

    MPROPERTY()
    List<String> Features;
    MPROPERTY()
    String Backend;
};

/// Settings players may change in a shipped game.
MSTRUCT(Config = "Test.Settings", UserConfigurable)
struct ConfigTestSettings
{
    MGENERATED_BODY()

    MPROPERTY()
    int32 Volume = 0;
};

/// Marked UserConfigurable, but a cheat keeps players out of all of it.
MSTRUCT(Config = "Test.Cheats", UserConfigurable)
struct ConfigTestCheats
{
    MGENERATED_BODY()

    MPROPERTY(Flags = Cheat)
    int32 GodMode = 0;
};
} // namespace Marlin::Tests
