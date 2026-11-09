#include "Common.h"

#include <eacp/Core/Utils/Time.h>

using namespace nano;
using eacp::Time::formatIso8601;
using eacp::Time::MS;

namespace
{
constexpr auto unixEpoch = MS {0};
constexpr auto selfDestructCrash = MS {1793126882569};
constexpr auto leapDayPlusOneMillisecond = MS {951782400001};
} // namespace

auto tFormatsIso8601 = test("Time/formatIso8601") = []
{
    check(formatIso8601(unixEpoch) == "1970-01-01T00:00:00.000Z");
    check(formatIso8601(selfDestructCrash) == "2026-10-27T18:48:02.569Z");
    check(formatIso8601(leapDayPlusOneMillisecond) == "2000-02-29T00:00:00.001Z");
};

auto tSinceEpochIsWallClock = test("Time/sinceEpochIsWallClock") = []
{ check(eacp::Time::sinceEpoch() > selfDestructCrash); };
