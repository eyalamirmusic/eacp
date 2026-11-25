#include "Process.h"

// For the platforms that start no child processes: iOS denies posix_spawn,
// fork and exec to third-party apps at run time, and the web has no processes.
// Every launch reports that it did not happen, as a failed spawn does elsewhere.
namespace eacp::Processes
{
struct Process::Native
{
    explicit Native(const ProcessOptions&) {}

    bool launched() const { return false; }
    bool isRunning() const { return false; }
    long id() const { return -1; }

    bool writeToInput(const std::string&) { return false; }
    void closeInput() {}

    int wait() { return -1; }
    void terminate() {}
    void kill() {}
    bool requestStop() { return false; }

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

bool Process::requestStop()
{
    return impl->requestStop();
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
