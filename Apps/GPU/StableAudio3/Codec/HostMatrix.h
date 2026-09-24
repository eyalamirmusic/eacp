#pragma once

#include <cstddef>
#include <vector>

namespace eacp::SA3Codec
{
struct HostMatrix
{
    std::vector<float> data;
    int rows = 0;
    int cols = 0;

    float& at(int row, int col)
    {
        return data[(std::size_t) row * (std::size_t) cols + (std::size_t) col];
    }

    float at(int row, int col) const
    {
        return data[(std::size_t) row * (std::size_t) cols + (std::size_t) col];
    }

    static HostMatrix zeros(int rows, int cols)
    {
        return HostMatrix {
            std::vector<float>((std::size_t) rows * (std::size_t) cols, 0.f), rows, cols};
    }
};

HostMatrix zeroPadRowsToMultiple(const HostMatrix& input, int multiple);
HostMatrix extractRows(const HostMatrix& input, int startRow, int rowCount);
void writeRows(HostMatrix& destination, int startRow, const HostMatrix& source);
HostMatrix concatRows(const HostMatrix& a, const HostMatrix& b, const HostMatrix& c);
HostMatrix addMatrices(const HostMatrix& a, const HostMatrix& b);
HostMatrix subtractMatrices(const HostMatrix& a, const HostMatrix& b);
HostMatrix sliceColumns(const HostMatrix& input, int startColumn, int columnCount);
}
