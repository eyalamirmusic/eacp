#include "Common.h"

// The loader, checked against glTF documents authored in the test.
//
// None of this needs a GPU, which is the payoff for MeshData being
// format-agnostic: node composition, index packing, material defaults and the
// packed vertex are all properties of a struct, and they are where the silent
// errors live. A wrong material default renders black, a rebased index renders
// correctly until a model passes 65536 vertices, and a dropped node renders a
// model missing a part nobody notices is missing.

using namespace nano;
using namespace eacp;
using namespace eacp::Mesh;
using namespace eacp::Mesh::Tests;

namespace
{
// One triangle, positions only. The smallest document that loads.
LoadResult loadTriangle()
{
    auto binary = BinaryBuffer {};
    binary.appendFloats({0, 0, 0, 1, 0, 0, 0, 1, 0});
    binary.appendIndices({0, 1, 2});

    return loadDocument(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
             "min": [0, 0, 0], "max": [1, 1, 0]},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })",
                        binary);
}
} // namespace

auto tLoadsATriangle = test("Gltf/loadsATriangle") = []
{
    auto loaded = loadTriangle();

    check(bool(loaded));
    check(loaded.data.vertices.size() == 3);
    check(loaded.data.indices.size() == 3);
    check(loaded.data.primitives.size() == 1);
    check(loaded.data.nodes.size() == 1);
    check(loaded.data.drawOrder.size() == 1);
};

auto tMalformedInputReportsAnError = test("Gltf/malformedInputReportsAnError") = []
{
    const char* garbage = "this is not a glTF document";
    auto loaded = loadGltfFromMemory(garbage, std::strlen(garbage));

    check(!bool(loaded));
    check(!loaded.error.empty());
    check(!loaded.data.isValid());
};

// The single most likely thing to be silently wrong in the whole module: a
// child's world transform is its parent's composed with its own, and a
// convention error here still draws a model - just one whose parts are in the
// wrong places.
//
// The parent rotates and the child translates, which is deliberate: two
// translations *commute*, so a tree built from them alone cannot tell
// parent * child from child * parent. The first version of this case used two
// translations and passed with the multiplication reversed.
auto tNodeTransformsComposeDownTheTree =
    test("Gltf/nodeTransformsComposeDownTheTree") = []
{
    auto binary = BinaryBuffer {};
    binary.appendFloats({0, 0, 0, 1, 0, 0, 0, 1, 0});
    binary.appendIndices({0, 1, 2});

    // The parent turns a quarter turn about z; the child is pushed 10 along x.
    // Composed the right way the child ends up on +y; composed the wrong way it
    // stays on +x, because rotating the origin does nothing.
    auto loaded = loadDocument(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"rotation": [0, 0, 0.7071068, 0.7071068], "children": [1]},
            {"translation": [10, 0, 0], "mesh": 0}
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })",
                               binary);

    check(bool(loaded));
    check(loaded.data.nodes.size() == 2);

    if (!hasNodes(loaded, 2))
        return;

    auto origin = loaded.data.nodes[1].worldTransform.transformPoint({0, 0, 0});
    check(near(origin, Vec3 {0.0f, 10.0f, 0.0f}, 1.0e-4f));

    // And the parent's own transform is untouched by its child's, which is the
    // other way a composition goes wrong.
    auto parentOrigin =
        loaded.data.nodes[0].worldTransform.transformPoint({0, 0, 0});
    check(near(parentOrigin, Vec3 {0.0f, 0.0f, 0.0f}, 1.0e-5f));

    // The parent really does rotate, so the case above is testing composition
    // rather than passing because the rotation was dropped.
    auto parentX = loaded.data.nodes[0].worldTransform.transformDirection({1, 0, 0});
    check(near(parentX, Vec3 {0.0f, 1.0f, 0.0f}, 1.0e-4f));
};

// glTF lets a node give either a matrix or a TRS triple. A file written both
// ways has to produce the same transform, or a model loads correctly from one
// exporter and wrongly from another.
auto tMatrixAndTrsAgree = test("Gltf/matrixAndTrsAgree") = []
{
    auto binary = BinaryBuffer {};
    binary.appendFloats({0, 0, 0, 1, 0, 0, 0, 1, 0});
    binary.appendIndices({0, 1, 2});

    // Node 0 is a scale of 2 then a translation of (1,2,3), written as TRS.
    // Node 1 is the same thing written as the column-major matrix glTF wants.
    auto loaded = loadDocument(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"translation": [1, 2, 3], "scale": [2, 2, 2], "mesh": 0},
            {"matrix": [2, 0, 0, 0,
                        0, 2, 0, 0,
                        0, 0, 2, 0,
                        1, 2, 3, 1], "mesh": 0}
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })",
                               binary);

    check(bool(loaded));
    check(loaded.data.nodes.size() == 2);

    if (!hasNodes(loaded, 2))
        return;

    for (auto i = 0; i < 16; ++i)
        check(near(loaded.data.nodes[0].worldTransform.values[i],
                   loaded.data.nodes[1].worldTransform.values[i],
                   1.0e-5f));

    // Against an absolute answer as well, so two identically wrong readings
    // cannot pass by agreeing with each other.
    auto moved = loaded.data.nodes[0].worldTransform.transformPoint({1, 0, 0});
    check(near(moved, Vec3 {3.0f, 2.0f, 3.0f}, 1.0e-5f));
};

// What Phase 1 of the GPU plan bought, asserted on the loader's output: two
// primitives share one vertex buffer, the second gets a base vertex, and its
// index values still start from zero.
//
// A loader that "helpfully" rebased into the shared buffer would pass every
// visual check and break silently on the first model past 65536 vertices.
auto tPrimitivesKeepZeroBasedIndices =
    test("Gltf/primitivesKeepZeroBasedIndices") = []
{
    auto binary = BinaryBuffer {};
    binary.appendFloats({0, 0, 0, 1, 0, 0, 0, 1, 0});
    binary.appendIndices({0, 1, 2});

    auto loaded = loadDocument(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [
            {"attributes": {"POSITION": 0}, "indices": 1},
            {"attributes": {"POSITION": 0}, "indices": 1}
        ]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })",
                               binary);

    check(bool(loaded));
    check(loaded.data.primitives.size() == 2);
    check(loaded.data.vertices.size() == 6);

    if (!hasPrimitives(loaded, 2))
        return;

    const auto& first = loaded.data.primitives[0];
    const auto& second = loaded.data.primitives[1];

    check(first.baseVertex == 0);
    check(second.baseVertex == 3);
    check(second.firstIndex == 3);

    // The values themselves: the second primitive indexes 0..2, not 3..5.
    for (auto i = 0; i < second.indexCount; ++i)
        check(loaded.data.indices[second.firstIndex + i] < 3u);

    check(fitsNarrowIndices(loaded.data));
};

// glTF's material defaults, not C++'s. A zero-initialised base colour is
// transparent black, which renders the whole model invisible and reads as a
// loader that never ran.
auto tMaterialDefaultsAreOpaqueWhite =
    test("Gltf/materialDefaultsAreOpaqueWhite") = []
{
    auto binary = BinaryBuffer {};
    binary.appendFloats({0, 0, 0, 1, 0, 0, 0, 1, 0});
    binary.appendIndices({0, 1, 2});

    // One material that says nothing at all, and a primitive naming no material.
    auto loaded = loadDocument(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "materials": [{"name": "says nothing"}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })",
                               binary);

    check(bool(loaded));
    check(loaded.data.materials.size() == 1);

    if (!hasMaterials(loaded, 1) || !hasPrimitives(loaded, 1))
        return;

    const auto& material = loaded.data.materials[0];

    for (auto c = 0; c < 4; ++c)
        check(near(material.baseColor[c], 1.0f, 1.0e-6f));

    check(material.baseColorImage == -1);
    check(material.alphaMode == AlphaMode::Opaque);
    check(!material.doubleSided);

    // A primitive naming no material gets -1 rather than 0, so it takes the
    // default material rather than whichever one the file happened to list
    // first.
    check(loaded.data.primitives[0].material == -1);
};

auto tAlphaModesAreRead = test("Gltf/alphaModesAreRead") = []
{
    auto binary = BinaryBuffer {};
    binary.appendFloats({0, 0, 0, 1, 0, 0, 0, 1, 0});
    binary.appendIndices({0, 1, 2});

    auto loaded = loadDocument(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "materials": [
            {"alphaMode": "MASK", "alphaCutoff": 0.25, "doubleSided": true},
            {"alphaMode": "BLEND",
             "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 0.5]}}
        ],
        "meshes": [{"primitives": [
            {"attributes": {"POSITION": 0}, "indices": 1, "material": 0}
        ]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })",
                               binary);

    check(bool(loaded));
    check(loaded.data.materials.size() == 2);

    if (!hasMaterials(loaded, 2) || !hasPrimitives(loaded, 1))
        return;

    check(loaded.data.materials[0].alphaMode == AlphaMode::Mask);
    check(near(loaded.data.materials[0].alphaCutoff, 0.25f, 1.0e-6f));
    check(loaded.data.materials[0].doubleSided);

    check(loaded.data.materials[1].alphaMode == AlphaMode::Blend);
    check(near(loaded.data.materials[1].baseColor[3], 0.5f, 1.0e-6f));

    // The primitive names material 0 and gets it.
    check(loaded.data.primitives[0].material == 0);
};

// A file with no NORMAL still has to shade. These are smooth rather than the
// flat normals the spec asks for - see the note in GltfLoader - so what is
// asserted is that they are unit length and point outward, which is what the
// shading needs and what a missing generator would fail.
auto tNormalsAreGeneratedWhenAbsent = test("Gltf/normalsAreGeneratedWhenAbsent") = []
{
    auto positions = cubePositions();
    auto indices = cubeIndices();

    auto binary = BinaryBuffer {};
    auto positionOffset = binary.appendFloats(positions);
    auto indexOffset = binary.appendIndices(indices);

    auto document = std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"}
                    + std::to_string(positionOffset) + R"(, "byteLength": )"
                    + std::to_string((int) positions.size() * 4) + R"(},
            {"buffer": 0, "byteOffset": )"
                    + std::to_string(indexOffset) + R"(, "byteLength": )"
                    + std::to_string((int) indices.size() * 2) + R"(}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": )"
                    + std::to_string((int) positions.size() / 3)
                    + R"(, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": )"
                    + std::to_string((int) indices.size()) + R"(, "type": "SCALAR"}
        ]
    })";

    auto loaded = loadDocument(document, binary);

    check(bool(loaded));
    check(loaded.data.vertices.size() == (int) positions.size() / 3);

    for (const auto& vertex: loaded.data.vertices)
    {
        auto normal = Vec3 {(float) vertex.normal.values[0] / 32767.0f,
                            (float) vertex.normal.values[1] / 32767.0f,
                            (float) vertex.normal.values[2] / 32767.0f};

        // Unit length, to the precision a signed normalized short has.
        check(near(length(normal), 1.0f, 0.01f));

        // On a cube centred at the origin, an outward normal points the same way
        // as the vertex's own position. That is what catches a generator with
        // its cross product the wrong way round, which produces perfectly unit
        // normals pointing into the model.
        auto position =
            Vec3 {vertex.position[0], vertex.position[1], vertex.position[2]};

        check(dot(normal, normalize(position)) > 0.0f);
    }
};

// The packed vertex, round-tripped. Each format's precision is stated as the
// tolerance, so a format silently mapped to the wrong width fails here rather
// than in a picture.
auto tPackedAttributesRoundTrip = test("Gltf/packedAttributesRoundTrip") = []
{
    auto binary = BinaryBuffer {};

    auto positionOffset = binary.appendFloats({0, 0, 0, 1, 0, 0, 0, 1, 0});
    auto normalOffset = binary.appendFloats({0, 0, 1, 0, 0, 1, 0, 0, 1});
    auto uvOffset = binary.appendFloats({0.25f, 0.75f, 0.5f, 0.5f, 1.0f, 0.0f});
    auto indexOffset = binary.appendIndices({0, 1, 2});

    auto document = std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes":
            {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"}
                    + std::to_string(positionOffset) + R"(, "byteLength": 36},
            {"buffer": 0, "byteOffset": )"
                    + std::to_string(normalOffset) + R"(, "byteLength": 36},
            {"buffer": 0, "byteOffset": )"
                    + std::to_string(uvOffset) + R"(, "byteLength": 24},
            {"buffer": 0, "byteOffset": )"
                    + std::to_string(indexOffset) + R"(, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })";

    auto loaded = loadDocument(document, binary);

    check(bool(loaded));
    check(loaded.data.vertices.size() == 3);

    if (!hasVertices(loaded, 3))
        return;

    const auto& vertex = loaded.data.vertices[0];

    // Position stays float: a world coordinate needs the mantissa, which is why
    // it is the one attribute that is not packed.
    check(near(vertex.position[0], 0.0f, 1.0e-6f));

    // A signed normalized short holds a direction to about 3e-5.
    check(near((float) vertex.normal.values[2] / 32767.0f, 1.0f, 1.0e-4f));

    // Half holds a UV to about one part in a thousand across [0, 1].
    check(near(GPU::halfToFloat(vertex.uv.values[0]), 0.25f, 1.0e-3f));
    check(near(GPU::halfToFloat(vertex.uv.values[1]), 0.75f, 1.0e-3f));

    // COLOR_0 was absent, so the vertex colour is opaque white rather than the
    // transparent black a zero-filled struct would hold - the same default trap
    // as the material's base colour, one level down.
    for (auto c = 0; c < 4; ++c)
        check(vertex.color.values[c] == 0xff);
};

// The bounds an app frames a camera with. Computed in world space, so a node's
// transform has to be in them.
auto tBoundsCoverTransformedGeometry =
    test("Gltf/boundsCoverTransformedGeometry") = []
{
    auto binary = BinaryBuffer {};
    binary.appendFloats({0, 0, 0, 1, 0, 0, 0, 1, 0});
    binary.appendIndices({0, 1, 2});

    auto loaded = loadDocument(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"translation": [100, 0, 0], "mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })",
                               binary);

    check(bool(loaded));

    // The triangle spans (0,0,0)..(1,1,0) locally; the node puts it at x = 100.
    check(near(loaded.data.boundsMin.x, 100.0f, 1.0e-4f));
    check(near(loaded.data.boundsMax.x, 101.0f, 1.0e-4f));
    check(near(loaded.data.boundsCenter().x, 100.5f, 1.0e-4f));
    check(near(loaded.data.boundsExtent(), 1.0f, 1.0e-4f));
};

// A primitive whose topology this loader does not read is skipped rather than
// taking the whole model down with it.
auto tNonTriangleTopologyIsSkipped = test("Gltf/nonTriangleTopologyIsSkipped") = []
{
    auto binary = BinaryBuffer {};
    binary.appendFloats({0, 0, 0, 1, 0, 0, 0, 1, 0});
    binary.appendIndices({0, 1, 2});

    // Mode 1 is LINES; mode 4 (the default) is TRIANGLES.
    auto loaded = loadDocument(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [
            {"attributes": {"POSITION": 0}, "indices": 1, "mode": 1},
            {"attributes": {"POSITION": 0}, "indices": 1}
        ]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })",
                               binary);

    check(bool(loaded));
    check(loaded.data.primitives.size() == 1);
    check(loaded.data.vertices.size() == 3);
};
