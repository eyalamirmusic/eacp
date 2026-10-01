#include <eacp/Graphics/Graphics.h>
#include <eacp/Text/TextRenderer.h>
#include <eacp/UI/Render/CoverageAtlas.h>
#include <eacp/UI/Render/GradientRamps.h>
#include <eacp/UI/Render/ShapeBatch.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string>

// Every finger on the screen as a disc and a label, through View's touch API:
// one TouchEvent per finger, each with its own id from touchBegan to
// touchEnded. The corner line counts them and prints the safe-area insets, and
// keeps clear of them. On a desktop the mouse is one finger.

using namespace eacp;
using namespace Graphics;

namespace
{
constexpr auto background = Color {0.08f, 0.09f, 0.12f};
constexpr auto labelColor = Color::gray(0.92f);
constexpr auto mouseId = -1;

constexpr Color palette[] = {
    {0.95f, 0.36f, 0.42f},
    {0.36f, 0.72f, 0.98f},
    {0.52f, 0.86f, 0.46f},
    {0.98f, 0.76f, 0.30f},
    {0.74f, 0.52f, 0.98f},
    {0.30f, 0.88f, 0.82f},
    {0.98f, 0.56f, 0.24f},
    {0.94f, 0.50f, 0.82f},
};

Color colorFor(int id)
{
    constexpr auto count = (int) std::size(palette);
    return palette[((id % count) + count) % count];
}

float radiusFor(float pressure)
{
    return 36.f + 28.f * std::min(pressure, 2.f);
}

std::string format(const char* pattern, auto... args)
{
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), pattern, args...);
    return buffer;
}

struct Finger final
{
    int id = 0;
    Point pos;
    float pressure = 1.f;
};

struct TouchView final : GPU::GPUView
{
    TouchView()
    {
        setSampleCount(1);
        setHandlesMouseEvents(true);
        setHandlesTouchEvents(true);
    }

    void touchBegan(const TouchEvent& event) override
    {
        fingers.add({event.id, event.pos, event.pressure});
        repaint();
    }

    void touchMoved(const TouchEvent& event) override
    {
        if (auto* finger = find(event.id))
        {
            finger->pos = event.pos;
            finger->pressure = event.pressure;
        }

        repaint();
    }

    void touchEnded(const TouchEvent& event) override { lift(event.id); }

    void mouseDown(const MouseEvent& event) override
    {
        fingers.add({mouseId, event.pos, 1.f});
        repaint();
    }

    void mouseDragged(const MouseEvent& event) override
    {
        if (auto* finger = find(mouseId))
            finger->pos = event.pos;

        repaint();
    }

    void mouseUp(const MouseEvent&) override { lift(mouseId); }

    void safeAreaInsetsChanged() override { repaint(); }
    void resized() override { repaint(); }

    Finger* find(int id)
    {
        for (auto& finger: fingers)
            if (finger.id == id)
                return &finger;

        return nullptr;
    }

    void lift(int id)
    {
        fingers.eraseIf([id](const Finger& finger) { return finger.id == id; });
        repaint();
    }

    void render(GPU::Frame& frame) override
    {
        auto size = frame.logicalSize();
        auto scale = frame.backingScale();
        auto pass = frame.beginPass({background});

        if (size.x <= 0.f || size.y <= 0.f)
            return;

        if (!shapes)
            shapes.emplace(atlas, ramps, size, scale, sampleCount());

        shapes->setLogicalSize(size);
        shapes->setPixelScale(scale);
        shapes->begin(pass);

        text.setViewport(size, scale);
        text.begin();

        for (auto& finger: fingers)
            drawFinger(finger, size);

        drawStatus(size);

        shapes->end();
        text.flush(pass);
    }

    void drawFinger(const Finger& finger, Point size)
    {
        auto radius = radiusFor(finger.pressure);
        auto color = colorFor(finger.id);
        auto ring = radius + 6.f;
        auto [x, y] = finger.pos;

        shapes->fillRect({x - radius, y - radius, radius * 2.f, radius * 2.f},
                         color.withAlpha(0.55f),
                         radius);
        shapes->drawRect(
            {x - ring, y - ring, ring * 2.f, ring * 2.f}, color, 3.f, ring);

        auto label = format("#%d %.0f,%.0f r%.0f", finger.id, x, y, radius);
        auto width = text.measure(label);
        auto right = size.x - getSafeAreaInsets().right;
        auto left =
            x + ring + 8.f + width < right ? x + ring + 8.f : x - ring - 8.f - width;

        text.draw(label, {left, y - ring - 8.f}, labelColor);
    }

    void drawStatus(Point size)
    {
        auto insets = getSafeAreaInsets();
        auto margin = 26.f;
        auto height = text.lineHeight();
        auto left = insets.left + margin;
        auto top = size.y - insets.bottom - margin - height * 2.f;

        std::string lines[] = {
            format("%d touch%s", fingers.size(), fingers.size() == 1 ? "" : "es"),
            format("safe area t%.0f l%.0f b%.0f r%.0f",
                   insets.top,
                   insets.left,
                   insets.bottom,
                   insets.right)};

        auto width = std::max(text.measure(lines[0]), text.measure(lines[1]));

        shapes->fillRect({left - 10.f, top - 8.f, width + 20.f, height * 2.f + 16.f},
                         Color {1.f, 1.f, 1.f, 0.08f},
                         8.f);

        for (auto& line: lines)
        {
            text.draw(line, {left, top + text.ascent()}, labelColor);
            top += height;
        }
    }

    UI::CoverageAtlas atlas;
    UI::GradientRamps ramps;
    std::optional<UI::ShapeBatch> shapes;
    Text::TextRenderer text {15.f};
    Vector<Finger> fingers;
};

WindowOptions windowOptions()
{
    auto options = WindowOptions {};

    options.width = 900;
    options.height = 600;
    options.title = "Touch Demo";
    options.backgroundColor = background;

    return options;
}
} // namespace

int main()
{
    return runWindowedApp<TouchView>(windowOptions());
}
