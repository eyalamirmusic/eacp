// Simplest possible TCP server: listen on a port and echo every line back.
// The mirror image of TcpClient - together they exercise both halves of the
// library with no external server. Ctrl-C to stop.

#include <eacp/Network/Network.h>

#include <iostream>

int main()
{
    using namespace std::chrono_literals;

    try
    {
        // Zero timeouts == block forever: wait for a client, and wait on its
        // input. That is what a long-lived server wants.
        auto listener = eacp::TCP::Listener::bind(5050, {0ms, 0ms});
        std::cout << "listening on port " << listener.port()
                  << " (ctrl-c to stop)\n";

        while (true)
        {
            auto client = listener.accept();
            std::cout << "client connected from " << client.address().host << "\n";

            try
            {
                while (true)
                    client.send(client.receiveLine() + "\n");
            }
            catch (const eacp::TCP::Error&)
            {
                std::cout << "client disconnected\n";
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "TcpServer: " << e.what() << "\n";
        return 1;
    }
}
