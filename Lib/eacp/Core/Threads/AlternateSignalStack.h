#pragma once

#include "../Utils/Containers.h"

#include <cstddef>

namespace eacp::Threads
{
// A stack overflow leaves its thread no stack to run a signal handler on, so
// the crash reporter's handler could not run and the process would die with
// no report. This gives the thread a separate block the kernel runs signal
// handlers on instead: sigaltstack(2), https://man7.org/linux/man-pages/man2/sigaltstack.2.html
// and https://www.gnu.org/software/libc/manual/html_node/Signal-Stack.html.
// Windows has no alternate stack; the same job is a SetThreadStackGuarantee
// reserve. Every Threads::Thread holds one; any other thread can make its own.
class AlternateSignalStack
{
public:
    static constexpr auto defaultSize = std::size_t {64 * 1024};

    explicit AlternateSignalStack(std::size_t size = defaultSize);
    ~AlternateSignalStack();

    AlternateSignalStack(const AlternateSignalStack&) = delete;
    AlternateSignalStack& operator=(const AlternateSignalStack&) = delete;

    [[nodiscard]] bool isInstalled() const;

private:
    Vector<std::byte> stackMemory;
    bool installed = false;
};
} // namespace eacp::Threads
