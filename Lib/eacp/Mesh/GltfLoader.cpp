#include "GltfLoader.h"

#include "GltfReader.h"

#include <eacp/Core/Utils/Files.h>

namespace eacp::Mesh
{
namespace
{
using namespace Gltf;

// glTF's primitive modes. Only triangles are read; the rest are skipped rather
// than triangulated, so a file mixing them still draws its triangles.
constexpr auto trianglesMode = 4;

std::string nameOf(const Miro::Json::Value& value, const char* fallback, int index)
{
    auto name = stringOr(value, "name");

    if (!name.empty())
        return name;

    return std::string {fallback} + " " + std::to_string(index);
}

AlphaMode toAlphaMode(const std::string& mode)
{
    if (mode == "MASK")
        return AlphaMode::Mask;
    if (mode == "BLEND")
        return AlphaMode::Blend;

    return AlphaMode::Opaque;
}

// A node's local transform. glTF lets a node give either a 4x4 matrix or a TRS
// triple, and the matrix wins when both are present - the one place the two
// forms a node may be written in have to agree.
Mat4 localTransform(const Miro::Json::Value& node)
{
    float matrix[16] {};

    if (readNumbers(node, "matrix", matrix, 16) == 16)
    {
        // glTF writes a matrix column-major, which is Mat4's own layout, so this
        // is a copy rather than a transpose. Getting that backwards is invisible
        // on a symmetric transform and catastrophic on every other.
        auto result = Mat4 {};

        for (auto i = 0; i < 16; ++i)
            result.values[i] = matrix[i];

        return result;
    }

    float translation[3] {0.0f, 0.0f, 0.0f};
    float rotation[4] {0.0f, 0.0f, 0.0f, 1.0f};
    float scale[3] {1.0f, 1.0f, 1.0f};

    readNumbers(node, "translation", translation, 3);
    readNumbers(node, "rotation", rotation, 4);
    readNumbers(node, "scale", scale, 3);

    return Mat4::fromTRS({translation[0], translation[1], translation[2]},
                         {rotation[0], rotation[1], rotation[2], rotation[3]},
                         {scale[0], scale[1], scale[2]});
}

class Builder
{
public:
    explicit Builder(const Document& documentToUse)
        : document(documentToUse)
    {
    }

    MeshData build()
    {
        loadImages();
        loadMaterials();
        loadMeshes();
        loadNodes();
        computeBounds();

        return std::move(data);
    }

private:
    void loadImages()
    {
        const auto* images = document.collection("images");

        if (images == nullptr)
            return;

        for (auto i = 0; i < images->size(); ++i)
        {
            const auto& entry = images->get(i);
            auto bytes = Vector<std::uint8_t> {};

            // Either inside the binary chunk, or at a URI - which is a base64
            // payload or a file beside the .gltf, both resolved by the reader.
            if (auto view = intOr(entry, "bufferView", -1); view >= 0)
            {
                auto span = document.bufferViewBytes(view);
                bytes.getVector().assign(span.begin(), span.end());
            }
            else
            {
                bytes = document.readUri(stringOr(entry, "uri"));
            }

            // An image that fails to decode still takes a slot, so every
            // material's index into this list stays the index the file gave it.
            // Dropping it would silently repoint every later material at the
            // wrong texture.
            data.images.add(bytes.size() > 0 ? Graphics::Image::decode(bytes)
                                             : Graphics::Image {});
        }
    }

    // A texture reference resolves to an image through the textures array, so a
    // material names a texture and this follows it one hop further.
    int imageIndexFor(const Miro::Json::Value& textureView) const
    {
        auto textureIndex = intOr(textureView, "index", -1);

        const auto* texture = at(document.collection("textures"), textureIndex);

        if (texture == nullptr)
            return -1;

        return intOr(*texture, "source", -1);
    }

    void loadMaterials()
    {
        const auto* materials = document.collection("materials");

        if (materials == nullptr)
            return;

        for (auto i = 0; i < materials->size(); ++i)
        {
            const auto& entry = materials->get(i);
            auto material = Material {};

            material.name = nameOf(entry, "material", i);
            material.alphaMode = toAlphaMode(stringOr(entry, "alphaMode", "OPAQUE"));
            material.alphaCutoff = floatOr(entry, "alphaCutoff", 0.5f);
            material.doubleSided = boolOr(entry, "doubleSided", false);

            // Everything outside metallic-roughness keeps the defaults, which
            // are opaque white rather than zero - see Material. A
            // specular-glossy or unlit material still draws, in its own base
            // colour if it has one and in white if it does not.
            if (const auto* pbr = member(entry, "pbrMetallicRoughness");
                pbr != nullptr)
            {
                readNumbers(*pbr, "baseColorFactor", material.baseColor, 4);

                if (const auto* texture = member(*pbr, "baseColorTexture");
                    texture != nullptr)
                    material.baseColorImage = imageIndexFor(*texture);
            }

            data.materials.add(std::move(material));
        }
    }

    void loadMeshes()
    {
        const auto* meshes = document.collection("meshes");

        if (meshes == nullptr)
            return;

        for (auto i = 0; i < meshes->size(); ++i)
        {
            const auto& entry = meshes->get(i);

            auto mesh = Mesh {};
            mesh.name = nameOf(entry, "mesh", i);

            if (const auto* primitives = arrayMember(entry, "primitives");
                primitives != nullptr)
            {
                for (auto p = 0; p < primitives->size(); ++p)
                {
                    const auto& primitive = primitives->get(p);

                    if (intOr(primitive, "mode", trianglesMode) != trianglesMode)
                        continue;

                    if (auto index = loadPrimitive(primitive); index >= 0)
                        mesh.primitives.add(index);
                }
            }

            data.meshes.add(std::move(mesh));
        }
    }

    int loadPrimitive(const Miro::Json::Value& source)
    {
        const auto* attributes = member(source, "attributes");

        if (attributes == nullptr)
            return -1;

        // Only set 0 of the multi-set attributes. A second UV set belongs to a
        // texture this slice does not read.
        auto positions = document.readFloats(intOr(*attributes, "POSITION", -1), 3);

        if (!positions.isValid())
            return -1;

        auto normals = document.readFloats(intOr(*attributes, "NORMAL", -1), 3);
        auto uvs = document.readFloats(intOr(*attributes, "TEXCOORD_0", -1), 2);
        auto colors = document.readFloats(intOr(*attributes, "COLOR_0", -1), 4);

        auto primitive = Primitive {};
        primitive.baseVertex = data.vertices.size();
        primitive.firstIndex = data.indices.size();
        primitive.material = intOr(source, "material", -1);

        appendVertices(positions, normals, uvs, colors);
        primitive.indexCount = appendIndices(source, positions.count);

        if (primitive.indexCount <= 0)
        {
            // Roll the vertices back rather than leaving a primitive with no
            // draw behind them: a vertex buffer with an orphaned range in it
            // silently inflates every later primitive's base vertex.
            data.vertices.resize(primitive.baseVertex);
            data.indices.resize(primitive.firstIndex);
            return -1;
        }

        if (!normals.isValid())
            generateNormals(primitive);

        computePrimitiveBounds(primitive, positions.count);

        data.primitives.add(primitive);
        return data.primitives.size() - 1;
    }

    void appendVertices(const FloatData& positions,
                        const FloatData& normals,
                        const FloatData& uvs,
                        const FloatData& colors)
    {
        for (auto i = 0; i < positions.count; ++i)
        {
            auto vertex = MeshVertex {};

            for (auto c = 0; c < 3; ++c)
                vertex.position[c] = positions.get(i, c);

            if (normals.isValid() && i < normals.count)
            {
                auto unit = normalize(
                    Vec3 {normals.get(i, 0), normals.get(i, 1), normals.get(i, 2)});

                vertex.normal = GPU::SNorm16x4::from(unit.x, unit.y, unit.z, 0.0f);
            }

            if (uvs.isValid() && i < uvs.count)
            {
                vertex.uv[0] = uvs.get(i, 0);
                vertex.uv[1] = uvs.get(i, 1);
            }

            // COLOR_0 may be VEC3 or VEC4, so alpha comes from the accessor only
            // when it has a fourth component and is opaque otherwise - the same
            // default trap as the material's base colour, one level down.
            if (colors.isValid() && i < colors.count)
            {
                auto alpha = colors.components > 3 ? colors.get(i, 3) : 1.0f;

                vertex.color = GPU::UNorm8x4::fromFloats(
                    colors.get(i, 0), colors.get(i, 1), colors.get(i, 2), alpha);
            }
            else
            {
                vertex.color = GPU::UNorm8x4::fromFloats(1.0f, 1.0f, 1.0f, 1.0f);
            }

            data.vertices.add(vertex);
        }
    }

    // Returns the number of indices written. They are relative to the
    // primitive's own first vertex - never rebased into the shared buffer - so
    // the width they need is set by the largest primitive rather than by the
    // model. See fitsNarrowIndices.
    int appendIndices(const Miro::Json::Value& source, int vertexCount)
    {
        if (auto accessor = intOr(source, "indices", -1); accessor >= 0)
        {
            auto indices = document.readIndices(accessor);

            for (auto index: indices)
            {
                // An index past the primitive's own vertices would read another
                // primitive's geometry once the buffers are shared, and on the
                // GPU it reads whatever is there. The primitive is refused
                // instead.
                if (index >= (std::uint32_t) vertexCount)
                    return 0;

                data.indices.add(index);
            }

            return indices.size();
        }

        // A primitive may draw its vertices in order with no index buffer at
        // all. One is synthesised so every draw downstream is indexed and there
        // is no second path through the renderer.
        for (auto i = 0; i < vertexCount; ++i)
            data.indices.add((std::uint32_t) i);

        return vertexCount;
    }

    // glTF says a mesh without normals is to be shaded flat, which needs a
    // vertex per face and so a different vertex count than the file declares.
    // These are area-weighted smooth normals instead: the same vertex buffer,
    // and a rounded silhouette where flat shading would facet it. The
    // difference shows on a cube and not on anything organic; a file wanting
    // hard edges is expected to supply its own normals, which almost all do.
    void generateNormals(const Primitive& primitive)
    {
        auto accumulated = Vector<Vec3> {};
        auto vertexCount = data.vertices.size() - primitive.baseVertex;
        accumulated.resize(vertexCount);

        auto positionAt = [&](std::uint32_t index)
        {
            const auto& vertex = data.vertices[primitive.baseVertex + (int) index];
            return Vec3 {vertex.position[0], vertex.position[1], vertex.position[2]};
        };

        for (auto i = 0; i + 2 < primitive.indexCount; i += 3)
        {
            auto a = data.indices[primitive.firstIndex + i];
            auto b = data.indices[primitive.firstIndex + i + 1];
            auto c = data.indices[primitive.firstIndex + i + 2];

            // Not normalized: the cross product's length is twice the
            // triangle's area, which is exactly the weight a big face should
            // carry over a sliver sharing the same vertex.
            auto faceNormal =
                cross(positionAt(b) - positionAt(a), positionAt(c) - positionAt(a));

            accumulated[(int) a] = accumulated[(int) a] + faceNormal;
            accumulated[(int) b] = accumulated[(int) b] + faceNormal;
            accumulated[(int) c] = accumulated[(int) c] + faceNormal;
        }

        for (auto i = 0; i < vertexCount; ++i)
        {
            auto unit = normalize(accumulated[i]);
            data.vertices[primitive.baseVertex + i].normal =
                GPU::SNorm16x4::from(unit.x, unit.y, unit.z, 0.0f);
        }
    }

    void computePrimitiveBounds(Primitive& primitive, int vertexCount)
    {
        if (vertexCount <= 0)
            return;

        const auto& first = data.vertices[primitive.baseVertex];
        primitive.boundsMin = {
            first.position[0], first.position[1], first.position[2]};
        primitive.boundsMax = primitive.boundsMin;

        for (auto i = 1; i < vertexCount; ++i)
        {
            const auto& vertex = data.vertices[primitive.baseVertex + i];
            auto position =
                Vec3 {vertex.position[0], vertex.position[1], vertex.position[2]};

            primitive.boundsMin = min(primitive.boundsMin, position);
            primitive.boundsMax = max(primitive.boundsMax, position);
        }
    }

    void loadNodes()
    {
        const auto* nodes = document.collection("nodes");

        if (nodes == nullptr)
            return;

        for (auto i = 0; i < nodes->size(); ++i)
        {
            const auto& entry = nodes->get(i);

            auto node = Node {};
            node.name = nameOf(entry, "node", i);
            node.transform = localTransform(entry);
            node.mesh = intOr(entry, "mesh", -1);

            if (const auto* children = arrayMember(entry, "children");
                children != nullptr)
                for (auto c = 0; c < children->size(); ++c)
                    if (const auto& child = children->get(c); child.isNumber())
                        node.children.add((int) child.asNumber());

            data.nodes.add(std::move(node));
        }

        // glTF stores children and not parents, so the back-edge is computed
        // here. MeshData carries it because the tree is drawn as a flat list and
        // an inspector still wants to know what contains what.
        for (auto i = 0; i < data.nodes.size(); ++i)
            for (auto child: data.nodes[i].children)
                if (child >= 0 && child < data.nodes.size())
                    data.nodes[child].parent = i;

        visitScene();
    }

    void visitScene()
    {
        const auto* scenes = document.collection("scenes");
        auto sceneIndex = intOr(document.root(), "scene", 0);

        const auto* scene = at(scenes, sceneIndex);

        if (scene == nullptr)
            scene = at(scenes, 0);

        const auto* roots =
            scene != nullptr ? arrayMember(*scene, "nodes") : nullptr;

        // The scene's roots, or every parentless node when the file names no
        // scene. A node reached from no root keeps its identity transform and
        // stays out of drawOrder rather than being drawn at the origin.
        if (roots != nullptr)
        {
            for (auto i = 0; i < roots->size(); ++i)
                if (const auto& root = roots->get(i); root.isNumber())
                    visit((int) root.asNumber(), Mat4::identity(), 0);
        }
        else
        {
            for (auto i = 0; i < data.nodes.size(); ++i)
                if (data.nodes[i].parent < 0)
                    visit(i, Mat4::identity(), 0);
        }
    }

    // Composes a node's transform with its parents' and records it, depth
    // first, which is also the order the nodes are drawn in.
    //
    // `depth` is a cycle guard. glTF's node graph is meant to be a tree, but
    // nothing in the file format prevents a child list from pointing back up,
    // and an unguarded recursion on one is a stack overflow rather than an
    // error.
    void visit(int index, const Mat4& parentTransform, int depth)
    {
        if (index < 0 || index >= data.nodes.size() || depth > data.nodes.size())
            return;

        auto& node = data.nodes[index];
        node.worldTransform = parentTransform * node.transform;

        if (node.mesh >= 0 && node.mesh < data.meshes.size())
            data.drawOrder.add(index);

        for (auto child: node.children)
            visit(child, node.worldTransform, depth + 1);
    }

    void computeBounds()
    {
        auto seen = false;

        for (auto nodeIndex: data.drawOrder)
        {
            const auto& node = data.nodes[nodeIndex];

            for (auto primitiveIndex: data.meshes[node.mesh].primitives)
            {
                const auto& primitive = data.primitives[primitiveIndex];

                // Every corner, not just the two extremes: a rotated box's
                // minimum corner does not map onto the rotated box's minimum,
                // and using only two corners quietly shrinks the bounds a
                // camera frames the model with.
                for (auto corner = 0; corner < 8; ++corner)
                {
                    auto local = Vec3 {
                        (corner & 1) ? primitive.boundsMax.x : primitive.boundsMin.x,
                        (corner & 2) ? primitive.boundsMax.y : primitive.boundsMin.y,
                        (corner & 4) ? primitive.boundsMax.z
                                     : primitive.boundsMin.z};

                    auto world = node.worldTransform.transformPoint(local);

                    data.boundsMin = seen ? min(data.boundsMin, world) : world;
                    data.boundsMax = seen ? max(data.boundsMax, world) : world;
                    seen = true;
                }
            }
        }
    }

    const Document& document;
    MeshData data;
};

LoadResult buildFrom(std::span<const std::uint8_t> bytes, const FilePath& basePath)
{
    auto document = Document {};

    if (!document.parse(bytes, basePath))
        return {{}, document.error()};

    auto builder = Builder {document};
    auto data = builder.build();

    if (!data.isValid())
        return {{}, "the file holds no triangle geometry"};

    return {std::move(data), {}};
}
} // namespace

LoadResult loadGltf(const FilePath& path)
{
    auto contents = Files::readFile(path);

    if (contents.empty())
        return {{}, "could not read " + path.str()};

    auto bytes = std::span<const std::uint8_t> {
        reinterpret_cast<const std::uint8_t*>(contents.data()), contents.size()};

    // The parent directory, because that is what a .gltf's own relative URIs
    // resolve against - its buffers and images sit beside it, not beside the
    // working directory.
    return buildFrom(bytes, path.parentDirectory());
}

LoadResult
    loadGltfFromMemory(const void* bytes, std::size_t size, const FilePath& basePath)
{
    if (bytes == nullptr || size == 0)
        return {{}, "no data"};

    return buildFrom(
        std::span<const std::uint8_t> {static_cast<const std::uint8_t*>(bytes),
                                       size},
        basePath);
}
} // namespace eacp::Mesh
