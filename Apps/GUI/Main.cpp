#include <eacp/Graphics/Graphics.h>
#include <eacp/Core/Core.h>

using namespace eacp;
using namespace Graphics;

struct ColoredView final : View
{
    ColoredView(Color colorToUse, const std::string& labelText)
        : color(colorToUse)
    {
        backgroundPath.addRoundedRect({0, 0, 150, 100}, 10.f);
        backgroundLayer.setPath(backgroundPath);
        backgroundLayer.setFillColor(color);
        addLayer(backgroundLayer);

        textLayer.setText(labelText);
        textLayer.setFont(labelFont);
        textLayer.setColor({1.f, 1.f, 1.f});
        addLayer(textLayer);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        textLayer.setBounds({0, 0, bounds.w, bounds.h});
        textLayer.setPosition({10.f, bounds.h / 2.f - 10.f});
    }

    Font labelFont {"Helvetica-Bold", 14.f};
    Color color;
    Path backgroundPath;
    ShapeLayer backgroundLayer;
    TextLayer textLayer;
};

struct AnimatedView final : View
{
    AnimatedView()
    {
        ellipsePath.addEllipse({0, 0, 80.f, 80.f});
        ellipseLayer.setFillColor({1.f, 0.5f, 0.f});
        ellipseLayer.setOpacity(0.5f);
        ellipseLayer.setPath(ellipsePath);
        addLayer(ellipseLayer);
    }

    void update()
    {
        auto trans = AnimationTransaction();
        opacity += 0.02f;

        if (opacity >= 0.9f)
            opacity = 0.1f;

        ellipseLayer.setOpacity(opacity);

        x += dx;

        if (x < 10 || x > 500)
            dx = -dx;

        ellipseLayer.setPosition({x, 0.f});
    }

    Path ellipsePath;
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
        layer.setFillColor(colorToUse);
        addLayer(layer);
    }

    void resized() override
    {
        path.addRect(getLocalBounds());
        layer.setPath(path);
        layer.setBounds(getLocalBounds());
    }

    Path path;
    ShapeLayer layer;
};

struct StrokeRect final : View
{
    void paint(Context& ctx) override
    {
        ctx.setColor(Color {0.5f, 0.5f, 0.5f});
        ctx.setLineWidth(2.f);
        ctx.strokeRect(getLocalBounds());
    }
};

struct TextDisplay final : View
{
    TextDisplay()
    {
        titleLayer.setText("TextLayer Demo");
        titleLayer.setFont(titleFont);
        titleLayer.setColor({0.9f, 0.9f, 0.9f});
        addLayer(titleLayer);

        subtitleLayer.setText("Using cached CATextLayer");
        subtitleLayer.setFont(subtitleFont);
        subtitleLayer.setColor({0.9f, 0.9f, 0.9f});
        addLayer(subtitleLayer);
    }

    void resized() override
    {
        auto trans = AnimationTransaction();
        auto bounds = getLocalBounds();
        titleLayer.setBounds({0, 0, 300, 30});
        titleLayer.setPosition({20.f, bounds.h - 40.f});

        subtitleLayer.setBounds({0, 0, 300, 20});
        subtitleLayer.setPosition({20.f, bounds.h - 65.f});
    }

    Font titleFont {"Helvetica-Bold", 24.f};
    Font subtitleFont {"Helvetica", 14.f};
    TextLayer titleLayer;
    TextLayer subtitleLayer;
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
        text.setBounds(getLocalBounds());
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
