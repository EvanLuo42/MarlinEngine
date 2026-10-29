#include "HAL/PlatformAsyncIo.h"

#include "Diagnostics/Assert.h"
#include "HAL/MicrosoftHeaders.h"

#include <ioringapi.h>

namespace Marlin
{
namespace
{
struct IoRingApi
{
    decltype(&QueryIoRingCapabilities) QueryCapabilities = nullptr;
    decltype(&CreateIoRing) Create = nullptr;
    decltype(&CloseIoRing) Close = nullptr;
    decltype(&BuildIoRingReadFile) BuildRead = nullptr;
    decltype(&BuildIoRingWriteFile) BuildWrite = nullptr;
    decltype(&SubmitIoRing) Submit = nullptr;
    decltype(&PopIoRingCompletion) PopCompletion = nullptr;

    bool IsAvailable() const
    {
        return Submit != nullptr;
    }
};

template <typename TFunction> void LoadFunction(HMODULE module, const char *name, TFunction &function)
{
    function = reinterpret_cast<TFunction>(GetProcAddress(module, name));
}

const IoRingApi &GetIoRingApi()
{
    static const IoRingApi api = [] {
        IoRingApi result;
        const HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        if (kernelBase == nullptr)
        {
            return result;
        }
        IoRingApi loaded;
        LoadFunction(kernelBase, "QueryIoRingCapabilities", loaded.QueryCapabilities);
        LoadFunction(kernelBase, "CreateIoRing", loaded.Create);
        LoadFunction(kernelBase, "CloseIoRing", loaded.Close);
        LoadFunction(kernelBase, "BuildIoRingReadFile", loaded.BuildRead);
        LoadFunction(kernelBase, "BuildIoRingWriteFile", loaded.BuildWrite);
        LoadFunction(kernelBase, "SubmitIoRing", loaded.Submit);
        LoadFunction(kernelBase, "PopIoRingCompletion", loaded.PopCompletion);
        if (loaded.QueryCapabilities == nullptr || loaded.Create == nullptr || loaded.Close == nullptr ||
            loaded.BuildRead == nullptr || loaded.BuildWrite == nullptr || loaded.Submit == nullptr ||
            loaded.PopCompletion == nullptr)
        {
            return result;
        }
        IORING_CAPABILITIES capabilities = {};
        if (FAILED(loaded.QueryCapabilities(&capabilities)) || capabilities.MaxVersion < IORING_VERSION_3)
        {
            return result;
        }
        return loaded;
    }();
    return api;
}

constexpr ULONG_PTR FailedKey = 1;
constexpr LONG StatusEndOfFile = static_cast<LONG>(0xC0000011L);

struct IocpSlot
{
    OVERLAPPED Overlapped;
    void *UserData;
};

struct AsyncIoState
{
    uint32 Capacity = 0;
    uint32 InFlight = 0;
    HIORING Ring = nullptr;
    HANDLE Port = nullptr;
    IocpSlot *Slots = nullptr;
    uint32 *FreeSlots = nullptr;
    uint32 FreeCount = 0;
};

AsyncIoState *AsyncState(void *state)
{
    return static_cast<AsyncIoState *>(state);
}

bool SubmitIocp(AsyncIoState &state, const AsyncIoRequest &request, bool write)
{
    if (state.FreeCount == 0)
    {
        return false;
    }
    const uint32 index = state.FreeSlots[--state.FreeCount];
    IocpSlot &slot = state.Slots[index];
    slot = {};
    slot.UserData = request.UserData;
    slot.Overlapped.Offset = static_cast<DWORD>(request.Offset);
    slot.Overlapped.OffsetHigh = static_cast<DWORD>(request.Offset >> 32);

    auto *const file = reinterpret_cast<HANDLE>(request.File.Value);
    CreateIoCompletionPort(file, state.Port, 0, 0);
    const BOOL started = write ? WriteFile(file, request.Buffer, request.Size, nullptr, &slot.Overlapped)
                               : ReadFile(file, request.Buffer, request.Size, nullptr, &slot.Overlapped);
    if (!started)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_HANDLE_EOF)
        {
            PostQueuedCompletionStatus(state.Port, 0, 0, &slot.Overlapped);
        }
        else if (error != ERROR_IO_PENDING)
        {
            PostQueuedCompletionStatus(state.Port, 0, FailedKey, &slot.Overlapped);
        }
    }
    ++state.InFlight;
    return true;
}

bool SubmitRing(AsyncIoState &state, const AsyncIoRequest &request, bool write)
{
    if (state.InFlight == state.Capacity)
    {
        return false;
    }
    const IoRingApi &api = GetIoRingApi();
    const IORING_HANDLE_REF file = IoRingHandleRefFromHandle(reinterpret_cast<HANDLE>(request.File.Value));
    const IORING_BUFFER_REF buffer = IoRingBufferRefFromPointer(request.Buffer);
    const auto userData = reinterpret_cast<UINT_PTR>(request.UserData);
    const HRESULT result =
        write ? api.BuildWrite(state.Ring, file, buffer, request.Size, request.Offset, FILE_WRITE_FLAGS_NONE, userData,
                               IOSQE_FLAGS_NONE)
              : api.BuildRead(state.Ring, file, buffer, request.Size, request.Offset, userData, IOSQE_FLAGS_NONE);
    if (FAILED(result))
    {
        return false;
    }
    ++state.InFlight;
    return true;
}

uint32 PopRing(AsyncIoState &state, AsyncIoCompletion *completions, uint32 maxCount)
{
    const IoRingApi &api = GetIoRingApi();
    uint32 count = 0;
    IORING_CQE entry;
    while (count < maxCount && api.PopCompletion(state.Ring, &entry) == S_OK)
    {
        const bool atEnd = entry.ResultCode == HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        completions[count++] = {
            .UserData = reinterpret_cast<void *>(entry.UserData),
            .BytesTransferred = atEnd ? 0 : static_cast<uint32>(entry.Information),
            .Succeeded = SUCCEEDED(entry.ResultCode) || atEnd,
        };
        --state.InFlight;
    }
    return count;
}
} // namespace

AsyncIoQueue::~AsyncIoQueue()
{
    Shutdown();
}

bool AsyncIoQueue::Initialize(uint32 capacity)
{
    MARLIN_ASSERT(_state == nullptr && capacity > 0);
    const HANDLE heap = GetProcessHeap();
    auto *state = static_cast<AsyncIoState *>(HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(AsyncIoState)));
    if (state == nullptr)
    {
        return false;
    }
    state->Capacity = capacity;

    const IoRingApi &api = GetIoRingApi();
    if (api.IsAvailable())
    {
        const IORING_CREATE_FLAGS flags = {IORING_CREATE_REQUIRED_FLAGS_NONE, IORING_CREATE_ADVISORY_FLAGS_NONE};
        if (SUCCEEDED(api.Create(IORING_VERSION_3, flags, capacity, capacity * 2, &state->Ring)))
        {
            _state = state;
            return true;
        }
        state->Ring = nullptr;
    }

    state->Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    state->Slots = static_cast<IocpSlot *>(HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(IocpSlot) * capacity));
    state->FreeSlots = static_cast<uint32 *>(HeapAlloc(heap, 0, sizeof(uint32) * capacity));
    if (state->Port == nullptr || state->Slots == nullptr || state->FreeSlots == nullptr)
    {
        _state = state;
        Shutdown();
        return false;
    }
    for (uint32 i = 0; i < capacity; ++i)
    {
        state->FreeSlots[i] = capacity - 1 - i;
    }
    state->FreeCount = capacity;
    _state = state;
    return true;
}

void AsyncIoQueue::Shutdown()
{
    AsyncIoState *state = AsyncState(_state);
    if (state == nullptr)
    {
        return;
    }
    if (state->Ring != nullptr)
    {
        GetIoRingApi().Close(state->Ring);
    }
    if (state->Port != nullptr)
    {
        CloseHandle(state->Port);
    }
    const HANDLE heap = GetProcessHeap();
    HeapFree(heap, 0, state->Slots);
    HeapFree(heap, 0, state->FreeSlots);
    HeapFree(heap, 0, state);
    _state = nullptr;
}

bool AsyncIoQueue::UsesIoRing() const
{
    return _state != nullptr && AsyncState(_state)->Ring != nullptr;
}

bool AsyncIoQueue::Read(const AsyncIoRequest &request)
{
    AsyncIoState &state = *AsyncState(_state);
    return state.Ring != nullptr ? SubmitRing(state, request, false) : SubmitIocp(state, request, false);
}

bool AsyncIoQueue::Write(const AsyncIoRequest &request)
{
    AsyncIoState &state = *AsyncState(_state);
    return state.Ring != nullptr ? SubmitRing(state, request, true) : SubmitIocp(state, request, true);
}

void AsyncIoQueue::Submit()
{
    const AsyncIoState &state = *AsyncState(_state);
    if (state.Ring != nullptr)
    {
        GetIoRingApi().Submit(state.Ring, 0, 0, nullptr);
    }
}

uint32 AsyncIoQueue::Wait(AsyncIoCompletion *completions, uint32 maxCount, uint32 timeout)
{
    AsyncIoState &state = *AsyncState(_state);
    if (state.Ring != nullptr)
    {
        uint32 count = PopRing(state, completions, maxCount);
        if (count == 0 && timeout != 0 && state.InFlight > 0)
        {
            GetIoRingApi().Submit(state.Ring, 1, timeout, nullptr);
            count = PopRing(state, completions, maxCount);
        }
        return count;
    }

    constexpr uint32 MaxBatch = 64;
    OVERLAPPED_ENTRY entries[MaxBatch];
    ULONG removed = 0;
    const ULONG batch = maxCount < MaxBatch ? maxCount : MaxBatch;
    if (!GetQueuedCompletionStatusEx(state.Port, entries, batch, &removed, timeout, FALSE))
    {
        return 0;
    }
    for (ULONG i = 0; i < removed; ++i)
    {
        const auto *slot = CONTAINING_RECORD(entries[i].lpOverlapped, IocpSlot, Overlapped);
        const auto status = static_cast<LONG>(entries[i].lpOverlapped->Internal);
        const bool atEnd = status == StatusEndOfFile;
        completions[i] = {
            .UserData = slot->UserData,
            .BytesTransferred = atEnd ? 0 : entries[i].dwNumberOfBytesTransferred,
            .Succeeded = entries[i].lpCompletionKey != FailedKey && (status >= 0 || atEnd),
        };
        state.FreeSlots[state.FreeCount++] = static_cast<uint32>(slot - state.Slots);
        --state.InFlight;
    }
    return removed;
}
} // namespace Marlin
