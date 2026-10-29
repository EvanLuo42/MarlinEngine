#include "Diagnostics/Log.h"

#include "Concurrency/ScopeLock.h"
#include "Concurrency/SpinLock.h"
#include "HAL/PlatformCrash.h"
#include "HAL/PlatformDebug.h"
#include "HAL/PlatformSync.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"

MARLIN_DEFINE_LOG_CATEGORY(LogCore);

namespace Marlin
{
namespace
{
constexpr usize BufferSize = usize{1} << 20;
constexpr usize RecordAlignment = 8;
constexpr uint32 PaddingFlag = 0x80000000u;
constexpr usize MaxMessageLength = BufferSize / 8;
constexpr uint32 MaxSinks = 16;
constexpr uint32 MaxCategories = 512;
constexpr usize DebugBatchSize = usize{16} * 1024;

/// The start of each record in the ring; Size is written last, so zero means not yet committed.
struct RecordHeader
{
    uint32 Size;
    uint32 ThreadId;
    uint64 Ticks;
    uint64 Frame;
    const LogCategory *Category;
    const char *File;
    int32 Line;
    uint32 MessageLength;
    LogLevel Level;
};

alignas(64) uint8 _buffer[BufferSize];
/// Writers contend on the write cursor while the flush moves the read cursor, so each has its own cache line.
alignas(64) Atomic<uint64> _writeCursor{0};
alignas(64) Atomic<uint64> _readCursor{0};
/// Blocks rather than spins: a flush formats and runs sinks, and threads that log errors or fill the ring wait on it.
Mutex _flushLock;
/// Debugger output gathered during a flush, under _flushLock, so it takes one system call rather than one a record.
char _debugBatch[DebugBatchSize];
usize _debugBatchLength = 0;
SpinLock _sinkLock;
ILogSink *_sinks[MaxSinks] = {};
SpinLock _categoryLock;
LogCategory *_categories[MaxCategories] = {};
uint32 _categoryCount = 0;
Atomic<uint64> _frame{0};
Atomic<uint8> _flushLevel{static_cast<uint8>(LogLevel::Error)};
Atomic<bool> _debugOutput{!MARLIN_BUILD_SHIPPING};
/// Set while this thread delivers records, so a sink that logs does not flush into itself.
thread_local bool _flushing = false;

/// The size word of the record at `offset`, which writers publish and the reader polls.
AtomicRef<uint32> SizeWord(usize offset)
{
    return AtomicRef<uint32>(*reinterpret_cast<uint32 *>(_buffer + offset));
}

/// Returns the position of `size` free bytes, padding to the end of the ring first if they would wrap; false
/// when the ring is full.
bool Reserve(usize size, uint64 &position)
{
    uint64 cursor = _writeCursor.Load(MemoryOrder::Relaxed);
    while (true)
    {
        const usize offset = cursor & (BufferSize - 1);
        const usize toEnd = BufferSize - offset;
        const usize needed = size <= toEnd ? size : toEnd + size;
        if (cursor + needed - _readCursor.Load(MemoryOrder::Acquire) > BufferSize)
        {
            return false;
        }
        if (_writeCursor.CompareExchangeWeak(cursor, cursor + needed, MemoryOrder::AcquireRelease))
        {
            if (needed != size)
            {
                SizeWord(offset).Store(static_cast<uint32>(toEnd) | PaddingFlag, MemoryOrder::Release);
                cursor += toEnd;
            }
            position = cursor;
            return true;
        }
    }
}

void OutputDebugBatch()
{
    if (_debugBatchLength != 0)
    {
        _debugBatch[_debugBatchLength] = '\0';
        PlatformDebug::Output(_debugBatch);
        _debugBatchLength = 0;
    }
}

void Deliver(const LogRecord &record)
{
    if (_debugOutput.Load(MemoryOrder::Relaxed))
    {
        StringBuilder<1024> line;
        Log::FormatRecord(line, record);
        line.Append('\n');
        if (_debugBatchLength + line.Length() >= DebugBatchSize)
        {
            OutputDebugBatch();
        }
        if (line.Length() >= DebugBatchSize)
        {
            PlatformDebug::Output(line.CStr());
        }
        else
        {
            MemCopy(_debugBatch + _debugBatchLength, line.Data(), line.Length());
            _debugBatchLength += line.Length();
        }
    }
    const ScopeLock lock(_sinkLock);
    for (ILogSink *sink : _sinks)
    {
        if (sink != nullptr)
        {
            sink->Write(record);
        }
    }
}
} // namespace

namespace Log
{
void AddSink(ILogSink *sink)
{
    const ScopeLock lock(_sinkLock);
    for (ILogSink *&slot : _sinks)
    {
        if (slot == nullptr)
        {
            slot = sink;
            return;
        }
    }
    MARLIN_FATAL("Too many log sinks");
}

void RemoveSink(ILogSink *sink) // NOLINT(misc-const-correctness)
{
    const ScopeLock lock(_sinkLock);
    for (ILogSink *&slot : _sinks)
    {
        slot = slot == sink ? nullptr : slot;
    }
}

void EnableDebugOutput(bool enabled)
{
    _debugOutput.Store(enabled);
}

void Flush()
{
    if (_flushing)
    {
        return;
    }
    const ScopeLock lock(_flushLock);
    _flushing = true;
    uint64 read = _readCursor.Load(MemoryOrder::Relaxed);
    const uint64 write = _writeCursor.Load(MemoryOrder::Acquire);
    while (read < write)
    {
        const usize offset = read & (BufferSize - 1);
        const uint32 size = SizeWord(offset).Load(MemoryOrder::Acquire);
        if (size == 0)
        {
            break;
        }
        const usize length = size & ~PaddingFlag;
        if ((size & PaddingFlag) == 0)
        {
            RecordHeader header{};
            MemCopy(&header, _buffer + offset, sizeof(header));
            const LogRecord record{
                header.Ticks,
                header.Frame,
                header.Category,
                header.File,
                {reinterpret_cast<const char *>(_buffer + offset + sizeof(RecordHeader)), header.MessageLength},
                header.ThreadId,
                header.Line,
                header.Level,
            };
            Deliver(record);
        }
        MemZero(_buffer + offset, length);
        read += length;
        _readCursor.Store(read, MemoryOrder::Release);
    }
    OutputDebugBatch();
    {
        const ScopeLock sinkLock(_sinkLock);
        for (ILogSink *sink : _sinks)
        {
            if (sink != nullptr)
            {
                sink->Flush();
            }
        }
    }
    _flushing = false;
}

void SetFlushLevel(LogLevel level)
{
    _flushLevel.Store(static_cast<uint8>(level));
}

void SetFrame(uint64 frame)
{
    _frame.Store(frame, MemoryOrder::Relaxed);
}

StringView GetLevelName(LogLevel level)
{
    constexpr StringView Names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Fatal", "Off"};
    return Names[static_cast<uint32>(level)];
}

bool ParseLevel(StringView name, LogLevel &level)
{
    for (uint8 value = 0; value <= static_cast<uint8>(LogLevel::Off); ++value)
    {
        if (name.EqualsIgnoreCase(GetLevelName(static_cast<LogLevel>(value))))
        {
            level = static_cast<LogLevel>(value);
            return true;
        }
    }
    return false;
}

void RegisterCategories(LogCategory *const *first, LogCategory *const *last)
{
    const ScopeLock lock(_categoryLock);
    for (LogCategory *const *entry = first; entry < last; ++entry)
    {
        LogCategory *category = *entry;
        bool known = category == nullptr;
        for (uint32 i = 0; i < _categoryCount && !known; ++i)
        {
            known = _categories[i] == category;
        }
        if (known)
        {
            continue;
        }
        if (_categoryCount == MaxCategories)
        {
            MARLIN_FATAL("Too many log categories");
        }
        _categories[_categoryCount++] = category;
    }
}

LogCategory *FindCategory(StringView name)
{
    const ScopeLock lock(_categoryLock);
    for (uint32 i = 0; i < _categoryCount; ++i)
    {
        if (name.EqualsIgnoreCase(_categories[i]->GetName()))
        {
            return _categories[i];
        }
    }
    return nullptr;
}

void ForEachCategory(FunctionRef<void(LogCategory &)> visitor)
{
    LogCategory *copy[MaxCategories];
    uint32 count = 0;
    {
        const ScopeLock lock(_categoryLock);
        count = _categoryCount;
        MemCopy(copy, _categories, count * sizeof(LogCategory *));
    }
    for (uint32 i = 0; i < count; ++i)
    {
        visitor(*copy[i]);
    }
}

bool SetLevels(StringView spec, StringWriter &errors)
{
    bool applied = true;
    for (const StringView part : spec.Split(','))
    {
        const StringView entry = part.Trim();
        if (entry.IsEmpty())
        {
            continue;
        }
        StringView name;
        StringView levelName;
        LogLevel level = LogLevel::Info;
        if (!entry.SplitFirst(':', name, levelName) || !ParseLevel(levelName.Trim(), level))
        {
            errors.Append(entry).Append(": expected Category:Level, with a level of Trace to Off\n");
            applied = false;
            continue;
        }
        LogCategory *category = FindCategory(name.Trim());
        if (category == nullptr)
        {
            errors.Append(entry).Append(": no log category is named '").Append(name.Trim()).Append("'\n");
            applied = false;
            continue;
        }
        category->SetLevel(level);
    }
    return applied;
}

void FormatRecord(StringWriter &output, const LogRecord &record)
{
    static const int64 anchorDate = PlatformTime::UtcNow() + PlatformTime::LocalUtcOffset();
    static const uint64 anchorTicks = PlatformTime::Ticks();
    const float64 elapsed = (static_cast<float64>(record.Ticks) - static_cast<float64>(anchorTicks)) /
                            static_cast<float64>(PlatformTime::TicksPerSecond());
    const int64 date = anchorDate + static_cast<int64>(elapsed * 1.0e7);
    constexpr int64 MillisecondsPerDay = int64{24} * 60 * 60 * 1000;
    const int64 milliseconds = ((date / 10000) % MillisecondsPerDay + MillisecondsPerDay) % MillisecondsPerDay;
    FormatTo(output, "[{0:D2}:{1:D2}:{2:D2}.{3:D3}][frame {4}][thread {5}] {6}: {7}: {8}", milliseconds / 3600000,
             milliseconds / 60000 % 60, milliseconds / 1000 % 60, milliseconds % 1000, record.Frame, record.ThreadId,
             record.Category->GetName(), GetLevelName(record.Level), record.Message);
}
} // namespace Log

namespace Private
{
void WriteLog(const LogCategory &category, LogLevel level, const char *file, int32 line, StringView message)
{
    message = message.Left(MaxMessageLength);
    if (level == LogLevel::Fatal && PlatformCrash::GetNotedFailure() == nullptr)
    {
        PlatformCrash::NoteFailure("Fatal error", message, file, line);
    }
    const usize size = AlignUp(sizeof(RecordHeader) + message.Length(), RecordAlignment);
    uint64 position = 0;
    while (!Reserve(size, position))
    {
        if (_flushing)
        {
            if (level == LogLevel::Fatal)
            {
                MARLIN_TRAP();
            }
            return;
        }
        Log::Flush();
        PlatformThread::YieldThread();
    }
    const usize offset = position & (BufferSize - 1);
    RecordHeader header{};
    header.ThreadId = PlatformThread::CurrentId();
    header.Ticks = PlatformTime::Ticks();
    header.Frame = _frame.Load(MemoryOrder::Relaxed);
    header.Category = &category;
    header.File = file;
    header.Line = line;
    header.MessageLength = static_cast<uint32>(message.Length());
    header.Level = level;
    MemCopy(_buffer + offset, &header, sizeof(header));
    MemCopy(_buffer + offset + sizeof(header), message.Data(), message.Length());
    SizeWord(offset).Store(static_cast<uint32>(size), MemoryOrder::Release);

    if (static_cast<uint8>(level) >= _flushLevel.Load(MemoryOrder::Relaxed))
    {
        Log::Flush();
    }
    if (level == LogLevel::Fatal)
    {
        if (PlatformDebug::IsDebuggerAttached())
        {
            MARLIN_DEBUG_BREAK();
        }
        MARLIN_TRAP();
    }
}
} // namespace Private
} // namespace Marlin
