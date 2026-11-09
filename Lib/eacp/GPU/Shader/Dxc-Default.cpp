#include "Dxc.h"

#include <eacp/Core/Utils/Singleton.h>

#include <mutex>

// Every backend but D3D12 compiles its own dialect, so the options are kept for
// an app that sets them on every platform and change nothing.

namespace eacp::GPU::Dxc
{
namespace
{
struct DxcDefaultOptions
{
    std::mutex mutex;
    Options options;
};

DxcDefaultOptions& dxcDefaultOptions()
{
    return Singleton::getImmortal<DxcDefaultOptions>();
}
} // namespace

void setOptions(Options options)
{
    auto& stored = dxcDefaultOptions();
    auto lock = std::lock_guard {stored.mutex};
    stored.options = std::move(options);
}

Options getOptions()
{
    auto& stored = dxcDefaultOptions();
    auto lock = std::lock_guard {stored.mutex};
    return stored.options;
}

bool isEnabled()
{
    return getOptions().enabled;
}

Status load()
{
    return status();
}

Status status()
{
    return {.state = isEnabled() ? State::unsupported : State::disabled};
}
} // namespace eacp::GPU::Dxc
