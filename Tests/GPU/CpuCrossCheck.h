#pragma once

#include "Common.h"

#include <eacp/GPU/Codegen/PackedScalars.h>
#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <bit>
#include <cstdint>
#include <cmath>
#include <cstring>
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

inline Vector<float> asFloats(const Vector<std::uint32_t>& words)
{
    auto floats = Vector<float> {};

    for (auto word: words)
        floats.add(std::bit_cast<float>(word));

    return floats;
}

inline std::uint32_t wordOf(float value)
{
    return std::bit_cast<std::uint32_t>(value);
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
    bool isHalf = false;
    Vector<float> floats;
    Vector<std::uint32_t> uints;
    Vector<std::uint16_t> halves;
    int first = 0;
    int count = 0;

    // A buffer of structs: its bytes, held in `uints` rounded up to a word,
    // and the struct, which the agreement compares a field at a time - the
    // padding between fields being nobody's to agree on.
    const ShaderGraph* graph = nullptr;
    int structId = -1;
    int structBytes = 0;

    std::span<std::byte> structRange()
    {
        return {reinterpret_cast<std::byte*>(uints.data()),
                (std::size_t) structBytes};
    }

    std::function<void(CpuCompute::Bindings&, HostArray&)> bindOnCpu =
        [](CpuCompute::Bindings&, HostArray&) {};
    std::function<void(const BufferRange&)> bindOnGpu = [](const BufferRange&) {};

    int elements() const
    {
        if (isHalf)
            return halves.size();

        return isUInt ? uints.size() : floats.size();
    }

    std::int64_t elementBytes() const { return isHalf ? 2 : 4; }

    const void* bytes() const
    {
        if (isHalf)
            return halves.data();

        return isUInt ? (const void*) uints.data() : (const void*) floats.data();
    }

    void* bytes()
    {
        if (isHalf)
            return halves.data();

        return isUInt ? (void*) uints.data() : (void*) floats.data();
    }

    // The f16 elements widened, which is how two backends' halves compare.
    Vector<float> halvesAsFloats() const
    {
        auto widened = Vector<float> {};

        for (auto bits: halves)
            widened.add(halfToFloat(bits));

        return widened;
    }

    std::span<float> floatRange() { return range(floats); }
    std::span<std::uint32_t> uintRange() { return range(uints); }
    std::span<std::uint16_t> halfRange() { return range(halves); }

private:
    template <typename T>
    std::span<T> range(Vector<T>& values) const
    {
        auto whole = std::span<T> {values.data(), (std::size_t) values.size()};
        return whole.subspan((std::size_t) first, (std::size_t) count);
    }
};

// Every component of every field of every element: an f16 widened, a float
// within the tolerance, an integer bit for bit.
inline bool structComponentsAgree(ValueType type,
                                  const std::byte* cpu,
                                  const std::byte* gpu,
                                  float tolerance)
{
    auto cpuWords = Array<std::uint32_t, 16> {};
    auto gpuWords = Array<std::uint32_t, 16> {};
    detail::readStorageValue(cpu, type, cpuWords.data());
    detail::readStorageValue(gpu, type, gpuWords.data());

    auto isFloat = !isSignedInteger(type) && !isUnsignedInteger(type);

    for (auto component = 0; component < componentCount(type); ++component)
    {
        auto a = cpuWords[component];
        auto b = gpuWords[component];

        if (a == b)
            continue;

        if (!isFloat
            || std::abs(std::bit_cast<float>(a) - std::bit_cast<float>(b))
                   > tolerance)
            return false;
    }

    return true;
}

inline void expectStructsAgree(const HostArray& cpu,
                               const HostArray& gpu,
                               float tolerance,
                               const std::source_location& where)
{
    const auto& type = cpu.graph->structType(cpu.structId);
    const auto* cpuBytes = reinterpret_cast<const std::byte*>(cpu.uints.data());
    const auto* gpuBytes = reinterpret_cast<const std::byte*>(gpu.uints.data());
    auto agreeing = true;

    for (auto element = 0; element < cpu.structBytes / type.size; ++element)
        for (const auto& field: type.fields)
            if (field.type != ValueType::Struct)
            {
                auto at = element * type.size + field.offset;
                agreeing = agreeing
                           && structComponentsAgree(
                               field.type, cpuBytes + at, gpuBytes + at, tolerance);
            }

    nano::check(agreeing, "gpu vs cpu: every field of every struct", where);
}

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

    Vector<std::int32_t> ints(const IntAtomicBuffer& member) const
    {
        return intsAt(member.slot);
    }

    Vector<std::int32_t> ints(const IntOutputBuffer& member) const
    {
        return intsAt(member.slot);
    }

    const Vector<std::uint16_t>& halves(const HalfOutputBuffer& member) const
    {
        return find(member.slot).halves;
    }

    template <ShaderStructLike T>
    StructBufferData<T> structs(const StructOutputBuffer<T>& member) const
    {
        const auto& array = find(member.slot);
        auto data =
            StructBufferData<T> {array.structBytes / StructLayout<T>::stride};
        auto bytes = data.bytes();
        std::memcpy(bytes.data(), array.uints.data(), bytes.size());
        return data;
    }

    Backend backend;

private:
    Vector<std::int32_t> intsAt(int slot) const
    {
        auto values = Vector<std::int32_t> {};

        for (auto word: find(slot).uints)
            values.add(std::bit_cast<std::int32_t>(word));

        return values;
    }

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

    // The signed buffers, held as the words their int32s are.
    CrossCheck& output(Uniform<IntAtomicBuffer>& member,
                       const Vector<std::int32_t>& initial)
    {
        return addInts(member, initial);
    }

    CrossCheck& input(Uniform<IntInputBuffer>& member,
                      const Vector<std::int32_t>& values)
    {
        return addInts(member, values);
    }

    CrossCheck& output(Uniform<IntOutputBuffer>& member,
                       const Vector<std::int32_t>& initial)
    {
        return addInts(member, initial);
    }

    // The f16 buffers, held as their IEEE binary16 bits. The GPU half runs
    // only where the device takes f16 (ComputeProgram::fitsShaderFloat16).
    CrossCheck& input(Uniform<HalfInputBuffer>& member,
                      const Vector<std::uint16_t>& values)
    {
        auto& array = add(member.slot, values, 0, -1);
        array.bindOnCpu = [&member](CpuCompute::Bindings& bindings, HostArray& host)
        { nano::check(bindings.set(member, host.halfRange())); };
        array.bindOnGpu = [&member](const BufferRange& range) { member = range; };
        return *this;
    }

    CrossCheck& output(Uniform<HalfOutputBuffer>& member,
                       const Vector<std::uint16_t>& initial)
    {
        auto& array = add(member.slot, initial, 0, -1);
        array.bindOnCpu = [&member](CpuCompute::Bindings& bindings, HostArray& host)
        { nano::check(bindings.set(member, host.halfRange())); };
        array.bindOnGpu = [&member](const BufferRange& range) { member = range; };
        return *this;
    }

    // A buffer of structs (ShaderStruct.h), from its host bytes.
    template <ShaderStructLike T>
    CrossCheck& input(Uniform<StructInputBuffer<T>>& member,
                      const StructBufferData<T>& values)
    {
        auto& array = addStruct(member.slot, values.bytes());
        array.bindOnCpu = [&member](CpuCompute::Bindings& bindings, HostArray& host)
        {
            auto bytes = host.structRange();
            nano::check(bindings.set(member, std::span<const std::byte> {bytes}));
        };
        array.bindOnGpu = [&member](const BufferRange& range) { member = range; };
        return *this;
    }

    template <ShaderStructLike T>
    CrossCheck& output(Uniform<StructOutputBuffer<T>>& member,
                       const StructBufferData<T>& initial)
    {
        auto& array = addStruct(member.slot, initial.bytes());
        array.bindOnCpu = [&member](CpuCompute::Bindings& bindings, HostArray& host)
        { nano::check(bindings.set(member, host.structRange())); };
        array.bindOnGpu = [&member](const BufferRange& range) { member = range; };
        return *this;
    }

    // After both halves have run, every array the GPU read back is compared
    // with the CPU's, element for element - a struct's field for field. Not
    // for a kernel whose result depends on the order threads ran in, such as
    // atomic tickets.
    CrossCheck& agreeing(float tolerance = 0.f)
    {
        agreement = tolerance;
        return *this;
    }

    // The values the kernel's overrides take on both backends: the CPU plan's
    // and the GPU pipeline's (OverrideValues.h).
    CrossCheck& overriding(const OverrideValues& values)
    {
        overrides = values;
        return *this;
    }

    // Leaves the GPU half out, for a kernel a known driver cannot run (WARP's
    // ARM64 JIT crashes on the suite's largest, Common.h); the CPU half runs.
    CrossCheck& cpuOnlyWhen(bool condition)
    {
        cpuOnly = condition;
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

    template <typename Member>
    CrossCheck& addInts(Member& member, const Vector<std::int32_t>& values)
    {
        auto words = Vector<std::uint32_t> {};

        for (auto value: values)
            words.add(std::bit_cast<std::uint32_t>(value));

        auto& array = add(member.slot, words, 0, -1);
        array.bindOnCpu = [&member](CpuCompute::Bindings& bindings, HostArray& host)
        {
            auto held = host.uintRange();
            auto ints = std::span<std::int32_t> {
                reinterpret_cast<std::int32_t*>(held.data()), held.size()};
            nano::check(bindings.set(member, ints));
        };
        array.bindOnGpu = [&member](const BufferRange& range) { member = range; };
        return *this;
    }

    HostArray& addStruct(int slot, std::span<const std::byte> bytes)
    {
        auto words = Vector<std::uint32_t> {};
        words.resize((int) ((bytes.size() + 3) / 4), 0u);
        std::memcpy(words.data(), bytes.data(), bytes.size());

        auto& array = add(slot, words, 0, -1);
        array.graph = &kernel.graph();
        array.structId = kernel.graph().storageStructType(slot);
        array.structBytes = (int) bytes.size();
        return array;
    }

    template <typename T>
    HostArray& add(int slot, const Vector<T>& values, int first, int count)
    {
        auto array = HostArray {};
        array.slot = slot;
        array.isUInt = std::is_same_v<T, std::uint32_t>;
        array.isHalf = std::is_same_v<T, std::uint16_t>;
        array.first = first;
        array.count = count < 0 ? values.size() - first : count;

        if constexpr (std::is_same_v<T, std::uint32_t>)
            array.uints = values;
        else if constexpr (std::is_same_v<T, std::uint16_t>)
            array.halves = values;
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
        auto options = CpuCompute::PlanOptions {};
        options.overrides = overrides;

        auto executor = CpuCompute::Executor {kernel, options};
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
            if (gpuResult[index].structId >= 0)
                expectStructsAgree(
                    cpuResult[index], gpuResult[index], *agreement, where);
            else if (gpuResult[index].isHalf)
                expectAgreement(cpuResult[index].halvesAsFloats(),
                                gpuResult[index].halvesAsFloats(),
                                *agreement,
                                where);
            else if (gpuResult[index].isUInt)
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

        if (cpuOnly || !device.isValid() || !kernel.fitsShaderFloat16(device))
            return;

        auto buffers = Vector<Buffer> {};
        buffers.reserve(arrays.size());

        for (auto& array: arrays)
        {
            auto bytes = array.elementBytes();
            buffers.add(device.makeBuffer(
                array.bytes(), bytes * array.elements(), BufferUsage::Storage));
            array.bindOnGpu(
                {&buffers.back(), bytes * array.first, bytes * array.count});
        }

        kernel.prepare(device, overrides);
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
                                host[index].elementBytes() * host[index].elements());
        }

        verify(Readback {Backend::Gpu, host});
        expectBackendsAgree(host, where);
    }

    ComputeProgram& kernel;
    Vector<HostArray> arrays;
    Vector<HostArray> cpuResult;
    std::optional<float> agreement;
    OverrideValues overrides;
    bool cpuOnly = false;
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
