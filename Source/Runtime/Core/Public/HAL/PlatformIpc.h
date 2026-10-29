//! Pipes and named shared memory for communication between processes.
//!
//! Named pipes and shared memory are local to the machine and the current user session. The blocking calls wait for the
//! other process; the non-blocking ones are marked as such.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// A native pipe handle and the event used for its overlapped I/O.
struct PipeHandle
{
    /// The native pipe handle; zero when invalid.
    uintptr Value = 0;
    /// The event used for overlapped named-pipe I/O; zero for anonymous pipes.
    uintptr Event = 0;

    /// Returns true when the handle refers to a pipe.
    bool IsValid() const
    {
        return Value != 0;
    }
};

/// Anonymous pipes come from `PlatformProcess::Create`; named pipes are local to the machine.
namespace PlatformPipe
{
/// Reads up to `size` bytes, blocking until data arrives, and writes the count to `bytesRead`.
///
/// # Errors
/// Returns false once the other end has closed or the read fails; `bytesRead` is then zero.
CORE_API bool Read(PipeHandle pipe, void *buffer, usize size, usize &bytesRead);
/// Writes `size` bytes, continuing until all are written.
///
/// # Errors
/// Returns false when the write fails; bytes may already have been written.
CORE_API bool Write(PipeHandle pipe, const void *data, usize size);
/// Writes the bytes available to read without blocking to `bytesAvailable`.
///
/// # Errors
/// Returns false once the other end has closed or the call fails; `bytesAvailable` is then zero.
CORE_API bool Peek(PipeHandle pipe, usize &bytesAvailable);
/// Closes the pipe and its event; does nothing for an invalid handle.
CORE_API void Close(PipeHandle pipe);

/// Creates one named-pipe server instance; another instance accepts the next client.
///
/// # Errors
/// Returns an invalid handle when the name is invalid or the instance cannot be created.
CORE_API PipeHandle CreateServer(const char *name);
/// Waits up to `timeout` milliseconds for a client to connect to this server instance.
///
/// # Errors
/// Returns false on timeout or when the wait fails.
CORE_API bool WaitForClient(PipeHandle server, uint32 timeout);
/// Drops the current client so the instance can wait for another.
CORE_API void Disconnect(PipeHandle server);
/// Opens a client end to the named pipe, waiting up to `timeout` milliseconds for a free instance.
///
/// # Errors
/// Returns an invalid handle on timeout or when no instance can be opened.
CORE_API PipeHandle Connect(const char *name, uint32 timeout);
} // namespace PlatformPipe

/// A mapped shared-memory region.
///
/// # Thread safety
/// The mapping is not synchronized; processes that share it must coordinate access themselves.
struct SharedMemory
{
    /// The first byte of the mapping.
    void *Data = nullptr;
    /// The mapped size in bytes.
    usize Size = 0;
    /// The platform mapping handle; `Close` closes it.
    uintptr Handle = 0;
};

/// Named memory shared between processes of the same user session.
namespace PlatformSharedMemory
{
/// Creates the region, or opens it if it exists; a new region starts zeroed.
///
/// The size is fixed by the first process that creates the region.
///
/// # Errors
/// Returns false when the name is invalid or the region cannot be created or mapped.
CORE_API bool Create(const char *name, usize size, SharedMemory &memory);
/// Maps an existing region into this process.
///
/// # Errors
/// Returns false when no process has created the region or it cannot be mapped at `size` bytes.
CORE_API bool Open(const char *name, usize size, SharedMemory &memory);
/// Unmaps the region, closes its handle and clears `memory`.
CORE_API void Close(SharedMemory &memory);
} // namespace PlatformSharedMemory
} // namespace Marlin
