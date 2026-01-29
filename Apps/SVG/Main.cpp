#include <eacp/SVG/SVG.h>
#include <eacp/Core/Core.h>

using namespace eacp;

struct MyApp
{
    MyApp()
    {
        auto path = Files::getBundleResourcePath("example.svg");
        auto contents = Files::readFile(path);
        LOG(contents);
        Apps::quit();
    }
};

int main()
{
    Apps::run<MyApp>();
    return 0;
}
