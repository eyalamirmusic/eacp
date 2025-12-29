#include <eacp/Network/Http.h>
#include <eacp/App/App.h>
#include <iostream>

void urlFunc()
{
    auto req = eacp::HTTP::Request();
    req.url = "https://www.google.com";
    auto res = eacp::HTTP::httpRequest(req);
    std::cout << res.content << std::endl;

    eacp::Apps::quit();
}

struct MyApp
{
    MyApp()
    {
        std::cout << "started!\n";
        eacp::Apps::quit();
    }

    ~MyApp() { std::cout << "Quit!"; }
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}