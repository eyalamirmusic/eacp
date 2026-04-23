#include <eacp/Graphics/Graphics.h>
#include <eacp/Graphics/Window/EmbeddedView.h>

using namespace eacp;
using namespace Graphics;

struct PluginContent final : View
{
    PluginContent()
    {
        background->setFillColor({0.15f, 0.18f, 0.22f});
        accent->setFillColor({0.9f, 0.55f, 0.2f});

        label->setColor({0.95f, 0.95f, 0.95f});
        label->setFont(FontOptions().withName("Helvetica-Bold"));

        addChildren({background, accent, label});
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto bgPath = Path();
        bgPath.addRect(bounds);
        background->setPath(bgPath);
        scaleToFit({background});

        auto accentPath = Path();
        accentPath.addRoundedRect(bounds.getRelative({0.1f, 0.35f, 0.8f, 0.3f}),
                                  12.f);
        accent->setPath(accentPath);
        scaleToFit({accent});

        label->setPosition({20.f, bounds.h - 40.f});
    }

    ShapeLayerView background;
    ShapeLayerView accent;
    TextLayerView label {"Embedded into host-provided NSView"};
};

struct FakeHostApp
{
    FakeHostApp()
    {
        window.setTitle("Fake Plugin Host");
        embedded.setContentView(content);
    }

    PluginContent content;
    Window window;
    EmbeddedView embedded {window.getContentViewHandle(), {640, 400}};
};

int main()
{
    eacp::Apps::run<FakeHostApp>();
    return 0;
}
