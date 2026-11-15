#include "AsyncCommand.h"

#include <eacp/Core/Platform/Platform.h>

#include <thread>

namespace eacp::Rpc
{
// With no threads to run it on, a worker-thread command runs on a later turn
// of the main thread's loop, as MainThreadDeferred does.
void runOnWorkerThread(Callback work)
{
    if constexpr (Platform::hasThreads())
        std::thread(std::move(work)).detach();
    else
        Threads::callAsync(std::move(work));
}

void resolveWith(Threads::Async<Miro::Json::Value> work, Miro::Resolve completion)
{
    work.then([completion](Miro::Json::Value value) { completion(value, nullptr); },
              [completion](const std::string& error)
              { completion(Miro::Json::Value {}, &error); });
}
} // namespace eacp::Rpc
