#include "AlternateSignalStack.h"

#include <algorithm>
#include <csignal>

namespace eacp::Threads
{
namespace
{
bool hasAlternateSignalStack()
{
    auto current = stack_t {};

    if (::sigaltstack(nullptr, &current) != 0)
        return false;

    return (current.ss_flags & SS_DISABLE) == 0;
}

bool isUsingStack(const Vector<std::byte>& memory)
{
    auto current = stack_t {};
    ::sigaltstack(nullptr, &current);
    return current.ss_sp == memory.data() && (current.ss_flags & SS_DISABLE) == 0;
}
} // namespace

AlternateSignalStack::AlternateSignalStack(std::size_t size)
{
    if (hasAlternateSignalStack())
        return;

    auto stackSize = std::max(size, static_cast<std::size_t>(MINSIGSTKSZ));
    stackMemory.resize(static_cast<int>(stackSize));

    auto stack = stack_t {};
    stack.ss_sp = stackMemory.data();
    stack.ss_size = stackSize;
    stack.ss_flags = 0;
    installed = ::sigaltstack(&stack, nullptr) == 0;

    if (!installed)
        stackMemory.clear();
}

AlternateSignalStack::~AlternateSignalStack()
{
    if (!installed || !isUsingStack(stackMemory))
        return;

    auto disabled = stack_t {};
    disabled.ss_flags = SS_DISABLE;
    ::sigaltstack(&disabled, nullptr);
}

bool AlternateSignalStack::isInstalled() const
{
    return installed;
}
} // namespace eacp::Threads
