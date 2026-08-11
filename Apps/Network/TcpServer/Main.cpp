#include <eacp/Network/Network.h>

#include <iostream>

int main()
{
    try
    {
        // Zero timeouts block forever: on accept, and on client input.
        auto listener = eacp::TCP::Listener::bind(5050, {{0}, {0}});
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
