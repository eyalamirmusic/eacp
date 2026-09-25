#include "GraphCommon.h"

// MILBlob storage version 2, the layout the phase 0 spike found Core ML takes.

using namespace nano;
using namespace eacp;
using namespace eacp::ML;
using namespace MLGraphTesting;

namespace
{
std::uint32_t readUInt32(const Bytes& bytes, int offset)
{
    auto value = std::uint32_t {0};

    for (auto byte = 0; byte < 4; ++byte)
        value |= static_cast<std::uint32_t>(bytes[offset + byte]) << (byte * 8);

    return value;
}

std::uint64_t readUInt64(const Bytes& bytes, int offset)
{
    auto value = std::uint64_t {0};

    for (auto byte = 0; byte < 8; ++byte)
        value |= static_cast<std::uint64_t>(bytes[offset + byte]) << (byte * 8);

    return value;
}

bool allZero(const Bytes& bytes, int first, int last)
{
    for (auto index = first; index < last; ++index)
        if (bytes[index] != 0)
            return false;

    return true;
}
} // namespace

auto tBlobHeader = test("MLGraph/Blob/headerIsSixtyFourBytes") = []
{
    auto writer = Blob::Writer {};
    auto& bytes = writer.bytes();

    check(bytes.size() == 64);
    check(hex(bytes).starts_with("00 00 00 00 02 00 00 00 00 00"));
    check(allZero(bytes, 8, 64));
    check(writer.count() == 0);
};

auto tBlobTwoEntries =
    test("MLGraph/Blob/entriesAreAlignedRecordsBeforeTheirData") = []
{
    auto writer = Blob::Writer {};
    auto first = Bytes {1, 2, 3, 4};
    auto second = Bytes {5, 6, 7, 8, 9, 10, 11, 12};

    auto firstOffset = writer.append(Blob::DataType::float16, first);
    auto secondOffset = writer.append(Blob::DataType::int32, second);
    auto& bytes = writer.bytes();

    check(firstOffset == 64);
    check(secondOffset == 192);
    check(bytes.size() == 264);
    check(writer.count() == 2);
    check(readUInt32(bytes, 0) == 2);
    check(readUInt32(bytes, 4) == 2);

    auto recordHex = [&bytes](int offset)
    {
        auto record = Bytes {};

        for (auto index = offset; index < offset + 32; ++index)
            record.add(bytes[index]);

        return hex(record);
    };

    // sentinel, mil dtype, size in bytes, offset of the data, padding bits.
    check(recordHex(64)
          == "EF BE AD DE 01 00 00 00 04 00 00 00 00 00 00 00 80 00 00 00 00 00 00 "
             "00 00 00 00 00 00 00 00 00");
    check(allZero(bytes, 96, 128));
    check(bytes[128] == 1 && bytes[131] == 4);
    check(allZero(bytes, 132, 192));

    check(readUInt32(bytes, 192) == Blob::metadataSentinel);
    check(readUInt32(bytes, 196) == 14);
    check(readUInt64(bytes, 200) == 8);
    check(readUInt64(bytes, 208) == 256);
    check(allZero(bytes, 216, 256));
    check(bytes[256] == 5 && bytes[263] == 12);
};

auto tBlobConstantOffsets = test("MLGraph/Blob/programPointsAtTheRecord") = []
{
    auto graph = Graph {};
    auto x = graph.input("x", {1, 2}, DType::float16);
    auto weights = halves({1, 2, 3, 4});
    auto biases = halves({0.5f, -0.5f});
    auto weight = graph.constant("w", {2, 2}, DType::float16, weights);
    auto bias = graph.constant("b", {2}, DType::float16, biases);
    graph.output(graph.linear(x, weight, bias), "y");

    auto package = buildChecked(graph);
    auto text = graph.toText();

    check(text.find("tensor<fp16, [2, 2]> w = const()[val = blob(64)];")
          != std::string::npos);
    check(text.find("tensor<fp16, [2]> b = const()[val = blob(192)];")
          != std::string::npos);
    check(package.weights.size() == 260);
    check(readUInt64(package.weights, 64 + 16) == 128);
    check(readUInt64(package.weights, 192 + 16) == 256);
    check(package.weights[256] == halves({0.5f})[0]);
};

auto tBlobHalfConstant = test("MLGraph/Blob/halfConstantStoresHalves") = []
{
    auto values = Vector<float> {1, -2, 0.5f, 65504};
    auto halvesBytes = halves({1, -2, 0.5f, 65504});
    auto add = [](const GPU::Float& a, const GPU::Float& b) { return a + b; };

    auto fromHalves = Graph {};
    auto x = fromHalves.input("x", {1, 4}, DType::float16);
    auto weight = fromHalves.constant("w", {1, 4}, DType::float16, halvesBytes);
    fromHalves.output(fromHalves.apply(x, weight, add), "y");

    auto fromFloats = Graph {};
    auto input = fromFloats.input("x", {1, 4}, DType::float16);
    auto converted = fromFloats.halfConstant("w", {1, 4}, values);
    check(fromFloats.type(converted) == DType::float16);
    fromFloats.output(fromFloats.apply(input, converted, add), "y");

    check(halfBytes(values) == halvesBytes);
    check(buildChecked(fromFloats).weights == buildChecked(fromHalves).weights);
};

auto tBlobLinearWithoutBias =
    test("MLGraph/Blob/aLinearWithoutABiasStoresZerosOnlyWhenUsed") = []
{
    auto weights = halves({1, 2, 3, 4});

    auto used = Graph {};
    auto x = used.input("x", {1, 2}, DType::float16);
    auto weight = used.constant("w", {2, 2}, DType::float16, weights);
    used.output(used.linear(x, weight), "y");

    auto package = buildChecked(used);
    check(package.weights.size() == 260);
    check(readUInt64(package.weights, 192 + 16) == 256);
    check(allZero(package.weights, 256, 260));

    auto unused = Graph {};
    auto input = unused.input("x", {1, 2}, DType::float16);
    auto ignored = unused.constant("w", {2, 2}, DType::float16, weights);
    unused.linear(input, ignored);
    unused.output(unused.gelu(input), "y");

    check(buildChecked(unused).weights.empty());
    check(unused.toText().find("const_") == std::string::npos);

    auto wide = Graph {};
    auto single = wide.input("x", {1, 2}, DType::float32);
    auto zeros = Bytes {};
    zeros.resize(24, 0);
    auto floats = wide.constant("w", {3, 2}, DType::float32, zeros);
    wide.output(wide.linear(single, floats), "y");
    check(buildChecked(wide).weights.size() == 256 + 12);

    auto flat = zeroConstant(wide, "flat", {4});
    check(!wide.linear(single, flat).isValid());
};
