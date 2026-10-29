#pragma once

#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "Net/NetLoop.h"
#include "Net/NetTransport.h"
#include "String/String.h"
#include "Net/NetSubsystem.gen.h"

namespace Marlin
{
/// Runs the process's NetLoop and holds its transports for the life of the engine (Documentation/Architecture/14
/// §3, §5.1).
MCLASS(Config = "Engine.Net")
class NET_API NetSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// IO threads.
    MPROPERTY()
    static uint32 Threads;
    /// The name of the transport sessions use unless they ask for another.
    MPROPERTY()
    static String Transport;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;

    [[nodiscard]] NetLoop &GetLoop()
    {
        return _loop;
    }

    [[nodiscard]] const NetTransportSet &GetTransports() const
    {
        return _transports;
    }

private:
    NetLoop _loop;
    NetTransportSet _transports;
};
} // namespace Marlin
