#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>
#include <eacp/Sprites/Sprites.h>

#include <algorithm>
#include <optional>

// Scrolling, clipped panes: RenderPass::setScissorRect, wheel events, and
// GPUView::backingScale working together, which is the combination every
// scrollable region needs.
//
// Two panes side by side, each holding a list taller than it is. Point at one
// and scroll: only that pane moves, and its rows are cut at its edge rather
// than spilling into its neighbour. The rows are deliberately drawn wider than
// their pane, so without a scissor rect the two lists would overlap in the
// middle of the window.
//
// The wheel handler shows the delta convention: a trackpad reports points and
// is applied as-is, a notched wheel reports lines and only the view knows what
// a line is worth. The right edge of each pane carries a scrollbar so the
// position is visible while dragging.

using namespace eacp;

namespace
{
struct Pane
{
    Graphics::Rect bounds;
    Graphics::Color rowColor;
    float scroll = 0.f;
    int rowCount = 0;
};

constexpr auto rowHeight = 28.f;
constexpr auto rowGap = 4.f;
constexpr auto padding = 16.f;
constexpr auto scrollbarWidth = 6.f;

constexpr auto background = Graphics::Color {0.11f, 0.12f, 0.15f};
constexpr auto paneColor = Graphics::Color {0.16f, 0.17f, 0.21f};
constexpr auto scrollbarColor = Graphics::Color {1.f, 1.f, 1.f, 0.25f};

struct ClippingView final : GPU::GPUView
{
    ClippingView()
    {
        // A crisp scissor edge: multisampling would feather the boundary across
        // a pixel, which is exactly what clipping is meant to avoid.
        setSampleCount(1);
        setHandlesMouseEvents(true);

        panes[0] = {.rowColor = {0.40f, 0.60f, 0.90f}, .rowCount = 40};
        panes[1] = {.rowColor = {0.90f, 0.52f, 0.35f}, .rowCount = 24};
    }

    void resized() override
    {
        GPUView::resized();

        const auto bounds = getLocalBounds();

        if (bounds.w > 0 && bounds.h > 0)
            sprites.emplace(Graphics::Point {bounds.w, bounds.h}, sampleCount());

        layOutPanes();
        repaint();
    }

    void layOutPanes()
    {
        auto area = getLocalBounds().inset(padding);
        const auto gap = padding;
        const auto paneWidth = (area.w - gap) / 2.f;

        panes[0].bounds = {area.x, area.y, paneWidth, area.h};
        panes[1].bounds = {area.x + paneWidth + gap, area.y, paneWidth, area.h};

        for (auto& pane: panes)
            pane.scroll = clampScroll(pane, pane.scroll);
    }

    static float contentHeight(const Pane& pane)
    {
        return (float) pane.rowCount * (rowHeight + rowGap);
    }

    // Scroll runs from 0 (top) down to a negative offset that puts the last row
    // at the bottom; a list shorter than its pane cannot scroll at all.
    static float clampScroll(const Pane& pane, float value)
    {
        const auto lowest = std::min(0.f, pane.bounds.h - contentHeight(pane));
        return std::clamp(value, lowest, 0.f);
    }

    // Logical points -> render-target pixels. setScissorRect works in pixels
    // because that is what both backends' scissor state means; everything else
    // in this file is in points.
    Graphics::Rect toPixels(const Graphics::Rect& rect) const
    {
        const auto scale = backingScale();
        return {rect.x * scale, rect.y * scale, rect.w * scale, rect.h * scale};
    }

    void drawPane(GPU::RenderPass& pass, const Pane& pane)
    {
        sprites->fillRect(pane.bounds, paneColor);

        // Everything from here to clearScissorRect is confined to the pane, so
        // the rows below can be laid out as if the pane were unbounded.
        pass.setScissorRect(toPixels(pane.bounds));

        for (auto row = 0; row < pane.rowCount; ++row)
        {
            const auto y =
                pane.bounds.y + pane.scroll + (float) row * (rowHeight + rowGap);

            // Wider than the pane on purpose: the overhang is what proves the
            // scissor rect is doing the work.
            const auto rowRect = Graphics::Rect {pane.bounds.x + 10.f,
                                                 y + rowGap,
                                                 pane.bounds.w * 1.4f,
                                                 rowHeight};

            // Alternate rows shade slightly so scrolling is legible.
            const auto color = row % 2 == 0 ? pane.rowColor
                                            : pane.rowColor.darker(0.12f);

            sprites->fillRect(rowRect, color);
        }

        drawScrollbar(pane);

        pass.clearScissorRect();
    }

    void drawScrollbar(const Pane& pane)
    {
        const auto content = contentHeight(pane);

        if (content <= pane.bounds.h)
            return;

        const auto visibleFraction = pane.bounds.h / content;
        const auto thumbHeight = std::max(pane.bounds.h * visibleFraction, 24.f);

        const auto scrolled = -pane.scroll / (content - pane.bounds.h);
        const auto travel = pane.bounds.h - thumbHeight;

        sprites->fillRect({pane.bounds.x + pane.bounds.w - scrollbarWidth - 2.f,
                           pane.bounds.y + scrolled * travel,
                           scrollbarWidth,
                           thumbHeight},
                          scrollbarColor);
    }

    void render(GPU::Frame& frame) override
    {
        auto pass = frame.beginPass({background});

        if (!sprites)
            return;

        sprites->begin(pass);

        for (const auto& pane: panes)
            drawPane(pass, pane);
    }

    void mouseWheel(const Graphics::MouseEvent& event) override
    {
        for (auto& pane: panes)
        {
            if (!pane.bounds.contains(event.pos))
                continue;

            // The delta's unit depends on the device: a trackpad's precise
            // delta is already in points, a notched wheel's is in lines, and
            // only this view knows how tall a line is.
            const auto points = event.preciseScrolling
                                    ? event.delta.y
                                    : event.delta.y * (rowHeight + rowGap);

            pane.scroll = clampScroll(pane, pane.scroll + points);
            repaint();
            return;
        }
    }

    std::optional<Sprites::SpriteRenderer> sprites;
    Pane panes[2];
};

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};

    options.width = 720;
    options.height = 520;
    options.minWidth = 360;
    options.minHeight = 240;
    options.title = "Clipping";
    options.backgroundColor = background;

    return options;
}

struct ClippingApp
{
    ClippingApp() { window.setContentView(view); }

    ClippingView view;
    Graphics::Window window {windowOptions()};
};
} // namespace

int main()
{
    return eacp::Apps::run<ClippingApp>();
}
