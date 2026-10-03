#include "LoggingPlatform.h"

#include <android/log.h>

#include <string>

namespace eacp::Detail
{

std::tm localTime(std::time_t time)
{
    auto result = std::tm {};
    localtime_r(&time, &result);
    return result;
}

void platformDebugOutput(std::string_view line)
{
    auto text = std::string {line};
    __android_log_write(ANDROID_LOG_INFO, "eacp", text.c_str());
}

} // namespace eacp::Detail
