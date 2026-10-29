#include "Testing/Testing.h"

#include "FbxSceneDecoder/FbxSceneDecoder.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
/// A unit quad facing up in an ASCII FBX that is Y up and in centimetres (UnitScaleFactor 1), without normals, UVs
/// or materials.
constexpr StringView QuadFbx = R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
Objects:  {
	Geometry: 1000, "Geometry::Quad", "Mesh" {
		Vertices: *12 {
			a: 0,0,0,1,0,0,1,0,-1,0,0,-1
		}
		PolygonVertexIndex: *4 {
			a: 0,1,2,-4
		}
		GeometryVersion: 124
	}
	Model: 2000, "Model::Floor", "Mesh" {
		Version: 232
	}
}
Connections:  {
	C: "OO",1000,2000
	C: "OO",2000,0
}
)";
/// The quad names no other files.
class NoFiles final : public ISourceFileReader
{
public:
    bool ReadReferencedRange(StringView /*uri*/, uint64 /*offset*/, uint64 /*size*/, List<uint8> & /*bytes*/,
                             StringWriter & /*error*/) override
    {
        return false;
    }
};
} // namespace

MARLIN_TEST(FbxSceneDecoder, AsciiQuad)
{
    const FbxSceneDecoder decoder;
    const Span<const uint8> bytes(reinterpret_cast<const uint8 *>(QuadFbx.Data()), QuadFbx.Length());
    MARLIN_REQUIRE(decoder.CanDecode(bytes));
    DecodedScene scene;
    StringBuilder<256> error;
    NoFiles files;
    MARLIN_REQUIRE(decoder.Decode(bytes, files, {}, scene, error));
    MARLIN_REQUIRE(scene.Nodes.Count() == 1);
    MARLIN_EXPECT(scene.Nodes[0].Name == "Floor" && scene.Nodes[0].Parent == -1 && scene.Nodes[0].Mesh == 0);
    MARLIN_REQUIRE(scene.Meshes.Count() == 1 && scene.Meshes[0].Primitives.Count() == 1);
    MARLIN_EXPECT(scene.Meshes[0].Name == "Quad");
    MARLIN_EXPECT(!scene.Materials.IsEmpty());
    // Two triangles, one vertex per corner, and the centimetres became metres.
    const DecodedPrimitive &primitive = scene.Meshes[0].Primitives[0];
    MARLIN_REQUIRE(primitive.Indices.Count() == 6 && primitive.Positions.Count() == 6);
    float32 minZ = 0;
    for (const Vector3 &position : primitive.Positions)
    {
        minZ = position.Z < minZ ? position.Z : minZ;
    }
    MARLIN_EXPECT(minZ < -0.0099f && minZ > -0.0101f);
    DecodedScene broken;
    MARLIN_EXPECT(!decoder.Decode(bytes.Slice(0, 40), files, {}, broken, error));
}
} // namespace Marlin::Tests
