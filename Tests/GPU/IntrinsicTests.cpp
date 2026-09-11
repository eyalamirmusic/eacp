#include "Common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

// The transcendentals a neural net's activation functions are written out of:
// the hyperbolics, the base-10 logarithm, and the error function.
//
// Four of them are native in both languages and cost nothing but a name. erf
// and erfc are native in neither - MSL rejects a call to erf as an undeclared
// identifier, and FXC's cs_5_0 has nothing either - so those two are a
// polynomial the emitter writes out, and the accuracy of one is a property of
// this project rather than of the driver.
//
// The polynomial runs here through Metal like any other kernel, but the HLSL
// copy of it cannot - there is no D3D on a Mac - so it is written out a second
// time in C++ and swept against std::erf. Both sides emit the same text, which
// is what makes that sweep say something about the Windows build.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

// Abramowitz & Stegun 7.1.26, in the form the HLSL helper emits it - the same
// operations in the same order, so this measures the shader's arithmetic and
// not a tidier rewriting of it.
float polynomialErfc(float x)
{
    auto a = std::fabs(x);
    auto t = 1.0f / (1.0f + 0.3275911f * a);
    auto e =
        t
        * (0.254829592f
           + t
                 * (-0.284496736f
                    + t * (1.421413741f + t * (-1.453152027f + t * 1.061405429f))))
        * std::exp(-a * a);

    return a == 0.0f ? 1.0f : (x < 0.0f ? 2.0f - e : e);
}

float polynomialErf(float x)
{
    auto a = std::fabs(x);
    auto t = 1.0f / (1.0f + 0.3275911f * a);
    auto e = 1.0f
             - t
                   * (0.254829592f
                      + t
                            * (-0.284496736f
                               + t
                                     * (1.421413741f
                                        + t * (-1.453152027f + t * 1.061405429f))))
                   * std::exp(-a * a);

    return a == 0.0f ? x : (x < 0.0f ? -e : e);
}

std::uint32_t bits(float value)
{
    auto pattern = std::uint32_t {};
    std::memcpy(&pattern, &value, sizeof(pattern));
    return pattern;
}

constexpr auto signBit = std::uint32_t {0x80000000};

// -6..6 at a step fine enough to land on the peak of the error curve, plus the
// magnitudes where the two tails saturate.
Vector<float> erfSweep()
{
    auto values = Vector<float> {};

    for (auto i = -6000; i <= 6000; ++i)
        values.add((float) i / 1000.0f);

    for (auto far: {8.0f, 12.0f, 30.0f, 120.0f})
    {
        values.add(far);
        values.add(-far);
    }

    return values;
}

struct HyperbolicKernel final : ComputeProgram
{
    HyperbolicKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = input[i];

        write(output, i * 3u, tanh(x));
        write(output, i * 3u + 1u, sinh(x));
        write(output, i * 3u + 2u, cosh(x));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// The repaired tanh, one value per thread. What the sweep it runs over is for
// is the two tails: a driver is free to evaluate tanh through exp, and the one
// eacp compiles its library with - fast math, no MTLCompileOptions - does, so
// the native builtin hands back a NaN out where this has to hand back a one.
struct SaturatingTanhKernel final : ComputeProgram
{
    SaturatingTanhKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, saturatingTanh(input[i]));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct Log10Kernel final : ComputeProgram
{
    Log10Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, log10(input[i]));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct ErrorFunctionKernel final : ComputeProgram
{
    ErrorFunctionKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = input[i];

        write(output, i * 2u, erf(x));
        write(output, i * 2u + 1u, erfc(x));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// Four components at a time, which is the whole question a componentwise
// intrinsic raises: a native builtin takes a vector on its own, but the erf
// helper is an ordinary function and gets one only from an overload per width.
struct VectorIntrinsicKernel final : ComputeProgram
{
    VectorIntrinsicKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = input.read4(i);

        write(hyperbolic, i, tanh(x));
        write(errorFunction, i, erf(x));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> hyperbolic;
    Uniform<OutputBuffer> errorFunction;

    EACP_SHADER(input, hyperbolic, errorFunction)
};

// Ordinary arithmetic, so nothing pulls a helper in.
struct PlainIntrinsicKernel final : ComputeProgram
{
    PlainIntrinsicKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * 2.0f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

Vector<float> runKernel(ComputeProgram& kernel,
                        Buffer& output,
                        int count,
                        int outputsPerThread)
{
    auto& device = Device::shared();
    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    auto result = Vector<float>(count * outputsPerThread);
    output.read(result.data(), result.size() * (int) sizeof(float));
    return result;
}

bool near(float gpu, double reference, double tolerance)
{
    return std::fabs((double) gpu - reference) <= tolerance;
}
} // namespace

// tanh, sinh, cosh and log10 exist in both languages under those exact names,
// so both backends must call them rather than spell them out.
auto tIntrinsicsAreNative = test("Intrinsics/bothBackendsCallTheNativeBuiltins") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();
    auto x = input[i];

    builder.write(output, i, tanh(x) + sinh(x) + cosh(x) + log10(x));

    const auto& graph = builder.graph();

    for (const auto& source: {emitMetal(graph), emitHlsl(graph)})
    {
        check(contains(source, "tanh("));
        check(contains(source, "sinh("));
        check(contains(source, "cosh("));
        check(contains(source, "log10("));
    }
};

// erf and erfc are the pair neither language has, so both backends carry the
// polynomial ahead of the body and call it by the same name.
auto tErrorFunctionGoesThroughAHelper =
    test("Intrinsics/errorFunctionIsEmittedAsAHelper") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();
    auto x = input[i];

    builder.write(output, i, erf(x) + erfc(x));

    const auto& graph = builder.graph();

    for (const auto& source: {emitMetal(graph), emitHlsl(graph)})
    {
        check(contains(source, "float eacpErf(float x)"));
        check(contains(source, "float eacpErfc(float x)"));
        check(contains(source, "0.3275911"));

        // Every width the EDSL can hand one, since the intrinsic is
        // componentwise and neither language resolves that for a user function
        // without an overload per width.
        for (const auto& name: {std::string("eacpErf"), std::string("eacpErfc")})
            for (const auto& width: {std::string("float2"),
                                     std::string("float3"),
                                     std::string("float4")})
                check(contains(source,
                               (width + " " + name + "(" + width + " x)").c_str()));

        // And the definitions arrive before the body that calls them.
        check(source.find("eacpErf(") < source.rfind("eacpErf("));
    }
};

// The saturating tanh is a helper on all three backends rather than a rename:
// what it adds is the two constant tails, which no dialect spells for us.
auto tSaturatingTanhGoesThroughAHelper =
    test("Intrinsics/saturatingTanhIsEmittedAsAHelper") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    builder.write(
        output, i, saturatingTanh(input[i]) + saturatingTanh(input.read4(i)).w());

    const auto& graph = builder.graph();

    for (const auto& source: {emitMetal(graph), emitHlsl(graph)})
    {
        check(contains(source, "float eacpSaturatingTanh(float x)"));
        check(contains(source, "x >= 10.0 ? 1.0 : (x <= -10.0 ? -1.0 : tanh(x))"));

        for (const auto& width:
             {std::string("float2"), std::string("float3"), std::string("float4")})
            check(contains(
                source, (width + " eacpSaturatingTanh(" + width + " x)").c_str()));

        check(source.find("eacpSaturatingTanh(")
              < source.rfind("eacpSaturatingTanh("));
    }

    auto glsl = emitGlsl(graph);

    check(contains(glsl, "float eacpSaturatingTanh(float x)"));
    check(contains(glsl, "vec4 eacpSaturatingTanh(vec4 x)"));
    check(contains(glsl, "x >= 10.0 ? 1.0 : (x <= -10.0 ? -1.0 : tanh(x))"));
    check(!contains(glsl, "float4"));

    expectGlslCompiles(graph);
};

// The helpers are emitted only into shaders that call them.
auto tHelpersAreNotAlwaysEmitted =
    test("Intrinsics/emitsTheErrorFunctionHelperOnlyWhenUsed") = []
{
    auto plain = PlainIntrinsicKernel {};
    const auto& source = plain.source().source;

    check(!contains(source, "eacpSaturatingTanh"));
    check(!contains(source, "eacpErf"));
    check(!contains(source, "eacpErfc"));
};

auto tHyperbolics = test("Intrinsics/computesTheHyperbolics") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = Vector<float> {};

    for (auto i = -60; i <= 60; ++i)
        values.add((float) i / 10.0f);

    auto count = values.size();

    auto input = device.makeBuffer(
        values.data(), count * (int) sizeof(float), BufferUsage::Storage);
    auto output = device.makeBuffer(count * 3 * (int) sizeof(float));

    auto kernel = HyperbolicKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare(device);

    auto result = runKernel(kernel, output, count, 3);

    for (auto i = 0; i < count; ++i)
    {
        auto x = (double) values[i];

        // sinh and cosh reach 200 at the end of the sweep, so what is held
        // fixed there is the relative error rather than the absolute one.
        auto tolerance = [](double reference)
        { return 1.0e-6 + 1.0e-6 * std::fabs(reference); };

        check(near(result[i * 3], std::tanh(x), tolerance(std::tanh(x))));
        check(near(result[i * 3 + 1], std::sinh(x), tolerance(std::sinh(x))));
        check(near(result[i * 3 + 2], std::cosh(x), tolerance(std::cosh(x))));
    }
};

// The saturating tanh, swept from the origin out past where a fast-math tanh
// stops being a number at all. std::tanh is the reference over the whole sweep,
// which is the claim: the two agree everywhere, and where the native builtin
// on this backend does not agree with either, this one still does.
auto tSaturatingTanh = test("Intrinsics/saturatingTanhAnswersTheTails") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = Vector<float> {};

    // The small end first, the origin and its negative zero included, then the
    // body of the curve, then the tails - 990 being what a tanh GELU's cubic
    // argument reaches for an activation of thirty, which is the value that
    // came back NaN and started this.
    for (auto small: {0.0f, -0.0f, 1.0e-20f, -1.0e-20f, 1.0e-7f, -1.0e-7f})
        values.add(small);

    for (auto i = -120; i <= 120; ++i)
        values.add((float) i / 20.0f);

    for (auto far: {9.0f, 9.5f, 10.0f, 10.5f, 30.0f, 120.0f, 990.0f, 1.0e20f})
    {
        values.add(far);
        values.add(-far);
    }

    auto count = values.size();

    auto input = device.makeBuffer(
        values.data(), count * (int) sizeof(float), BufferUsage::Storage);
    auto output = device.makeBuffer(count * (int) sizeof(float));

    auto kernel = SaturatingTanhKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare(device);

    auto result = runKernel(kernel, output, count, 1);

    for (auto i = 0; i < count; ++i)
    {
        auto x = values[i];

        check(std::isfinite(result[i]));
        check(near(result[i], std::tanh((double) x), 1.0e-6));

        // And exactly, not nearly, from the threshold outward - ten itself
        // included, since the helper compares inclusively so that the value it
        // documents as the threshold is one it answers. Between 9.011 and ten
        // the function has already rounded to one in float32 and the native
        // builtin is what returns it, which is a claim about the driver's tanh
        // rather than about this - so the tolerance above covers that stretch
        // and this covers the constant.
        if (std::fabs(x) >= 10.0f)
            check(result[i] == (x > 0.0f ? 1.0f : -1.0f));
    }
};

auto tLog10 = test("Intrinsics/computesLog10") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    // Twenty decades, which is where a mel front-end's clamped magnitudes sit
    // and where a base change by multiplication would show its error.
    auto values = Vector<float> {};

    for (auto decade = -10; decade <= 9; ++decade)
        for (auto step = 0; step < 10; ++step)
            values.add((float) (std::pow(10.0, decade) * (1.0 + step * 0.1)));

    auto count = values.size();

    auto input = device.makeBuffer(
        values.data(), count * (int) sizeof(float), BufferUsage::Storage);
    auto output = device.makeBuffer(count * (int) sizeof(float));

    auto kernel = Log10Kernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare(device);

    auto result = runKernel(kernel, output, count, 1);

    for (auto i = 0; i < count; ++i)
    {
        auto reference = std::log10((double) values[i]);

        // Metal's logarithm is specified to about 14 ulp, and a log10 of 1e-10
        // is ten whole units, so the error that buys is four orders larger than
        // the one near log10(1). The tolerance tracks the result's magnitude
        // for that reason rather than being loosened everywhere.
        check(near(result[i], reference, 1.0e-6 + 1.0e-5 * std::fabs(reference)));
    }
};

auto tErrorFunction = test("Intrinsics/computesErfAndErfc") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = Vector<float> {};

    for (auto i = -60; i <= 60; ++i)
        values.add((float) i / 10.0f);

    values.add(0.0f);
    values.add(9.0f);
    values.add(-9.0f);

    auto count = values.size();

    auto input = device.makeBuffer(
        values.data(), count * (int) sizeof(float), BufferUsage::Storage);
    auto output = device.makeBuffer(count * 2 * (int) sizeof(float));

    auto kernel = ErrorFunctionKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare(device);

    auto result = runKernel(kernel, output, count, 2);

    // What the GPU runs is an approximation, so this is the approximation's own
    // float32 budget (under 6e-7, pinned below) with room for the driver's exp
    // on top - not the tolerance a native builtin would deserve. Metal comes in
    // at 1.7e-7, better than the same expression does in C++, since it contracts
    // the Horner chain into fused multiply-adds.
    for (auto i = 0; i < count; ++i)
    {
        auto x = (double) values[i];

        check(near(result[i * 2], std::erf(x), 2.0e-6));
        check(near(result[i * 2 + 1], std::erfc(x), 2.0e-6));
    }
};

// The same two intrinsics over a Float4 record, which is the width the HLSL
// helper needs an overload for and the native MSL call gets for free.
auto tVectorIntrinsics = test("Intrinsics/appliesComponentwiseToAVector") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = Vector<float> {};

    for (auto i = -24; i <= 24; ++i)
        values.add((float) i / 4.0f);

    auto records = values.size() / 4;
    auto count = records * 4;

    auto input = device.makeBuffer(
        values.data(), count * (int) sizeof(float), BufferUsage::Storage);
    auto hyperbolic = device.makeBuffer(count * (int) sizeof(float));
    auto errorFunction = device.makeBuffer(count * (int) sizeof(float));

    auto kernel = VectorIntrinsicKernel {};
    kernel.input = input;
    kernel.hyperbolic = hyperbolic;
    kernel.errorFunction = errorFunction;
    kernel.prepare(device);

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, records);
    }

    commands.commit();

    auto tanhResult = Vector<float>(count);
    hyperbolic.read(tanhResult.data(), count * (int) sizeof(float));

    auto erfResult = Vector<float>(count);
    errorFunction.read(erfResult.data(), count * (int) sizeof(float));

    for (auto i = 0; i < count; ++i)
    {
        auto x = (double) values[i];

        check(near(tanhResult[i], std::tanh(x), 1.0e-6));
        check(near(erfResult[i], std::erf(x), 2.0e-6));
    }
};

// A GPU sweep says what one driver's arithmetic does with the polynomial; this
// says what the polynomial is. The same expression in C++, swept against the
// standard library: 3.7e-7 worst absolute error for erf and 3.8e-7 for erfc
// here, and 6.0e-7 for the same source built without fused multiply-add - the
// approximation's own 1.5e-7 plus what evaluating it in float32 costs. The
// bound below is the one a backend has to stay inside however it contracts.
//
// It is also the only check the HLSL definition gets, since no D3D device runs
// here to compile it.
auto tPolynomialAccuracy =
    test("Intrinsics/errorFunctionPolynomialMatchesTheReference") = []
{
    auto worstErf = 0.0;
    auto worstErfc = 0.0;

    for (auto x: erfSweep())
    {
        worstErf =
            std::max(worstErf, std::fabs(polynomialErf(x) - std::erf((double) x)));
        worstErfc = std::max(worstErfc,
                             std::fabs(polynomialErfc(x) - std::erfc((double) x)));
    }

    check(worstErf < 1.0e-6);
    check(worstErfc < 1.0e-6);

    check(bits(polynomialErf(0.0f)) == bits(0.0f));
    check(bits(polynomialErf(-0.0f)) == bits(-0.0f));

    check(polynomialErfc(0.0f) == 1.0f);
    check(polynomialErfc(-0.0f) == 1.0f);
    check(polynomialErf(0.0f) + polynomialErfc(0.0f) == 1.0f);

    for (auto x: erfSweep())
        check(bits(polynomialErf(-x)) == (bits(polynomialErf(x)) ^ signBit));

    // The two tails, where erfc has to saturate rather than drift: a large
    // negative argument is 2 exactly, a large positive one underflows to zero,
    // and both are what a shader summing them relies on.
    check(polynomialErfc(-30.0f) == 2.0f);
    check(polynomialErfc(30.0f) == 0.0f);
    check(polynomialErf(-30.0f) == -1.0f);
    check(polynomialErf(30.0f) == 1.0f);
};

// erf is odd and exactly zero at the origin. The polynomial behind it is
// neither: 1 - poly(0) * exp(0) leaves about 1e-9 there, and both signs of zero
// took the branch for a positive argument, so erf(-x) was not the negation of
// erf(x) across it.
auto tErrorFunctionIsOddAboutZero =
    test("Intrinsics/errorFunctionIsOddAboutZero") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = Vector<float> {};

    for (auto x: {0.0f, 1.0e-20f, 1.0f, 3.0f})
    {
        values.add(x);
        values.add(-x);
    }

    auto count = values.size();

    auto input = device.makeBuffer(
        values.data(), count * (int) sizeof(float), BufferUsage::Storage);
    auto output = device.makeBuffer(count * 2 * (int) sizeof(float));

    auto kernel = ErrorFunctionKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare(device);

    auto result = runKernel(kernel, output, count, 2);

    check(bits(result[0]) == bits(0.0f));
    check(bits(result[2]) == bits(-0.0f));

    for (auto i = 0; i < count; i += 2)
        check(bits(result[(i + 1) * 2]) == (bits(result[i * 2]) ^ signBit));
};

// The complement has the same defect at the origin, and it is the one that
// makes erf(x) + erfc(x) exactly 1 there rather than one ulp under it.
auto tComplementIsExactlyOneAtZero =
    test("Intrinsics/complementIsExactlyOneAtZero") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = Vector<float> {};

    values.add(0.0f);
    values.add(-0.0f);

    auto count = values.size();

    auto input = device.makeBuffer(
        values.data(), count * (int) sizeof(float), BufferUsage::Storage);
    auto output = device.makeBuffer(count * 2 * (int) sizeof(float));

    auto kernel = ErrorFunctionKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare(device);

    auto result = runKernel(kernel, output, count, 2);

    for (auto i = 0; i < count; ++i)
    {
        check(result[i * 2 + 1] == 1.0f);
        check(result[i * 2] + result[i * 2 + 1] == 1.0f);
    }
};

// What carries both signs of zero through is returning the argument itself, and
// what pins the complement is the one it complements, so both backends have to
// say so.
auto tErrorFunctionHelpersPinTheOrigin =
    test("Intrinsics/errorFunctionHelpersPinTheOrigin") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();
    auto x = input[i];

    builder.write(output, i, erf(x) + erfc(x));

    for (const auto& source: {emitMetal(builder.graph()),
                              emitHlsl(builder.graph()),
                              emitGlsl(builder.graph())})
    {
        check(contains(source, "return a == 0.0 ? x : (x < 0.0 ? -e : e);"));
        check(contains(source, "return a == 0.0 ? 1.0 : (x < 0.0 ? 2.0 - e : e);"));
    }

    expectGlslCompiles(builder.graph());
};
