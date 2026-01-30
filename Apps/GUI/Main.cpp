#include <eacp/Graphics/Graphics.h>
#include <eacp/Core/Core.h>

using namespace eacp;
using namespace Graphics;

struct ChildView: View
{

};

struct ParentView final : View
{
    ParentView()
    {
        addChildren({child});
    }

    ChildView child;
};

struct MyApp
{
    MyApp()
    {
        window.setContentView(parentView);
        Apps::quit();
    }

    ParentView parentView;
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}
