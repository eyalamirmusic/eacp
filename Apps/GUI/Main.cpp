
#include <eacp/App/App.h>
#include <eacp/Graphics/Window.h>
#include <eacp/Threads/Timer.h>

using namespace eacp;
using namespace Graphics;

struct MyView final : View
{
    MyView() { p.addEllipse({0.1f, 0.1f, 200.f, 200.f}); }

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

        repaint();
    }

    Path p;
    Color c {0.f, 1.f, 0.f};
    Threads::Timer timer {[&] { update(); }, 60};
};

struct MyApp
{
    MyApp() { window.setContentView(view); }

    MyView view;
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}