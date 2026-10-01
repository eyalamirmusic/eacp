#pragma once

#include "Common.h"

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <cstdint>
#include <cmath>
#include <functional>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

// One kernel, two backends, one set of checks. A CrossCheck runs a
// ComputeProgram on the CPU over plain host arrays, always, and then - where
// Device::shared() is valid - on the GPU over Buffers holding the same bytes,
// and hands each backend's arrays to the same verify callback. The CPU half
// never self-skips, so a lane with no device still checks every number.
//
// A verify callback passes readback.name() as each check's text, so a failure
// says which backend produced it.

namespace eacp::GPU::CrossChecks
{
enum class Backend
{
    Cpu,
    Gpu
};

template <typename T>
Vector<T> filled(int elements, T value)
{
    auto values = Vector<T> {};
    values.assign(elements, value);
    return values;
}

// The two backends' copies of one array, element for element: floats within
// `tolerance`, integers exactly.
inline void expectAgreement(
    const Vector<float>& cpu,
    const Vector<float>& gpu,
    float tolerance = 0.f,
    const std::source_location& where = std::source_location::current())
{
    nano::check(cpu.size() == gpu.size(), "gpu vs cpu: the sizes", where);

    auto agreeing = 0;

    for (auto i = 0; i < cpu.size() && i < gpu.size(); ++i)
        if (cpu[i] == gpu[i] || std::abs(cpu[i] - gpu[i]) <= tolerance)
            ++agreeing;

    nano::check(agreeing == cpu.size(), "gpu vs cpu: every element", where);
}

inline void expectAgreement(
    const Vector<std::uint32_t>& cpu,
    const Vector<std::uint32_t>& gpu,
    const std::source_location& where = std::source_location::current())
{
    nano::check(cpu == gpu, "gpu vs cpu: every element", where);
}

struct HostArray
{
    int slot = -1;
    bool isUInt = false;
    Vector<float> floats;
    Vector<std::uint32_t> uints;
    int first = 0;
    int count = 0;

    std::function<void(CpuCompute::Bindings&, HostArray&)> bindOnCpu =
        [](CpuCompute::Bindings&, HostArray&) {};
    std::function<void(const BufferRange&)> bindOnGpu = [](const BufferRange&) {};

    int elements() const { return isUInt ? uints.size() : floats.size(); }
    const void* bytes() const
    {
        return isUInt ? (const void*) uints.data() : (const void*) floats.data();
    }
    void* bytes() { return isUInt ? (void*) uints.data() : (void*) floats.data(); }

    std::span<float> floatRange() { return range(floats); }
    std::span<std::uint32_t> uintRange() { return range(uints); }

private:
    template <typename T>
    std::span<T> range(Vector<T>& values) const
    {
        auto whole = std::span<T> {values.data(), (std::size_t) values.size()};
        return whole.subspan((std::size_t) first, (std::size_t) count);
    }
};

class Readback
{
public:
    Readback(Backend backendToUse, const Vector<HostArray>& arraysToRead)
        : backend(backendToUse)
        , arrays(arraysToRead)
    {
    }

    const char* name() const { return backend == Backend::Cpu ? "cpu" : "gpu"; }

    const Vector<float>& floats(const OutputBuffer& member) const
    {
        return find(member.slot).floats;
    }

    const Vector<std::uint32_t>& uints(const UIntOutputBuffer& member) const
    {
        return find(member.slot).uints;
    }

    const Vector<std::uint32_t>& uints(const AtomicBuffer& member) const
    {
        return find(member.slot).uints;
    }

    Backend backend;

private:
    const HostArray& find(int slot) const
    {
        for (const auto& array: arrays)
            if (array.slot == slot)
                return array;

        static const auto missing = HostArray {};
        nano::check(false, "the member was never given to the CrossCheck");
        return missing;
    }

    const Vector<HostArray>& arrays;
};

using Verify = std::function<void(const Readback&)>;

class CrossCheck
{
public:
    explicit CrossCheck(ComputeProgram& kernelToRun)
        : kernel(kernelToRun)
    {
    }

    // Everything from `first` on, or `count` elements from it: a count
    // binds a range, the CPU's subspan and the GPU's BufferRange alike.
    CrossCheck& input(Uniform<InputBuffer>& member,
                      const Vector<float>& values,
                      int first = 0,
                      int count = -1)
    {
        auto& array = add(member.slot, values, first, count);
        array.bindOnCpu = [&member](CpuCompute::Bindings& bindings, HostArray& host)
        { nano::check(bindings.set(member, host.floatRange())); };
        array.bindOnGpu = [&member](const BufferRange& range) { member = range; };
        return *this;
    }

    CrossCheck& input(Uniform<UIntInputBuffer>& member,
                      const Vector<std::uint32_t>& values,
                      int first = 0,
                      int count = -1)
    {
        auto& array = add(member.slot, values, first, count);
        array.bindOnCpu = [&member](CpuCompute::Bindings& bindings, HostArray& host)
        { nano::check(bindings.set(member, host.uintRange())); };
        array.bindOnGpu = [&member](const BufferRange& range) { member = range; };
        return *this;
    }

    // An output starting as `initial`: the elements the kernel does not
    // write read back as they were, which is how a sentinel shows a guard.
    CrossCheck& output(Uniform<OutputBuffer>& member,
                       const Vector<float>& initial,
                       int first = 0,
                       int count = -1)
    {
        auto& array = add(member.slot, initial, first, count);
        array.bindOnCpu = [&member](CpuCompute::Bindings& bindings, HostArray& host)
        { nano::check(bindings.set(member, host.floatRange())); };
        array.bindOnGpu = [&member](const BufferRange& range) { member = range; };
        return *this;
    }

    CrossCheck& output(Uniform<UIntOutputBuffer>& member,
                       const Vector<std::uint32_t>& initial,
                       int first = 0,
                       int count = -1)
    {
        auto& array = add(member.slot, initial, first, count);
        array.bindOnCpu = [&member](CpuCompute::Bindings& bindings, HostArray& host)
        { nano::check(bindings.set(member, host.uintRange())); };
        array.bindOnGpu = [&member](const BufferRange& range) { member = range; };
        return *this;
    }

    CrossCheck& output(Uniform<AtomicBuffer>& member,
                       const Vector<std::uint32_t>& initial,
                       int first = 0,
                       int count = -1)
    {
        auto& array = add(member.slot, initial, first, count);
        array.bindOnCpu = [&member](CpuCompute::Bindings& bindings, HostArray& host)
        { nano::check(bindings.set(member, host.uintRange())); };
        array.bindOnGpu = [&member](const BufferRange& range) { member = range; };
        return *this;
    }

    CrossCheck&
        output(Uniform<AtomicBuffer>& member, int elements, std::uint32_t fill = 0u)
    {
        return output(member, filled(elements, fill));
    }

    // After both halves have run, every array the GPU read back is compared
    // with the CPU's, element for element. Not for a kernel whose result
    // depends on the order threads ran in, such as atomic tickets.
    CrossCheck& agreeing(float tolerance = 0.f)
    {
        agreement = tolerance;
        return *this;
    }

    CrossCheck& output(Uniform<OutputBuffer>& member, int elements, float fill = 0.f)
    {
        return output(member, filled(elements, fill));
    }

    CrossCheck& output(Uniform<UIntOutputBuffer>& member,
                       int elements,
                       std::uint32_t fill = 0u)
    {
        return output(member, filled(elements, fill));
    }

    void run(int count,
             const Verify& verify,
             const std::source_location& where = std::source_location::current())
    {
        runBoth([count](CpuCompute::Executor& executor,
                        const CpuCompute::Bindings& bindings)
                { return executor.dispatch(bindings, count); },
                [count](ComputePass& pass, ComputeProgram& program)
                { pass.dispatch(program, count); },
                verify,
                where);
    }

    void run(int width,
             int height,
             const Verify& verify,
             const std::source_location& where = std::source_location::current())
    {
        runBoth([width, height](CpuCompute::Executor& executor,
                                const CpuCompute::Bindings& bindings)
                { return executor.dispatch(bindings, width, height); },
                [width, height](ComputePass& pass, ComputeProgram& program)
                { pass.dispatch(program, width, height); },
                verify,
                where);
    }

    void run(int width,
             int height,
             int depth,
             const Verify& verify,
             const std::source_location& where = std::source_location::current())
    {
        runBoth([width, height, depth](CpuCompute::Executor& executor,
                                       const CpuCompute::Bindings& bindings)
                { return executor.dispatch(bindings, width, height, depth); },
                [width, height, depth](ComputePass& pass, ComputeProgram& program)
                { pass.dispatch(program, width, height, depth); },
                verify,
                where);
    }

    // An indirect 1D dispatch whose DispatchArguments sit at
    // arguments[offsetInElements]; on the GPU the same words are a Buffer
    // read at offsetInElements * 4 bytes.
    void runIndirect(
        const Vector<std::uint32_t>& arguments,
        int guardCount,
        int offsetInElements,
        const Verify& verify,
        const std::source_location& where = std::source_location::current())
    {
        auto buffer = std::optional<Buffer> {};

        if (Device::shared().isValid())
            buffer.emplace(
                Device::shared().makeBuffer(arguments.data(),
                                            elementBytes * arguments.size(),
                                            BufferUsage::Storage));

        auto words = std::span<const std::uint32_t> {arguments.data(),
                                                     (std::size_t) arguments.size()};

        runBoth(
            [=](CpuCompute::Executor& executor, const CpuCompute::Bindings& bindings)
            {
                return executor.dispatchIndirect(
                    bindings, words, guardCount, offsetInElements);
            },
            [&](ComputePass& pass, ComputeProgram& program)
            {
                pass.dispatchIndirect(
                    program, *buffer, guardCount, elementBytes * offsetInElements);
            },
            verify,
            where);
    }

private:
    using CpuDispatch =
        std::function<bool(CpuCompute::Executor&, const CpuCompute::Bindings&)>;
    using GpuDispatch = std::function<void(ComputePass&, ComputeProgram&)>;

    static constexpr auto elementBytes = std::int64_t {4};

    template <typename T>
    HostArray& add(int slot, const Vector<T>& values, int first, int count)
    {
        auto array = HostArray {};
        array.slot = slot;
        array.isUInt = std::is_same_v<T, std::uint32_t>;
        array.first = first;
        array.count = count < 0 ? values.size() - first : count;

        if constexpr (std::is_same_v<T, std::uint32_t>)
            array.uints = values;
        else
            array.floats = values;

        arrays.add(std::move(array));
        return arrays.back();
    }

    void runBoth(const CpuDispatch& cpuDispatch,
                 const GpuDispatch& gpuDispatch,
                 const Verify& verify,
                 const std::source_location& where)
    {
        runOnCpu(cpuDispatch, verify, where);
        runOnGpu(gpuDispatch, verify, where);
    }

    void runOnCpu(const CpuDispatch& dispatch,
                  const Verify& verify,
                  const std::source_location& where)
    {
        auto executor = CpuCompute::Executor {kernel};
        nano::check(executor.isValid(), "cpu: " + executor.reason(), where);

        if (!executor.isValid())
            return;

        auto host = arrays;
        auto bindings = CpuCompute::Bindings {};

        for (auto& array: host)
            array.bindOnCpu(bindings, array);

        nano::check(dispatch(executor, bindings), "cpu: the dispatch ran", where);
        verify(Readback {Backend::Cpu, host});
        cpuResult = std::move(host);
    }

    void expectBackendsAgree(const Vector<HostArray>& gpuResult,
                             const std::source_location& where) const
    {
        if (!agreement || cpuResult.size() != gpuResult.size())
            return;

        for (auto index = 0; index < gpuResult.size(); ++index)
        {
            if (gpuResult[index].isUInt)
                expectAgreement(
                    cpuResult[index].uints, gpuResult[index].uints, where);
            else
                expectAgreement(cpuResult[index].floats,
                                gpuResult[index].floats,
                                *agreement,
                                where);
        }
    }

    void runOnGpu(const GpuDispatch& dispatch,
                  const Verify& verify,
                  const std::source_location& where)
    {
        auto& device = Device::shared();

        if (!device.isValid())
            return;

        auto buffers = Vector<Buffer> {};
        buffers.reserve(arrays.size());

        for (auto& array: arrays)
        {
            buffers.add(device.makeBuffer(array.bytes(),
                                          elementBytes * array.elements(),
                                          BufferUsage::Storage));
            array.bindOnGpu({&buffers.back(),
                             elementBytes * array.first,
                             elementBytes * array.count});
        }

        kernel.prepare(device);
        nano::check(kernel.pipeline().isValid(), "gpu: the pipeline built", where);

        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            dispatch(pass, kernel);
        }

        commands.commit();

        auto host = arrays;

        for (auto index = 0; index < host.size(); ++index)
        {
            host[index].bindOnGpu({});
            buffers[index].read(host[index].bytes(),
                                elementBytes * host[index].elements());
        }

        verify(Readback {Backend::Gpu, host});
        expectBackendsAgree(host, where);
    }

    ComputeProgram& kernel;
    Vector<HostArray> arrays;
    Vector<HostArray> cpuResult;
    std::optional<float> agreement;
};

// A kernel run on the CPU by hand, for the cases whose bindings a CrossCheck
// cannot express - two kernels over one array, a range past its end. The
// executor is checked valid, with its reason, and the dispatch checked to run.
template <typename... Extents>
bool dispatchOnCpu(ComputeKernel& kernel,
                   const CpuCompute::Bindings& bindings,
                   Extents... extents)
{
    auto executor = CpuCompute::Executor {kernel};
    nano::check(executor.isValid(), "cpu: " + executor.reason());

    if (!executor.isValid())
        return false;

    auto ran = executor.dispatch(bindings, extents...);
    nano::check(ran, "cpu: the dispatch ran");
    return ran;
}

// The indirect twin: the arguments are host words another CPU dispatch may
// have written, read at offsetInElements.
inline bool dispatchIndirectOnCpu(ComputeKernel& kernel,
                                  const CpuCompute::Bindings& bindings,
                                  std::span<const std::uint32_t> arguments,
                                  int guardCount,
                                  int offsetInElements = 0)
{
    auto executor = CpuCompute::Executor {kernel};
    nano::check(executor.isValid(), "cpu: " + executor.reason());

    if (!executor.isValid())
        return false;

    auto ran =
        executor.dispatchIndirect(bindings, arguments, guardCount, offsetInElements);
    nano::check(ran, "cpu: the dispatch ran");
    return ran;
}
} // namespace eacp::GPU::CrossChecks
