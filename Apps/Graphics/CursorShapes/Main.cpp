#include <eacp/UI/UI.h>

#include <string>

using namespace eacp;

namespace
{
struct Band final : UI::Component
{
    Band(eacp::Graphics::MouseCursor cursorToUse,
         std::string labelToUse,
         const UI::Color& colourToUse)
        : label(std::move(labelToUse))
        , colour(colourToUse)
    {
        setMouseCursor(cursorToUse);

        // Without this the pointer never enters the band, so the host has no
        // cursor to take.
        setInterceptsMouseClicks(true);
    }

    void mouseEnter(const UI::MouseEvent&) override { repaint(); }
    void mouseExit(const UI::MouseEvent&) override { repaint(); }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(isMouseOver() ? colour.brighter(0.06f) : colour);

        g.setColour({0.97f, 0.97f, 0.99f, 1.f});
        g.drawText(label, getLocalBounds().inset(12.f, 0.f).removeFromTop(48.f));
    }

    std::string label;
    UI::Color colour;
};

struct Bands final : UI::Component
{
    Bands()
    {
        for (auto& band: bands)
            addAndMakeVisible(*band);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const auto bandWidth = area.w / bands.size();

        for (auto& band: bands)
            band->setBounds(area.removeFromLeft(bandWidth));
    }

    OwnedVector<Band> bands = []
    {
        using Cursor = eacp::Graphics::MouseCursor;

        auto list = OwnedVector<Band> {};

        list.createNew(Cursor::Default, "Default", UI::Color {0.16f, 0.17f, 0.21f});
        list.createNew(Cursor::IBeam, "IBeam", UI::Color {0.20f, 0.24f, 0.32f});
        list.createNew(
            Cursor::PointingHand, "Hand", UI::Color {0.24f, 0.30f, 0.42f});
        list.createNew(
            Cursor::ResizeLeftRight, "Resize L/R", UI::Color {0.28f, 0.36f, 0.52f});
        list.createNew(
            Cursor::Crosshair, "Crosshair", UI::Color {0.32f, 0.42f, 0.62f});

        return list;
    }();
};

struct Host final : UI::ComponentHost
{
    Host() { setRootComponent(bands); }

    Bands bands;
};

eacp::Graphics::WindowOptions windowOptions()
{
    auto options = eacp::Graphics::WindowOptions {};

    options.width = 760;
    options.height = 200;
    options.title = "Cursor Shapes — move across the bands";
    options.backgroundColor = eacp::Graphics::Color {0.16f, 0.17f, 0.21f};

    return options;
}

struct CursorShapesApp
{
    CursorShapesApp() { window.setContentView(host); }

    Host host;
    eacp::Graphics::Window window {windowOptions()};
};
} // namespace

int main(int argc, char* argv[])
{
    return Apps::run<CursorShapesApp>(argc, argv);
}
