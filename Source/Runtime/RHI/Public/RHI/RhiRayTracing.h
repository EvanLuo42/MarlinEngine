#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "EnumFlags.h"
#include "RHI/RhiFormat.h"
#include "RHI/RhiHandle.h"
#include "String/StringView.h"

namespace Marlin
{
enum class RhiAccelerationStructureKind : uint8
{
    BottomLevel,
    TopLevel,
};

enum class RhiBuildFlags : uint8
{
    None = 0,
    /// May be refit with an Update build.
    AllowUpdate = 1 << 0,
    AllowCompaction = 1 << 1,
    PreferFastTrace = 1 << 2,
    PreferFastBuild = 1 << 3,
    MinimizeMemory = 1 << 4,
};
MARLIN_ENUM_FLAGS(RhiBuildFlags)

enum class RhiGeometryFlags : uint8
{
    None = 0,
    /// Any-hit shaders are not run.
    Opaque = 1 << 0,
    /// Any-hit shaders run at most once per primitive.
    NoDuplicateAnyHit = 1 << 1,
};
MARLIN_ENUM_FLAGS(RhiGeometryFlags)

enum class RhiGeometryKind : uint8
{
    Triangles,
    /// Axis-aligned boxes, six floats each, for intersection shaders.
    Boxes,
};

/// Geometry data by GPU address (RhiDevice::GetBufferAddress).
struct RhiGeometry
{
    RhiGeometryKind Kind = RhiGeometryKind::Triangles;
    RhiGeometryFlags Flags = RhiGeometryFlags::Opaque;
    uint64 VertexAddress = 0;
    uint32 VertexStride = 0;
    uint32 VertexCount = 0;
    RhiFormat VertexFormat = RhiFormat::RGB32Float;
    /// Zero for no index buffer; then vertices are taken in threes.
    uint64 IndexAddress = 0;
    uint32 IndexCount = 0;
    /// 16 or 32.
    uint8 IndexBits = 32;
    /// A 3x4 row-major transform, or zero for none.
    uint64 TransformAddress = 0;
    uint64 BoxAddress = 0;
    uint32 BoxStride = 24;
    uint32 BoxCount = 0;
};

enum class RhiInstanceFlags : uint8
{
    None = 0,
    TriangleCullDisable = 1 << 0,
    TriangleFrontCounterClockwise = 1 << 1,
    ForceOpaque = 1 << 2,
    ForceNonOpaque = 1 << 3,
};
MARLIN_ENUM_FLAGS(RhiInstanceFlags)

/// One instance of a top-level structure as the GPU reads it: 64 bytes, the same on every backend.
struct RhiInstance
{
    /// A 3x4 row-major object-to-world transform.
    float32 Transform[3][4] = {};
    uint32 InstanceId : 24 = 0;
    uint32 Mask : 8 = 0xFF;
    uint32 HitGroupOffset : 24 = 0;
    uint32 Flags : 8 = 0;
    /// RhiDevice::GetAccelerationStructureAddress of the bottom-level structure.
    uint64 BottomLevelAddress = 0;
};
static_assert(sizeof(RhiInstance) == 64);

/// What a build reads: geometries for a bottom level, an array of RhiInstance for a top level.
struct RhiAccelerationStructureInputs
{
    RhiAccelerationStructureKind Kind = RhiAccelerationStructureKind::BottomLevel;
    RhiBuildFlags Flags = RhiBuildFlags::PreferFastTrace;
    Span<const RhiGeometry> Geometries;
    uint64 InstanceAddress = 0;
    uint32 InstanceCount = 0;
};

struct RhiAccelerationStructureSizes
{
    uint64 StructureSize = 0;
    uint64 BuildScratchSize = 0;
    uint64 UpdateScratchSize = 0;
};

/// Lives in a buffer with the AccelerationStructure usage, at an offset aligned to 256.
struct RhiAccelerationStructureDesc
{
    RhiAccelerationStructureKind Kind = RhiAccelerationStructureKind::BottomLevel;
    RhiBuffer Buffer;
    uint64 Offset = 0;
    uint64 Size = 0;
    StringView DebugName;
};

struct RhiAccelerationStructureBuild
{
    RhiAccelerationStructure Destination;
    RhiAccelerationStructureInputs Inputs;
    /// Refits Source into Destination, which may be the same; Source was built with AllowUpdate.
    bool Update = false;
    RhiAccelerationStructure Source;
    uint64 ScratchAddress = 0;
};

enum class RhiAccelerationStructureCopyMode : uint8
{
    Clone,
    /// Destination must be at least the compacted size the source reported.
    Compact,
    /// Into a buffer, in a form only the same device and driver can read back.
    Serialize,
    Deserialize,
};

enum class RhiAccelerationStructureProperty : uint8
{
    CompactedSize,
    SerializedSize,
};

/// GPU addresses of the shader binding tables, which hold shader group handles followed by records.
struct RhiShaderTable
{
    uint64 Address = 0;
    uint64 Size = 0;
    uint64 Stride = 0;
};

struct RhiDispatchRaysDesc
{
    RhiShaderTable RayGeneration;
    RhiShaderTable Miss;
    RhiShaderTable HitGroups;
    RhiShaderTable Callable;
    uint32 Width = 1;
    uint32 Height = 1;
    uint32 Depth = 1;
};
} // namespace Marlin
