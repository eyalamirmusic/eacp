#import <Metal/Metal.h>

#include "Common.h"

#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/GPU/Device/MetalErrors-Apple.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// Which Metal build failures are retried. The compiler service failing under
// load says nothing about the source, and is worth another try; a failure that
// names what is wrong with the source or the pipeline is the same on every try,
// and must come back at once.
namespace MetalErrorsTests
{
namespace
{
NSError* metalError(NSString* domain, NSInteger code, NSString* description)
{
    return [NSError errorWithDomain:domain
                               code:code
                           userInfo:@{NSLocalizedDescriptionKey: description}];
}
} // namespace

auto tServiceFailuresAreTransient =
    test("MetalErrors/aFailureThatSaysNothingAboutTheSourceIsRetried") = []
{
    ObjC::AutoReleasePool pool;

    check(Detail::isTransientBuildFailure(nil), "failed without an error");
    check(Detail::isTransientBuildFailure(metalError(
              MTLLibraryErrorDomain, MTLLibraryErrorCompileFailure, @"Compilation failed")),
          "the bare message CI's virtualised GPU reports");
    check(Detail::isTransientBuildFailure(metalError(
        MTLLibraryErrorDomain, MTLLibraryErrorInternal, @"Something went wrong")));
    check(Detail::isTransientBuildFailure(
              metalError(@"AGXMetalG13X", 3, @"Compiler encountered an internal error")),
          "a pipeline build whose backend compiler fell over");
    check(Detail::isTransientBuildFailure(metalError(
        @"MTLLibraryErrorDomain", 3, @"Compiler service connection interrupted")));
};

auto tDiagnosticsAreNotTransient =
    test("MetalErrors/aFailureThatNamesTheProblemIsNotRetried") = []
{
    ObjC::AutoReleasePool pool;

    check(!Detail::isTransientBuildFailure(metalError(
              MTLLibraryErrorDomain,
              MTLLibraryErrorCompileFailure,
              @"program_source:2:55: error: use of undeclared identifier 'x'")),
          "a diagnostic against the source");
    check(!Detail::isTransientBuildFailure(metalError(
              @"AGXMetalG17X",
              3,
              @"Fragment input(s) `user(locn0)` mismatching vertex shader output "
              @"type(s) or not written by vertex shader")),
          "a pipeline whose stages do not link");
    check(!Detail::isTransientBuildFailure(metalError(
        MTLLibraryErrorDomain, MTLLibraryErrorFunctionNotFound, @"Function k not found")));
    check(!Detail::isTransientBuildFailure(metalError(
              @"AGXMetalG17X",
              2,
              @"Compilation failed due to an interrupted connection: "
              @"XPC_ERROR_CONNECTION_INTERRUPTED. This error occurred after multiple "
              @"retries.")),
          "a crash Metal has already retried, which a kernel causes every time");
};

// End to end: a source that does not compile fails on its first attempt, with
// no retry delaying it - the same failure on every attempt, so a retry would
// only be the wait before reporting it.
auto tBrokenSourceFailsAtOnce = test("MetalErrors/aBrokenSourceFailsOnItsFirstTry") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    ObjC::AutoReleasePool pool;

    auto source = ShaderSource {};
    source.source = "kernel void broken(device float* o [[buffer(0)]]) "
                    "{ o[0] = notDeclaredAnywhere; }\n";
    source.computeEntry = "broken";

    auto attempts = 0;
    auto compile = [&](NSError** error)
    {
        ++attempts;
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();
        return [metalDevice newLibraryWithSource:@(source.source.c_str())
                                         options:nil
                                           error:error];
    };

    id<MTLLibrary> library = Detail::buildRetryingTransientFailures<id<MTLLibrary>>(
        "compiling a deliberately broken test shader", compile);

    check(library == nil);
    check(attempts == 1);
    check(!ShaderLibrary(device, source).isValid());
};

auto tTransientFailureIsRetried =
    test("MetalErrors/aTransientFailureIsRetriedUntilItBuilds") = []
{
    ObjC::AutoReleasePool pool;

    auto attempts = 0;
    auto failOnceThenBuild = [&](NSError** error) -> NSObject*
    {
        if (++attempts == 1)
        {
            *error = metalError(
                MTLLibraryErrorDomain, MTLLibraryErrorCompileFailure, @"Compilation failed");
            return nil;
        }

        return [[NSObject alloc] init];
    };

    auto* built = Detail::buildRetryingTransientFailures<NSObject*>(
        "a build that fails once", failOnceThenBuild);

    check(built != nil);
    check(attempts == 2);
    [built release];

    auto alwaysFailing = [&](NSError** error) -> NSObject*
    {
        ++attempts;
        *error = metalError(
            MTLLibraryErrorDomain, MTLLibraryErrorCompileFailure, @"Compilation failed");
        return nil;
    };

    attempts = 0;
    check(Detail::buildRetryingTransientFailures<NSObject*>("a build that never works",
                                                            alwaysFailing)
          == nil);
    check(attempts == Detail::buildAttempts, "and gives up after a bounded number");
};
} // namespace MetalErrorsTests
