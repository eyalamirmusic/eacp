#include "Process.h"

#include <cassert>

// iOS sandboxes third-party apps and forbids process creation: posix_spawn,
// fork and exec are all denied at runtime (EPERM), so the POSIX implementation
// cannot function here regardless of what compiles. This stub fails loudly in
// debug if a launch is attempted and otherwise reports a clean not-launched
// result, so run() returns ProcessResult{launched = false}.
namespace eacp::Processes
{
struct Process::Native
{
    explicit Native(const ProcessOptions&)
    {
        assert(false && "Process spawning is unavailable on iOS");
    }

    bool launched() const { return false; }
    bool isRunning() const { return false; }
    long id() const { return -1; }

    bool writeToInput(const std::string&) { return false; }
    void closeInput() {}

    int wait() { return -1; }
    void terminate() {}
    void kill() {}

    std::string output() const { return {}; }
    std::string errorOutput() const { return {}; }
};

Process::Process(ProcessOptions options)
    : impl(std::move(options))
{
}

Process::~Process() = default;

bool Process::launched() const
{
    return impl->launched();
}
bool Process::isRunning() const
{
    return impl->isRunning();
}
long Process::id() const
{
    return impl->id();
}

bool Process::writeToInput(const std::string& data)
{
    return impl->writeToInput(data);
}

void Process::closeInput()
{
    impl->closeInput();
}

int Process::wait()
{
    return impl->wait();
}
void Process::terminate()
{
    impl->terminate();
}
void Process::kill()
{
    impl->kill();
}

std::string Process::output() const
{
    return impl->output();
}
std::string Process::errorOutput() const
{
    return impl->errorOutput();
}
} // namespace eacp::Processes
