#include <eacp/Core/Core.h>

#include <NanoTest/NanoTest.h>

#include <csignal>
#include <execinfo.h>
#include <unistd.h>

namespace
{
int argCount = 0;
char** argValues = nullptr;
int exitCode = 0;

void runTests()
{
    exitCode = nano::run(argCount, argValues);
}

void printBacktraceAndReraise(int signal)
{
    auto frames = eacp::Array<void*, 128> {};
    auto count = backtrace(frames.data(), (int) frames.getSize());
    backtrace_symbols_fd(frames.data(), count, STDERR_FILENO);

    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

void printBacktraceOnCrash()
{
    for (auto signal: {SIGTRAP, SIGILL, SIGABRT, SIGSEGV, SIGBUS})
        std::signal(signal, printBacktraceAndReraise);
}
} // namespace

int main(int argc, char* argv[])
{
    argCount = argc;
    argValues = argv;
    printBacktraceOnCrash();

    eacp::Apps::run(runTests);
    return exitCode;
}
