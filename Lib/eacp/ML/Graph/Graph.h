#pragma once

#include "Tensor.h"

#include "../MIL/Blob.h"
#include "../MIL/Half.h"
#include "../MIL/MILWriter.h"
#include "../MIL/Package.h"

#include <concepts>
#include <optional>
#include <string_view>

namespace eacp::ML
{
using ElementwiseBody = std::function<GPU::Float(const Vector<GPU::Float>&)>;

// A tensor program recorded op by op and lowered to a Core ML ML Program.
//
// Misuse - a rank or dimension that does not fit, an axis out of range, a
// duplicate name, an elementwise body using something MIL cannot express - is
// recorded rather than thrown: the op returns an invalid Tensor, ops given an
// invalid Tensor return one without adding a second error, errors() says what
// went wrong first, and build() of a graph with errors is an empty Package.
class Graph
{
public:
    Tensor input(std::string_view name, const Shape& shape, DType type);

    Tensor input(std::string_view name,
                 const Shape& defaultShape,
                 const Vector<Shape>& enumeratedShapes,
                 DType type);

    void output(Tensor value, std::string_view name);

    // Goes into the weight blob if it reaches an output. Input and output names
    // must be identifiers, since the runner addresses features by them; a
    // constant's name is made one, every character outside [A-Za-z0-9_]
    // becoming '_'.
    Tensor constant(std::string_view name,
                    const Shape& shape,
                    DType type,
                    Span<const std::uint8_t> bytes);

    Tensor halfConstant(std::string_view name,
                        const Shape& shape,
                        Span<const float> values);

    Tensor scalar(float value);

    Tensor linear(Tensor x, Tensor weight, Tensor bias);
    Tensor
        matmul(Tensor a, Tensor b, bool transposeA = false, bool transposeB = false);
    Tensor transpose(Tensor x, const Vector<int>& perm);
    Tensor reshape(Tensor x, const Shape& shape);
    Tensor softmax(Tensor x, int axis);
    Tensor sum(Tensor x, int axis, bool keepDims = false);
    Tensor max(Tensor x, int axis, bool keepDims = false);
    Tensor layerNorm(Tensor x,
                     const Vector<int>& axes,
                     Tensor gamma,
                     Tensor beta,
                     float epsilon = 1e-5f);
    Tensor conv(Tensor x, Tensor weight, Tensor bias, int stride, int padding);
    Tensor gather(Tensor table, Tensor indices, int axis);
    Tensor concat(const Vector<Tensor>& parts, int axis);
    Tensor slice(Tensor x, const Vector<int>& begin, const Vector<int>& end);
    Tensor scaledDotProductAttention(Tensor q, Tensor k, Tensor v, bool causal);
    Tensor gelu(Tensor x);
    Tensor cast(Tensor x, DType type);

    Tensor apply(const Vector<Tensor>& operands, const ElementwiseBody& body);

    template <typename Body>
        requires std::invocable<const Body&, const GPU::Float&>
    Tensor apply(Tensor x, const Body& body)
    {
        auto unpacked = [&body](const Vector<GPU::Float>& values) -> GPU::Float
        { return body(values[0]); };

        return apply(Vector<Tensor> {x}, unpacked);
    }

    template <typename Body>
        requires std::invocable<const Body&, const GPU::Float&, const GPU::Float&>
    Tensor apply(Tensor a, Tensor b, const Body& body)
    {
        auto unpacked = [&body](const Vector<GPU::Float>& values) -> GPU::Float
        { return body(values[0], values[1]); };

        return apply(Vector<Tensor> {a, b}, unpacked);
    }

    template <typename Body>
        requires std::invocable<const Body&,
                                const GPU::Float&,
                                const GPU::Float&,
                                const GPU::Float&>
    Tensor apply(Tensor a, Tensor b, Tensor c, const Body& body)
    {
        auto unpacked = [&body](const Vector<GPU::Float>& values) -> GPU::Float
        { return body(values[0], values[1], values[2]); };

        return apply(Vector<Tensor> {a, b, c}, unpacked);
    }

    const Shape& shape(Tensor value) const;
    DType type(Tensor value) const;

    bool isValid() const { return errorList.empty(); }
    const Vector<std::string>& errors() const { return errorList; }

    MIL::Specification specification() const;
    std::string toText() const;
    Package build() const;

private:
    class ElementwiseLowering;
    class Lowering;

    enum class NodeKind
    {
        input,
        constant,
        scalar,
        operation
    };

    struct Parameter
    {
        std::string name;
        Vector<int> tensors;
        std::optional<MIL::Value> immediate;
    };

    struct Node
    {
        NodeKind kind = NodeKind::operation;
        std::string name;
        Shape shape;
        MIL::DataType type = MIL::DataType::float32;
        Vector<Shape> enumeratedShapes;
        Bytes bytes;
        float value = 0.f;
        std::string op;
        Vector<Parameter> parameters;
    };

    struct OutputBinding
    {
        int tensor = -1;
        std::string name;
    };

    Tensor fail(std::string_view op, const std::string& message);
    bool acceptOperands(std::string_view op, std::initializer_list<Tensor> values);
    bool isKnown(Tensor value) const;
    const Node& node(Tensor value) const;
    bool isNameTaken(std::string_view name) const;
    bool isFloat(Tensor value) const;
    std::optional<int> normalizedAxis(int axis, int rank) const;

    Tensor addNode(const Node& newNode);
    Tensor addOperation(std::string_view op,
                        const Shape& shape,
                        MIL::DataType type,
                        const Vector<Parameter>& parameters);
    Tensor addBlobConstant(std::string_view name,
                           const Shape& shape,
                           MIL::DataType type,
                           Span<const std::uint8_t> bytes);

    static Parameter tensorParameter(std::string_view name, Tensor value);
    static Parameter valueParameter(std::string_view name, const MIL::Value& value);

    std::optional<Shape> broadcastShape(const Shape& a, const Shape& b) const;
    Tensor reduce(std::string_view op, Tensor x, int axis, bool keepDims);

    Vector<Node> nodes;
    Vector<OutputBinding> outputs;
    Vector<std::string> errorList;
    GPU::ShaderBuilder builder;
    bool needsCoreML8 = false;
};
} // namespace eacp::ML
