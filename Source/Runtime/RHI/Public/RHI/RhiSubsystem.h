#pragma once

#include "Concurrency/Atomic.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "HAL/PlatformSync.h"
#include "HAL/PlatformThread.h"
#include "Memory/UniquePtr.h"
#include "RHI/RhiBackend.h"
#include "RHI/RhiDevice.h"
#include "RHI/RhiPipelineLibrary.h"
#include "String/String.h"
#include "String/StringView.h"
#include "RHI/RhiSubsystem.gen.h"

namespace Marlin
{
class IGpuCaptureTool;
class IGpuCrashTool;
class IRhiDeviceLayer;
class IRhiPresentHook;
enum class RhiLatencyMarker : uint8;
struct CrashInfo;

/// Picks a backend, injects the capture tool, sets up the crash tools, creates the device and wraps it in the
/// enabled layers. Targets that link no backend, such as servers, have none. The command line overrides Config:
/// `-Rhi=Vulkan`, `-GpuValidation=Api|Sync`, `-GpuCapture=RenderDoc`, `-GpuCaptureAt=<frame>`.
MCLASS(Config = "Engine.Rhi")
class RHI_API RhiSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// The backend to use; empty for the platform's preferred one that is available.
    MPROPERTY()
    static String Backend;
    /// The graphics API's validation; Api in Debug, None otherwise, unless Config says.
    MPROPERTY()
    static RhiApiValidation ApiValidation;
    /// The capture tool to inject at startup; empty for none.
    MPROPERTY()
    static String CaptureTool;
    MPROPERTY()
    static bool Breadcrumbs;
    /// Seconds a queue may hold unfinished work without finishing any of it before the GPU counts as hung and is
    /// handled like a lost device; zero turns the watchdog off. It pauses while a debugger is attached.
    MPROPERTY()
    static float32 GpuHangTimeout;
    /// The user-mode GPU driver modules, without extension: a crash in one of them is reported with the GPU's state,
    /// as a lost device is, since the driver can crash on its own thread before the device reports the loss.
    MPROPERTY()
    static List<String> DriverModules;

    MFUNCTION()
    static bool ShouldCreate(const SubsystemContext &context);

    /// Captures the next *frames* frames with the injected capture tool.
    MFUNCTION(ConsoleCommand = "r.GpuCapture")
    static void CaptureCommand(uint32 frames);

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;
    void EndFrame(const FrameTime &time) override;

    /// Hands the frame's queue work (completed releases, whole-frame captures) to a frame owner, such as the
    /// Renderer's render job, which calls BeginSubmitFrame at the start of every frame it submits. Until a call
    /// claims it, BeginFrame does it on the main thread. Submissions belong to one submitter at a time (07 §1.2).
    void ClaimFrameSubmission()
    {
        _frameClaimed = true;
    }

    /// The start-of-frame queue work of *frame*, by the submitter: ends and starts frame captures, releases what
    /// the queues finished with and calls the submit frame callbacks.
    void BeginSubmitFrame(uint64 frame);

    /// Runs at the start of every frame the submitter submits, on its thread, once the previous frame's work is all
    /// submitted: where memory a frame's commands use retires, which the main thread's BeginFrame is too early for
    /// while the render job of the last frame still records.
    using SubmitFrameCallback = void (*)(uint64 frame, void *context);
    void AddSubmitFrameCallback(SubmitFrameCallback callback, void *context);
    void RemoveSubmitFrameCallback(SubmitFrameCallback callback, void *context);

    /// The device with its layers; null if no backend could create one.
    [[nodiscard]] RhiDevice *GetDevice() const
    {
        return _device;
    }

    [[nodiscard]] IGpuCaptureTool *GetCaptureTool() const
    {
        return _captureTool;
    }

    /// The device layers wrapped around the backend's device, closest first.
    [[nodiscard]] List<Name> GetLayerNames() const;

    /// Captures the next *frames* whole frames, from frame boundary to frame boundary.
    void RequestCapture(uint32 frames = 1);
    /// Starts a capture of whatever is submitted until EndCapture, on the submit thread between submissions.
    bool BeginCapture(StringView title);
    /// Ends the capture; *file* is where the tool wrote it, if it says.
    bool EndCapture(String &file);
    [[nodiscard]] bool IsCapturing() const
    {
        return _capturing;
    }

    /// Swapchains and presents go through these, so present hooks (low latency SDKs, frame generation) see them.
    Result<RhiSwapchain, RhiError> CreateSwapchain(const RhiSwapchainDesc &desc);
    void DestroySwapchain(RhiSwapchain swapchain);
    /// Paces frame *frame* before its simulation starts: a hook that paces, or RhiDevice::WaitForPresent.
    void WaitForFrame(RhiSwapchain swapchain, uint64 frame);
    void SetLatencyMarker(RhiSwapchain swapchain, RhiLatencyMarker marker, uint64 frame);
    Result<void, RhiError> Present(RhiSwapchain swapchain, uint64 frame, RhiQueueType queue = RhiQueueType::Graphics);

    /// Writes the crash report for a lost device, with breadcrumbs and the crash tools' dumps, and exits; *hung* when
    /// the watchdog found a queue that stopped finishing its work.
    [[noreturn]] void HandleDeviceLost(bool hung = false);
    /// Shows the crash tools every pipeline's shaders.
    void HandlePipelineCreated(Span<const RhiShader> shaders, StringView debugName) const;

    /// The device's pipelines by key. Outside Shipping, what a run asks for is saved to /User/Pipelines on exit.
    [[nodiscard]] RhiPipelineLibrary *GetPipelineLibrary() const
    {
        return _pipelines.Get();
    }

    /// The project's pipeline list, /Project/Pipelines/<Backend>.mpl, for loading screens to preload.
    [[nodiscard]] Span<const RhiPipelineRecord> GetPipelineList() const
    {
        return _pipelineList;
    }

private:
    void LoadPipelineCache(List<uint8> &data) const;
    void SavePipelineCache() const;
    static void WatchdogMain(void *argument);
    void RunWatchdog();
    /// Logs the lost device's breadcrumbs and the crash tools' dumps under *headline*, once.
    void WriteDeviceLostReport(StringView headline);
    static void OnCrash(const CrashInfo &info, void *context);

    IRhiBackend *_backend = nullptr;
    RhiDevice *_backendDevice = nullptr;
    RhiDevice *_device = nullptr;
    List<IRhiDeviceLayer *> _layers;
    List<RhiDevice *> _layerDevices;
    IGpuCaptureTool *_captureTool = nullptr;
    List<IGpuCrashTool *> _crashTools;
    List<IRhiPresentHook *> _presentHooks;
    UniquePtr<RhiPipelineLibrary> _pipelines;
    List<RhiPipelineRecord> _pipelineList;
    /// `-GpuCaptureAt=` frame, or zero.
    uint64 _captureAtFrame = 0;
    uint32 _captureFramesLeft = 0;
    uint32 _capturesRequested = 0;
    bool _capturing = false;
    bool _frameClaimed = false;
    ThreadHandle _watchdog;
    Event _watchdogStop{EventReset::Manual};
    struct SubmitFrameListener
    {
        SubmitFrameCallback Callback = nullptr;
        void *Context = nullptr;
    };
    List<SubmitFrameListener> _submitFrameListeners;
    Atomic<bool> _deviceLostHandled{false};
    Atomic<bool> _deviceLostReported{false};
};
} // namespace Marlin
