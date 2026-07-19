#include <eacp/Graphics/Graphics.h>

#include <array>

using namespace eacp;
using namespace Graphics;

// The pointer changing shape per *region* of one view.
//
// That is the case the API exists for, and the one a per-view cursor cannot do.
// A GPU-drawn UI is a single view with a whole widget tree painted into it — an
// editor is one view holding a splitter, a text area and a file list — so the
// shape has to follow the pointer inside a view rather than being fixed for it.
//
// Move across the bands and watch the pointer. The shape is set from mouseMoved
// on every move, with no bookkeeping about which band was last under the
// pointer, because setting the same shape twice is free.

struct CursorBandsView final : View
{
    struct Band
    {
        MouseCursor cursor;
        const char* label;
        Color color;
    };

    static constexpr auto bandCount = 5;

    CursorBandsView()
    {
        getProperties().handlesMouseEvents = true;

        for (auto index = 0; index < bandCount; ++index)
        {
            fills[index]->setFillColor(bands[index].color);
            labels[index]->setText(bands[index].label);
            labels[index]->setColor({0.97f, 0.97f, 0.99f});

            addChildren({fills[index], labels[index]});
        }
    }

    int bandAt(const Point& position) const
    {
        const auto width = getLocalBounds().w;

        if (width <= 0.f)
            return 0;

        const auto index = (int) (position.x / (width / bandCount));

        return std::clamp(index, 0, bandCount - 1);
    }

    void mouseMoved(const MouseEvent& event) override
    {
        setMouseCursor(bands[bandAt(event.pos)].cursor);
    }

    // Leaving the view puts the arrow back. Without this the last band's shape
    // would follow the pointer out over the window's own chrome.
    void mouseExited(const MouseEvent&) override
    {
        setMouseCursor(MouseCursor::Default);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const auto bandWidth = area.w / bandCount;

        for (auto index = 0; index < bandCount; ++index)
        {
            const auto slice = area.removeFromLeft(bandWidth);

            auto path = Path();
            path.addRect(slice);
            fills[index]->setPath(path);

            scaleToFit({fills[index], labels[index]});
            labels[index]->setPosition({slice.x + 12.f, 24.f});
        }
    }

    static constexpr std::array<Band, bandCount> bands {
        {{MouseCursor::Default, "Default", {0.16f, 0.17f, 0.21f}},
         {MouseCursor::IBeam, "IBeam", {0.20f, 0.24f, 0.32f}},
         {MouseCursor::PointingHand, "Hand", {0.24f, 0.30f, 0.42f}},
         {MouseCursor::ResizeLeftRight, "Resize L/R", {0.28f, 0.36f, 0.52f}},
         {MouseCursor::Crosshair, "Crosshair", {0.32f, 0.42f, 0.62f}}}};

    std::array<ShapeLayerView, bandCount> fills;
    std::array<TextLayerView, bandCount> labels;
};

struct CursorShapesApp
{
    CursorShapesApp() { window.setContentView(view); }

    CursorBandsView view;
    Window window {[]
                   {
                       auto options = WindowOptions {};
                       options.width = 760;
                       options.height = 200;
                       options.title = "Cursor Shapes — move across the bands";
                       options.backgroundColor = Color {0.16f, 0.17f, 0.21f};
                       return options;
                   }()};
};

int main(int argc, char* argv[])
{
    return Apps::run<CursorShapesApp>(argc, argv);
}
