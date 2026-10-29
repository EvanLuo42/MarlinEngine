#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformFile.h"
#include "String/StringView.h"

namespace Marlin
{
/// A RUNTIME_FUNCTION entry of an x64 image's exception directory; addresses are relative to the image base.
struct PeRuntimeFunction
{
    uint32 Begin = 0;
    uint32 End = 0;
    uint32 UnwindInfo = 0;
};

/// A PE32+ image laid out as it is in memory, for the unwind data and code bytes that minidumps leave out.
class PeImage
{
public:
    /// Fails unless the file at *path* is the PE32+ image with this time stamp and size, so a rebuilt image with
    /// different code is never used. Reads the headers first, so another image is rejected without reading it all.
    bool Load(StringView path, uint32 timeStamp, uint32 imageSize);

    /// The bytes the image takes in memory.
    [[nodiscard]] usize GetSize() const
    {
        return _bytes.Count();
    }

    /// Copies the image bytes at *rva*; returns how many there are.
    usize Read(uint64 rva, void *buffer, usize size) const;
    /// The function entry that holds *rva*; false for a leaf function, which has none.
    bool FindFunction(uint32 rva, PeRuntimeFunction &function) const;

private:
    bool Load(FileHandle handle, uint32 timeStamp, uint32 imageSize);

    List<uint8> _bytes;
    uint32 _exceptionRva = 0;
    uint32 _exceptionSize = 0;
};
} // namespace Marlin
