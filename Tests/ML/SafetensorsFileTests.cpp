#include <NanoTest/NanoTest.h>

#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Loader/SafetensorsFile.h>

#include <cmath>
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
