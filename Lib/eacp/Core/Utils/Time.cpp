#include "Time.h"

#include "Format.h"

#include <chrono>
#include <thread>

namespace eacp::Time
{
namespace
{
std::int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void sleep(MS duration)
{
    std::this_thread::sleep_for(std::chrono::milliseconds {duration.count});
}

void sleepMS(int ms)
{
    sleep(MS {ms});
}

MS sinceEpoch()
{
    using namespace std::chrono;
    const auto now = system_clock::now().time_since_epoch();
    return MS {duration_cast<milliseconds>(now).count()};
}

std::string formatIso8601(MS sinceUnixEpoch)
{
    using namespace std::chrono;
    const auto time = sys_time<milliseconds> {milliseconds {sinceUnixEpoch.count}};
    const auto day = floor<days>(time);
    const auto date = year_month_day {day};
    const auto clock = hh_mm_ss {time - day};
    const auto year = static_cast<int>(date.year());
    const auto month = static_cast<unsigned>(date.month());
    const auto dayOfMonth = static_cast<unsigned>(date.day());
    return Strings::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                           year,
                           month,
                           dayOfMonth,
                           clock.hours().count(),
                           clock.minutes().count(),
                           clock.seconds().count(),
                           clock.subseconds().count());
}

Deadline::Deadline(MS timeout)
    : end(nowMs() + timeout.count)
{
}

bool Deadline::expired() const
{
    return nowMs() >= end;
}

MS Deadline::remaining() const
{
    auto left = end - nowMs();
    return MS {left > 0 ? left : 0};
}
} // namespace eacp::Time
