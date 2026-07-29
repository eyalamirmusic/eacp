#pragma once

#include "MeshData.h"
#include "MeshShader.h"

#include <optional>

// A loaded model, uploaded once and drawn every frame.
//
// The geometry is static, so it goes into ordinary GPU::Buffers rather than
// StreamingBuffers: it is written at upload and never rewritten, which is the
// case Buffer::update was always right for. What changes per draw is the
// transform and the material, and those go through RenderPass::setUniforms -
// setVertexBytes on Metal, a transient constant buffer on D3D12 - which is the
// path built for exactly that.
//
// One vertex buffer and one index buffer hold the whole model. Each primitive
// keeps its own base vertex, so its indices start from zero and stay narrow even
// on a model far past 65536 vertices - the mechanism §1.1 of imgui-eacp's
// EACP_GPU_PLAN.md set out to add, and the reason this type does not have to
// rebase anything as it uploads.

namespace eacp::Mesh
{
struct RenderOptions
{
    // The whole model's transform, applied above every node's own.
    Mat4 modelTransform = Mat4::identity();

    Mat4 view = Mat4::identity();
    Mat4 projection = Mat4::identity();

    ShadingMode shading = ShadingMode::Lambert;

    // World-space direction the light travels *from*. The renderer's caller
    // usually points this at the camera, so the lit side is the side being
    // looked at.
    Vec3 lightDirection {0.35f, 0.6f, 0.7f};
};

// What one draw() cost, for an app that reports it. Counted rather than timed:
// the GPU-side answer is what Device::lastFrameTimings() gives a labelled pass,
// and a second CPU clock around encoding would say less than these do.
struct RenderStats
{
    int drawCalls = 0;
    int triangles = 0;

    // How many times the pipeline changed within one draw(). Primitives are
    // ordered so this stays near the number of distinct material kinds rather
    // than rising with the number of primitives.
    int pipelineSwitches = 0;
};

class MeshRenderer
{
public:
    // Both must match the target the draws land in, since the pipelines are
    // built here: a view's own sample count and its drawable's format, or the
    // texture's when the model is drawn into one (pixelFormatFor(its format)).
    // Neither backend takes a draw whose pipeline disagrees with its
    // attachment.
    //
    // The target also has to have a depth buffer - GPUView::setDepth(true), or
    // TextureDescriptor::depth. That is not optional here: without it the model
    // draws in whatever order its nodes are listed in, which is painter's order
    // and wrong for anything convex.
    explicit MeshRenderer(
        int sampleCount,
        GPU::PixelFormat colorFormat = GPU::PixelFormat::BGRA8Unorm);

    // Uploads a model's geometry and textures. Replaces whatever was loaded
    // before, and must run with no encoder open - so before Frame::beginPass,
    // for the same reason DrawRenderer::prepare does.
    void setModel(const MeshData& data);

    bool hasModel() const { return vertexBuffer.has_value(); }

    // Records the whole model. Opaque geometry first, then masked, then blended
    // - which is the order alpha needs and the reason the material's alpha mode
    // picks a pipeline rather than only a uniform.
    void draw(GPU::RenderPass& pass, const RenderOptions& options);

    const RenderStats& lastStats() const { return stats; }

    // Every texture the model uploaded, in the order MeshData listed its images,
    // with an invalid texture wherever an image failed to decode. Exposed so an
    // inspector can show what a material actually got.
    const OwnedVector<GPU::Texture>& modelTextures() const { return textures; }

private:
    // A primitive plus everything about it the draw loop needs, resolved once at
    // upload rather than chased through three vectors every frame.
    struct DrawItem
    {
        int firstIndex = 0;
        int indexCount = 0;
        int baseVertex = 0;

        // Which node this came from, kept so an inspector can name a draw. The
        // transform itself is looked up rather than copied here, because a model
        // whose nodes move only has to have that one vector rewritten.
        int node = 0;
        int pipeline = 0;

        const GPU::Texture* texture = nullptr;

        float baseColor[4] {1.0f, 1.0f, 1.0f, 1.0f};
        float alphaCutoff = 0.0f;
    };

    // The pipeline settings a material implies. Kept as a value so the set of
    // distinct ones can be found by comparison, and one pipeline built per
    // distinct answer instead of one per primitive.
    struct PipelineKey
    {
        AlphaMode alphaMode = AlphaMode::Opaque;
        bool doubleSided = false;

        // A node whose transform mirrors reverses the winding of every triangle
        // under it, so front and back swap. Flipping the pipeline's front face
        // is the fix; rewriting the model's indices is not.
        bool mirrored = false;

        bool operator==(const PipelineKey&) const = default;
    };

    void uploadGeometry(const MeshData& data);
    void uploadTextures(const MeshData& data);
    void buildDrawItems(const MeshData& data);

    int pipelineFor(const PipelineKey& key);

    MeshShader shader;
    int sampleCountValue = 1;
    GPU::PixelFormat colorFormatValue = GPU::PixelFormat::BGRA8Unorm;

    // One library, many pipelines. A cull mode, a blend mode and a depth-write
    // flag live on the pipeline rather than on the shader, so a model with an
    // opaque material, a double-sided one and a translucent one needs three
    // pipeline states - but all three are the same compiled code, and building
    // them through three ShaderPrograms would compile it three times.
    std::optional<GPU::ShaderLibrary> library;
    OwnedVector<GPU::RenderPipeline> pipelines;
    Vector<PipelineKey> pipelineKeys;

    std::optional<GPU::Buffer> vertexBuffer;
    std::optional<GPU::Buffer> indexBuffer;
    GPU::IndexFormat indexFormat = GPU::IndexFormat::UInt16;

    OwnedVector<GPU::Texture> textures;

    // Bound for every material without a base colour texture, so the shader has
    // no branch and no unbound slot. One opaque white texel.
    std::optional<GPU::Texture> whiteTexture;

    Vector<DrawItem> items;
    Vector<Mat4> worldTransforms;

    RenderStats stats;
};
} // namespace eacp::Mesh
