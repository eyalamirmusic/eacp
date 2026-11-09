#import <Metal/Metal.h>

#include "MetalErrors-Apple.h"

#include <eacp/Core/Utils/Logging.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace eacp::GPU::Detail
{
namespace
{
bool mentions(NSString* text, NSString* phrase)
{
    return [text rangeOfString:phrase options:NSCaseInsensitiveSearch].location
           != NSNotFound;
}

bool namesTheSource(NSString* description)
{
    return mentions(description, @"program_source") || mentions(description, @"error:");
}

// Newer Metal retries a crashed compiler service itself and says so. What is
// still failing after that is crashing it every time - a kernel past what the
// compiler can take - and another round would only crash it again.
bool metalAlreadyRetried(NSString* description)
{
    return mentions(description, @"after multiple retries");
}

bool saysOnlyThatItFailed(NSString* description)
{
    auto trimmed = [description
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];

    return [trimmed caseInsensitiveCompare:@"Compilation failed"] == NSOrderedSame
           || mentions(trimmed, @"internal error") || mentions(trimmed, @"interrupted")
           || mentions(trimmed, @"MTLCompilerService");
}

// The userInfo less the description, which is logged on its own line already.
std::string extraInfo(NSError* error)
{
    auto info = [NSMutableDictionary dictionaryWithDictionary:error.userInfo];
    [info removeObjectForKey:NSLocalizedDescriptionKey];

    if (info.count == 0)
        return {};

    return info.description.UTF8String;
}

std::string describe(NSError* error)
{
    if (error == nil)
        return "no error was reported";

    auto text = std::string("domain ") + error.domain.UTF8String + ", code "
                + std::to_string((long) error.code) + ": "
                + error.localizedDescription.UTF8String;

    if (auto extra = extraInfo(error); !extra.empty())
        text += "\n" + extra;

    return text;
}

constexpr auto commandBufferFailuresLogged = 16;
std::atomic<int> commandBufferFailures {0};

void logCommandBufferFailure(id<MTLCommandBuffer> buffer)
{
    const auto index = ++commandBufferFailures;

    if (index > commandBufferFailuresLogged + 1)
        return;

    if (index == commandBufferFailuresLogged + 1)
    {
        LOG("eacp: further Metal command buffer failures are not logged");
        return;
    }

    auto label = buffer.label != nil ? std::string(buffer.label.UTF8String) : "";

    LOG("eacp: a Metal command buffer failed on the GPU",
        label.empty() ? "" : " (" + label + ")",
        ", so none of its work landed - ",
        describe(buffer.error));
}
} // namespace

bool isTransientBuildFailure(NSError* error)
{
    if (error == nil)
        return true;

    const auto description = error.localizedDescription;

    if (namesTheSource(description) || metalAlreadyRetried(description))
        return false;

    if ([error.domain isEqualToString:MTLLibraryErrorDomain]
        && error.code == MTLLibraryErrorInternal)
        return true;

    return saysOnlyThatItFailed(description);
}

void logBuildFailure(const char* what, NSError* error, int attempt, bool retrying)
{
    const auto firstAndFinal = attempt == 1 && !retrying;
    const auto progress = firstAndFinal
                              ? std::string()
                              : " (attempt " + std::to_string(attempt) + " of "
                                    + std::to_string(buildAttempts)
                                    + (retrying ? ", retrying)" : ")");

    LOG("eacp: ", what, " failed", progress, " - ", describe(error));
}

void waitBeforeBuildRetry(int attempt)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(50 * attempt * attempt));
}

void reportFailureOf(id<MTLCommandBuffer> buffer)
{
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        if (finished.status == MTLCommandBufferStatusError)
            logCommandBufferFailure(finished);
    }];
}
} // namespace eacp::GPU::Detail
