//! Asynchronous file reads and writes through an I/O ring, with a completion-port fallback.
//!
//! Windows 11 22H2 and later use IoRing; older systems use overlapped I/O on an I/O completion port. A queue is used by
//! one thread at a time.

#pragma once

#include "CoreTypes.h"
#include "HAL/PlatformFile.h"

namespace Marlin
{
/// One read or write submitted to an `AsyncIoQueue`.
///
/// # Safety
/// `Buffer` must stay valid until the matching completion is returned; the operation reads or writes it asynchronously.
struct AsyncIoRequest
{
    /// The file, opened with `FileFlags::Async`.
    FileHandle File;
    /// The byte offset in the file.
    uint64 Offset = 0;
    /// The buffer to read into or write from.
    void *Buffer = nullptr;
    /// The number of bytes to transfer.
    uint32 Size = 0;
    /// An opaque pointer returned unchanged in the completion.
    void *UserData = nullptr;
};

/// The result of one `AsyncIoQueue` request.
struct AsyncIoCompletion
{
    /// The `UserData` of the request.
    void *UserData = nullptr;
    /// The number of bytes transferred.
    uint32 BytesTransferred = 0;
    /// True when the operation succeeded; a read at the end of the file succeeds with zero bytes.
    bool Succeeded = false;
};

/// Asynchronous file reads and writes, through an I/O ring where the OS has it.
///
/// A queue holds up to a fixed number of requests in flight. A request either starts immediately or waits for
/// `Submit`, depending on the path, and its completion comes back from `Wait`. Where IoRing is unavailable the queue
/// uses overlapped I/O on an I/O completion port; `UsesIoRing` reports which path is active.
///
/// # Preconditions
/// `Initialize` succeeds before `Read`, `Write`, `Submit` or `Wait` is called; those functions dereference the queue
/// state without checking it.
///
/// # Thread safety
/// A queue is used by one thread at a time; it has no internal synchronization. Separate queues may be used on
/// separate threads.
///
/// # Examples
///
/// ```cpp
/// AsyncIoQueue queue;
/// if (queue.Initialize(64))
/// {
///     AsyncIoRequest request;
///     request.File = file; // Opened with FileFlags::Async.
///     request.Offset = 0;
///     request.Buffer = data;
///     request.Size = size;
///     queue.Read(request);
///     queue.Submit();
///
///     AsyncIoCompletion completion;
///     const uint32 count = queue.Wait(&completion, 1, 1000); // -> 1 when the read completes within 1000 ms.
/// }
/// ```
class CORE_API AsyncIoQueue
{
public:
    /// Creates a queue that is not initialized.
    AsyncIoQueue() = default;
    /// Shuts the queue down.
    ~AsyncIoQueue();
    MARLIN_NONCOPYABLE(AsyncIoQueue);

    /// Creates the queue with room for `capacity` requests in flight.
    ///
    /// # Preconditions
    /// The queue is not initialized and `capacity` is greater than zero. Both are checked by an assertion in builds
    /// with assertions; no check runs otherwise.
    ///
    /// # Errors
    /// Returns false when the state or the OS queue cannot be created.
    bool Initialize(uint32 capacity);
    /// Closes the queue and releases its resources.
    void Shutdown();
    /// Returns true when requests go through an I/O ring rather than the completion-port fallback.
    ///
    /// False before `Initialize` and on systems whose IoRing is unavailable.
    bool UsesIoRing() const;

    /// Queues a read of `request.Size` bytes at `request.Offset`.
    ///
    /// With IoRing the request starts on `Submit` or `Wait`; with the completion-port fallback it starts at once. A
    /// read at or past the end of the file completes successfully with zero bytes transferred.
    ///
    /// Returns false when the queue is full or the platform rejects the request.
    bool Read(const AsyncIoRequest &request);
    /// Queues a write of `request.Size` bytes at `request.Offset`.
    ///
    /// With IoRing the request starts on `Submit` or `Wait`; with the completion-port fallback it starts at once.
    /// Returns false when the queue is full or the platform rejects the request.
    bool Write(const AsyncIoRequest &request);
    /// Starts the requests queued since the last call.
    ///
    /// With the completion-port fallback this does nothing; reads and writes start when they are queued.
    void Submit();
    /// Writes up to `maxCount` completions and returns how many, waiting up to `timeout` milliseconds for the first.
    ///
    /// With IoRing, if no completion is ready, this submits the requests queued since the last call and waits for one.
    /// Returns 0 on timeout; `~0u` waits indefinitely.
    uint32 Wait(AsyncIoCompletion *completions, uint32 maxCount, uint32 timeout);

private:
    void *_state = nullptr;
};
} // namespace Marlin
