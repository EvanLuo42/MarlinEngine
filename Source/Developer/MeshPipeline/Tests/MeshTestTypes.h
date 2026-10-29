#pragma once

#include "AssetPipeline/SourceDecoder.h"
#include "Math/MathUtility.h"
#include "MeshTestTypes.gen.h"

namespace Marlin::Tests
{
/// Decodes any `.testscene` file into the same scene: a UV sphere of radius 1 in two materials, placed by a node
/// moved 2 m along x, a quad without normals or UVs under it, and a box without normals that nothing places.
MCLASS()
class TestSceneDecoder final : public SceneDecoder
{
    MGENERATED_BODY()
public:
    // RECOVERY-DECISION TM-D8: 16128 triangles, enough for a DAG at least three deep.
    uint32 Rings = 64;
    uint32 Segments = 128;

    [[nodiscard]] Span<const StringView> GetExtensions() const override
    {
        static constexpr StringView Extensions[] = {"testscene"};
        return Extensions;
    }

    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }

    [[nodiscard]] bool CanDecode(Span<const uint8> /*header*/) const override
    {
        return true;
    }

    bool Decode(Span<const uint8> /*bytes*/, ISourceFileReader & /*files*/, StringView /*mesh*/, DecodedScene &scene,
                StringWriter & /*error*/) const override
    {
        scene.Nodes.Clear();
        scene.Meshes.Clear();
        scene.Materials.Clear();
        scene.Materials.AddDefaulted().Name = "Rock";
        scene.Materials.AddDefaulted().Name = "Moss";
        // The sphere: the upper half is Rock, the lower Moss. The pole rows only keep the triangle that is not
        // degenerate, so there are 2 * Segments * (Rings - 1) triangles.
        DecodedMesh &sphere = scene.Meshes.AddDefaulted();
        sphere.Name = "Sphere";
        const auto at = [this](uint32 ring, uint32 segment) { return ring * (Segments + 1) + segment; };
        for (uint32 half = 0; half < 2; ++half)
        {
            DecodedPrimitive &primitive = sphere.Primitives.AddDefaulted();
            primitive.Material = half;
            primitive.TexCoords.AddDefaulted();
            for (uint32 ring = 0; ring <= Rings; ++ring)
            {
                const float32 theta = Math::Pi * static_cast<float32>(ring) / static_cast<float32>(Rings);
                for (uint32 segment = 0; segment <= Segments; ++segment)
                {
                    const float32 phi = Math::TwoPi * static_cast<float32>(segment) / static_cast<float32>(Segments);
                    const Vector3 position(Math::Sin(theta) * Math::Cos(phi), Math::Cos(theta),
                                           Math::Sin(theta) * Math::Sin(phi));
                    primitive.Positions.Add(position);
                    primitive.Normals.Add(position);
                    primitive.TexCoords[0].Add(Vector2(static_cast<float32>(segment) / static_cast<float32>(Segments),
                                                       static_cast<float32>(ring) / static_cast<float32>(Rings)));
                }
            }
            for (uint32 ring = half * Rings / 2; ring < (half + 1) * Rings / 2; ++ring)
            {
                for (uint32 segment = 0; segment < Segments; ++segment)
                {
                    if (ring != 0)
                    {
                        primitive.Indices.AddRange({at(ring, segment), at(ring, segment + 1), at(ring + 1, segment)});
                    }
                    if (ring != Rings - 1)
                    {
                        primitive.Indices.AddRange(
                            {at(ring, segment + 1), at(ring + 1, segment + 1), at(ring + 1, segment)});
                    }
                }
            }
        }
        // The quad, facing up, in Rock.
        DecodedMesh &quad = scene.Meshes.AddDefaulted();
        quad.Name = "Quad";
        DecodedPrimitive &primitive = quad.Primitives.AddDefaulted();
        primitive.Positions.AddRange({Vector3(-1, 0, -1), Vector3(1, 0, -1), Vector3(1, 0, 1), Vector3(-1, 0, 1)});
        primitive.Indices.AddRange({0, 3, 2, 0, 2, 1});
        // A box of eight shared corners and no normals, which no node places: its edges are hard, so each corner
        // becomes three vertices.
        DecodedMesh &box = scene.Meshes.AddDefaulted();
        box.Name = "Box";
        DecodedPrimitive &faces = box.Primitives.AddDefaulted();
        for (uint32 corner = 0; corner < 8; ++corner)
        {
            faces.Positions.Add(Vector3((corner & 1) != 0 ? 1.0f : -1.0f, (corner & 2) != 0 ? 1.0f : -1.0f,
                                        (corner & 4) != 0 ? 1.0f : -1.0f));
        }
        faces.Indices.AddRange({0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                                2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5});

        DecodedNode &rocks = scene.Nodes.AddDefaulted();
        rocks.Name = "Rocks";
        rocks.Mesh = 0;
        rocks.Transform.Rows[0].W = 2;
        DecodedNode &floor = scene.Nodes.AddDefaulted();
        floor.Name = "Floor";
        floor.Parent = 0;
        floor.Mesh = 1;
        floor.Transform.Rows[1].W = -1.5f;
        // A pivot that places nothing: turned a quarter about Y, twice as large, 3 m along Z.
        DecodedNode &pivot = scene.Nodes.AddDefaulted();
        pivot.Name = "Pivot";
        pivot.Transform.Rows[0] = Vector4(0, 0, 2, 0);
        pivot.Transform.Rows[1] = Vector4(0, 2, 0, 0);
        pivot.Transform.Rows[2] = Vector4(-2, 0, 0, 3);
        return true;
    }
};
} // namespace Marlin::Tests
