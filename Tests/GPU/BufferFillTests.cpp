#include "Common.h"

#include <cstdint>

// CommandBuffer::fill - a fill the GPU performs, recorded on the command buffer
// and ordered against the passes before and after it, so a buffer is re-zeroed
// between dispatches without the bytes ever coming from the host.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto count = 64;
constexpr auto byteCount = (int) sizeof(std::uint32_t) * count;
constexpr auto pattern = (std::uint8_t) 0xab;
constexpr auto sentinel = (std::uint32_t) 0x11111111;

struct CopyKernel final : ComputeProgram
{
    CopyKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i]);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct RampKernel final : ComputeProgram
{
    RampKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, toFloat(i));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

std::uint32_t repeated(std::uint8_t value)
{
    const auto word = (std::uint32_t) value;
    return word | (word << 8) | (word << 16) | (word << 24);
}

Buffer makeWords(std::uint32_t value)
{
    auto words = Vector<std::uint32_t> {};
    words.assign(count, value);

    return Buffer {Device::shared(), words.data(), byteCount, BufferUsage::Storage};
}

Buffer makeFloats(float value)
{
    auto values = Vector<float> {};
    values.assign(count, value);

    return Buffer {Device::shared(), values.data(), byteCount, BufferUsage::Storage};
}

Vector<std::uint32_t> readWords(const Buffer& buffer)
{
    auto words = Vector<std::uint32_t> {};
    words.resize(count);
    buffer.read(words.data(), byteCount);
    return words;
}

Vector<float> readFloats(const Buffer& buffer)
{
    auto values = Vector<float> {};
    values.resize(count);
    buffer.read(values.data(), byteCount);
    return values;
}
} // namespace

// A buffer that never held anything, filled through the command buffer: every
// byte of it is the value, which is what tells the fill apart from the zeros an
// allocation may happen to come back with.
auto tFillCoversTheBuffer = test("GPU/bufferFillCoversTheBuffer") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto buffer = device.makeBuffer(byteCount);

    auto commands = device.makeCommandBuffer();
    commands.fill(buffer, pattern);
    commands.commit();

    for (auto word: readWords(buffer))
        check(word == repeated(pattern));
};

// A range fill touches the range and nothing else.
auto tRangeFillStaysInItsRange = test("GPU/bufferRangeFillStaysInRange") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto first = 16;
    constexpr auto filled = 8;
    constexpr auto wordBytes = (int) sizeof(std::uint32_t);

    auto buffer = makeWords(sentinel);

    auto commands = device.makeCommandBuffer();
    commands.fill(BufferRange {&buffer, first * wordBytes, filled * wordBytes},
                  pattern);
    commands.commit();

    auto words = readWords(buffer);

    for (auto i = 0; i < count; ++i)
    {
        const auto inRange = i >= first && i < first + filled;
        check(words[i] == (inRange ? repeated(pattern) : sentinel));
    }
};

// The ordering, forwards: a pass recorded after the fill reads what the fill
// wrote rather than what the buffer held when the command buffer opened.
auto tPassAfterFillSeesIt = test("GPU/bufferFillIsSeenByALaterPass") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = makeFloats(7.0f);
    auto output = makeFloats(-1.0f);

    auto kernel = CopyKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    commands.fill(input);

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    for (auto value: readFloats(output))
        check(value == 0.0f);
};

// And backwards: a fill recorded after a pass overwrites what the pass wrote.
// The witness runs the same kernel on the same command buffer and is not
// filled, so a pass that never ran cannot pass this by leaving the fill alone
// with the buffer.
auto tFillAfterPassOverwritesIt = test("GPU/bufferFillOverwritesAnEarlierPass") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto target = device.makeBuffer(byteCount);
    auto witness = device.makeBuffer(byteCount);

    auto kernel = RampKernel {};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        kernel.output = target;
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    {
        kernel.output = witness;
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.fill(target, pattern);
    commands.commit();

    auto ramp = readFloats(witness);

    for (auto i = 0; i < count; ++i)
        check(ramp[i] == (float) i);

    for (auto word: readWords(target))
        check(word == repeated(pattern));
};
