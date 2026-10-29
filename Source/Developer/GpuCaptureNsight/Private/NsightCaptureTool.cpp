#include "NsightCaptureTool.h"

#include "Diagnostics/Log.h"
#include "HAL/MicrosoftHeaders.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "RHI/RhiLog.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

#include <NGFX_GraphicsCapture_Vulkan.h>

namespace Marlin
{
String NsightCaptureTool::Installation;

namespace
{
constexpr int32 CaptureFileWaitMilliseconds = 60000;

String ToUtf8(const wchar_t *text)
{
    char buffer[1024];
    const usize length = Microsoft::WideToUtf8(text, buffer, sizeof(buffer));
    String result = length < sizeof(buffer) ? String(StringView(buffer, length)) : String();
    result.Replace('\\', '/');
    return result;
}

const char *ResultName(NGFX_Result result)
{
    switch (result)
    {
    case NGFX_Result_Success:
        return "Success";
    case NGFX_Result_NotImplemented:
        return "NotImplemented";
    case NGFX_Result_LibNotFound:
        return "LibNotFound";
    case NGFX_Result_InvalidLib:
        return "InvalidLib";
    case NGFX_Result_DifferentActivityInjected:
        return "DifferentActivityInjected";
    case NGFX_Result_InvalidParameter:
        return "InvalidParameter";
    case NGFX_Result_InvalidState:
        return "InvalidState";
    case NGFX_Result_Timeout:
        return "Timeout";
    case NGFX_Result_InsufficientBuffer:
        return "InsufficientBuffer";
    default:
        return "UnspecifiedError";
    }
}

uint32 GetCaptureFileCount()
{
    NGFX_ArtifactFileCount_Params params{NGFX_ArtifactFileCount_Params_VER, 0};
    return NGFX_GraphicsCapture_GetCaptureFileCount(&params) == NGFX_Result_Success ? params.count : 0;
}
} // namespace

Name NsightCaptureTool::GetName() const
{
    return Name("Nsight");
}

bool NsightCaptureTool::SupportsBackend(Name backend) const
{
    return backend.ToView() == StringView("Vulkan");
}

bool NsightCaptureTool::IsAttached() const
{
    NGFX_ActivityType activity = NGFX_ActivityType_COUNT;
    return NGFX_GetInjectedActivity(&activity) == NGFX_Result_Success &&
           activity == NGFX_ActivityType_GraphicsCapture;
}

bool NsightCaptureTool::Inject()
{
    NGFX_SetLibraryLoadFn(&NGFX_LoadLib_NoVerification);
    NGFX_InstallationInfo installations[16] = {};
    uint32 count = 0;
    NGFX_EnumerateInstallations(installations, 16, &count);
    String wanted = Installation;
    wanted.Replace('\\', '/');
    const NGFX_InstallationInfo *chosen = nullptr;
    for (uint32 i = 0; i < count && chosen == nullptr; ++i)
    {
        const String path = ToUtf8(installations[i].installationPath);
        if (wanted.IsEmpty() || path.AsView().Contains(wanted.AsView()))
        {
            chosen = &installations[i];
            _installation = path;
            MARLIN_LOG(LogRhi, Info, "Nsight Graphics {0}.{1}.{2} at {3}", installations[i].versionMajor,
                       installations[i].versionMinor, installations[i].versionPatch, path);
        }
    }
    if (chosen == nullptr)
    {
        NGFX_FreeInstallations(installations, count);
        MARLIN_LOG(LogRhi, Warning, "No Nsight Graphics install{0}{1} was found", Installation.IsEmpty() ? "" : " under ",
                   Installation);
        return false;
    }

    StringBuilder<512> output;
    Vfs::CreateDirectory("/User/Captures");
    if (Vfs::GetPhysicalPath("/User/Captures", output))
    {
        _outputDirectory = String(output.AsView());
    }
    NGFX_GraphicsCapture_InjectionSettings settings{};
    settings.version = NGFX_GraphicsCapture_InjectionSettings_VER;
    NGFX_GraphicsCapture_InjectionSettings_SetDefaults(&settings);
    settings.outputDir = _outputDirectory.IsEmpty() ? nullptr : _outputDirectory.CStr();
    settings.captureDefaultHotkey = false;
    settings.noHUD = true;
    NGFX_GraphicsCapture_Inject_Vulkan_Params params{NGFX_GraphicsCapture_Inject_Vulkan_Params_VER,
                                                     chosen->installationPath, &settings};
    const NGFX_Result result = NGFX_GraphicsCapture_Inject_Vulkan(&params);
    NGFX_FreeInstallations(installations, count);
    if (result != NGFX_Result_Success)
    {
        MARLIN_LOG(LogRhi, Warning, "Nsight Graphics did not inject ({0})", ResultName(result));
        return false;
    }
    return true;
}

void NsightCaptureTool::OnDeviceCreated(RhiDevice & /*device*/)
{
    NGFX_GraphicsCapture_InitializeActivity_Vulkan_Params params{
        NGFX_GraphicsCapture_InitializeActivity_Vulkan_Params_VER};
    const NGFX_Result result = NGFX_GraphicsCapture_InitializeActivity_Vulkan(&params);
    _initialized = result == NGFX_Result_Success;
    if (!_initialized)
    {
        MARLIN_LOG(LogRhi, Warning, "Nsight Graphics capture did not initialize ({0})", ResultName(result));
    }
}

void NsightCaptureTool::OnDeviceDestroying(RhiDevice & /*device*/)
{
    _initialized = false;
}

bool NsightCaptureTool::BeginCapture(RhiDevice & /*device*/, StringView /*title*/)
{
    if (!_initialized)
    {
        return false;
    }
    _capturesBefore = GetCaptureFileCount();
    NGFX_GraphicsCapture_StartCapture_Vulkan_Params params{NGFX_GraphicsCapture_StartCapture_Vulkan_Params_VER};
    const NGFX_Result result = NGFX_GraphicsCapture_StartCapture_Vulkan(&params);
    if (result != NGFX_Result_Success)
    {
        MARLIN_LOG(LogRhi, Warning, "Nsight Graphics did not start a capture ({0})", ResultName(result));
        return false;
    }
    return true;
}

bool NsightCaptureTool::EndCapture(RhiDevice & /*device*/, String &file)
{
    NGFX_GraphicsCapture_StopCapture_Vulkan_Params params{NGFX_GraphicsCapture_StopCapture_Vulkan_Params_VER};
    NGFX_Result result = NGFX_GraphicsCapture_StopCapture_Vulkan(&params);
    if (result != NGFX_Result_Success)
    {
        MARLIN_LOG(LogRhi, Warning, "Nsight Graphics did not stop the capture ({0})", ResultName(result));
        return false;
    }
    wchar_t path[1024] = {};
    NGFX_WaitForArtifactFilePath_Params wait{};
    wait.version = NGFX_WaitForArtifactFilePath_Params_VER;
    wait.artifactIndex = _capturesBefore;
    wait.timeoutMs = CaptureFileWaitMilliseconds;
    wait.filePath = path;
    wait.filePathCapacity = 1024;
    result = NGFX_GraphicsCapture_WaitForCaptureFilePath(&wait);
    if (result != NGFX_Result_Success)
    {
        MARLIN_LOG(LogRhi, Warning, "Nsight Graphics wrote no capture file ({0})", ResultName(result));
        return false;
    }
    file = ToUtf8(path);
    return true;
}

bool NsightCaptureTool::OpenCapture(StringView file)
{
    for (const StringView relative : {StringView("ngfx-ui.exe"), StringView("host/windows-desktop-nomad-x64/ngfx-ui.exe")})
    {
        StringBuilder<512> executable;
        FormatTo(executable, "{0}/{1}", _installation, relative);
        if (!PlatformFile::FileExists(executable.CStr()))
        {
            continue;
        }
        StringBuilder<1024> arguments;
        FormatTo(arguments, "\"{0}\"", file);
        ProcessDesc desc;
        desc.Executable = executable.CStr();
        desc.Arguments = arguments.CStr();
        desc.Flags = ProcessFlags::Detached;
        ProcessHandle process;
        return PlatformProcess::Create(desc, process);
    }
    return false;
}
} // namespace Marlin
