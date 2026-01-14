
#include <eacp/App/App.h>
#include <eacp/Graphics/Window.h>
#include <eacp/Graphics/Path.h>
#include <eacp/Graphics/Font.h>
#include <eacp/Threads/Timer.h>
#include <string>

using namespace eacp;
using namespace Graphics;
using namespace std::string_literals;

struct ColoredView final : View
{
    ColoredView(Color colorToUse, const std::string& labelText = "")
        : color(colorToUse)
        , label(labelText)
    {
    }

    void paint(Context& ctx) override
    {
        ctx.setColor(color);
        auto bounds = getBounds();
        ctx.fillRoundedRect({0, 0, bounds.w, bounds.h}, 10.f);

        ctx.setColor(textColor);
        ctx.drawText(label, Point {10.f, bounds.h / 2.f - 7.f}, labelFont);
    }

    Color textColor {1.f, 1.f, 1.f};
    Font labelFont {"Helvetica-Bold", 14.f};
    Color color;
    std::string label;
};

struct AnimatedView final : View
{
    AnimatedView() { p.addEllipse({0, 0, 80.f, 80.f}); }

    void paint(Context& ctx) override
    {
        ctx.setColor(c);
        ctx.fillPath(p);
    }

    void update()
    {
        c.a += 0.02f;

        if (c.a >= 0.9f)
            c.a = 0.1f;

        auto bounds = getBounds();
        x += dx;

        if (x < 10 || x > 500)
            dx = -dx;

        setBounds({x, bounds.y, bounds.w, bounds.h});
        repaint();
    }

    Path p;
    Color c {1.f, 0.5f, 0.f};
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
        ctx.drawText("Text Rendering Example", {20.f, bounds.h - 40.f}, titleFont);

        ctx.drawText("Using CoreText", {20.f, bounds.h - 65.f}, subtitleFont);
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