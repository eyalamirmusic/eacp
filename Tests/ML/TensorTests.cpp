#include <NanoTest/NanoTest.h>

#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Tensor/Tensor.h>

#include <cmath>
#include <vector>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
std::vector<float> scatteredValues(int count, int salt)
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.push_back((float) (((i * 37 + salt * 11) % 23) - 11) * 0.125f);

    return values;
}
}

auto tTensorRoundTripsF32 = test("Tensor/roundTripsF32") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = scatteredValues(37, 3);
    auto tensor = Tensor::fromHostF32(values.data(), {37}, device);

    check(tensor.count() == 37);
    check(tensor.dtype() == DType::F32);

    auto readBack = tensor.toHostF32();
    check(readBack.size() == values.size());

    for (auto i = std::size_t {}; i < values.size(); ++i)
        check(readBack[i] == values[i]);
};

auto tTensorRoundTripsPackedF16 = test("Tensor/roundTripsPackedF16") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = scatteredValues(41, 5);
    auto tensor = Tensor::fromHostPackedF16(values.data(), {41}, device);

    check(tensor.dtype() == DType::F16Packed);
    check(tensor.isPacked());

    auto readBack = tensor.toHostF32();
    check(readBack.size() == values.size());

    for (auto i = std::size_t {}; i < values.size(); ++i)
        check(std::abs(readBack[i] - values[i]) < 0.01f);
};

auto tTensorShapeHelpers = test("Tensor/rowsAndColumnsReflectShape") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = scatteredValues(24, 7);
    auto tensor = Tensor::fromHostF32(values.data(), {4, 6}, device);

    check(tensor.rows() == 4);
    check(tensor.cols() == 6);
    check(tensor.count() == 24);

    auto rank3 = Tensor::fromHostF32(values.data(), {2, 3, 4}, device);
    check(rank3.rows() == 6);
    check(rank3.cols() == 4);
};
