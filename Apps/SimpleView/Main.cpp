#include <eacp/Graphics/Graphics.h>
#include <eacp/Core/Core.h>

using namespace eacp;
using namespace Graphics;

struct ParentView final : View
{

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
