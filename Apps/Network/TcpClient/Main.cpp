// Simplest possible TCP client: connect to an echo server, send a line,
// print what comes back. tcpbin.com:4242 echoes whatever you send it.

#include <eacp/Network/TCP/Connection.h>

#include <iostream>

int main()
{
    auto connection = eacp::TCP::Connection::connect({"tcpbin.com", 4242});

    connection.send("hello world\n");
    std::cout << "got back: " << connection.receiveLine() << "\n";

    return 0;
}
