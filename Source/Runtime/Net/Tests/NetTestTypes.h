#pragma once

#include "Concurrency/Atomic.h"
#include "CoreTypes.h"
#include "Net/NetTransport.h"
#include "NetTestTypes.gen.h"

namespace Marlin::Tests
{
/// A transport the way another module would add one: messages framed over a TCP NetStream, which provides every delivery.
MCLASS()
class TestStreamTransport : public NetTransport
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "Test";
    }
    [[nodiscard]] bool Supports(NetDelivery delivery) const override;
    [[nodiscard]] UniquePtr<NetEndpoint> Listen(NetLoop &loop, const NetListenDesc &desc,
                                                NetSessionAcceptHandler onAccept) override;
    [[nodiscard]] SharedPtr<NetSession> Connect(NetLoop &loop, StringView host, uint16 port, const NetSessionDesc &desc,
                                                uint32 timeout) override;
};

/// A transport that only promises reliable delivery, to see unsupported channels refused.
MCLASS()
class TestReliableOnlyTransport final : public TestStreamTransport
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "ReliableOnly";
    }
    [[nodiscard]] bool Supports(NetDelivery delivery) const override
    {
        return delivery == NetDelivery::Reliable || delivery == NetDelivery::ReliableOrdered;
    }
};

/// A wrapper that takes over the Test transport and counts the sessions it connects, as a wrapper would.
MCLASS(Replaces = "TestStreamTransport")
class TestCountingTransport final : public NetTransport
{
    MGENERATED_BODY()
public:
    MFUNCTION()
    static bool ShouldCreate(const ExtensionContext &context);

    [[nodiscard]] StringView GetName() const override
    {
        return "Test";
    }
    [[nodiscard]] bool Supports(NetDelivery delivery) const override;
    [[nodiscard]] UniquePtr<NetEndpoint> Listen(NetLoop &loop, const NetListenDesc &desc,
                                                NetSessionAcceptHandler onAccept) override;
    [[nodiscard]] SharedPtr<NetSession> Connect(NetLoop &loop, StringView host, uint16 port, const NetSessionDesc &desc,
                                                uint32 timeout) override;

    /// Set by the test that wants it; also where it finds what it wraps.
    static bool Enabled;
    static const NetTransportSet *Owner;
    static Atomic<uint32> Connects;
};
} // namespace Marlin::Tests
