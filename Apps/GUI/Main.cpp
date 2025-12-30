
#include <eacp/App/App.h>
#include <eacp/Graphics/Window.h>
#include <eacp/Threads/Timer.h>
#include <iostream>

using namespace eacp::Graphics;

struct MyView : View
{
    void paint(Context& ctx) override
    {
        ctx.setFillColor(c);
        auto r = Rect(10.f, 10.f, 50.f, 50.f);
        ctx.fillRect(r);
    }

    void mouseDown(const MouseEvent&) override
    {
        c.a += 0.1f;

        if (c.a >= 0.9f)
            c.a = 0.1f;

        repaint();
    }

    Color c {0.f, 1.f, 0.f};
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