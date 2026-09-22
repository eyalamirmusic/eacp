#pragma once

#include "../../GPU/Buffer/Buffer.h"
#include "../../GPU/Device/Device.h"

#include <cstdint>
#include <vector>

namespace eacp::ML
{
enum class DType
{
    F32,
    F16Packed
};

int elementCountOf(const std::vector<int>& shape);

class Tensor
{
public:
    Tensor(GPU::Buffer bufferToUse, std::vector<int> shapeToUse, DType dtypeToUse);

    static Tensor fromHostF32(const float* data,
                              std::vector<int> shape,
                              GPU::Device& device = GPU::Device::shared());

    static Tensor fromHostPackedF16(const float* data,
                                    std::vector<int> shape,
                                    GPU::Device& device = GPU::Device::shared());

    static Tensor uninitializedF32(std::vector<int> shape,
                                   GPU::Device& device = GPU::Device::shared());

    std::vector<float> toHostF32() const;

    const std::vector<int>& shape() const { return shapeValue; }
    int rank() const { return (int) shapeValue.size(); }
    int dim(int axis) const { return shapeValue[(std::size_t) axis]; }
    int count() const { return elementCountOf(shapeValue); }

    int rows() const;
    int cols() const;

    DType dtype() const { return dtypeValue; }
    bool isPacked() const { return dtypeValue == DType::F16Packed; }

    GPU::Buffer& buffer() { return bufferValue; }
    const GPU::Buffer& buffer() const { return bufferValue; }

private:
    GPU::Buffer bufferValue;
    std::vector<int> shapeValue;
    DType dtypeValue;
};
}
