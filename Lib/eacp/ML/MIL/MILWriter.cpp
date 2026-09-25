#include "MILWriter.h"
#include "Half.h"

#include <cstdio>

namespace eacp::ML::MIL
{
namespace
{
using Protobuf::Writer;

Writer encodeDimension(std::int64_t size)
{
    auto dimension = Writer {};

    if (size == unknownDimension)
    {
        dimension.messageField(2, Writer {});
        return dimension;
    }

    auto constant = Writer {};
    constant.uint64Field(1, static_cast<std::uint64_t>(size));
    dimension.messageField(1, constant);
    return dimension;
}

Writer encodeValueType(const TensorType& type)
{
    auto valueType = Writer {};
    valueType.messageField(1, encode(type));
    return valueType;
}

struct TensorValueEncoder
{
    Writer operator()(const Floats& floats) const
    {
        return wrap(1, packed(floats.values));
    }

    Writer operator()(const Ints& ints) const
    {
        auto repeated = Writer {};
        repeated.packedInt32(1, ints.values);
        return wrap(2, repeated);
    }

    Writer operator()(const Bools& bools) const
    {
        auto repeated = Writer {};
        repeated.packedBool(1, bools.values);
        return wrap(3, repeated);
    }

    Writer operator()(const StringList& strings) const
    {
        auto repeated = Writer {};

        for (auto& text: strings.values)
            repeated.stringField(1, text);

        return wrap(4, repeated);
    }

    Writer operator()(const RawBytes& raw) const
    {
        auto repeated = Writer {};
        repeated.bytesField(1, raw.values);
        return wrap(7, repeated);
    }

    static Writer packed(Span<const float> values)
    {
        auto repeated = Writer {};
        repeated.packedFloat(1, values);
        return repeated;
    }

    static Writer wrap(int field, const Writer& repeated)
    {
        auto tensor = Writer {};
        tensor.messageField(field, repeated);
        return tensor;
    }
};

Writer encodeImmediate(const ImmediateValue& immediate)
{
    auto tensor = std::visit(TensorValueEncoder {}, immediate);
    auto encoded = Writer {};
    encoded.messageField(1, tensor);
    return encoded;
}

Writer encodeBlobFile(const BlobFileValue& blob)
{
    auto encoded = Writer {};
    encoded.stringField(1, blob.fileName);
    encoded.uint64Field(2, blob.offset);
    return encoded;
}

Writer encodeArgument(const Vector<std::string>& names)
{
    auto argument = Writer {};

    for (auto& name: names)
    {
        auto binding = Writer {};
        binding.stringField(1, name);
        argument.messageField(1, binding);
    }

    return argument;
}

Writer encodeShape(const Vector<std::int64_t>& shape)
{
    auto encoded = Writer {};
    encoded.packedInt64(1, shape);
    return encoded;
}

std::string numberText(double number)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%g", number);
    return text;
}

template <typename T, typename Format>
std::string listText(const Vector<T>& values, const Format& format, bool isScalar)
{
    if (isScalar && values.size() == 1)
        return format(values[0]);

    auto text = std::string {"["};

    for (auto index = 0; index < values.size(); ++index)
    {
        if (index > 0)
            text += ", ";

        text += format(values[index]);
    }

    return text + "]";
}

Vector<float> halvesToFloats(const Bytes& raw)
{
    auto floats = Vector<float> {};

    for (auto index = 0; index + 1 < raw.size(); index += 2)
    {
        auto bits = static_cast<std::uint16_t>(raw[index] | (raw[index + 1] << 8));
        floats.add(halfToFloat(bits));
    }

    return floats;
}

struct ImmediateText
{
    std::string operator()(const Floats& floats) const
    {
        auto format = [](float value) { return numberText(value); };
        return listText(floats.values, format, isScalar);
    }

    std::string operator()(const Ints& ints) const
    {
        auto format = [](std::int32_t value) { return std::to_string(value); };
        return listText(ints.values, format, isScalar);
    }

    std::string operator()(const Bools& bools) const
    {
        auto format = [](std::uint8_t value)
        { return std::string {value != 0 ? "true" : "false"}; };
        return listText(bools.values, format, isScalar);
    }

    std::string operator()(const StringList& strings) const
    {
        auto format = [](const std::string& value) { return "\"" + value + "\""; };
        return listText(strings.values, format, isScalar);
    }

    std::string operator()(const RawBytes& raw) const
    {
        auto format = [](float value) { return numberText(value); };
        return listText(halvesToFloats(raw.values), format, isScalar);
    }

    bool isScalar = true;
};

struct ValueText
{
    std::string operator()(const ImmediateValue& immediate) const
    {
        return std::visit(ImmediateText {isScalar}, immediate);
    }

    std::string operator()(const BlobFileValue& blob) const
    {
        return "blob(" + std::to_string(blob.offset) + ")";
    }

    bool isScalar = true;
};

std::string joinedNames(const Vector<std::string>& names)
{
    auto text = std::string {"("};

    for (auto index = 0; index < names.size(); ++index)
    {
        if (index > 0)
            text += ", ";

        text += names[index];
    }

    return text + ")";
}

std::string argumentText(const Vector<std::string>& names)
{
    return names.size() == 1 ? names[0] : joinedNames(names);
}

const Value* findAttribute(const Operation& operation, std::string_view name)
{
    for (auto& attribute: operation.attributes)
        if (attribute.name == name)
            return &attribute.value;

    return nullptr;
}

std::string operationText(const Operation& operation)
{
    auto text = std::string {"    "};

    for (auto index = 0; index < operation.outputs.size(); ++index)
    {
        auto& output = operation.outputs[index];
        text += (index > 0 ? ", " : "") + toText(output.type) + " " + output.name;
    }

    text += " = " + operation.type + "(";

    for (auto index = 0; index < operation.inputs.size(); ++index)
    {
        auto& input = operation.inputs[index];
        text += (index > 0 ? ", " : "") + input.parameter + " = "
                + argumentText(input.arguments);
    }

    text += ")";

    if (auto* value = findAttribute(operation, "val"))
        text += "[val = " + toText(*value) + "]";

    return text + ";\n";
}

TensorType scalarType(DataType type)
{
    return {type, {}};
}

TensorType vectorType(DataType type, int count)
{
    auto tensorType = TensorType {type, {}};
    tensorType.dimensions.add(count);
    return tensorType;
}
} // namespace

int sizeOf(DataType type)
{
    switch (type)
    {
        case DataType::boolean:
            return 1;
        case DataType::float16:
            return 2;
        case DataType::float32:
        case DataType::int32:
            return 4;
        case DataType::string:
            return 0;
    }

    return 0;
}

std::string toText(DataType type)
{
    switch (type)
    {
        case DataType::boolean:
            return "bool";
        case DataType::string:
            return "string";
        case DataType::float16:
            return "fp16";
        case DataType::float32:
            return "fp32";
        case DataType::int32:
            return "int32";
    }

    return "?";
}

Value Value::scalar(float number, DataType type)
{
    if (type == DataType::float16)
    {
        auto raw = RawBytes {};
        appendHalf(raw.values, number);
        return {scalarType(type), ImmediateValue {raw}};
    }

    auto floats = Floats {};
    floats.values.add(number);
    return {scalarType(DataType::float32), ImmediateValue {floats}};
}

Value Value::scalar(std::int32_t number)
{
    auto ints = Ints {};
    ints.values.add(number);
    return {scalarType(DataType::int32), ImmediateValue {ints}};
}

Value Value::scalar(bool flag)
{
    auto bools = Bools {};
    bools.values.add(flag ? 1 : 0);
    return {scalarType(DataType::boolean), ImmediateValue {bools}};
}

Value Value::string(std::string_view text)
{
    auto strings = StringList {};
    strings.values.add(std::string {text});
    return {scalarType(DataType::string), ImmediateValue {strings}};
}

Value Value::ints(const Vector<std::int32_t>& numbers)
{
    return {vectorType(DataType::int32, numbers.size()),
            ImmediateValue {Ints {numbers}}};
}

Value Value::bools(const Vector<std::uint8_t>& flags)
{
    return {vectorType(DataType::boolean, flags.size()),
            ImmediateValue {Bools {flags}}};
}

Value Value::blob(const TensorType& type, std::uint64_t offset)
{
    auto file = BlobFileValue {};
    file.offset = offset;
    return {type, file};
}

Writer encode(const TensorType& type)
{
    auto tensor = Writer {};
    tensor.int64Field(1, static_cast<std::int64_t>(type.dataType));
    tensor.int64Field(2, type.dimensions.size());

    for (auto size: type.dimensions)
        tensor.messageField(3, encodeDimension(size));

    return tensor;
}

Writer encode(const Value& value)
{
    auto encoded = Writer {};
    encoded.messageField(2, encodeValueType(value.type));

    if (auto* immediate = std::get_if<ImmediateValue>(&value.value))
        encoded.messageField(3, encodeImmediate(*immediate));
    else
        encoded.messageField(5,
                             encodeBlobFile(std::get<BlobFileValue>(value.value)));

    return encoded;
}

Writer encode(const NamedValueType& named)
{
    auto encoded = Writer {};
    encoded.stringField(1, named.name);
    encoded.messageField(2, encodeValueType(named.type));
    return encoded;
}

Writer encode(const Operation& operation)
{
    auto encoded = Writer {};
    encoded.stringField(1, operation.type);

    for (auto& input: operation.inputs)
        encoded.mapEntry(2, input.parameter, encodeArgument(input.arguments));

    for (auto& output: operation.outputs)
        encoded.messageField(3, encode(output));

    for (auto& attribute: operation.attributes)
        encoded.mapEntry(5, attribute.name, encode(attribute.value));

    return encoded;
}

Writer encode(const Block& block)
{
    auto encoded = Writer {};

    for (auto& output: block.outputs)
        encoded.stringField(2, output);

    for (auto& operation: block.operations)
        encoded.messageField(3, encode(operation));

    return encoded;
}

Writer encode(const Function& function)
{
    auto encoded = Writer {};

    for (auto& input: function.inputs)
        encoded.messageField(1, encode(input));

    encoded.stringField(2, function.opset);
    encoded.mapEntry(3, function.opset, encode(function.block));
    return encoded;
}

Writer encode(const Program& program)
{
    auto encoded = Writer {};
    encoded.int64Field(1, program.version);
    encoded.mapEntry(2, "main", encode(program.main));
    return encoded;
}

Writer encode(const ArrayFeature& feature)
{
    auto array = Writer {};
    array.packedInt64(1, feature.shape);
    array.int64Field(2, static_cast<std::int64_t>(feature.dataType));

    if (!feature.enumeratedShapes.empty())
    {
        auto enumerated = Writer {};

        for (auto& shape: feature.enumeratedShapes)
            enumerated.messageField(1, encodeShape(shape));

        array.messageField(21, enumerated);
    }

    auto featureType = Writer {};
    featureType.messageField(5, array);

    auto encoded = Writer {};
    encoded.stringField(1, feature.name);
    encoded.messageField(3, featureType);
    return encoded;
}

Writer encode(const Description& description)
{
    auto encoded = Writer {};

    for (auto& feature: description.inputs)
        encoded.messageField(1, encode(feature));

    for (auto& feature: description.outputs)
        encoded.messageField(10, encode(feature));

    return encoded;
}

Writer encode(const Specification& specification)
{
    auto encoded = Writer {};
    encoded.int64Field(1, specification.specificationVersion);
    encoded.messageField(2, encode(specification.description));
    encoded.messageField(502, encode(specification.program));
    return encoded;
}

Bytes serialize(const Specification& specification)
{
    return encode(specification).bytes();
}

std::string toText(const TensorType& type)
{
    auto text = "tensor<" + toText(type.dataType) + ", [";

    for (auto index = 0; index < type.dimensions.size(); ++index)
    {
        auto size = type.dimensions[index];
        text += index > 0 ? ", " : "";
        text += size == unknownDimension ? std::string {"?"} : std::to_string(size);
    }

    return text + "]>";
}

std::string toText(const Value& value)
{
    return std::visit(ValueText {value.type.dimensions.empty()}, value.value);
}

std::string toText(const Program& program)
{
    auto& function = program.main;
    auto text = "program(" + std::to_string(program.version) + ")\nfunc main<"
                + function.opset + ">(";

    for (auto index = 0; index < function.inputs.size(); ++index)
    {
        auto& input = function.inputs[index];
        text += (index > 0 ? ", " : "") + toText(input.type) + " " + input.name;
    }

    text += ") {\n";

    for (auto& operation: function.block.operations)
        text += operationText(operation);

    return text + "} -> " + joinedNames(function.block.outputs) + ";\n";
}
} // namespace eacp::ML::MIL
