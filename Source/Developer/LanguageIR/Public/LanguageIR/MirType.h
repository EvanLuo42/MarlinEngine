#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Memory/Arena.h"
#include "String/StringView.h"

namespace Marlin
{
class StringWriter;

enum class MirTypeKind : uint8
{
    Void,
    Bool,
    Int,
    Float,
    Vector,
    Matrix,
    Array,
    RuntimeArray,
    Struct,
    Pointer,
    Function,
    Texture,
    Buffer,
    Sampler,
    AccelerationStructure,
    RayQuery,
    HitObject,
    CoopVector,
    CoopMatrix,
    /// A reference to an object on the script GC heap (MarlinScript.md); GetElement gives the contents of the
    /// object, such as the layout of a class or the array type of an array.
    Reference,
};

enum class MirAddressSpace : uint8
{
    Function,
    Private,
    GroupShared,
    Buffer,
    Uniform,
    Input,
    Output,
    RayPayload,
    IncomingRayPayload,
    HitAttribute,
    CallableData,
    IncomingCallableData,
    ShaderRecord,
    TaskPayload,
    /// A `[Specialization]` constant: read-only, its value fixed when the pipeline is created (§9.4).
    Specialization,
    /// Inside an object on the script GC heap: a field or an array element, as FieldAddress and ElementAddress
    /// point at.
    Managed,
    /// A script static field.
    Static,
};

enum class MirTextureDim : uint8
{
    Texture1D,
    Texture2D,
    Texture3D,
    Cube,
};

enum class MirBufferKind : uint8
{
    Structured,
    ByteAddress,
};

/// The storage format of a writable texture; Unknown for read-only textures and unformatted access.
enum class MirImageFormat : uint8
{
    Unknown,
    Rgba32f,
    Rgba16f,
    Rg32f,
    Rg16f,
    R11g11b10f,
    R32f,
    R16f,
    Rgba16Unorm,
    Rgb10a2Unorm,
    Rgba8Unorm,
    Rg16Unorm,
    Rg8Unorm,
    R16Unorm,
    R8Unorm,
    Rgba16Snorm,
    Rgba8Snorm,
    Rg16Snorm,
    Rg8Snorm,
    R16Snorm,
    R8Snorm,
    Rgba32i,
    Rgba16i,
    Rgba8i,
    Rg32i,
    Rg16i,
    Rg8i,
    R32i,
    R16i,
    R8i,
    R64i,
    Rgba32u,
    Rgba16u,
    Rgba8u,
    Rg32u,
    Rg16u,
    Rg8u,
    R32u,
    R16u,
    R8u,
    R64u,
};

enum class MirMatrixUse : uint8
{
    A,
    B,
    Accumulator,
};

class MirType;

struct MirStructField
{
    StringView Name;
    const MirType *Type = nullptr;
    uint32 Offset = 0;
    /// A `[NoDiff]` field (§9.12): its derivative is always zero, so differentiation does not flow through it.
    bool NoDiff = false;
};

/// A MIR type. Types are unique within a MirTypeContext and compare by pointer, except structs, which are
/// nominal. Types that can live in memory have a size and alignment by the layout rules of the language (§10).
class LANGUAGEIR_API MirType
{
public:
    [[nodiscard]] MirTypeKind GetKind() const
    {
        return _kind;
    }

    [[nodiscard]] bool Is(MirTypeKind kind) const
    {
        return _kind == kind;
    }

    /// Bits of an Int or Float, or of the elements of a vector, matrix or cooperative type.
    [[nodiscard]] uint32 GetWidth() const
    {
        return _width;
    }

    [[nodiscard]] bool IsSigned() const
    {
        return _signed;
    }

    /// The element of a vector, matrix (its row vector), array, cooperative type or texture; the pointee of a
    /// pointer; the return type of a function.
    [[nodiscard]] const MirType *GetElement() const
    {
        return _element;
    }

    /// Components of a vector or cooperative vector, elements of an array, columns of a matrix or cooperative
    /// matrix.
    [[nodiscard]] uint32 GetCount() const
    {
        return _count;
    }

    /// Rows of a matrix or cooperative matrix.
    [[nodiscard]] uint32 GetRows() const
    {
        return _rows;
    }

    [[nodiscard]] MirAddressSpace GetAddressSpace() const
    {
        return _space;
    }

    [[nodiscard]] MirTextureDim GetTextureDim() const
    {
        return _dim;
    }

    [[nodiscard]] bool IsArrayed() const
    {
        return _arrayed;
    }

    [[nodiscard]] bool IsMultisampled() const
    {
        return _multisampled;
    }

    /// Whether a texture or buffer is writable.
    [[nodiscard]] bool IsWritable() const
    {
        return _writable;
    }

    [[nodiscard]] MirImageFormat GetImageFormat() const
    {
        return _format;
    }

    [[nodiscard]] MirBufferKind GetBufferKind() const
    {
        return _bufferKind;
    }

    /// Whether a sampler compares.
    [[nodiscard]] bool IsComparison() const
    {
        return _signed;
    }

    [[nodiscard]] MirMatrixUse GetMatrixUse() const
    {
        return _matrixUse;
    }

    [[nodiscard]] StringView GetName() const
    {
        return _name;
    }

    [[nodiscard]] Span<const MirStructField> GetFields() const
    {
        return _fields;
    }

    [[nodiscard]] Span<const MirType *const> GetParameters() const
    {
        return _parameters;
    }

    /// Bytes in memory; 0 for types that cannot be stored.
    [[nodiscard]] uint32 GetSize() const
    {
        return _size;
    }

    [[nodiscard]] uint32 GetAlignment() const
    {
        return _alignment;
    }

    /// Distance between the elements of an array or the rows of a matrix.
    [[nodiscard]] uint32 GetStride() const
    {
        return _stride;
    }

    [[nodiscard]] bool IsScalar() const
    {
        return _kind == MirTypeKind::Bool || _kind == MirTypeKind::Int || _kind == MirTypeKind::Float;
    }

    [[nodiscard]] bool IsNumeric() const
    {
        return _kind == MirTypeKind::Int || _kind == MirTypeKind::Float;
    }

    /// The scalar of a scalar, vector or matrix.
    [[nodiscard]] const MirType *GetScalar() const;

    /// Whether this type is a handle of the bindless heaps, stored as a 32-bit index.
    [[nodiscard]] bool IsHandle() const
    {
        return _kind == MirTypeKind::Texture || _kind == MirTypeKind::Buffer || _kind == MirTypeKind::Sampler ||
               _kind == MirTypeKind::AccelerationStructure;
    }

    void Print(StringWriter &output) const;

private:
    friend class MirTypeContext;

    MirTypeKind _kind = MirTypeKind::Void;
    uint8 _width = 0;
    bool _signed = false;
    bool _arrayed = false;
    bool _multisampled = false;
    bool _writable = false;
    MirAddressSpace _space = MirAddressSpace::Function;
    MirTextureDim _dim = MirTextureDim::Texture2D;
    MirImageFormat _format = MirImageFormat::Unknown;
    MirBufferKind _bufferKind = MirBufferKind::Structured;
    MirMatrixUse _matrixUse = MirMatrixUse::A;
    uint32 _count = 0;
    uint32 _rows = 0;
    uint32 _size = 0;
    uint32 _alignment = 0;
    uint32 _stride = 0;
    const MirType *_element = nullptr;
    StringView _name;
    Span<const MirStructField> _fields;
    Span<const MirType *const> _parameters;
};

/// Creates and uniques the types of a module; the types live as long as it.
class LANGUAGEIR_API MirTypeContext
{
public:
    explicit MirTypeContext(Arena &arena);
    MARLIN_NONCOPYABLE(MirTypeContext);

    [[nodiscard]] const MirType *GetVoid();
    [[nodiscard]] const MirType *GetBool();
    [[nodiscard]] const MirType *GetInt(uint32 width, bool isSigned);
    [[nodiscard]] const MirType *GetFloat(uint32 width);
    [[nodiscard]] const MirType *GetVector(const MirType *scalar, uint32 count);
    /// A matrix of *rows* rows, each a vector of *columns* components.
    [[nodiscard]] const MirType *GetMatrix(const MirType *scalar, uint32 rows, uint32 columns);
    [[nodiscard]] const MirType *GetArray(const MirType *element, uint32 count);
    [[nodiscard]] const MirType *GetRuntimeArray(const MirType *element);
    [[nodiscard]] const MirType *GetPointer(const MirType *pointee, MirAddressSpace space);
    /// A reference to a script object whose contents are *target*.
    [[nodiscard]] const MirType *GetReference(const MirType *target);
    [[nodiscard]] const MirType *GetFunction(const MirType *result, Span<const MirType *const> parameters);
    [[nodiscard]] const MirType *GetTexture(const MirType *sampled, MirTextureDim dim, bool arrayed, bool multisampled,
                                            bool writable, MirImageFormat format);
    /// A structured buffer of *element*, or a byte address buffer when *element* is null.
    [[nodiscard]] const MirType *GetBuffer(const MirType *element, bool writable);
    [[nodiscard]] const MirType *GetSampler(bool comparison);
    [[nodiscard]] const MirType *GetAccelerationStructure();
    /// A ray query whose traversals all use *flags* (RayFlags) besides the ones given when it starts; GetCount
    /// returns them.
    [[nodiscard]] const MirType *GetRayQuery(uint32 flags);
    [[nodiscard]] const MirType *GetHitObject();
    [[nodiscard]] const MirType *GetCoopVector(const MirType *scalar, uint32 count);
    [[nodiscard]] const MirType *GetCoopMatrix(const MirType *scalar, uint32 rows, uint32 columns, MirMatrixUse use);

    /// A new struct; *fields* with an Offset of ~0u are laid out by the language rules after the previous field.
    /// *size* of 0 rounds the end up to the alignment. Null if an explicit offset breaks the alignment rules.
    [[nodiscard]] const MirType *CreateStruct(StringView name, Span<const MirStructField> fields, uint32 size = 0);

    static constexpr uint32 AutoOffset = ~0u;

private:
    struct Key
    {
        MirTypeKind Kind = MirTypeKind::Void;
        uint8 Width = 0;
        uint8 Flags = 0;
        uint8 Extra = 0;
        uint32 Count = 0;
        uint32 Rows = 0;
        const MirType *Element = nullptr;
        const MirType *const *Parameters = nullptr;
        uint32 ParameterCount = 0;

        [[nodiscard]] uint64 GetHash() const;
        bool operator==(const Key &other) const;
    };

    const MirType *Intern(const Key &key, MirType &prototype);
    StringView CopyString(StringView text);

    Arena *_arena;
    HashMap<Key, const MirType *> _types;
};
} // namespace Marlin
