//! Starting, inspecting and ending processes, plus the calling process's own path, command line and output.
//!
//! `Create` starts a child with optional pipes to its standard streams; the returned handle waits for the child or
//! reads its exit code. The remaining functions report on or end the calling process.

#pragma once

#include "CoreTypes.h"
#include "EnumFlags.h"
#include "HAL/PlatformIpc.h"

namespace Marlin
{
/// Options for the child process that `Create` starts.
///
/// Values combine with `|`.
enum class ProcessFlags : uint8
{
    /// No options.
    None = 0,
    /// Gives the caller a pipe to the child's standard input.
    RedirectInput = 1 << 0,
    /// Gives the caller a pipe from the child's standard output and standard error, which share it.
    RedirectOutput = 1 << 1,
    /// Gives the child no console window.
    Hidden = 1 << 2,
    /// The OS terminates the child when this process exits.
    KillOnParentExit = 1 << 3,
    /// The child gets no console and outlives this process's console.
    Detached = 1 << 4,
};
MARLIN_ENUM_FLAGS(ProcessFlags)

/// Describes a child process for `PlatformProcess::Create`.
///
/// The strings are read during the call only; the child keeps no pointer to them.
struct ProcessDesc
{
    /// UTF-8 path or name of the executable to run.
    const char *Executable = nullptr;
    /// Command-line arguments passed as they are; quoting is the caller's.
    const char *Arguments = nullptr;
    /// UTF-8 working directory, or null to inherit this process's.
    const char *WorkingDirectory = nullptr;
    /// Options that control pipes, the console and the child's lifetime.
    ProcessFlags Flags = ProcessFlags::None;
};

/// A handle to a running process.
struct ProcessHandle
{
    /// The OS process handle, or zero for an invalid handle.
    uintptr Value = 0;
    /// The process identifier, or zero for an invalid handle.
    uint32 Id = 0;

    /// Returns whether the handle refers to a process.
    bool IsValid() const
    {
        return Value != 0;
    }
};

/// Starts child processes and reports on the calling process.
///
/// Functions that fill a buffer return the UTF-8 length without the terminator, and write the terminated string only
/// if that length is less than `capacity`.
namespace PlatformProcess
{
/// Writes the UTF-8 path of this process's executable to `buffer`, using `/` separators.
///
/// Returns the length the path needs; nothing is written when that length is not less than `capacity`. Zero means
/// the path could not be read.
CORE_API usize ExecutablePath(char *buffer, usize capacity);
/// Writes this process's full command line, executable included, to `buffer`.
///
/// The text keeps the form the OS supplied, so quoting is preserved. Returns the length it needs; nothing is
/// written when that length is not less than `capacity`.
CORE_API usize CommandLine(char *buffer, usize capacity);

/// Returns the process identifier of this process.
CORE_API uint32 CurrentId();
/// Writes `length` bytes of UTF-8 text to this process's standard output without buffering.
///
/// Writes nothing when the process has no standard output or a write fails.
CORE_API void WriteOutput(const char *text, usize length);
/// Ends this process with `exitCode` and never returns.
///
/// Other threads stop where they are; no destructors run for their stacks.
[[noreturn]] CORE_API void Exit(int32 exitCode);

/// Starts the process described by `desc`.
///
/// On success `process` refers to the running child. With `ProcessFlags::RedirectInput`, `input` receives a pipe to
/// the child's standard input; with `ProcessFlags::RedirectOutput`, `output` receives a pipe from its standard
/// output and standard error. The caller owns those pipes and closes them with `PlatformPipe::Close`; a pipe that
/// no parameter can receive is closed by the call.
///
/// `ProcessFlags::KillOnParentExit` is best effort: a child that the OS cannot put in the job still starts.
///
/// # Errors
/// Returns false and leaves `process` invalid when the executable cannot be started, or when `desc` names a path
/// or directory that is not valid UTF-8; any pipe it created is closed.
///
/// # Thread safety
/// May be called from any thread.
///
/// # Examples
///
/// ```cpp
/// ProcessDesc desc;
/// desc.Executable = "C:/Tools/MarlinConverter.exe";
/// desc.Arguments = "-Project=Sandbox";
/// desc.Flags = ProcessFlags::Hidden | ProcessFlags::KillOnParentExit;
/// ProcessHandle process;
/// if (PlatformProcess::Create(desc, process))
/// {
///     int32 exitCode = 0;
///     const bool exited = PlatformProcess::Wait(process, 30000); // -> true
///     const bool read = PlatformProcess::GetExitCode(process, exitCode); // -> true
///     PlatformProcess::Close(process);
/// }
/// ```
CORE_API bool Create(const ProcessDesc &desc, ProcessHandle &process, PipeHandle *input = nullptr,
                     PipeHandle *output = nullptr);
/// Opens a handle to the running process `processId`, for `Wait`, `GetExitCode` or `Terminate`.
///
/// # Errors
/// Returns false when no process with that id exists or this process may not open it.
CORE_API bool Open(uint32 processId, ProcessHandle &process);
/// Waits up to `timeout` milliseconds for `process` to exit.
///
/// Returns false on timeout. A timeout of `0xFFFFFFFF` waits without a limit.
CORE_API bool Wait(ProcessHandle process, uint32 timeout);
/// Returns whether `process` is still running.
CORE_API bool IsRunning(ProcessHandle process);
/// Returns whether a process with `processId` exists.
///
/// A process that exists but cannot be opened counts as running, so a protected process is reported as running.
CORE_API bool IsRunning(uint32 processId);
/// Reads the exit code of `process` unless it is still running.
///
/// # Errors
/// Returns false while the process runs; `exitCode` is left unchanged.
CORE_API bool GetExitCode(ProcessHandle process, int32 &exitCode);
/// Terminates `process` with `exitCode`.
///
/// The call does not wait for the process to exit; `Wait` does that.
CORE_API void Terminate(ProcessHandle process, int32 exitCode);
/// Releases `process`'s handle; the process keeps running.
///
/// A handle from `Create` or `Open` must be closed exactly once.
CORE_API void Close(ProcessHandle process);
} // namespace PlatformProcess
} // namespace Marlin
