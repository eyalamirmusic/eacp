#include <eacp/Network/Http.h>
#include <eacp/App/App.h>
#include <iostream>
#include <thread>

using EndFunc = std::function<void(eacp::HTTP::Response)>;

void urlFunc(EndFunc func)
{
    auto req = eacp::HTTP::Request();
    req.url = "https://www.google.com";
    auto res = eacp::HTTP::httpRequest(req);
    std::cout << "URL req completed\n";

    auto endFunc = [res, func] { func(res); };

    eacp::Threads::callAsync(endFunc);
}

struct MyApp
{
    MyApp()
    {
        std::cout << "started!\n";
        t = std::thread([&] { threadFunc(); });
    }

    ~MyApp()
    {
        std::cout << "Quit!";
        t.join();
    }

    void threadFunc()
    {
        auto endFunc = [](eacp::HTTP::Response response)
        {
            std::cout << response.content << std::endl;
            eacp::Apps::quit();
        };

        urlFunc(endFunc);
    }

    std::thread t;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}