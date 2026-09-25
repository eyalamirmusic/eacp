#pragma once

#include "Bindings.h"
#include "Plan.h"
#include "Workspace.h"

#include <eacp/GPU/Codegen/ComputeKernel.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// Runs a compute kernel on the calling thread: one plan, one workspace, and
// the dispatch forms ComputePass has, indirect included. Everything is allocated by the
// constructor; a dispatch takes no lock, makes no system call and allocates
// nothing, so it can run on an audio thread.
//
// A dispatch can also be split over the caller's own threads: prepareDispatch
// takes the bindings and copies the uniforms once, and dispatchGroups then runs
// any range of its thread groups, each thread with a Workspace of its own made
// from plan(). Disjoint ranges of a race-free kernel give exactly what one
// dispatch gives, whichever thread runs which.

namespace eacp::GPU::CpuCompute
{
// A dispatch with its bindings resolved, its grid sized and a copy of the
// kernel's uniforms: a self-contained value, copied to each thread that runs
// part of it, which nothing done to the executor afterwards changes. Its
// thread groups are numbered x fastest, x + groupsX * (y + groupsY * z).
class PreparedDispatch
{
public:
    // False when the plan is invalid, the kernel was written for another rank,
    // or a slot it reads or writes is unbound or bound as the wrong kind.
    bool isValid() const { return valid; }

    // Zero for an extent of zero, which is valid and runs nothing.
    std::int64_t groupCount() const { return totalGroups; }
    const std::array<std::uint32_t, 3>& groups() const { return groupsPerAxis; }

private:
    friend class Executor;

    struct BoundSlot
    {
        std::byte* data = nullptr;
        std::uint32_t count = 0;
    };

    std::array<BoundSlot, Plan::maxSlots> slots {};
    std::array<Word, Plan::maxUniformWords> uniforms {};
    std::array<std::uint32_t, 3> extents {};
    std::array<std::uint32_t, 3> groupsPerAxis {};
    std::int64_t totalGroups = 0;
    std::uint64_t planSerial = 0;
    bool valid = false;
};

class Executor
{
public:
    // A kernel's own members: its uniforms are read back through the member
    // walk on every dispatch, so `kernel.gain = ...` a moment before is what
    // the dispatch sees. The kernel must outlive the executor.
    explicit Executor(ComputeKernel& kernel, Plan::Options options = {});

    // A bare graph, recorded on a ShaderBuilder: the uniforms are set by slot
    // with setUniform. The graph is decoded here and not referenced again.
    explicit Executor(const ShaderGraph& graph, Plan::Options options = {});

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

    // ComputePass::dispatchIndirect for a 1D kernel: the thread group counts
    // are the DispatchArguments at arguments[offsetInElements] - groups, not
    // threads - and guardCount is the extent the bounds guard and gridCount()
    // read. Groups in y and z repeat the x range, as on the GPU. Arguments
    // that do not hold a whole DispatchArguments at the offset, or a count of
    // zero on any axis, run nothing; like the direct forms, false means only
    // an invalid plan, a kernel of another rank or a slot left unbound.
    bool dispatchIndirect(const Bindings& bindings,
                          std::span<const std::uint32_t> arguments,
                          int guardCount,
                          int offsetInElements = 0);

    // The first half of each dispatch form above, on the calling thread: the
    // bindings resolved and the kernel's uniforms read and copied into the
    // result, once. A later setUniform, member change, prepare or dispatch on
    // this executor leaves it as it was; nothing runs until dispatchGroups.
    PreparedDispatch prepareDispatch(const Bindings& bindings, int count);
    PreparedDispatch
        prepareDispatch(const Bindings& bindings, int width, int height);
    PreparedDispatch
        prepareDispatch(const Bindings& bindings, int width, int height, int depth);
    PreparedDispatch
        prepareDispatchIndirect(const Bindings& bindings,
                                std::span<const std::uint32_t> arguments,
                                int guardCount,
                                int offsetInElements = 0);

    // Runs thread groups [first, first + count) of a prepared dispatch in
    // workspace, which must have been made from plan(). Only the part of the
    // range inside [0, groupCount()) runs, so a caller can split groupCount()
    // into equal shares and let the last run short. Any number of threads may
    // call this at once on one PreparedDispatch, each with its own workspace;
    // it is const, and allocates, locks and logs nothing. False, and nothing
    // run, for a prepared dispatch that is not valid, or one prepared by
    // another executor, or a workspace made from another plan.
    bool dispatchGroups(const PreparedDispatch& prepared,
                        std::int64_t first,
                        std::int64_t count,
                        Workspace& workspace) const;

private:
    using GridSize = std::array<int, 3>;

    PreparedDispatch
        prepare(const Bindings& bindings, DispatchRank rank, GridSize extents);
    bool resolveSlots(const Bindings& bindings, PreparedDispatch& prepared) const;
    void readUniforms(PreparedDispatch& prepared);
    bool runWhole(const PreparedDispatch& prepared);

    Plan executionPlan;
    Workspace scratch;
    Vector<Word> uniformBlock;
    ComputeKernel* kernel = nullptr;
};
} // namespace eacp::GPU::CpuCompute
