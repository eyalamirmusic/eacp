#include <eacp/SVG/SVG.h>
#include <eacp/Core/Core.h>

using namespace eacp;
using namespace Graphics;

struct ParentView final : View
{
    void resized() override
    {
    }
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
