
#include <eacp/App/App.h>
#include <eacp/Graphics/Window.h>
#include <eacp/Threads/DisplayLink.h>

using namespace eacp;
using namespace Graphics;

struct MyView : View
{
    MyView()
    {
        p.addEllipse({0.1f, 0.1f, 0.8f, 0.8f});
    }

    void paint(Context& ctx) override
    {
        ctx.saveState();
        ctx.scale(getBounds().w, getBounds().h);
        ctx.setColor(c);
        ctx.fillPath(p);
        ctx.restoreState();
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
    Threads::DisplayLink displayLink {[&] { update(); }};
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