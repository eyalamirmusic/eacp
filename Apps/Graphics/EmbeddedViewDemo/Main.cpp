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
    FakeHostApp()
    {
        window.setTitle("Fake Plugin Host");
        embedded.setContentView(host);
    }

    Host host;
    eacp::Graphics::Window window;
    eacp::Graphics::EmbeddedView embedded {window.getContentViewHandle(),
                                           {640, 400}};
};
} // namespace

int main()
{
    return eacp::Apps::run<FakeHostApp>();
}
