#include "Common.h"

#include <cmath>

// The vertex's storage formats, and the one place a packed format was measured
// and rejected.
//
// Every other attribute is packed because its range is known: a normal is a
// direction, a colour is [0, 1]. A UV's range is not known - glTF tiles a
// texture by handing out coordinates well outside [0, 1] - and that is what
// makes it the attribute a packed format gets wrong.

using namespace nano;
using namespace eacp;
using namespace eacp::Mesh;
using namespace eacp::Mesh::Tests;

namespace
{
// One texel of a 1024-wide texture, as a fraction of a tile. The unit every
// tolerance here is stated in, because it is the unit the error is visible in.
constexpr auto texel = 1.0f / 1024.0f;

float halfErrorAt(float uv)
{
    return std::fabs(GPU::halfToFloat(GPU::halfFromFloat(uv)) - uv);
}

// A quad whose UVs tile the texture forty times over, which is what an
// architectural floor or a terrain patch arrives as.
std::string tiledQuad(BinaryBuffer& binary)
{
    auto positionOffset = binary.appendFloats({0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0});

    // Deliberately not multiples of anything: a UV that happens to land on a
    // representable half proves nothing about the format.
    auto uvOffset = binary.appendFloats(
        {0.0f, 0.0f, 40.02f, 0.0f, 0.0f, 24.51f, 40.02f, 24.51f});

    auto indexOffset = binary.appendIndices({0, 1, 2, 1, 3, 2});

    return std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes":
            {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"}
           + std::to_string(positionOffset) + R"(, "byteLength": 48},
            {"buffer": 0, "byteOffset": )"
           + std::to_string(uvOffset) + R"(, "byteLength": 32},
            {"buffer": 0, "byteOffset": )"
           + std::to_string(indexOffset) + R"(, "byteLength": 12}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"}
        ]
    })";
}
} // namespace

// The case the vertex format exists to pass. A UV that tiles has to survive the
// load to better than a texel, exactly as one inside [0, 1] does - the
// coordinate is larger but the texture it addresses is not.
auto tTiledUvKeepsTexelPrecision = test("MeshVertex/tiledUvKeepsTexelPrecision") = []
{
    auto binary = BinaryBuffer {};
    auto loaded = loadDocument(tiledQuad(binary), binary);

    check(bool(loaded));

    if (!hasVertices(loaded, 4))
        return;

    check(near(loaded.data.vertices[1].uv[0], 40.02f, texel));
    check(near(loaded.data.vertices[2].uv[1], 24.51f, texel));
    check(near(loaded.data.vertices[3].uv[0], 40.02f, texel));
    check(near(loaded.data.vertices[3].uv[1], 24.51f, texel));
};

// Why that case needs a Float2, stated as the measurement rather than as a
// comment - so "it is only a UV, pack it" is a change that fails a test.
//
// Half carries eleven bits of mantissa wherever it sits, so its absolute error
// doubles with every octave while the texel it has to resolve stays the same
// size. Inside one tile that is a quarter of a texel; at the coordinate a tiled
// floor reaches it is sixteen of them, which is a visible seam between two
// triangles that should meet.
auto tHalfCannotHoldATiledUv = test("MeshVertex/halfCannotHoldATiledUv") = []
{
    // Inside a single tile it is comfortable, which is what made this easy to
    // miss: every hand-authored test and the whole sample scene lives here.
    check(halfErrorAt(0.7371f) < 0.5f * texel);

    // It is still fine a couple of octaves up, so the format is not simply
    // unusable - it runs out, and this is roughly where.
    check(halfErrorAt(3.7371f) < texel);

    // And by the time a floor tiles forty times it is out by sixteen texels.
    check(halfErrorAt(40.02f) > 10.0f * texel);
    check(halfErrorAt(24.51f) > 4.0f * texel);
};

// The attributes that stay packed, and the precision each one actually has.
// Stated here rather than inferred from a picture, because a format mapped to
// the wrong width still draws.
auto tPackedAttributesKeepTheirPrecision =
    test("MeshVertex/packedAttributesKeepTheirPrecision") = []
{
    // A normal is a direction, so a signed normalized short spends its whole
    // range on [-1, 1] and holds one to about 3e-5.
    auto normal = GPU::SNorm16x4::from(0.0f, -1.0f, 0.5f, 0.0f);

    check(near((float) normal.values[1] / 32767.0f, -1.0f, 1.0e-4f));
    check(near((float) normal.values[2] / 32767.0f, 0.5f, 1.0e-4f));

    // A colour is [0, 1] in eight bits, which is what COLOR_0 already was.
    auto color = GPU::UNorm8x4::fromFloats(1.0f, 0.5f, 0.0f, 1.0f);

    check(color.values[0] == 255);
    check(color.values[1] == 127 || color.values[1] == 128);
    check(color.values[3] == 255);
};
