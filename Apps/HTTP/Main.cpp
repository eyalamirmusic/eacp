#include <eacp/Core/Core.h>

int main()
{
    auto url = "https://echo.free.beeceptor.com";
    auto req = eacp::HTTP::Request::post(url, "Hey, I'm a body");
    req.headers["Secret"] = "MySecret";
    auto res = req.perform();

    eacp::LOG(res.content);
    return 0;
}