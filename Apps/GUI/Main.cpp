
#include <eacp/App/App.h>
#include <eacp/Graphics/Window.h>
#include <eacp/Threads/Timer.h>
#include <iostream>

using namespace eacp;
using namespace Graphics;

struct MyView : View
{
    void paint(Context& ctx) override
    {
        ctx.setColor(c);
        auto r = Rect(10.f, 10.f, 100.f, 100.f);
        ctx.strokeRect(r);
    }

    void update()
    {
        c.a += 0.04f;

        if (c.a >= 0.9f)
            c.a = 0.1f;

        repaint();
    }

    Color c {0.f, 1.f, 0.f};
    Threads::Timer timer {[&] { update(); }, 30};
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