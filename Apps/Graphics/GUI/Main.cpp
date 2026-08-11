#include <eacp/Core/Platform/Platform.h>
#include <eacp/UI/UI.h>

#include <algorithm>
#include <string>

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
