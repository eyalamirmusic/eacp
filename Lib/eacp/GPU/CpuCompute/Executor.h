#pragma once

#include "Bindings.h"
#include "Plan.h"
#include "Workspace.h"

#include <eacp/GPU/Codegen/ComputeKernel.h>

#include <array>
#include <string>

// Runs a compute kernel on the calling thread: one plan, one workspace, and
// the three dispatch forms ComputePass has. Everything is allocated by the
// constructor; a dispatch takes no lock, makes no system call and allocates
// nothing, so it can run on an audio thread.

namespace eacp::GPU::CpuCompute
{
class Executor
{
public:
    // A kernel's own members: its uniforms are read back through the member
    // walk on every dispatch, so `kernel.gain = ...` a moment before is what
    // the dispatch sees. The kernel must outlive the executor.
    explicit Executor(ComputeKernel& kernel);

    // A bare graph, recorded on a ShaderBuilder: the uniforms are set by slot
    // with setUniform. The graph is decoded here and not referenced again.
    explicit Executor(const ShaderGraph& graph);

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    bool isValid() const { return executionPlan.isValid(); }
    const std::string& reason() const { return executionPlan.reason(); }
    const Plan& plan() const { return executionPlan; }

    // The value of uniform `slot`, tightly packed as Uniform<T>::value is;
    // bytes must be byteSize of the slot's type. For an executor built over a
    // kernel, the member walk overwrites it at the next dispatch.
    bool setUniform(int slot, const void* data, int bytes);

    // Runs the kernel over count threads (1D), a width x height grid (2D) or a
    // width x height x depth volume (3D), rounded up to whole thread groups
    // with the bounds guard masking the rest. False, and nothing run, when the
    // plan is invalid, the kernel was written for another rank, or a slot the
    // kernel reads or writes is unbound or bound as the wrong kind. An extent
    // of zero or less runs nothing and is not an error.
    bool dispatch(const Bindings& bindings, int count);
    bool dispatch(const Bindings& bindings, int width, int height);
    bool dispatch(const Bindings& bindings, int width, int height, int depth);

private:
    using GridSize = std::array<int, 3>;

    bool run(const Bindings& bindings, DispatchRank rank, GridSize extents);

    Plan executionPlan;
    Workspace scratch;
    ComputeKernel* kernel = nullptr;
};
} // namespace eacp::GPU::CpuCompute
