#include "UncaughtExceptions-macOS.h"

#import <Foundation/Foundation.h>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto uncaughtExceptionReason = std::string_view {"NSException"};

std::string_view getUTF8View(NSString* text)
{
    if (text == nil)
        return {};

    return text.UTF8String;
}

void recordUncaughtException(NSException* exception)
{
    recordCrashMessage(uncaughtExceptionReason,
                       getUTF8View(exception.name),
                       getUTF8View(exception.reason));
}
} // namespace

void installUncaughtExceptionRecorder()
{
    NSSetUncaughtExceptionHandler(&recordUncaughtException);
}
} // namespace eacp::CrashReporting::Detail
