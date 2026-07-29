#include "Common.h"

#include <eacp/GPU/GPU.h>

// The one part of the module that needs a device: MeshRenderer drawing a loaded
// model, checked by reading the pixels back.
//
// Everything above this file is a property of a struct. These are the properties
// that only exist once the geometry, the transforms and the pipeline state are
// all connected - and each is a case where being wrong still draws a picture.

using namespace nano;
using namespace eacp;
using namespace eacp::Mesh;
using namespace eacp::Mesh::Tests;

namespace
{
bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f && c.b < 0.5f;
}

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f && c.b < 0.5f;
}

// The clear colour: a third colour, so "nothing drew here" reads as itself
// rather than as one of the two cubes.
bool isBlue(const Graphics::Color& c)
{
    return c.b > 0.5f && c.r < 0.5f && c.g < 0.5f;
}

// A document holding one cube per node, each node translated and coloured by the
// caller. Two overlapping cubes at different depths is what a depth test has an
// opinion about; two side by side is not.
std::string cubeSceneDocument(const std::vector<float>& positions,
                              const std::vector<std::uint16_t>& indices,
                              int positionOffset,
                              int indexOffset,
                              const std::string& nodes,
                              const std::string& materials,
                              const std::string& primitives)
{
    return std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": )"}
           + nodes + R"(,
        "materials": )"
           + materials + R"(,
        "meshes": )"
           + primitives + R"(,
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"
           + std::to_string(positionOffset) + R"(, "byteLength": )"
           + std::to_string((int) positions.size() * 4) + R"(},
            {"buffer": 0, "byteOffset": )"
           + std::to_string(indexOffset) + R"(, "byteLength": )"
           + std::to_string((int) indices.size() * 2) + R"(}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": )"
           + std::to_string((int) positions.size() / 3) + R"(, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": )"
           + std::to_string((int) indices.size()) + R"(, "type": "SCALAR"}
        ]
    })";
}

// Two unit cubes on the z axis: a red one nearer the camera at z = +2, a green
// one further away at z = -2. Both are drawn, and which one survives at the
// centre pixel is the whole question.
LoadResult loadTwoCubes(bool mirrorTheNearOne = false)
{
    auto positions = cubePositions();
    auto indices = cubeIndices();

    auto binary = BinaryBuffer {};
    auto positionOffset = binary.appendFloats(positions);
    auto indexOffset = binary.appendIndices(indices);

    auto nearScale = mirrorTheNearOne ? R"("scale": [-1, 1, 1], )" : "";

    auto nodes =
        std::string {"[{"} + nearScale + R"("translation": [0, 0, 2], "mesh": 0},
                     {"translation": [0, 0, -2], "mesh": 1}])";

    auto materials =
        R"([{"pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1]}},
             {"pbrMetallicRoughness": {"baseColorFactor": [0, 1, 0, 1]}}])";

    auto meshes =
        R"([{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1,
                             "material": 0}]},
             {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1,
                             "material": 1}]}])";

    return loadDocument(cubeSceneDocument(positions,
                                          indices,
                                          positionOffset,
                                          indexOffset,
                                          nodes,
                                          materials,
                                          meshes),
                        binary);
}

struct ModelView final : GPU::GPUView
{
    ModelView()
        : renderer(1)
    {
        // MSAA would feather the cube edges and average two colours along them,
        // turning exact pixel assertions into approximate ones for no gain.
        setSampleCount(1);
        setDepth(true);
    }

    void render(GPU::Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 1.f, 1.f}});

        if (!renderer.hasModel())
            return;

        renderer.draw(pass, options);
    }

    MeshRenderer renderer;
    RenderOptions options;
};

// The camera sits on +z looking at the origin, so the cube at z = +2 is the
// nearer one.
RenderOptions cameraLookingDownZ()
{
    auto options = RenderOptions {};

    options.view =
        Mat4::lookAt({0.0f, 0.0f, 12.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    options.projection = Mat4::perspective(1.0f, 0.8f, 0.1f, 100.0f);
    options.shading = ShadingMode::Unlit;

    return options;
}

Graphics::Image renderModel(const MeshData& data, const RenderOptions& options)
{
    auto view = ModelView {};

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.renderer.setModel(data);
    view.options = options;

    return view.renderToImage(1.f);
}

Graphics::Color centreOf(const Graphics::Image& image)
{
    return image.at(image.width() / 2, image.height() / 2);
}

// A cube whose corners are already offset in x, so the offset is in the *vertex
// data* rather than in a node transform. That is what makes the base vertex
// observable: two primitives that differ only by their node's transform draw the
// same geometry, and a renderer ignoring the base vertex entirely would pass.
std::vector<float> cubeAt(float x)
{
    auto positions = cubePositions();

    for (auto i = std::size_t {0}; i < positions.size(); i += 3)
        positions[i] += x;

    return positions;
}
} // namespace

// The end-to-end case: geometry uploaded, transforms composed, a base vertex per
// primitive, a depth test. If any link in that is wrong this is what says so.
auto tNearerGeometryOccludesFurther = test("MeshRender/nearerOccludesFurther") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto loaded = loadTwoCubes();
    check(bool(loaded));

    auto image = renderModel(loaded.data, cameraLookingDownZ());

    if (!image.isValid())
        return;

    // The red cube is nearer, so it is what the centre pixel holds - not the
    // green one behind it, and not the clear colour.
    check(isRed(centreOf(image)));
};

// The same scene from the other side. Without a depth test this draws whichever
// cube the file lists last whichever way the camera faces, so a pass reading
// green from behind and red from in front is the depth buffer doing its job
// rather than the draw order happening to suit.
auto tOcclusionFollowsTheCamera = test("MeshRender/occlusionFollowsTheCamera") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto loaded = loadTwoCubes();
    check(bool(loaded));

    auto options = cameraLookingDownZ();
    options.view =
        Mat4::lookAt({0.0f, 0.0f, -12.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});

    auto image = renderModel(loaded.data, options);

    if (!image.isValid())
        return;

    check(isGreen(centreOf(image)));
};

// Face culling is on, and a mirrored node is the case it goes wrong on: a
// negative scale reverses the winding of every triangle under it, so what was an
// outward face is now a back face and gets thrown away.
//
// Making that observable takes more care than it looks. The obvious scene - a
// mirrored cube on its own - passes whether or not the front face is flipped,
// because culling a cube's near faces simply reveals its *far* faces, which are
// the same colour. The first version of this case was exactly that, and it
// passed with the flip deleted.
//
// So a green slab is parked *inside* the red cube, between its near face and its
// far one. Correct: the red near face is closest and the centre is red. Without
// the flip: the red near faces are culled, the deepest red surface is the far
// face behind the slab, and the centre comes out green.
//
// This is also the case §5.9 of the plan asked whether the corpus even contains.
// It is authored here rather than hoped for.
auto tMirroredNodeStillDrawsFrontFaces =
    test("MeshRender/mirroredNodeStillDrawsFrontFaces") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto positions = cubePositions();
    auto indices = cubeIndices();

    auto binary = BinaryBuffer {};
    auto positionOffset = binary.appendFloats(positions);
    auto indexOffset = binary.appendIndices(indices);

    // The red cube spans z 1..3 after its translation; the green slab is 0.04
    // deep at z = 2, so it sits between the cube's two faces and neither
    // surface touches the other.
    auto nodes = R"([
        {"translation": [0, 0, 2], "scale": [-1, 1, 1], "mesh": 0},
        {"translation": [0, 0, 2], "scale": [0.6, 0.6, 0.02], "mesh": 1}
    ])";

    auto materials =
        R"([{"pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1]}},
             {"pbrMetallicRoughness": {"baseColorFactor": [0, 1, 0, 1]}}])";

    auto meshes =
        R"([{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1,
                             "material": 0}]},
             {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1,
                             "material": 1}]}])";

    auto loaded = loadDocument(cubeSceneDocument(positions,
                                                 indices,
                                                 positionOffset,
                                                 indexOffset,
                                                 nodes,
                                                 materials,
                                                 meshes),
                               binary);

    check(bool(loaded));

    // The mirror has to actually be in the data, or this passes by drawing an
    // unmirrored cube. The slab must not be mirrored, or it needs the flip too
    // and the case stops isolating one thing.
    check(loaded.data.nodes[0].worldTransform.linearDeterminant() < 0.0f);
    check(loaded.data.nodes[1].worldTransform.linearDeterminant() > 0.0f);

    auto image = renderModel(loaded.data, cameraLookingDownZ());

    if (!image.isValid())
        return;

    check(isRed(centreOf(image)));
};

// The shading modes are a uniform rather than three pipelines, so this is also
// the case that says the uniform reaches the shader at all.
auto tShadingModesDiffer = test("MeshRender/shadingModesDiffer") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto loaded = loadTwoCubes();
    check(bool(loaded));

    auto options = cameraLookingDownZ();
    auto unlit = renderModel(loaded.data, options);

    if (!unlit.isValid())
        return;

    options.shading = ShadingMode::Normals;
    auto normals = renderModel(loaded.data, options);

    // The near cube's front face points at +z, so as a colour it is (0.5, 0.5,
    // 1) - blue-ish, and nothing like the flat red the unlit mode draws. Both
    // halves are asserted: a mode uniform that never reached the shader would
    // leave these identical.
    check(isRed(centreOf(unlit)));
    check(!isRed(centreOf(normals)));

    auto normalPixel = centreOf(normals);
    check(normalPixel.b > normalPixel.r);
};

// What Phase 1 of the GPU plan bought, at the end it is actually used: two
// primitives packed into one vertex buffer, each drawn with its own base vertex
// and first index.
//
// The geometry is offset in the vertex data rather than by a node transform,
// which is the only way this is observable. Two primitives sharing an accessor
// and differing by their node's transform draw identically whether or not the
// base vertex is threaded through - so the obvious version of this case tests
// nothing, and the first draft of this file was that version.
//
// Ignoring baseVertex draws the left cube twice and leaves the right of the
// image at the clear colour; ignoring firstIndex does the same thing.
auto tBaseVertexSelectsThePrimitive =
    test("MeshRender/baseVertexSelectsPrimitive") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto left = cubeAt(-2.2f);
    auto right = cubeAt(2.2f);
    auto indices = cubeIndices();

    auto binary = BinaryBuffer {};
    auto leftOffset = binary.appendFloats(left);
    auto rightOffset = binary.appendFloats(right);
    auto indexOffset = binary.appendIndices(indices);

    auto positionBytes = std::to_string((int) left.size() * 4);
    auto vertexCount = std::to_string((int) left.size() / 3);

    auto document =
        std::string {R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "materials": [
            {"pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1]}},
            {"pbrMetallicRoughness": {"baseColorFactor": [0, 1, 0, 1]}}
        ],
        "meshes": [{"primitives": [
            {"attributes": {"POSITION": 0}, "indices": 2, "material": 0},
            {"attributes": {"POSITION": 1}, "indices": 2, "material": 1}
        ]}],
        "buffers": [{"uri": "@BUFFER@", "byteLength": @BYTELENGTH@}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": )"}
        + std::to_string(leftOffset) + R"(, "byteLength": )" + positionBytes + R"(},
            {"buffer": 0, "byteOffset": )"
        + std::to_string(rightOffset) + R"(, "byteLength": )" + positionBytes + R"(},
            {"buffer": 0, "byteOffset": )"
        + std::to_string(indexOffset) + R"(, "byteLength": )"
        + std::to_string((int) indices.size() * 2) + R"(}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": )"
        + vertexCount + R"(, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5126, "count": )"
        + vertexCount + R"(, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": )"
        + std::to_string((int) indices.size()) + R"(, "type": "SCALAR"}
        ]
    })";

    auto loaded = loadDocument(document, binary);
    check(bool(loaded));

    // The second primitive has to actually be offset in the shared buffer, or
    // there is no base vertex for the draw to get wrong.
    check(loaded.data.primitives.size() == 2);
    check(loaded.data.primitives[1].baseVertex == (int) left.size() / 3);
    check(loaded.data.primitives[1].firstIndex == (int) indices.size());

    auto image = renderModel(loaded.data, cameraLookingDownZ());

    if (!image.isValid())
        return;

    auto row = image.height() / 2;

    check(isRed(image.at(image.width() / 4, row)));
    check(isGreen(image.at(image.width() * 3 / 4, row)));
};

// A model that never loaded must not draw anything, rather than drawing whatever
// the buffers last held.
auto tEmptyModelDrawsNothing = test("MeshRender/emptyModelDrawsNothing") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto image = renderModel(MeshData {}, cameraLookingDownZ());

    if (!image.isValid())
        return;

    check(isBlue(centreOf(image)));
};
