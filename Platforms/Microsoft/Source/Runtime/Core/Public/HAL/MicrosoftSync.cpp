#include "HAL/PlatformSync.h"

#include "Diagnostics/Assert.h"
#include "HAL/MicrosoftHeaders.h"

namespace Marlin
{
static_assert(sizeof(SRWLOCK) == sizeof(void *), "Mutex and ReadWriteLock hold an SRWLOCK in a pointer");
static_assert(InfiniteTimeout == INFINITE);

void Mutex::Lock()
{
    AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&_state));
}

bool Mutex::TryLock()
{
    return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&_state)) != 0;
}

void Mutex::Unlock()
{
    ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&_state));
}

void ReadWriteLock::Lock()
{
    AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&_state));
}

bool ReadWriteLock::TryLock()
{
    return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&_state)) != 0;
}

void ReadWriteLock::Unlock()
{
    ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&_state));
}

void ReadWriteLock::LockShared()
{
    AcquireSRWLockShared(reinterpret_cast<PSRWLOCK>(&_state));
}

bool ReadWriteLock::TryLockShared()
{
    return TryAcquireSRWLockShared(reinterpret_cast<PSRWLOCK>(&_state)) != 0;
}

void ReadWriteLock::UnlockShared()
{
    ReleaseSRWLockShared(reinterpret_cast<PSRWLOCK>(&_state));
}

Event::Event(EventReset reset, const char *name)
{
    const Microsoft::WideObjectName wideName(name);
    if (name == nullptr || wideName.Get() != nullptr)
    {
        _handle = CreateEventW(nullptr, reset == EventReset::Manual ? TRUE : FALSE, FALSE, wideName.Get());
    }
    if (_handle == nullptr)
    {
        MARLIN_FATAL("CreateEventW failed");
    }
}

Event::~Event()
{
    CloseHandle(_handle);
}

void Event::Set()
{
    SetEvent(_handle);
}

void Event::Reset()
{
    ResetEvent(_handle);
}

bool Event::Wait(uint32 timeout)
{
    return WaitForSingleObject(_handle, timeout) == WAIT_OBJECT_0;
}

Semaphore::Semaphore(uint32 initialCount, uint32 maximumCount)
    : _handle(CreateSemaphoreW(nullptr, static_cast<LONG>(initialCount), static_cast<LONG>(maximumCount), nullptr))
{
    if (_handle == nullptr)
    {
        MARLIN_FATAL("CreateSemaphoreW failed");
    }
}

Semaphore::~Semaphore()
{
    CloseHandle(_handle);
}

void Semaphore::Release(uint32 count)
{
    MARLIN_VERIFY(ReleaseSemaphore(_handle, static_cast<LONG>(count), nullptr));
}

bool Semaphore::Wait(uint32 timeout)
{
    return WaitForSingleObject(_handle, timeout) == WAIT_OBJECT_0;
}

namespace PlatformSync
{
bool WaitOnAddress(const void *address, const void *expected, usize size, uint32 timeout)
{
    return ::WaitOnAddress(const_cast<void *>(address), const_cast<void *>(expected), size, timeout) != FALSE;
}

void WakeOneOnAddress(const void *address)
{
    WakeByAddressSingle(const_cast<void *>(address));
}

void WakeAllOnAddress(const void *address)
{
    WakeByAddressAll(const_cast<void *>(address));
}
} // namespace PlatformSync
} // namespace Marlin
