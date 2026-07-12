#include <Miro/Reflect.h>
#include <eacp/Network/Network.h>
#include <iostream>

struct Req
{
    std::string text;

    MIRO_REFLECT(text)
};

int main()
{
    auto req = eacp::HTTP::Request(
        "https://tamber-embed-server-620733406514.us-central1.run.app/embed");

    req.type = "POST";
    req.headers["secret"] = "MagicTheGathering";

    for (int index = 0; index < 1000; ++index)
    {
        Req r;
        r.text = std::to_string(index);
        req.body = Miro::toJSONString(r);
        auto res = req.perform();
        std::cout << res.content << std::endl;
    }

    return 0;
}