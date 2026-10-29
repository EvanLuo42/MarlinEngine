#include "Testing/Testing.h"

#include "GltfSceneDecoder/GltfSceneDecoder.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "meshoptimizer.h"

namespace Marlin::Tests
{
namespace
{
/// A unit quad facing up in material Stone, two-sided, placed by Floor, 1 m up, and by its child scaled by two.
constexpr StringView QuadJson =
    R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
    R"("nodes":[{"name":"Floor","mesh":0,"translation":[0,1,0],"children":[1]},)"
    R"({"name":"Child","mesh":0,"scale":[2,2,2]}],)"
    R"("meshes":[{"name":"Quad",)"
    R"("primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]}],)"
    R"("materials":[{"name":"Stone","doubleSided":true}],"buffers":[{"byteLength":92}],)"
    R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":32},)"
    R"({"buffer":0,"byteOffset":80,"byteLength":12}],)"
    R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,-1],"max":[1,0,0]},)"
    R"({"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},)"
    R"({"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"}]})";

constexpr float32 QuadPositions[12] = {0, 0, 0, 1, 0, 0, 1, 0, -1, 0, 0, -1};
constexpr uint16 QuadIndices[6] = {0, 1, 2, 0, 2, 3};

/// The files a test scene names, by URI; remembers which were asked for and how many bytes were read.
class TestFiles final : public ISourceFileReader
{
public:
    struct File
    {
        String Uri;
        List<uint8> Bytes;
    };

    List<File> Files;
    List<String> Asked;
    uint64 BytesRead = 0;

    void Add(StringView uri, Span<const uint8> bytes)
    {
        File &file = Files.AddDefaulted();
        file.Uri = uri;
        file.Bytes.AddRange(bytes);
    }

    bool ReadReferencedRange(StringView uri, uint64 offset, uint64 size, List<uint8> &bytes,
                             StringWriter &error) override
    {
        Asked.Add(String(uri));
        for (const File &file : Files)
        {
            if (file.Uri != uri)
            {
                continue;
            }
            const uint64 end = size == ToEnd ? file.Bytes.Count() : offset + size;
            if (offset > end || end > file.Bytes.Count())
            {
                FormatTo(error, "{0} is too short", uri);
                return false;
            }
            bytes.Clear();
            bytes.AddRange(file.Bytes.AsSpan().Slice(static_cast<usize>(offset), static_cast<usize>(end - offset)));
            BytesRead += end - offset;
            return true;
        }
        FormatTo(error, "{0} does not exist", uri);
        return false;
    }
};

Span<const uint8> AsBytes(StringView text)
{
    return {reinterpret_cast<const uint8 *>(text.Data()), text.Length()};
}

void Append32(List<uint8> &bytes, uint32 value)
{
    for (uint32 shift = 0; shift < 32; shift += 8)
    {
        bytes.Add(static_cast<uint8>(value >> shift));
    }
}

/// QuadJson's buffer: positions, UVs, indices.
List<uint8> QuadBuffer()
{
    const float32 texCoords[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    List<uint8> buffer;
    buffer.Resize(sizeof(QuadPositions) + sizeof(texCoords) + sizeof(QuadIndices));
    MemCopy(buffer.Data(), QuadPositions, sizeof(QuadPositions));
    MemCopy(buffer.Data() + sizeof(QuadPositions), texCoords, sizeof(texCoords));
    MemCopy(buffer.Data() + sizeof(QuadPositions) + sizeof(texCoords), QuadIndices, sizeof(QuadIndices));
    return buffer;
}

/// QuadJson and its buffer as a .glb: the header, then a JSON and a BIN chunk, each padded to four bytes.
List<uint8> QuadGlb()
{
    List<uint8> json;
    json.AddRange(AsBytes(QuadJson));
    while (json.Count() % 4 != 0)
    {
        json.Add(' ');
    }
    const List<uint8> buffer = QuadBuffer();

    List<uint8> glb;
    Append32(glb, 0x46546C67); // "glTF"
    Append32(glb, 2);
    Append32(glb, static_cast<uint32>(12 + 8 + json.Count() + 8 + buffer.Count()));
    Append32(glb, static_cast<uint32>(json.Count()));
    Append32(glb, 0x4E4F534A); // "JSON"
    glb.AddRange(json.AsSpan());
    Append32(glb, static_cast<uint32>(buffer.Count()));
    Append32(glb, 0x004E4942); // "BIN"
    glb.AddRange(buffer.AsSpan());
    return glb;
}

void ExpectQuad(const DecodedScene &scene)
{
    MARLIN_REQUIRE(scene.Nodes.Count() == 2);
    MARLIN_EXPECT(scene.Nodes[0].Name == "Floor" && scene.Nodes[0].Mesh == 0 && scene.Nodes[0].Parent == -1);
    MARLIN_EXPECT(scene.Nodes[0].Transform.Rows[1].W == 1);
    MARLIN_EXPECT(scene.Nodes[1].Parent == 0 && scene.Nodes[1].Transform.Rows[0].X == 2);
    MARLIN_REQUIRE(scene.Meshes.Count() == 1 && scene.Meshes[0].Primitives.Count() == 1);
    MARLIN_EXPECT(scene.Meshes[0].Name == "Quad");
    const DecodedPrimitive &primitive = scene.Meshes[0].Primitives[0];
    MARLIN_EXPECT(scene.Materials.Count() == 1 && scene.Materials[0].Name == "Stone" && primitive.Material == 0);
    MARLIN_EXPECT(scene.Materials[0].TwoSided);
    MARLIN_REQUIRE(primitive.TexCoords.Count() == 1 && primitive.TexCoords[0].Count() == 4);
    MARLIN_REQUIRE(primitive.Positions.Count() == 4 && primitive.Indices.Count() == 6);
    MARLIN_EXPECT(primitive.TexCoords[0][2].X == 1 && primitive.Positions[2].Z == -1 && primitive.Indices[4] == 2);
}

/// A quad of positions and indices only, in *buffers* and *views*, with *extensions* required.
String PlainQuadJson(StringView extensions, StringView positionType, StringView buffers, StringView views)
{
    String json(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)");
    json.Append(R"("extensionsUsed":[)").Append(extensions).Append(R"(],"extensionsRequired":[)").Append(extensions);
    json.Append(R"(],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)");
    json.Append(R"("buffers":[)").Append(buffers).Append(R"(],"bufferViews":[)").Append(views);
    json.Append(R"(],"accessors":[{"bufferView":0,"componentType":)").Append(positionType);
    json.Append(R"(,"count":4,"type":"VEC3","min":[0,0,-1],"max":[1,0,0]},)");
    json.Append(R"({"bufferView":1,"componentType":5123,"count":6,"type":"SCALAR"}]})");
    return json;
}
} // namespace

MARLIN_TEST(GltfSceneDecoder, Glb)
{
    const GltfSceneDecoder decoder;
    const List<uint8> glb = QuadGlb();
    MARLIN_REQUIRE(decoder.CanDecode(glb.AsSpan().Slice(0, 16)));
    TestFiles files;
    DecodedScene scene;
    StringBuilder<256> error;
    MARLIN_REQUIRE(decoder.Decode(glb.AsSpan(), files, {}, scene, error));
    ExpectQuad(scene);
    MARLIN_EXPECT(files.Asked.IsEmpty());
}

MARLIN_TEST(GltfSceneDecoder, ExternalBuffers)
{
    const GltfSceneDecoder decoder;
    String json(QuadJson);
    (void)json.Replace(R"("buffers":[{"byteLength":92}])", R"("buffers":[{"byteLength":92,"uri":"Quad%20Data.bin"}])");
    MARLIN_REQUIRE(decoder.CanDecode(AsBytes(json.AsView())));

    // The outline reads no buffers, so the file need not be there.
    TestFiles files;
    DecodedScene outline;
    StringBuilder<256> error;
    MARLIN_REQUIRE(decoder.DecodeOutline(AsBytes(json.AsView()), files, outline, error));
    MARLIN_EXPECT(outline.Meshes.Count() == 1 && outline.Meshes[0].Primitives.IsEmpty() && files.Asked.IsEmpty());

    DecodedScene missing;
    MARLIN_EXPECT(!decoder.Decode(AsBytes(json.AsView()), files, {}, missing, error));
    MARLIN_EXPECT(error.AsView().Contains("Quad Data.bin does not exist"));

    // The URI arrives percent-decoded.
    const List<uint8> buffer = QuadBuffer();
    files.Add("Quad Data.bin", buffer.AsSpan());
    DecodedScene scene;
    error.Clear();
    MARLIN_REQUIRE(decoder.Decode(AsBytes(json.AsView()), files, {}, scene, error));
    ExpectQuad(scene);

    // A file shorter than its buffer is an error, not a read past the end.
    files.Files[0].Bytes.Resize(40);
    DecodedScene truncated;
    MARLIN_EXPECT(!decoder.Decode(AsBytes(json.AsView()), files, {}, truncated, error));
}

MARLIN_TEST(GltfSceneDecoder, MeshoptCompression)
{
    // The positions and indices compressed into Quad.meshopt; the fallback buffer is never read, though it names a
    // file that does not exist.
    List<uint8> vertices;
    vertices.Resize(meshopt_encodeVertexBufferBound(4, 12));
    vertices.Resize(meshopt_encodeVertexBuffer(vertices.Data(), vertices.Count(), QuadPositions, 4, 12));
    MARLIN_REQUIRE(!vertices.IsEmpty());
    while (vertices.Count() % 4 != 0)
    {
        vertices.Add(0);
    }
    const uint32 indices[6] = {QuadIndices[0], QuadIndices[1], QuadIndices[2],
                               QuadIndices[3], QuadIndices[4], QuadIndices[5]};
    List<uint8> triangles;
    triangles.Resize(meshopt_encodeIndexBufferBound(6, 4));
    triangles.Resize(meshopt_encodeIndexBuffer(triangles.Data(), triangles.Count(), indices, 6));
    MARLIN_REQUIRE(!triangles.IsEmpty());
    List<uint8> compressed;
    compressed.AddRange(vertices.AsSpan());
    compressed.AddRange(triangles.AsSpan());

    const String vertexBytes = Format("{0}", vertices.Count());
    const String triangleBytes = Format("{0}", triangles.Count());
    String buffers(R"({"byteLength":)");
    buffers.Append(Format("{0}", compressed.Count())).Append(R"(,"uri":"Quad.meshopt"},)");
    buffers.Append(R"({"byteLength":60,"uri":"Fallback.bin","extensions":{"EXT_meshopt_compression":{"fallback":true}}})");
    String views(R"({"buffer":1,"byteOffset":0,"byteLength":48,"byteStride":12,"extensions":{"EXT_meshopt_compression":)");
    views.Append(R"({"buffer":0,"byteOffset":0,"byteLength":)").Append(vertexBytes);
    views.Append(R"(,"byteStride":12,"count":4,"mode":"ATTRIBUTES"}}},)");
    views.Append(R"({"buffer":1,"byteOffset":48,"byteLength":12,"extensions":{"EXT_meshopt_compression":)");
    views.Append(R"({"buffer":0,"byteOffset":)").Append(vertexBytes).Append(R"(,"byteLength":)").Append(triangleBytes);
    views.Append(R"(,"byteStride":2,"count":6,"mode":"TRIANGLES"}}})");
    const String json = PlainQuadJson(R"("EXT_meshopt_compression")", "5126", buffers, views);

    const GltfSceneDecoder decoder;
    TestFiles files;
    files.Add("Quad.meshopt", compressed.AsSpan());
    DecodedScene scene;
    StringBuilder<256> error;
    MARLIN_REQUIRE(decoder.Decode(AsBytes(json.AsView()), files, {}, scene, error));
    // Each compressed view reads just its range of the file; the fallback is never read.
    MARLIN_EXPECT(files.Asked.Count() == 2 && files.Asked[0] == "Quad.meshopt" && files.Asked[1] == "Quad.meshopt");
    MARLIN_EXPECT(files.BytesRead == compressed.Count());
    MARLIN_REQUIRE(scene.Meshes.Count() == 1 && scene.Meshes[0].Primitives.Count() == 1);
    const DecodedPrimitive &primitive = scene.Meshes[0].Primitives[0];
    MARLIN_REQUIRE(primitive.Positions.Count() == 4 && primitive.Indices.Count() == 6);
    MARLIN_EXPECT(primitive.Positions[1].X == 1 && primitive.Positions[2].Z == -1 && primitive.Positions[3].Z == -1);
    // The index codec keeps the triangles but may rotate the corners of each.
    uint32 sum = 0;
    for (const uint32 index : primitive.Indices)
    {
        sum += index;
    }
    MARLIN_EXPECT(sum == 8);
}

MARLIN_TEST(GltfSceneDecoder, MeshQuantization)
{
    // Positions as unnormalized shorts, which glTF reads as their integer values, each padded to four bytes.
    const int16 positions[16] = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, -1, 0, 0, 0, -1, 0};
    List<uint8> buffer;
    buffer.Resize(sizeof(positions) + sizeof(QuadIndices));
    MemCopy(buffer.Data(), positions, sizeof(positions));
    MemCopy(buffer.Data() + sizeof(positions), QuadIndices, sizeof(QuadIndices));
    const String json = PlainQuadJson(R"("KHR_mesh_quantization")", "5122", R"({"byteLength":44,"uri":"Quad.bin"})",
                                      R"({"buffer":0,"byteOffset":0,"byteLength":32,"byteStride":8},)"
                                      R"({"buffer":0,"byteOffset":32,"byteLength":12})");
    const GltfSceneDecoder decoder;
    TestFiles files;
    files.Add("Quad.bin", buffer.AsSpan());
    DecodedScene scene;
    StringBuilder<256> error;
    MARLIN_REQUIREF(decoder.Decode(AsBytes(json.AsView()), files, {}, scene, error), "{0}", error.AsView());
    MARLIN_REQUIRE(scene.Meshes.Count() == 1 && scene.Meshes[0].Primitives.Count() == 1);
    const DecodedPrimitive &primitive = scene.Meshes[0].Primitives[0];
    MARLIN_REQUIRE(primitive.Positions.Count() == 4);
    MARLIN_EXPECT(primitive.Positions[1].X == 1 && primitive.Positions[2].Z == -1);
}

MARLIN_TEST(GltfSceneDecoder, UnreadableRequiredExtensions)
{
    constexpr StringView Json = R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_draco_mesh_compression"],)"
                                R"("extensionsRequired":["KHR_draco_mesh_compression"]})";
    const GltfSceneDecoder decoder;
    TestFiles files;
    DecodedScene scene;
    StringBuilder<256> error;
    MARLIN_EXPECT(!decoder.Decode(AsBytes(Json), files, {}, scene, error));
    MARLIN_EXPECT(error.AsView().Contains("KHR_draco_mesh_compression"));
    MARLIN_EXPECT(!decoder.DecodeOutline(AsBytes(Json), files, scene, error));
}

MARLIN_TEST(GltfSceneDecoder, OneMeshReadsOnlyItsRanges)
{
    // Two quads, A and B, one after the other in one buffer file: 48 bytes of positions and 12 of indices each.
    List<uint8> buffer;
    for (uint32 quad = 0; quad < 2; ++quad)
    {
        const usize at = buffer.Count();
        buffer.Resize(at + sizeof(QuadPositions) + sizeof(QuadIndices));
        MemCopy(buffer.Data() + at, QuadPositions, sizeof(QuadPositions));
        MemCopy(buffer.Data() + at + sizeof(QuadPositions), QuadIndices, sizeof(QuadIndices));
    }
    constexpr StringView Json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],"nodes":[{"mesh":0},{"mesh":1}],)"
        R"("meshes":[{"name":"A","primitives":[{"attributes":{"POSITION":0},"indices":1}]},)"
        R"({"name":"B","primitives":[{"attributes":{"POSITION":2},"indices":3}]}],)"
        R"("buffers":[{"byteLength":120,"uri":"Quads.bin"}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":12},)"
        R"({"buffer":0,"byteOffset":60,"byteLength":48},{"buffer":0,"byteOffset":108,"byteLength":12}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,-1],"max":[1,0,0]},)"
        R"({"bufferView":1,"componentType":5123,"count":6,"type":"SCALAR"},)"
        R"({"bufferView":2,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,-1],"max":[1,0,0]},)"
        R"({"bufferView":3,"componentType":5123,"count":6,"type":"SCALAR"}]})";
    const GltfSceneDecoder decoder;
    TestFiles files;
    files.Add("Quads.bin", buffer.AsSpan());
    DecodedScene scene;
    StringBuilder<256> error;
    MARLIN_REQUIREF(decoder.Decode(AsBytes(Json), files, "B", scene, error), "{0}", error.AsView());
    MARLIN_REQUIRE(scene.Meshes.Count() == 2 && scene.Nodes.Count() == 2);
    MARLIN_EXPECT(scene.Meshes[0].Primitives.IsEmpty() && scene.Meshes[1].Primitives.Count() == 1);
    MARLIN_EXPECT(scene.Meshes[1].Primitives[0].Positions.Count() == 4);
    MARLIN_EXPECT(files.BytesRead == 60);

    DecodedScene both;
    files.BytesRead = 0;
    MARLIN_REQUIRE(decoder.Decode(AsBytes(Json), files, {}, both, error));
    MARLIN_EXPECT(both.Meshes[0].Primitives.Count() == 1 && both.Meshes[1].Primitives.Count() == 1);
    MARLIN_EXPECT(files.BytesRead == 120);
}

MARLIN_TEST(GltfSceneDecoder, GpuInstancingIsNotRead)
{
    // Used but not required: the mesh is placed once, at its node, as the extension's fallback.
    const List<uint8> buffer = QuadBuffer();
    String json(QuadJson);
    (void)json.Replace(R"("buffers":[{"byteLength":92}])", R"("buffers":[{"byteLength":92,"uri":"Quad.bin"}])");
    (void)json.Replace(R"({"name":"Floor","mesh":0,)",
                       R"({"name":"Floor","mesh":0,"extensions":{"EXT_mesh_gpu_instancing":)"
                       R"({"attributes":{"TRANSLATION":0}}},)");
    (void)json.Replace(R"({"asset":{"version":"2.0"},)",
                       R"({"asset":{"version":"2.0"},"extensionsUsed":["EXT_mesh_gpu_instancing"],)");
    const GltfSceneDecoder decoder;
    TestFiles files;
    files.Add("Quad.bin", buffer.AsSpan());
    DecodedScene scene;
    StringBuilder<256> error;
    MARLIN_REQUIREF(decoder.Decode(AsBytes(json.AsView()), files, {}, scene, error), "{0}", error.AsView());
    ExpectQuad(scene);

    // Required: an error.
    (void)json.Replace(R"("extensionsUsed":["EXT_mesh_gpu_instancing"],)",
                       R"("extensionsUsed":["EXT_mesh_gpu_instancing"],"extensionsRequired":["EXT_mesh_gpu_instancing"],)");
    DecodedScene required;
    MARLIN_EXPECT(!decoder.Decode(AsBytes(json.AsView()), files, {}, required, error));
    MARLIN_EXPECT(error.AsView().Contains("EXT_mesh_gpu_instancing"));
}
} // namespace Marlin::Tests
