#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "RHI/RhiHandle.h"
#include "Reflection/TypeOf.h"
#include "String/Name.h"
#include "String/String.h"
#include "String/StringView.h"
#include "RHI/GpuTools.gen.h"

namespace Marlin
{
class IRhiBackend;
class RhiDevice;
class StringWriter;
struct RhiShader;

/// A frame debugger the engine can start captures with, such as RenderDoc or Nsight Graphics, found through
/// reflection. Tools hook the graphics API, so at most one is injected, at startup, before the backend loads.
MINTERFACE()
class RHI_API IGpuCaptureTool
{
    MGENERATED_BODY()
public:
    virtual ~IGpuCaptureTool() = default;

    /// What `-GpuCapture=` names.
    [[nodiscard]] virtual Name GetName() const = 0;
    [[nodiscard]] virtual bool SupportsBackend(Name backend) const = 0;
    /// True if the tool already hooked the process, because it launched it.
    [[nodiscard]] virtual bool IsAttached() const = 0;
    /// Loads the tool into the process before the backend loads the API; false if it is not installed.
    virtual bool Inject() = 0;
    /// After the backend loads the API, before the device exists: the tool may ask the backend for extensions.
    virtual void PrepareBackend(IRhiBackend &backend)
    {
    }
    virtual void OnDeviceCreated(RhiDevice &device) = 0;
    virtual void OnDeviceDestroying(RhiDevice &device) = 0;
    /// Starts capturing everything submitted from now on; on the submit thread, between submissions.
    virtual bool BeginCapture(RhiDevice &device, StringView title) = 0;
    /// Ends the capture and gives the file it wrote, or nothing if the tool keeps captures itself.
    virtual bool EndCapture(RhiDevice &device, String &file) = 0;
    /// Opens the tool's UI on *file*, if it can.
    virtual bool OpenCapture(StringView file)
    {
        return false;
    }
};

/// Collects GPU crash dumps, such as Nsight Aftermath or DRED, found through reflection. Every enabled tool that
/// supports the backend is set up before the device is created. Crash tools run alongside a capture tool, at a
/// level that does not disturb it: costly modes, such as Aftermath's call stack capturing or shader error
/// reporting, change GPU timings and what the capture replays.
MINTERFACE()
class RHI_API IGpuCrashTool
{
    MGENERATED_BODY()
public:
    virtual ~IGpuCrashTool() = default;

    [[nodiscard]] virtual Name GetName() const = 0;
    [[nodiscard]] virtual bool SupportsBackend(Name backend) const = 0;
    /// After the backend loads the API, before the device exists; it may ask the backend for extensions.
    /// *captureTool* is the injected capture tool, if any. False if the tool cannot run here, such as on another
    /// GPU vendor.
    virtual bool Initialize(IRhiBackend &backend, const IGpuCaptureTool *captureTool) = 0;
    virtual void Shutdown() = 0;
    virtual void OnDeviceCreated(RhiDevice &device) = 0;
    virtual void OnDeviceDestroying(RhiDevice &device) = 0;
    /// Every pipeline's shaders as it is created, on the creating thread, so dumps can name the shaders that faulted.
    virtual void OnPipelineCreated(Span<const RhiShader> /*shaders*/, StringView /*debugName*/)
    {
    }
    /// After device loss: waits for the tool's dump and writes where it is to the crash report.
    virtual void OnDeviceLost(RhiDevice &device, StringWriter &report) = 0;
};

/// Points in a frame that latency tools time, such as Reflex.
enum class RhiLatencyMarker : uint8
{
    SimulationStart,
    SimulationEnd,
    RenderSubmitStart,
    RenderSubmitEnd,
    PresentStart,
    PresentEnd,
    InputSample,
};

/// Hooks into presenting, such as a low latency SDK (Reflex, Anti-Lag 2, XeLL) or frame generation, found through
/// reflection; every enabled hook that supports the backend runs. Swapchains and presents go through RhiSubsystem,
/// which calls the hooks around the device.
MINTERFACE()
class RHI_API IRhiPresentHook
{
    MGENERATED_BODY()
public:
    virtual ~IRhiPresentHook() = default;

    [[nodiscard]] virtual Name GetName() const = 0;
    [[nodiscard]] virtual bool SupportsBackend(Name backend) const = 0;
    /// Before the backend loads the API: an SDK that interposes on it, such as Streamline, replaces the loader and
    /// asks for instance extensions here.
    virtual void BeforeBackendLoad(IRhiBackend &backend)
    {
    }
    /// After the backend loads the API, before the device exists; it may ask the backend for device extensions and
    /// queues. False if the hook cannot run here, such as on another GPU vendor or when Config turns it off.
    virtual bool Initialize(IRhiBackend &backend) = 0;
    virtual void Shutdown() = 0;
    virtual void OnDeviceCreated(RhiDevice &device) = 0;
    virtual void OnDeviceDestroying(RhiDevice &device) = 0;
    virtual void OnSwapchainCreated(RhiDevice &device, RhiSwapchain swapchain)
    {
    }
    virtual void OnSwapchainDestroying(RhiDevice &device, RhiSwapchain swapchain)
    {
    }
    /// Paces the frame instead of RhiDevice::WaitForPresent; false to leave it to the device.
    virtual bool WaitForFrame(RhiDevice &device, RhiSwapchain swapchain, uint64 frame)
    {
        return false;
    }
    virtual void SetMarker(RhiDevice &device, RhiSwapchain swapchain, RhiLatencyMarker marker, uint64 frame)
    {
    }
    virtual void BeforePresent(RhiDevice &device, RhiSwapchain swapchain, uint64 frame)
    {
    }
    virtual void AfterPresent(RhiDevice &device, RhiSwapchain swapchain, uint64 frame)
    {
    }
};

/// Wraps a device in another that checks or records what is done with it, such as RHIValidation; found through
/// reflection and applied in order of priority, the lowest closest to the backend.
MINTERFACE()
class RHI_API IRhiDeviceLayer
{
    MGENERATED_BODY()
public:
    virtual ~IRhiDeviceLayer() = default;

    [[nodiscard]] virtual Name GetName() const = 0;
    [[nodiscard]] virtual int32 GetPriority() const = 0;
    /// From the command line or Config.
    [[nodiscard]] virtual bool IsEnabled() const = 0;
    /// A device that forwards to *inner*, which it does not own.
    [[nodiscard]] virtual RhiDevice *Wrap(RhiDevice &inner) = 0;
    virtual void Unwrap(RhiDevice *outer) = 0;
};
} // namespace Marlin
