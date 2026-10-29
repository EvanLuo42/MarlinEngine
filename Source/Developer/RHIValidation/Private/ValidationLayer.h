#pragma once

#include "CoreTypes.h"
#include "RHI/GpuTools.h"
#include "ValidationLayer.gen.h"

namespace Marlin
{
/// Wraps the device in a ValidationDevice when the command line has -RHIValidation.
MCLASS()
class RHIVALIDATION_API RhiValidationLayer : public IRhiDeviceLayer
{
    MGENERATED_BODY()
public:
    [[nodiscard]] Name GetName() const override;
    [[nodiscard]] int32 GetPriority() const override;
    [[nodiscard]] bool IsEnabled() const override;
    [[nodiscard]] RhiDevice *Wrap(RhiDevice &inner) override;
    void Unwrap(RhiDevice *outer) override;
};
} // namespace Marlin
