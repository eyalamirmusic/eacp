#pragma once

#include "Protobuf.h"

#include <string>
#include <variant>

// The slice of Model.proto and MIL.proto an ML Program uses, as plain data, and
// its encoding. Field numbers are coremltools' mlmodel/format ones.
namespace eacp::ML::MIL
{
enum class DataType
{
    boolean = 1,
    string = 2,
    float16 = 10,
    float32 = 11,
    int32 = 23
};

inline constexpr std::int64_t unknownDimension = -1;

int sizeOf(DataType type);
std::string toText(DataType type);

struct TensorType
{
    DataType dataType = DataType::float32;
    Vector<std::int64_t> dimensions;
};

struct Floats
{
    Vector<float> values;
};

struct Ints
{
    Vector<std::int32_t> values;
};

struct Bools
{
    Vector<std::uint8_t> values;
};

struct StringList
{
    Vector<std::string> values;
};

// Raw little-endian elements, which is how an fp16 tensor goes inline.
struct RawBytes
{
    Bytes values;
};

using ImmediateValue = std::variant<Floats, Ints, Bools, StringList, RawBytes>;

struct BlobFileValue
{
    std::string fileName = "@model_path/weights/weight.bin";
    std::uint64_t offset = 0;
};

struct Value
{
    TensorType type;
    std::variant<ImmediateValue, BlobFileValue> value;

    static Value scalar(float number, DataType type);
    static Value scalar(std::int32_t number);
    static Value scalar(bool flag);
    static Value string(std::string_view text);
    static Value ints(const Vector<std::int32_t>& numbers);
    static Value bools(const Vector<std::uint8_t>& flags);
    static Value blob(const TensorType& type, std::uint64_t offset);
};

struct NamedValueType
{
    std::string name;
    TensorType type;
};

struct Input
{
    std::string parameter;
    Vector<std::string> arguments;
};

struct Attribute
{
    std::string name;
    Value value;
};

struct Operation
{
    std::string type;
    Vector<Input> inputs;
    Vector<NamedValueType> outputs;
    Vector<Attribute> attributes;
};

struct Block
{
    Vector<std::string> outputs;
    Vector<Operation> operations;
};

struct Function
{
    Vector<NamedValueType> inputs;
    std::string opset = "CoreML7";
    Block block;
};

struct Program
{
    std::int64_t version = 1;
    Function main;
};

enum class ArrayDataType
{
    float16 = 65552,
    float32 = 65568,
    int32 = 131104
};

struct ArrayFeature
{
    std::string name;
    Vector<std::int64_t> shape;
    ArrayDataType dataType = ArrayDataType::float32;
    Vector<Vector<std::int64_t>> enumeratedShapes;
};

struct Description
{
    Vector<ArrayFeature> inputs;
    Vector<ArrayFeature> outputs;
};

struct Specification
{
    std::int32_t specificationVersion = 8;
    Description description;
    Program program;
};

Protobuf::Writer encode(const TensorType& type);
Protobuf::Writer encode(const Value& value);
Protobuf::Writer encode(const NamedValueType& named);
Protobuf::Writer encode(const Operation& operation);
Protobuf::Writer encode(const Block& block);
Protobuf::Writer encode(const Function& function);
Protobuf::Writer encode(const Program& program);
Protobuf::Writer encode(const ArrayFeature& feature);
Protobuf::Writer encode(const Description& description);
Protobuf::Writer encode(const Specification& specification);

Bytes serialize(const Specification& specification);

std::string toText(const TensorType& type);
std::string toText(const Value& value);
std::string toText(const Program& program);
} // namespace eacp::ML::MIL
