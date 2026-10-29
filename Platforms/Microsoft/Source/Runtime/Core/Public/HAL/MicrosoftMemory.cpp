#include "HAL/PlatformMemory.h"

#include "Diagnostics/Assert.h"
#include "HAL/MicrosoftHeaders.h"

#include <malloc.h>

namespace Marlin::PlatformMemory
{
namespace
{
using VirtualAlloc2Function = PVOID(WINAPI *)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);

usize AllocationGranularity()
{
    static const usize granularity = [] {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<usize>(info.dwAllocationGranularity);
    }();
    return granularity;
}

VirtualAlloc2Function VirtualAlloc2Pointer()
{
    static const VirtualAlloc2Function function = [] {
        const HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        return kernelBase != nullptr
                   ? reinterpret_cast<VirtualAlloc2Function>(GetProcAddress(kernelBase, "VirtualAlloc2"))
                   : nullptr;
    }();
    return function;
}

DWORD PageProtection(PageAccess access)
{
    switch (access)
    {
    case PageAccess::None:
        return PAGE_NOACCESS;
    case PageAccess::ReadOnly:
        return PAGE_READONLY;
    case PageAccess::ReadWrite:
        return PAGE_READWRITE;
    }
    MARLIN_UNREACHABLE();
}
} // namespace

PlatformMemoryInfo GetInfo()
{
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return {
        .PageSize = system.dwPageSize,
        .AllocationGranularity = system.dwAllocationGranularity,
        .LargePageSize = GetLargePageMinimum(),
        .TotalPhysical = status.ullTotalPhys,
        .AvailablePhysical = status.ullAvailPhys,
    };
}

void *Reserve(usize size, usize alignment)
{
    if (alignment <= AllocationGranularity())
    {
        return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    }
    MARLIN_ASSERT((alignment & (alignment - 1)) == 0);
    const VirtualAlloc2Function virtualAlloc2 = VirtualAlloc2Pointer();
    if (virtualAlloc2 == nullptr)
    {
        return nullptr;
    }
    MEM_ADDRESS_REQUIREMENTS requirements = {};
    requirements.Alignment = alignment;
    MEM_EXTENDED_PARAMETER parameter = {};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return virtualAlloc2(nullptr, nullptr, size, MEM_RESERVE, PAGE_NOACCESS, &parameter, 1);
}

void Release(void *address, [[maybe_unused]] usize size)
{
    if (address != nullptr)
    {
        MARLIN_VERIFY(VirtualFree(address, 0, MEM_RELEASE));
    }
}

bool Commit(void *address, usize size)
{
    return VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void Decommit(void *address, usize size)
{
    MARLIN_VERIFY(VirtualFree(address, size, MEM_DECOMMIT));
}

bool Protect(void *address, usize size, PageAccess access)
{
    DWORD previous = 0;
    return VirtualProtect(address, size, PageProtection(access), &previous) != 0;
}

void *SystemAllocate(usize size)
{
    return malloc(size);
}

void SystemFree(void *pointer)
{
    free(pointer);
}
} // namespace Marlin::PlatformMemory
