#include "GlyphRenderer.h"

// No EACP_SHADER_VALUE declarations: every field the shader reads is a plain
// float[N], which the EDSL already maps to FloatN.

namespace eacp::Text
{
using namespace eacp::GPU;

namespace
{
constexpr GlyphQuadCorner unitQuad[] = {
    {{0.f, 0.f}},
    {{1.f, 0.f}},
    {{0.f, 1.f}},
    {{1.f, 0.f}},
    {{1.f, 1.f}},
    {{0.f, 1.f}},
};
} // namespace

// One shader body for both atlases, the coverage handling switched at build
// time.
struct GlyphRenderer::Program final : ShaderProgram
{
    explicit Program(bool coloredToUse)
        : colored(coloredToUse)
    {
        // Linear, so a glyph at a fractional position resamples smoothly rather
        // than shimmering. On the shader, not the texture: see TextureSampling.
        atlas.sampling = {TextureFilter::Linear, TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto corner = vertexInput(&GlyphQuadCorner::corner);
        auto rect = instanceInput(&GlyphInstance::rect, 1);
        auto source = instanceInput(&GlyphInstance::source, 1);
        auto color = instanceInput(&GlyphInstance::color, 1);

        // In logical points.
        auto position = float2(rect.x() + corner.x() * rect.z(),
                               rect.y() + corner.y() * rect.w());

        // Points to clip space, Y flipped: the geometry is authored y-down.
        auto clipX = position.x() / screenSize.x() * 2.0f - 1.0f;
        auto clipY = 1.0f - position.y() / screenSize.y() * 2.0f;

        setPosition(float4(clipX, clipY, 0.0f, 1.0f));

        // Texels to normalised UV.
        auto uv = float2((source.x() + corner.x() * source.z()) / atlasSize.x(),
                         (source.y() + corner.y() * source.w()) / atlasSize.y());

        auto sampled = sample(atlas, varying(uv));
        auto tint = varying(color);

        if (colored)
        {
            // The instance colour supplies alpha only, so a faded emoji works.
            setFragment(float4(
                sampled.x(), sampled.y(), sampled.z(), sampled.w() * tint.w()));
        }
        else
        {
            // An R8Unorm sample arrives as (coverage, 0, 0, 1), so multiplying
            // it by the tint would give opaque red: coverage becomes the alpha.
            auto coverage = sampled.x();

            setFragment(float4(tint.x(), tint.y(), tint.z(), tint.w() * coverage));
        }
    }

    Uniform<Float2> screenSize;
    Uniform<Float2> atlasSize;
    Uniform<Texture2D> atlas;

    EACP_SHADER(screenSize, atlasSize, atlas)

    bool colored = false;
};

GlyphRenderer::GlyphRenderer()
    : maskProgram(makeOwned<Program>(false))
    , colorProgram(makeOwned<Program>(true))
{
}

GlyphRenderer::~GlyphRenderer() = default;

void GlyphRenderer::setViewportSize(Graphics::Point size)
{
    viewport = {size.x > 0.f ? size.x : 1.f, size.y > 0.f ? size.y : 1.f};
}

void GlyphRenderer::begin()
{
    masks.clear();
    colors.clear();
}

void GlyphRenderer::add(const Graphics::Rect& destination,
                        const Graphics::Rect& source,
                        const Graphics::Color& color,
                        bool colored)
{
    if (destination.w <= 0.f || destination.h <= 0.f)
        return;

    auto instance = GlyphInstance {};

    instance.rect[0] = destination.x;
    instance.rect[1] = destination.y;
    instance.rect[2] = destination.w;
    instance.rect[3] = destination.h;

    instance.source[0] = source.x;
    instance.source[1] = source.y;
    instance.source[2] = source.w;
    instance.source[3] = source.h;

    instance.color[0] = color.r;
    instance.color[1] = color.g;
    instance.color[2] = color.b;
    instance.color[3] = color.a;

    (colored ? colors : masks).push_back(instance);
}

void GlyphRenderer::drawQueue(RenderPass& pass,
                              std::vector<GlyphInstance>& queue,
                              Texture& texture,
                              bool colored)
{
    if (queue.empty())
        return;

    auto& program = colored ? *colorProgram : *maskProgram;

    program.screenSize = Array {viewport.x, viewport.y};
    program.atlasSize = Array {static_cast<float>(texture.width()),
                               static_cast<float>(texture.height())};
    program.atlas = texture;

    program.setInstances(1, queue.data(), static_cast<int>(queue.size()));

    pass.drawInstanced(program, static_cast<int>(queue.size()));
}

void GlyphRenderer::flush(RenderPass& pass, GlyphAtlas& atlas)
{
    if (!prepared)
    {
        // Glyph coverage is an alpha ramp, so without blending the antialiased
        // edges punch holes in what is behind them. The mode accumulates the
        // destination's alpha, for text drawn into a texture that is then faded.
        maskProgram->prepare(1,
                             false,
                             PrimitiveTopology::Triangles,
                             BlendMode::AlphaBlendOntoTransparent);
        colorProgram->prepare(1,
                              false,
                              PrimitiveTopology::Triangles,
                              BlendMode::AlphaBlendOntoTransparent);

        // The quad never changes, so it is uploaded once rather than per draw --
        // where it cost two committed resources and a queue submission on D3D12.
        maskProgram->setVertices(unitQuad);
        colorProgram->setVertices(unitQuad);

        prepared = true;
    }

    drawQueue(pass, masks, atlas.maskTexture(), false);
    drawQueue(pass, colors, atlas.colorTexture(), true);

    masks.clear();
    colors.clear();
}
} // namespace eacp::Text
