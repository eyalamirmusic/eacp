#include "GltfLoader.h"

#include <eacp/Core/Utils/Files.h>

#include <cgltf.h>

#include <cstring>

namespace eacp::Mesh
{
namespace
{
std::string describe(cgltf_result result)
{
    switch (result)
    {
        case cgltf_result_data_too_short:
            return "the file ends part-way through data it declares";
        case cgltf_result_unknown_format:
            return "not a glTF or GLB file";
        case cgltf_result_invalid_json:
            return "the JSON is malformed";
        case cgltf_result_invalid_gltf:
            return "valid JSON, but not valid glTF";
        case cgltf_result_out_of_memory:
            return "out of memory";
        case cgltf_result_legacy_gltf:
            return "glTF 1.0, which this loader does not read";
        case cgltf_result_file_not_found:
            return "file not found";
        case cgltf_result_io_error:
            return "the file could not be read";
        case cgltf_result_invalid_options:
            return "invalid loader options";
        default:
            return "unknown error";
    }
}

std::string nameOf(const char* name, const char* fallback, int index)
{
    if (name != nullptr && name[0] != '\0')
        return name;

    return std::string {fallback} + " " + std::to_string(index);
}

Mat4 localTransform(const cgltf_node& node)
{
    // cgltf writes column-major, which is Mat4's own layout, so this is a copy
    // rather than a transpose. It also resolves has_matrix against the TRS
    // fields, which is the one place the two forms a node may be written in
    // have to agree.
    auto result = Mat4 {};
    cgltf_node_transform_local(&node, result.values);
    return result;
}

AlphaMode toAlphaMode(cgltf_alpha_mode mode)
{
    switch (mode)
    {
        case cgltf_alpha_mode_mask:
            return AlphaMode::Mask;
        case cgltf_alpha_mode_blend:
            return AlphaMode::Blend;
        default:
            return AlphaMode::Opaque;
    }
}

// The bytes of a glTF image, wherever it keeps them: inside the binary chunk,
// inline as a base64 data URI, or in a file beside the .gltf.
//
// cgltf_load_buffers resolves data URIs for *buffers* only, so the inline image
// case is decoded here rather than being one that quietly yields no texture.
Graphics::ImageData imageBytes(const cgltf_options& options,
                               const cgltf_image& image,
                               const FilePath& basePath)
{
    if (image.buffer_view != nullptr)
    {
        const auto* start = cgltf_buffer_view_data(image.buffer_view);

        if (start == nullptr)
            return {};

        auto bytes = Graphics::ImageData {};
        bytes.getVector().assign(start, start + image.buffer_view->size);
        return bytes;
    }

    if (image.uri == nullptr)
        return {};

    auto uri = std::string {image.uri};

    if (uri.starts_with("data:"))
    {
        auto comma = uri.find(',');

        if (comma == std::string::npos)
            return {};

        const auto* base64 = uri.c_str() + comma + 1;
        auto encoded = uri.size() - comma - 1;

        // Four base64 characters carry three bytes, less whatever the padding
        // stands in for. cgltf's decoder needs the exact output size up front,
        // having no way to tell a caller how much it produced.
        auto padding = std::size_t {0};

        while (padding < 2 && encoded > padding
               && base64[encoded - 1 - padding] == '=')
            ++padding;

        auto decodedSize = encoded / 4 * 3 - padding;
        void* decoded = nullptr;

        if (cgltf_load_buffer_base64(&options, decodedSize, base64, &decoded)
            != cgltf_result_success)
            return {};

        auto bytes = Graphics::ImageData {};
        const auto* start = static_cast<const std::uint8_t*>(decoded);
        bytes.getVector().assign(start, start + decodedSize);

        options.memory.free_func(options.memory.user_data, decoded);
        return bytes;
    }

    // A relative path, percent-escaped the way a URI is - so "chair%20base.png"
    // is a file whose name has a space in it, and reading it unescaped finds
    // nothing.
    cgltf_decode_uri(uri.data());
    uri.resize(std::strlen(uri.c_str()));

    if (basePath.empty())
        return {};

    auto contents = Files::readFile(basePath / uri);

    auto bytes = Graphics::ImageData {};
    bytes.getVector().assign(contents.begin(), contents.end());
    return bytes;
}

class Builder
{
public:
    Builder(const cgltf_options& optionsToUse,
            const cgltf_data& sourceToUse,
            const FilePath& basePathToUse)
        : options(optionsToUse)
        , source(sourceToUse)
        , basePath(basePathToUse)
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
        for (auto i = cgltf_size {0}; i < source.images_count; ++i)
        {
            auto bytes = imageBytes(options, source.images[i], basePath);

            // An image that fails to decode still takes a slot, so every
            // material's index into this list stays the index cgltf gave it.
            // Dropping it would silently repoint every later material at the
            // wrong texture.
            data.images.add(bytes.size() > 0 ? Graphics::Image::decode(bytes)
                                             : Graphics::Image {});
        }
    }

    int imageIndexFor(const cgltf_texture_view& view) const
    {
        if (view.texture == nullptr || view.texture->image == nullptr)
            return -1;

        return (int) cgltf_image_index(&source, view.texture->image);
    }

    void loadMaterials()
    {
        for (auto i = cgltf_size {0}; i < source.materials_count; ++i)
        {
            const auto& from = source.materials[i];
            auto material = Material {};

            material.name = nameOf(from.name, "material", (int) i);
            material.alphaMode = toAlphaMode(from.alpha_mode);
            material.alphaCutoff = from.alpha_cutoff;
            material.doubleSided = from.double_sided != 0;

            // Everything outside metallic-roughness keeps the defaults, which
            // are opaque white rather than zero - see Material. A specular-glossy
            // or unlit material still draws, in its own base colour if it has
            // one and in white if it does not.
            if (from.has_pbr_metallic_roughness)
            {
                const auto& pbr = from.pbr_metallic_roughness;

                for (auto c = 0; c < 4; ++c)
                    material.baseColor[c] = pbr.base_color_factor[c];

                material.baseColorImage = imageIndexFor(pbr.base_color_texture);
            }

            data.materials.add(std::move(material));
        }
    }

    void loadMeshes()
    {
        for (auto i = cgltf_size {0}; i < source.meshes_count; ++i)
        {
            const auto& from = source.meshes[i];
            auto mesh = Mesh {};

            mesh.name = nameOf(from.name, "mesh", (int) i);

            for (auto p = cgltf_size {0}; p < from.primitives_count; ++p)
            {
                // Points, lines and the strip/fan topologies are skipped rather
                // than triangulated. A file mixing them with triangles still
                // loads and draws its triangles, which is more useful than
                // refusing the whole model.
                if (from.primitives[p].type != cgltf_primitive_type_triangles)
                    continue;

                if (auto index = loadPrimitive(from.primitives[p]); index >= 0)
                    mesh.primitives.add(index);
            }

            data.meshes.add(std::move(mesh));
        }
    }

    int loadPrimitive(const cgltf_primitive& from)
    {
        const cgltf_accessor* positions = nullptr;
        const cgltf_accessor* normals = nullptr;
        const cgltf_accessor* uvs = nullptr;
        const cgltf_accessor* colors = nullptr;

        for (auto a = cgltf_size {0}; a < from.attributes_count; ++a)
        {
            const auto& attribute = from.attributes[a];

            // Only set 0 of the multi-set attributes. A second UV set belongs to
            // a texture this slice does not read.
            if (attribute.index != 0)
                continue;

            switch (attribute.type)
            {
                case cgltf_attribute_type_position:
                    positions = attribute.data;
                    break;
                case cgltf_attribute_type_normal:
                    normals = attribute.data;
                    break;
                case cgltf_attribute_type_texcoord:
                    uvs = attribute.data;
                    break;
                case cgltf_attribute_type_color:
                    colors = attribute.data;
                    break;
                default:
                    break;
            }
        }

        if (positions == nullptr || positions->count == 0)
            return -1;

        auto primitive = Primitive {};
        primitive.baseVertex = data.vertices.size();
        primitive.firstIndex = data.indices.size();
        primitive.material = from.material != nullptr
                                 ? (int) cgltf_material_index(&source, from.material)
                                 : -1;

        appendVertices(*positions, normals, uvs, colors);
        primitive.indexCount = appendIndices(from, (int) positions->count);

        if (normals == nullptr)
            generateNormals(primitive);

        computePrimitiveBounds(primitive, (int) positions->count);

        data.primitives.add(primitive);
        return data.primitives.size() - 1;
    }

    void appendVertices(const cgltf_accessor& positions,
                        const cgltf_accessor* normals,
                        const cgltf_accessor* uvs,
                        const cgltf_accessor* colors)
    {
        for (auto i = cgltf_size {0}; i < positions.count; ++i)
        {
            auto vertex = MeshVertex {};

            float position[3] {};
            cgltf_accessor_read_float(&positions, i, position, 3);

            for (auto c = 0; c < 3; ++c)
                vertex.position[c] = position[c];

            if (normals != nullptr && i < normals->count)
            {
                float normal[3] {};
                cgltf_accessor_read_float(normals, i, normal, 3);
                auto unit = normalize(Vec3 {normal[0], normal[1], normal[2]});
                vertex.normal = GPU::SNorm16x4::from(unit.x, unit.y, unit.z, 0.0f);
            }

            if (uvs != nullptr && i < uvs->count)
            {
                float uv[2] {};
                cgltf_accessor_read_float(uvs, i, uv, 2);
                vertex.uv = GPU::Float16x2::from(uv[0], uv[1]);
            }

            // COLOR_0 may be vec3 or vec4, and read_float only writes as many
            // components as the accessor has - so alpha is seeded opaque here
            // rather than left at whatever the stack held.
            float color[4] {1.0f, 1.0f, 1.0f, 1.0f};

            if (colors != nullptr && i < colors->count)
                cgltf_accessor_read_float(colors, i, color, 4);

            vertex.color =
                GPU::UNorm8x4::fromFloats(color[0], color[1], color[2], color[3]);

            data.vertices.add(vertex);
        }
    }

    // Returns the number of indices written. They are relative to the
    // primitive's own first vertex - never rebased into the shared buffer - so
    // the width they need is set by the largest primitive rather than by the
    // model. See fitsNarrowIndices.
    int appendIndices(const cgltf_primitive& from, int vertexCount)
    {
        if (from.indices != nullptr)
        {
            auto count = (int) from.indices->count;

            for (auto i = 0; i < count; ++i)
                data.indices.add((std::uint32_t) cgltf_accessor_read_index(
                    from.indices, (cgltf_size) i));

            return count;
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

            // Not normalized: the cross product's length is twice the triangle's
            // area, which is exactly the weight a big face should carry over a
            // sliver sharing the same vertex.
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
        for (auto i = cgltf_size {0}; i < source.nodes_count; ++i)
        {
            const auto& from = source.nodes[i];
            auto node = Node {};

            node.name = nameOf(from.name, "node", (int) i);
            node.transform = localTransform(from);
            node.mesh = from.mesh != nullptr
                            ? (int) cgltf_mesh_index(&source, from.mesh)
                            : -1;
            node.parent = from.parent != nullptr
                              ? (int) cgltf_node_index(&source, from.parent)
                              : -1;

            for (auto c = cgltf_size {0}; c < from.children_count; ++c)
                node.children.add((int) cgltf_node_index(&source, from.children[c]));

            data.nodes.add(std::move(node));
        }

        // The scene's roots, or every parentless node when the file names no
        // scene. A node reached from no root keeps its identity transform and
        // stays out of drawOrder rather than being drawn at the origin.
        const auto* scene = source.scene != nullptr   ? source.scene
                            : source.scenes_count > 0 ? &source.scenes[0]
                                                      : nullptr;

        if (scene != nullptr)
        {
            for (auto i = cgltf_size {0}; i < scene->nodes_count; ++i)
                visit((int) cgltf_node_index(&source, scene->nodes[i]),
                      Mat4::identity());
        }
        else
        {
            for (auto i = 0; i < data.nodes.size(); ++i)
                if (data.nodes[i].parent < 0)
                    visit(i, Mat4::identity());
        }
    }

    // Composes a node's transform with its parents' and records it, depth
    // first, which is also the order the nodes are drawn in.
    void visit(int index, const Mat4& parentTransform)
    {
        if (index < 0 || index >= data.nodes.size())
            return;

        auto& node = data.nodes[index];
        node.worldTransform = parentTransform * node.transform;

        if (node.mesh >= 0)
            data.drawOrder.add(index);

        for (auto child: node.children)
            visit(child, node.worldTransform);
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
                // and using only two corners quietly shrinks the bounds a camera
                // frames the model with.
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

    const cgltf_options& options;
    const cgltf_data& source;
    const FilePath& basePath;

    MeshData data;
};

LoadResult buildFrom(const cgltf_options& options,
                     cgltf_data& parsed,
                     const FilePath& basePath)
{
    if (auto result = cgltf_validate(&parsed); result != cgltf_result_success)
        return {{}, "invalid glTF: " + describe(result)};

    auto builder = Builder {options, parsed, basePath};
    auto data = builder.build();

    if (!data.isValid())
        return {{}, "the file holds no triangle geometry"};

    return {std::move(data), {}};
}
} // namespace

LoadResult loadGltf(const FilePath& path)
{
    auto options = cgltf_options {};
    cgltf_data* parsed = nullptr;

    if (auto result = cgltf_parse_file(&options, path.c_str(), &parsed);
        result != cgltf_result_success)
        return {{}, "could not read " + path.str() + ": " + describe(result)};

    // cgltf resolves a .gltf's external buffers against the path it was given,
    // not against the working directory - so this is the same path, not the
    // parent.
    if (auto result = cgltf_load_buffers(&options, parsed, path.c_str());
        result != cgltf_result_success)
    {
        cgltf_free(parsed);
        return {{}, "could not read the model's buffers: " + describe(result)};
    }

    auto loaded = buildFrom(options, *parsed, path.parentDirectory());
    cgltf_free(parsed);
    return loaded;
}

LoadResult
    loadGltfFromMemory(const void* bytes, std::size_t size, const FilePath& basePath)
{
    auto options = cgltf_options {};
    cgltf_data* parsed = nullptr;

    if (auto result = cgltf_parse(&options, bytes, size, &parsed);
        result != cgltf_result_success)
        return {{}, "could not parse the model: " + describe(result)};

    // A trailing separator so cgltf joins onto the directory rather than
    // replacing its last component, which is what it does with a file path.
    auto bufferBase = basePath.empty() ? std::string {} : basePath.str() + "/";

    if (auto result = cgltf_load_buffers(
            &options, parsed, bufferBase.empty() ? nullptr : bufferBase.c_str());
        result != cgltf_result_success)
    {
        cgltf_free(parsed);
        return {{}, "could not read the model's buffers: " + describe(result)};
    }

    auto loaded = buildFrom(options, *parsed, basePath);
    cgltf_free(parsed);
    return loaded;
}
} // namespace eacp::Mesh
