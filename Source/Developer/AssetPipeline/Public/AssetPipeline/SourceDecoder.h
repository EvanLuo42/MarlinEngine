#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Math/Matrix.h"
#include "Math/Vector.h"
#include "Reflection/ExtensionSet.h"
#include "String/String.h"
#include "String/StringView.h"
#include "AssetPipeline/SourceDecoder.gen.h"

namespace Marlin
{
class StringWriter;

enum class DecodedPixelFormat : uint8
{
    /// 8 bits per channel.
    RGBA8,
    /// 16 bits per channel, unsigned normalized.
    RGBA16,
    RGBA32Float,
};

/// An image as decoders hand it to converters: always four channels, rows top to bottom with no padding. Missing
/// channels are filled in: grey becomes RGB, a missing alpha is opaque.
struct DecodedImage
{
    uint32 Width = 0;
    uint32 Height = 0;
    DecodedPixelFormat Format = DecodedPixelFormat::RGBA8;
    List<uint8> Pixels;

    [[nodiscard]] uint32 GetPixelBytes() const
    {
        return Format == DecodedPixelFormat::RGBA8 ? 4 : Format == DecodedPixelFormat::RGBA16 ? 8 : 16;
    }
};

/// Reads one image file format (Documentation/Architecture/13 §7.1): an extension point chosen by file extension,
/// then by the file's header when several claim one. Converters never read formats themselves; they call
/// ConversionContext::Decode, which records the decoders as a DDC input so upgrading one reconverts only what it read.
/// Bump GetVersion whenever the decoded pixels change for the same file.
MCLASS(Abstract, ExtensionPoint)
class ASSETPIPELINE_API ImageDecoder
{
    MGENERATED_BODY()
public:
    ImageDecoder() = default;
    virtual ~ImageDecoder() = default;
    MARLIN_NONCOPYABLE(ImageDecoder);

    /// File extensions without the dot, lower case.
    [[nodiscard]] virtual Span<const StringView> GetExtensions() const = 0;
    [[nodiscard]] virtual uint32 GetVersion() const = 0;
    /// True if *header*, the first bytes of a file, is this format.
    [[nodiscard]] virtual bool CanDecode(Span<const uint8> header) const = 0;
    virtual bool Decode(Span<const uint8> bytes, DecodedImage &image, StringWriter &error) const = 0;
};

/// The image decoders linked into the process, by extension in priority order. Immutable once built.
class ASSETPIPELINE_API ImageDecoderSet
{
public:
    ImageDecoderSet() = default;
    MARLIN_NONCOPYABLE(ImageDecoderSet);

    void Build(const ExtensionContext &context);

    /// The decoders that claim *extension*, highest priority first; empty if none.
    [[nodiscard]] Span<ImageDecoder *const> Find(StringView extension) const;
    /// The decoder for *bytes* among those of *extension*: the first whose header check passes; or null.
    [[nodiscard]] ImageDecoder *Choose(StringView extension, Span<const uint8> bytes) const;

private:
    ExtensionSet<ImageDecoder> _decoders;
    HashMap<String, List<ImageDecoder *>> _byExtension;
};

/// Triangles of one material, as decoders hand them over: every stream holds one entry per vertex, and the optional
/// ones are empty when the file has none.
struct DecodedPrimitive
{
    /// Into DecodedScene::Materials.
    uint32 Material = 0;
    List<Vector3> Positions;
    List<Vector3> Normals;
    /// xyz and the bitangent sign in w.
    List<Vector4> Tangents;
    /// One list per UV set, in the file's order.
    List<List<Vector2>> TexCoords;
    List<Vector4> Colors;
    /// Three per triangle, counter-clockwise when seen from the front.
    List<uint32> Indices;
};

struct DecodedMesh
{
    String Name;
    List<DecodedPrimitive> Primitives;
};

/// A node of the scene's hierarchy; Transform is relative to the parent.
struct DecodedNode
{
    String Name;
    int32 Parent = -1;
    Matrix4x4 Transform;
    /// Into DecodedScene::Meshes, or -1.
    int32 Mesh = -1;
};

struct DecodedMaterial
{
    String Name;
    /// Both sides of its triangles show (glTF doubleSided); otherwise the renderer culls back faces.
    bool TwoSided = false;
};

/// A scene file as decoders hand it to converters and importers, already in engine space: right-handed, Y up,
/// metres (Documentation/Architecture/03 §6).
struct DecodedScene
{
    List<DecodedNode> Nodes;
    List<DecodedMesh> Meshes;
    List<DecodedMaterial> Materials;
};

/// The other files a source file names, such as a glTF's separate buffers. Decoders read them through this, never
/// the file system, so that a conversion records each one as an input of its result (Documentation/Architecture/06
/// §3.2).
class ASSETPIPELINE_API ISourceFileReader
{
public:
    ISourceFileReader() = default;
    virtual ~ISourceFileReader() = default;
    MARLIN_NONCOPYABLE(ISourceFileReader);

    static constexpr uint64 ToEnd = ~uint64{0};

    /// Reads *size* bytes from *offset* (ToEnd: the rest) of the file *uri* names: a relative path from the decoded
    /// file's folder, '/' between folders, already percent-decoded. False with the reason in *error* if it resolves to
    /// no file (ResolveReferencedPath), or the file cannot be read or is shorter than asked for. Reading only what is
    /// needed of a large file, such as one mesh's part of a scene's buffer, keeps the rest out of memory.
    virtual bool ReadReferencedRange(StringView uri, uint64 offset, uint64 size, List<uint8> &bytes,
                                     StringWriter &error) = 0;
    /// The whole file.
    bool ReadReferenced(StringView uri, List<uint8> &bytes, StringWriter &error)
    {
        return ReadReferencedRange(uri, 0, ToEnd, bytes, error);
    }
};

/// The virtual path of *uri*, relative to *file*'s folder: "." and ".." are resolved and '\' counts as '/'. False if
/// *uri* is empty, absolute, has a scheme or drive (a ':'), or climbs out of *file*'s mount, its first folder.
[[nodiscard]] ASSETPIPELINE_API bool ResolveReferencedPath(StringView file, StringView uri, String &path);

/// Reads one scene file format, such as glTF or FBX (Documentation/Architecture/13 §7.1); chosen like ImageDecoder.
/// Bump GetVersion whenever the decoded scene changes for the same file.
MCLASS(Abstract, ExtensionPoint)
class ASSETPIPELINE_API SceneDecoder
{
    MGENERATED_BODY()
public:
    SceneDecoder() = default;
    virtual ~SceneDecoder() = default;
    MARLIN_NONCOPYABLE(SceneDecoder);

    /// File extensions without the dot, lower case.
    [[nodiscard]] virtual Span<const StringView> GetExtensions() const = 0;
    [[nodiscard]] virtual uint32 GetVersion() const = 0;
    /// True if *header*, the first bytes of a file, is this format.
    [[nodiscard]] virtual bool CanDecode(Span<const uint8> header) const = 0;
    /// Decodes *bytes*, reading the files it names through *files*. Given a *mesh* name, only that mesh needs its
    /// primitives, so a decoder that can skip the others reads and decodes just that mesh's data and lists the rest
    /// without primitives, as DecodeOutline does; one that cannot may decode them all.
    virtual bool Decode(Span<const uint8> bytes, ISourceFileReader &files, StringView mesh, DecodedScene &scene,
                        StringWriter &error) const = 0;
    /// The nodes, meshes and materials with their names, as Decode gives them, but no primitives: what an importer
    /// needs to list the parts of a file. The fallback material Decode adds may be missing. Decoders that can skip
    /// the geometry override this; by default it is Decode.
    virtual bool DecodeOutline(Span<const uint8> bytes, ISourceFileReader &files, DecodedScene &scene,
                               StringWriter &error) const
    {
        return Decode(bytes, files, {}, scene, error);
    }
};

/// The scene decoders linked into the process, by extension in priority order. Immutable once built.
class ASSETPIPELINE_API SceneDecoderSet
{
public:
    SceneDecoderSet() = default;
    MARLIN_NONCOPYABLE(SceneDecoderSet);

    void Build(const ExtensionContext &context);

    [[nodiscard]] Span<SceneDecoder *const> Find(StringView extension) const;
    [[nodiscard]] SceneDecoder *Choose(StringView extension, Span<const uint8> bytes) const;

private:
    ExtensionSet<SceneDecoder> _decoders;
    HashMap<String, List<SceneDecoder *>> _byExtension;
};
} // namespace Marlin
