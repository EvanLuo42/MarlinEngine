#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "EnumFlags.h"
#include "RHI/RhiDevice.h"
#include "RHI/RhiResources.h"
#include "Reflection/TypeOf.h"
#include "String/Name.h"
#include "Templates/Result.h"
#include "RHI/RhiBackend.gen.h"

namespace Marlin
{
/// The graphics API's own validation (Vulkan validation layers, the D3D12 debug layer), by level.
MENUM(Flags)
enum class RhiApiValidation : uint8
{
    None = 0,
    /// Checks API usage on the CPU.
    Api = 1 << 0,
    /// Also instruments shaders to check descriptor indices and buffer accesses on the GPU.
    GpuAssisted = 1 << 1,
    /// Also checks barriers and hazards.
    Sync = 1 << 2,
};
MARLIN_ENUM_FLAGS(RhiApiValidation)

struct RhiDeviceDesc
{
    RhiApiValidation ApiValidation = RhiApiValidation::None;
    /// The adapter of RhiBackend::GetAdapters to use; -1 picks the best one that meets the baseline.
    int32 AdapterIndex = -1;
    /// What RhiDevice::GetPipelineCacheData returned on the last run; ignored if it is from another driver.
    Span<const uint8> PipelineCache;
    /// Gives objects and markers their names in the driver, for debuggers and capture tools.
    bool DebugNames = true;
    /// Writes breadcrumbs around every marker, for GPU crash reports.
    bool Breadcrumbs = true;
    /// Called once with *DeviceLostContext*, on the thread that saw the device lost, before the call that saw it
    /// returns.
    void (*OnDeviceLost)(void *context) = nullptr;
    void *DeviceLostContext = nullptr;
    /// Called with *PipelineCreatedContext* from every CreatePipeline, on its thread, with the pipeline's shaders.
    void (*OnPipelineCreated)(void *context, Span<const RhiShader> shaders, StringView debugName) = nullptr;
    void *PipelineCreatedContext = nullptr;
};

/// A graphics API, found through reflection: the RHI subsystem creates the first backend that is available, or the
/// one Config or the command line names. Every capture and crash tool sees the backend before the device exists.
MINTERFACE()
class RHI_API IRhiBackend
{
    MGENERATED_BODY()
public:
    virtual ~IRhiBackend() = default;

    /// Such as Vulkan or D3D12; what `-Rhi=` and `Engine.Rhi.Backend` name.
    [[nodiscard]] virtual Name GetName() const = 0;
    /// Platforms try backends from the highest priority down.
    [[nodiscard]] virtual int32 GetPriority() const = 0;
    /// Loads the API; false if it is not installed or too old.
    virtual bool Initialize(RhiApiValidation validation) = 0;
    virtual void Shutdown() = 0;
    /// The adapters that meet the hardware baseline.
    [[nodiscard]] virtual Span<const RhiAdapterInfo> GetAdapters() const = 0;
    [[nodiscard]] virtual Result<RhiDevice *, RhiError> CreateDevice(const RhiDeviceDesc &desc) = 0;
    virtual void DestroyDevice(RhiDevice *device) = 0;
};
} // namespace Marlin
