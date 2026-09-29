#pragma once

#include <eacp/GPU/GPU.h>

#include <algorithm>

// The one shader everything in this example is drawn with: an instanced
// rounded rectangle that is also a drop shadow, a hover glow and a travelling
// sheen, so a whole menu — panel, shadow, highlights and all — is one draw.
//
// The distance field is the one UI::ShapeBatch uses (Lib/eacp/UI/Render/
// ShapeBatch.cpp): the box shrunk by its corner radius and let back out by it,
// negative inside and positive outside, read once as a hard edge and once
// through a ramp as wide as the blur. What is added here is the pair of
// animated terms a menu wants and a rectangle does not — a hovered item eases
// toward an accent colour, and a bright band travels across it.
//
// Antialiasing is analytic, so the views set a sample count of 1: the
// fragment's own distance from the edge is its coverage, which is smoother at
// any corner radius than multisampling and costs no second target.

namespace PopupMenu
{
// A unit-quad corner, each component 0 or 1, mapped onto every shape's box.
struct PanelVertex
{
    float corner[2];
};

// One shape. Everything that varies from shape to shape lives here, so a run
// of them is a single instanced draw rather than a draw apiece.
struct PanelInstance
{
    // The box, in the view's own points: where its centre is and half its
    // size. The field is measured against this.
    float center[2] = {};
    float halfSize[2] = {};

    // Half the quad actually drawn — the box plus the margin its edge ramp
    // needs to land in, which is a pixel for a hard edge and the whole blur
    // for a shadow.
    float halfExtent[2] = {};

    // Where the shape casting this shadow sits, measured from this box's
    // centre, so the shadow can be cut out of it. Zero — and unread — for
    // everything that is not a shadow.
    float casterOffset[2] = {};

    float color[4] = {};

    // Where a hovered shape is headed, and how far it gets at full hover. A
    // highlight that eases toward a colour reads as a menu; one that eases
    // toward white reads as a bug.
    float accent[4] = {};

    // Corner radius in points, one over twice the shadow blur (zero for a
    // hard edge), how hovered this shape is (0 to 1), and how far the sheen
    // band has travelled across it, in turns. Everything but the first two is
    // left at zero by a shape that is not a highlight.
    float style[4] = {};
};

struct PanelShader final : eacp::GPU::ShaderProgram
{
    PanelShader() { compile(); }

    void define() override
    {
        auto corner = vertexInput(&PanelVertex::corner);

        auto center = instanceInput(&PanelInstance::center, 1);
        auto halfSize = instanceInput(&PanelInstance::halfSize, 1);
        auto halfExtent = instanceInput(&PanelInstance::halfExtent, 1);
        auto casterOffset = instanceInput(&PanelInstance::casterOffset, 1);
        auto color = instanceInput(&PanelInstance::color, 1);
        auto accent = instanceInput(&PanelInstance::accent, 1);
        auto style = instanceInput(&PanelInstance::style, 1);

        // Where this fragment sits inside the box, in points, measured from
        // its centre. The quad spans the grown extent, so this runs past
        // halfSize by the margin — which is exactly the band the ramp needs.
        auto offset = (corner - 0.5f) * (halfExtent * 2.f);
        auto position = center + offset;

        auto clipX = position.x() / screenSize.x() * 2.f - 1.f;
        auto clipY = 1.f - position.y() / screenSize.y() * 2.f;
        setPosition(eacp::GPU::float4(clipX, clipY, 0.f, 1.f));

        auto local = varying(offset);
        auto fragHalfSize = varying(halfSize);
        auto fragCaster = varying(casterOffset);
        auto fragColor = varying(color);
        auto fragAccent = varying(accent);
        auto fragStyle = varying(style);

        auto radius = fragStyle.x();
        auto shadowRamp = fragStyle.y();
        auto hover = fragStyle.z();
        auto sheen = fragStyle.w();

        auto roundedBox = [](auto point, auto half, auto cornerRadius)
        {
            auto q =
                abs(point) - (half - eacp::GPU::float2(cornerRadius, cornerRadius));
            auto outside = length(max(0.f, q));
            auto inside = min(0.f, max(q.x(), q.y()));

            return outside + inside - cornerRadius;
        };

        auto distance = roundedBox(local, fragHalfSize, radius);

        // Half a device pixel each side of the edge, which is the widest ramp
        // that still reads as an edge rather than a blur.
        auto hardEdge = clamp(0.5f - distance * pixelScale, 0.f, 1.f);

        // The same field read through a curve instead of a line, which is what
        // makes a shadow a blur rather than a fade.
        auto ramp = [&](auto edgeDistance)
        {
            return smoothstep(
                0.f, 1.f, clamp(0.5f - edgeDistance * shadowRamp, 0.f, 1.f));
        };

        // Along the box's own axes and multiplied, because a blurred rectangle
        // is what it blurs to in x times what it blurs to in y — the Gaussian
        // being separable. Off the field alone the corners come out too
        // bright, every direction out of one looking like an edge.
        auto axes = ramp(abs(local.x()) - fragHalfSize.x())
                    * ramp(abs(local.y()) - fragHalfSize.y());

        // The shape that casts the shadow, knocked out of it: the panel over
        // it is translucent, and a shadow left under its body would show
        // through as a dark rectangle instead of the view behind the window.
        auto casterDistance = roundedBox(local - fragCaster, fragHalfSize, radius);
        auto casterCoverage = clamp(0.5f - casterDistance * pixelScale, 0.f, 1.f);

        auto blurred = min(ramp(distance), axes) * (1.f - casterCoverage);

        // Mixed rather than branched, so a card and the shadow under it go out
        // in one instanced draw.
        auto coverage = mix(hardEdge, blurred, step(0.0001f, shadowRamp));

        // Diagonally across the box, 0 at the top-left corner and 1 at the
        // bottom-right, so the sheen travels rather than sweeping straight
        // down. The span is never zero: nothing queues a box with no size.
        auto span = (fragHalfSize.x() + fragHalfSize.y()) * 2.f;
        auto across = (local.x() + local.y()) / span + 0.5f;

        // The band's head runs from just off one corner to just off the other,
        // so it enters and leaves rather than appearing in the middle.
        auto head = fract(sheen) * 1.4f - 0.2f;
        auto band = 1.f - smoothstep(0.f, 0.17f, abs(across - head));

        auto tinted = mix(fragColor.xyz(), fragAccent.xyz(), hover * fragAccent.w());

        // The band only exists where the shape is hovered, so a resting item
        // is flat and the one under the pointer is the only thing moving.
        auto lift = hover * band * 0.22f;
        auto lit = tinted + eacp::GPU::float3(lift, lift, lift);

        setFragment(
            eacp::GPU::float4(lit.x(), lit.y(), lit.z(), fragColor.w() * coverage));
    }

    eacp::GPU::Uniform<eacp::GPU::Float2> screenSize;
    eacp::GPU::Uniform<eacp::GPU::Float> pixelScale;

    EACP_SHADER(screenSize, pixelScale)
};

// Collects shapes for a frame and draws them in one call. A menu is a handful
// of these, so the vector is cleared rather than freed and the per-frame path
// allocates nothing once the first open has sized it.
class PanelBatch
{
public:
    PanelBatch()
    {
        static constexpr PanelVertex unitQuad[] = {{{0.f, 0.f}},
                                                   {{1.f, 0.f}},
                                                   {{1.f, 1.f}},
                                                   {{0.f, 0.f}},
                                                   {{1.f, 1.f}},
                                                   {{0.f, 1.f}}};

        shader.setVertices(unitQuad);

        // Always blended, and in the mode that accumulates the target's own
        // alpha rather than weighting the source's by itself: every edge here
        // is a coverage ramp, and the popup's target starts fully transparent,
        // where AlphaBlend would composite each edge too faintly.
        shader.prepare(1,
                       false,
                       eacp::GPU::PrimitiveTopology::Triangles,
                       eacp::GPU::BlendMode::AlphaBlendOntoTransparent);
    }

    void begin(eacp::Graphics::Point size, float scale)
    {
        shader.screenSize =
            eacp::Array {std::max(1.f, size.x), std::max(1.f, size.y)};
        shader.pixelScale = scale > 0.f ? scale : 1.f;
        instances.clear();
    }

    // A rounded rectangle with a hard, analytically antialiased edge.
    void fillRect(const eacp::Graphics::Rect& rect,
                  const eacp::Graphics::Color& color,
                  float cornerRadius = 0.f)
    {
        add(rect, color, cornerRadius, 0.f, antialiasMargin);
    }

    // The shadow `caster` casts: the same rounded box, displaced by `drop` and
    // read through a ramp as wide as the blur, with the caster itself cut out
    // of it — so a panel that is translucent shows the view behind the window
    // through its body rather than its own shadow.
    void fillShadow(const eacp::Graphics::Rect& caster,
                    const eacp::Graphics::Color& color,
                    float cornerRadius,
                    float blur,
                    eacp::Graphics::Point drop)
    {
        const auto rect = eacp::Graphics::Rect {
            caster.x + drop.x, caster.y + drop.y, caster.w, caster.h};

        auto& instance =
            add(rect, color, cornerRadius, 1.f / std::max(0.001f, blur * 2.f), blur);

        instance.casterOffset[0] = -drop.x;
        instance.casterOffset[1] = -drop.y;
    }

    // The hovered item: the same fill, easing toward `accent` by `hover`, with
    // a band travelling across it at `sheen` turns.
    void fillHighlight(const eacp::Graphics::Rect& rect,
                       const eacp::Graphics::Color& color,
                       const eacp::Graphics::Color& accent,
                       float cornerRadius,
                       float hover,
                       float sheen)
    {
        auto& instance = add(rect, color, cornerRadius, 0.f, antialiasMargin);

        instance.accent[0] = accent.r;
        instance.accent[1] = accent.g;
        instance.accent[2] = accent.b;
        instance.accent[3] = accent.a;
        instance.style[2] = hover;
        instance.style[3] = sheen;
    }

    void flush(eacp::GPU::RenderPass& pass)
    {
        if (instances.empty())
            return;

        shader.setInstances(1, instances.data(), instances.size());
        pass.drawInstanced(shader, instances.size());
        instances.clear();
    }

private:
    static constexpr auto antialiasMargin = 1.f;

    PanelInstance& add(const eacp::Graphics::Rect& rect,
                       const eacp::Graphics::Color& color,
                       float cornerRadius,
                       float shadowRamp,
                       float margin)
    {
        auto& instance = instances.create();

        auto center = rect.center();
        instance.center[0] = center.x;
        instance.center[1] = center.y;

        instance.halfSize[0] = rect.w * 0.5f;
        instance.halfSize[1] = rect.h * 0.5f;
        instance.halfExtent[0] = instance.halfSize[0] + margin;
        instance.halfExtent[1] = instance.halfSize[1] + margin;

        instance.color[0] = color.r;
        instance.color[1] = color.g;
        instance.color[2] = color.b;
        instance.color[3] = color.a;

        instance.style[0] = std::min(
            cornerRadius, std::min(instance.halfSize[0], instance.halfSize[1]));
        instance.style[1] = shadowRamp;

        return instance;
    }

    PanelShader shader;
    eacp::Vector<PanelInstance> instances;
};
} // namespace PopupMenu
