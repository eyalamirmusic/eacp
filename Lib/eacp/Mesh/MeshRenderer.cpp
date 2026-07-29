#include "MeshRenderer.h"

#include <algorithm>

namespace eacp::Mesh
{
namespace
{
// The order primitives are drawn in. Opaque first so it fills the depth buffer
// for everything after it, then masked, then blended - which has to come last
// because it does not write depth and so cannot sort against itself.
int drawOrderOf(AlphaMode mode)
{
    switch (mode)
    {
        case AlphaMode::Opaque:
            return 0;
        case AlphaMode::Mask:
            return 1;
        case AlphaMode::Blend:
            return 2;
    }

    return 0;
}

Vector<std::uint16_t> narrowed(const Vector<std::uint32_t>& indices)
{
    auto result = Vector<std::uint16_t> {};
    result.reserve(indices.size());

    for (auto index: indices)
        result.add((std::uint16_t) index);

    return result;
}
} // namespace

MeshRenderer::MeshRenderer(int sampleCount, GPU::PixelFormat colorFormat)
    : sampleCountValue(sampleCount)
    , colorFormatValue(colorFormat)
{
}

void MeshRenderer::setModel(const MeshData& data)
{
    items.clear();
    pipelines.clear();
    pipelineKeys.clear();
    textures.clear();
    worldTransforms.clear();
    vertexBuffer.reset();
    indexBuffer.reset();
    stats = {};

    if (!data.isValid())
        return;

    library.emplace(GPU::Device::shared(), shader.source());

    uploadGeometry(data);
    uploadTextures(data);
    buildDrawItems(data);
}

void MeshRenderer::uploadGeometry(const MeshData& data)
{
    vertexBuffer.emplace(GPU::Device::shared(),
                         data.vertices.data(),
                         sizeof(MeshVertex) * (std::size_t) data.vertices.size(),
                         GPU::BufferUsage::Vertex);

    // Half the index bandwidth whenever the model allows it, which is whenever
    // no single primitive passes 65536 vertices - see fitsNarrowIndices for why
    // that is a question about a primitive rather than about the model.
    if (fitsNarrowIndices(data))
    {
        auto narrow = narrowed(data.indices);

        indexFormat = GPU::IndexFormat::UInt16;
        indexBuffer.emplace(GPU::Device::shared(),
                            narrow.data(),
                            sizeof(std::uint16_t) * (std::size_t) narrow.size(),
                            GPU::BufferUsage::Index);
    }
    else
    {
        indexFormat = GPU::IndexFormat::UInt32;
        indexBuffer.emplace(GPU::Device::shared(),
                            data.indices.data(),
                            sizeof(std::uint32_t)
                                * (std::size_t) data.indices.size(),
                            GPU::BufferUsage::Index);
    }
}

void MeshRenderer::uploadTextures(const MeshData& data)
{
    auto white = std::uint8_t {0xff};
    const std::uint8_t whitePixel[4] {white, white, white, white};

    whiteTexture.emplace(GPU::Device::shared(),
                         GPU::TextureDescriptor {.width = 1, .height = 1},
                         whitePixel);

    for (const auto& image: data.images)
    {
        // An image that failed to decode still takes a slot, so a material's
        // index into this list is the index MeshData gave it. The slot holds
        // nothing and the material falls back to the white texture.
        if (!image.isValid())
        {
            textures.add(OwningPointer<GPU::Texture> {});
            continue;
        }

        auto descriptor = GPU::TextureDescriptor {
            .width = image.width(), .height = image.height(), .mipmapped = true};

        textures.createNew(GPU::Device::shared(), descriptor, image.pixels().data());
    }
}

int MeshRenderer::pipelineFor(const PipelineKey& key)
{
    for (auto i = 0; i < pipelineKeys.size(); ++i)
        if (pipelineKeys[i] == key)
            return i;

    auto descriptor = GPU::RenderPipelineDescriptor {};

    descriptor.library = &*library;
    descriptor.vertexLayout = shader.vertexLayout();
    descriptor.sampleCount = sampleCountValue;
    descriptor.colorFormat = colorFormatValue;
    descriptor.depth = true;

    descriptor.cullMode =
        key.doubleSided ? GPU::CullMode::None : GPU::CullMode::Back;

    // A mirrored node reverses the winding of every triangle under it, so the
    // face that was front is now back. Flipping which winding counts as front
    // undoes that for the whole subtree, and costs nothing; rewriting the
    // model's indices would cost a pass over all of them and a second copy of
    // the geometry.
    descriptor.frontFace =
        key.mirrored ? GPU::Winding::Clockwise : GPU::Winding::CounterClockwise;

    // Blended geometry tests against the depth already written and must not add
    // to it, or the nearer of two translucent surfaces hides the further one
    // instead of blending over it. Masked geometry does write: a cutout is
    // opaque wherever it survives, and sorts like opaque geometry.
    if (key.alphaMode == AlphaMode::Blend)
    {
        descriptor.blendMode = GPU::BlendMode::AlphaBlend;
        descriptor.depthWrite = false;
    }

    pipelines.createNew(GPU::Device::shared(), descriptor);
    pipelineKeys.add(key);

    return pipelines.size() - 1;
}

void MeshRenderer::buildDrawItems(const MeshData& data)
{
    worldTransforms.resize(data.nodes.size());

    for (auto i = 0; i < data.nodes.size(); ++i)
        worldTransforms[i] = data.nodes[i].worldTransform;

    for (auto nodeIndex: data.drawOrder)
    {
        const auto& node = data.nodes[nodeIndex];
        auto mirrored = node.worldTransform.linearDeterminant() < 0.0f;

        for (auto primitiveIndex: data.meshes[node.mesh].primitives)
        {
            const auto& primitive = data.primitives[primitiveIndex];

            auto item = DrawItem {};
            item.firstIndex = primitive.firstIndex;
            item.indexCount = primitive.indexCount;
            item.baseVertex = primitive.baseVertex;
            item.node = nodeIndex;

            auto key = PipelineKey {};
            key.mirrored = mirrored;

            // glTF's default material, for a primitive that names none: opaque,
            // single-sided, white. Reading it off the defaults in Material
            // rather than repeating them here.
            auto material = primitive.material >= 0
                                ? data.materials[primitive.material]
                                : Material {};

            for (auto c = 0; c < 4; ++c)
                item.baseColor[c] = material.baseColor[c];

            key.alphaMode = material.alphaMode;
            key.doubleSided = material.doubleSided;

            item.alphaCutoff =
                material.alphaMode == AlphaMode::Mask ? material.alphaCutoff : 0.0f;

            if (material.baseColorImage >= 0
                && material.baseColorImage < textures.size()
                && textures[material.baseColorImage] != nullptr)
                item.texture = textures[material.baseColorImage];

            item.pipeline = pipelineFor(key);

            items.add(item);
        }
    }

    // Stable by alpha mode, so opaque fills the depth buffer before anything
    // tests against it and blended geometry draws last. Within a mode the file's
    // own order survives, which is the only order a scene without a camera can
    // sort blended surfaces by.
    auto& raw = items.getVector();

    std::stable_sort(raw.begin(),
                     raw.end(),
                     [this](const DrawItem& a, const DrawItem& b)
                     {
                         return drawOrderOf(pipelineKeys[a.pipeline].alphaMode)
                                < drawOrderOf(pipelineKeys[b.pipeline].alphaMode);
                     });
}

void MeshRenderer::draw(GPU::RenderPass& pass, const RenderOptions& options)
{
    stats = {};

    if (!hasModel() || items.size() == 0)
        return;

    pass.setVertexBuffer(*vertexBuffer);

    shader.shadingMode = (std::int32_t) options.shading;
    shader.lightDirection = normalize(options.lightDirection);

    auto viewProjection = options.projection * options.view;
    auto boundPipeline = -1;

    for (const auto& item: items)
    {
        if (item.pipeline != boundPipeline)
        {
            pass.setPipeline(*pipelines[item.pipeline]);
            boundPipeline = item.pipeline;
            ++stats.pipelineSwitches;

            // Both backends reset the bound vertex buffer with the pipeline on
            // at least one path, and re-binding it is a no-op where they do not.
            pass.setVertexBuffer(*vertexBuffer);
        }

        auto model = options.modelTransform * worldTransforms[item.node];

        shader.modelViewProjection = viewProjection * model;
        shader.normalTransform = model.normalMatrix();
        shader.baseColor = Array {item.baseColor[0],
                                  item.baseColor[1],
                                  item.baseColor[2],
                                  item.baseColor[3]};
        shader.alphaCutoff = item.alphaCutoff;

        const auto& texture =
            item.texture != nullptr ? *item.texture : *whiteTexture;
        pass.setFragmentTexture(texture, 0, meshSampling);

        // Hand-rolled rather than pass.draw(shader): one program draws every
        // primitive, and draw(program) would re-bind the pipeline and the vertex
        // buffer for each of them.
        pass.setUniforms(shader);

        // The base vertex is what lets every primitive's indices start from zero
        // in one shared buffer, which is the whole reason they are still 16 bits
        // wide on a model with far more than 65536 vertices in it.
        pass.drawIndexed(*indexBuffer,
                         item.indexCount,
                         indexFormat,
                         item.firstIndex,
                         item.baseVertex);

        ++stats.drawCalls;
        stats.triangles += item.indexCount / 3;
    }
}
} // namespace eacp::Mesh
