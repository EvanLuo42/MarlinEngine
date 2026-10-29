#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Reflection/TypeOf.h"
#include "String/String.h"
#include "RHI/RhiRemote.gen.h"

namespace Marlin
{
MSTRUCT()
struct RhiGpuInfo
{
    MGENERATED_BODY()

    MPROPERTY()
    String Backend;
    MPROPERTY()
    String Adapter;
    MPROPERTY()
    String Driver;
    MPROPERTY()
    uint32 VendorId = 0;
    MPROPERTY()
    uint64 DeviceMemoryBytes = 0;
    MPROPERTY()
    List<String> Features;
    MPROPERTY()
    List<String> Layers;
    MPROPERTY()
    String CaptureTool;
};

MSTRUCT()
struct RhiGpuMemoryPool
{
    MGENERATED_BODY()

    /// Device, Upload or Readback.
    MPROPERTY()
    String Location;
    MPROPERTY()
    bool Textures = false;
    MPROPERTY()
    uint32 Blocks = 0;
    MPROPERTY()
    uint32 Allocations = 0;
    MPROPERTY()
    uint64 BlockBytes = 0;
    MPROPERTY()
    uint64 UsedBytes = 0;
};

MSTRUCT()
struct RhiGpuMemory
{
    MGENERATED_BODY()

    MPROPERTY()
    uint64 DeviceBudget = 0;
    MPROPERTY()
    uint64 DeviceUsage = 0;
    MPROPERTY()
    uint64 HostBudget = 0;
    MPROPERTY()
    uint64 HostUsage = 0;
    MPROPERTY()
    List<RhiGpuMemoryPool> Pools;
};

/// The Render group of Remote methods the RHI serves (Documentation/Architecture/10): GPU information and memory,
/// and captures, so the editor can capture a game process or a devkit.
MSTRUCT(Category = "Render")
struct RhiRemoteMethods
{
    MGENERATED_BODY()

    /// The backend, adapter, features, device layers and capture tool.
    MFUNCTION(Remote)
    static RhiGpuInfo GpuInfo();

    /// The driver's budget and what the RHI's allocator holds, pool by pool.
    MFUNCTION(Remote)
    static RhiGpuMemory GpuMemory();

    /// Captures the next *frames* frames with the injected capture tool; false if there is none.
    MFUNCTION(Remote)
    static bool Capture(uint32 frames = 1);
};
} // namespace Marlin
