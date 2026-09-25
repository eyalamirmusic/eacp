#include "Graph.h"

#include <algorithm>
#include <bit>

namespace eacp::ML
{
namespace
{
MIL::DataType toMIL(DType type)
{
    switch (type)
    {
        case DType::float16:
            return MIL::DataType::float16;
        case DType::float32:
            return MIL::DataType::float32;
        case DType::int32:
            return MIL::DataType::int32;
    }

    return MIL::DataType::float32;
}

DType fromMIL(MIL::DataType type)
{
    switch (type)
    {
        case MIL::DataType::float16:
            return DType::float16;
        case MIL::DataType::int32:
            return DType::int32;
        default:
            return DType::float32;
    }
}

bool hasPositiveDims(const Shape& shape)
{
    for (auto size: shape.dims)
        if (size <= 0)
            return false;

    return true;
}

bool isIdentifierCharacter(char character, bool first)
{
    auto isLetter = (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z') || character == '_';
    auto isDigit = character >= '0' && character <= '9';
    return isLetter || (!first && isDigit);
}

bool isIdentifier(std::string_view name)
{
    for (auto index = std::size_t {0}; index < name.size(); ++index)
        if (!isIdentifierCharacter(name[index], index == 0))
            return false;

    return !name.empty();
}

std::string identifierFrom(std::string_view name)
{
    auto identifier = std::string {};

    for (auto character: name)
        identifier += isIdentifierCharacter(character, false) ? character : '_';

    if (!identifier.empty() && !isIdentifierCharacter(identifier[0], true))
        identifier.insert(identifier.begin(), '_');

    return identifier;
}

Shape mergedShape(const Shape& defaultShape, const Vector<Shape>& shapes)
{
    auto merged = defaultShape;

    for (auto& shape: shapes)
        for (auto axis = 0; axis < merged.rank(); ++axis)
            if (shape[axis] != merged[axis])
                merged.dims[axis] = Shape::unknown;

    return merged;
}

Vector<std::int32_t> toInt32s(const Vector<int>& values)
{
    auto result = Vector<std::int32_t> {};

    for (auto value: values)
        result.add(static_cast<std::int32_t>(value));

    return result;
}

Bytes causalMaskBytes(int rows, int columns, MIL::DataType type)
{
    auto bytes = Bytes {};

    for (auto row = 0; row < rows; ++row)
    {
        for (auto column = 0; column < columns; ++column)
        {
            auto value = column > row ? 0.f : 1.f;

            if (type == MIL::DataType::float16)
            {
                appendHalf(bytes, value);
                continue;
            }

            auto bits = std::bit_cast<std::uint32_t>(value);

            for (auto shift = 0; shift < 32; shift += 8)
                bytes.add(static_cast<std::uint8_t>(bits >> shift));
        }
    }

    return bytes;
}
} // namespace

std::string toString(DType type)
{
    return MIL::toText(toMIL(type));
}

Shape::Shape(std::initializer_list<int> dimsToUse)
    : dims(dimsToUse)
{
}

Shape::Shape(const Vector<int>& dimsToUse)
    : dims(dimsToUse)
{
}

bool Shape::isFixed() const
{
    for (auto size: dims)
        if (size == unknown)
            return false;

    return true;
}

std::int64_t Shape::count() const
{
    if (!isFixed())
        return unknown;

    auto total = std::int64_t {1};

    for (auto size: dims)
        total *= size;

    return total;
}

std::string Shape::toString() const
{
    auto text = std::string {"["};

    for (auto axis = 0; axis < rank(); ++axis)
    {
        text += axis > 0 ? ", " : "";
        text +=
            dims[axis] == unknown ? std::string {"?"} : std::to_string(dims[axis]);
    }

    return text + "]";
}

Tensor Graph::fail(std::string_view op, const std::string& message)
{
    errorList.add(std::string {op} + ": " + message);
    return {};
}

bool Graph::acceptOperands(std::string_view op, std::initializer_list<Tensor> values)
{
    for (auto value: values)
    {
        if (isKnown(value))
            continue;

        if (errorList.empty())
            fail(op, "an operand is not a tensor of this graph");

        return false;
    }

    return true;
}

bool Graph::isKnown(Tensor value) const
{
    return value.id >= 0 && value.id < nodes.size();
}

const Graph::Node& Graph::node(Tensor value) const
{
    return nodes[value.id];
}

bool Graph::isNameTaken(std::string_view name) const
{
    for (auto& existing: nodes)
        if (existing.kind != NodeKind::operation && existing.name == name)
            return true;

    for (auto& binding: outputs)
        if (binding.name == name)
            return true;

    return false;
}

bool Graph::isFloat(Tensor value) const
{
    auto type = node(value).type;
    return type == MIL::DataType::float16 || type == MIL::DataType::float32;
}

std::optional<int> Graph::normalizedAxis(int axis, int rank) const
{
    auto normalized = axis < 0 ? axis + rank : axis;

    if (normalized < 0 || normalized >= rank)
        return std::nullopt;

    return normalized;
}

Tensor Graph::addNode(const Node& newNode)
{
    nodes.add(newNode);
    return {nodes.size() - 1};
}

Tensor Graph::addOperation(std::string_view op,
                           const Shape& shape,
                           MIL::DataType type,
                           const Vector<Parameter>& parameters)
{
    auto operation = Node {};
    operation.kind = NodeKind::operation;
    operation.op = op;
    operation.shape = shape;
    operation.type = type;
    operation.parameters = parameters;
    return addNode(operation);
}

Tensor Graph::addBlobConstant(std::string_view name,
                              const Shape& shape,
                              MIL::DataType type,
                              Span<const std::uint8_t> bytes)
{
    auto constant = Node {};
    constant.kind = NodeKind::constant;
    constant.name = name;
    constant.shape = shape;
    constant.type = type;

    auto tensor = addNode(constant);
    nodes[tensor.id].bytes.getVector().assign(bytes.begin(), bytes.end());
    return tensor;
}

Graph::Parameter Graph::tensorParameter(std::string_view name, Tensor value)
{
    auto parameter = Parameter {};
    parameter.name = name;
    parameter.tensors.add(value.id);
    return parameter;
}

Graph::Parameter Graph::valueParameter(std::string_view name,
                                       const MIL::Value& value)
{
    auto parameter = Parameter {};
    parameter.name = name;
    parameter.immediate = value;
    return parameter;
}

std::optional<Shape> Graph::broadcastShape(const Shape& a, const Shape& b) const
{
    auto rank = std::max(a.rank(), b.rank());
    auto result = Shape {};
    result.dims.resize(rank);

    for (auto axis = 0; axis < rank; ++axis)
    {
        auto fromA = axis - (rank - a.rank());
        auto fromB = axis - (rank - b.rank());
        auto sizeA = fromA >= 0 ? a[fromA] : 1;
        auto sizeB = fromB >= 0 ? b[fromB] : 1;

        if (sizeA == sizeB || sizeB == 1)
            result.dims[axis] = sizeA;
        else if (sizeA == 1)
            result.dims[axis] = sizeB;
        else
            return std::nullopt;
    }

    return result;
}

Tensor Graph::input(std::string_view name, const Shape& shape, DType type)
{
    return input(name, shape, {}, type);
}

Tensor Graph::input(std::string_view name,
                    const Shape& defaultShape,
                    const Vector<Shape>& enumeratedShapes,
                    DType type)
{
    if (!isIdentifier(name) || isNameTaken(name))
        return fail("input",
                    "name '" + std::string {name}
                        + "' is not an identifier or is taken");

    if (defaultShape.rank() == 0 || !hasPositiveDims(defaultShape))
        return fail("input",
                    "'" + std::string {name} + "' needs a rank of at least one "
                        + "and positive dimensions, not " + defaultShape.toString());

    auto shapes = Vector<Shape> {defaultShape};

    for (auto& shape: enumeratedShapes)
    {
        if (shape.rank() != defaultShape.rank() || !hasPositiveDims(shape))
            return fail("input",
                        "enumerated shape " + shape.toString() + " of '"
                            + std::string {name} + "' does not match the rank of "
                            + defaultShape.toString());

        if (!shapes.contains(shape))
            shapes.add(shape);
    }

    auto newInput = Node {};
    newInput.kind = NodeKind::input;
    newInput.name = name;
    newInput.type = toMIL(type);
    newInput.shape = mergedShape(defaultShape, shapes);

    if (shapes.size() > 1)
        newInput.enumeratedShapes = shapes;
    else
        newInput.enumeratedShapes = {defaultShape};

    return addNode(newInput);
}

void Graph::output(Tensor value, std::string_view name)
{
    if (!acceptOperands("output", {value}))
        return;

    if (!isIdentifier(name) || isNameTaken(name))
    {
        fail("output",
             "name '" + std::string {name} + "' is not an identifier or is taken");
        return;
    }

    if (node(value).type == MIL::DataType::boolean || node(value).shape.rank() == 0)
    {
        fail("output", "'" + std::string {name} + "' is not a multi-array");
        return;
    }

    outputs.add({value.id, std::string {name}});
}

Tensor Graph::constant(std::string_view name,
                       const Shape& shape,
                       DType type,
                       Span<const std::uint8_t> bytes)
{
    auto identifier = identifierFrom(name);

    if (identifier.empty() || isNameTaken(identifier))
        return fail("constant", "name '" + identifier + "' is empty or taken");

    if (!shape.isFixed() || !hasPositiveDims(shape))
        return fail("constant",
                    "'" + std::string {name} + "' has shape " + shape.toString());

    auto milType = toMIL(type);
    auto expected = static_cast<std::uint64_t>(shape.count())
                    * static_cast<std::uint64_t>(MIL::sizeOf(milType));

    if (bytes.getSize() != expected)
        return fail("constant",
                    "'" + std::string {name} + "' has "
                        + std::to_string(bytes.getSize()) + " bytes where "
                        + shape.toString() + " of " + toString(type) + " needs "
                        + std::to_string(expected));

    return addBlobConstant(identifier, shape, milType, bytes);
}

Tensor Graph::halfConstant(std::string_view name,
                           const Shape& shape,
                           Span<const float> values)
{
    if (static_cast<std::int64_t>(values.getSize()) != shape.count())
        return fail("halfConstant",
                    "'" + std::string {name} + "' has "
                        + std::to_string(values.getSize()) + " values where "
                        + shape.toString() + " needs "
                        + std::to_string(shape.count()));

    auto bytes = halfBytes(values);
    return constant(name, shape, DType::float16, bytes);
}

Tensor Graph::scalar(float value)
{
    auto newScalar = Node {};
    newScalar.kind = NodeKind::scalar;
    newScalar.type = MIL::DataType::float32;
    newScalar.value = value;
    return addNode(newScalar);
}

Tensor Graph::linear(Tensor x, Tensor weight, Tensor bias)
{
    if (!acceptOperands("linear", {x, weight, bias}))
        return {};

    auto& in = node(x).shape;
    auto& weights = node(weight).shape;
    auto& biases = node(bias).shape;

    if (in.rank() < 1 || weights.rank() != 2 || biases.rank() != 1)
        return fail("linear",
                    "needs x of rank >= 1, weight [out, in] and bias [out], not "
                        + in.toString() + ", " + weights.toString() + ", "
                        + biases.toString());

    if (in[in.rank() - 1] != weights[1] || biases[0] != weights[0])
        return fail("linear",
                    "x " + in.toString() + " does not fit weight "
                        + weights.toString() + " and bias " + biases.toString());

    if (!isFloat(x) || node(weight).type != node(x).type
        || node(bias).type != node(x).type)
        return fail("linear", "x, weight and bias need one floating-point type");

    auto result = in;
    result.dims[result.rank() - 1] = weights[0];

    return addOperation("linear",
                        result,
                        node(x).type,
                        {tensorParameter("x", x),
                         tensorParameter("weight", weight),
                         tensorParameter("bias", bias)});
}

Tensor Graph::linear(Tensor x, Tensor weight)
{
    if (!acceptOperands("linear", {x, weight}))
        return {};

    auto weights = node(weight).shape;

    if (weights.rank() != 2 || !weights.isFixed() || !isFloat(x))
        return fail("linear",
                    "needs a floating-point x and weight [out, in], not "
                        + weights.toString());

    auto type = node(x).type;
    auto zeros = Bytes {};
    zeros.resize(weights[0] * MIL::sizeOf(type), 0);
    auto bias = addBlobConstant("", {weights[0]}, type, zeros);

    return linear(x, weight, bias);
}

Tensor Graph::matmul(Tensor a, Tensor b, bool transposeA, bool transposeB)
{
    if (!acceptOperands("matmul", {a, b}))
        return {};

    auto left = node(a).shape;
    auto right = node(b).shape;

    if (left.rank() < 2 || right.rank() < 2)
        return fail("matmul",
                    "needs operands of rank >= 2, not " + left.toString() + " and "
                        + right.toString());

    if (!isFloat(a) || node(a).type != node(b).type)
        return fail("matmul", "needs two operands of one floating-point type");

    if (transposeA)
        std::swap(left.dims[left.rank() - 2], left.dims[left.rank() - 1]);

    if (transposeB)
        std::swap(right.dims[right.rank() - 2], right.dims[right.rank() - 1]);

    if (left[left.rank() - 1] != right[right.rank() - 2])
        return fail("matmul",
                    "inner dimensions differ: " + left.toString() + " x "
                        + right.toString());

    auto batchOf = [](const Shape& shape)
    {
        auto batch = Shape {};

        for (auto axis = 0; axis < shape.rank() - 2; ++axis)
            batch.dims.add(shape[axis]);

        return batch;
    };

    auto batch = broadcastShape(batchOf(left), batchOf(right));

    if (!batch)
        return fail("matmul",
                    "batch dimensions do not broadcast: " + left.toString() + " x "
                        + right.toString());

    auto result = *batch;
    result.dims.add(left[left.rank() - 2]);
    result.dims.add(right[right.rank() - 1]);

    return addOperation(
        "matmul",
        result,
        node(a).type,
        {tensorParameter("x", a),
         tensorParameter("y", b),
         valueParameter("transpose_x", MIL::Value::scalar(transposeA)),
         valueParameter("transpose_y", MIL::Value::scalar(transposeB))});
}

Tensor Graph::transpose(Tensor x, const Vector<int>& perm)
{
    if (!acceptOperands("transpose", {x}))
        return {};

    auto& in = node(x).shape;
    auto seen = Vector<int> {};

    if (perm.size() != in.rank())
        return fail("transpose",
                    "perm has " + std::to_string(perm.size()) + " axes for "
                        + in.toString());

    auto result = Shape {};

    for (auto axis: perm)
    {
        if (axis < 0 || axis >= in.rank() || seen.contains(axis))
            return fail("transpose", "perm is not a permutation of the axes");

        seen.add(axis);
        result.dims.add(in[axis]);
    }

    return addOperation("transpose",
                        result,
                        node(x).type,
                        {tensorParameter("x", x),
                         valueParameter("perm", MIL::Value::ints(toInt32s(perm)))});
}

Tensor Graph::reshape(Tensor x, const Shape& shape)
{
    if (!acceptOperands("reshape", {x}))
        return {};

    auto& in = node(x).shape;
    auto inferred = -1;
    auto knownCount = std::int64_t {1};

    for (auto axis = 0; axis < shape.rank(); ++axis)
    {
        if (shape[axis] == Shape::unknown && inferred < 0)
            inferred = axis;
        else if (shape[axis] <= 0)
            return fail("reshape",
                        shape.toString() + " may hold one -1 and positive sizes");
        else
            knownCount *= shape[axis];
    }

    auto result = shape;

    if (in.isFixed())
    {
        auto total = in.count();

        if (inferred >= 0 && total % knownCount == 0)
            result.dims[inferred] = static_cast<int>(total / knownCount);

        if (result.count() != total)
            return fail("reshape",
                        "cannot reshape " + in.toString() + " to "
                            + shape.toString());
    }
    else if (inferred < 0)
    {
        return fail("reshape",
                    "reshaping " + in.toString()
                        + " needs a -1 for the enumerated dimension");
    }

    return addOperation(
        "reshape",
        result,
        node(x).type,
        {tensorParameter("x", x),
         valueParameter("shape", MIL::Value::ints(toInt32s(shape.dims)))});
}

Tensor Graph::softmax(Tensor x, int axis)
{
    if (!acceptOperands("softmax", {x}))
        return {};

    auto normalized = normalizedAxis(axis, node(x).shape.rank());

    if (!normalized)
        return fail("softmax",
                    "axis " + std::to_string(axis) + " is outside "
                        + node(x).shape.toString());

    if (!isFloat(x))
        return fail("softmax", "needs a floating-point tensor");

    return addOperation(
        "softmax",
        node(x).shape,
        node(x).type,
        {tensorParameter("x", x), valueParameter("axis", MIL::Value::scalar(axis))});
}

Tensor Graph::reduce(std::string_view op, Tensor x, int axis, bool keepDims)
{
    if (!acceptOperands(op, {x}))
        return {};

    auto& in = node(x).shape;
    auto normalized = normalizedAxis(axis, in.rank());

    if (!normalized)
        return fail(op,
                    "axis " + std::to_string(axis) + " is outside " + in.toString());

    auto result = Shape {};

    for (auto index = 0; index < in.rank(); ++index)
    {
        if (index != *normalized)
            result.dims.add(in[index]);
        else if (keepDims)
            result.dims.add(1);
    }

    return addOperation(op,
                        result,
                        node(x).type,
                        {tensorParameter("x", x),
                         valueParameter("axes", MIL::Value::ints({axis})),
                         valueParameter("keep_dims", MIL::Value::scalar(keepDims))});
}

Tensor Graph::sum(Tensor x, int axis, bool keepDims)
{
    return reduce("reduce_sum", x, axis, keepDims);
}

Tensor Graph::max(Tensor x, int axis, bool keepDims)
{
    return reduce("reduce_max", x, axis, keepDims);
}

Tensor Graph::layerNorm(
    Tensor x, const Vector<int>& axes, Tensor gamma, Tensor beta, float epsilon)
{
    if (!acceptOperands("layer_norm", {x, gamma, beta}))
        return {};

    auto& in = node(x).shape;
    auto normalizedShape = Shape {};
    auto seen = Vector<int> {};

    if (axes.empty())
        return fail("layer_norm", "needs at least one axis");

    for (auto axis: axes)
    {
        auto normalized = normalizedAxis(axis, in.rank());

        if (!normalized || seen.contains(*normalized))
            return fail("layer_norm",
                        "axes are not distinct axes of " + in.toString());

        seen.add(*normalized);
        normalizedShape.dims.add(in[*normalized]);
    }

    if (!normalizedShape.isFixed() || node(gamma).shape != normalizedShape
        || node(beta).shape != normalizedShape)
        return fail("layer_norm",
                    "gamma and beta need the normalized shape "
                        + normalizedShape.toString());

    if (!isFloat(x) || node(gamma).type != node(x).type
        || node(beta).type != node(x).type)
        return fail("layer_norm", "x, gamma and beta need one floating-point type");

    return addOperation(
        "layer_norm",
        in,
        node(x).type,
        {tensorParameter("x", x),
         valueParameter("axes", MIL::Value::ints(toInt32s(axes))),
         tensorParameter("gamma", gamma),
         tensorParameter("beta", beta),
         valueParameter("epsilon", MIL::Value::scalar(epsilon, node(x).type))});
}

Tensor Graph::conv(Tensor x, Tensor weight, Tensor bias, int stride, int padding)
{
    if (!acceptOperands("conv", {x, weight, bias}))
        return {};

    auto& in = node(x).shape;
    auto& weights = node(weight).shape;
    auto& biases = node(bias).shape;

    if (in.rank() != 3 || weights.rank() != 3 || biases.rank() != 1)
        return fail("conv",
                    "needs x [N, C, L], weight [out, C, k] and bias [out], not "
                        + in.toString() + ", " + weights.toString() + ", "
                        + biases.toString());

    if (in[1] != weights[1] || biases[0] != weights[0])
        return fail("conv",
                    "x " + in.toString() + " does not fit weight "
                        + weights.toString() + " and bias " + biases.toString());

    if (stride < 1 || padding < 0)
        return fail("conv", "needs a stride of at least one and padding >= 0");

    if (!isFloat(x) || node(weight).type != node(x).type
        || node(bias).type != node(x).type)
        return fail("conv", "x, weight and bias need one floating-point type");

    auto length = in[2];

    if (length != Shape::unknown)
    {
        auto padded = length + 2 * padding - weights[2];

        if (padded < 0)
            return fail("conv", "the kernel is longer than the padded input");

        length = padded / stride + 1;
    }

    return addOperation(
        "conv",
        {in[0], weights[0], length},
        node(x).type,
        {tensorParameter("x", x),
         tensorParameter("weight", weight),
         tensorParameter("bias", bias),
         valueParameter("strides", MIL::Value::ints({stride})),
         valueParameter("pad_type", MIL::Value::string("custom")),
         valueParameter("pad", MIL::Value::ints({padding, padding})),
         valueParameter("dilations", MIL::Value::ints({1})),
         valueParameter("groups", MIL::Value::scalar(std::int32_t {1}))});
}

Tensor Graph::gather(Tensor table, Tensor indices, int axis)
{
    if (!acceptOperands("gather", {table, indices}))
        return {};

    auto& values = node(table).shape;
    auto normalized = normalizedAxis(axis, values.rank());

    if (!normalized)
        return fail("gather",
                    "axis " + std::to_string(axis) + " is outside "
                        + values.toString());

    if (node(indices).type != MIL::DataType::int32)
        return fail("gather", "indices need to be int32");

    auto result = Shape {};

    for (auto index = 0; index < *normalized; ++index)
        result.dims.add(values[index]);

    for (auto size: node(indices).shape.dims)
        result.dims.add(size);

    for (auto index = *normalized + 1; index < values.rank(); ++index)
        result.dims.add(values[index]);

    return addOperation(
        "gather",
        result,
        node(table).type,
        {tensorParameter("x", table),
         tensorParameter("indices", indices),
         valueParameter("axis", MIL::Value::scalar(static_cast<std::int32_t>(axis))),
         valueParameter("batch_dims", MIL::Value::scalar(std::int32_t {0})),
         valueParameter("validate_indices", MIL::Value::scalar(false))});
}

Tensor Graph::concat(const Vector<Tensor>& parts, int axis)
{
    if (parts.empty())
        return fail("concat", "needs at least one part");

    for (auto part: parts)
        if (!acceptOperands("concat", {part}))
            return {};

    auto result = node(parts[0]).shape;
    auto type = node(parts[0]).type;
    auto normalized = normalizedAxis(axis, result.rank());

    if (!normalized)
        return fail("concat",
                    "axis " + std::to_string(axis) + " is outside "
                        + result.toString());

    auto parameter = Parameter {};
    parameter.name = "values";
    parameter.tensors.add(parts[0].id);

    for (auto index = 1; index < parts.size(); ++index)
    {
        auto& shape = node(parts[index]).shape;

        if (shape.rank() != result.rank() || node(parts[index]).type != type)
            return fail("concat", "parts need one rank and one type");

        for (auto dim = 0; dim < shape.rank(); ++dim)
            if (dim != *normalized && shape[dim] != result[dim])
                return fail("concat",
                            shape.toString() + " does not fit "
                                + node(parts[0]).shape.toString());

        auto& total = result.dims[*normalized];
        total = total == Shape::unknown || shape[*normalized] == Shape::unknown
                    ? Shape::unknown
                    : total + shape[*normalized];

        parameter.tensors.add(parts[index].id);
    }

    return addOperation(
        "concat",
        result,
        type,
        {parameter,
         valueParameter("axis", MIL::Value::scalar(static_cast<std::int32_t>(axis))),
         valueParameter("interleave", MIL::Value::scalar(false))});
}

Tensor Graph::slice(Tensor x, const Vector<int>& begin, const Vector<int>& end)
{
    if (!acceptOperands("slice_by_index", {x}))
        return {};

    auto& in = node(x).shape;

    if (begin.size() != in.rank() || end.size() != in.rank())
        return fail("slice_by_index",
                    "begin and end need one entry per axis of " + in.toString());

    auto result = Shape {};
    auto endMask = Vector<std::uint8_t> {};
    auto endValues = Vector<int> {};

    for (auto axis = 0; axis < in.rank(); ++axis)
    {
        auto size = in[axis];
        auto wholeUnknown = size == Shape::unknown && begin[axis] == 0
                            && end[axis] == Shape::unknown;
        auto inRange = size != Shape::unknown && begin[axis] >= 0
                       && begin[axis] < end[axis] && end[axis] <= size;

        if (!wholeUnknown && !inRange)
            return fail("slice_by_index",
                        "axis " + std::to_string(axis) + " of " + in.toString()
                            + " cannot be sliced [" + std::to_string(begin[axis])
                            + ", " + std::to_string(end[axis]) + ")");

        result.dims.add(wholeUnknown ? Shape::unknown : end[axis] - begin[axis]);
        endMask.add(wholeUnknown ? 1 : 0);
        endValues.add(wholeUnknown ? 0 : end[axis]);
    }

    return addOperation(
        "slice_by_index",
        result,
        node(x).type,
        {tensorParameter("x", x),
         valueParameter("begin", MIL::Value::ints(toInt32s(begin))),
         valueParameter("end", MIL::Value::ints(toInt32s(endValues))),
         valueParameter("end_mask", MIL::Value::bools(endMask))});
}

Tensor Graph::sliceLike(Tensor x, Tensor reference)
{
    auto op = "slice_by_index";

    if (!acceptOperands(op, {x, reference}))
        return {};

    auto in = node(x).shape;
    auto like = node(reference).shape;

    if (like.rank() != in.rank())
        return fail(op,
                    "the reference " + like.toString() + " needs the rank of "
                        + in.toString());

    for (auto axis = 0; axis < in.rank(); ++axis)
    {
        auto fits = like[axis] == Shape::unknown
                    || (in[axis] != Shape::unknown && like[axis] <= in[axis]);

        if (!fits)
            return fail(op,
                        "axis " + std::to_string(axis) + " of " + in.toString()
                            + " cannot be cut to " + like.toString());
    }

    if (x == reference)
        return x;

    auto begin = Vector<int> {};
    begin.resize(in.rank(), 0);

    if (like.isFixed())
        return slice(x, begin, like.dims);

    auto extents = addOperation("shape",
                                {like.rank()},
                                MIL::DataType::int32,
                                {tensorParameter("x", reference)});

    return addOperation(op,
                        like,
                        node(x).type,
                        {tensorParameter("x", x),
                         valueParameter("begin", MIL::Value::ints(toInt32s(begin))),
                         tensorParameter("end", extents)});
}

Tensor Graph::scaledDotProductAttention(Tensor q, Tensor k, Tensor v, bool causal)
{
    auto op = "scaled_dot_product_attention";

    if (!acceptOperands(op, {q, k, v}))
        return {};

    auto queries = node(q).shape;
    auto keys = node(k).shape;
    auto values = node(v).shape;
    auto rank = queries.rank();

    if (rank < 3 || keys.rank() != rank || values.rank() != rank)
        return fail(op,
                    "needs q, k and v of one rank >= 3, not " + queries.toString()
                        + ", " + keys.toString() + ", " + values.toString());

    if (!isFloat(q) || node(k).type != node(q).type || node(v).type != node(q).type)
        return fail(op, "q, k and v need one floating-point type");

    for (auto axis = 0; axis < rank - 2; ++axis)
        if (queries[axis] != keys[axis] || queries[axis] != values[axis])
            return fail(op, "q, k and v need the same batch dimensions");

    if (queries[rank - 1] != keys[rank - 1] || keys[rank - 2] != values[rank - 2])
        return fail(op,
                    "q " + queries.toString() + ", k " + keys.toString() + " and v "
                        + values.toString() + " do not fit");

    auto parameters = Vector<Parameter> {tensorParameter("query", q),
                                         tensorParameter("key", k),
                                         tensorParameter("value", v)};

    if (causal)
    {
        auto rows = queries[rank - 2];
        auto columns = keys[rank - 2];

        if (rows == Shape::unknown || columns == Shape::unknown)
            return fail(op, "a causal mask needs fixed query and key lengths");

        auto type = node(q).type;
        auto allowed = causalMaskBytes(rows, columns, type);
        auto allowedTensor = addBlobConstant("", {rows, columns}, type, allowed);
        auto mask =
            addOperation("greater",
                         {rows, columns},
                         MIL::DataType::boolean,
                         {tensorParameter("x", allowedTensor),
                          valueParameter("y", MIL::Value::scalar(0.5f, type))});
        parameters.add(tensorParameter("attn_mask", mask));
    }

    auto result = queries;
    result.dims[rank - 1] = values[rank - 1];
    needsCoreML8 = true;

    return addOperation(op, result, node(q).type, parameters);
}

Tensor Graph::gelu(Tensor x)
{
    if (!acceptOperands("gelu", {x}))
        return {};

    if (!isFloat(x))
        return fail("gelu", "needs a floating-point tensor");

    return addOperation("gelu",
                        node(x).shape,
                        node(x).type,
                        {tensorParameter("x", x),
                         valueParameter("mode", MIL::Value::string("EXACT"))});
}

Tensor Graph::cast(Tensor x, DType type)
{
    if (!acceptOperands("cast", {x}))
        return {};

    if (node(x).type == toMIL(type))
        return x;

    return addOperation(
        "cast",
        node(x).shape,
        toMIL(type),
        {tensorParameter("x", x),
         valueParameter("dtype", MIL::Value::string(toString(type)))});
}

const Shape& Graph::shape(Tensor value) const
{
    static const auto none = Shape {};
    return isKnown(value) ? node(value).shape : none;
}

DType Graph::type(Tensor value) const
{
    return isKnown(value) ? fromMIL(node(value).type) : DType::float32;
}
} // namespace eacp::ML
