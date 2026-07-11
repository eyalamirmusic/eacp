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
