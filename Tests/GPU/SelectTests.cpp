#include "Common.h"

#include <string>

// select() outside the float vocabulary: an index chosen by a comparison, and a
// mask chosen from two others. Both backends print the conditional operator
// rather than a call, so what each family adds is the type its result is named
// with.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

// The larger of two unsigned values, and a literal floor under it, both picked
// by a comparison. Integers, so the answer is exact and there is nothing to
// tolerance.
struct UIntSelectKernel final : ComputeProgram
{
    UIntSelectKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto a = toUInt(left[i]);
        auto b = toUInt(right[i]);

        write(output, i, toFloat(select(a > b, a, b)));
        write(output, i + gridCount(), toFloat(select(a > b, 100u, b)));
    }

    Uniform<InputBuffer> left;
    Uniform<InputBuffer> right;
    Uniform<OutputBuffer> output;

    EACP_SHADER(left, right, output)
};
} // namespace

auto tUIntSelectEmitsAUIntConditional = test("Select/aUIntSelectIsNamedAsAUInt") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto i = builder.threadId();
    auto picked = select(i > 4u, i, 7u);

    builder.write(output, picked, toFloat(picked));

    for (const auto& source: {emitMetal(builder.graph()),
                              emitHlsl(builder.graph()),
                              emitGlsl(builder.graph())})
    {
        check(contains(source, "uint t0 = ((gid > 4u) ? gid : 7u);"));
        check(contains(source, "buffer0[t0] = float(t0);"));
    }

    expectGlslCompiles(builder.graph());
};

auto tIntVectorSelectEmitsAnIntVectorConditional =
    test("Select/anIntVectorSelectIsNamedAsAnIntVector") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    auto a = int2(builder.integer(1), builder.integer(2));
    auto b = int2(builder.integer(3), builder.integer(4));
    auto picked = select(i > 0u, a, b);

    builder.write(output, i, toFloat(picked + picked));

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "int2 t1 = ((gid > 0u) ? int2(1, 2) : int2(3, 4));"));
        check(contains(source, "float2 t2 = float2((t1 + t1));"));
    }

    // The same conditional, in the vector spellings GLSL has of both types.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "ivec2 t1 = ((gid > 0u) ? ivec2(1, 2) : ivec2(3, 4));"));
    check(contains(glsl, "vec2 t2 = vec2((t1 + t1));"));

    expectGlslCompiles(builder.graph());
};

auto tBoolSelectIsNamedAsABool = test("Select/aBoolSelectIsNamedAsABool") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    auto picked = select(i > 0u, input[i] > 1.0f, false);

    builder.write(output, i, toFloat(picked) + toFloat(picked));

    for (const auto& source: {emitMetal(builder.graph()),
                              emitHlsl(builder.graph()),
                              emitGlsl(builder.graph())})
    {
        check(contains(source,
                       "bool t0 = ((gid > 0u) ? (buffer0[gid] > 1.0) : false);"));
        check(contains(source, "buffer1[gid] = (float(t0) + float(t0));"));
    }

    expectGlslCompiles(builder.graph());
};

auto tUIntSelectRunsExactly = test("Select/picksTheUnsignedValueAsked") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto left = Vector<float> {};
    auto right = Vector<float> {};

    for (auto i = 0; i < 64; ++i)
    {
        left.add((float) i);
        right.add((float) (63 - i));
    }

    auto count = left.size();
    auto bytes = count * (int) sizeof(float);

    auto leftBuffer = device.makeBuffer(left.data(), bytes, BufferUsage::Storage);
    auto rightBuffer = device.makeBuffer(right.data(), bytes, BufferUsage::Storage);
    auto output = device.makeBuffer(bytes * 2);

    auto kernel = UIntSelectKernel {};
    kernel.left = leftBuffer;
    kernel.right = rightBuffer;
    kernel.output = output;
    kernel.prepare(device);

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    auto result = Vector<float>(count * 2);
    output.read(result.data(), bytes * 2);

    for (auto i = 0; i < count; ++i)
    {
        auto a = (unsigned) i;
        auto b = (unsigned) (63 - i);

        check(result[i] == (float) (a > b ? a : b));
        check(result[count + i] == (float) (a > b ? 100u : b));
    }
};
