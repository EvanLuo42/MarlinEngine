#include "MeshPipeline/MeshImporter.h"

#include "Containers/HashSet.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Math/MathUtility.h"
#include "MeshPipeline/MeshConverter.h"
#include "RenderComponents/RenderComponents.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "World/Hierarchy.h"

namespace Marlin
{
namespace
{
// RECOVERY-DECISION TM-D19: the combined mesh, the prefab and its entities take sub-paths that start with a colon,
// which no name a scene gives its meshes does, so they never meet the per-mesh sub-paths.
constexpr StringView CombinedSubPath = ":Combined";
constexpr StringView PrefabSubPath = ":Prefab";

/// *transform*, a node's affine transform, as translation, rotation and scale; shear is lost. A mirroring transform
/// becomes a negative X scale.
LocalTransform Decompose(const Matrix4x4 &transform)
{
    const Vector4 *rows = transform.Rows;
    Vector3 axes[3];
    float32 scale[3];
    for (uint32 c = 0; c < 3; ++c)
    {
        axes[c] = Vector3(rows[0][c], rows[1][c], rows[2][c]);
        scale[c] = axes[c].Length();
        axes[c] = scale[c] > 0 ? axes[c] / scale[c] : Vector3();
    }
    if (Vector3::Dot(Vector3::Cross(axes[0], axes[1]), axes[2]) < 0)
    {
        scale[0] = -scale[0];
        axes[0] = -axes[0];
    }
    // The rotation matrix, columns axes, as a quaternion.
    const float32 m00 = axes[0].X, m11 = axes[1].Y, m22 = axes[2].Z;
    const float32 trace = m00 + m11 + m22;
    Quaternion rotation;
    if (trace > 0)
    {
        const float32 s = Math::Sqrt(trace + 1) * 2;
        rotation =
            Quaternion((axes[1].Z - axes[2].Y) / s, (axes[2].X - axes[0].Z) / s, (axes[0].Y - axes[1].X) / s, s / 4);
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float32 s = Math::Sqrt(1 + m00 - m11 - m22) * 2;
        rotation =
            Quaternion(s / 4, (axes[1].X + axes[0].Y) / s, (axes[2].X + axes[0].Z) / s, (axes[1].Z - axes[2].Y) / s);
    }
    else if (m11 > m22)
    {
        const float32 s = Math::Sqrt(1 + m11 - m00 - m22) * 2;
        rotation =
            Quaternion((axes[1].X + axes[0].Y) / s, s / 4, (axes[2].Y + axes[1].Z) / s, (axes[2].X - axes[0].Z) / s);
    }
    else
    {
        const float32 s = Math::Sqrt(1 + m22 - m00 - m11) * 2;
        rotation =
            Quaternion((axes[2].X + axes[0].Z) / s, (axes[2].Y + axes[1].Z) / s, s / 4, (axes[0].Y - axes[1].X) / s);
    }
    LocalTransform local;
    local.Translation = WorldPosition(rows[0].W, rows[1].W, rows[2].W);
    local.Rotation = rotation.Normalized();
    local.Scale = Vector3(scale[0], scale[1], scale[2]);
    return local;
}

String NodeSubPath(StringView part)
{
    return Format(":Node/{0}", part);
}
} // namespace

void GetScenePartNames(const DecodedScene &scene, List<String> &names)
{
    names.Clear();
    HashSet<String> taken;
    for (usize n = 0; n < scene.Nodes.Count(); ++n)
    {
        const String base = scene.Nodes[n].Name.IsEmpty() ? Format("Node{0}", n) : scene.Nodes[n].Name;
        String name = base;
        for (uint32 suffix = 2; !taken.Add(name); ++suffix)
        {
            name = Format("{0}_{1}", base, suffix);
        }
        names.Add(Move(name));
    }
}

bool MeshImporter::CanImport(const ImportContext &context) const
{
    return !context.GetSceneDecoders().Find(context.GetExtension()).IsEmpty();
}

bool MeshImporter::Import(ImportContext &context) const
{
    // Only the names and the nodes are needed here; the geometry is the converter's business.
    DecodedScene scene;
    if (!context.DecodeOutline(scene))
    {
        return false;
    }
    if (scene.Meshes.IsEmpty())
    {
        context.Error("the scene has no meshes");
        return false;
    }
    const MeshImportOptions &options = context.GetOptions<MeshImportOptions>();
    if (options.Combine)
    {
        MeshSource source;
        source.SourceFile = context.GetSourceLink();
        context.AddObject(CombinedSubPath, context.GetBaseName(), source);
        return true;
    }

    List<Guid> meshes;
    for (usize index = 0; index < scene.Meshes.Count(); ++index)
    {
        // RECOVERY-DECISION TM-D11: the mesh's name is its sub-path (its index if it has none), so reordering the
        // file keeps every UUID; even a lone mesh is converted by name, so adding a second one later keeps it too.
        const String &name = scene.Meshes[index].Name;
        const String subPath = name.IsEmpty() ? Format("{0}", index) : name;
        MeshSource source;
        source.SourceFile = context.GetSourceLink();
        source.Mesh = name;
        const String objectName = scene.Meshes.Count() == 1 ? String(context.GetBaseName())
                                                            : Format("{0}_{1}", context.GetBaseName(), subPath);
        context.AddObject(subPath.AsView(), objectName.AsView(), source);
        meshes.Add(context.GetUuid(subPath.AsView()));
    }

    // The prefab: an entity per node, in the file's hierarchy, those that place a mesh with a RenderMesh of it. Each
    // entity's sub-path is its part name, so reimporting a changed file keeps the entities it already has.
    List<String> parts;
    GetScenePartNames(scene, parts);
    StringBuilder<1024> members;
    for (usize n = 0; n < scene.Nodes.Count(); ++n)
    {
        const DecodedNode &node = scene.Nodes[n];
        const String subPath = NodeSubPath(parts[n].AsView());
        StringBuilder<1024> text;
        text.Append(Format("PrefabEntity\n{{\n    !UUID = \"{0}\"\n    !Name = \"{1}\"\n",
                           context.GetUuid(subPath.AsView()), parts[n])
                        .AsView());
        if (node.Parent >= 0 && static_cast<usize>(node.Parent) < scene.Nodes.Count())
        {
            const String parent = NodeSubPath(parts[static_cast<usize>(node.Parent)].AsView());
            text.Append(Format("    Parent = <#{0}>\n", context.GetUuid(parent.AsView())).AsView());
        }
        const LocalTransform local = Decompose(node.Transform);
        text.Append("    Components =\n    {\n        LocalTransform = ");
        MarlinTextSerializer::WriteValue(text, TypeOf<LocalTransform>(), &local, 2);
        text.Append('\n');
        if (node.Mesh >= 0 && static_cast<usize>(node.Mesh) < meshes.Count())
        {
            text.Append(
                Format("        RenderMesh = {{ Mesh = <#{0}> }}\n", meshes[static_cast<usize>(node.Mesh)]).AsView());
        }
        text.Append("    }\n}\n");
        context.AddObjectText(subPath.AsView(), text.AsView());
        members.Append(members.IsEmpty() ? "" : ", ")
            .Append(Format("<#{0}>", context.GetUuid(subPath.AsView())).AsView());
    }
    const String prefab = Format("Prefab\n{{\n    !UUID = \"{0}\"\n    !Name = \"{1}\"\n    Members = [{2}]\n}}\n",
                                 context.GetUuid(PrefabSubPath), context.GetBaseName(), members.AsView());
    context.AddObjectText(PrefabSubPath, prefab.AsView());
    return true;
}
} // namespace Marlin
