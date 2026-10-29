#include "RHI/RhiDevice.h"

namespace Marlin
{
const char *GetErrorName(RhiError error)
{
    switch (error)
    {
    case RhiError::OutOfDeviceMemory:
        return "OutOfDeviceMemory";
    case RhiError::OutOfHostMemory:
        return "OutOfHostMemory";
    case RhiError::PipelineCompilationFailed:
        return "PipelineCompilationFailed";
    case RhiError::Unsupported:
        return "Unsupported";
    case RhiError::SurfaceLost:
        return "SurfaceLost";
    case RhiError::DeviceLost:
        return "DeviceLost";
    }
    return "Unknown";
}

bool RhiDevice::HasFeature(Name feature) const
{
    for (const Name supported : GetFeatures())
    {
        if (supported == feature)
        {
            return true;
        }
    }
    return false;
}
} // namespace Marlin
