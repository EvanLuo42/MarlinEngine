#include "HAL/PlatformCpu.h"

#include "HAL/MicrosoftHeaders.h"
#include "Templates/Bit.h"

namespace Marlin::PlatformCpu
{
CpuTopology GetTopology()
{
    CpuTopology topology;
    GROUP_AFFINITY groupAffinity = {};
    GetThreadGroupAffinity(GetCurrentThread(), &groupAffinity);

    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    const HANDLE heap = GetProcessHeap();
    auto *buffer = static_cast<uint8 *>(HeapAlloc(heap, 0, size));
    if (buffer == nullptr ||
        !GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer), &size))
    {
        HeapFree(heap, 0, buffer);
        return topology;
    }

    for (DWORD offset = 0; offset < size;)
    {
        const auto *info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer + offset);
        offset += info->Size;
        uint64 mask = 0;
        for (WORD i = 0; i < info->Processor.GroupCount; ++i)
        {
            if (info->Processor.GroupMask[i].Group == groupAffinity.Group)
            {
                mask |= info->Processor.GroupMask[i].Mask;
            }
        }
        if (mask == 0 || topology.PhysicalCount == CpuTopology::MaxCores)
        {
            continue;
        }
        topology.CoreMasks[topology.PhysicalCount++] = mask;
        topology.LogicalCount += static_cast<uint32>(PopCount(mask));
    }
    HeapFree(heap, 0, buffer);
    return topology;
}
} // namespace Marlin::PlatformCpu
