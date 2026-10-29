#include "FbxSceneDecoder/FbxSceneDecoder.h"

#include "String/Format.h"
#include "String/StringBuilder.h"
#include "ufbx.h"

namespace Marlin
{
namespace
{
/// What ReadMesh records for a part without a material; Decode points it at a fallback material.
constexpr uint32 NoMaterial = ~0u;

String ToString(const ufbx_string &text)
{
    return {text.data, text.length};
}

Matrix4x4 ToMatrix(const ufbx_matrix &m)
{
    const auto f = [](ufbx_real value) { return static_cast<float32>(value); };
    return {Vector4(f(m.m00), f(m.m01), f(m.m02), f(m.m03)), Vector4(f(m.m10), f(m.m11), f(m.m12), f(m.m13)),
            Vector4(f(m.m20), f(m.m21), f(m.m22), f(m.m23)), Vector4(0, 0, 0, 1)};
}

Vector3 ToVector(const ufbx_vec3 &v)
{
    return {static_cast<float32>(v.x), static_cast<float32>(v.y), static_cast<float32>(v.z)};
}

/// One primitive per material part, with one vertex per triangle corner; MeshConverter welds them.
void ReadMesh(const ufbx_mesh &source, DecodedMesh &mesh)
{
    const auto at = [](const auto &attribute, uint32 index) {
        return attribute.values.data[attribute.indices.data[index]];
    };
    const bool hasNormals = source.vertex_normal.exists;
    const bool hasTangents = hasNormals && source.vertex_tangent.exists && source.vertex_bitangent.exists;
    List<uint32> corners;
    corners.Resize(source.max_face_triangles * 3);
    for (usize p = 0; p < source.material_parts.count; ++p)
    {
        const ufbx_mesh_part &part = source.material_parts.data[p];
        if (part.num_triangles == 0)
        {
            continue;
        }
        DecodedPrimitive &primitive = mesh.Primitives.AddDefaulted();
        const ufbx_material *material =
            part.index < source.materials.count ? source.materials.data[part.index] : nullptr;
        primitive.Material = material != nullptr ? material->typed_id : NoMaterial;
        primitive.TexCoords.Resize(source.uv_sets.count);
        for (usize f = 0; f < part.face_indices.count; ++f)
        {
            const ufbx_face face = source.faces.data[part.face_indices.data[f]];
            const uint32 triangles = ufbx_triangulate_face(corners.Data(), corners.Count(), &source, face);
            for (uint32 c = 0; c < triangles * 3; ++c)
            {
                const uint32 index = corners[c];
                primitive.Indices.Add(static_cast<uint32>(primitive.Positions.Count()));
                primitive.Positions.Add(ToVector(at(source.vertex_position, index)));
                if (hasNormals)
                {
                    primitive.Normals.Add(ToVector(at(source.vertex_normal, index)));
                }
                if (hasTangents)
                {
                    // RECOVERY-DECISION DEC-D1: flipping V reverses the bitangent, so the sign is the opposite of
                    // the file's handedness of normal, tangent and bitangent.
                    const Vector3 n = ToVector(at(source.vertex_normal, index));
                    const Vector3 t = ToVector(at(source.vertex_tangent, index));
                    const Vector3 b = ToVector(at(source.vertex_bitangent, index));
                    const float32 handedness = (n.Y * t.Z - n.Z * t.Y) * b.X + (n.Z * t.X - n.X * t.Z) * b.Y +
                                               (n.X * t.Y - n.Y * t.X) * b.Z;
                    primitive.Tangents.Add(Vector4(t, handedness < 0 ? 1.0f : -1.0f));
                }
                for (usize s = 0; s < source.uv_sets.count; ++s)
                {
                    const ufbx_vec2 uv = at(source.uv_sets.data[s].vertex_uv, index);
                    primitive.TexCoords[s].Add(Vector2(static_cast<float32>(uv.x), 1.0f - static_cast<float32>(uv.y)));
                }
                if (source.vertex_color.exists)
                {
                    const ufbx_vec4 color = at(source.vertex_color, index);
                    primitive.Colors.Add(Vector4(static_cast<float32>(color.x), static_cast<float32>(color.y),
                                                 static_cast<float32>(color.z), static_cast<float32>(color.w)));
                }
            }
        }
    }
}

void AddNode(const ufbx_node &node, int32 parent, DecodedScene &scene)
{
    const auto index = static_cast<int32>(scene.Nodes.Count());
    DecodedNode &added = scene.Nodes.AddDefaulted();
    added.Name = ToString(node.name);
    added.Parent = parent;
    added.Transform = ToMatrix(node.node_to_parent);
    added.Mesh = node.mesh != nullptr ? static_cast<int32>(node.mesh->typed_id) : -1;
    for (usize child = 0; child < node.children.count; ++child)
    {
        AddNode(*node.children.data[child], index, scene);
    }
}
} // namespace

Span<const StringView> FbxSceneDecoder::GetExtensions() const
{
    static constexpr StringView Extensions[] = {"fbx"};
    return Extensions;
}

bool FbxSceneDecoder::CanDecode(Span<const uint8> header) const
{
    // RECOVERY-DECISION DEC-D2: the binary signature, or ASCII text that opens with the FBX header comment or node.
    const StringView text(reinterpret_cast<const char *>(header.Data()), header.Count());
    return text.StartsWith("Kaydara FBX Binary") || text.TrimStart().StartsWith("; FBX") ||
           text.Left(1024).Contains("FBXHeaderExtension");
}

namespace
{
/// What Decode and DecodeOutline do; without *geometry* ufbx skips the vertices and meshes get no primitives.
bool DecodeFbx(Span<const uint8> bytes, DecodedScene &scene, StringWriter &error, bool geometry)
{
    scene.Nodes.Clear();
    scene.Meshes.Clear();
    scene.Materials.Clear();

    // RECOVERY-DECISION DEC-D3: the conversion to engine space modifies the geometry, so no node outside the decoded
    // hierarchy (the root) carries any of it, and geometric transforms are baked the same way.
    ufbx_load_opts options = {};
    options.file_format = UFBX_FILE_FORMAT_FBX;
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1;
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    options.ignore_geometry = !geometry;
    ufbx_error failure;
    ufbx_scene *source = ufbx_load_memory(bytes.Data(), bytes.Count(), &options, &failure);
    if (source == nullptr)
    {
        error.Append(StringView(failure.description.data, failure.description.length));
        return false;
    }

    for (usize m = 0; m < source->materials.count; ++m)
    {
        const ufbx_material &material = *source->materials.data[m];
        scene.Materials.AddDefaulted().Name =
            material.name.length > 0 ? ToString(material.name) : Format("Material{0}", m);
    }
    bool needsFallback = false;
    for (usize m = 0; m < source->meshes.count; ++m)
    {
        const ufbx_mesh &sourceMesh = *source->meshes.data[m];
        DecodedMesh &mesh = scene.Meshes.AddDefaulted();
        if (geometry)
        {
            ReadMesh(sourceMesh, mesh);
        }
        mesh.Name = ToString(sourceMesh.name);
        if (mesh.Name.IsEmpty())
        {
            mesh.Name = Format("Mesh{0}", m);
        }
        for (const DecodedPrimitive &primitive : mesh.Primitives)
        {
            needsFallback = needsFallback || primitive.Material == NoMaterial;
        }
    }
    // RECOVERY-DECISION DEC-D4: parts without a material share one material named "Default", added only when used.
    if (needsFallback)
    {
        const auto fallback = static_cast<uint32>(scene.Materials.Count());
        scene.Materials.AddDefaulted().Name = "Default";
        for (DecodedMesh &mesh : scene.Meshes)
        {
            for (DecodedPrimitive &primitive : mesh.Primitives)
            {
                primitive.Material = primitive.Material == NoMaterial ? fallback : primitive.Material;
            }
        }
    }

    // The root node is ufbx's, not the file's: its children are the scene's top-level nodes.
    for (usize child = 0; child < source->root_node->children.count; ++child)
    {
        AddNode(*source->root_node->children.data[child], -1, scene);
    }
    ufbx_free_scene(source);
    return true;
}
} // namespace

bool FbxSceneDecoder::Decode(Span<const uint8> bytes, ISourceFileReader & /*files*/, StringView /*mesh*/,
                             DecodedScene &scene, StringWriter &error) const
{
    return DecodeFbx(bytes, scene, error, true);
}

bool FbxSceneDecoder::DecodeOutline(Span<const uint8> bytes, ISourceFileReader & /*files*/, DecodedScene &scene,
                                    StringWriter &error) const
{
    return DecodeFbx(bytes, scene, error, false);
}
} // namespace Marlin
