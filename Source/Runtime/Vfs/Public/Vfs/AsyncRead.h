#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "HAL/PlatformSync.h"
#include "Memory/SharedPtr.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
/// The order queued reads start in; a running read is never preempted.
enum class IoPriority : uint8
{
    /// The frame waits on it, such as a missing texture the camera looks at.
    Critical,
    Streaming,
    Background,
};

struct FileRegion;

namespace Private
{
struct AsyncReadState;
} // namespace Private

/// A read in flight. Copies share it; the bytes live as long as any copy does.
class VFS_API AsyncReadRequest
{
public:
    AsyncReadRequest();
    explicit AsyncReadRequest(SharedPtr<Private::AsyncReadState> state);
    ~AsyncReadRequest();
    AsyncReadRequest(const AsyncReadRequest &other);
    AsyncReadRequest &operator=(const AsyncReadRequest &other);
    AsyncReadRequest(AsyncReadRequest &&other) noexcept;
    AsyncReadRequest &operator=(AsyncReadRequest &&other) noexcept;

    [[nodiscard]] bool IsValid() const;
    [[nodiscard]] bool IsDone() const;
    [[nodiscard]] bool Succeeded() const;
    /// The bytes read, once it succeeded.
    [[nodiscard]] Span<const uint8> GetData() const;
    /// Hands the bytes read to the caller, who frees them with Memory::Free; GetData is empty afterwards. Null if
    /// the read did not succeed.
    [[nodiscard]] uint8 *TakeData(usize &size) const;

    /// Blocks until the read finishes; false on timeout, in milliseconds. A job should use the completion callback
    /// instead of blocking its worker.
    bool Wait(uint32 timeout = InfiniteTimeout) const;
    /// Drops the read if it has not started; it then completes without success.
    void Cancel() const;

private:
    SharedPtr<Private::AsyncReadState> _state;
};

using AsyncReadCallback = Function<void(const AsyncReadRequest &request)>;

namespace Vfs
{
inline constexpr uint64 WholeFile = ~uint64{0};

/// Reads *size* bytes of *path* from *offset* on the I/O thread, which batches reads through the platform's
/// asynchronous I/O (IoRing on Windows). *completion* runs on the I/O thread when the read ends, successful or not,
/// and should hand longer work to a job. A path that does not resolve completes at once without success.
VFS_API AsyncReadRequest ReadAsync(StringView path, IoPriority priority = IoPriority::Streaming, uint64 offset = 0,
                                   uint64 size = WholeFile, AsyncReadCallback completion = {});
/// Reads part of *region*, such as an object in a package that a catalog located; *offset* is within the region.
VFS_API AsyncReadRequest ReadAsync(const FileRegion &region, IoPriority priority = IoPriority::Streaming,
                                   uint64 offset = 0, uint64 size = WholeFile, AsyncReadCallback completion = {});

/// Finishes the reads in flight, cancels the queued ones and stops the I/O thread; ReadAsync starts it again.
VFS_API void ShutdownAsync();
} // namespace Vfs
} // namespace Marlin
