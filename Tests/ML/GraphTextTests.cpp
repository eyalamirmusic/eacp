#include "GraphCommon.h"

// toText() per op: the program as readable MIL, the part emitMetal plays for
// the shader tests. Every program is also built and, where Core ML is here,
// compiled.

using namespace nano;
using namespace eacp;
using namespace eacp::ML;
using namespace MLGraphTesting;

auto tTextLinear = test("MLGraph/Text/linear") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {2, 4}, DType::float16);
    auto weight = zeroConstant(graph, "w", {3, 4});
    auto bias = zeroConstant(graph, "b", {3});
    graph.output(graph.linear(x, weight, bias), "y");
    buildChecked(graph);

    checkText(graph.toText(),
              "program(1)\n"
              "func main<CoreML7>(tensor<fp16, [2, 4]> x) {\n"
              "    tensor<fp16, [3, 4]> w = const()[val = blob(64)];\n"
              "    tensor<fp16, [3]> b = const()[val = blob(192)];\n"
              "    tensor<fp16, [2, 3]> y = linear(x = x, weight = w, bias = b);\n"
              "} -> (y);\n");
};

auto tTextMatmul = test("MLGraph/Text/matmul") = []
{
    auto graph = Graph {};
    auto a = graph.input("a", {2, 3}, DType::float16);
    auto b = graph.input("b", {4, 3}, DType::float16);
    graph.output(graph.matmul(a, b, false, true), "y");
    buildChecked(graph);

    checkText(
        graph.toText(),
        "program(1)\n"
        "func main<CoreML7>(tensor<fp16, [2, 3]> a, tensor<fp16, [4, 3]> b) {\n"
        "    tensor<bool, []> y_transpose_x = const()[val = false];\n"
        "    tensor<bool, []> y_transpose_y = const()[val = true];\n"
        "    tensor<fp16, [2, 4]> y = matmul(x = a, y = b, transpose_x = "
        "y_transpose_x, transpose_y = y_transpose_y);\n"
        "} -> (y);\n");
};

auto tTextTranspose = test("MLGraph/Text/transposeAndReshape") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {2, 3, 4}, DType::float16);
    auto turned = graph.transpose(x, {2, 0, 1});
    graph.output(graph.reshape(turned, {Shape::unknown, 6}), "y");
    buildChecked(graph);

    checkText(
        graph.toText(),
        "program(1)\n"
        "func main<CoreML7>(tensor<fp16, [2, 3, 4]> x) {\n"
        "    tensor<int32, [3]> transpose_1_perm = const()[val = [2, 0, 1]];\n"
        "    tensor<fp16, [4, 2, 3]> transpose_1 = transpose(x = x, perm = "
        "transpose_1_perm);\n"
        "    tensor<int32, [2]> y_shape = const()[val = [-1, 6]];\n"
        "    tensor<fp16, [4, 6]> y = reshape(x = transpose_1, shape = y_shape);\n"
        "} -> (y);\n");
};

auto tTextSoftmax = test("MLGraph/Text/softmax") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {2, 3}, DType::float16);
    graph.output(graph.softmax(x, -1), "y");
    buildChecked(graph);

    checkText(graph.toText(),
              "program(1)\n"
              "func main<CoreML7>(tensor<fp16, [2, 3]> x) {\n"
              "    tensor<int32, []> y_axis = const()[val = -1];\n"
              "    tensor<fp16, [2, 3]> y = softmax(x = x, axis = y_axis);\n"
              "} -> (y);\n");
};

auto tTextReductions = test("MLGraph/Text/sumAndMax") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {3, 4}, DType::float32);
    graph.output(graph.sum(x, 1), "total");
    graph.output(graph.max(x, 0, true), "largest");
    buildChecked(graph);

    checkText(graph.toText(),
              "program(1)\n"
              "func main<CoreML7>(tensor<fp32, [3, 4]> x) {\n"
              "    tensor<int32, [1]> total_axes = const()[val = [1]];\n"
              "    tensor<bool, []> total_keep_dims = const()[val = false];\n"
              "    tensor<fp32, [3]> total = reduce_sum(x = x, axes = total_axes, "
              "keep_dims = total_keep_dims);\n"
              "    tensor<int32, [1]> largest_axes = const()[val = [0]];\n"
              "    tensor<bool, []> largest_keep_dims = const()[val = true];\n"
              "    tensor<fp32, [1, 4]> largest = reduce_max(x = x, axes = "
              "largest_axes, keep_dims = largest_keep_dims);\n"
              "} -> (total, largest);\n");
};

auto tTextLayerNorm = test("MLGraph/Text/layerNorm") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {5, 8}, DType::float32);
    auto ones = Vector<float> {};
    ones.resize(8, 1.f);
    auto zeros = Vector<float> {};
    zeros.resize(8, 0.f);
    auto gamma = graph.constant("gamma", {8}, DType::float32, asBytes(ones));
    auto beta = graph.constant("beta", {8}, DType::float32, asBytes(zeros));
    graph.output(graph.layerNorm(x, {-1}, gamma, beta, 1e-5f), "y");
    buildChecked(graph);

    checkText(
        graph.toText(),
        "program(1)\n"
        "func main<CoreML7>(tensor<fp32, [5, 8]> x) {\n"
        "    tensor<fp32, [8]> gamma = const()[val = blob(64)];\n"
        "    tensor<fp32, [8]> beta = const()[val = blob(192)];\n"
        "    tensor<int32, [1]> y_axes = const()[val = [-1]];\n"
        "    tensor<fp32, []> y_epsilon = const()[val = 1e-05];\n"
        "    tensor<fp32, [5, 8]> y = layer_norm(x = x, axes = y_axes, gamma = "
        "gamma, beta = beta, epsilon = y_epsilon);\n"
        "} -> (y);\n");
};

auto tTextConv = test("MLGraph/Text/conv") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {1, 4, 10}, DType::float16);
    auto weight = zeroConstant(graph, "w", {6, 4, 3});
    auto bias = zeroConstant(graph, "b", {6});
    graph.output(graph.conv(x, weight, bias, 2, 1), "y");
    buildChecked(graph);

    checkText(graph.toText(),
              "program(1)\n"
              "func main<CoreML7>(tensor<fp16, [1, 4, 10]> x) {\n"
              "    tensor<fp16, [6, 4, 3]> w = const()[val = blob(64)];\n"
              "    tensor<fp16, [6]> b = const()[val = blob(320)];\n"
              "    tensor<int32, [1]> y_strides = const()[val = [2]];\n"
              "    tensor<string, []> y_pad_type = const()[val = \"custom\"];\n"
              "    tensor<int32, [2]> y_pad = const()[val = [1, 1]];\n"
              "    tensor<int32, [1]> y_dilations = const()[val = [1]];\n"
              "    tensor<int32, []> y_groups = const()[val = 1];\n"
              "    tensor<fp16, [1, 6, 5]> y = conv(x = x, weight = w, bias = b, "
              "strides = y_strides, pad_type = y_pad_type, pad = y_pad, dilations = "
              "y_dilations, groups = y_groups);\n"
              "} -> (y);\n");
};

auto tTextGather = test("MLGraph/Text/gather") = []
{
    auto graph = Graph {};
    auto table = zeroConstant(graph, "table", {10, 4});
    auto tokens = graph.input("tokens", {3}, DType::int32);
    graph.output(graph.gather(table, tokens, 0), "y");
    buildChecked(graph);

    checkText(
        graph.toText(),
        "program(1)\n"
        "func main<CoreML7>(tensor<int32, [3]> tokens) {\n"
        "    tensor<fp16, [10, 4]> table = const()[val = blob(64)];\n"
        "    tensor<int32, []> y_axis = const()[val = 0];\n"
        "    tensor<int32, []> y_batch_dims = const()[val = 0];\n"
        "    tensor<bool, []> y_validate_indices = const()[val = false];\n"
        "    tensor<fp16, [3, 4]> y = gather(x = table, indices = tokens, axis = "
        "y_axis, batch_dims = y_batch_dims, validate_indices = "
        "y_validate_indices);\n"
        "} -> (y);\n");
};

auto tTextConcat = test("MLGraph/Text/concat") = []
{
    auto graph = Graph {};
    auto a = graph.input("a", {2, 3}, DType::float16);
    auto b = graph.input("b", {2, 5}, DType::float16);
    graph.output(graph.concat({a, b}, 1), "y");
    buildChecked(graph);

    checkText(
        graph.toText(),
        "program(1)\n"
        "func main<CoreML7>(tensor<fp16, [2, 3]> a, tensor<fp16, [2, 5]> b) {\n"
        "    tensor<int32, []> y_axis = const()[val = 1];\n"
        "    tensor<bool, []> y_interleave = const()[val = false];\n"
        "    tensor<fp16, [2, 8]> y = concat(values = (a, b), axis = y_axis, "
        "interleave = y_interleave);\n"
        "} -> (y);\n");
};

auto tTextSlice = test("MLGraph/Text/slice") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {8, 4}, {{6, 4}}, DType::float16);
    graph.output(graph.slice(x, {0, 1}, {Shape::unknown, 3}), "y");
    buildChecked(graph);

    checkText(
        graph.toText(),
        "program(1)\n"
        "func main<CoreML7>(tensor<fp16, [?, 4]> x) {\n"
        "    tensor<int32, [2]> y_begin = const()[val = [0, 1]];\n"
        "    tensor<int32, [2]> y_end = const()[val = [0, 3]];\n"
        "    tensor<bool, [2]> y_end_mask = const()[val = [true, false]];\n"
        "    tensor<fp16, [?, 2]> y = slice_by_index(x = x, begin = y_begin, end "
        "= y_end, end_mask = y_end_mask);\n"
        "} -> (y);\n");
};

auto tTextSliceLike = test("MLGraph/Text/sliceLikeReadsTheReferencesShape") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {6, 4}, {{2, 4}, {4, 4}}, DType::float16);
    auto table = zeroConstant(graph, "table", {6, 4});
    auto add = [](const GPU::Float& a, const GPU::Float& b) { return a + b; };
    graph.output(graph.apply(x, graph.sliceLike(table, x), add), "y");
    buildChecked(graph);

    checkText(graph.toText(),
              "program(1)\n"
              "func main<CoreML7>(tensor<fp16, [?, 4]> x) {\n"
              "    tensor<fp16, [6, 4]> table = const()[val = blob(64)];\n"
              "    tensor<int32, [2]> shape_2 = shape(x = x);\n"
              "    tensor<int32, [2]> slice_by_index_3_begin = const()[val = [0, "
              "0]];\n"
              "    tensor<fp16, [?, 4]> slice_by_index_3 = slice_by_index(x = "
              "table, begin = slice_by_index_3_begin, end = shape_2);\n"
              "    tensor<fp16, [?, 4]> y = add(x = x, y = slice_by_index_3);\n"
              "} -> (y);\n");
};

auto tTextSliceLikeFixed = test("MLGraph/Text/sliceLikeAFixedReferenceIsASlice") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {2, 4}, DType::float16);
    auto table = zeroConstant(graph, "table", {6, 4});
    graph.output(graph.sliceLike(table, x), "y");
    buildChecked(graph);

    checkText(
        graph.toText(),
        "program(1)\n"
        "func main<CoreML7>(tensor<fp16, [2, 4]> x) {\n"
        "    tensor<fp16, [6, 4]> table = const()[val = blob(64)];\n"
        "    tensor<int32, [2]> y_begin = const()[val = [0, 0]];\n"
        "    tensor<int32, [2]> y_end = const()[val = [2, 4]];\n"
        "    tensor<bool, [2]> y_end_mask = const()[val = [false, false]];\n"
        "    tensor<fp16, [2, 4]> y = slice_by_index(x = table, begin = y_begin, "
        "end = y_end, end_mask = y_end_mask);\n"
        "} -> (y);\n");
};

auto tTextLinearWithoutBias = test("MLGraph/Text/linearWithoutABias") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {2, 4}, DType::float16);
    auto weight = zeroConstant(graph, "w", {3, 4});
    graph.output(graph.linear(x, weight), "y");
    buildChecked(graph);

    checkText(graph.toText(),
              "program(1)\n"
              "func main<CoreML7>(tensor<fp16, [2, 4]> x) {\n"
              "    tensor<fp16, [3, 4]> w = const()[val = blob(64)];\n"
              "    tensor<fp16, [3]> const_2 = const()[val = blob(192)];\n"
              "    tensor<fp16, [2, 3]> y = linear(x = x, weight = w, bias = "
              "const_2);\n"
              "} -> (y);\n");
};

auto tTextAttention = test("MLGraph/Text/scaledDotProductAttention") = []
{
    auto graph = Graph {};
    auto q = graph.input("q", {1, 4, 8}, DType::float16);
    auto k = graph.input("k", {1, 4, 8}, DType::float16);
    auto v = graph.input("v", {1, 4, 8}, DType::float16);
    graph.output(graph.scaledDotProductAttention(q, k, v, false), "open");
    graph.output(graph.scaledDotProductAttention(q, k, v, true), "causal");
    buildChecked(graph);

    checkText(
        graph.toText(),
        "program(1)\n"
        "func main<CoreML8>(tensor<fp16, [1, 4, 8]> q, tensor<fp16, [1, 4, 8]> "
        "k, tensor<fp16, [1, 4, 8]> v) {\n"
        "    tensor<fp16, [1, 4, 8]> open = scaled_dot_product_attention(query = "
        "q, key = k, value = v);\n"
        "    tensor<fp16, [4, 4]> const_4 = const()[val = blob(64)];\n"
        "    tensor<fp16, []> greater_5_y = const()[val = 0.5];\n"
        "    tensor<bool, [4, 4]> greater_5 = greater(x = const_4, y = "
        "greater_5_y);\n"
        "    tensor<fp16, [1, 4, 8]> causal = scaled_dot_product_attention(query "
        "= q, key = k, value = v, attn_mask = greater_5);\n"
        "} -> (open, causal);\n");

    // The mask is a bool one, true where a query may attend: an additive
    // float mask is ignored by Core ML's fp16 attention from 32 positions up.
    // A bool tensor cannot live in the blob, so it is a comparison of a 0/1
    // constant that can.
    auto package = graph.build();
    auto allowedAt = [&package](int row, int column)
    {
        auto index = 128 + (row * 4 + column) * 2;
        return halfToFloat(static_cast<std::uint16_t>(
            package.weights[index] | (package.weights[index + 1] << 8)));
    };

    check(allowedAt(0, 0) == 1.f);
    check(allowedAt(3, 3) == 1.f);
    check(allowedAt(2, 1) == 1.f);
    check(allowedAt(1, 2) == 0.f);
    check(allowedAt(0, 3) == 0.f);
};

auto tTextGeluCast = test("MLGraph/Text/geluAndCast") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {3, 4}, DType::float16);
    graph.output(graph.cast(graph.gelu(x), DType::float32), "y");
    buildChecked(graph);

    checkText(graph.toText(),
              "program(1)\n"
              "func main<CoreML7>(tensor<fp16, [3, 4]> x) {\n"
              "    tensor<string, []> gelu_1_mode = const()[val = \"EXACT\"];\n"
              "    tensor<fp16, [3, 4]> gelu_1 = gelu(x = x, mode = gelu_1_mode);\n"
              "    tensor<string, []> y_dtype = const()[val = \"fp32\"];\n"
              "    tensor<fp32, [3, 4]> y = cast(x = gelu_1, dtype = y_dtype);\n"
              "} -> (y);\n");
};

auto tTextOutputs = test("MLGraph/Text/outputsNameTheirOpOrAnIdentity") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {3}, DType::float16);
    auto weights = zeroConstant(graph, "w", {3});
    auto normalized = graph.softmax(x, 0);
    graph.softmax(x, 0);
    graph.output(normalized, "a");
    graph.output(normalized, "b");
    graph.output(x, "c");
    graph.output(weights, "d");
    buildChecked(graph);

    checkText(graph.toText(),
              "program(1)\n"
              "func main<CoreML7>(tensor<fp16, [3]> x) {\n"
              "    tensor<fp16, [3]> w = const()[val = blob(64)];\n"
              "    tensor<int32, []> a_axis = const()[val = 0];\n"
              "    tensor<fp16, [3]> a = softmax(x = x, axis = a_axis);\n"
              "    tensor<fp16, [3]> b = identity(x = a);\n"
              "    tensor<fp16, [3]> c = identity(x = x);\n"
              "    tensor<fp16, [3]> d = identity(x = w);\n"
              "} -> (a, b, c, d);\n");
};

auto tTextIdentifiers = test("MLGraph/Text/namesAreMILIdentifiers") = []
{
    // Core ML's MIL parser takes identifiers only, and crashes rather than
    // failing on a '.', so feature names are refused and constant names -
    // safetensors keys, typically - are made into identifiers.
    auto graph = Graph {};
    check(!graph.input("mel.frames", {3}, DType::float16).isValid());
    check(failedWith(graph, "input: name 'mel.frames' is not an identifier"));

    auto renamed = Graph {};
    auto x = renamed.input("x", {3}, DType::float16);
    auto weight = zeroConstant(renamed, "blocks.0.attn_ln.weight", {3});
    auto bias = zeroConstant(renamed, "0bias", {3});
    renamed.output(renamed.layerNorm(x, {0}, weight, bias), "y");
    buildChecked(renamed);

    auto text = renamed.toText();
    check(text.find("blocks_0_attn_ln_weight = const()") != std::string::npos);
    check(text.find("_0bias = const()") != std::string::npos);

    auto zero = zeroHalves(1);
    check(!renamed.constant("blocks_0_attn_ln_weight", {1}, DType::float16, zero)
               .isValid());

    renamed.output(x, "y.out");
    check(failedWith(renamed, "output: name 'y.out' is not an identifier"));
};

auto tTextNames = test("MLGraph/Text/generatedNamesAvoidTheCallersNames") = []
{
    auto graph = Graph {};
    auto x = graph.input("softmax_1", {3}, DType::float16);
    graph.output(graph.gelu(graph.softmax(x, 0)), "y");
    buildChecked(graph);

    auto text = graph.toText();
    check(text.find("tensor<fp16, [3]> softmax_1_1 = softmax(x = softmax_1, axis = "
                    "softmax_1_1_axis);")
          != std::string::npos);
};
