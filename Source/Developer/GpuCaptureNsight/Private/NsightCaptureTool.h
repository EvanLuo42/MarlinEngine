#pragma once

#include "CoreTypes.h"
#include "RHI/GpuTools.h"
#include "String/String.h"
#include "NsightCaptureTool.gen.h"

namespace Marlin
{
/// Nsight Graphics frame captures: injects Nsight's capture into the process before the backend loads Vulkan, and
/// starts and stops captures between submissions. Captures go to /User/Captures; OpenCapture opens one in Nsight.
MCLASS(Config = "Engine.GpuCapture.Nsight")
class GPUCAPTURENSIGHT_API NsightCaptureTool : public IGpuCaptureTool
{
    MGENERATED_BODY()
public:
    /// The Nsight Graphics install to use, as NGFX finds them; empty for the newest.
    MPROPERTY()
    static String Installation;

    [[nodiscard]] Name GetName() const override;
    [[nodiscard]] bool SupportsBackend(Name backend) const override;
    [[nodiscard]] bool IsAttached() const override;
    bool Inject() override;
    void OnDeviceCreated(RhiDevice &device) override;
    void OnDeviceDestroying(RhiDevice &device) override;
    bool BeginCapture(RhiDevice &device, StringView title) override;
    bool EndCapture(RhiDevice &device, String &file) override;
    bool OpenCapture(StringView file) override;

private:
    String _installation;
    String _outputDirectory;
    bool _initialized = false;
    uint32 _capturesBefore = 0;
};
} // namespace Marlin
