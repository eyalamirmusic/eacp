#include "GraphCommon.h"

// The wire encoding against bytes worked out by hand from the protobuf spec
// and from the field numbers of coremltools' Model.proto and MIL.proto.

using namespace nano;
using namespace eacp;
using namespace eacp::ML;
using namespace MLGraphTesting;

namespace
{
std::string varintHex(std::uint64_t value)
{
    auto writer = Protobuf::Writer {};
    writer.varint(value);
    return hex(writer.bytes());
}
} // namespace

auto tProtobufVarint =
    test("MLGraph/Protobuf/varintsAreLittleEndianSevenBitGroups") = []
{
    check(varintHex(0) == "00");
    check(varintHex(1) == "01");
    check(varintHex(127) == "7F");
    check(varintHex(128) == "80 01");
    check(varintHex(300) == "AC 02");
    check(varintHex(std::uint64_t {1} << 32) == "80 80 80 80 10");
};

auto tProtobufNegative = test("MLGraph/Protobuf/negativeIntsTakeTenBytes") = []
{
    auto writer = Protobuf::Writer {};
    writer.int64Field(1, -1);
    check(hex(writer.bytes()) == "08 FF FF FF FF FF FF FF FF FF 01");

    auto packed = Protobuf::Writer {};
    auto values = Vector<std::int32_t> {-1};
    packed.packedInt32(1, values);
    check(hex(packed.bytes()) == "0A 0A FF FF FF FF FF FF FF FF FF 01");
};

auto tProtobufDefaults = test("MLGraph/Protobuf/defaultScalarsAreOmitted") = []
{
    auto writer = Protobuf::Writer {};
    writer.uint64Field(1, 0);
    writer.boolField(2, false);
    writer.packedInt64(3, {});
    check(writer.bytes().empty());

    writer.boolField(2, true);
    check(hex(writer.bytes()) == "10 01");
};

auto tProtobufLength = test("MLGraph/Protobuf/lengthDelimitedFields") = []
{
    auto writer = Protobuf::Writer {};
    writer.stringField(2, "hi");
    check(hex(writer.bytes()) == "12 02 68 69");

    auto empty = Protobuf::Writer {};
    empty.messageField(2, Protobuf::Writer {});
    check(hex(empty.bytes()) == "12 00");
};

auto tProtobufNested = test("MLGraph/Protobuf/nestedMessagesAndMaps") = []
{
    auto inner = Protobuf::Writer {};
    inner.uint64Field(1, 150);

    auto outer = Protobuf::Writer {};
    outer.messageField(3, inner);
    check(hex(outer.bytes()) == "1A 03 08 96 01");

    auto value = Protobuf::Writer {};
    value.uint64Field(1, 1);

    auto map = Protobuf::Writer {};
    map.mapEntry(2, "k", value);
    check(hex(map.bytes()) == "12 07 0A 01 6B 12 02 08 01");
};

auto tProtobufPacked = test("MLGraph/Protobuf/packedFloatsAndBools") = []
{
    auto floats = Protobuf::Writer {};
    auto values = Vector<float> {1.f};
    floats.packedFloat(1, values);
    check(hex(floats.bytes()) == "0A 04 00 00 80 3F");

    auto bools = Protobuf::Writer {};
    auto flags = Vector<std::uint8_t> {1, 0};
    bools.packedBool(1, flags);
    check(hex(bools.bytes()) == "0A 02 01 00");
};

auto tMILTensorType = test("MLGraph/Protobuf/tensorTypeWithAnUnknownDimension") = []
{
    // dataType = 1, rank = 2, dimensions = 3: a ConstantDimension (1) of size
    // 2 and an UnknownDimension (2).
    auto type = MIL::TensorType {MIL::DataType::float16, {2, MIL::unknownDimension}};
    check(hex(MIL::encode(type).bytes())
          == "08 0A 10 02 1A 04 0A 02 08 02 1A 02 12 00");
};

auto tMILScalarValue = test("MLGraph/Protobuf/immediateInt32Value") = []
{
    // Value.type (2) -> ValueType.tensorType (1); Value.immediateValue (3) ->
    // ImmediateValue.tensor (1) -> TensorValue.ints (2) -> values (1).
    check(hex(MIL::encode(MIL::Value::scalar(std::int32_t {-1})).bytes())
          == "12 04 0A 02 08 17 1A 10 0A 0E 12 0C 0A 0A FF FF FF FF FF FF FF FF "
             "FF 01");

    // An fp16 scalar goes inline as TensorValue.bytes (7).
    check(hex(MIL::encode(MIL::Value::scalar(1.f, MIL::DataType::float16)).bytes())
          == "12 04 0A 02 08 0A 1A 08 0A 06 3A 04 0A 02 00 3C");

    check(hex(MIL::encode(MIL::Value::string("EXACT")).bytes())
          == "12 04 0A 02 08 02 1A 0B 0A 09 22 07 0A 05 45 58 41 43 54");
};

auto tMILBlobValue = test("MLGraph/Protobuf/blobFileValue") = []
{
    // Value.blobFileValue (5): fileName (1), offset (2).
    auto type = MIL::TensorType {MIL::DataType::float16, {2}};
    auto encoded = hex(MIL::encode(MIL::Value::blob(type, 64)).bytes());

    auto fileName = std::string {"@model_path/weights/weight.bin"};
    auto nameBytes = Bytes {};

    for (auto character: fileName)
        nameBytes.add(static_cast<std::uint8_t>(character));

    check(fileName.size() == 30);
    check(encoded
          == "12 0C 0A 0A 08 0A 10 01 1A 04 0A 02 08 02 2A 22 0A 1E "
                 + hex(nameBytes) + " 10 40");
};

auto tMILOperation = test("MLGraph/Protobuf/operationFields") = []
{
    // type (1), inputs map (2) of Argument -> Binding.name, outputs (3).
    auto operation = MIL::Operation {};
    operation.type = "relu";
    operation.inputs.add({"x", {"a"}});
    operation.outputs.add({"b", {MIL::DataType::float32, {}}});

    check(hex(MIL::encode(operation).bytes())
          == "0A 04 72 65 6C 75 12 0A 0A 01 78 12 05 0A 03 0A 01 61 1A 09 0A 01 62 "
             "12 04 0A 02 08 0B");
};

auto tMILFeature =
    test("MLGraph/Protobuf/multiArrayFeatureWithEnumeratedShapes") = []
{
    // FeatureDescription: name (1), type (3) -> multiArrayType (5): shape (1),
    // dataType (2, FLOAT16 = 65552), enumeratedShapes (21) -> shapes (1).
    auto feature = MIL::ArrayFeature {};
    feature.name = "x";
    feature.shape = {2, 3};
    feature.dataType = MIL::ArrayDataType::float16;
    feature.enumeratedShapes = {{2, 3}, {4, 3}};

    check(hex(MIL::encode(feature).bytes())
          == "0A 01 78 1A 19 2A 17 0A 02 02 03 10 90 80 04 AA 01 0C 0A 04 0A 02 02 "
             "03 0A 04 0A 02 04 03");
};

auto tMILSpecification = test("MLGraph/Protobuf/modelFieldNumbers") = []
{
    // Model: specificationVersion (1), description (2), mlProgram (502).
    // Program: version (1), functions (2, "main"); Function: opset (2),
    // block_specializations (3, keyed by the opset).
    auto specification = MIL::Specification {};
    auto bytes = MIL::serialize(specification);

    check(hex(bytes)
          == "08 08 12 00 B2 1F 22 08 01 12 1E 0A 04 6D 61 69 6E 12 16 12 07 43 6F "
             "72 65 4D 4C 37 1A 0B 0A 07 43 6F 72 65 4D 4C 37 12 00");
};

auto tMILSliceLike = test("MLGraph/Protobuf/sliceLikeBindsAShapeOpAsItsEnd") = []
{
    auto graph = Graph {};
    auto rows = graph.input("rows", {6, 4}, {{2, 4}}, DType::float16);
    auto table = zeroConstant(graph, "table", {6, 4});
    graph.output(graph.sliceLike(table, rows), "y");

    auto specification = graph.specification();
    auto& operations = specification.program.main.block.operations;
    auto isShape = [](const MIL::Operation& op) { return op.type == "shape"; };
    auto isSlice = [](const MIL::Operation& op)
    { return op.type == "slice_by_index"; };
    auto* shape = operations.findIf(isShape);
    auto* slice = operations.findIf(isSlice);
    check(shape != nullptr && slice != nullptr);

    if (shape == nullptr || slice == nullptr)
        return;

    auto isEnd = [](const MIL::Input& input) { return input.parameter == "end"; };
    auto* end = slice->inputs.findIf(isEnd);
    check(end != nullptr && end->arguments == Vector<std::string> {"shape_2"});

    // type "shape", input x bound to "rows", and one output: shape_2, an int32
    // tensor (23) of rank 1 with the one ConstantDimension 2.
    auto withoutAttributes = *shape;
    withoutAttributes.attributes.clear();
    check(hex(MIL::encode(withoutAttributes).bytes())
          == "0A 05 73 68 61 70 65 12 0D 0A 01 78 12 08 0A 06 0A 04 72 6F 77 73 "
             "1A 17 0A 07 73 68 61 70 65 5F 32 12 0C 0A 0A 08 17 10 01 1A 04 0A "
             "02 08 02");
};
