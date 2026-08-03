#include <eacp/Core/Platform/Platform.h>
#include <eacp/UI/UI.h>

#include <algorithm>
#include <string>

// The original tour of the drawing API, through the component tier.
//
// Every element here used to be a native view holding native layers: a
// CAShapeLayer per rectangle, a CATextLayer per string, each one an object the
// window server knows about. They are components now, and the whole window is a
// single native view — so the tour is also a demonstration that the same picture
// costs one view rather than a dozen.
//
// What is worth comparing against the old file is where the state lives. A
// native layer holds its own colour and is *told* when to change, so a hover
// meant an updatePathColor() reaching into three layers; a component holds the
// state and paint() reads it, so a hover is a repaint and the colour is decided
// in one place.

using namespace eacp;

namespace
{
struct ColouredBox final : UI::Component
{
    ColouredBox(const UI::Color& colourToUse, std::string labelToUse)
        : colour(colourToUse)
        , label(std::move(labelToUse))
    {
        setInterceptsMouseClicks(true);
    }

    UI::Color currentColour() const
    {
        auto base = on ? UI::Color {0.9f, 0.9f, 0.9f, 1.f} : colour;

        return base.withAlpha(isMouseOver() ? 1.f : 0.5f);
    }

    void mouseEnter(const UI::MouseEvent&) override { repaint(); }
    void mouseExit(const UI::MouseEvent&) override { repaint(); }

    void mouseDown(const UI::MouseEvent&) override
    {
        on = !on;
        repaint();
    }

    void paint(UI::Graphics& g) override
    {
        g.setColour(currentColour());
        g.fillRoundedRect(getLocalBounds(), 10.f);

        g.setColour({0.05f, 0.05f, 0.07f, 1.f});
        g.drawText(label, getLocalBounds().inset(10.f, 0.f));
    }

    bool on = false;
    UI::Color colour;
    std::string label;
};

// A circle crossing the panel and fading as it goes, driven by a display link.
//
// The animation is the component's own two floats: the link advances them and
// asks for a repaint, and paint() draws from them. Nothing is retained between
// frames — there is no layer to move — so a frame of this costs the same quad
// the static shapes around it cost.
struct AnimatedDisc final : UI::Component
{
    void update(Threads::FrameTime time)
    {
        auto delta = static_cast<float>(time.delta);

        opacity += fadeSpeed * delta;

        if (opacity >= 0.9f)
            opacity = 0.1f;

        x += dx * delta;

        if (x < 0.1f || x > 0.9f)
        {
            x = std::clamp(x, 0.1f, 0.9f);
            dx = -dx;
        }

        repaint();
    }

    void paint(UI::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        auto size = std::min(bounds.w, bounds.h) * 0.18f;

        g.setColour(UI::Color {1.f, 0.5f, 0.f}.withAlpha(opacity));
        g.fillRoundedRect(
            {x * bounds.w - size * 0.5f, bounds.h * 0.5f - size * 0.5f, size, size},
            size * 0.5f);
    }

    static constexpr auto fadeSpeed = 1.2f;

    float opacity = 0.5f;
    float x = 0.3f;
    float dx = 0.18f;

    Threads::DisplayLink link {[this](Threads::FrameTime time) { update(time); }};
};

struct GradientPanel final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        // Placed in this component's own points, and resolved once for the fill
        // that follows: the ramp is shared, so a second panel with these colours
        // would cost nothing more than this one.
        auto gradient = UI::Gradient {};
        gradient.start = {0.f, 0.f};
        gradient.end = {bounds.w, bounds.h};
        gradient.stops.add({{0.2f, 0.4f, 0.9f, 1.f}, 0.f});
        gradient.stops.add({{0.9f, 0.2f, 0.5f, 1.f}, 0.5f});
        gradient.stops.add({{0.9f, 0.6f, 0.1f, 1.f}, 1.f});

        g.setGradient(gradient);
        g.fillRoundedRect(bounds, 12.f);
        g.clearGradient();
    }
};

struct TextPanel final : UI::Component
{
    TextPanel()
    {
        title.setFontSize(18.f);
        title.setColour({0.9f, 0.9f, 0.9f, 1.f});

        // A second face, in the same atlas as the first and as every glyph in
        // the tree: what used to be a CATextLayer with its own font is a size
        // and a weight on the run being drawn.
        subtitle.setFontStyle(UI::FontStyle::Bold);
        subtitle.setColour({0.9f, 0.9f, 0.9f, 1.f});

        addChildren({title, subtitle});
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(20.f, 0.f);

        title.setBounds(area.removeFromTop(28.f));
        subtitle.setBounds(area.removeFromTop(22.f));
    }

    UI::Label title {"Text at two faces"};
    UI::Label subtitle {"Both out of one glyph atlas"};
};

struct DemoRoot final : UI::Component
{
    DemoRoot() { addChildren({blue, purple, red, gradient, disc, text}); }

    void paint(UI::Graphics& g) override
    {
        g.fillAll({0.1f, 0.1f, 0.1f, 1.f});

        // The stroked border the old StrokeRect was a whole view for.
        g.setColour({0.5f, 0.5f, 0.5f, 1.f});
        g.drawRect(getLocalBounds(), 2.f);
    }

    void resized() override
    {
        blue.setPos({0.1f, 0.1f, 0.2f, 0.2f});
        purple.setPos({0.4f, 0.1f, 0.2f, 0.2f});
        red.setPos({0.7f, 0.1f, 0.2f, 0.2f});

        gradient.setPos({0.1f, 0.4f, 0.8f, 0.2f});
        disc.setPos({0.f, 0.65f, 1.f, 0.15f});

        text.setBounds(getLocalBounds().removeFromBottom(80.f));
    }

    ColouredBox blue {{0.2f, 0.4f, 0.8f, 1.f}, "Blue"};
    ColouredBox purple {{0.4f, 0.1f, 0.3f, 0.5f}, "Purple"};
    ColouredBox red {{1.0f, 0.f, 0.1f, 0.7f}, "Red"};

    GradientPanel gradient;
    AnimatedDisc disc;
    TextPanel text;
};

struct Host final : UI::ComponentHost
{
    Host()
    {
        setBackgroundColour({0.1f, 0.1f, 0.1f, 1.f});
        setRootComponent(root);
    }

    DemoRoot root;
};

struct MyApp
{
    MyApp()
    {
        LOG(Platform::getAppName(), " v", Platform::getAppVersion());
        window.setContentView(host);
    }

    Host host;
    eacp::Graphics::Window window;
};
} // namespace

int main()
{
    return eacp::Apps::run<MyApp>();
}
