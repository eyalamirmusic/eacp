#include "Common.h"

using namespace nano;
namespace Proc = eacp::Processes;

// A Cocoa terminate: must unwind run<T>() and destroy the app before main
// returns, like Apps::quit() — not fall through to exit().
auto tCocoaTerminateUnwindsRun =
    test("App/cocoaTerminateUnwindsRunAndDestroysApp") = []
{
    auto result = Proc::run(EACP_TERMINATION_HARNESS, {});

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 0);
    check(result.output == "app-destroyed\nrun-returned\n");
};

// quit(returnValue) must flow out of run<T>() so main can return it as the
// process exit code.
auto tQuitReturnValueBecomesExitCode =
    test("App/quitReturnValueBecomesProcessExitCode") = []
{
    auto result = Proc::run(EACP_TERMINATION_HARNESS, {"quit-code"});

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 42);
    check(result.output == "app-destroyed\nrun-returned\n");
};
