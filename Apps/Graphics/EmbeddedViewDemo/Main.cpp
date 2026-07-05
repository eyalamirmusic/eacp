#include <eacp/UI/UI.h>

// A component tree inside a host-provided native view.
//
// The point of the demo is the boundary: `EmbeddedView` attaches to an NSView or
// HWND somebody else owns — a plugin host's editor window — and everything
// inside it is ours. What is worth seeing after the port is that the boundary
// did not move. A ComponentHost is a GPUView and a GPUView is an
// eacp::Graphics::View, so the thing being embedded is the same kind of object
// it always was, and the whole widget tree behind it is one native view rather
// than one per element.
//
// The host here has a layout of its own: it keeps a strip along the top of its
// window and gives the surface what is left, laying it out again on every
// resize. That is the arrangement any host bigger than a bare window ends up
// with, and the reason setBounds exists — a surface left to itself fills the
// window it was handed and would cover the strip.
//
// It is also where the two coordinate systems part company. The window's
// content view is a plain platform view, which on macOS measures its subviews
// up from its bottom edge, while the rect below is eacp's: points, y-down. The
// strip is at the top of the window on both platforms, and if the conversion
// were missing it would be at the bottom on one of them.

using namespace eacp;

namespace
{
struct PluginContent final : UI::Component
{
    PluginContent()
    {
        label.setFontStyle(UI::FontStyle::Bold);
        label.setColour({0.95f, 0.95f, 0.95f, 1.f});

        addAndMakeVisible(label);
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll({0.15f, 0.18f, 0.22f, 1.f});

        g.setColour({0.9f, 0.55f, 0.2f, 1.f});
        g.fillRoundedRect(getLocalBounds().getRelative({0.1f, 0.35f, 0.8f, 0.3f}),
                          12.f);
    }

    void resized() override
    {
        label.setBounds(getLocalBounds().inset(20.f, 0.f).removeFromTop(60.f));
    }

    UI::Label label {"Embedded into host-provided NSView"};
};

struct Host final : UI::ComponentHost
{
    Host() { setRootComponent(content); }

    PluginContent content;
};

struct FakeHostApp
{
    static constexpr auto initialWidth = 640;
    static constexpr auto initialHeight = 400;

    // The host's own chrome — where its toolbar would go. Nothing draws it
    // here; it is the band of window the surface has to keep off.
    static constexpr auto stripHeight = 44.f;
    static constexpr auto margin = 12.f;

    FakeHostApp()
    {
        embedded.setContentView(host);
        placeSurface(initialWidth, initialHeight);
    }

    void placeSurface(int width, int height)
    {
        embedded.setBounds({margin,
                            stripHeight,
                            static_cast<float>(width) - margin * 2.f,
                            static_cast<float>(height) - stripHeight - margin});
    }

    Graphics::WindowOptions makeOptions()
    {
        auto options = Graphics::WindowOptions {};

        options.title = "Fake Plugin Host";
        options.width = initialWidth;
        options.height = initialHeight;

        // The host lays the surface out again itself, because placing it once
        // took it off the automatic sizing that would have done so.
        options.onResize = [this](int width, int height)
        { placeSurface(width, height); };

        return options;
    }

    Host host;
    Graphics::Window window {makeOptions()};
    Graphics::EmbeddedView embedded {window.getContentViewHandle(),
                                     {initialWidth, initialHeight}};
};
} // namespace

int main()
{
    return eacp::Apps::run<FakeHostApp>();
}
