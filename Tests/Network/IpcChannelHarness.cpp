#include <eacp/Network/Network.h>

#include <cstdio>
#include <cstdlib>
#include <string>

// A standalone peer for the channel tests, so a stream can be watched
// crossing a real process boundary - including a server that dies without
// cleaning up, which is the only way to prove a successor reclaims the name.
//
//   IpcChannelHarness <name> client <message>
//   IpcChannelHarness <name> serve
//   IpcChannelHarness <name> abandon
namespace
{
constexpr auto succeeded = 0;
constexpr auto mismatched = 3;
constexpr auto failed = 4;
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
        return failed;

    auto name = std::string {argv[1]};
    auto mode = std::string {argv[2]};

    try
    {
        if (mode == "client")
        {
            if (argc < 4)
                return failed;

            auto message = std::string {argv[3]};
            auto channel = eacp::IPC::Channel::connect(name);
            channel.send(message + "\n");
            return channel.receiveLine() == "echo:" + message ? succeeded
                                                              : mismatched;
        }

        if (mode == "serve")
        {
            auto server = eacp::IPC::ChannelServer {name};

            // The parent waits to see this before dialing, so a test is
            // timing the channel rather than racing this process's startup.
            std::puts("listening");
            std::fflush(stdout);

            auto channel = server.accept(eacp::Time::MS {10000});

            if (!channel)
                return failed;

            auto line = channel->receiveLine();
            channel->send("echo:" + line + "\n");
            return succeeded;
        }

        if (mode == "abandon")
        {
            auto server = eacp::IPC::ChannelServer {name};
            std::puts("listening");
            std::fflush(stdout);

            // Skips every destructor, leaving the endpoint and the lock
            // handle to the kernel - a stand-in for a crash.
            std::_Exit(succeeded);
        }
    }
    catch (const eacp::IPC::Error&)
    {
        return failed;
    }

    return failed;
}
