#include <eacp/UI/UI.h>

#include <string>

// A ratio-locked canvas under a header and beside an inspector, neither of
// which scales with it.
//
// What it is here to show is that nobody answers a resize. The window is
// handed one AspectRatioLock - the ratio and the two fixed borders - as its
// sizeConstraint, and from then on every size it can take (a drag from any
// edge, the zoom button, fullscreen, a display too small for it) is put
// through that rule before it is applied. The canvas never sees a shape it
// has to letterbox, and the header and inspector never see one they have to
// squeeze into, so resized() below is three lines of plain layout.
//
// The same object is read back in that layout: lockedArea() is where the
// canvas goes. So the header height and the inspector width are written once,
// in the lock, and the constraint and the layout cannot disagree about them.
//
// Drag any edge and watch the inspector: the canvas stays 16:9 to the point,
// the header keeps its height, the inspector its width.
//
// The inspector's buttons are the programmatic path, Window::setSize, and it
// is put through the same rule: the two canvas presets ask for a size the
// lock allows and get it exactly, and the third asks for a square and gets
// the 16:9 window nearest to it, width winning as on a corner drag. The app
// cannot put the window into a shape it would refuse the user.

using namespace eacp;

namespace
{
constexpr auto headerHeight = 56.f;
constexpr auto inspectorWidth = 220.f;
constexpr auto padding = 12.f;

const auto lock = Graphics::AspectRatioLock {
    {16.f, 9.f}, {.top = headerHeight, .right = inspectorWidth}};

std::string sizeText(float width, float height)
{
    return std::to_string((int) width) + " x " + std::to_string((int) height);
}

std::string ratioText(float width, float height)
{
    auto ratio = height > 0.f ? width / height : 0.f;
    auto text = std::to_string(ratio);
    return text.substr(0, text.find('.') + 4);
}

struct Header final : UI::Component
{
    Header()
    {
        title.setFontSize(15.f);
        hint.setColour(UI::defaultTheme().dimText);
        hint.setJustification(UI::Justification::Right);
        addChildren({title, hint});
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().panel);
        g.setColour(UI::defaultTheme().outline);
        g.fillRect(getLocalBounds().fromBottom(1.f));
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(padding, 0.f);
        hint.setBounds(area.removeFromRight(260.f));
        title.setBounds(area);
    }

    UI::Label title {"Header: any width, always 56 pt tall"};
    UI::Label hint {"Drag any edge - the canvas stays 16:9"};
};

// The locked content. A 16 by 9 grid, so a wrong shape would show as cells
// that are not square, and the diagonals, which cross at the centre only if
// the rect is the one the lock promised.
struct Canvas final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        g.fillAll({0.07f, 0.08f, 0.10f, 1.f});

        g.setColour({1.f, 1.f, 1.f, 0.06f});

        for (auto column = 1; column < 16; ++column)
        {
            auto x = bounds.w * (float) column / 16.f;
            g.drawLine({x, 0.f}, {x, bounds.h});
        }

        for (auto row = 1; row < 9; ++row)
        {
            auto y = bounds.h * (float) row / 9.f;
            g.drawLine({0.f, y}, {bounds.w, y});
        }

        g.setColour(UI::defaultTheme().accentDim);
        g.drawLine({0.f, 0.f}, {bounds.w, bounds.h});
        g.drawLine({bounds.w, 0.f}, {0.f, bounds.h});

        g.setColour(UI::defaultTheme().accent);
        g.drawRect(bounds, 2.f);

        g.setFontSize(28.f);
        g.setColour(UI::defaultTheme().text);
        g.drawText("16 : 9", bounds, UI::Justification::Centred);
    }

    void resized() override { onSizeChanged(getWidth(), getHeight()); }

    std::function<void(float width, float height)> onSizeChanged = [](float,
                                                                      float) {};
};

struct Inspector final : UI::Component
{
    Inspector()
    {
        heading.setFontSize(15.f);

        for (auto* caption: {&windowCaption, &canvasCaption, &ratioCaption})
            caption->setColour(UI::defaultTheme().dimText);

        addChildren({heading,
                     windowCaption,
                     windowSize,
                     canvasCaption,
                     canvasSize,
                     ratioCaption,
                     ratio,
                     setSizeCaption,
                     smallCanvas,
                     largeCanvas,
                     square});

        smallCanvas.onClick = [this] { requestCanvasSize({640.f, 360.f}); };
        largeCanvas.onClick = [this] { requestCanvasSize({1280.f, 720.f}); };
        square.onClick = [this] { onSizeRequested({800.f, 800.f}); };
    }

    void requestCanvasSize(Graphics::Point canvas)
    {
        onSizeRequested({canvas.x + inspectorWidth, canvas.y + headerHeight});
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().panel);
        g.setColour(UI::defaultTheme().outline);
        g.fillRect(getLocalBounds().fromLeft(1.f));
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(padding);
        heading.setBounds(area.removeFromTop(28.f));
        area.removeFromTop(padding);

        for (auto* row: {&windowCaption,
                         &windowSize,
                         &canvasCaption,
                         &canvasSize,
                         &ratioCaption,
                         &ratio})
        {
            row->setBounds(area.removeFromTop(22.f));
        }

        area.removeFromTop(padding);
        setSizeCaption.setBounds(area.removeFromTop(22.f));

        for (auto* button: {&smallCanvas, &largeCanvas, &square})
        {
            button->setBounds(area.removeFromTop(28.f));
            area.removeFromTop(6.f);
        }
    }

    void showSizes(Graphics::Point window, Graphics::Point canvas)
    {
        windowSize.setText(sizeText(window.x, window.y));
        canvasSize.setText(sizeText(canvas.x, canvas.y));
        ratio.setText(ratioText(canvas.x, canvas.y)
                      + "  (16:9 = " + ratioText(16.f, 9.f) + ")");
    }

    UI::Label heading {"Inspector: 220 pt wide"};
    UI::Label windowCaption {"Window"};
    UI::Label windowSize;
    UI::Label canvasCaption {"Canvas"};
    UI::Label canvasSize;
    UI::Label ratioCaption {"Canvas ratio"};
    UI::Label ratio;
    UI::Label setSizeCaption {"Set size"};
    UI::Button smallCanvas {"Canvas 640 x 360"};
    UI::Button largeCanvas {"Canvas 1280 x 720"};
    UI::Button square {"Ask for 800 x 800"};

    std::function<void(Graphics::Point size)> onSizeRequested = [](auto&&) {};
};

struct DemoRoot final : UI::Component
{
    DemoRoot()
    {
        canvas.onSizeChanged = [this](float width, float height)
        { inspector.showSizes({getWidth(), getHeight()}, {width, height}); };

        addChildren({header, inspector, canvas});
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().background);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        header.setBounds(area.removeFromTop(headerHeight));
        inspector.setBounds(area.removeFromRight(inspectorWidth));
        canvas.setBounds(lock.lockedArea(getLocalBounds()));
    }

    Header header;
    Inspector inspector;
    Canvas canvas;
};

struct DemoHost final : UI::ComponentHost
{
    DemoHost()
    {
        setFontPointSize(13.f);
        setRootComponent(root);
    }

    DemoRoot root;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 960 + (int) inspectorWidth;
    options.height = 540 + (int) headerHeight;
    options.minWidth = 480 + (int) inspectorWidth;
    options.minHeight = 270 + (int) headerHeight;
    options.title = "eacp UI - aspect lock under a header";
    options.sizeConstraint = lock;
    options.allowsFullScreen = false;

    return options;
}

struct App
{
    App()
    {
        host.root.inspector.onSizeRequested = [this](Graphics::Point size)
        { window.setSize(size); };
    }

    DemoHost host;
    Graphics::Window window {host, makeOptions()};
};
} // namespace

int main()
{
    return eacp::Apps::run<App>();
}
