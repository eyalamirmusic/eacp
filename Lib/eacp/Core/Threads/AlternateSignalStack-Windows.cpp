#include "AlternateSignalStack.h"

#include <eacp/Core/Utils/WinInclude.h>

namespace eacp::Threads
{
// Windows has no alternate stack, only a reserve kept back for the exception
// filter after a stack overflow. It can only grow, so the destructor leaves
// it, and a thread that already reserves as much is left as it is.
AlternateSignalStack::AlternateSignalStack(std::size_t size)
{
    auto current = ULONG {0};
    ::SetThreadStackGuarantee(&current);

    if (current >= size)
        return;

    auto reserve = static_cast<ULONG>(size);
    installed = ::SetThreadStackGuarantee(&reserve) != 0;
}

AlternateSignalStack::~AlternateSignalStack() = default;

bool AlternateSignalStack::isInstalled() const
{
    return installed;
}
} // namespace eacp::Threads
