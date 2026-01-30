#include <eacp/SVG/SVG.h>

using namespace eacp;

struct MyApp
{
    MyApp()
    {
        auto path = Files::getBundleResourcePath("example.svg");
        auto contents = Files::readFile(path);
        result = SVG::parse(contents);
        if (result.root)
        {
            result.root->setBounds({0, 0, result.width, result.height});
            result.root->stretchToFit();
            window.setContentView(*result.root);
        }
    }

    SVG::ParseResult result;
    Graphics::Window window;
};

int main()
{
    Apps::run<MyApp>();

    return 0;
}
