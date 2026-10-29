#include "Net/NetTransport.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/AssertFormat.h"
#include "String/Ascii.h"
#include "String/Number.h"

namespace Marlin
{
namespace
{
String Key(StringView name)
{
    String key(name);
    for (usize i = 0; i < key.Length(); ++i)
    {
        key.Data()[i] = Ascii::ToLower(key.Data()[i]);
    }
    return key;
}

int64 PriorityOf(const TypeInfo &type)
{
    int64 priority = 0;
    (void)TryParse(type.GetMetadata().GetOr("Priority", "0"), priority);
    return priority;
}
} // namespace

NetSession::NetSession(StringView description) : _description(description)
{
}

void NetSession::Start(NetSessionHandlers handlers)
{
    {
        const ScopeLock lock(_lock);
        _handlers = Move(handlers);
    }
    OnStart();
}

void NetSession::Deliver(uint32 channel, Span<const uint8> message)
{
    if (_handlers.OnMessage && IsOpen())
    {
        _handlers.OnMessage(*this, channel, message);
    }
}

void NetSession::Finish()
{
    NetSessionHandlers handlers;
    {
        const ScopeLock lock(_lock);
        if (_finished.Load(MemoryOrder::Acquire))
        {
            return;
        }
        _finished.Store(true, MemoryOrder::Release);
        handlers = Move(_handlers);
        _handlers = {};
    }
    if (handlers.OnClose)
    {
        handlers.OnClose(*this);
    }
}

void NetTransportSet::Build(const ExtensionContext &context, StringView defaultName)
{
    _transports.Build(context);
    _byName.Clear();
    HashMap<String, int64> priorities;
    for (NetTransport *transport : _transports)
    {
        const String key = Key(transport->GetName());
        const int64 priority = PriorityOf(transport->GetType());
        if (NetTransport *const *existing = _byName.Find(key))
        {
            if (*priorities.Find(key) == priority)
            {
                MARLIN_FATALF("{0} and {1} are both the {2} transport at priority {3}; give one a higher Priority, "
                              "Replaces the other, or disable one in Engine.Extensions",
                              (*existing)->GetType().GetName(), transport->GetType().GetName(), transport->GetName(),
                              priority);
            }
            continue;
        }
        _byName.FindOrAdd(key) = transport;
        priorities.FindOrAdd(key) = priority;
    }
    _default = Find(defaultName);
}

NetTransport *NetTransportSet::Find(StringView name) const
{
    NetTransport *const *transport = _byName.Find(Key(name));
    return transport != nullptr ? *transport : nullptr;
}
} // namespace Marlin
