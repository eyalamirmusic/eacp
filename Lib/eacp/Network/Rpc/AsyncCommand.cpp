#include "AsyncCommand.h"

#include <thread>

namespace eacp::Rpc
{
void runOnWorkerThread(Callback work)
{
    std::thread(std::move(work)).detach();
}

void resolveWith(Threads::Async<Miro::Json::Value> work, Miro::Resolve completion)
{
    work.then([completion](Miro::Json::Value value) { completion(value, nullptr); },
              [completion](const std::string& error)
              { completion(Miro::Json::Value {}, &error); });
}
} // namespace eacp::Rpc
