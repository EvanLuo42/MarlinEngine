#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "HAL/PlatformFile.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
/// The processor of the process that was dumped.
enum class MinidumpArch : uint16
{
    X64,
    Arm64,
    Unknown,
};

struct MinidumpModule
{
    uint64 Base = 0;
    uint32 Size = 0;
    /// The image's link time stamp; with Size, the image's key in a symbol store.
    uint32 TimeStamp = 0;
    /// Where the image was loaded from on the machine that crashed.
    String Path;
    /// The file name of the debug file the image names, such as Game.pdb; empty if it names none.
    String DebugFile;
    /// The debug file's identity in the order a symbol store writes it: the PDB GUID, or the ELF build id.
    List<uint8> DebugId;
    uint32 DebugAge = 0;
};

struct MinidumpThread
{
    uint32 Id = 0;
    /// The thread's register context in the layout of the dumped processor, such as a Windows CONTEXT.
    Span<const uint8> Context;
};

/// A minidump file, mapped and read in place: the modules, threads, exception and memory that its streams hold.
/// Spans stay valid until Close.
class CRASHANALYSIS_API Minidump
{
public:
    Minidump();
    ~Minidump();
    MARLIN_NONCOPYABLE(Minidump);

    /// Fails, with the reason in *error*, if the file cannot be read or is not a minidump.
    bool Open(StringView path, String &error);
    void Close();

    [[nodiscard]] MinidumpArch GetArch() const
    {
        return _arch;
    }

    [[nodiscard]] Span<const MinidumpModule> GetModules() const
    {
        return _modules.AsSpan();
    }

    [[nodiscard]] Span<const MinidumpThread> GetThreads() const
    {
        return _threads.AsSpan();
    }

    /// The module that holds *address*, or null.
    [[nodiscard]] const MinidumpModule *FindModule(uint64 address) const;

    [[nodiscard]] bool HasException() const
    {
        return _hasException;
    }

    [[nodiscard]] uint32 GetExceptionThread() const
    {
        return _exceptionThread;
    }

    [[nodiscard]] uint32 GetExceptionCode() const
    {
        return _exceptionCode;
    }

    [[nodiscard]] uint64 GetExceptionAddress() const
    {
        return _exceptionAddress;
    }

    /// The exception thread's context at the exception, rather than inside the handler.
    [[nodiscard]] Span<const uint8> GetExceptionContext() const
    {
        return _exceptionContext;
    }

    /// Copies the dumped memory at *address*; returns how many bytes the dump has there, from the start.
    usize ReadMemory(uint64 address, void *buffer, usize size) const;

private:
    struct Range
    {
        uint64 Start = 0;
        uint64 Size = 0;
        uint64 Offset = 0;
    };

    [[nodiscard]] Span<const uint8> Bytes(uint64 offset, uint64 size) const;
    [[nodiscard]] String ReadString(uint32 offset) const;
    void ReadModules(Span<const uint8> stream);
    void ReadThreads(Span<const uint8> stream);
    void ReadMemoryList(Span<const uint8> stream);
    void ReadMemory64List(Span<const uint8> stream);
    void ReadException(Span<const uint8> stream);
    void ReadSystemInfo(Span<const uint8> stream);

    FileHandle _file;
    MappedRegion _region;
    const uint8 *_data = nullptr;
    usize _size = 0;
    MinidumpArch _arch = MinidumpArch::Unknown;
    List<MinidumpModule> _modules;
    List<MinidumpThread> _threads;
    List<Range> _ranges;
    bool _hasException = false;
    uint32 _exceptionThread = 0;
    uint32 _exceptionCode = 0;
    uint64 _exceptionAddress = 0;
    Span<const uint8> _exceptionContext;
};
} // namespace Marlin
