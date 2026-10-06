#include "ThreadUtils.h"

#include <functional>
#include <thread>

namespace eacp::Threads
{
std::uint64_t currentThreadId()
{
    return static_cast<std::uint64_t>(
        std::hash<std::thread::id> {}(std::this_thread::get_id()));
}
} // namespace eacp::Threads
