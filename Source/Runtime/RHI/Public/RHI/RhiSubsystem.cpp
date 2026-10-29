#include "RHI/RhiSubsystem.h"

#include "CrashReport/CrashReportClient.h"
#include "Diagnostics/Log.h"
#include "Diagnostics/Profiler.h"
#include "Engine/Engine.h"
#include "Engine/StartupProgress.h"
#include "HAL/PlatformDebug.h"
#include "HAL/PlatformStackTrace.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "RHI/GpuTools.h"
#include "RHI/RhiLog.h"
#include "Reflection/Reflection.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

MARLIN_DEFINE_LOG_CATEGORY(LogRhi);

namespace Marlin
{
String RhiSubsystem::Backend;
RhiApiValidation RhiSubsystem::ApiValidation = MARLIN_BUILD_DEBUG ? RhiApiValidation::Api : RhiApiValidation::None;
String RhiSubsystem::CaptureTool;
bool RhiSubsystem::Breadcrumbs = true;
// RECOVERY-DECISION D2: 10 s default, checked every second, paused only under a debugger (the tooltip).
float32 RhiSubsystem::GpuHangTimeout = 10;
List<String> RhiSubsystem::DriverModules;

namespace
{
constexpr uint32 WatchdogPeriodMilliseconds = 1000;
/// How long a driver crash waits for another thread's device-lost report, which itself waits up to 10 s for a dump.
constexpr uint32 DeviceLostReportWaitMilliseconds = 15000;
constexpr StringView QueueNames[RhiQueueTypeCount] = {"Graphics", "Compute", "Copy"};

constexpr StringView AdapterKindName(RhiAdapterKind kind)
{
    return kind == RhiAdapterKind::Discrete     ? "discrete"
           : kind == RhiAdapterKind::Integrated ? "integrated"
           : kind == RhiAdapterKind::Software   ? "software"
                                                : "other";
}

template <typename TInterface> void CreateImplementations(List<TInterface *> &objects)
{
    List<const TypeInfo *> types;
    Reflection::GetDerivedTypes(TypeOf<TInterface>(), types);
    types.Sort([](const TypeInfo *a, const TypeInfo *b) { return a->GetName().CompareTo(b->GetName()) < 0; });
    for (const TypeInfo *type : types)
    {
        if (type->GetKind() != TypeKind::Class || type->IsAbstract())
        {
            continue;
        }
        void *object = type->New();
        if (object == nullptr)
        {
            MARLIN_LOG(LogRhi, Error, "{0} cannot be constructed", type->GetName());
            continue;
        }
        objects.Add(static_cast<TInterface *>(type->CastTo(object, TypeOf<TInterface>())));
    }
}

template <typename TInterface> void DestroyAll(List<TInterface *> &objects)
{
    for (TInterface *object : objects)
    {
        delete object;
    }
    objects.Clear();
}

String GetPipelineCachePath(Name backend)
{
    StringBuilder<128> path;
    FormatTo(path, "/Cache/Rhi/PipelineCache-{0}.bin", backend);
    return String(path.AsView());
}
} // namespace

bool RhiSubsystem::ShouldCreate(const SubsystemContext & /*context*/)
{
    if (CommandLine::HasSwitch("NoRhi"))
    {
        return false;
    }
    List<const TypeInfo *> types;
    Reflection::GetDerivedTypes(TypeOf<IRhiBackend>(), types);
    for (const TypeInfo *type : types)
    {
        if (type->GetKind() == TypeKind::Class && !type->IsAbstract())
        {
            return true;
        }
    }
    return false;
}

void RhiSubsystem::CaptureCommand(uint32 frames)
{
    if (RhiSubsystem *rhi = Engine::GetSubsystem<RhiSubsystem>())
    {
        if (rhi->GetCaptureTool() == nullptr)
        {
            MARLIN_LOG(LogRhi, Warning, "No capture tool is injected; start with -GpuCapture=<tool>");
            return;
        }
        rhi->RequestCapture(frames);
    }
}

void RhiSubsystem::Initialize(SubsystemContext & /*context*/)
{
    StartupProgress::SetStatus("Creating GPU device");
    StringView value;
    // Shipping takes no GPU debugging from players: no validation layers, captures or capture frames.
    if (MARLIN_BUILD_SHIPPING || !CommandLine::TryGetValue("GpuCaptureAt", _captureAtFrame))
    {
        _captureAtFrame = 0;
    }
    String backendName = Backend;
    if (CommandLine::TryGetValue("Rhi", value))
    {
        backendName = String(value);
    }
    RhiApiValidation validation = MARLIN_BUILD_SHIPPING ? RhiApiValidation::None : ApiValidation;
    if (!MARLIN_BUILD_SHIPPING && CommandLine::TryGetValue("GpuValidation", value) &&
        !Reflection::ParseValue(value, TypeOf<RhiApiValidation>(), &validation))
    {
        MARLIN_LOG(LogRhi, Warning, "-GpuValidation={0} is not a valid RhiApiValidation", value);
    }
    String captureName = MARLIN_BUILD_SHIPPING ? String() : CaptureTool;
    if (!MARLIN_BUILD_SHIPPING && CommandLine::TryGetValue("GpuCapture", value))
    {
        captureName = String(value);
    }

    List<IRhiBackend *> backends;
    CreateImplementations(backends);
    backends.Sort([](const IRhiBackend *a, const IRhiBackend *b) { return a->GetPriority() > b->GetPriority(); });
    List<IGpuCaptureTool *> captureTools;
    CreateImplementations(captureTools);
    List<IGpuCrashTool *> crashTools;
    CreateImplementations(crashTools);
    List<IRhiPresentHook *> presentHooks;
    CreateImplementations(presentHooks);

    for (IRhiBackend *backend : backends)
    {
        if (!backendName.IsEmpty() && backend->GetName().ToView() != backendName.AsView())
        {
            continue;
        }
        const Name name = backend->GetName();
        for (IRhiPresentHook *hook : presentHooks)
        {
            if (hook->SupportsBackend(name))
            {
                hook->BeforeBackendLoad(*backend);
            }
        }
        if (!captureName.IsEmpty() && _captureTool == nullptr)
        {
            for (IGpuCaptureTool *tool : captureTools)
            {
                if (tool->GetName().ToView() == captureName.AsView() && tool->SupportsBackend(name))
                {
                    if (tool->IsAttached() || tool->Inject())
                    {
                        _captureTool = tool;
                    }
                    else
                    {
                        MARLIN_LOG(LogRhi, Warning, "{0} is not installed; running without a capture tool",
                                   captureName);
                    }
                    break;
                }
            }
        }
        if (!backend->Initialize(validation))
        {
            MARLIN_LOG(LogRhi, Warning, "The {0} backend is not available", name);
            continue;
        }
        _backend = backend;
        break;
    }
    if (_backend != nullptr && _captureTool != nullptr)
    {
        _captureTool->PrepareBackend(*_backend);
    }
    for (IGpuCrashTool *tool : crashTools)
    {
        if (_backend != nullptr && tool->SupportsBackend(_backend->GetName()) &&
            tool->Initialize(*_backend, _captureTool))
        {
            _crashTools.Add(tool);
        }
    }
    for (IRhiPresentHook *hook : presentHooks)
    {
        if (_backend != nullptr && hook->SupportsBackend(_backend->GetName()) && hook->Initialize(*_backend))
        {
            _presentHooks.Add(hook);
            MARLIN_LOG(LogRhi, Info, "Present hook {0} enabled", hook->GetName());
        }
        else
        {
            delete hook;
        }
    }
    if (_captureTool == nullptr && !captureName.IsEmpty())
    {
        MARLIN_LOG(LogRhi, Warning, "No capture tool named {0} supports the backend", captureName);
    }
    for (IRhiBackend *backend : backends)
    {
        if (backend != _backend)
        {
            delete backend;
        }
    }
    for (IGpuCaptureTool *tool : captureTools)
    {
        if (tool != _captureTool)
        {
            delete tool;
        }
    }
    for (IGpuCrashTool *tool : crashTools)
    {
        if (!_crashTools.Contains(tool))
        {
            delete tool;
        }
    }
    if (_backend == nullptr)
    {
        MARLIN_LOG(LogRhi, Error, "No graphics backend is available");
        return;
    }

    List<uint8> pipelineCache;
    LoadPipelineCache(pipelineCache);
    RhiDeviceDesc desc;
    desc.ApiValidation = validation;
    desc.PipelineCache = pipelineCache;
    desc.Breadcrumbs = Breadcrumbs;
    desc.DebugNames = !MARLIN_BUILD_SHIPPING;
    desc.OnDeviceLost = [](void *context) { static_cast<RhiSubsystem *>(context)->HandleDeviceLost(); };
    desc.DeviceLostContext = this;
    if (!_crashTools.IsEmpty())
    {
        desc.OnPipelineCreated = [](void *context, Span<const RhiShader> shaders, StringView debugName) {
            static_cast<const RhiSubsystem *>(context)->HandlePipelineCreated(shaders, debugName);
        };
        desc.PipelineCreatedContext = this;
    }
    Result<RhiDevice *, RhiError> device = _backend->CreateDevice(desc);
    if (device.IsError())
    {
        MARLIN_LOG(LogRhi, Error, "The {0} backend cannot create a device: {1}", _backend->GetName(),
                   GetErrorName(device.Error()));
        return;
    }
    _backendDevice = device.Value();
    _device = _backendDevice;
    const RhiAdapterInfo &adapter = _device->GetAdapterInfo();
    StringBuilder<256> details(AdapterKindName(adapter.Kind));
    if (adapter.DeviceMemoryBytes != 0)
    {
        FormatTo(details, ", {0:F1} GiB device memory",
                 static_cast<float64>(adapter.DeviceMemoryBytes) / (1024.0 * 1024.0 * 1024.0));
    }
    if (adapter.UnifiedMemory)
    {
        details.Append(", unified memory");
    }
    MARLIN_LOG(LogRhi, Info, "{0} device on {1} ({2}); driver {3}", _backend->GetName(), adapter.Name, details.AsView(),
               adapter.DriverVersion);

    if (_captureTool != nullptr)
    {
        _captureTool->OnDeviceCreated(*_backendDevice);
    }
    for (IGpuCrashTool *tool : _crashTools)
    {
        tool->OnDeviceCreated(*_backendDevice);
    }
    for (IRhiPresentHook *hook : _presentHooks)
    {
        hook->OnDeviceCreated(*_backendDevice);
    }

    CreateImplementations(_layers);
    _layers.Sort(
        [](const IRhiDeviceLayer *a, const IRhiDeviceLayer *b) { return a->GetPriority() < b->GetPriority(); });
    usize kept = 0;
    for (IRhiDeviceLayer *layer : _layers)
    {
        if (!layer->IsEnabled())
        {
            delete layer;
            continue;
        }
        _layers[kept++] = layer;
        _device = layer->Wrap(*_device);
        _layerDevices.Add(_device);
        MARLIN_LOG(LogRhi, Info, "{0} layer enabled", layer->GetName());
    }
    _layers.Resize(kept);

    _pipelines = MakeUnique<RhiPipelineLibrary>(*_device);
    StringBuilder<128> listPath;
    FormatTo(listPath, "/Project/Pipelines/{0}.mpl", _backend->GetName());
    if (Vfs::IsMounted("/Project") && Vfs::Exists(listPath.AsView()) &&
        RhiPipelineLibrary::Load(listPath.AsView(), _backend->GetName(), _pipelineList))
    {
        MARLIN_LOG(LogRhi, Info, "{0} pipelines listed for preloading", _pipelineList.Count());
    }

    if (GpuHangTimeout > 0)
    {
        ThreadDesc thread;
        thread.Name = "GPU Watchdog";
        thread.Entry = &RhiSubsystem::WatchdogMain;
        thread.Argument = this;
        thread.Priority = ThreadPriority::BelowNormal;
        _watchdog = PlatformThread::Create(thread);
    }
    if (!DriverModules.IsEmpty() && !CrashReport::AddCrashHook(&RhiSubsystem::OnCrash, this))
    {
        MARLIN_LOG(LogRhi, Warning, "No crash hook is free: a GPU driver crash is reported without the GPU's state");
    }
}

void RhiSubsystem::Deinitialize()
{
    CrashReport::RemoveCrashHook(&RhiSubsystem::OnCrash, this);
    if (_watchdog.IsValid())
    {
        _watchdogStop.Set();
        PlatformThread::Join(_watchdog);
        _watchdog = {};
    }
    if (_backendDevice != nullptr)
    {
        if (_capturing)
        {
            String file;
            EndCapture(file);
        }
        _backendDevice->WaitIdle();
        if (_pipelines && !MARLIN_BUILD_SHIPPING)
        {
            const List<RhiPipelineRecord> records = _pipelines->GetRecords();
            if (!records.IsEmpty())
            {
                StringBuilder<128> path;
                FormatTo(path, "/User/Pipelines/{0}-{1}.mpl", _backend->GetName(), PlatformTime::UtcNow());
                RhiPipelineLibrary::Save(path.AsView(), _backend->GetName(), records);
            }
        }
        _pipelines.Reset();
        _backendDevice->ProcessCompletions();
        SavePipelineCache();
        for (usize i = _layers.Count(); i-- > 0;)
        {
            _layers[i]->Unwrap(_layerDevices[i]);
        }
        if (_captureTool != nullptr)
        {
            _captureTool->OnDeviceDestroying(*_backendDevice);
        }
        for (IGpuCrashTool *tool : _crashTools)
        {
            tool->OnDeviceDestroying(*_backendDevice);
        }
        for (IRhiPresentHook *hook : _presentHooks)
        {
            hook->OnDeviceDestroying(*_backendDevice);
        }
        _backend->DestroyDevice(_backendDevice);
    }
    DestroyAll(_layers);
    _layerDevices.Clear();
    _device = nullptr;
    _backendDevice = nullptr;
    for (IGpuCrashTool *tool : _crashTools)
    {
        tool->Shutdown();
    }
    DestroyAll(_crashTools);
    for (IRhiPresentHook *hook : _presentHooks)
    {
        hook->Shutdown();
    }
    DestroyAll(_presentHooks);
    delete _captureTool;
    _captureTool = nullptr;
    if (_backend != nullptr)
    {
        _backend->Shutdown();
        delete _backend;
        _backend = nullptr;
    }
}

void RhiSubsystem::BeginFrame(const FrameTime &time)
{
    if (!_frameClaimed)
    {
        BeginSubmitFrame(time.Frame);
    }
}

void RhiSubsystem::BeginSubmitFrame(uint64 frame)
{
    if (_device == nullptr)
    {
        return;
    }
    if (_captureFramesLeft > 0 && --_captureFramesLeft == 0)
    {
        String file;
        EndCapture(file);
    }
    if (_captureAtFrame != 0 && frame == _captureAtFrame)
    {
        RequestCapture(1);
    }
    if (_capturesRequested > 0 && !_capturing)
    {
        StringBuilder<64> title;
        FormatTo(title, "Frame {0}", frame);
        if (BeginCapture(title.AsView()))
        {
            _captureFramesLeft = _capturesRequested;
        }
        _capturesRequested = 0;
    }
    _device->ProcessCompletions();
    for (const SubmitFrameListener &listener : _submitFrameListeners)
    {
        listener.Callback(frame, listener.Context);
    }
}

void RhiSubsystem::AddSubmitFrameCallback(SubmitFrameCallback callback, void *context)
{
    _submitFrameListeners.Add({callback, context});
}

void RhiSubsystem::RemoveSubmitFrameCallback(SubmitFrameCallback callback, void *context)
{
    for (usize i = 0; i < _submitFrameListeners.Count();)
    {
        if (_submitFrameListeners[i].Callback == callback && _submitFrameListeners[i].Context == context)
        {
            _submitFrameListeners.RemoveAt(i);
            continue;
        }
        ++i;
    }
}

void RhiSubsystem::EndFrame(const FrameTime & /*time*/)
{
    if (_device == nullptr)
    {
        return;
    }
#if MARLIN_PROFILING
    const RhiMemoryBudget budget = _device->GetMemoryBudget(RhiMemoryLocation::Device);
    MARLIN_PROFILE_PLOT("GPU Memory Usage (MiB)", static_cast<float64>(budget.Usage) / (1024.0 * 1024.0));
    MARLIN_PROFILE_PLOT("GPU Memory Budget (MiB)", static_cast<float64>(budget.Budget) / (1024.0 * 1024.0));
    List<RhiMemoryPoolStats> pools;
    _device->GetMemoryStats(pools);
    uint64 used = 0;
    uint64 held = 0;
    for (const RhiMemoryPoolStats &pool : pools)
    {
        used += pool.UsedBytes;
        held += pool.BlockBytes;
    }
    MARLIN_PROFILE_PLOT("GPU Allocator Used (MiB)", static_cast<float64>(used) / (1024.0 * 1024.0));
    MARLIN_PROFILE_PLOT("GPU Allocator Held (MiB)", static_cast<float64>(held) / (1024.0 * 1024.0));
#endif
}

List<Name> RhiSubsystem::GetLayerNames() const
{
    List<Name> names;
    for (const IRhiDeviceLayer *layer : _layers)
    {
        names.Add(layer->GetName());
    }
    return names;
}

void RhiSubsystem::RequestCapture(uint32 frames)
{
    _capturesRequested = frames > 0 ? frames : 1;
}

bool RhiSubsystem::BeginCapture(StringView title)
{
    if (_captureTool == nullptr || _capturing || _backendDevice == nullptr)
    {
        return false;
    }
    _capturing = _captureTool->BeginCapture(*_backendDevice, title);
    return _capturing;
}

bool RhiSubsystem::EndCapture(String &file)
{
    if (!_capturing)
    {
        return false;
    }
    _capturing = false;
    _captureFramesLeft = 0;
    if (!_captureTool->EndCapture(*_backendDevice, file))
    {
        MARLIN_LOG(LogRhi, Warning, "{0} did not finish the capture", _captureTool->GetName());
        return false;
    }
    if (!file.IsEmpty())
    {
        MARLIN_LOG(LogRhi, Info, "Capture written to {0}", file);
    }
    return true;
}

Result<RhiSwapchain, RhiError> RhiSubsystem::CreateSwapchain(const RhiSwapchainDesc &desc)
{
    Result<RhiSwapchain, RhiError> swapchain = _device->CreateSwapchain(desc);
    if (swapchain.IsOk())
    {
        for (IRhiPresentHook *hook : _presentHooks)
        {
            hook->OnSwapchainCreated(*_backendDevice, swapchain.Value());
        }
    }
    return swapchain;
}

void RhiSubsystem::DestroySwapchain(RhiSwapchain swapchain)
{
    for (IRhiPresentHook *hook : _presentHooks)
    {
        hook->OnSwapchainDestroying(*_backendDevice, swapchain);
    }
    _device->Destroy(swapchain);
}

void RhiSubsystem::WaitForFrame(RhiSwapchain swapchain, uint64 frame)
{
    for (IRhiPresentHook *hook : _presentHooks)
    {
        if (hook->WaitForFrame(*_backendDevice, swapchain, frame))
        {
            return;
        }
    }
    _device->WaitForPresent(swapchain);
}

void RhiSubsystem::SetLatencyMarker(RhiSwapchain swapchain, RhiLatencyMarker marker, uint64 frame)
{
    for (IRhiPresentHook *hook : _presentHooks)
    {
        hook->SetMarker(*_backendDevice, swapchain, marker, frame);
    }
}

Result<void, RhiError> RhiSubsystem::Present(RhiSwapchain swapchain, uint64 frame, RhiQueueType queue)
{
    for (IRhiPresentHook *hook : _presentHooks)
    {
        hook->SetMarker(*_backendDevice, swapchain, RhiLatencyMarker::PresentStart, frame);
        hook->BeforePresent(*_backendDevice, swapchain, frame);
    }
    Result<void, RhiError> result = _device->Present(swapchain, queue);
    for (IRhiPresentHook *hook : _presentHooks)
    {
        hook->AfterPresent(*_backendDevice, swapchain, frame);
        hook->SetMarker(*_backendDevice, swapchain, RhiLatencyMarker::PresentEnd, frame);
    }
    return result;
}

void RhiSubsystem::HandleDeviceLost(bool hung)
{
    // RECOVERY-DECISION D3: a second caller (watchdog racing an API loss) parks while the first one reports.
    if (_deviceLostHandled.Exchange(true))
    {
        for (;;)
        {
            PlatformThread::Sleep(1000);
        }
    }
    WriteDeviceLostReport(hung ? "The GPU hung: a queue stopped finishing its work." : "The GPU device was lost.");
    MARLIN_FATAL(hung ? "GPU hung" : "GPU device lost");
}

void RhiSubsystem::WriteDeviceLostReport(StringView headline)
{
    StringBuilder<16384> report;
    FormatTo(report, "{0}\n", headline);
    if (_backendDevice != nullptr)
    {
        _backendDevice->WriteCrashReport(report);
        for (IGpuCrashTool *tool : _crashTools)
        {
            tool->OnDeviceLost(*_backendDevice, report);
        }
    }
    MARLIN_LOG(LogRhi, Error, "{0}", report.AsView());
    _deviceLostReported.Store(true, MemoryOrder::Release);
}

void RhiSubsystem::OnCrash(const CrashInfo &info, void *context)
{
    auto &self = *static_cast<RhiSubsystem *>(context);
    StackFrameSymbol symbol;
    (void)PlatformStackTrace::Symbolize(info.Address, symbol);
    const StringView module(symbol.Module);
    bool driver = false;
    for (const String &name : DriverModules)
    {
        driver = driver || (!module.IsEmpty() && module.EqualsIgnoreCase(name.AsView()));
    }
    if (!driver || self._backendDevice == nullptr)
    {
        return;
    }
    if (self._deviceLostHandled.Exchange(true))
    {
        // Another thread is writing the report of the lost device; the crash is reported once it is in the log.
        const uint64 deadline =
            PlatformTime::Ticks() + PlatformTime::TicksPerSecond() * DeviceLostReportWaitMilliseconds / 1000;
        while (!self._deviceLostReported.Load(MemoryOrder::Acquire) && PlatformTime::Ticks() < deadline)
        {
            PlatformThread::Sleep(50);
        }
        return;
    }
    StringBuilder<256> headline;
    FormatTo(headline, "The GPU driver crashed in {0}.", module);
    self.WriteDeviceLostReport(headline.AsView());
}

void RhiSubsystem::WatchdogMain(void *argument)
{
    static_cast<RhiSubsystem *>(argument)->RunWatchdog();
}

void RhiSubsystem::RunWatchdog()
{
    const auto timeout = static_cast<uint64>(GpuHangTimeout * static_cast<float32>(PlatformTime::TicksPerSecond()));
    RhiQueueValues last = _backendDevice->GetCompletedValues();
    const uint64 start = PlatformTime::Ticks();
    uint64 progressAt[RhiQueueTypeCount] = {start, start, start};
    while (!_watchdogStop.Wait(WatchdogPeriodMilliseconds))
    {
        const uint64 now = PlatformTime::Ticks();
        const RhiQueueValues submitted = _backendDevice->GetSubmittedValues();
        const RhiQueueValues completed = _backendDevice->GetCompletedValues();
        const bool paused = PlatformDebug::IsDebuggerAttached();
        for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
        {
            const uint64 done = completed.Values[queue];
            if (paused || done != last.Values[queue] || done >= submitted.Values[queue])
            {
                progressAt[queue] = now;
            }
            else if (now - progressAt[queue] >= timeout)
            {
                MARLIN_LOG(LogRhi, Error, "The {0} queue finished nothing for {1} s: submission {2} of {3} is stuck",
                           QueueNames[queue], GpuHangTimeout, done + 1, submitted.Values[queue]);
                HandleDeviceLost(true);
            }
        }
        last = completed;
    }
}

void RhiSubsystem::HandlePipelineCreated(Span<const RhiShader> shaders, StringView debugName) const
{
    for (IGpuCrashTool *tool : _crashTools)
    {
        tool->OnPipelineCreated(shaders, debugName);
    }
}

void RhiSubsystem::LoadPipelineCache(List<uint8> &data) const
{
    const String path = GetPipelineCachePath(_backend->GetName());
    if (Vfs::Exists(path) && !Vfs::ReadAll(path, data))
    {
        MARLIN_LOG(LogRhi, Warning, "Cannot read the pipeline cache {0}", path);
        data.Clear();
    }
    if (!data.IsEmpty())
    {
        MARLIN_LOG(LogRhi, Debug, "Loaded {0} KiB of pipeline cache from {1}", data.Count() / 1024, path);
    }
}

void RhiSubsystem::SavePipelineCache() const
{
    const List<uint8> data = _backendDevice->GetPipelineCacheData();
    if (data.IsEmpty())
    {
        return;
    }
    const String path = GetPipelineCachePath(_backend->GetName());
    if (!Vfs::WriteAll(path, StringView(reinterpret_cast<const char *>(data.Data()), data.Count())))
    {
        MARLIN_LOG(LogRhi, Warning, "Cannot write the pipeline cache {0}", path);
    }
}
} // namespace Marlin
