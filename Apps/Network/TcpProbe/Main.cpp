// Probes whether a TCP endpoint accepts connections and reports how long the
// verdict took. The timing is the point: a refused port answers in
// milliseconds, while only a genuinely unresponsive host should spend the
// connect timeout. Try it against a dead port, then against TcpServer:
//
//   TcpProbe --host 127.0.0.1 --port 5050 [--timeout-ms 5000]

#include <eacp/Network/TCP/Connection.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    using namespace std::chrono;

    auto host = std::string {"127.0.0.1"};
    auto port = std::uint16_t {5050};
    auto timeout = milliseconds {5000};

    for (auto i = 1; i < argc; ++i)
    {
        auto arg = std::string {argv[i]};
        if (arg == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (arg == "--port" && i + 1 < argc)
            port = (std::uint16_t) std::stoi(argv[++i]);
        else if (arg == "--timeout-ms" && i + 1 < argc)
            timeout = milliseconds {std::stoi(argv[++i])};
        else
        {
            std::cerr
                << "usage: TcpProbe [--host HOST] [--port PORT] [--timeout-ms MS]\n";
            return 2;
        }
    }

    const auto start = steady_clock::now();
    auto elapsedMs = [start]
    { return duration_cast<milliseconds>(steady_clock::now() - start).count(); };

    try
    {
        auto connection =
            eacp::TCP::Connection::connect({host, port}, {timeout, timeout});
        std::cout << host << ":" << port << " is open (verdict in " << elapsedMs()
                  << " ms)\n";
        return 0;
    }
    catch (const eacp::TCP::Error& e)
    {
        std::cout << host << ":" << port << " is not accepting connections "
                  << "(verdict in " << elapsedMs() << " ms): " << e.what() << "\n";
        return 1;
    }
}
