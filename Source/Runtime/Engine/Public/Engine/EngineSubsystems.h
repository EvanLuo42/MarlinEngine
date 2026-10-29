#pragma once

#include "Concurrency/Jobs.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "Engine/Subsystem.h"
#include "HAL/PlatformFile.h"
#include "String/StringBuilder.h"
#include "Engine/EngineSubsystems.gen.h"

namespace Marlin
{
/// Runs the job system for the life of the engine.
MCLASS(Config = "Engine.Jobs")
class ENGINE_API JobSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// Worker threads; zero for one per physical core, less one.
    MPROPERTY()
    static uint32 WorkerCount;
    /// Physical cores left to the OS.
    MPROPERTY()
    static uint32 ReservedCores;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
};

/// Writes the log to /User/Logs/<Target>.log and flushes it every frame; the log front end is in Core.
MCLASS(Config = "Engine.Log")
class ENGINE_API LogSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// Writes no log file when false; the debugger output still works.
    MPROPERTY()
    static bool WriteFile;

    /// Sets the threshold of log category *category* to *level* (Trace, Debug, Info, Warning, Error, Fatal or Off).
    ///
    /// Shipping builds compile out the messages below a category's Shipping level, so lowering it there shows nothing
    /// more.
    MFUNCTION(ConsoleCommand = "Log.Level")
    static String SetCategoryLevel(const String &category, const String &level);
    /// Lists every log category with its threshold.
    MFUNCTION(ConsoleCommand = "Log.List")
    static String ListCategories();

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void EndFrame(const FrameTime &time) override;

    /// Whether the log file's sink is in place, so a flush before the main loop, such as a splash screen's, loses no
    /// record from the file. Any thread.
    [[nodiscard]] static bool IsStarted();

private:
    /// Buffers records and writes them out as the log flushes: on a job, so the frame does not wait for the disk,
    /// except for a batch with an error, which may be the last before a crash.
    class FileSink final : public ILogSink
    {
    public:
        void Write(const LogRecord &record) override;
        void Flush() override;
        /// Returns once the write in flight, if any, is done.
        void Finish();

        FileHandle File;

    private:
        static void WriteJob(void *argument);

        StringBuilder<16384> _pending;
        /// What the job in flight writes; only touched again once it is done.
        List<char> _writing;
        JobCounter _written;
        bool _urgent = false;
    };

    FileSink _sink;
};
} // namespace Marlin
