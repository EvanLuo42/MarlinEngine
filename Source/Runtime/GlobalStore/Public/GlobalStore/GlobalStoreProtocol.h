#pragma once

#include "CoreTypes.h"
#include "Hash/Hash128.h"

namespace Marlin
{
/// The GlobalStore wire format: fixed-size little-endian headers over TCP, requests pipelined and answered in
/// order on each connection. Keys are 128-bit hashes; values are opaque bytes.
inline constexpr uint32 GlobalStoreRequestMagic = 0x3153474Du;
inline constexpr uint32 GlobalStoreResponseMagic = 0x3253474Du;
inline constexpr uint16 GlobalStoreDefaultPort = 7480;
inline constexpr uint64 GlobalStoreWholeValue = ~uint64{0};
/// Values are at most this large; bulk data larger than that is stored in parts by the caller.
inline constexpr uint64 GlobalStoreMaxValue = uint64{2} << 30;

enum class GlobalStoreOp : uint8
{
    /// Reads Size bytes from Offset; Size GlobalStoreWholeValue reads to the end.
    Get = 1,
    /// Stores Size bytes that follow the header. Storing a key that exists is a no-op: values never change.
    Put = 2,
    /// Answers whether the key exists, and its size.
    Has = 3,
};

enum class GlobalStoreStatus : uint8
{
    Ok = 0,
    Missing = 1,
    Error = 2,
};

struct GlobalStoreRequest
{
    uint32 Magic = GlobalStoreRequestMagic;
    GlobalStoreOp Op = GlobalStoreOp::Get;
    uint8 Padding[3] = {};
    uint32 Id = 0;
    uint32 Reserved = 0;
    Hash128 Key;
    uint64 Offset = 0;
    uint64 Size = 0;
};

struct GlobalStoreResponse
{
    uint32 Magic = GlobalStoreResponseMagic;
    GlobalStoreStatus Status = GlobalStoreStatus::Ok;
    uint8 Padding[3] = {};
    uint32 Id = 0;
    uint32 Reserved = 0;
    /// The size of the whole value.
    uint64 TotalSize = 0;
    /// The bytes that follow: the part a Get asked for, zero otherwise.
    uint64 Size = 0;
};

/// Key domains (Documentation/Architecture/06 §3.3): every kind of value hashes its key text with its own seed, so
/// keys of different kinds never collide.
namespace GlobalStoreDomain
{
/// A ConvertedResult, under its result key.
inline constexpr uint64 ConvertedResult = 0x4D52534C54524553ull;
/// A dependency record: what a conversion read, under its input key; also the records that tie derived results
/// to the conversion that made them.
inline constexpr uint64 DependencyRecord = 0x4D44455052454353ull;
} // namespace GlobalStoreDomain

static_assert(sizeof(GlobalStoreRequest) == 48);
static_assert(sizeof(GlobalStoreResponse) == 32);
} // namespace Marlin
