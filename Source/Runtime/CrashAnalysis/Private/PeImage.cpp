#include "PeImage.h"

#include "HAL/PlatformFile.h"
#include "Memory/MemoryOps.h"
#include "String/String.h"

namespace Marlin
{
namespace
{
constexpr uint16 DosSignature = 0x5A4D;     // "MZ"
constexpr uint32 NtSignature = 0x00004550u; // "PE\0\0"
constexpr uint16 Pe32PlusMagic = 0x20B;
constexpr uint32 ExceptionDirectory = 3;
constexpr usize SectionHeaderSize = 40;

/// The value at *offset*, or zero past the end.
template <typename T> T ReadValue(const List<uint8> &bytes, usize offset)
{
    T value{};
    if (offset <= bytes.Count() && bytes.Count() - offset >= sizeof(T))
    {
        MemCopy(&value, bytes.Data() + offset, sizeof(T));
    }
    return value;
}

/// Reads *size* bytes at *offset*; false unless all of them are read.
bool ReadExactly(FileHandle handle, uint64 offset, void *buffer, usize size)
{
    auto *bytes = static_cast<uint8 *>(buffer);
    while (size > 0)
    {
        usize read = 0;
        if (!PlatformFile::ReadAt(handle, offset, bytes, size, read) || read == 0)
        {
            return false;
        }
        offset += read;
        bytes += read;
        size -= read;
    }
    return true;
}

/// Grows *head*, the start of a *fileSize*-byte file, to its first *size* bytes or the whole file if shorter.
bool ReadHead(FileHandle handle, uint64 fileSize, usize size, List<uint8> &head)
{
    const usize wanted = fileSize < size ? static_cast<usize>(fileSize) : size;
    if (wanted <= head.Count())
    {
        return true;
    }
    const usize have = head.Count();
    head.ResizeUninitialized(wanted);
    return ReadExactly(handle, have, head.Data() + have, wanted - have);
}
} // namespace

bool PeImage::Load(StringView path, uint32 timeStamp, uint32 imageSize)
{
    _bytes.Clear();
    _exceptionRva = 0;
    _exceptionSize = 0;
    const String file(path);
    const FileHandle handle = PlatformFile::Open(file.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!handle.IsValid())
    {
        return false;
    }
    const bool loaded = Load(handle, timeStamp, imageSize);
    PlatformFile::Close(handle);
    if (!loaded)
    {
        _bytes.Clear();
        _exceptionRva = 0;
        _exceptionSize = 0;
    }
    return loaded;
}

bool PeImage::Load(FileHandle handle, uint32 timeStamp, uint32 imageSize)
{
    uint64 size = 0;
    if (!PlatformFile::GetSize(handle, size) || size >= (uint64{1} << 31))
    {
        return false;
    }
    // The headers: enough for the NT headers, then for the section table they describe. Values past the end of the
    // file read as zero.
    List<uint8> head;
    if (!ReadHead(handle, size, 0x1000, head) || ReadValue<uint16>(head, 0) != DosSignature)
    {
        return false;
    }
    const auto nt = ReadValue<uint32>(head, 0x3C);
    const usize optional = usize{nt} + 24;
    if (!ReadHead(handle, size, optional + 144, head))
    {
        return false;
    }
    if (ReadValue<uint32>(head, nt) != NtSignature || ReadValue<uint32>(head, nt + 8) != timeStamp ||
        ReadValue<uint16>(head, optional) != Pe32PlusMagic || ReadValue<uint32>(head, optional + 56) != imageSize)
    {
        return false;
    }
    const auto sectionCount = ReadValue<uint16>(head, nt + 6);
    const auto optionalSize = ReadValue<uint16>(head, nt + 20);
    const usize sections = optional + optionalSize;
    if (!ReadHead(handle, size, sections + usize{sectionCount} * SectionHeaderSize, head))
    {
        return false;
    }
    const auto headersSize = ReadValue<uint32>(head, optional + 60);
    const auto directoryCount = ReadValue<uint32>(head, optional + 108);
    if (directoryCount > ExceptionDirectory)
    {
        _exceptionRva = ReadValue<uint32>(head, optional + 112 + ExceptionDirectory * 8);
        _exceptionSize = ReadValue<uint32>(head, optional + 116 + ExceptionDirectory * 8);
    }

    // Each part goes from the file straight to where it is in memory.
    _bytes.Resize(imageSize);
    if (headersSize < size && headersSize < imageSize && !ReadExactly(handle, 0, _bytes.Data(), headersSize))
    {
        return false;
    }
    for (uint32 i = 0; i < sectionCount; ++i)
    {
        const usize header = sections + i * SectionHeaderSize;
        const auto virtualSize = ReadValue<uint32>(head, header + 8);
        const auto virtualAddress = ReadValue<uint32>(head, header + 12);
        const auto rawSize = ReadValue<uint32>(head, header + 16);
        const auto rawOffset = ReadValue<uint32>(head, header + 20);
        uint64 count = rawSize < virtualSize || virtualSize == 0 ? rawSize : virtualSize;
        if (rawOffset > size || virtualAddress > imageSize)
        {
            continue;
        }
        count = count < size - rawOffset ? count : size - rawOffset;
        count = count < imageSize - virtualAddress ? count : imageSize - virtualAddress;
        if (!ReadExactly(handle, rawOffset, _bytes.Data() + virtualAddress, static_cast<usize>(count)))
        {
            return false;
        }
    }
    return true;
}

usize PeImage::Read(uint64 rva, void *buffer, usize size) const
{
    if (rva >= _bytes.Count())
    {
        return 0;
    }
    const uint64 available = _bytes.Count() - rva;
    const usize count = available < size ? static_cast<usize>(available) : size;
    MemCopy(buffer, _bytes.Data() + rva, count);
    return count;
}

bool PeImage::FindFunction(uint32 rva, PeRuntimeFunction &function) const
{
    constexpr usize EntrySize = 12;
    usize first = 0;
    usize count = _exceptionSize / EntrySize;
    while (count > 0)
    {
        const usize half = count / 2;
        const usize entry = _exceptionRva + (first + half) * EntrySize;
        const auto begin = ReadValue<uint32>(_bytes, entry);
        const auto end = ReadValue<uint32>(_bytes, entry + 4);
        if (rva < begin)
        {
            count = half;
        }
        else if (rva >= end)
        {
            first += half + 1;
            count -= half + 1;
        }
        else
        {
            function.Begin = begin;
            function.End = end;
            function.UnwindInfo = ReadValue<uint32>(_bytes, entry + 8);
            return true;
        }
    }
    return false;
}
} // namespace Marlin
