// Simplest possible TCP server: listen on a port, accept one client, and
// echo every line back until they hang up. The mirror image of TcpClient -
// together they exercise both halves of the library with no external server.

#include <eacp/Network/TCP/Listener.h>

#include <iostream>

int main()
{
    auto listener = eacp::TCP::Listener::bind(5050);
    std::cout << "listening on port " << listener.port() << " ...\n";

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

    return 0;
}
