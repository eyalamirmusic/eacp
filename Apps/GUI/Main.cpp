#include <eacp/Graphics/Graphics.h>
#include <eacp/Core/Core.h>

using namespace eacp;
using namespace Graphics;

struct ColoredView final : View
{
    ColoredView(Color colorToUse, const std::string& labelText)
        : color(colorToUse)
        , textLayer(labelText)
    {
        getProperties().handlesMouseEvents = true;
        addChildren({backgroundLayer, textLayer});

        textLayer->setText(labelText);
        updatePathColor();
    }

    Color getColor() const
    {
        if (on)
            return {1.f, 1.f, 1.f};

        return color;
    }

    void updatePathColor() { backgroundLayer->setFillColor(getColor()); }

    void setMode(bool mode)
    {
        on = mode;
        updatePathColor();
    }

    void mouseEntered(const MouseEvent&) override { setMode(true); }

    void mouseExited(const MouseEvent&) override { setMode(false); }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto path = Path();

        path.addRoundedRect(bounds, 10.f);
        backgroundLayer->setPath(path);

        scaleToFit({backgroundLayer, textLayer});
        textLayer->setPosition({10.f, bounds.h / 2.f - 10.f});
    }

    bool on = false;
    Color color;

    ShapeLayerView backgroundLayer;
    TextLayerView textLayer;
};

struct AnimatedView final : View
{
    AnimatedView()
    {
        ellipseLayer.setFillColor({1.f, 0.5f, 0.f});
        ellipseLayer.setOpacity(0.5f);
        addLayer(ellipseLayer);
    }

    void mouseDown(const MouseEvent&) override { LOG("Animated mouseDown!"); }

    void resized() override
    {
        auto path = Path();
        path.addEllipse(getLocalBounds().getRelative({0.f, 0.f, 0.1f, 0.1f}));
        ellipseLayer.setPath(path);
    }

    void update()
    {
        opacity += 0.02f;

        if (opacity >= 0.9f)
            opacity = 0.1f;

        ellipseLayer.setOpacity(opacity);

        x += dx;

        if (x < 0.1f || x > 0.9f)
            dx = -dx;

        ellipseLayer.setPosition({x * getBounds().w, 0.f});
    }

    ShapeLayer ellipseLayer;
    float opacity = 0.5f;
    float x = 0.3f;
    float dx = 0.003f;
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
        layer.scaleToFit();
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
        scaleToFit({titleLayer, subtitleLayer});

        auto bounds = getLocalBounds();

        titleLayer->setPosition({20.f, bounds.h - 40.f});
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
        child1.setBoundsRelative({0.1f, 0.1f, 0.2f, 0.2f});
        child2.setBoundsRelative({0.4f, 0.1f, 0.2f, 0.2f});
        child3.setBoundsRelative({0.7f, 0.1f, 0.2f, 0.2f});

        scaleToFit({animatedChild, rec, stroke});
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
