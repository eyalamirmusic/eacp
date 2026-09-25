#include "GraphCommon.h"

// Shape inference for every op, and the misuse each refuses. Every graph that
// is valid is also built, and compiled by Core ML where it is here.

using namespace nano;
using namespace eacp;
using namespace eacp::ML;
using namespace MLGraphTesting;

auto tShapeInputs = test("MLGraph/Shape/inputsAndEnumeratedShapes") = []
{
    auto graph = Graph {};
    auto x =
        graph.input("x", {1500, 384}, {{448, 384}, {1024, 384}}, DType::float16);
    auto y = graph.input("y", {2, 3}, DType::float32);

    check(graph.shape(x) == Shape {Shape::unknown, 384});
    check(graph.type(x) == DType::float16);
    check(graph.shape(y) == Shape {2, 3});
    check(!graph.shape(x).isFixed());
    check(graph.shape(y).count() == 6);

    graph.output(graph.softmax(x, -1), "z");
    graph.output(graph.softmax(y, 0), "w");
    buildChecked(graph);

    auto specification = graph.specification();
    auto& feature = specification.description.inputs[0];
    check(feature.shape == Vector<std::int64_t> {1500, 384});
    check(feature.enumeratedShapes.size() == 3);
    check(feature.enumeratedShapes[0] == Vector<std::int64_t> {1500, 384});
    check(feature.enumeratedShapes[2] == Vector<std::int64_t> {1024, 384});
    check(specification.description.inputs[1].enumeratedShapes.empty());
    check(specification.description.outputs[0].shape.empty());
    check(specification.description.outputs[1].shape == Vector<std::int64_t> {2, 3});
};

auto tShapeInputRefusals = test("MLGraph/Shape/inputRefusals") = []
{
    auto duplicate = Graph {};
    duplicate.input("x", {2}, DType::float16);
    check(!duplicate.input("x", {2}, DType::float16).isValid());
    check(failedWith(duplicate, "taken"));

    auto scalar = Graph {};
    check(!scalar.input("x", {}, DType::float16).isValid());

    auto ranks = Graph {};
    check(!ranks.input("x", {2, 3}, {{2}}, DType::float16).isValid());
    check(failedWith(ranks, "enumerated"));
};

auto tShapeConstant = test("MLGraph/Shape/constantByteCountIsChecked") = []
{
    auto graph = Graph {};
    auto bytes = zeroHalves(5);
    check(!graph.constant("w", {2, 3}, DType::float16, bytes).isValid());
    check(failedWith(graph, "needs 12"));
};

auto tShapeHalfConstant = test("MLGraph/Shape/halfConstantValueCountIsChecked") = []
{
    auto graph = Graph {};
    auto values = Vector<float> {1, 2, 3, 4, 5};
    check(!graph.halfConstant("w", {2, 3}, values).isValid());
    check(failedWith(graph, "halfConstant: 'w' has 5 values where [2, 3] needs 6"));

    auto fitting = Graph {};
    auto six = Vector<float> {1, 2, 3, 4, 5, 6};
    auto weight = fitting.halfConstant("w", {2, 3}, six);
    check(fitting.isValid());
    check(fitting.shape(weight) == Shape {2, 3});
    check(fitting.type(weight) == DType::float16);
};

auto tShapeLinear = test("MLGraph/Shape/linear") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {7, 4}, DType::float16);
    auto weight = zeroConstant(graph, "w", {3, 4});
    auto bias = zeroConstant(graph, "b", {3});
    auto y = graph.linear(x, weight, bias);

    check(graph.shape(y) == Shape {7, 3});
    graph.output(y, "y");
    buildChecked(graph);

    auto wrong = Graph {};
    auto input = wrong.input("x", {7, 5}, DType::float16);
    auto w = zeroConstant(wrong, "w", {3, 4});
    auto b = zeroConstant(wrong, "b", {3});
    check(!wrong.linear(input, w, b).isValid());
    check(failedWith(wrong, "linear: x [7, 5] does not fit"));
};

auto tShapeMatmul = test("MLGraph/Shape/matmulWithTransposesAndBatches") = []
{
    auto graph = Graph {};
    auto a = graph.input("a", {2, 5, 4}, DType::float16);
    auto b = graph.input("b", {2, 6, 4}, DType::float16);
    auto c = graph.input("c", {4, 3}, DType::float16);

    auto scores = graph.matmul(a, b, false, true);
    auto product = graph.matmul(a, c);
    auto both = graph.matmul(a, a, true, false);

    check(graph.shape(scores) == Shape {2, 5, 6});
    check(graph.shape(product) == Shape {2, 5, 3});
    check(graph.shape(both) == Shape {2, 4, 4});

    graph.output(scores, "scores");
    graph.output(product, "product");
    graph.output(both, "both");
    buildChecked(graph);

    check(!graph.matmul(a, b).isValid());
    check(failedWith(graph, "inner dimensions differ"));
};

auto tShapeTranspose = test("MLGraph/Shape/transpose") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {2, 3, 4}, DType::float16);
    auto y = graph.transpose(x, {2, 0, 1});

    check(graph.shape(y) == Shape {4, 2, 3});
    graph.output(y, "y");
    buildChecked(graph);

    check(!graph.transpose(x, {0, 0, 1}).isValid());
    check(!graph.transpose(x, {1, 0}).isValid());
};

auto tShapeReshape = test("MLGraph/Shape/reshapeInfersOneDimension") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {6, 8}, DType::float16);
    auto y = graph.reshape(x, {Shape::unknown, 2, 4});

    check(graph.shape(y) == Shape {6, 2, 4});
    graph.output(y, "y");
    buildChecked(graph);

    check(!graph.reshape(x, {5, 10}).isValid());
    check(!graph.reshape(x, {Shape::unknown, Shape::unknown}).isValid());

    auto flexible = Graph {};
    auto rows = flexible.input("x", {6, 8}, {{4, 8}}, DType::float16);
    auto heads = flexible.reshape(rows, {Shape::unknown, 2, 4});
    check(flexible.shape(heads) == Shape {Shape::unknown, 2, 4});
    flexible.output(heads, "y");
    buildChecked(flexible);

    check(!flexible.reshape(rows, {48}).isValid());
    check(failedWith(flexible, "needs a -1"));
};

auto tShapeSoftmax = test("MLGraph/Shape/softmaxAxis") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {3, 4}, DType::float16);

    check(graph.shape(graph.softmax(x, 0)) == Shape {3, 4});
    check(!graph.softmax(x, 2).isValid());
    check(failedWith(graph, "softmax: axis 2 is outside [3, 4]"));
};

auto tShapeReductions = test("MLGraph/Shape/sumAndMax") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {3, 4, 5}, DType::float16);
    auto summed = graph.sum(x, 1);
    auto kept = graph.max(x, -1, true);

    check(graph.shape(summed) == Shape {3, 5});
    check(graph.shape(kept) == Shape {3, 4, 1});

    graph.output(summed, "summed");
    graph.output(kept, "kept");
    buildChecked(graph);

    check(!graph.sum(x, 3).isValid());
};

auto tShapeLayerNorm = test("MLGraph/Shape/layerNorm") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {5, 8}, DType::float16);
    auto gamma = zeroConstant(graph, "gamma", {8});
    auto beta = zeroConstant(graph, "beta", {8});
    auto y = graph.layerNorm(x, {-1}, gamma, beta);

    check(graph.shape(y) == Shape {5, 8});
    graph.output(y, "y");
    buildChecked(graph);

    auto wrongGamma = zeroConstant(graph, "gamma5", {5});
    check(!graph.layerNorm(x, {1}, wrongGamma, beta).isValid());
    check(failedWith(graph, "normalized shape [8]"));
};

auto tShapeConv = test("MLGraph/Shape/conv1d") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {1, 4, 10}, DType::float16);
    auto weight = zeroConstant(graph, "w", {6, 4, 3});
    auto bias = zeroConstant(graph, "b", {6});

    auto same = graph.conv(x, weight, bias, 1, 1);
    auto strided = graph.conv(x, weight, bias, 2, 1);

    check(graph.shape(same) == Shape {1, 6, 10});
    check(graph.shape(strided) == Shape {1, 6, 5});

    graph.output(same, "same");
    graph.output(strided, "strided");
    buildChecked(graph);

    auto wrongChannels = zeroConstant(graph, "w5", {6, 5, 3});
    check(!graph.conv(x, wrongChannels, bias, 1, 1).isValid());
    check(!graph.conv(x, weight, bias, 0, 1).isValid());
};

auto tShapeGather = test("MLGraph/Shape/gather") = []
{
    auto graph = Graph {};
    auto table = zeroConstant(graph, "table", {10, 4});
    auto indices = graph.input("tokens", {3}, DType::int32);
    auto rows = graph.gather(table, indices, 0);

    check(graph.shape(rows) == Shape {3, 4});
    graph.output(rows, "rows");
    buildChecked(graph);

    auto floats = graph.input("floats", {3}, DType::float16);
    check(!graph.gather(table, floats, 0).isValid());
    check(failedWith(graph, "int32"));
};

auto tShapeConcat = test("MLGraph/Shape/concat") = []
{
    auto graph = Graph {};
    auto a = graph.input("a", {2, 3}, DType::float16);
    auto b = graph.input("b", {2, 5}, DType::float16);
    auto joined = graph.concat({a, b}, 1);

    check(graph.shape(joined) == Shape {2, 8});
    graph.output(joined, "joined");
    buildChecked(graph);

    check(!graph.concat({a, b}, 0).isValid());
    check(!graph.concat({}, 0).isValid());
};

auto tShapeSlice = test("MLGraph/Shape/slice") = []
{
    auto graph = Graph {};
    auto table = zeroConstant(graph, "table", {10, 4});
    auto rows = graph.slice(table, {2, 0}, {6, 4});

    check(graph.shape(rows) == Shape {4, 4});

    auto x = graph.input("x", {8, 4}, {{6, 4}}, DType::float16);
    auto columns = graph.slice(x, {0, 1}, {Shape::unknown, 3});
    check(graph.shape(columns) == Shape {Shape::unknown, 2});

    graph.output(rows, "rows");
    graph.output(columns, "columns");
    buildChecked(graph);

    check(!graph.slice(table, {0, 0}, {11, 4}).isValid());
    check(!graph.slice(x, {1, 0}, {Shape::unknown, 4}).isValid());
};

auto tShapeSliceLike = test("MLGraph/Shape/sliceLike") = []
{
    auto graph = Graph {};
    auto table = zeroConstant(graph, "table", {10, 4});
    auto rows = graph.input("rows", {8, 4}, {{6, 4}}, DType::float16);
    auto fixed = graph.input("fixed", {3, 4}, DType::float16);

    auto cut = graph.sliceLike(table, rows);
    check(graph.shape(cut) == Shape {Shape::unknown, 4});
    auto head = graph.sliceLike(table, fixed);
    check(graph.shape(head) == Shape {3, 4});
    check(graph.sliceLike(rows, rows) == rows);

    auto add = [](const GPU::Float& a, const GPU::Float& b) { return a + b; };
    graph.output(graph.apply(rows, cut, add), "cut");
    graph.output(graph.apply(fixed, head, add), "head");
    buildChecked(graph);

    auto flat = graph.input("flat", {4}, DType::float16);
    check(!graph.sliceLike(table, flat).isValid());
    check(failedWith(graph, "needs the rank of"));

    auto tooWide = Graph {};
    auto small = zeroConstant(tooWide, "small", {10, 2});
    auto wide = tooWide.input("wide", {8, 4}, {{6, 4}}, DType::float16);
    check(!tooWide.sliceLike(small, wide).isValid());

    auto fromUnknown = Graph {};
    auto enumerated = fromUnknown.input("x", {8, 4}, {{6, 4}}, DType::float16);
    auto known = fromUnknown.input("y", {2, 4}, DType::float16);
    check(!fromUnknown.sliceLike(enumerated, known).isValid());
};

// The Whisper encoder's ops over an enumerated sequence length: the unknown
// axis survives conv, transpose, reshape with a -1, linear, layer norm, attention
// at rank 3 and 4, and an apply of two tensors that share it.
auto tShapeUnknownPropagates =
    test("MLGraph/Shape/anEnumeratedLengthFlowsThroughTheEncoderOps") = []
{
    auto graph = Graph {};
    auto add = [](const GPU::Float& a, const GPU::Float& b) { return a + b; };
    auto mel = graph.input("mel", {1, 4, 16}, {{1, 4, 12}}, DType::float16);
    auto convBias = zeroConstant(graph, "cb", {6});
    auto first =
        graph.conv(mel, zeroConstant(graph, "c1", {6, 4, 3}), convBias, 1, 1);
    auto second =
        graph.conv(first, zeroConstant(graph, "c2", {6, 6, 3}), convBias, 2, 1);
    check(graph.shape(first) == Shape {1, 6, Shape::unknown});
    check(graph.shape(second) == Shape {1, 6, Shape::unknown});

    auto frames = graph.transpose(second, {0, 2, 1});
    check(graph.shape(frames) == Shape {1, Shape::unknown, 6});

    auto rows = graph.reshape(frames, {Shape::unknown, 6});
    check(graph.shape(rows) == Shape {Shape::unknown, 6});

    auto positioned = graph.apply(
        rows, graph.sliceLike(zeroConstant(graph, "positions", {8, 6}), rows), add);
    check(graph.shape(positioned) == Shape {Shape::unknown, 6});

    auto normed = graph.layerNorm(positioned,
                                  {-1},
                                  zeroConstant(graph, "gamma", {6}),
                                  zeroConstant(graph, "beta", {6}));
    auto projected = graph.linear(normed, zeroConstant(graph, "w", {6, 6}));
    check(graph.shape(normed) == Shape {Shape::unknown, 6});
    check(graph.shape(projected) == Shape {Shape::unknown, 6});

    auto heads =
        graph.transpose(graph.reshape(projected, {Shape::unknown, 2, 3}), {1, 0, 2});
    auto attended = graph.scaledDotProductAttention(heads, heads, heads, false);
    check(graph.shape(attended) == Shape {2, Shape::unknown, 3});

    auto batched = graph.transpose(
        graph.reshape(projected, {1, Shape::unknown, 2, 3}), {0, 2, 1, 3});
    auto attended4 =
        graph.scaledDotProductAttention(batched, batched, batched, false);
    check(graph.shape(attended4) == Shape {1, 2, Shape::unknown, 3});

    auto merged =
        graph.reshape(graph.transpose(attended, {1, 0, 2}), {Shape::unknown, 6});
    auto merged4 =
        graph.reshape(graph.transpose(attended4, {0, 2, 1, 3}), {Shape::unknown, 6});
    auto summed = graph.apply(merged, merged4, add);
    check(graph.shape(summed) == Shape {Shape::unknown, 6});

    graph.output(summed, "y");
    buildChecked(graph);
};

auto tShapeAttention = test("MLGraph/Shape/scaledDotProductAttention") = []
{
    auto graph = Graph {};
    auto q = graph.input("q", {2, 5, 8}, DType::float16);
    auto k = graph.input("k", {2, 7, 8}, DType::float16);
    auto v = graph.input("v", {2, 7, 4}, DType::float16);
    auto attended = graph.scaledDotProductAttention(q, k, v, false);

    check(graph.shape(attended) == Shape {2, 5, 4});
    graph.output(attended, "attended");

    auto square = graph.input("square", {2, 5, 8}, DType::float16);
    auto causal = graph.scaledDotProductAttention(square, square, square, true);
    check(graph.shape(causal) == Shape {2, 5, 8});
    graph.output(causal, "causal");

    buildChecked(graph);
    check(graph.specification().program.main.opset == "CoreML8");
    check(graph.specification().specificationVersion == 9);

    check(!graph.scaledDotProductAttention(q, v, v, false).isValid());

    auto flat = graph.input("flat", {5, 8}, DType::float16);
    check(!graph.scaledDotProductAttention(flat, flat, flat, false).isValid());
};

auto tShapeGeluCast = test("MLGraph/Shape/geluAndCast") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {3, 4}, DType::float16);
    auto activated = graph.gelu(x);
    auto widened = graph.cast(activated, DType::float32);

    check(graph.shape(widened) == Shape {3, 4});
    check(graph.type(widened) == DType::float32);
    check(graph.cast(x, DType::float16) == x);

    graph.output(widened, "y");
    buildChecked(graph);
    check(graph.specification().program.main.opset == "CoreML7");

    auto indices = graph.input("i", {3}, DType::int32);
    check(!graph.gelu(indices).isValid());
};

auto tShapeErrorsPropagate =
    test("MLGraph/Shape/invalidTensorsPropagateOneError") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {3, 4}, DType::float16);
    auto bad = graph.softmax(x, 5);
    auto worse = graph.gelu(graph.softmax(bad, 0));
    graph.output(worse, "y");

    check(!worse.isValid());
    check(graph.errors().size() == 1);
    check(graph.build().isEmpty());
    check(graph.toText() == "invalid graph: softmax: axis 5 is outside [3, 4]\n");
};

auto tShapeOutputs = test("MLGraph/Shape/outputRefusals") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {3}, DType::float16);
    graph.output(x, "x");
    check(failedWith(graph, "output: name 'x'"));

    auto empty = Graph {};
    empty.input("x", {3}, DType::float16);
    check(empty.isValid());
    check(empty.build().isEmpty());
    check(empty.toText() == "invalid graph: no outputs\n");

    auto foreign = Graph {};
    foreign.output(Tensor {7}, "y");
    check(failedWith(foreign, "not a tensor of this graph"));
};
