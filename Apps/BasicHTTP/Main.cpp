#include <eacp/Network/Http.h>
#include <iostream>

int main()
{
    auto req = eacp::HTTP::Request("https://echo.free.beeceptor.com");
    req.type = "POST";
    req.body = "Here's some body";

    auto res = req.perform();
    std::cout << res.content;

    return 0;
}