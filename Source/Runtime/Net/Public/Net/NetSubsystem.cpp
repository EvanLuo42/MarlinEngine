#include "Net/NetSubsystem.h"

#include "Config/Config.h"
#include "Diagnostics/Log.h"

namespace Marlin
{
uint32 NetSubsystem::Threads = 2;
String NetSubsystem::Transport;

void NetSubsystem::Initialize(SubsystemContext & /*context*/)
{
    if (!_loop.Start(Threads))
    {
        MARLIN_LOG(LogCore, Error, "The net loop did not start");
    }
    List<String> disabled;
    (void)Config::TryGet("Engine.Extensions.Disabled", disabled);
    ExtensionContext extensions;
    extensions.Disabled = disabled.AsSpan();
    const StringView name = Transport.IsEmpty() ? StringView("Udp") : Transport.AsView();
    _transports.Build(extensions, name);
    if (_transports.GetDefault() == nullptr && _transports.Count() != 0)
    {
        MARLIN_LOG(LogCore, Warning, "Engine.Net.Transport {0} is not linked", name);
    }
}

void NetSubsystem::Deinitialize()
{
    _loop.Stop();
}
} // namespace Marlin
