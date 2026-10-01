#include "Graph.h"

namespace eacp::ML
{
namespace
{
Blob::DataType blobDataTypeOf(MIL::DataType type)
{
    switch (type)
    {
        case MIL::DataType::float16:
            return Blob::DataType::float16;
        case MIL::DataType::int32:
            return Blob::DataType::int32;
        default:
            return Blob::DataType::float32;
    }
}

MIL::ArrayDataType arrayDataTypeOf(MIL::DataType type)
{
    switch (type)
    {
        case MIL::DataType::float16:
            return MIL::ArrayDataType::float16;
        case MIL::DataType::int32:
            return MIL::ArrayDataType::int32;
        default:
            return MIL::ArrayDataType::float32;
    }
}

Vector<std::int64_t> dimensionsOf(const Shape& shape)
{
    auto dimensions = Vector<std::int64_t> {};

    for (auto size: shape.dims)
        dimensions.add(size == Shape::unknown ? MIL::unknownDimension : size);

    return dimensions;
}

MIL::Operation namedOperation(std::string_view type,
                              const std::string& name,
                              const MIL::TensorType& outputType)
{
    auto operation = MIL::Operation {};
    operation.type = type;
    operation.outputs.add({name, outputType});
    operation.attributes.add({"name", MIL::Value::string(name)});
    return operation;
}

MIL::Operation constOperation(const std::string& name, const MIL::Value& value)
{
    auto operation = namedOperation("const", name, value.type);
    operation.attributes.add({"val", value});
    return operation;
}
} // namespace

class Graph::Lowering
{
public:
    explicit Lowering(const Graph& graphToUse)
        : graph(graphToUse)
    {
        auto count = graph.nodes.size();
        names.resize(count);
        live.resize(count, 0);
        blobOffsets.resize(count, 0);
        identityOutputs.resize(graph.outputs.size(), 0);
    }

    MIL::Specification run()
    {
        markLive();
        reserveUserNames();
        writeLiveConstants();
        nameOutputs();
        nameRemainingNodes();

        auto specification = MIL::Specification {};
        specification.specificationVersion = graph.needsCoreML8 ? 9 : 8;
        specification.program.main.opset =
            graph.needsCoreML8 ? "CoreML8" : "CoreML7";

        describeInputs(specification);
        emitOperations(specification.program.main.block);
        emitOutputs(specification);
        return specification;
    }

    const Blob::Writer& weights() const { return blob; }

private:
    void reserveUserNames()
    {
        for (auto index = 0; index < graph.nodes.size(); ++index)
        {
            auto& node = graph.nodes[index];

            if (node.kind == NodeKind::operation || node.name.empty()
                || isUnusedConstant(index))
                continue;

            names[index] = node.name;
            taken.add(node.name);
        }

        for (auto& binding: graph.outputs)
            taken.add(binding.name);
    }

    void markLive()
    {
        for (auto& binding: graph.outputs)
            live[binding.tensor] = 1;

        for (auto index = graph.nodes.size() - 1; index >= 0; --index)
        {
            if (live[index] == 0)
                continue;

            for (auto& parameter: graph.nodes[index].parameters)
                for (auto tensor: parameter.tensors)
                    live[tensor] = 1;
        }
    }

    void writeLiveConstants()
    {
        for (auto index = 0; index < graph.nodes.size(); ++index)
        {
            auto& node = graph.nodes[index];

            if (node.kind == NodeKind::constant && live[index] != 0)
                blobOffsets[index] =
                    blob.append(blobDataTypeOf(node.type), node.bytes);
        }
    }

    void nameOutputs()
    {
        for (auto index = 0; index < graph.outputs.size(); ++index)
        {
            auto& binding = graph.outputs[index];
            auto& node = graph.nodes[binding.tensor];

            if (node.kind == NodeKind::operation && names[binding.tensor].empty())
                names[binding.tensor] = binding.name;
            else
                identityOutputs[index] = 1;
        }
    }

    bool isUnusedConstant(int index) const
    {
        return graph.nodes[index].kind == NodeKind::constant && live[index] == 0;
    }

    void nameRemainingNodes()
    {
        auto ordinal = 0;

        for (auto index = 0; index < graph.nodes.size(); ++index)
        {
            if (isUnusedConstant(index))
                continue;

            auto number = ordinal++;

            if (!names[index].empty() || live[index] == 0)
                continue;

            auto& node = graph.nodes[index];
            auto base = node.kind == NodeKind::operation ? node.op
                        : node.kind == NodeKind::scalar  ? std::string {"scalar"}
                                                         : std::string {"const"};

            names[index] = uniqueName(base + "_" + std::to_string(number));
        }
    }

    std::string uniqueName(const std::string& base)
    {
        auto name = base;

        for (auto suffix = 1; taken.contains(name); ++suffix)
            name = base + "_" + std::to_string(suffix);

        taken.add(name);
        return name;
    }

    MIL::TensorType typeOf(int index) const
    {
        auto& node = graph.nodes[index];
        return {node.type, dimensionsOf(node.shape)};
    }

    void describeInputs(MIL::Specification& specification) const
    {
        for (auto index = 0; index < graph.nodes.size(); ++index)
        {
            auto& node = graph.nodes[index];

            if (node.kind != NodeKind::input)
                continue;

            specification.program.main.inputs.add({node.name, typeOf(index)});

            auto feature = MIL::ArrayFeature {};
            feature.name = node.name;
            feature.shape = dimensionsOf(node.enumeratedShapes[0]);
            feature.dataType = arrayDataTypeOf(node.type);

            if (node.enumeratedShapes.size() > 1)
                for (auto& shape: node.enumeratedShapes)
                    feature.enumeratedShapes.add(dimensionsOf(shape));

            specification.description.inputs.add(feature);
        }
    }

    void emitOperations(MIL::Block& block)
    {
        for (auto index = 0; index < graph.nodes.size(); ++index)
        {
            if (live[index] == 0)
                continue;

            auto& node = graph.nodes[index];

            switch (node.kind)
            {
                case NodeKind::input:
                    break;
                case NodeKind::constant:
                    block.operations.add(constOperation(
                        names[index],
                        MIL::Value::blob(typeOf(index), blobOffsets[index])));
                    break;
                case NodeKind::scalar:
                    block.operations.add(constOperation(
                        names[index],
                        MIL::Value::scalar(node.value, MIL::DataType::float32)));
                    break;
                case NodeKind::operation:
                    emitOperation(block, index);
                    break;
            }
        }
    }

    void emitOperation(MIL::Block& block, int index)
    {
        auto& node = graph.nodes[index];
        auto operation = namedOperation(node.op, names[index], typeOf(index));

        for (auto& parameter: node.parameters)
        {
            auto input = MIL::Input {};
            input.parameter = parameter.name;

            if (parameter.immediate)
            {
                auto constName = uniqueName(names[index] + "_" + parameter.name);
                block.operations.add(
                    constOperation(constName, *parameter.immediate));
                input.arguments.add(constName);
            }

            for (auto tensor: parameter.tensors)
                input.arguments.add(names[tensor]);

            operation.inputs.add(input);
        }

        block.operations.add(operation);
    }

    void emitOutputs(MIL::Specification& specification)
    {
        auto& block = specification.program.main.block;

        for (auto index = 0; index < graph.outputs.size(); ++index)
        {
            auto& binding = graph.outputs[index];
            auto type = typeOf(binding.tensor);

            if (identityOutputs[index] != 0)
            {
                auto identity = namedOperation("identity", binding.name, type);
                identity.inputs.add({"x", {names[binding.tensor]}});
                block.operations.add(identity);
            }

            block.outputs.add(binding.name);

            auto feature = MIL::ArrayFeature {};
            feature.name = binding.name;
            feature.dataType = arrayDataTypeOf(type.dataType);

            if (graph.nodes[binding.tensor].shape.isFixed())
                feature.shape = type.dimensions;

            specification.description.outputs.add(feature);
        }
    }

    const Graph& graph;
    Vector<std::string> names;
    Vector<std::uint8_t> live;
    Vector<std::uint8_t> identityOutputs;
    Vector<std::uint64_t> blobOffsets;
    Vector<std::string> taken;
    Blob::Writer blob;
};

MIL::Specification Graph::specification() const
{
    return Lowering {*this}.run();
}

std::string Graph::toText() const
{
    if (!isValid())
        return "invalid graph: " + errorList[0] + "\n";

    if (outputs.empty())
        return "invalid graph: no outputs\n";

    return MIL::toText(specification().program);
}

Package Graph::build() const
{
    if (!isValid() || outputs.empty())
        return {};

    auto lowering = Lowering {*this};
    auto package = Package {};
    package.model = MIL::serialize(lowering.run());
    package.manifest = Package::standardManifest();

    if (lowering.weights().count() > 0)
        package.weights = lowering.weights().bytes();

    return package;
}
} // namespace eacp::ML
