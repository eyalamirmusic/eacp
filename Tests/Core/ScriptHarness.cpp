#include <eacp/Core/Core.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// A stand-in for a build command, the same program on every platform, run by
// the hot-reload and BuildRunner tests in place of a shell. Its arguments are
// steps, run in order (HarnessScript.h builds them):
//
//   print <text>          <text> and a newline to stdout
//   printError <text>     the same to stderr
//   sleep <ms>            waits
//   exit <code>           stops here with that exit code
//   copy <from> <to>      copies a file, or stops with exit code 2
//   write <path> <text>   replaces a file's contents
//   ifExists <path>       runs the next step only if <path> exists
//   unlessExists <path>   runs the next step only if it does not
//   spawnSleeping <ms>    starts another copy that sleeps, sharing this one's
//                         stdio, and does not wait for it
namespace
{
using Arguments = eacp::Vector<std::string>;

void useBinaryStdio()
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

void keepRunningPastExit(eacp::Processes::Process* process)
{
    static auto* kept = new eacp::Vector<eacp::Processes::Process*> {};
    kept->add(process);
}

void spawnSleeping(const std::string& milliseconds)
{
    auto options = eacp::Processes::ProcessOptions {};
    options.executable = eacp::Files::executablePath().str();
    options.arguments = {"sleep", milliseconds};
    options.captureOutput = false;
    keepRunningPastExit(new eacp::Processes::Process {options});
}

struct Script
{
    explicit Script(const Arguments& argumentsToUse)
        : arguments(argumentsToUse)
    {
    }

    int run()
    {
        while (position < arguments.size())
        {
            auto step = next();

            if (step == "exit")
                return std::stoi(next());

            if (!runStep(step))
                return 2;
        }

        return 0;
    }

    bool runStep(const std::string& step)
    {
        if (step == "print")
            eacp::Console::print(next());
        else if (step == "printError")
            eacp::Console::printError(next());
        else if (step == "sleep")
            std::this_thread::sleep_for(
                std::chrono::milliseconds {std::stoi(next())});
        else if (step == "copy")
            return copy();
        else if (step == "write")
            return write();
        else if (step == "ifExists")
            skipNextUnless(eacp::Files::exists(eacp::FilePath {next()}));
        else if (step == "unlessExists")
            skipNextUnless(!eacp::Files::exists(eacp::FilePath {next()}));
        else if (step == "spawnSleeping")
            spawnSleeping(next());
        else
            return fail("unknown step " + step);

        return true;
    }

    bool copy()
    {
        auto from = eacp::FilePath {next()};
        auto to = eacp::FilePath {next()};

        if (eacp::Files::copyFile(from, to))
            return true;

        return fail("cannot copy " + from.str() + " to " + to.str());
    }

    bool write()
    {
        auto path = eacp::FilePath {next()};
        auto text = next();

        try
        {
            eacp::Files::writeFile(path, text);
            return true;
        }
        catch (const std::exception& e)
        {
            return fail(e.what());
        }
    }

    void skipNextUnless(bool condition)
    {
        if (condition || position >= arguments.size())
            return;

        auto step = next();

        for (auto count = argumentCountOf(step); count > 0; --count)
            next();
    }

    static int argumentCountOf(const std::string& step)
    {
        if (step == "copy" || step == "write")
            return 2;

        return 1;
    }

    static bool fail(const std::string& reason)
    {
        eacp::Console::printError("ScriptHarness: " + reason);
        return false;
    }

    std::string next()
    {
        return position < arguments.size() ? arguments[position++] : std::string {};
    }

    Arguments arguments;
    int position = firstStep;

    static constexpr auto firstStep = 1;
};
} // namespace

int main(int argc, char* argv[])
{
    useBinaryStdio();

    return Script {eacp::Apps::utf8Arguments(argc, argv)}.run();
}
