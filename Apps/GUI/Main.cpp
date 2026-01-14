
#include <eacp/App/App.h>
#include <eacp/Graphics/Window.h>
#include <eacp/Graphics/Path.h>
#include <eacp/Graphics/Font.h>
#include <eacp/Graphics/ShapeLayer.h>
#include <eacp/Threads/Timer.h>

using namespace eacp;
using namespace Graphics;

struct ColoredView final : View
{
    ColoredView(Color colorToUse, const std::string& labelText)
        : color(colorToUse)
        , label(labelText)
    {
        backgroundPath.addRoundedRect({0, 0, 150, 100}, 10.f);
        backgroundLayer.setPath(backgroundPath);
        backgroundLayer.setFillColor(color);
        addShapeLayer(backgroundLayer);
    }

    void paint(Context& ctx) override
    {
        ctx.setColor(textColor);
        auto bounds = getBounds();
        ctx.drawText(label, Point {10.f, bounds.h / 2.f - 7.f}, labelFont);
    }

    Color textColor {1.f, 1.f, 1.f};
    Font labelFont {"Helvetica-Bold", 14.f};
    Color color;
    std::string label;
    Path backgroundPath;
    ShapeLayer backgroundLayer;
};

struct AnimatedView final : View
{
    AnimatedView()
    {
        ellipsePath.addEllipse({0, 0, 80.f, 80.f});
        ellipseLayer.setPath(ellipsePath);
        ellipseLayer.setFillColor({1.f, 0.5f, 0.f});
        ellipseLayer.setOpacity(0.5f);
        addShapeLayer(ellipseLayer);
    }

    void update()
    {
        opacity += 0.02f;

        if (opacity >= 0.9f)
            opacity = 0.1f;

        ellipseLayer.setOpacity(opacity);

        auto bounds = getBounds();
        x += dx;

        if (x < 10 || x > 500)
            dx = -dx;

        setBounds({x, bounds.y, bounds.w, bounds.h});
    }

    Path ellipsePath;
    ShapeLayer ellipseLayer;
    float opacity = 0.5f;
    float x = 50.f;
    float dx = 2.f;
    Threads::Timer timer {[&] { update(); }, 60};
};

struct ParentView final : View
{
    ParentView()
    {
        child1.setBounds({50, 50, 150, 100});
        child2.setBounds({220, 50, 150, 100});
        child3.setBounds({390, 50, 150, 100});
        animatedChild.setBounds({50, 200, 100, 100});

        addSubview(child1);
        addSubview(child2);
        addSubview(child3);
        addSubview(animatedChild);
    }

    void paint(Context& ctx) override
    {
        auto bounds = getBounds();
        ctx.setColor(Color {0.1f, 0.1f, 0.1f});
        ctx.fillRect({0, 0, bounds.w, bounds.h});

        ctx.setColor(Color {0.5f, 0.5f, 0.5f});
        ctx.setLineWidth(2.f);
        ctx.strokeRect({0, 0, bounds.w, bounds.h});

        ctx.setColor(Color {0.9f, 0.9f, 0.9f});
        ctx.drawText("ShapeLayer Demo", {20.f, bounds.h - 40.f}, titleFont);

        ctx.drawText(
            "Using cached CAShapeLayer", {20.f, bounds.h - 65.f}, subtitleFont);
    }

    Font titleFont {"Helvetica-Bold", 24.f};
    Font subtitleFont {"Helvetica", 14.f};

    ColoredView child1 {{0.2f, 0.4f, 0.8f}, "Blue"};
    ColoredView child2 {{0.4f, 0.1f, 0.3f, 0.5f}, "Purple"};
    ColoredView child3 {{1.0, 0.f, 0.1f, 0.7f}, "Red"};
    AnimatedView animatedChild;
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
