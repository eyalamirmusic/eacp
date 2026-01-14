#include <eacp/Core/Core.h>

struct App
{
    App() { eacp::Apps::quit(); }
};

int main()
{
    eacp::Apps::run<App>();
    return 0;
}