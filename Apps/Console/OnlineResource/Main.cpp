#include <eacp/Core/App/Args.h>
#include <eacp/Core/Core.h>
#include <eacp/Network/Network.h>

using namespace eacp;

// A console app that needs a file from the network asks for it and has it on
// disk when the call returns. The first run downloads it into this app's own
// Application Support folder; later runs find it there and, when the server
// gave an ETag or a Last-Modified, only ask it whether the file changed. It
// throws if the file could not be had. The file is the robots.txt of
// httpbingo.org, which sends CORS headers, so this runs in a browser page too,
// or of the server given as the argument.
int main(int argc, char** argv)
{
    auto server = std::string {"https://httpbingo.org"};

    auto cli = Args::Parser {"OnlineResourceDemo", "Fetches one file, once."};
    cli.positional(server, "server", "the server to fetch /robots.txt from");

    if (auto code = cli.exitCodeFor(cli.parse(argc, argv)))
        return *code;

    while (server.ends_with("/"))
        server.pop_back();

    auto res = OnlineResource::fetch({.info = {.url = server + "/robots.txt"}});
    LOG(res->read());
}
