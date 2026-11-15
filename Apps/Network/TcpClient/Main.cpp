// Simplest possible TCP client: connect to an echo server, send a line,
// print what comes back. Defaults to tcpbin.com:4242 (a public echo server);
// point it at the local TcpServer with:
//
//   TcpClient --host 127.0.0.1 --port 5050

#include <eacp/Core/App/Args.h>
#include <eacp/Network/Network.h>

#include <iostream>

int main(int argc, char** argv)
{
    auto host = std::string {"tcpbin.com"};
    auto port = std::uint16_t {4242};

    auto cli = eacp::Args::Parser {
        "TcpClient",
        "Connects to an echo server, sends a line and prints what comes back."};
    cli.option("--host", host, "the echo server's host name or address")
        .hint("<host>");
    cli.option("--port", port, "the echo server's port").atLeast(1).atMost(65535);
    cli.epilogue("Point it at the local TcpServer with:\n"
                 "  TcpClient --host 127.0.0.1 --port 5050");

    if (auto code = cli.exitCodeFor(cli.parse(argc, argv)))
        return *code;

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
