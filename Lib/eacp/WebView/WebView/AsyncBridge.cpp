#include "AsyncBridge.h"

#include <thread>

namespace eacp::Graphics
{
void runOnWorkerThread(Callback work)
{
    std::thread(std::move(work)).detach();
}
} // namespace eacp::Graphics
