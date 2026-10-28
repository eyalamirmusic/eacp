#pragma once

#include <cstddef>
#include <memory>

namespace eacp::Threads
{
// Room for the calling thread's crash handling to run in after its own stack
// has run out. On POSIX a sigaltstack, so a SA_ONSTACK handler still runs
// after a stack overflow; on Windows a SetThreadStackGuarantee reserve, its
// equivalent for structured exception filters. Every Threads::Thread holds
// one for its life; a thread eacp did not create makes one at its start and
// keeps it until it exits. A thread that already has an alternate stack keeps
// it, and this one does nothing.
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
    std::unique_ptr<std::byte[]> memory;
    bool installed = false;
};
} // namespace eacp::Threads
