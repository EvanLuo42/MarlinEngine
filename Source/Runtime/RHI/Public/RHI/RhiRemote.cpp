#include "RHI/RhiRemote.h"

#include "Engine/Engine.h"
#include "RHI/GpuTools.h"
#include "RHI/RhiSubsystem.h"

namespace Marlin
{
namespace
{
const char *LocationName(RhiMemoryLocation location)
{
    switch (location)
    {
    case RhiMemoryLocation::Device:
        return "Device";
    case RhiMemoryLocation::Upload:
        return "Upload";
    case RhiMemoryLocation::Readback:
        return "Readback";
    }
    return "Unknown";
}
} // namespace

RhiGpuInfo RhiRemoteMethods::GpuInfo()
{
    RhiGpuInfo info;
    const RhiSubsystem *rhi = Engine::GetSubsystem<RhiSubsystem>();
    const RhiDevice *device = rhi != nullptr ? rhi->GetDevice() : nullptr;
    if (device == nullptr)
    {
        return info;
    }
    const RhiAdapterInfo &adapter = device->GetAdapterInfo();
    info.Backend = String(device->GetBackendName().ToView());
    info.Adapter = adapter.Name;
    info.Driver = adapter.DriverVersion;
    info.VendorId = adapter.VendorId;
    info.DeviceMemoryBytes = adapter.DeviceMemoryBytes;
    for (const Name feature : device->GetFeatures())
    {
        info.Features.Add(String(feature.ToView()));
    }
    for (const Name layer : rhi->GetLayerNames())
    {
        info.Layers.Add(String(layer.ToView()));
    }
    if (rhi->GetCaptureTool() != nullptr)
    {
        info.CaptureTool = String(rhi->GetCaptureTool()->GetName().ToView());
    }
    return info;
}

RhiGpuMemory RhiRemoteMethods::GpuMemory()
{
    RhiGpuMemory memory;
    const RhiSubsystem *rhi = Engine::GetSubsystem<RhiSubsystem>();
    const RhiDevice *device = rhi != nullptr ? rhi->GetDevice() : nullptr;
    if (device == nullptr)
    {
        return memory;
    }
    const RhiMemoryBudget local = device->GetMemoryBudget(RhiMemoryLocation::Device);
    const RhiMemoryBudget host = device->GetMemoryBudget(RhiMemoryLocation::Upload);
    memory.DeviceBudget = local.Budget;
    memory.DeviceUsage = local.Usage;
    memory.HostBudget = host.Budget;
    memory.HostUsage = host.Usage;
    List<RhiMemoryPoolStats> pools;
    device->GetMemoryStats(pools);
    for (const RhiMemoryPoolStats &stats : pools)
    {
        RhiGpuMemoryPool &pool = memory.Pools.AddDefaulted();
        pool.Location = String(LocationName(stats.Location));
        pool.Textures = stats.Textures;
        pool.Blocks = stats.BlockCount;
        pool.Allocations = stats.AllocationCount;
        pool.BlockBytes = stats.BlockBytes;
        pool.UsedBytes = stats.UsedBytes;
    }
    return memory;
}

bool RhiRemoteMethods::Capture(uint32 frames)
{
    RhiSubsystem *rhi = Engine::GetSubsystem<RhiSubsystem>();
    if (rhi == nullptr || rhi->GetCaptureTool() == nullptr)
    {
        return false;
    }
    rhi->RequestCapture(frames);
    return true;
}
} // namespace Marlin
