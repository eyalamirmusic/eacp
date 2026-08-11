#include <eacp/UI/UI.h>

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
