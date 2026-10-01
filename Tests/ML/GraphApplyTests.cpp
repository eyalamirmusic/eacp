#include "GraphCommon.h"

#include <functional>

// The elementwise apply: an expression written with the shader EDSL's value
// operators, recorded into the graph's ShaderBuilder and lowered node by node
// into MIL ops.

using namespace nano;
using namespace eacp;
using namespace eacp::ML;
using namespace MLGraphTesting;

using GPU::Float;

namespace
{
bool contains(const std::string& text, std::string_view fragment)
{
    return text.find(fragment) != std::string::npos;
}

std::string appliedText(const std::function<Float(const Float&)>& body)
{
    auto graph = Graph {};
    auto x = graph.input("x", {4}, DType::float16);
    graph.output(graph.apply(x, body), "y");
    buildChecked(graph);
    return graph.toText();
}
} // namespace

auto tApplyChain = test("MLGraph/Apply/inputsConstantsAndBinaries") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {4}, DType::float16);
    auto body = [](const Float& value) { return value * 2.f + 1.f; };
    graph.output(graph.apply(x, body), "y");
    buildChecked(graph);

    checkText(graph.toText(),
              "program(1)\n"
              "func main<CoreML7>(tensor<fp16, [4]> x) {\n"
              "    tensor<fp16, []> mul_1_y = const()[val = 2];\n"
              "    tensor<fp16, [4]> mul_1 = mul(x = x, y = mul_1_y);\n"
              "    tensor<fp16, []> y_y = const()[val = 1];\n"
              "    tensor<fp16, [4]> y = add(x = mul_1, y = y_y);\n"
              "} -> (y);\n");
};

auto tApplyBinaries = test("MLGraph/Apply/everyArithmeticOperator") = []
{
    auto graph = Graph {};
    auto a = graph.input("a", {2, 3}, DType::float32);
    auto b = graph.input("b", {2, 3}, DType::float32);
    auto body = [](const Float& x, const Float& y) { return (x - y) / (x * y + x); };
    graph.output(graph.apply(a, b, body), "y");
    buildChecked(graph);

    auto text = graph.toText();
    check(contains(text, "sub_2 = sub(x = a, y = b);"));
    check(contains(text, "mul_3 = mul(x = a, y = b);"));
    check(contains(text, "add_4 = add(x = mul_3, y = a);"));
    check(
        contains(text, "tensor<fp32, [2, 3]> y = real_div(x = sub_2, y = add_4);"));
};

auto tApplyNegate = test("MLGraph/Apply/unaryMinusIsAMultiply") = []
{
    auto negate = [](const Float& x) { return -x; };
    auto text = appliedText(negate);
    check(contains(text, "tensor<fp16, []> y_y = const()[val = -1];"));
    check(contains(text, "y = mul(x = x, y = y_y);"));
};

auto tApplyCalls = test("MLGraph/Apply/everyLoweredCall") = []
{
    auto unary = [](const Float& x)
    {
        return exp(x) + log(x) + tanh(x) + sqrt(x) + rsqrt(x) + abs(x) + floor(x)
               + erf(x);
    };

    auto text = appliedText(unary);

    for (auto op: {"exp", "tanh", "sqrt", "abs", "floor", "erf"})
        check(contains(text, std::string {" = "} + op + "(x = x);"));

    check(contains(text, "log_2 = log(x = x, epsilon = log_2_epsilon);"));
    check(contains(text, "tensor<fp16, []> log_2_epsilon = const()[val = 0];"));
    check(contains(text, "rsqrt(x = x, epsilon = rsqrt_"));

    auto binary = [](const Float& x)
    { return max(x, 0.5f) + min(x, 2.f) + pow(x, 3.f); };

    auto calls = appliedText(binary);
    check(contains(calls, "= maximum(x = x, y = maximum_1_y);"));
    check(contains(calls, "= minimum(x = x, y = minimum_2_y);"));
    check(contains(calls, "= pow(x = x, y = pow_4_y);"));
    check(contains(calls, "tensor<fp16, []> pow_4_y = const()[val = 3];"));
};

auto tApplyClamp = test("MLGraph/Apply/clampIsClipOrAPairOfBounds") = []
{
    auto clampToUnit = [](const Float& x) { return clamp(x, -1.f, 1.f); };
    auto constant = appliedText(clampToUnit);
    check(contains(constant, "y = clip(x = x, alpha = y_alpha, beta = y_beta);"));

    auto graph = Graph {};
    auto x = graph.input("x", {4}, DType::float16);
    auto low = graph.input("low", {4}, DType::float16);
    auto body = [](const Float& value, const Float& bound)
    { return clamp(value, bound, 1.f); };
    graph.output(graph.apply(x, low, body), "y");
    buildChecked(graph);

    auto text = graph.toText();
    check(contains(text, "maximum_2 = maximum(x = x, y = low);"));
    check(contains(text, "y = minimum(x = maximum_2, y = y_y);"));
};

auto tApplyCompareSelect = test("MLGraph/Apply/comparisonsAndSelect") = []
{
    auto body = [](const Float& x) { return select(x > 0.f, x, x * 0.1f); };
    auto text = appliedText(body);

    check(contains(
        text, "tensor<bool, [4]> greater_1 = greater(x = x, y = greater_1_y);"));
    check(contains(text, "y = select(cond = greater_1, a = x, b = mul_2);"));

    auto every = [](const Float& x)
    {
        auto inRange = (x >= -1.f && x <= 1.f) || (x < -5.f);
        auto special = x == 2.f || x != 3.f;
        return select(inRange && !special, x, 0.f);
    };

    auto logic = appliedText(every);

    for (auto op: {"greater_equal",
                   "less_equal",
                   "less(",
                   "equal(",
                   "not_equal",
                   "logical_and",
                   "logical_or",
                   "logical_not"})
        check(contains(logic, op));
};

auto tApplyBroadcast = test("MLGraph/Apply/scalarTensorsBroadcast") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {3, 4}, DType::float16);
    auto scale = graph.input("scale", {1}, DType::float16);
    auto half = graph.scalar(0.5f);
    auto body = [](const Float& value, const Float& factor, const Float& offset)
    { return value * factor + offset; };
    auto y = graph.apply(x, scale, half, body);

    check(graph.shape(y) == Shape {3, 4});
    graph.output(y, "y");
    buildChecked(graph);

    auto text = graph.toText();
    check(contains(text, "tensor<fp16, [3, 4]> mul_3 = mul(x = x, y = scale);"));
    check(contains(text, "tensor<fp16, []> y_y = const()[val = 0.5];"));
    check(!contains(text, "scalar"));
};

auto tApplyVector = test("MLGraph/Apply/anyNumberOfOperands") = []
{
    auto graph = Graph {};
    auto parts = Vector<Tensor> {};

    for (auto index = 0; index < 4; ++index)
        parts.add(graph.input("x" + std::to_string(index), {2}, DType::float16));

    auto total = [](const Vector<Float>& values)
    { return values[0] + values[1] + values[2] + values[3]; };

    auto y = graph.apply(parts, total);
    graph.output(y, "y");
    buildChecked(graph);

    auto same = [](const Float& value) { return value; };
    auto identity = graph.apply(parts[0], same);
    check(identity == parts[0]);
};

auto tApplyRefusedCall = test("MLGraph/Apply/refusesACallWithNoLowering") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {4}, DType::float16);
    auto sine = [](const Float& value) { return sin(value); };
    auto y = graph.apply(x, sine);

    check(!y.isValid());
    check(failedWith(graph, "apply: Call 'sin' has no MIL lowering"));
};

auto tApplyRefusedKind = test("MLGraph/Apply/refusesVectorsAndSwizzles") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {4}, DType::float16);
    auto body = [](const Float& value) { return unpackHalf2(asUInt(value)).x(); };

    check(!graph.apply(x, body).isValid());
    check(failedWith(graph, "Swizzle"));
};

auto tApplyForeignValue = test("MLGraph/Apply/refusesAValueFromAnotherApply") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {4}, DType::float16);
    auto kept = Float {};
    auto keep = [&kept](const Float& value)
    {
        kept = value;
        return value * 2.f;
    };
    graph.apply(x, keep);

    auto reuse = [&kept](const Float& value) { return value + kept; };
    check(!graph.apply(x, reuse).isValid());
    check(failedWith(graph, "outside this apply"));
};

auto tApplyRefusedOperands = test("MLGraph/Apply/refusesOperandsThatDoNotFit") = []
{
    auto body = [](const Float& a, const Float& b) { return a + b; };

    auto types = Graph {};
    auto half = types.input("a", {4}, DType::float16);
    auto single = types.input("b", {4}, DType::float32);
    check(!types.apply(half, single, body).isValid());
    check(failedWith(types, "one floating-point type"));

    auto shapes = Graph {};
    auto three = shapes.input("a", {3}, DType::float16);
    auto four = shapes.input("b", {4}, DType::float16);
    check(!shapes.apply(three, four, body).isValid());
    check(failedWith(shapes, "does not broadcast"));

    auto ints = Graph {};
    auto indices = ints.input("a", {4}, DType::int32);
    auto same = [](const Float& value) { return value; };
    check(!ints.apply(indices, same).isValid());

    auto scalars = Graph {};
    auto two = scalars.scalar(2.f);
    auto root = [](const Float& value) { return sqrt(value); };
    check(!scalars.apply(two, root).isValid());
    check(failedWith(scalars, "does not depend on any tensor"));
};
