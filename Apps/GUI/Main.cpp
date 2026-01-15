#include <eacp/Graphics/Graphics.h>
#include <eacp/Core/Core.h>

using namespace eacp;
using namespace Graphics;

struct ColoredView final : View
{
    ColoredView(Color color, const std::string& labelText)
        : textLayer(labelText)
    {
        backgroundLayer->setFillColor(color);
        addChildren({backgroundLayer, textLayer});

        textLayer->setText(labelText);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto path = Path();

        path.addRoundedRect(bounds, 10.f);
        backgroundLayer->setPath(path);

        backgroundLayer.setBounds(bounds);
        textLayer->setBounds(bounds);
        textLayer->setPosition({10.f, bounds.h / 2.f - 10.f});
    }

    ShapeLayerView backgroundLayer;
    TextLayerView textLayer;
};

struct AnimatedView final : View
{
    AnimatedView()
    {
        auto path = Path();
        path.addEllipse({0, 0, 80.f, 80.f});
        ellipseLayer.setFillColor({1.f, 0.5f, 0.f});
        ellipseLayer.setOpacity(0.5f);
        ellipseLayer.setPath(path);
        addLayer(ellipseLayer);
    }

    void update()
    {
        opacity += 0.02f;

        if (opacity >= 0.9f)
            opacity = 0.1f;

        ellipseLayer.setOpacity(opacity);

        x += dx;

        if (x < 10 || x > 500)
            dx = -dx;

        ellipseLayer.setPosition({x, 0.f});
    }

    ShapeLayer ellipseLayer;
    float opacity = 0.5f;
    float x = 50.f;
    float dx = 2.f;
    Threads::Timer timer {[&] { update(); }, 60};
};

struct FilledRect final : View
{
    FilledRect(const Color& colorToUse)
    {
        layer->setFillColor(colorToUse);
        addChildren({layer});
    }

    void resized() override
    {
        path.clear();
        path.addRect(getLocalBounds());
        layer->setPath(path);
        layer->setBounds(getLocalBounds());
    }

    Path path;
    ShapeLayerView layer;
};

struct StrokeRect final : View
{
    StrokeRect()
    {
        view->setStrokeColor({0.5f, 0.5f, 0.5f});
        view->setStrokeWidth(2.f);
        addChildren({view});
    }

    void resized() override
    {
        auto path = Path();
        path.addRect(getLocalBounds());
        view->setPath(path);
    }

    ShapeLayerView view;
};

struct TextDisplay final : View
{
    TextDisplay()
    {
        auto color = Color(0.9f, 0.9f, 0.9f);
        titleLayer->setColor(color);

        subtitleLayer->setFont({"Helvetica-Bold"});
        subtitleLayer->setColor(color);

        addChildren({titleLayer, subtitleLayer});
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        titleLayer.setBounds(bounds);
        titleLayer->setPosition({20.f, bounds.h - 40.f});

        subtitleLayer.setBounds(bounds);
        subtitleLayer->setPosition({20.f, bounds.h - 65.f});
    }

    TextLayerView titleLayer {"TextLayer Demo"};
    TextLayerView subtitleLayer {"Using cached CATextLayer"};
};

struct ParentView final : View
{
    ParentView()
    {
        addChildren({rec, stroke, child1, child2, child3, animatedChild, text});
    }

    void resized() override
    {
        child1.setBounds({50, 50, 150, 100});
        child2.setBounds({220, 50, 150, 100});
        child3.setBounds({390, 50, 150, 100});
        animatedChild.setBounds(getLocalBounds());
        rec.setBounds(getLocalBounds());
        stroke.setBounds(getLocalBounds());
        text.setBounds({0, getLocalBounds().h - 30, 300, 30});
    }

    FilledRect rec {{0.1f, 0.1f, 0.1f}};
    StrokeRect stroke;
    ColoredView child1 {{0.2f, 0.4f, 0.8f}, "Blue"};
    ColoredView child2 {{0.4f, 0.1f, 0.3f, 0.5f}, "Purple"};
    ColoredView child3 {{1.0, 0.f, 0.1f, 0.7f}, "Red"};
    AnimatedView animatedChild;
    TextDisplay text;
};

struct MyApp
{
    MyApp() { window.setContentView(parentView); }

    ParentView parentView;
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}
