// Linked into every web executable as a --post-js (CMake/Web.cmake).
//
// A C++ exception that leaves main(), or a callback the runtime called, ends
// a native program in std::terminate. Emscripten instead hands it to quit_,
// which under node exits non-zero but in a page only throws it on. A main()
// linked with the nested pump runs as a promise, so in a page that throw
// becomes an unhandled rejection, nothing reports an exit, and the program
// neither ends nor says why. Aborting with the exception's message instead
// ends it everywhere: the message is printed and Module.onAbort is called.
//
// getExceptionMessage is in the output only when something asks for it:
// eacp_web_link_options exports it (EXPORTED_RUNTIME_METHODS), since a
// --post-js cannot declare library dependencies, and an optimised build
// without ASSERTIONS or EXCEPTION_STACK_TRACES otherwise leaves it out.
{
    const eacpQuit = quit_;

    const eacpDescribe = exception =>
    {
        if (typeof getExceptionMessage === 'function')
        {
            try
            {
                return 'uncaught exception: ' + getExceptionMessage(exception).join(': ');
            }
            catch {}
        }

        return 'uncaught exception';
    };

    quit_ = (status, toThrow) =>
    {
        if (toThrow instanceof WebAssembly.Exception && !ABORT)
            abort(eacpDescribe(toThrow));

        eacpQuit(status, toThrow);
    };
}
