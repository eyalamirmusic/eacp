// Simplest possible TCP client: connect to an echo server, send a line,
// print what comes back. Defaults to tcpbin.com:4242 (a public echo server);
// point it at the local TcpServer with:
//
//   TcpClient --host 127.0.0.1 --port 5050

#include <eacp/Network/Network.h>

#include <iostream>

int main(int argc, char** argv)
{
    auto host = std::string {"tcpbin.com"};
    auto port = std::uint16_t {4242};

    for (auto i = 1; i < argc; ++i)
    {
        auto arg = std::string {argv[i]};
        if (arg == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (arg == "--port" && i + 1 < argc)
            port = (std::uint16_t) std::stoi(argv[++i]);
        else
        {
            std::cerr << "usage: TcpClient [--host HOST] [--port PORT]\n";
            return 2;
        }
    }

    try
    {
        auto connection = eacp::TCP::Connection::connect({host, port});
        connection.send("hello world\n");
        std::cout << "got back: " << connection.receiveLine() << "\n";
        return 0;
    }
    catch (const eacp::TCP::Error& e)
    {
        std::cerr << "TcpClient: " << e.what() << "\n";
        return 1;
    }
}
