#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "LanguageIR/MirType.h"
#include "Memory/Arena.h"
#include "String/StringView.h"

namespace Marlin
{
class StringWriter;
struct SemaStructInstance;
struct SemaSymbol;

enum class SemaTypeKind : uint8
{
    Error,
    Void,
    Bool,
    Int,
    Float,
    Vector,
    Matrix,
    Array,
    Struct,
    Enum,
    Interface,
    Texture,
    Buffer,
    Sampler,
    AccelerationStructure,
    RayQuery,
    HitObject,
    CoopVector,
    CoopMatrix,
    MatrixRef,
    VectorRef,
    /// The vertices, indices or primitives a mesh shader writes (§9.2).
    MeshOutput,
    Feature,
    /// Compile-time text: `nameof`, attribute arguments, feature names. Never exists at run time.
    String,
    /// The type of `null` and of `default` before the context gives it one.
    Null,
    /// A script class: a reference to an object on the GC heap. `object` is the class all others derive from.
    Class,
    /// A script delegate: a reference to an immutable list of calls.
    Delegate,
    /// A script array `T[]` of run-time length on the GC heap.
    GcArray,
    /// `T?`: a reference that may be null, or a value type with `HasValue`.
    Nullable,
    /// A lambda or a method group before the delegate type it converts to is known.
    FunctionGroup,
};

enum class SemaMeshOutput : uint8
{
    Vertices,
    Indices,
    Primitives,
};

/// How a cooperative-vector matrix is laid out in its buffer (§9.9).
enum class SemaMatrixLayout : uint8
{
    RowMajor,
    ColumnMajor,
    InferencingOptimal,
    TrainingOptimal,
};

/// A type of the language. Types are unique within a SemaTypeTable and compare by pointer.
class SemaType
{
public:
    [[nodiscard]] SemaTypeKind GetKind() const
    {
        return _kind;
    }

    [[nodiscard]] bool Is(SemaTypeKind kind) const
    {
        return _kind == kind;
    }

    [[nodiscard]] bool IsError() const
    {
        return _kind == SemaTypeKind::Error;
    }

    [[nodiscard]] uint32 GetWidth() const
    {
        return _width;
    }

    [[nodiscard]] bool IsSigned() const
    {
        return _signed;
    }

    /// The element of a vector, matrix (its scalar), array, cooperative type, matrix or vector reference, the
    /// texel type of a texture or the element of a structured buffer; the underlying integer of an enum; the
    /// element of a script array; the type a `T?` wraps.
    [[nodiscard]] const SemaType *GetElement() const
    {
        return _element;
    }

    /// Components of a vector, columns of a matrix, elements of an array or cooperative vector.
    [[nodiscard]] uint32 GetCount() const
    {
        return _count;
    }

    [[nodiscard]] uint32 GetRows() const
    {
        return _rows;
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

    [[nodiscard]] bool IsWritable() const
    {
        return _writable;
    }

    [[nodiscard]] MirImageFormat GetImageFormat() const
    {
        return _format;
    }

    [[nodiscard]] bool IsComparison() const
    {
        return _signed;
    }

    [[nodiscard]] MirMatrixUse GetMatrixUse() const
    {
        return _matrixUse;
    }

    [[nodiscard]] SemaMatrixLayout GetMatrixLayout() const
    {
        return _layout;
    }

    [[nodiscard]] SemaMeshOutput GetMeshOutput() const
    {
        return _meshOutput;
    }

    [[nodiscard]] SemaStructInstance *GetStruct() const
    {
        return _struct;
    }

    /// The declaration of an enum or interface type.
    [[nodiscard]] SemaSymbol *GetSymbol() const
    {
        return _symbol;
    }

    [[nodiscard]] bool IsScalar() const
    {
        return _kind == SemaTypeKind::Bool || _kind == SemaTypeKind::Int || _kind == SemaTypeKind::Float;
    }

    [[nodiscard]] bool IsNumeric() const
    {
        return _kind == SemaTypeKind::Int || _kind == SemaTypeKind::Float;
    }

    /// The scalar of a scalar, vector or matrix; null otherwise.
    [[nodiscard]] const SemaType *GetScalar() const;

    /// Scalars, vectors and matrices.
    [[nodiscard]] bool IsArithmetic() const
    {
        return GetScalar() != nullptr;
    }

    [[nodiscard]] bool IsHandle() const
    {
        return _kind == SemaTypeKind::Texture || _kind == SemaTypeKind::Buffer || _kind == SemaTypeKind::Sampler ||
               _kind == SemaTypeKind::AccelerationStructure;
    }

    /// Whether values of this type may live in GPU memory and [Uniform] parameters (§4.3, §10): no ray queries, hit
    /// objects or cooperative types.
    [[nodiscard]] bool IsStorable() const;

    /// Classes, interfaces, delegates, script arrays and strings: values that refer to an object on the GC heap
    /// (MarlinScript §3.1). `T?` of a reference type is not one itself.
    [[nodiscard]] bool IsReference() const;

    /// Struct, class, interface and delegate types made from a SemaStructInstance, whose members it declares.
    [[nodiscard]] bool HasInstance() const
    {
        return _struct != nullptr;
    }

    /// The language name: `float3`, `Texture2D<half4>`, `Slab`, `Vector<int, 2>[4]`.
    void Print(StringWriter &output) const;

private:
    friend class SemaTypeTable;

    SemaTypeKind _kind = SemaTypeKind::Error;
    uint8 _width = 0;
    bool _signed = false;
    bool _arrayed = false;
    bool _multisampled = false;
    bool _writable = false;
    MirTextureDim _dim = MirTextureDim::Texture2D;
    MirImageFormat _format = MirImageFormat::Unknown;
    MirMatrixUse _matrixUse = MirMatrixUse::A;
    SemaMatrixLayout _layout = SemaMatrixLayout::RowMajor;
    SemaMeshOutput _meshOutput = SemaMeshOutput::Vertices;
    uint32 _count = 0;
    uint32 _rows = 0;
    const SemaType *_element = nullptr;
    SemaStructInstance *_struct = nullptr;
    SemaSymbol *_symbol = nullptr;
};

/// Creates and uniques the types of a compilation.
class SemaTypeTable
{
public:
    explicit SemaTypeTable(Arena &arena);
    MARLIN_NONCOPYABLE(SemaTypeTable);

    [[nodiscard]] const SemaType *GetError() const
    {
        return _error;
    }

    [[nodiscard]] const SemaType *GetVoid() const
    {
        return _void;
    }

    [[nodiscard]] const SemaType *GetBool() const
    {
        return _bool;
    }

    [[nodiscard]] const SemaType *GetNull() const
    {
        return _null;
    }

    [[nodiscard]] const SemaType *GetFeature() const
    {
        return _feature;
    }

    [[nodiscard]] const SemaType *GetString() const
    {
        return _string;
    }

    [[nodiscard]] const SemaType *GetFunctionGroup() const
    {
        return _functionGroup;
    }

    [[nodiscard]] const SemaType *GetInt(uint32 width, bool isSigned);
    [[nodiscard]] const SemaType *GetFloat(uint32 width);
    [[nodiscard]] const SemaType *GetVector(const SemaType *scalar, uint32 count);
    [[nodiscard]] const SemaType *GetMatrix(const SemaType *scalar, uint32 rows, uint32 columns);
    [[nodiscard]] const SemaType *GetArray(const SemaType *element, uint32 count);
    [[nodiscard]] const SemaType *GetStruct(SemaStructInstance *instance);
    [[nodiscard]] const SemaType *GetEnum(SemaSymbol *symbol, const SemaType *underlying);
    [[nodiscard]] const SemaType *GetInterface(SemaSymbol *symbol);
    /// An interface with its generic arguments, such as `IEnumerable<int>` (MarlinScript §3.4).
    [[nodiscard]] const SemaType *GetInterface(SemaStructInstance *instance);
    [[nodiscard]] const SemaType *GetClass(SemaStructInstance *instance);
    [[nodiscard]] const SemaType *GetDelegate(SemaStructInstance *instance);
    [[nodiscard]] const SemaType *GetGcArray(const SemaType *element);
    /// `T?`; already nullable types and `null` are returned unchanged.
    [[nodiscard]] const SemaType *GetNullable(const SemaType *element);
    [[nodiscard]] const SemaType *GetTexture(const SemaType *texel, MirTextureDim dim, bool arrayed, bool multisampled,
                                             bool writable, MirImageFormat format);
    /// A structured buffer of *element*, or a byte address buffer when it is null.
    [[nodiscard]] const SemaType *GetBuffer(const SemaType *element, bool writable);
    [[nodiscard]] const SemaType *GetSampler(bool comparison);
    [[nodiscard]] const SemaType *GetAccelerationStructure();
    /// `RayQuery<Flags>`: *flags* are the RayFlags fixed for every traversal of the query.
    [[nodiscard]] const SemaType *GetRayQuery(uint32 flags);
    [[nodiscard]] const SemaType *GetHitObject();
    [[nodiscard]] const SemaType *GetCoopVector(const SemaType *scalar, uint32 count);
    [[nodiscard]] const SemaType *GetCoopMatrix(const SemaType *scalar, uint32 rows, uint32 columns, MirMatrixUse use);
    [[nodiscard]] const SemaType *GetMatrixRef(const SemaType *scalar, uint32 rows, uint32 columns,
                                               SemaMatrixLayout layout);
    [[nodiscard]] const SemaType *GetVectorRef(const SemaType *scalar, uint32 count);
    [[nodiscard]] const SemaType *GetMeshOutput(SemaMeshOutput kind, const SemaType *element, uint32 count);

    /// A scalar, vector or matrix of *shape* with *scalar* in place of its scalar.
    [[nodiscard]] const SemaType *WithScalar(const SemaType *shape, const SemaType *scalar);

private:
    struct Key
    {
        SemaTypeKind Kind = SemaTypeKind::Error;
        uint8 Width = 0;
        uint8 Flags = 0;
        uint8 Extra = 0;
        uint32 Count = 0;
        uint32 Rows = 0;
        const void *Element = nullptr;
        const void *Owner = nullptr;

        [[nodiscard]] uint64 GetHash() const;
        bool operator==(const Key &) const = default;
    };

    const SemaType *Intern(const Key &key, const SemaType &prototype);
    const SemaType *MakeFixed(SemaTypeKind kind);

    Arena *_arena;
    HashMap<Key, const SemaType *> _types;
    const SemaType *_error;
    const SemaType *_void;
    const SemaType *_bool;
    const SemaType *_null;
    const SemaType *_feature;
    const SemaType *_string;
    const SemaType *_functionGroup;
};
} // namespace Marlin
