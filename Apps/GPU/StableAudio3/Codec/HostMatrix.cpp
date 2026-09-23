#include "HostMatrix.h"

namespace eacp::SA3Codec
{
HostMatrix zeroPadRowsToMultiple(const HostMatrix& input, int multiple)
{
    auto remainder = input.rows % multiple;
    auto padRows = remainder == 0 ? 0 : multiple - remainder;

    if (padRows == 0)
        return input;

    auto result = HostMatrix::zeros(input.rows + padRows, input.cols);
    writeRows(result, 0, input);
    return result;
}

HostMatrix extractRows(const HostMatrix& input, int startRow, int rowCount)
{
    auto result = HostMatrix::zeros(rowCount, input.cols);

    for (auto row = 0; row < rowCount; ++row)
        for (auto col = 0; col < input.cols; ++col)
            result.at(row, col) = input.at(startRow + row, col);

    return result;
}

void writeRows(HostMatrix& destination, int startRow, const HostMatrix& source)
{
    for (auto row = 0; row < source.rows; ++row)
        for (auto col = 0; col < source.cols; ++col)
            destination.at(startRow + row, col) = source.at(row, col);
}

HostMatrix concatRows(const HostMatrix& a, const HostMatrix& b, const HostMatrix& c)
{
    auto result = HostMatrix::zeros(a.rows + b.rows + c.rows, a.cols);
    writeRows(result, 0, a);
    writeRows(result, a.rows, b);
    writeRows(result, a.rows + b.rows, c);
    return result;
}

HostMatrix addMatrices(const HostMatrix& a, const HostMatrix& b)
{
    auto result = a;

    for (auto i = std::size_t {}; i < result.data.size(); ++i)
        result.data[i] += b.data[i];

    return result;
}

HostMatrix subtractMatrices(const HostMatrix& a, const HostMatrix& b)
{
    auto result = a;

    for (auto i = std::size_t {}; i < result.data.size(); ++i)
        result.data[i] -= b.data[i];

    return result;
}

HostMatrix sliceColumns(const HostMatrix& input, int startColumn, int columnCount)
{
    auto result = HostMatrix::zeros(input.rows, columnCount);

    for (auto row = 0; row < input.rows; ++row)
        for (auto col = 0; col < columnCount; ++col)
            result.at(row, col) = input.at(row, startColumn + col);

    return result;
}
}
