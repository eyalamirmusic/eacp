
#include <eacp/App/App.h>
#include <eacp/Graphics/Window.h>

using namespace eacp::Graphics;

struct MyView : View
{
    void paint(Context& ctx) override
    {
        auto c = Color(0.f, 1.f, 0.f);
        ctx.setFillColor(c);
        auto r = Rect(10.f, 10.f, 50.f, 50.f);
        ctx.fillRect(r);
    }
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