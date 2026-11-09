#pragma once

#import <Metal/Metal.h>

namespace eacp::GPU::Detail
{
// Metal compiles in a separate process, MTLCompilerService, reached over XPC.
// On a loaded or virtualised machine that service now and then fails a build
// with no diagnostic at all - an internal error, or a bare "Compilation failed"
// - and the same source builds on the next call. A failure that names what is
// wrong with the source or the pipeline is never one of these, and is never
// retried.
bool isTransientBuildFailure(NSError* error);

void logBuildFailure(const char* what, NSError* error, int attempt, bool retrying);

void waitBeforeBuildRetry(int attempt);

constexpr auto buildAttempts = 3;

// Runs `build` - a Metal new...:error: call returning a +1 object - until it
// succeeds, fails with a diagnostic, or has failed transiently buildAttempts
// times. Every failure is logged in full, so one that is retried still leaves
// a trace.
template <typename Result, typename Build>
Result buildRetryingTransientFailures(const char* what, const Build& build)
{
    for (auto attempt = 1;; ++attempt)
    {
        NSError* error = nil;
        Result result = build(&error);

        if (result != nil)
            return result;

        const auto retrying =
            attempt < buildAttempts && isTransientBuildFailure(error);

        logBuildFailure(what, error, attempt, retrying);

        if (!retrying)
            return nil;

        waitBeforeBuildRetry(attempt);
    }
}

// A command buffer that fails on the GPU fails silently: its work simply never
// lands. This logs the failure - status, domain, code, description - when the
// buffer completes, so a frame that read back nothing says why. Call it before
// the buffer is committed.
void reportFailureOf(id<MTLCommandBuffer> buffer);
} // namespace eacp::GPU::Detail
