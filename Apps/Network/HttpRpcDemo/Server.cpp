#include "Schema.h"

#include <eacp/Network/HTTPRpc/RpcServer.h>

#include <iostream>

int main(int argc, char** argv)
{
    auto port = 8088;
    if (argc > 1)
        port = std::atoi(argv[1]);

    // Lifetime contract: api declared first → destructed last (after
    // the bridge's listeners and handlers have torn down).
    auto api = Api::PingApi {};
    auto bridge = Miro::Bridge {};
    bridge.use(api);

    auto httpServer = eacp::HTTP::Server {};
    auto rpc = eacp::HTTP::Rpc::Server {httpServer, bridge};

    if (!httpServer.listen(port))
    {
        std::cerr << "Failed to bind RPC server on port " << port << "\n";
        return 1;
    }

    std::cout << "RPC listening on http://localhost:" << port
              << "/rpc (Ctrl-C to quit)\n";

    eacp::Threads::runEventLoop();
    return 0;
}
