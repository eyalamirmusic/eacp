#pragma once

#include "ComputeProgram.h"

#include <functional>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <vector>

namespace eacp::GPU
{
namespace Detail
{
using KernelFactory = std::function<std::unique_ptr<ComputeProgram>()>;

ComputeProgram& findOrBuildKernel(Device& device,
                                  std::type_index type,
                                  std::vector<int> variant,
                                  const KernelFactory& build);
} // namespace Detail

// The one prepared Kernel on this Device: constructed - which records its graph
// and emits its source - and prepared the first time it is asked for, then
// handed back as it is. ComputeProgram::prepare already shares the compiled
// pipeline between kernels with the same source; this also skips the graph and
// the emission, which a kernel dispatched a thousand times a step would
// otherwise redo on every call. Constructor arguments are part of what tells
// two kernels apart, and so must be integers or enums.
//
// The instance is shared by every caller. A dispatch copies the uniforms and
// binds the buffers there and then, so each caller sets every member it
// declares before dispatching, and none may rely on a value a fresh kernel
// would have held. Use it from the Device's own thread.
//
// The first use on a Device builds the kernel; the shader compile under that is
// paid once per machine rather than once per launch, since compiled shaders are
// cached on disk between runs (see compileComputeCached).
template <typename Kernel, typename... Args>
Kernel& sharedKernel(Device& device, Args... args)
{
    static_assert(((std::is_integral_v<Args> || std::is_enum_v<Args>) && ...),
                  "kernel variants are keyed by integer or enum arguments");

    auto build = [&]
    {
        auto kernel = std::make_unique<Kernel>(args...);
        kernel->prepare(device);
        return std::unique_ptr<ComputeProgram> {std::move(kernel)};
    };

    auto& kernel = Detail::findOrBuildKernel(device,
                                             std::type_index {typeid(Kernel)},
                                             {static_cast<int>(args)...},
                                             build);

    return static_cast<Kernel&>(kernel);
}
} // namespace eacp::GPU
