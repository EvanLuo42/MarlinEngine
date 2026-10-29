//! Log categories, records, sinks, and the formatted message macros.
//!
//! Messages go into a lock-free ring buffer and reach the sinks when `Log::Flush` drains it.

#pragma once

#include "Concurrency/Atomic.h"
#include "CoreTypes.h"
#include "String/Format.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
/// The severity of a log message, also used as a category's threshold.
enum class LogLevel : uint8
{
    /// Very detailed diagnostics.
    Trace,
    /// Diagnostics useful while developing.
    Debug,
    /// Normal operation messages.
    Info,
    /// A problem that the current operation recovered from.
    Warning,
    /// An operation failed.
    Error,
    /// Logged, flushed, then the process crashes.
    Fatal,
    /// Disables the category's non-fatal messages.
    Off,
};

/// A named log category with a runtime threshold.
///
/// Declare one with `MARLIN_DECLARE_LOG_CATEGORY` and define it with `MARLIN_DEFINE_LOG_CATEGORY`. The threshold can
/// be changed at run time; messages below it are not formatted.
class LogCategory
{
public:
    /// Creates a category with `name` and a threshold of `level`.
    ///
    /// `name` must outlive the category.
    constexpr LogCategory(const char *name, LogLevel level) : _name(name), _level(static_cast<uint8>(level))
    {
    }

    ~LogCategory() = default;
    MARLIN_NONCOPYABLE(LogCategory);

    /// Returns the category's name.
    [[nodiscard]] constexpr const char *GetName() const
    {
        return _name;
    }

    /// Returns the current threshold.
    [[nodiscard]] LogLevel GetLevel() const
    {
        return static_cast<LogLevel>(_level.Load(MemoryOrder::Relaxed));
    }

    /// Sets the threshold.
    void SetLevel(LogLevel level)
    {
        _level.Store(static_cast<uint8>(level), MemoryOrder::Relaxed);
    }

    /// Returns whether `level` is at or above the threshold.
    [[nodiscard]] bool IsEnabled(LogLevel level) const
    {
        return static_cast<uint8>(level) >= _level.Load(MemoryOrder::Relaxed);
    }

private:
    const char *_name;
    Atomic<uint8> _level;
};

/// One log message as sinks receive it.
///
/// The pointers stay valid only during the `Write` call.
struct LogRecord
{
    /// The PlatformTime ticks when the message was written.
    uint64 Ticks = 0;
    /// The frame number set by `Log::SetFrame`.
    uint64 Frame = 0;
    /// The category that produced the message.
    const LogCategory *Category = nullptr;
    /// The source file.
    const char *File = nullptr;
    /// The formatted message.
    StringView Message;
    /// The id of the writing thread.
    uint32 ThreadId = 0;
    /// The source line.
    int32 Line = 0;
    /// The severity.
    LogLevel Level = LogLevel::Info;
};

/// Receives records as `Log::Flush` drains the buffer.
///
/// `Write` and `Flush` are called on the flushing thread, one at a time, so an implementation does not need its own
/// lock for its own state.
class ILogSink
{
public:
    ILogSink() = default;
    virtual ~ILogSink() = default;
    ILogSink(const ILogSink &) = delete;
    ILogSink &operator=(const ILogSink &) = delete;
    ILogSink(ILogSink &&) = delete;
    ILogSink &operator=(ILogSink &&) = delete;

    /// Receives one record.
    virtual void Write(const LogRecord &record) = 0;
    /// Called after each `Log::Flush`, for sinks that buffer.
    virtual void Flush()
    {
    }
};

/// The process log: sinks, the ring buffer, and flushing.
///
/// Messages go into a lock-free ring buffer and reach the sinks when `Flush` drains it: from the subsystem that owns
/// logging, each frame or from a thread, and at once for errors. A full buffer is drained by the writer.
namespace Log
{
/// Registers `sink`; it stays registered until removed.
///
/// The debugger output sink is built in. `sink` must outlive its registration.
///
/// # Errors
/// At most 16 sinks can be registered; registering a 17th is fatal.
CORE_API void AddSink(ILogSink *sink);
/// Removes `sink`.
CORE_API void RemoveSink(ILogSink *sink);
/// Enables or disables the built-in debugger output sink.
///
/// It is on by default outside Shipping.
CORE_API void EnableDebugOutput(bool enabled);

/// Delivers everything buffered so far to the sinks.
CORE_API void Flush();
/// Flushes right after every message at or above `level`; `Error` by default.
CORE_API void SetFlushLevel(LogLevel level);

/// Sets the frame number stamped on subsequent records.
CORE_API void SetFrame(uint64 frame);

/// Formats a record as `[hh:mm:ss.mmm][frame N][thread N] Category: Level: message`, in local time.
CORE_API void FormatRecord(StringWriter &output, const LogRecord &record);

/// Returns the name of `level`.
CORE_API StringView GetLevelName(LogLevel level);
/// Parses a level name such as `Debug`, ignoring case; false if `name` is none.
CORE_API bool ParseLevel(StringView name, LogLevel &level);

/// Registers the categories whose pointers lie in `[first, last)`: a module's section of
/// `MARLIN_DEFINE_LOG_CATEGORY` entries, which the generated `Register_<Module>` passes in.
///
/// Null entries (section padding and markers) and categories already registered are skipped, so a monolithic
/// executable, whose modules share one section, registers each category once.
///
/// # Errors
/// At most 512 categories can be registered; registering more is fatal.
CORE_API void RegisterCategories(LogCategory *const *first, LogCategory *const *last);
/// Returns the registered category named `name`, ignoring case, or null.
[[nodiscard]] CORE_API LogCategory *FindCategory(StringView name);
/// Calls `visitor` for each registered category, in registration order; it runs without the registry lock.
CORE_API void ForEachCategory(FunctionRef<void(LogCategory &)> visitor);
/// Applies `Category:Level,Category:Level` to the registered categories, such as `LogCore:Debug,LogAssetIndex:Trace`.
///
/// Every entry that names a category and a level is applied; the others are described in `errors`, one per line.
/// Returns whether all entries were applied.
CORE_API bool SetLevels(StringView spec, StringWriter &errors);
} // namespace Log

namespace Private
{
/// Formats and buffers one message.
CORE_API void WriteLog(const LogCategory &category, LogLevel level, const char *file, int32 line, StringView message);

template <typename... TArgs>
MARLIN_NOINLINE void Log(const LogCategory &category, LogLevel level, const char *file, int32 line,
                         FormatString<::Marlin::TypeIdentity<TArgs>...> format, const TArgs &...args)
{
    StringBuilder<512> message;
    FormatTo(message, format, args...);
    WriteLog(category, level, file, line, message.AsView());
}
} // namespace Private
} // namespace Marlin

/// Declares a log category in a module's header.
///
/// The category logs at `DefaultLevel` and above by default, and compiles out anything below `ShippingLevel` in
/// Shipping. One source file of the module defines it with `MARLIN_DEFINE_LOG_CATEGORY`, so every module shares one
/// threshold.
///
/// # Examples
///
/// ```cpp
/// MARLIN_DECLARE_LOG_CATEGORY(PHYSICS_API, LogPhysics, Info, Warning);
/// ```
#define MARLIN_DECLARE_LOG_CATEGORY(Api, Name, DefaultLevel, ShippingLevel)                                            \
    extern Api::Marlin::LogCategory Name;                                                                              \
    inline constexpr ::Marlin::LogLevel MARLIN_CONCAT(Name, _DefaultLevel) = ::Marlin::LogLevel::DefaultLevel;         \
    inline constexpr ::Marlin::LogLevel MARLIN_CONCAT(Name, _CompiledLevel) =                                          \
        MARLIN_BUILD_SHIPPING ? ::Marlin::LogLevel::ShippingLevel : ::Marlin::LogLevel::Trace

/// Places a variable in part `Part` ("a", "m" or "z") of the section that collects a module's log categories.
///
/// The parts of one section are laid out in name order, so the entries (`m`) lie between the markers (`a` and `z`) of
/// the module. Nothing runs at startup: `Register_<Module>` reads the range. Only the Microsoft object format orders
/// parts this way; other platforms need their own spelling, with the begin and end of the section.
#if MARLIN_PLATFORM_WINDOWS
#define MARLIN_LOG_SECTION(Part) __attribute__((section(".mlogc$" Part), used))
#else
#error "Log categories need a section spelling for this platform"
#endif

/// Defines a category declared with `MARLIN_DECLARE_LOG_CATEGORY`.
///
/// Use it in exactly one source file of the module. It also records the category in the module's section, which is how
/// `Log::FindCategory` and the `Log.Level` console command find it.
#define MARLIN_DEFINE_LOG_CATEGORY(Name)                                                                               \
    constinit ::Marlin::LogCategory Name                                                                               \
    {                                                                                                                  \
        #Name, MARLIN_CONCAT(Name, _DefaultLevel)                                                                      \
    };                                                                                                                 \
    MARLIN_LOG_SECTION("m") ::Marlin::LogCategory *const MARLIN_CONCAT(Name, _Entry) = &Name

/// Defines the markers around a module's categories; the generated module file has it once, at global scope.
#define MARLIN_DEFINE_LOG_CATEGORY_MARKERS()                                                                           \
    MARLIN_LOG_SECTION("a") ::Marlin::LogCategory *const MarlinLogCategoriesBegin = nullptr;                           \
    MARLIN_LOG_SECTION("z") ::Marlin::LogCategory *const MarlinLogCategoriesEnd = nullptr

/// Registers the categories between the markers of `MARLIN_DEFINE_LOG_CATEGORY_MARKERS`.
#define MARLIN_REGISTER_LOG_CATEGORIES() ::Marlin::Log::RegisterCategories(&MarlinLogCategoriesBegin, &MarlinLogCategoriesEnd)

/// Logs a formatted message to `Category` at `Level`.
///
/// Arguments are not evaluated when the level is disabled. `Fatal` is never disabled: it logs, flushes and crashes.
///
/// # Message conventions
/// `Info` is for lifecycle and state changes, `Debug` for per-object detail, `Warning` for a recovered degradation
/// and `Error` for a failed operation. Name the object and give the numbers and the fallback that explain the
/// message; keep `Info` off per-frame and per-object paths.
///
/// # Examples
///
/// ```cpp
/// MARLIN_LOG(LogPhysics, Warning, "{0} bodies asleep", count);
/// ```
#define MARLIN_LOG(Category, Level, ...)                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if constexpr (::Marlin::LogLevel::Level >= MARLIN_CONCAT(Category, _CompiledLevel) ||                          \
                      ::Marlin::LogLevel::Level == ::Marlin::LogLevel::Fatal)                                          \
        {                                                                                                              \
            if (::Marlin::LogLevel::Level == ::Marlin::LogLevel::Fatal ||                                              \
                Category.IsEnabled(::Marlin::LogLevel::Level))                                                         \
            {                                                                                                          \
                ::Marlin::Private::Log(Category, ::Marlin::LogLevel::Level, __FILE__, __LINE__, __VA_ARGS__);          \
            }                                                                                                          \
        }                                                                                                              \
    } while (false)

MARLIN_DECLARE_LOG_CATEGORY(CORE_API, LogCore, Info, Warning);
