#pragma once

#include "../Common.h"

#include <initializer_list>
#include <string>

namespace eacp::ML
{
enum class DType
{
    float16,
    float32,
    int32
};

std::string toString(DType type);

// A tensor's dimensions, outermost first. A dimension is `unknown` where an
// input enumerates more than one size for it; reshape also takes `unknown` as
// the one dimension to infer, as MIL's reshape does.
struct Shape
{
    static constexpr int unknown = -1;

    Shape() = default;
    Shape(std::initializer_list<int> dimsToUse);
    explicit Shape(const Vector<int>& dimsToUse);

    int rank() const { return dims.size(); }
    int operator[](int axis) const { return dims[axis]; }

    bool isFixed() const;
    std::int64_t count() const;

    bool operator==(const Shape& other) const { return dims == other.dims; }
    bool operator!=(const Shape& other) const { return !(*this == other); }

    std::string toString() const;

    Vector<int> dims;
};

// A value in a Graph: an index into it, meaningless in any other graph. A
// default-constructed Tensor is invalid, which is what every op returns
// after recording an error.
struct Tensor
{
    bool isValid() const { return id >= 0; }

    bool operator==(const Tensor& other) const = default;

    int id = -1;
};
} // namespace eacp::ML
