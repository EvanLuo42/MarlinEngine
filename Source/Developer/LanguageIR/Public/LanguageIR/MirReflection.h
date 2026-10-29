#pragma once

#include "Containers/List.h"
#include "LanguageIR/MirModule.h"
#include "String/String.h"

namespace Marlin
{
/// A member of a block the engine fills, such as the [Uniform] parameter block or a shader record.
struct MirReflectedField
{
    StringView Name;
    const MirType *Type = nullptr;
    uint32 Offset = 0;
    uint32 Size = 0;
};

/// A stage input or output: a system value, or a varying at a location.
struct MirReflectedVarying
{
    StringView Name;
    const MirType *Type = nullptr;
    MirBuiltin Builtin = MirBuiltin::None;
    uint32 Location = ~0u;
    bool PerPrimitive = false;
};

struct MirReflectedSpecialization
{
    StringView Name;
    uint32 Id = 0;
    const MirType *Type = nullptr;
    /// The default value's bits, as MirConstant stores them.
    uint64 DefaultBits = 0;
};

/// A type ray tracing stages connect by (§9.8), with its name and a hash of its layout, so the engine can check
/// that the stages of one pipeline agree.
struct MirReflectedStageType
{
    StringView Name;
    uint64 LayoutHash = 0;
    uint32 Size = 0;
};

struct MirReflectedCoopMatrix
{
    const MirType *Scalar = nullptr;
    uint32 Rows = 0;
    uint32 Columns = 0;
    MirMatrixUse Use = MirMatrixUse::A;
};

/// What the engine needs to know about one compiled entry point (MarlinLanguage.md §12): its stage and group size,
/// the blocks the engine fills, its inputs and outputs, the memory it needs, and the features and formats it uses.
/// Names point into the module's arena.
struct MirReflection
{
    StringView Name;
    MirStage Stage = MirStage::Compute;
    uint32 GroupSize[3] = {1, 1, 1};
    uint32 MaxVertices = 0;
    uint32 MaxPrimitives = 0;
    MirMeshTopology Topology = MirMeshTopology::Triangle;
    /// The optional features (§9.7) the entry point needs.
    List<StringView> Features;
    /// The [Uniform] parameter block (§9.3).
    List<MirReflectedField> Arguments;
    uint32 ArgumentsSize = 0;
    /// The block is in upload memory and the arguments passed directly are a Ptr to it.
    bool ArgumentsIndirect = false;
    List<MirReflectedField> ShaderRecord;
    List<MirReflectedVarying> Inputs;
    List<MirReflectedVarying> Outputs;
    uint32 GroupSharedBytes = 0;
    uint32 TaskPayloadBytes = 0;
    List<MirReflectedSpecialization> Specializations;
    /// The formats of `Debug.Print`, by the index a DebugPrint names (§9.11).
    List<StringView> DebugFormats;
    /// Where among the arguments passed directly the debug output buffer's address goes (§9.11); ~0u if the entry
    /// point prints nothing.
    uint32 DebugBufferOffset = ~0u;
    MirReflectedStageType RayPayload;
    MirReflectedStageType HitAttributes;
    MirReflectedStageType CallableData;
    /// Whether the entry point launches rays (TraceRay or HitObject.TraceRay), so the pipeline's recursion depth
    /// must allow it.
    bool TracesRays = false;
    List<MirReflectedCoopMatrix> CoopMatrices;
};

/// Fills *reflection* for *entry* of *module*, which has been through the shader pipeline and legalization.
LANGUAGEIR_API void ReflectMirEntryPoint(const MirModule &module, const MirEntryPoint &entry, MirReflection &reflection);

/// A readable dump of *reflection*, for tests and tools.
LANGUAGEIR_API String PrintMirReflection(const MirReflection &reflection);
} // namespace Marlin
