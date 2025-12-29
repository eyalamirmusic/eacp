
#include <eacp/App/App.h>
#include <eacp/Graphics/Window.h>

struct MyApp
{
    eacp::Graphics::Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}