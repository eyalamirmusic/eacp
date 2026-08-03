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

// A quit handler must be able to refuse a real Cocoa terminate: — the app
// stays up, and only Apps::quit() ends it. Both halves matter: without the
// refusal a tray app cannot survive Cmd+Q, and without quit() bypassing the
// handler it could never be quit at all.
auto tQuitHandlerRefusesCocoaTerminate =
    test("App/quitHandlerCanRefuseCocoaTerminate") = []
{
    auto result = Proc::run(EACP_TERMINATION_HARNESS, {"refuse-quit"});

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 0);
    check(result.output == "quit-refused\napp-destroyed\nrun-returned\n");
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
