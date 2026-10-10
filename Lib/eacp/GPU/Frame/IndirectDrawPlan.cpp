#include "IndirectDrawPlan.h"

#include <eacp/Core/Utils/Logging.h>

namespace eacp::GPU
{
namespace
{
bool isWordAligned(std::int64_t value)
{
    return value >= 0 && value % 4 == 0;
}

std::int64_t lastRecordEnd(const IndirectDrawPlan& plan, std::int64_t recordBytes)
{
    return plan.offset + (std::int64_t) (plan.drawCount - 1) * plan.stride
           + recordBytes;
}
} // namespace

std::optional<IndirectDrawPlan> planIndirectDraws(const Buffer& arguments,
                                                  std::int64_t offset,
                                                  int drawCount,
                                                  std::int64_t stride,
                                                  std::int64_t recordBytes)
{
    if (drawCount < 1)
        return {};

    if (!arguments.isValid())
    {
        LOG("eacp: an indirect draw names an invalid argument buffer");
        return {};
    }

    auto plan =
        IndirectDrawPlan {offset, drawCount, stride > 0 ? stride : recordBytes};

    if (!isWordAligned(plan.offset) || !isWordAligned(plan.stride)
        || plan.stride < recordBytes)
    {
        LOG("eacp: an indirect draw's offset and stride must be multiples of four "
            "and the stride no shorter than one record");
        return {};
    }

    if (lastRecordEnd(plan, recordBytes) > arguments.size())
    {
        LOG("eacp: an indirect draw reads past the end of its argument buffer");
        return {};
    }

    return plan;
}

bool isValidDrawCountBuffer(const Buffer& count, std::int64_t countOffset)
{
    if (count.isValid() && isWordAligned(countOffset)
        && countOffset + (std::int64_t) sizeof(std::uint32_t) <= count.size())
        return true;

    LOG("eacp: an indirect-count draw's count buffer is invalid or too short");
    return false;
}

void traceIndirectCountUnsupported()
{
    LOG("eacp: this device has no indirect-count draws "
        "(Device::supportsIndirectCount); the draw is refused");
}
} // namespace eacp::GPU
