#include <eacp/Core/Core.h>

#include <stdexcept>

using namespace eacp;

// Linked with the nested pump: main() suspends in one, so it is running as a
// promise when the exception leaves it, and the run must end at once with the
// exception's message and a non-zero code rather than staying alive.
int main()
{
    Threads::runEventLoopFor(Time::MS {20});
    throw std::runtime_error("ThrowFromMainHarness threw on purpose");
}
