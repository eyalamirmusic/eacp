// Linked into every web executable as a --post-js (CMake/Web.cmake).
//
// A C++ exception that leaves main(), or a callback the runtime called, ends
// a native program in std::terminate. Emscripten instead hands it to quit_,
// which under node exits non-zero but in a page only throws it on. A main()
// linked with the nested pump runs as a promise, so in a page that throw
// becomes an unhandled rejection, nothing reports an exit, and the program
// neither ends nor says why. Aborting with the exception's message instead
// ends it everywhere: the message is printed and Module.onAbort is called.
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
