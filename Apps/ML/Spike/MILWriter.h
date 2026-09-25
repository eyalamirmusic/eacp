#pragma once

// A hand-rolled writer for Core ML's ML Program format: the protobuf wire
// encoding, the slice of Model.proto / MIL.proto a matmul- or linear-and-softmax
// program needs, the MILBlob weight file and the .mlpackage directory around them.

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace MILWriter
{
static_assert(std::endian::native == std::endian::little);

using Bytes = std::vector<uint8_t>;

inline uint16_t floatToHalf(float value)
{
    auto bits = std::bit_cast<uint32_t>(value);
    auto sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
    auto exponent = static_cast<int>((bits >> 23) & 0xffu);
    auto mantissa = bits & 0x7fffffu;

    if (exponent == 0xff)
        return static_cast<uint16_t>(sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0));

    auto halfExponent = exponent - 127 + 15;

    if (halfExponent >= 0x1f)
        return static_cast<uint16_t>(sign | 0x7c00u);

    if (halfExponent <= 0)
    {
        if (halfExponent < -10)
            return sign;

        auto full = mantissa | 0x800000u;
        auto shift = static_cast<uint32_t>(14 - halfExponent);
        auto half = full >> shift;
        auto remainder = full & ((1u << shift) - 1);
        auto halfway = 1u << (shift - 1);

        if (remainder > halfway || (remainder == halfway && (half & 1u)))
            ++half;

        return static_cast<uint16_t>(sign | half);
    }

    auto half = static_cast<uint32_t>(halfExponent << 10) | (mantissa >> 13);
    auto remainder = mantissa & 0x1fffu;

    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u)))
        ++half;

    return static_cast<uint16_t>(sign | half);
}

inline float halfToFloat(uint16_t value)
{
    auto sign = (value & 0x8000u) != 0 ? -1.f : 1.f;
    auto exponent = (value >> 10) & 0x1f;
    auto mantissa = value & 0x3ff;

    if (exponent == 0)
        return sign * std::ldexp(static_cast<float>(mantissa), -24);

    if (exponent == 0x1f)
        return mantissa != 0 ? NAN : sign * INFINITY;

    return sign * std::ldexp(static_cast<float>(mantissa | 0x400), exponent - 25);
}

template <typename T>
std::span<const uint8_t> asBytes(std::span<const T> values)
{
    return {reinterpret_cast<const uint8_t*>(values.data()), values.size_bytes()};
}

namespace Proto
{
enum class WireType : uint8_t
{
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    Fixed32 = 5
};

class Encoder
{
public:
    void varint(uint64_t value)
    {
        while (value >= 0x80)
        {
            out.push_back(static_cast<uint8_t>(value | 0x80));
            value >>= 7;
        }

        out.push_back(static_cast<uint8_t>(value));
    }

    void tag(int field, WireType type)
    {
        varint((static_cast<uint64_t>(field) << 3) | static_cast<uint64_t>(type));
    }

    void uint64Field(int field, uint64_t value)
    {
        if (value == 0)
            return;

        tag(field, WireType::Varint);
        varint(value);
    }

    void int64Field(int field, int64_t value)
    {
        uint64Field(field, static_cast<uint64_t>(value));
    }

    void boolField(int field, bool value) { uint64Field(field, value ? 1 : 0); }

    void bytesField(int field, std::span<const uint8_t> value)
    {
        tag(field, WireType::LengthDelimited);
        varint(value.size());
        out.insert(out.end(), value.begin(), value.end());
    }

    void stringField(int field, std::string_view value)
    {
        bytesField(field,
                   {reinterpret_cast<const uint8_t*>(value.data()), value.size()});
    }

    void messageField(int field, const Encoder& message)
    {
        bytesField(field, message.bytes());
    }

    void mapEntry(int field, std::string_view key, const Encoder& value)
    {
        auto entry = Encoder {};
        entry.stringField(1, key);
        entry.messageField(2, value);
        messageField(field, entry);
    }

    void packedInt64(int field, std::span<const int64_t> values)
    {
        auto packed = Encoder {};

        for (auto value: values)
            packed.varint(static_cast<uint64_t>(value));

        packedField(field, packed);
    }

    void packedInt32(int field, std::span<const int32_t> values)
    {
        auto packed = Encoder {};

        for (auto value: values)
            packed.varint(static_cast<uint64_t>(static_cast<int64_t>(value)));

        packedField(field, packed);
    }

    void packedBool(int field, const std::vector<bool>& values)
    {
        auto packed = Encoder {};

        for (auto value: values)
            packed.varint(value ? 1 : 0);

        packedField(field, packed);
    }

    void packedFloat(int field, std::span<const float> values)
    {
        auto packed = Encoder {};
        auto raw = asBytes(values);
        packed.out.assign(raw.begin(), raw.end());
        packedField(field, packed);
    }

    const Bytes& bytes() const { return out; }

private:
    void packedField(int field, const Encoder& packed)
    {
        if (!packed.out.empty())
            messageField(field, packed);
    }

    Bytes out;
};
} // namespace Proto

namespace MIL
{
using Proto::Encoder;

enum class DataType
{
    Bool = 1,
    String = 2,
    Float16 = 10,
    Float32 = 11,
    Int32 = 23
};

inline uint64_t sizeOf(DataType type)
{
    switch (type)
    {
        case DataType::Bool:
            return 1;
        case DataType::Float16:
            return 2;
        case DataType::Float32:
        case DataType::Int32:
            return 4;
        default:
            throw std::invalid_argument("MILWriter: type has no fixed size");
    }
}

struct Dimension
{
    std::optional<uint64_t> size;

    Encoder encode() const
    {
        auto dimension = Encoder {};

        if (size)
        {
            auto constant = Encoder {};
            constant.uint64Field(1, *size);
            dimension.messageField(1, constant);
        }
        else
        {
            dimension.messageField(2, Encoder {});
        }

        return dimension;
    }
};

struct TensorType
{
    DataType dataType = DataType::Float32;
    std::vector<Dimension> dimensions;

    Encoder encode() const
    {
        auto tensor = Encoder {};
        tensor.int64Field(1, static_cast<int64_t>(dataType));
        tensor.int64Field(2, static_cast<int64_t>(dimensions.size()));

        for (auto& dimension: dimensions)
            tensor.messageField(3, dimension.encode());

        return tensor;
    }

    uint64_t elementCount() const
    {
        auto count = uint64_t {1};

        for (auto& dimension: dimensions)
            count *= dimension.size.value();

        return count;
    }

    bool isFixed() const
    {
        for (auto& dimension: dimensions)
            if (!dimension.size)
                return false;

        return true;
    }
};

struct ValueType
{
    TensorType tensorType;

    Encoder encode() const
    {
        auto type = Encoder {};
        type.messageField(1, tensorType.encode());
        return type;
    }
};

struct TensorValue
{
    std::variant<std::vector<float>,
                 std::vector<int32_t>,
                 std::vector<bool>,
                 std::vector<std::string>,
                 Bytes>
        value;

    Encoder encode() const
    {
        auto repeated = Encoder {};
        auto field = static_cast<int>(value.index()) + 1;

        if (auto* floats = std::get_if<0>(&value))
            repeated.packedFloat(1, *floats);
        else if (auto* ints = std::get_if<1>(&value))
            repeated.packedInt32(1, *ints);
        else if (auto* bools = std::get_if<2>(&value))
            repeated.packedBool(1, *bools);
        else if (auto* strings = std::get_if<3>(&value))
            for (auto& string: *strings)
                repeated.stringField(1, string);
        else
        {
            field = 7;
            repeated.bytesField(1, std::get<4>(value));
        }

        auto tensor = Encoder {};
        tensor.messageField(field, repeated);
        return tensor;
    }
};

struct BlobFileValue
{
    std::string fileName = "@model_path/weights/weight.bin";
    uint64_t offset = 0;

    Encoder encode() const
    {
        auto blob = Encoder {};
        blob.stringField(1, fileName);
        blob.uint64Field(2, offset);
        return blob;
    }
};

struct Value
{
    ValueType type;
    std::variant<TensorValue, BlobFileValue> value;

    Encoder encode() const
    {
        auto encoded = Encoder {};
        encoded.messageField(2, type.encode());

        if (auto* tensor = std::get_if<TensorValue>(&value))
        {
            auto immediate = Encoder {};
            immediate.messageField(1, tensor->encode());
            encoded.messageField(3, immediate);
        }
        else
        {
            encoded.messageField(5, std::get<BlobFileValue>(value).encode());
        }

        return encoded;
    }

    static Value scalar(DataType dataType, TensorValue tensor)
    {
        return {{{dataType, {}}}, std::move(tensor)};
    }

    static Value string(std::string text)
    {
        return scalar(DataType::String, {std::vector {std::move(text)}});
    }
};

struct Argument
{
    std::vector<std::string> names;

    Encoder encode() const
    {
        auto argument = Encoder {};

        for (auto& name: names)
        {
            auto binding = Encoder {};
            binding.stringField(1, name);
            argument.messageField(1, binding);
        }

        return argument;
    }
};

struct NamedValueType
{
    std::string name;
    ValueType type;

    Encoder encode() const
    {
        auto named = Encoder {};
        named.stringField(1, name);
        named.messageField(2, type.encode());
        return named;
    }
};

struct Operation
{
    std::string type;
    std::map<std::string, Argument> inputs;
    std::vector<NamedValueType> outputs;
    std::map<std::string, Value> attributes;

    Encoder encode() const
    {
        auto operation = Encoder {};
        operation.stringField(1, type);

        for (auto& [key, argument]: inputs)
            operation.mapEntry(2, key, argument.encode());

        for (auto& output: outputs)
            operation.messageField(3, output.encode());

        for (auto& [key, value]: attributes)
            operation.mapEntry(5, key, value.encode());

        return operation;
    }
};

struct Block
{
    std::vector<std::string> outputs;
    std::vector<Operation> operations;

    Encoder encode() const
    {
        auto block = Encoder {};

        for (auto& output: outputs)
            block.stringField(2, output);

        for (auto& operation: operations)
            block.messageField(3, operation.encode());

        return block;
    }
};

struct Function
{
    std::vector<NamedValueType> inputs;
    std::string opset = "CoreML7";
    std::map<std::string, Block> blockSpecializations;

    Encoder encode() const
    {
        auto function = Encoder {};

        for (auto& input: inputs)
            function.messageField(1, input.encode());

        function.stringField(2, opset);

        for (auto& [key, block]: blockSpecializations)
            function.mapEntry(3, key, block.encode());

        return function;
    }
};

struct Program
{
    int64_t version = 1;
    std::map<std::string, Function> functions;

    Encoder encode() const
    {
        auto program = Encoder {};
        program.int64Field(1, version);

        for (auto& [key, function]: functions)
            program.mapEntry(2, key, function.encode());

        return program;
    }
};
} // namespace MIL

namespace Specification
{
using Proto::Encoder;

enum class ArrayDataType
{
    Float32 = 65568,
    Float16 = 65552,
    Int32 = 131104
};

inline ArrayDataType toArrayDataType(MIL::DataType type)
{
    switch (type)
    {
        case MIL::DataType::Float16:
            return ArrayDataType::Float16;
        case MIL::DataType::Float32:
            return ArrayDataType::Float32;
        case MIL::DataType::Int32:
            return ArrayDataType::Int32;
        default:
            throw std::invalid_argument("MILWriter: no multi-array type for this");
    }
}

using Shape = std::vector<int64_t>;

struct ArrayFeatureType
{
    Shape shape;
    ArrayDataType dataType = ArrayDataType::Float32;
    std::vector<Shape> enumeratedShapes;

    Encoder encode() const
    {
        auto array = Encoder {};
        array.packedInt64(1, shape);
        array.int64Field(2, static_cast<int64_t>(dataType));

        if (!enumeratedShapes.empty())
        {
            auto enumerated = Encoder {};

            for (auto& enumeratedShape: enumeratedShapes)
            {
                auto encodedShape = Encoder {};
                encodedShape.packedInt64(1, enumeratedShape);
                enumerated.messageField(1, encodedShape);
            }

            array.messageField(21, enumerated);
        }

        return array;
    }
};

struct FeatureDescription
{
    std::string name;
    ArrayFeatureType multiArrayType;

    Encoder encode() const
    {
        auto featureType = Encoder {};
        featureType.messageField(5, multiArrayType.encode());

        auto feature = Encoder {};
        feature.stringField(1, name);
        feature.messageField(3, featureType);
        return feature;
    }
};

struct ModelDescription
{
    std::vector<FeatureDescription> input;
    std::vector<FeatureDescription> output;

    Encoder encode() const
    {
        auto description = Encoder {};

        for (auto& feature: input)
            description.messageField(1, feature.encode());

        for (auto& feature: output)
            description.messageField(10, feature.encode());

        return description;
    }
};

struct Model
{
    int32_t specificationVersion = 8;
    ModelDescription description;
    MIL::Program mlProgram;

    Bytes serialize() const
    {
        auto model = Encoder {};
        model.int64Field(1, specificationVersion);
        model.messageField(2, description.encode());
        model.messageField(502, mlProgram.encode());
        return model.bytes();
    }
};
} // namespace Specification

namespace Blob
{
enum class BlobDataType : uint32_t
{
    Float16 = 1,
    Float32 = 2,
    UInt8 = 3,
    Int8 = 4,
    Int32 = 14
};

inline BlobDataType toBlobDataType(MIL::DataType type)
{
    switch (type)
    {
        case MIL::DataType::Float16:
            return BlobDataType::Float16;
        case MIL::DataType::Float32:
            return BlobDataType::Float32;
        case MIL::DataType::Int32:
            return BlobDataType::Int32;
        default:
            throw std::invalid_argument("MILWriter: type cannot live in a blob");
    }
}

constexpr uint64_t storageAlignment = 64;
constexpr uint32_t blobMetadataSentinel = 0xDEADBEEF;

struct StorageHeader
{
    uint32_t count = 0;
    uint32_t version = 2;
    uint64_t reserved[7] {};
};

struct BlobMetadata
{
    uint32_t sentinel = blobMetadataSentinel;
    BlobDataType milDtype = BlobDataType::Float16;
    uint64_t sizeInBytes = 0;
    uint64_t offset = 0;
    uint64_t paddingSizeInBits = 0;
    uint64_t reserved[4] {};
};

static_assert(sizeof(StorageHeader) == 64);
static_assert(sizeof(BlobMetadata) == 64);

class StorageWriter
{
public:
    StorageWriter() { write(StorageHeader {}); }

    uint64_t append(BlobDataType type, std::span<const uint8_t> data)
    {
        padTo(storageAlignment);

        auto metadataOffset = static_cast<uint64_t>(out.size());
        auto metadata = BlobMetadata {};
        metadata.milDtype = type;
        metadata.sizeInBytes = data.size();
        metadata.offset = metadataOffset + sizeof(BlobMetadata);

        write(metadata);
        out.insert(out.end(), data.begin(), data.end());
        ++header().count;

        return metadataOffset;
    }

    int count() const { return static_cast<int>(header().count); }
    const Bytes& bytes() const { return out; }

private:
    template <typename T>
    void write(const T& value)
    {
        auto raw = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), raw, raw + sizeof(T));
    }

    void padTo(uint64_t alignment)
    {
        out.resize((out.size() + alignment - 1) / alignment * alignment, 0);
    }

    StorageHeader& header() { return *reinterpret_cast<StorageHeader*>(out.data()); }

    const StorageHeader& header() const
    {
        return *reinterpret_cast<const StorageHeader*>(out.data());
    }

    Bytes out;
};
} // namespace Blob

struct Var
{
    std::string name;
    MIL::TensorType type;
};

enum class ConstStorage
{
    Blob,
    Inline
};

class Graph
{
public:
    explicit Graph(std::string opsetToUse = "CoreML7")
        : opset(std::move(opsetToUse))
    {
    }

    Var input(const std::string& name,
              MIL::DataType dataType,
              const std::vector<Specification::Shape>& shapes)
    {
        auto& defaultShape = shapes.front();
        auto var = Var {name, {dataType, {}}};

        for (auto axis = std::size_t {0}; axis < defaultShape.size(); ++axis)
        {
            auto dimension =
                MIL::Dimension {static_cast<uint64_t>(defaultShape[axis])};

            for (auto& shape: shapes)
                if (shape[axis] != defaultShape[axis])
                    dimension.size.reset();

            var.type.dimensions.push_back(dimension);
        }

        auto feature = Specification::FeatureDescription {
            name, {defaultShape, Specification::toArrayDataType(dataType), {}}};

        if (shapes.size() > 1)
            feature.multiArrayType.enumeratedShapes = shapes;

        inputs.push_back({name, {var.type}});
        inputFeatures.push_back(std::move(feature));
        return var;
    }

    Var constant(const std::string& name,
                 MIL::TensorType type,
                 std::span<const uint8_t> data,
                 ConstStorage storage = ConstStorage::Blob)
    {
        if (data.size() != type.elementCount() * MIL::sizeOf(type.dataType))
            throw std::invalid_argument("MILWriter: constant size mismatch");

        auto value = MIL::Value {{type}, {}};

        if (storage == ConstStorage::Blob)
        {
            auto offset = weights.append(Blob::toBlobDataType(type.dataType), data);
            value.value = MIL::BlobFileValue {.offset = offset};
        }
        else
        {
            value.value = inlineTensor(type.dataType, data);
        }

        return addConst(name, std::move(value));
    }

    Var constantBool(const std::string& name, bool flag)
    {
        return addConst(
            name, MIL::Value::scalar(MIL::DataType::Bool, {std::vector {flag}}));
    }

    Var constantInt(const std::string& name, int32_t number)
    {
        return addConst(
            name, MIL::Value::scalar(MIL::DataType::Int32, {std::vector {number}}));
    }

    Var matmul(const std::string& name,
               const Var& x,
               const Var& y,
               bool transposeX = false,
               bool transposeY = false)
    {
        auto flagX = constantBool(name + "_transpose_x_0", transposeX);
        auto flagY = constantBool(name + "_transpose_y_0", transposeY);

        auto dimsX = x.type.dimensions;
        auto dimsY = y.type.dimensions;

        if (transposeX)
            std::swap(dimsX[dimsX.size() - 2], dimsX.back());

        if (transposeY)
            std::swap(dimsY[dimsY.size() - 2], dimsY.back());

        auto result = Var {name, {x.type.dataType, dimsX}};
        result.type.dimensions.back() = dimsY.back();

        return addOperation(
            "matmul",
            {{"x", x}, {"y", y}, {"transpose_x", flagX}, {"transpose_y", flagY}},
            std::move(result));
    }

    Var linear(const std::string& name,
               const Var& x,
               const Var& weight,
               std::optional<Var> bias = {})
    {
        auto outFeatures = weight.type.dimensions.front();

        if (!bias)
        {
            auto biasType = MIL::TensorType {weight.type.dataType, {outFeatures}};
            auto zeros =
                Bytes(biasType.elementCount() * MIL::sizeOf(biasType.dataType), 0);
            bias = constant(name + "_bias_0", biasType, zeros);
        }

        auto result = Var {name, x.type};
        result.type.dimensions.back() = outFeatures;

        return addOperation("linear",
                            {{"x", x}, {"weight", weight}, {"bias", *bias}},
                            std::move(result));
    }

    Var softmax(const std::string& name, const Var& x, int32_t axis)
    {
        auto axisVar = constantInt(name + "_axis_0", axis);
        return addOperation(
            "softmax", {{"x", x}, {"axis", axisVar}}, {name, x.type});
    }

    void output(const Var& var)
    {
        auto shape = Specification::Shape {};

        if (var.type.isFixed())
            for (auto& dimension: var.type.dimensions)
                shape.push_back(static_cast<int64_t>(*dimension.size));

        outputs.push_back(var.name);
        outputFeatures.push_back(
            {var.name,
             {shape, Specification::toArrayDataType(var.type.dataType), {}}});
    }

    Specification::Model model(int32_t specificationVersion = 8) const
    {
        auto function = MIL::Function {inputs, opset, {}};
        function.blockSpecializations[opset] = {outputs, operations};

        auto result = Specification::Model {};
        result.specificationVersion = specificationVersion;
        result.description = {inputFeatures, outputFeatures};
        result.mlProgram.functions["main"] = std::move(function);
        return result;
    }

    const Blob::StorageWriter& weightFile() const { return weights; }

private:
    static MIL::TensorValue inlineTensor(MIL::DataType type,
                                         std::span<const uint8_t> data)
    {
        if (type == MIL::DataType::Float32)
        {
            auto floats = std::vector<float>(data.size() / sizeof(float));
            std::memcpy(floats.data(), data.data(), data.size());
            return {std::move(floats)};
        }

        if (type == MIL::DataType::Int32)
        {
            auto ints = std::vector<int32_t>(data.size() / sizeof(int32_t));
            std::memcpy(ints.data(), data.data(), data.size());
            return {std::move(ints)};
        }

        return {Bytes(data.begin(), data.end())};
    }

    Var addConst(const std::string& name, MIL::Value value)
    {
        auto var = Var {name, value.type.tensorType};
        auto operation = MIL::Operation {"const", {}, {{name, {var.type}}}, {}};
        operation.attributes.emplace("name", MIL::Value::string(name));
        operation.attributes.emplace("val", std::move(value));
        operations.push_back(std::move(operation));
        return var;
    }

    Var addOperation(const std::string& type,
                     const std::map<std::string, Var>& arguments,
                     Var result)
    {
        auto operation =
            MIL::Operation {type, {}, {{result.name, {result.type}}}, {}};

        for (auto& [key, var]: arguments)
            operation.inputs[key] = {{var.name}};

        operation.attributes.emplace("name", MIL::Value::string(result.name));
        operations.push_back(std::move(operation));
        return result;
    }

    std::string opset;
    std::vector<MIL::NamedValueType> inputs;
    std::vector<Specification::FeatureDescription> inputFeatures;
    std::vector<Specification::FeatureDescription> outputFeatures;
    std::vector<std::string> outputs;
    std::vector<MIL::Operation> operations;
    Blob::StorageWriter weights;
};

namespace Package
{
inline std::string makeUUID()
{
    auto device = std::random_device {};
    auto bytes = std::array<uint8_t, 16> {};

    for (auto& byte: bytes)
        byte = static_cast<uint8_t>(device());

    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);

    constexpr auto digits = "0123456789ABCDEF";
    auto text = std::string {};

    for (auto index = std::size_t {0}; index < bytes.size(); ++index)
    {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            text += '-';

        text += digits[bytes[index] >> 4];
        text += digits[bytes[index] & 0x0f];
    }

    return text;
}

inline std::string manifestItem(const std::string& identifier,
                                const std::string& description,
                                const std::string& name,
                                const std::string& path)
{
    return "        \"" + identifier + "\": {\n"
           + "            \"author\": \"com.apple.CoreML\",\n"
           + "            \"description\": \"" + description + "\",\n"
           + "            \"name\": \"" + name + "\",\n" + "            \"path\": \""
           + path + "\"\n" + "        }";
}

inline std::string manifest()
{
    auto modelIdentifier = makeUUID();

    return "{\n    \"fileFormatVersion\": \"1.0.0\",\n"
           "    \"itemInfoEntries\": {\n"
           + manifestItem(modelIdentifier,
                          "CoreML Model Specification",
                          "model.mlmodel",
                          "com.apple.CoreML/model.mlmodel")
           + ",\n"
           + manifestItem(makeUUID(),
                          "CoreML Model Weights",
                          "weights",
                          "com.apple.CoreML/weights")
           + "\n    },\n    \"rootModelIdentifier\": \"" + modelIdentifier
           + "\"\n}\n";
}

inline void writeFile(const std::filesystem::path& path,
                      std::span<const uint8_t> data)
{
    auto file = std::ofstream(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));

    if (!file)
        throw std::runtime_error("MILWriter: cannot write " + path.string());
}

inline void write(const std::filesystem::path& packagePath,
                  const Bytes& modelBytes,
                  const Blob::StorageWriter& weights)
{
    namespace fs = std::filesystem;

    fs::remove_all(packagePath);

    auto dataDirectory = packagePath / "Data" / "com.apple.CoreML";
    auto weightsDirectory = dataDirectory / "weights";
    fs::create_directories(weightsDirectory);

    auto manifestText = manifest();
    writeFile(packagePath / "Manifest.json",
              {reinterpret_cast<const uint8_t*>(manifestText.data()),
               manifestText.size()});
    writeFile(dataDirectory / "model.mlmodel", modelBytes);

    if (weights.count() > 0)
        writeFile(weightsDirectory / "weight.bin", weights.bytes());
}

inline void write(const std::filesystem::path& packagePath, const Graph& graph)
{
    write(packagePath, graph.model().serialize(), graph.weightFile());
}
} // namespace Package
} // namespace MILWriter
