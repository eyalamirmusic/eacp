#pragma once

// A nested pump on the web suspends the main thread through JSPI, which only
// an executable linked with -sJSPI can do. eacp-core defines these weakly as
// "cannot suspend"; eacp-core-nested-pump (NestedPump-Web.cpp) replaces them
// in the executables that link it, which are the test suites.
extern "C"
{
    bool eacpWebCanSuspend();

    // A negative timeout waits until woken. A wake with nobody suspended is kept
    // for the next suspension, which then returns at once.
    void eacpWebSuspendUntilWoken(double timeoutMs);
    void eacpWebWakeSuspended();
}
