#include "Common.h"

// Reached into directly rather than through Mesh.h: the reader is an
// implementation detail of the loader and is deliberately not in the umbrella,
// but two of its pieces - the type table and the URI decoder - are worth
// asserting on without a whole document around them.
#include <eacp/Mesh/GltfReader.h>

// The layer that used to be somebody else's: the GLB container, the accessor
// indirection, and the component types a real exporter emits.
//
// GltfLoaderTests covers the schema — nodes, materials, primitives — and every
// one of its documents is float attributes in a base64 .gltf, because that is
// what a test authored by hand naturally writes. None of it touches a GLB, a
// byte stride, a normalized short or a sparse accessor. All four are paths a
// dependency handled before and this tree handles now, so all four need a case
// that fails when they are wrong.
//
// Each is written so that being wrong is *visible*: a wrong component type
// yields a wrong number rather than a refusal, which is exactly the class of bug
// that reaches the screen instead of the log.

using namespace nano;
using namespace eacp;
using namespace eacp::Mesh;
using namespace eacp::Mesh::Tests;

namespace
{
constexpr auto tight = 1.0e-5f;

// A triangle whose vertices sit at known, distinguishable coordinates, so a
// misread stride or offset moves one of them somewhere identifiable rather than
// producing plausible geometry.
const std::vector<float> trianglePositions {0, 0, 0, 2, 0, 0, 0, 4, 0};

std::string triangleAccessors(int positionView, int indexView)
{
    return std::string {R"(
        "accessors": [
            {"bufferView": )"}
           + std::to_string(positionView) + R"(, "componentType": 5126,
             "count": 3, "type": "VEC3"},
            {"bufferView": )"
           + std::to_string(indexView) + R"(, "componentType": 5123,
             "count": 3, "type": "SCALAR"}
        ])";
}
} // namespace

// The container. A .glb keeps its JSON and its binary in one file behind a
// 12-byte header, and buffer 0 with no "uri" means "the binary chunk" - so
// getting the chunk walk wrong yields a model with no geometry rather than an
// error, which is why this asserts on the vertices and not only on success.
auto tLoadsAGlbContainer = test("GltfReader/loadsAGlbContainer") = []
{
    auto binary = BinaryBuffer {};
    auto positionAt = binary.appendFloats(trianglePositions);
    auto indexAt = binary.appendIndices({0, 1, 2});

    auto json = std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                    "indices": 1}]}],
        "buffers": [{"byteLength": )"}
                + std::to_string(binary.size()) + R"(}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"
                + std::to_string(positionAt) + R"(, "byteLength": 36},
            {"buffer": 0, "byteOffset": )"
                + std::to_string(indexAt) + R"(, "byteLength": 6}
        ],)" + triangleAccessors(0, 1)
                + R"(
    })";

    auto loaded = loadBytes(makeGlb(json, binary.bytes));

    check(bool(loaded));
    check(loaded.data.vertices.size() == 3);

    if (!hasVertices(loaded, 3))
        return;

    // The actual coordinates, so a chunk offset out by the header's 12 bytes is
    // caught rather than producing three vertices of nonsense.
    check(near(loaded.data.vertices[1].position[0], 2.0f, tight));
    check(near(loaded.data.vertices[2].position[1], 4.0f, tight));
};

auto tTruncatedGlbIsRefused = test("GltfReader/truncatedGlbIsRefused") = []
{
    auto binary = BinaryBuffer {};
    binary.appendFloats(trianglePositions);

    auto glb = makeGlb(R"({"asset": {"version": "2.0"}})", binary.bytes);

    // Cut the file in half while leaving the header's declared length alone,
    // which is what a partial download looks like. Reading it must not walk off
    // the end.
    glb.resize(glb.size() / 2);

    auto loaded = loadBytes(glb);

    check(!bool(loaded));
    check(!loaded.error.empty());
};

// Interleaved vertex data: one buffer view holding position and normal per
// vertex, with byteStride saying how far apart consecutive vertices are and each
// accessor's byteOffset saying where inside a vertex its attribute starts.
//
// This is how every real exporter writes geometry, and it is the single most
// likely thing to be wrong in a hand-written accessor reader: ignoring the
// stride reads the normal as the next vertex's position, which produces a
// recognisable model with wrong shading rather than an error.
auto tInterleavedByteStrideIsHonoured =
    test("GltfReader/interleavedByteStrideIsHonoured") = []
{
    // Three vertices of {position(3), normal(3)}, interleaved: 24 bytes apart.
    auto binary = BinaryBuffer {};
    auto vertexAt = binary.appendFloats({0,
                                         0,
                                         0,
                                         /* n */ 1,
                                         0,
                                         0,
                                         2,
                                         0,
                                         0,
                                         /* n */ 0,
                                         1,
                                         0,
                                         0,
                                         4,
                                         0,
                                         /* n */ 0,
                                         0,
                                         1});
    auto indexAt = binary.appendIndices({0, 1, 2});

    auto loaded = loadDocument(std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                                    "indices": 2}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"}
                                   + std::to_string(vertexAt)
                                   + R"(, "byteLength": 72, "byteStride": 24},
            {"buffer": 0, "byteOffset": )"
                                   + std::to_string(indexAt) + R"(, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "byteOffset": 0, "componentType": 5126,
             "count": 3, "type": "VEC3"},
            {"bufferView": 0, "byteOffset": 12, "componentType": 5126,
             "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })",
                               binary);

    check(bool(loaded));
    check(loaded.data.vertices.size() == 3);

    if (!hasVertices(loaded, 3))
        return;

    // Positions land where the stride says.
    check(near(loaded.data.vertices[1].position[0], 2.0f, tight));
    check(near(loaded.data.vertices[2].position[1], 4.0f, tight));

    // And each vertex's normal is its own, not the next vertex's position. A
    // reader ignoring byteStride gives vertex 0 the normal (2,0,0) normalized -
    // which is (1,0,0), the right answer by accident - so vertex 1 is the one
    // that decides it: its normal is +y, and the stride-ignoring read there is
    // (0,4,0) normalized, also +y. Vertex 2 is the discriminator, since its
    // normal is +z and no position in this buffer points that way.
    auto normalZ = (float) loaded.data.vertices[2].normal.values[2] / 32767.0f;
    check(near(normalZ, 1.0f, 1.0e-3f));
};

// Normalized integer attributes, which is how a colour and a compact UV arrive.
// The spec's mapping is per component type - 255 for unsigned byte, 65535 for
// unsigned short - and using the wrong divisor produces a washed-out or
// oversaturated model that still looks like a model.
auto tNormalizedComponentTypesMapToUnitRange =
    test("GltfReader/normalizedComponentTypesMapToUnitRange") = []
{
    auto binary = BinaryBuffer {};
    auto positionAt = binary.appendFloats(trianglePositions);

    // COLOR_0 as normalized unsigned byte, VEC4: opaque red, mid grey, white.
    auto colorAt =
        binary.appendBytes({255, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255});

    // TEXCOORD_0 as normalized unsigned short, VEC2: (0,0), (1,1), (0.5, 0.25).
    auto uvAt = binary.appendIndices({0, 0, 65535, 65535, 32768, 16384});
    auto indexAt = binary.appendIndices({0, 1, 2});

    auto loaded =
        loadDocument(std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes":
            {"POSITION": 0, "COLOR_0": 1, "TEXCOORD_0": 2}, "indices": 3}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"}
                         + std::to_string(positionAt) + R"(, "byteLength": 36},
            {"buffer": 0, "byteOffset": )"
                         + std::to_string(colorAt) + R"(, "byteLength": 12},
            {"buffer": 0, "byteOffset": )"
                         + std::to_string(uvAt) + R"(, "byteLength": 12},
            {"buffer": 0, "byteOffset": )"
                         + std::to_string(indexAt) + R"(, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5121, "normalized": true,
             "count": 3, "type": "VEC4"},
            {"bufferView": 2, "componentType": 5123, "normalized": true,
             "count": 3, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })",
                     binary);

    check(bool(loaded));
    check(loaded.data.vertices.size() == 3);

    if (!hasVertices(loaded, 3))
        return;

    // The colour survives a round trip through UNorm8x4 exactly, being the same
    // eight bits it started as.
    check(loaded.data.vertices[0].color.values[0] == 255);
    check(loaded.data.vertices[0].color.values[1] == 0);
    check(loaded.data.vertices[1].color.values[0] == 128);

    // The UVs went 0..65535 -> 0..1. The normalization's precision is the
    // tolerance, and it is the only one left now that the UV is stored as a
    // float: 1/65535 is what a normalized short can say.
    check(near(loaded.data.vertices[1].uv[0], 1.0f, 1.0e-4f));
    check(near(loaded.data.vertices[2].uv[0], 0.5f, 1.0e-4f));
    check(near(loaded.data.vertices[2].uv[1], 0.25f, 1.0e-4f));
};

// A sparse accessor: dense data plus a short list of elements that override it.
// Rare in the wild and easy to ignore, and ignoring it silently draws the
// un-overridden mesh - so the case asserts the override landed *and* that the
// elements around it did not move.
auto tSparseAccessorOverridesElements =
    test("GltfReader/sparseAccessorOverridesElements") = []
{
    auto binary = BinaryBuffer {};
    auto positionAt = binary.appendFloats(trianglePositions);
    auto indexAt = binary.appendIndices({0, 1, 2});

    // Override element 1 only, moving it to (9, 9, 0).
    auto sparseIndexAt = binary.appendIndices({1});
    auto sparseValueAt = binary.appendFloats({9, 9, 0});

    auto loaded =
        loadDocument(std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                    "indices": 1}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"}
                         + std::to_string(positionAt) + R"(, "byteLength": 36},
            {"buffer": 0, "byteOffset": )"
                         + std::to_string(indexAt) + R"(, "byteLength": 6},
            {"buffer": 0, "byteOffset": )"
                         + std::to_string(sparseIndexAt) + R"(, "byteLength": 2},
            {"buffer": 0, "byteOffset": )"
                         + std::to_string(sparseValueAt) + R"(, "byteLength": 12}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
             "sparse": {
                "count": 1,
                "indices": {"bufferView": 2, "componentType": 5123},
                "values": {"bufferView": 3}
             }},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })",
                     binary);

    check(bool(loaded));
    check(loaded.data.vertices.size() == 3);

    if (!hasVertices(loaded, 3))
        return;

    // The overridden element moved.
    check(near(loaded.data.vertices[1].position[0], 9.0f, tight));
    check(near(loaded.data.vertices[1].position[1], 9.0f, tight));

    // Its neighbours did not, which is what separates "sparse was applied" from
    // "the whole accessor was read from the wrong place".
    check(near(loaded.data.vertices[0].position[0], 0.0f, tight));
    check(near(loaded.data.vertices[2].position[1], 4.0f, tight));
};

// An index pointing past its own primitive's vertices. On the GPU that reads
// whatever happens to be next in the shared buffer, which is another
// primitive's geometry - so it is refused at load rather than drawn.
auto tOutOfRangeIndexIsRefused = test("GltfReader/outOfRangeIndexIsRefused") = []
{
    auto binary = BinaryBuffer {};
    auto positionAt = binary.appendFloats(trianglePositions);

    // Three vertices, and an index naming the fourth.
    auto indexAt = binary.appendIndices({0, 1, 3});

    auto loaded =
        loadDocument(std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                    "indices": 1}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"}
                         + std::to_string(positionAt) + R"(, "byteLength": 36},
            {"buffer": 0, "byteOffset": )"
                         + std::to_string(indexAt) + R"(, "byteLength": 6}
        ],)" + triangleAccessors(0, 1)
                         + R"(
    })",
                     binary);

    // The primitive was the only one, so refusing it leaves nothing to draw.
    check(!bool(loaded));

    // And it left nothing half-written behind it: a rolled-back primitive that
    // kept its vertices would inflate every later base vertex.
    check(loaded.data.vertices.size() == 0);
    check(loaded.data.indices.size() == 0);
};

// glTF 1.0 is a different format behind the same extension, and reading one as
// 2.0 produces geometry that is wrong rather than absent.
auto tWrongVersionIsRefused = test("GltfReader/wrongVersionIsRefused") = []
{
    const char* legacy = R"({"asset": {"version": "1.0"}, "meshes": {}})";
    auto legacyResult = loadGltfFromMemory(legacy, std::strlen(legacy));

    check(!bool(legacyResult));
    check(legacyResult.error.find("1.0") != std::string::npos);

    // A file with no asset block at all is not a glTF either, and says so rather
    // than reporting no geometry.
    const char* headless = R"({"meshes": []})";
    auto headlessResult = loadGltfFromMemory(headless, std::strlen(headless));

    check(!bool(headlessResult));
    check(headlessResult.error.find("asset") != std::string::npos);
};

// glTF's node graph is meant to be a tree, and nothing in the file format stops
// a child list pointing back at an ancestor. Unguarded, composing transforms
// down one recurses until the stack runs out.
auto tCyclicNodeGraphTerminates = test("GltfReader/cyclicNodeGraphTerminates") = []
{
    auto binary = BinaryBuffer {};
    auto positionAt = binary.appendFloats(trianglePositions);
    auto indexAt = binary.appendIndices({0, 1, 2});

    // Node 0's child is node 1, whose child is node 0.
    auto loaded =
        loadDocument(std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"children": [1], "mesh": 0},
            {"children": [0], "mesh": 0}
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                    "indices": 1}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"}
                         + std::to_string(positionAt) + R"(, "byteLength": 36},
            {"buffer": 0, "byteOffset": )"
                         + std::to_string(indexAt) + R"(, "byteLength": 6}
        ],)" + triangleAccessors(0, 1)
                         + R"(
    })",
                     binary);

    // Reaching this line at all is the assertion: without the depth guard the
    // process is gone before any check runs.
    check(bool(loaded));

    // And the walk stopped rather than running to the guard's limit repeatedly:
    // two nodes, each drawn at most once per path into it.
    check(loaded.data.drawOrder.size() <= 4);
};

// A buffer that declares more bytes than arrived. Every accessor bound-checks
// against its view, and a short buffer would let those checks pass against data
// that is not there.
auto tShortBufferIsRefused = test("GltfReader/shortBufferIsRefused") = []
{
    auto binary = BinaryBuffer {};
    auto positionAt = binary.appendFloats(trianglePositions);
    auto indexAt = binary.appendIndices({0, 1, 2});

    // The document claims a kilobyte; the data URI carries 42 bytes.
    auto loaded =
        loadDocument(std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                    "indices": 1}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": 1024}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"}
                         + std::to_string(positionAt) + R"(, "byteLength": 36},
            {"buffer": 0, "byteOffset": )"
                         + std::to_string(indexAt) + R"(, "byteLength": 6}
        ],)" + triangleAccessors(0, 1)
                         + R"(
    })",
                     binary);

    check(!bool(loaded));
    check(loaded.error.find("shorter") != std::string::npos);
};

// A bufferView reaching past the end of its buffer. The bounds check is per
// element rather than once up front, so this pins that it happens at all.
auto tOverlongBufferViewIsRefused =
    test("GltfReader/overlongBufferViewIsRefused") = []
{
    auto binary = BinaryBuffer {};
    auto positionAt = binary.appendFloats(trianglePositions);
    auto indexAt = binary.appendIndices({0, 1, 2});

    // The position view claims 360 bytes of a 42-byte buffer.
    auto loaded =
        loadDocument(std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                    "indices": 1}]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"}
                         + std::to_string(positionAt) + R"(, "byteLength": 360},
            {"buffer": 0, "byteOffset": )"
                         + std::to_string(indexAt) + R"(, "byteLength": 6}
        ],)" + triangleAccessors(0, 1)
                         + R"(
    })",
                     binary);

    check(!bool(loaded));
};

// The base64 decoder, through the path every other test in this suite uses. A
// payload with a character that is not base64 must yield nothing rather than a
// truncated buffer, since a partial decode is geometry that is wrong in a way
// nothing downstream can detect.
auto tCorruptBase64YieldsNothing = test("GltfReader/corruptBase64YieldsNothing") = []
{
    const char* document = R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                    "indices": 1}]}],
        "buffers": [{"uri": "data:application/octet-stream;base64,!!!not base64!!!",
                     "byteLength": 42}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ]
    })";

    auto loaded = loadGltfFromMemory(document, std::strlen(document));

    check(!bool(loaded));
};

// Matrix accessors are refused rather than half-read: byte- and short-component
// matrices pad every column to four bytes, a rule that silently skews geometry.
// No attribute this reader consumes is a matrix, so refusing costs nothing.
auto tMatrixAccessorTypesAreRefused =
    test("GltfReader/matrixAccessorTypesAreRefused") = []
{
    check(Gltf::componentsForType("SCALAR") == 1);
    check(Gltf::componentsForType("VEC2") == 2);
    check(Gltf::componentsForType("VEC3") == 3);
    check(Gltf::componentsForType("VEC4") == 4);

    check(Gltf::componentsForType("MAT2") == 0);
    check(Gltf::componentsForType("MAT3") == 0);
    check(Gltf::componentsForType("MAT4") == 0);

    // And an unknown string is not silently a scalar.
    check(Gltf::componentsForType("VEC5") == 0);
    check(Gltf::componentsForType("") == 0);
};

// Percent-escapes in a URI. "chair%20base.png" is a file whose name has a space
// in it, and looking for the unescaped name finds nothing - a texture silently
// missing rather than an error.
auto tPercentEscapesAreDecoded = test("GltfReader/percentEscapesAreDecoded") = []
{
    check(Gltf::decodePercentEscapes("chair%20base.png") == "chair base.png");
    check(Gltf::decodePercentEscapes("plain.png") == "plain.png");

    // A stray percent is left alone rather than eating the characters after it.
    check(Gltf::decodePercentEscapes("100%.png") == "100%.png");
    check(Gltf::decodePercentEscapes("%zz") == "%zz");

    // Lower and upper case hex both decode.
    check(Gltf::decodePercentEscapes("a%2Fb") == "a/b");
    check(Gltf::decodePercentEscapes("a%2fb") == "a/b");
};
