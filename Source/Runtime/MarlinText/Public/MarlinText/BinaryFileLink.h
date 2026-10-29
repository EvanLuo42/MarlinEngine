#pragma once

#include "CoreTypes.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "MarlinText/BinaryFileLink.gen.h"

namespace Marlin
{
/// A field that links a binary source file (Documentation/Architecture/06 §1.1), written `` `source:Rock.png#GUID` ``:
/// the GUID decides, the path relative to the .masset is a hint for people.
MSTRUCT()
struct MARLINTEXT_API BinaryFileLink
{
    MGENERATED_BODY()

    MPROPERTY()
    String Path;
    MPROPERTY()
    Guid Uuid;

    [[nodiscard]] bool IsValid() const
    {
        return Uuid.IsValid();
    }
};
} // namespace Marlin
