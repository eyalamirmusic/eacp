#include "GlyphRenderer.h"

// No EACP_SHADER_VALUE declarations here: every field the shader reads is a
// plain float[N], which the EDSL already maps to FloatN. The macro is only
// needed for structs with named components (a Vec2 with .x and .y).

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

// One shader body for both atlases, with the coverage handling switched at
// build time: a mask supplies alpha from its single channel, a colour glyph
// carries its own RGBA.
struct GlyphRenderer::Program final : ShaderProgram
{
    explicit Program(bool coloredToUse)
        : colored(coloredToUse)
    {
        // Linear so a glyph drawn at a fractional position or a non-integer
        // zoom resamples smoothly rather than shimmering. Declared here rather
        // than on the atlas texture because the shader is what decides how it
        // samples - see GPU::TextureSampling.
        atlas.sampling = {TextureFilter::Linear, TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto corner = vertexInput(&GlyphQuadCorner::corner);
        auto rect = instanceInput(&GlyphInstance::rect, 1);
        auto source = instanceInput(&GlyphInstance::source, 1);
        auto color = instanceInput(&GlyphInstance::color, 1);

        // Unit corner -> destination rect, in logical points.
        auto position = float2(rect.x() + corner.x() * rect.z(),
                               rect.y() + corner.y() * rect.w());

        // Points -> clip space. Y is flipped because the geometry is authored
        // y-down, matching Graphics::Rect and every layout calculation above.
        auto clipX = position.x() / screenSize.x() * 2.0f - 1.0f;
        auto clipY = 1.0f - position.y() / screenSize.y() * 2.0f;

        setPosition(float4(clipX, clipY, 0.0f, 1.0f));

        // Texel rect -> normalised UV.
        auto uv = float2((source.x() + corner.x() * source.z()) / atlasSize.x(),
                         (source.y() + corner.y() * source.w()) / atlasSize.y());

        auto sampled = sample(atlas, varying(uv));
        auto tint = varying(color);

        if (colored)
        {
            // A colour glyph carries its own colour; the instance colour only
            // supplies alpha, so a faded emoji is still possible.
            setFragment(float4(
                sampled.x(), sampled.y(), sampled.z(), sampled.w() * tint.w()));
        }
        else
        {
            // The whole reason this shader exists. An R8Unorm sample arrives as
            // (coverage, 0, 0, 1): the coverage is in red and the alpha is a
            // meaningless 1. Multiplying that by a tint — which is what a
            // general sprite shader does — yields opaque red. Coverage has to
            // become *alpha* instead, leaving the colour untouched.
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

    program.screenSize = std::array {viewport.x, viewport.y};
    program.atlasSize = std::array {static_cast<float>(texture.width()),
                                    static_cast<float>(texture.height())};
    program.atlas = texture;

    program.setVertices(unitQuad);
    program.setInstances(1, queue.data(), static_cast<int>(queue.size()));

    pass.drawInstanced(program, static_cast<int>(queue.size()));
}

void GlyphRenderer::flush(RenderPass& pass, GlyphAtlas& atlas)
{
    if (!prepared)
    {
        // Blending is required: glyph coverage is an alpha ramp, and without it
        // the antialiased edges punch holes in whatever is behind them.
        maskProgram->prepare(
            1, false, PrimitiveTopology::Triangles, BlendMode::AlphaBlend);
        colorProgram->prepare(
            1, false, PrimitiveTopology::Triangles, BlendMode::AlphaBlend);
        prepared = true;
    }

    drawQueue(pass, masks, atlas.maskTexture(), false);
    drawQueue(pass, colors, atlas.colorTexture(), true);

    masks.clear();
    colors.clear();
}
} // namespace eacp::Text
