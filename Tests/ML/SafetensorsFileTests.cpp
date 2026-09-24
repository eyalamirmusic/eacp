#include <NanoTest/NanoTest.h>

#include <eacp/GPU/Codegen/PackedVertex.h>
#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/TensorOps.h>
#include <eacp/ML/Loader/SafetensorsFile.h>

#include <array>
#include <cmath>
#include <optional>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
std::filesystem::path writeSampleFile()
{
    auto header = std::string {
        "{\"weight\":{\"dtype\":\"F32\",\"shape\":[2,3],\"data_offsets\":[0,24]},"
        "\"bias\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[24,28]},"
        "\"__metadata__\":{\"format\":\"pt\"}}"};

    auto weightValues =
        std::vector<float> {1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
    auto biasValue = 7.5f;

    auto path = std::filesystem::temp_directory_path()
              / "eacp-ml-safetensors-test.safetensors";

    auto file = std::ofstream {path, std::ios::binary};

    auto headerLength = (std::uint64_t) header.size();
    file.write(reinterpret_cast<const char*>(&headerLength), sizeof(headerLength));
    file.write(header.data(), (std::streamsize) header.size());
    file.write(reinterpret_cast<const char*>(weightValues.data()),
              (std::streamsize) (weightValues.size() * sizeof(float)));
    file.write(reinterpret_cast<const char*>(&biasValue), sizeof(biasValue));
    file.close();

    return path;
}
}

auto tSafetensorsParsesHeaderAndShapes =
    test("SafetensorsFile/parsesHeaderAndShapes") = []
{
    auto path = writeSampleFile();
    auto file = SafetensorsFile::open(path.string());

    check(file.has_value());

    auto weight = file->find("weight");
    check(weight != nullptr);
    check(weight->dtype == SafetensorsDType::F32);
    check(weight->shape.size() == 2);
    check(weight->shape[0] == 2);
    check(weight->shape[1] == 3);

    auto bias = file->find("bias");
    check(bias != nullptr);
    check(bias->shape.size() == 1);
    check(bias->shape[0] == 1);

    check(file->find("missing") == nullptr);

    std::filesystem::remove(path);
};

auto tSafetensorsLoadsScalarAndTensor =
    test("SafetensorsFile/loadsScalarAndTensorValues") = []
{
    auto path = writeSampleFile();
    auto file = SafetensorsFile::open(path.string());

    check(file.has_value());

    auto biasValue = file->loadScalar("bias");
    check(std::abs(biasValue - 7.5f) < 1.0e-6f);

    auto& device = Device::shared();

    if (device.isValid())
    {
        auto weight = file->loadF32("weight", device);
        check(weight.shape().size() == 2);
        check(weight.count() == 6);

        auto values = weight.toHostF32();
        auto expected = std::vector<float> {1.f, 2.f, 3.f, 4.f, 5.f, 6.f};

        for (auto i = std::size_t {}; i < expected.size(); ++i)
            check(values[i] == expected[i]);

        auto packed = file->loadPackedF16("weight", device);
        check(packed.isPacked());

        auto packedValues = packed.toHostF32();

        for (auto i = std::size_t {}; i < expected.size(); ++i)
            check(std::abs(packedValues[i] - expected[i]) < 0.01f);
    }

    std::filesystem::remove(path);
};

namespace
{
constexpr auto alignedValues = std::array {1.5f, -2.f, 3.25f, 4.f};
constexpr auto bf16Values = std::array {0.5f, -8.f};
constexpr auto oddValues = std::array {9.f, 10.5f, -11.f};

void writeBytes(std::ofstream& file, const void* data, std::size_t bytes)
{
    file.write(reinterpret_cast<const char*>(data), (std::streamsize) bytes);
}

// A header padded so the data starts on eight bytes, then an F32 tensor on the
// grid, a BF16 one, and an F32 one two bytes off it - behind a lone BF16 value.
std::filesystem::path writeMixedFile()
{
    auto header = std::string {
        "{\"aligned\":{\"dtype\":\"F32\",\"shape\":[2,2],\"data_offsets\":[0,16]},"
        "\"bf16\":{\"dtype\":\"BF16\",\"shape\":[2],\"data_offsets\":[16,20]},"
        "\"pad\":{\"dtype\":\"BF16\",\"shape\":[1],\"data_offsets\":[20,22]},"
        "\"odd\":{\"dtype\":\"F32\",\"shape\":[3],\"data_offsets\":[22,34]}}"};

    while ((8 + header.size()) % 8 != 0)
        header += ' ';

    auto path = std::filesystem::temp_directory_path()
                / "eacp-ml-safetensors-mixed.safetensors";
    auto file = std::ofstream {path, std::ios::binary};

    auto headerLength = (std::uint64_t) header.size();
    writeBytes(file, &headerLength, sizeof(headerLength));
    writeBytes(file, header.data(), header.size());
    writeBytes(file, alignedValues.data(), sizeof(alignedValues));

    for (auto value: bf16Values)
    {
        auto bits = bfloat16FromFloat(value);
        writeBytes(file, &bits, sizeof(bits));
    }

    auto pad = std::uint16_t {};
    writeBytes(file, &pad, sizeof(pad));
    writeBytes(file, oddValues.data(), sizeof(oddValues));

    return path;
}

template <std::size_t N>
std::vector<float> vectorOf(const std::array<float, N>& values)
{
    return {values.begin(), values.end()};
}
} // namespace

auto tSafetensorsLoadsInPlaceWhereItCan =
    test("SafetensorsFile/loadsF32InPlaceWhereTheDeviceAdoptsTheMapping") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto path = writeMixedFile();
    auto file = SafetensorsFile::open(path.string());
    check(file.has_value());

    auto aligned = file->loadF32("aligned", device);
    auto bf16 = file->loadF32("bf16", device);
    auto odd = file->loadF32("odd", device);

    check(aligned.toHostF32() == vectorOf(alignedValues));
    check(bf16.toHostF32() == vectorOf(bf16Values));
    check(odd.toHostF32() == vectorOf(oddValues));
    check(file->readF32("bf16") == vectorOf(bf16Values));

    auto counts = file->loadCounts();
    auto adopts = Buffer::canAdoptMemory(device);

    check(counts.inPlace == (adopts ? 1 : 0));
    check(counts.copied == (adopts ? 1 : 2));
    check(counts.converted == 1);

    if (adopts)
        check(aligned.byteOffset() > 0 && aligned.byteOffset() % 8 == 0);

    std::filesystem::remove(path);
};

auto tSafetensorsTensorsOutliveTheFile =
    test("SafetensorsFile/tensorsLoadedInPlaceOutliveTheFile") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto path = writeMixedFile();
    auto aligned = std::optional<Tensor> {};

    {
        auto file = SafetensorsFile::open(path.string());
        check(file.has_value());
        aligned = file->loadF32("aligned", device);
    }

    std::filesystem::remove(path);

    auto commands = device.makeCommandBuffer();
    auto doubled = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        doubled = add(pass, *aligned, *aligned, device);
    }

    commands.commit();

    auto expected = vectorOf(alignedValues);

    for (auto& value: expected)
        value *= 2.f;

    check(aligned->toHostF32() == vectorOf(alignedValues));
    check(doubled->toHostF32() == expected);
};
