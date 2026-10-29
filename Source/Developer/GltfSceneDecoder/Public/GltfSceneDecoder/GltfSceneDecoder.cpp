#include "GltfSceneDecoder/GltfSceneDecoder.h"

#include "Memory/UniquePtr.h"
#include "Templates/Function.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "cgltf.h"
#include "meshoptimizer.h"

namespace Marlin
{
namespace
{
/// cgltf's file reader. Decode hides every separate buffer file from cgltf_load_buffers and reads only the ranges it
/// needs itself (ViewLoader), so cgltf never reads a file.
cgltf_result RefuseFile(const cgltf_memory_options * /*memory*/, const cgltf_file_options * /*file*/,
                        const char * /*path*/, cgltf_size * /*size*/, void ** /*data*/)
{
    return cgltf_result_file_not_found;
}

void ReleaseNothing(const cgltf_memory_options * /*memory*/, const cgltf_file_options * /*file*/, void * /*data*/)
{
}

/// The extensions a file may require: those Decode reads, and those that only change what it does not read. Materials
/// are Marlin's own, so of a glTF material only the name is read, as the slot; its parameters, textures and images
/// are not, embedded or not.
bool IsRequirable(StringView extension)
{
    return extension == "KHR_mesh_quantization" || extension == "EXT_meshopt_compression" ||
           extension == "KHR_lights_punctual" || extension.StartsWith("KHR_materials_") ||
           extension.StartsWith("KHR_texture_") || extension.StartsWith("EXT_texture_");
}

/// Gives buffer views their bytes when an accessor that reads them is about to be read, so that a decode reads only
/// what it uses: a view of a buffer in memory (the .glb's own, a data URI) reads it in place; a view of a separate
/// buffer file reads just its range of the file; an EXT_meshopt_compression view reads its compressed range and is
/// decompressed (its own buffer is the uncompressed fallback, never read). What it reads lives as long as it does.
class ViewLoader
{
public:
    ViewLoader(cgltf_data &data, ISourceFileReader &files) : _data(data), _files(files)
    {
        _loaded.Resize(data.buffer_views_count, false);
        _uris.Resize(data.buffers_count, nullptr);
    }

    ~ViewLoader()
    {
        for (cgltf_size b = 0; b < _data.buffers_count; ++b)
        {
            if (_uris[b] != nullptr)
            {
                _data.buffers[b].uri = _uris[b];
            }
        }
        // Views this loaded point into memory cgltf does not own.
        for (cgltf_size v = 0; v < _data.buffer_views_count; ++v)
        {
            _data.buffer_views[v].data = nullptr;
        }
    }

    MARLIN_NONCOPYABLE(ViewLoader);

    /// Hides the separate buffer files from cgltf_load_buffers, which then loads only those in memory.
    void HideFiles()
    {
        for (cgltf_size b = 0; b < _data.buffers_count; ++b)
        {
            cgltf_buffer &buffer = _data.buffers[b];
            if (buffer.uri != nullptr && !StringView(buffer.uri).StartsWith("data:"))
            {
                _uris[b] = buffer.uri;
                buffer.uri = nullptr;
            }
        }
    }

    bool Load(const cgltf_accessor *accessor, StringWriter &error)
    {
        if (accessor == nullptr)
        {
            return true;
        }
        if (accessor->is_sparse && (!Load(accessor->sparse.indices_buffer_view, error) ||
                                    !Load(accessor->sparse.values_buffer_view, error)))
        {
            return false;
        }
        return Load(accessor->buffer_view, error);
    }

private:
    bool Load(cgltf_buffer_view *view, StringWriter &error)
    {
        if (view == nullptr)
        {
            return true;
        }
        const auto index = static_cast<usize>(view - _data.buffer_views);
        if (_loaded[index])
        {
            return true;
        }
        _loaded[index] = true;
        if (!view->has_meshopt_compression)
        {
            if (view->buffer->data != nullptr)
            {
                return true;
            }
            List<uint8> &bytes = *_storage.Add(MakeUnique<List<uint8>>());
            if (!Read(*view->buffer, view->offset, view->size, bytes, error))
            {
                return false;
            }
            view->data = bytes.Data();
            return true;
        }
        const cgltf_meshopt_compression &compression = view->meshopt_compression;
        if (compression.count * compression.stride < view->size)
        {
            FormatTo(error, "buffer view {0} decompresses to fewer bytes than it holds", index);
            return false;
        }
        List<uint8> compressed;
        const uint8 *source = nullptr;
        if (compression.buffer->data != nullptr)
        {
            source = static_cast<const uint8 *>(compression.buffer->data) + compression.offset;
        }
        else if (Read(*compression.buffer, compression.offset, compression.size, compressed, error))
        {
            source = compressed.Data();
        }
        else
        {
            return false;
        }
        List<uint8> &bytes = *_storage.Add(MakeUnique<List<uint8>>());
        bytes.ResizeUninitialized(compression.count * compression.stride);
        int result = -1;
        switch (compression.mode)
        {
        case cgltf_meshopt_compression_mode_attributes:
            result = meshopt_decodeVertexBuffer(bytes.Data(), compression.count, compression.stride, source,
                                                compression.size);
            break;
        case cgltf_meshopt_compression_mode_triangles:
            result = meshopt_decodeIndexBuffer(bytes.Data(), compression.count, compression.stride, source,
                                               compression.size);
            break;
        case cgltf_meshopt_compression_mode_indices:
            result = meshopt_decodeIndexSequence(bytes.Data(), compression.count, compression.stride, source,
                                                 compression.size);
            break;
        default:
            break;
        }
        if (result != 0)
        {
            FormatTo(error, "buffer view {0} cannot be decompressed", index);
            return false;
        }
        switch (compression.filter)
        {
        case cgltf_meshopt_compression_filter_octahedral:
            meshopt_decodeFilterOct(bytes.Data(), compression.count, compression.stride);
            break;
        case cgltf_meshopt_compression_filter_quaternion:
            meshopt_decodeFilterQuat(bytes.Data(), compression.count, compression.stride);
            break;
        case cgltf_meshopt_compression_filter_exponential:
            meshopt_decodeFilterExp(bytes.Data(), compression.count, compression.stride);
            break;
        default:
            break;
        }
        view->data = bytes.Data();
        return true;
    }

    /// *size* bytes from *offset* of a separate buffer file.
    bool Read(const cgltf_buffer &buffer, cgltf_size offset, cgltf_size size, List<uint8> &bytes, StringWriter &error)
    {
        const char *uri = _uris[static_cast<usize>(&buffer - _data.buffers)];
        if (uri == nullptr || offset + size > buffer.size)
        {
            error.Append("a buffer view reads past its buffer, or from a buffer that has no data");
            return false;
        }
        String decoded(uri);
        decoded.Resize(cgltf_decode_uri(decoded.Data()));
        if (!_files.ReadReferencedRange(decoded.AsView(), offset, size, bytes, error))
        {
            return false;
        }
        if (bytes.Count() != size)
        {
            FormatTo(error, "{0} is shorter than its buffer", decoded);
            return false;
        }
        return true;
    }

    cgltf_data &_data;
    ISourceFileReader &_files;
    List<bool> _loaded;
    /// The separate files of the buffers, hidden from cgltf.
    List<char *> _uris;
    List<UniquePtr<List<uint8>>> _storage;
};

StringView ResultText(cgltf_result result)
{
    switch (result)
    {
    case cgltf_result_data_too_short:
        return "the file is truncated";
    case cgltf_result_unknown_format:
        return "not a glTF file, or a buffer URI that is neither a relative path nor base64 data";
    case cgltf_result_invalid_json:
        return "the JSON is invalid";
    case cgltf_result_invalid_gltf:
        return "the glTF is invalid";
    case cgltf_result_file_not_found:
    case cgltf_result_io_error:
        return "a buffer file it names cannot be read";
    case cgltf_result_legacy_gltf:
        return "glTF 1.0 is not supported";
    default:
        return "cgltf failed";
    }
}

/// The triangle list of *primitive*: its indices, or its vertices in order, with strips and fans unrolled. False if
/// an index is out of range or cannot be read.
bool ReadTriangles(const cgltf_primitive &primitive, cgltf_size vertexCount, List<uint32> &indices)
{
    List<uint32> order;
    if (primitive.indices != nullptr)
    {
        const cgltf_size count = primitive.indices->count;
        order.ResizeUninitialized(count);
        if (cgltf_accessor_unpack_indices(primitive.indices, order.Data(), sizeof(uint32), count) != count)
        {
            return false;
        }
        for (const uint32 index : order)
        {
            if (index >= vertexCount)
            {
                return false;
            }
        }
    }
    else
    {
        for (cgltf_size v = 0; v < vertexCount; ++v)
        {
            order.Add(static_cast<uint32>(v));
        }
    }
    const usize count = order.Count();
    switch (primitive.type)
    {
    case cgltf_primitive_type_triangles:
        indices.AddRange(order.AsSpan().Slice(0, count - count % 3));
        break;
    case cgltf_primitive_type_triangle_strip:
        // Every other triangle of a strip is wound the other way round.
        for (usize i = 0; i + 2 < count; ++i)
        {
            const bool odd = (i & 1) != 0;
            indices.AddRange({order[odd ? i + 1 : i], order[odd ? i : i + 1], order[i + 2]});
        }
        break;
    case cgltf_primitive_type_triangle_fan:
        for (usize i = 1; i + 1 < count; ++i)
        {
            indices.AddRange({order[0], order[i], order[i + 1]});
        }
        break;
    default:
        break;
    }
    return true;
}

/// Reads *source* into *primitive*; false if an attribute's count differs from the positions' or cannot be read.
/// A primitive without a material points one past the file's materials, where Decode adds the fallback.
bool ReadPrimitive(const cgltf_data &data, const cgltf_primitive &source, DecodedPrimitive &primitive)
{
    const cgltf_accessor *positions = nullptr;
    for (cgltf_size a = 0; a < source.attributes_count; ++a)
    {
        if (source.attributes[a].type == cgltf_attribute_type_position)
        {
            positions = source.attributes[a].data;
        }
    }
    if (positions == nullptr || positions->type != cgltf_type_vec3)
    {
        return false;
    }
    const cgltf_size vertexCount = positions->count;
    const auto unpack = [vertexCount](const cgltf_accessor &accessor, auto &list, cgltf_size components) {
        list.Resize(vertexCount);
        return accessor.count == vertexCount && cgltf_num_components(accessor.type) == components &&
               cgltf_accessor_unpack_floats(&accessor, reinterpret_cast<cgltf_float *>(list.Data()),
                                            vertexCount * components) == vertexCount * components;
    };
    if (!unpack(*positions, primitive.Positions, 3))
    {
        return false;
    }
    for (cgltf_size a = 0; a < source.attributes_count; ++a)
    {
        const cgltf_attribute &attribute = source.attributes[a];
        const cgltf_accessor &accessor = *attribute.data;
        bool read = true;
        switch (attribute.type)
        {
        case cgltf_attribute_type_normal:
            read = unpack(accessor, primitive.Normals, 3);
            break;
        case cgltf_attribute_type_tangent:
            read = unpack(accessor, primitive.Tangents, 4);
            break;
        case cgltf_attribute_type_texcoord:
            if (primitive.TexCoords.Count() <= static_cast<usize>(attribute.index))
            {
                primitive.TexCoords.Resize(static_cast<usize>(attribute.index) + 1);
            }
            read = unpack(accessor, primitive.TexCoords[static_cast<usize>(attribute.index)], 2);
            break;
        case cgltf_attribute_type_color:
            if (attribute.index != 0)
            {
                break;
            }
            primitive.Colors.Resize(vertexCount, Vector4(1));
            read = accessor.count == vertexCount;
            for (cgltf_size v = 0; read && v < vertexCount; ++v)
            {
                // A VEC3 color leaves the opaque alpha in place.
                read = cgltf_accessor_read_float(&accessor, v, &primitive.Colors[v].X, 4);
            }
            break;
        default:
            break;
        }
        if (!read)
        {
            return false;
        }
    }
    primitive.Material = static_cast<uint32>(source.material != nullptr ? cgltf_material_index(&data, source.material)
                                                                         : data.materials_count);
    return ReadTriangles(source, vertexCount, primitive.Indices);
}

/// EXT_mesh_gpu_instancing is not read: a file that uses it without requiring it places each such mesh once, at its
/// node, as the extension's fallback.
void AddNode(const cgltf_data &data, const cgltf_node &node, int32 parent, DecodedScene &scene)
{
    const auto index = static_cast<int32>(scene.Nodes.Count());
    DecodedNode &added = scene.Nodes.AddDefaulted();
    added.Name = node.name != nullptr ? node.name : "";
    added.Parent = parent;
    // Column-major, as glTF stores matrices.
    cgltf_float m[16];
    cgltf_node_transform_local(&node, m);
    for (uint32 row = 0; row < 4; ++row)
    {
        added.Transform.Rows[row] = Vector4(m[row], m[4 + row], m[8 + row], m[12 + row]);
    }
    added.Mesh = node.mesh != nullptr ? static_cast<int32>(cgltf_mesh_index(&data, node.mesh)) : -1;
    for (cgltf_size child = 0; child < node.children_count; ++child)
    {
        AddNode(data, *node.children[child], index, scene);
    }
}
} // namespace

Span<const StringView> GltfSceneDecoder::GetExtensions() const
{
    static constexpr StringView Extensions[] = {"glb", "gltf"};
    return Extensions;
}

bool GltfSceneDecoder::CanDecode(Span<const uint8> header) const
{
    // A .glb starts with its magic; a .gltf is a JSON object, perhaps after a byte order mark and white space.
    if (header.Count() >= 4 && header[0] == 'g' && header[1] == 'l' && header[2] == 'T' && header[3] == 'F')
    {
        return true;
    }
    usize at = 0;
    if (header.Count() >= 3 && header[0] == 0xEF && header[1] == 0xBB && header[2] == 0xBF)
    {
        at = 3;
    }
    for (; at < header.Count(); ++at)
    {
        const uint8 c = header[at];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        {
            return c == '{';
        }
    }
    return false;
}

namespace
{
/// What Decode and DecodeOutline do. Only the meshes *geometry* accepts get primitives, and only their data is read.
bool DecodeGltf(Span<const uint8> bytes, ISourceFileReader &files, DecodedScene &scene, StringWriter &error,
                FunctionRef<bool(StringView mesh)> geometry)
{
    scene.Nodes.Clear();
    scene.Meshes.Clear();
    scene.Materials.Clear();

    struct Parsed
    {
        cgltf_data *Data = nullptr;
        ~Parsed()
        {
            cgltf_free(Data);
        }
    } parsed;
    cgltf_options options = {};
    options.file.read = &RefuseFile;
    options.file.release = &ReleaseNothing;
    cgltf_result result = cgltf_parse(&options, bytes.Data(), bytes.Count(), &parsed.Data);
    if (result != cgltf_result_success)
    {
        error.Append(ResultText(result));
        return false;
    }
    for (cgltf_size e = 0; e < parsed.Data->extensions_required_count; ++e)
    {
        const StringView extension = parsed.Data->extensions_required[e];
        if (!IsRequirable(extension))
        {
            FormatTo(error, "it requires {0}, which Marlin does not read", extension);
            return false;
        }
    }
    // Destroyed before cgltf_free, which must see the buffers' URIs again.
    ViewLoader views(*parsed.Data, files);
    views.HideFiles();
    // An empty base path: no file is left for cgltf to read.
    result = cgltf_load_buffers(&options, parsed.Data, "");
    if (result == cgltf_result_success)
    {
        result = cgltf_validate(parsed.Data);
    }
    if (result != cgltf_result_success)
    {
        error.Append(ResultText(result));
        return false;
    }
    const cgltf_data &data = *parsed.Data;

    for (cgltf_size m = 0; m < data.materials_count; ++m)
    {
        const char *name = data.materials[m].name;
        DecodedMaterial &material = scene.Materials.AddDefaulted();
        material.Name = name != nullptr && name[0] != '\0' ? String(name) : Format("Material{0}", m);
        material.TwoSided = data.materials[m].double_sided != 0;
    }
    bool needsFallback = false;
    for (cgltf_size m = 0; m < data.meshes_count; ++m)
    {
        const cgltf_mesh &source = data.meshes[m];
        DecodedMesh &mesh = scene.Meshes.AddDefaulted();
        mesh.Name = source.name != nullptr && source.name[0] != '\0' ? String(source.name) : Format("Mesh{0}", m);
        const bool decode = geometry(mesh.Name.AsView());
        for (cgltf_size p = 0; p < source.primitives_count; ++p)
        {
            const cgltf_primitive &primitive = source.primitives[p];
            if (primitive.type != cgltf_primitive_type_triangles &&
                primitive.type != cgltf_primitive_type_triangle_strip &&
                primitive.type != cgltf_primitive_type_triangle_fan)
            {
                continue;
            }
            needsFallback = needsFallback || primitive.material == nullptr;
            if (!decode)
            {
                continue;
            }
            bool loaded = views.Load(primitive.indices, error);
            for (cgltf_size a = 0; loaded && a < primitive.attributes_count; ++a)
            {
                loaded = views.Load(primitive.attributes[a].data, error);
            }
            if (!loaded)
            {
                FormatTo(error, " (primitive {0} of mesh {1})", p, m);
                return false;
            }
            DecodedPrimitive &decoded = mesh.Primitives.AddDefaulted();
            if (!ReadPrimitive(data, primitive, decoded))
            {
                FormatTo(error, "primitive {0} of mesh {1} has inconsistent attributes", p, m);
                return false;
            }
        }
    }
    // RECOVERY-DECISION DEC-D4: primitives without a material share one material named "Default", added only when
    // used.
    if (needsFallback)
    {
        scene.Materials.AddDefaulted().Name = "Default";
    }

    // The default scene's nodes, or the first scene's; a file without scenes places its root nodes.
    const cgltf_scene *placed = data.scene != nullptr ? data.scene : data.scenes_count > 0 ? data.scenes : nullptr;
    if (placed != nullptr)
    {
        for (cgltf_size n = 0; n < placed->nodes_count; ++n)
        {
            AddNode(data, *placed->nodes[n], -1, scene);
        }
    }
    else
    {
        for (cgltf_size n = 0; n < data.nodes_count; ++n)
        {
            if (data.nodes[n].parent == nullptr)
            {
                AddNode(data, data.nodes[n], -1, scene);
            }
        }
    }
    return true;
}
} // namespace

bool GltfSceneDecoder::Decode(Span<const uint8> bytes, ISourceFileReader &files, StringView mesh,
                              DecodedScene &scene, StringWriter &error) const
{
    return DecodeGltf(bytes, files, scene, error,
                      [mesh](StringView name) { return mesh.IsEmpty() || name == mesh; });
}

bool GltfSceneDecoder::DecodeOutline(Span<const uint8> bytes, ISourceFileReader &files, DecodedScene &scene,
                                     StringWriter &error) const
{
    return DecodeGltf(bytes, files, scene, error, [](StringView /*name*/) { return false; });
}
} // namespace Marlin
