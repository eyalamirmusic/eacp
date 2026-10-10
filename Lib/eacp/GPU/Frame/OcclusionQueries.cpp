#include "OcclusionQueries.h"

// The members every backend answers the same way, on top of the per-backend
// read().

namespace eacp::GPU
{
std::uint64_t OcclusionQueries::result(int index) const
{
    if (index < 0 || index >= size())
        return 0;

    auto value = std::uint64_t {0};
    read(std::span<std::uint64_t> {&value, 1}, index);
    return value;
}
} // namespace eacp::GPU
