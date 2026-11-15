#include <eacp/Core/Core.h>
#include <eacp/Network/Network.h>

#include <memory>
#include <tuple>

using namespace eacp;

struct QuitWithFetchHarness
{
    QuitWithFetchHarness()
    {
        auto args = Apps::commandLineArgs();

        if (args.size() < 2)
        {
            Console::printError("usage: QuitWithFetchHarness <test server URL>");
            Apps::quit(1);
            return;
        }

        auto server = args.back();

        HTTP::asyncRequest(HTTP::Request {server + "/slow-headers?ms=800"},
                           [this](const HTTP::Response&)
                           { fail("the request answered before the quit"); });

        webSocket = std::make_unique<WebSocket::Connection>(
            "ws" + server.substr(4) + "/ws", WebSocket::Callbacks {});

        Threads::callAfter(Time::MS {200}, [this] { quitWithBothInFlight(); });
    }

    void fail(const std::string& reason)
    {
        failed = true;
        Console::printError(Strings::concat("QuitWithFetchHarness: ", reason));
        Apps::quit(1);
    }

    void quitWithBothInFlight()
    {
        if (failed)
            return;

        if (webSocket->state() != WebSocket::State::open)
            return fail("the socket had not opened by the quit");

        webSocket->send("echoed after the exit");
        leaveTheSocketOpenAcrossTheExit();
        Apps::quit(0);
    }

    void leaveTheSocketOpenAcrossTheExit() { std::ignore = webSocket.release(); }

    std::unique_ptr<WebSocket::Connection> webSocket;
    bool failed = false;
};

int main(int argc, char* argv[])
{
    return Apps::run<QuitWithFetchHarness>(argc, argv);
}
