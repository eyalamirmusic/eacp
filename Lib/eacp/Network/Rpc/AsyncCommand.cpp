#include "AsyncCommand.h"

#include <thread>

namespace eacp::Rpc
{
void runOnWorkerThread(Callback work)
{
    std::thread(std::move(work)).detach();
}
} // namespace eacp::Rpc
