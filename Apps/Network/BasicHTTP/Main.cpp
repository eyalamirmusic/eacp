#include <Miro/Reflect.h>
#include <eacp/Core/App/Args.h>
#include <eacp/Network/Network.h>
#include <iostream>

// A reflected struct sent as JSON in ten POSTs to an echo server, each answer
// printed: httpbingo.org by default, which sends CORS headers and takes the ten
// at once, or the httpbin-compatible server given as the argument. Exits 1 if
// any request failed.
struct Req
{
    std::string text;

    MIRO_REFLECT(text)
};

int main(int argc, char** argv)
{
    auto server = std::string {"https://httpbingo.org"};

    auto cli = eacp::Args::Parser {"BasicHttp", "Ten JSON POSTs, echoed back."};
    cli.positional(server, "server", "an httpbin-compatible server");

    if (auto code = cli.exitCodeFor(cli.parse(argc, argv)))
        return *code;

    while (server.ends_with("/"))
        server.pop_back();

    auto req = eacp::HTTP::Request(server + "/post");

    req.type = "POST";
    req.headers["Content-Type"] = "application/json";

    auto failures = 0;

    for (int index = 0; index < 10; ++index)
    {
        Req r;
        r.text = std::to_string(index);
        req.body = Miro::toJSONString(r);
        auto res = req.perform();
        std::cout << res.content << std::endl;

        if (!res.error.empty() || res.statusCode != 200)
        {
            std::cout << "request " << index << " failed: status " << res.statusCode
                      << " " << res.error << std::endl;
            ++failures;
        }
    }

    return failures == 0 ? 0 : 1;
}
