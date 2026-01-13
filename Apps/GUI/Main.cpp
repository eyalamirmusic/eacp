
#include <eacp/App/App.h>
#include <eacp/Graphics/Window.h>
#include <eacp/Graphics/Path.h>
#include <eacp/Threads/Timer.h>

using namespace eacp;
using namespace Graphics;

struct ColoredView final : View
{
    ColoredView(Color colorToUse)
        : color(colorToUse)
    {
    }

    void paint(Context& ctx) override
    {
        ctx.setColor(color);
        auto bounds = getBounds();
        ctx.fillRoundedRect({0, 0, bounds.w, bounds.h}, 10.f);
    }

    Color color;
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
    }

    ColoredView child1 {{0.2f, 0.4f, 0.8f}};
    ColoredView child2 {{0.4f, 0.1f, 0.3f, 0.5f}};
    ColoredView child3 {{1.0, 0.f, 0.1f, 0.7f}};
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