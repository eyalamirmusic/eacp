// Simplest possible TCP client: connect to an echo server, send a line,
// print what comes back. Defaults to tcpbin.com:4242 (a public echo server);
// pass a host and port to aim it elsewhere, e.g. the local TcpServer:
//
//   TcpClient 127.0.0.1 5050

#include <eacp/Network/TCP/Connection.h>

#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    auto host = argc > 1 ? std::string {argv[1]} : "tcpbin.com";
    auto port = (std::uint16_t) (argc > 2 ? std::stoi(argv[2]) : 4242);

    auto connection = eacp::TCP::Connection::connect({host, port});

    connection.send("hello world\n");
    std::cout << "got back: " << connection.receiveLine() << "\n";

    return 0;
}
