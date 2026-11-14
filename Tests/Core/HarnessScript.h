#pragma once

#include <eacp/Core/Core.h>

// A command for ScriptHarness (ScriptHarness.cpp), the portable stand-in for
// `sh -c` the BuildRunner and hot-reload tests run: each call adds a step, and
// options() is the command that runs them in order. Nothing runs with no step.
class HarnessScript
{
public:
    HarnessScript& print(const std::string& text) { return add("print", text); }

    HarnessScript& printError(const std::string& text)
    {
        return add("printError", text);
    }

    HarnessScript& sleep(int milliseconds)
    {
        return add("sleep", std::to_string(milliseconds));
    }

    HarnessScript& exit(int code) { return add("exit", std::to_string(code)); }

    HarnessScript& copy(const eacp::FilePath& from, const eacp::FilePath& to)
    {
        return add("copy", from.str(), to.str());
    }

    HarnessScript& write(const eacp::FilePath& path, const std::string& text)
    {
        return add("write", path.str(), text);
    }

    // The step added next runs only if `path` exists, or only if it does not.
    HarnessScript& ifExists(const eacp::FilePath& path)
    {
        return add("ifExists", path.str());
    }

    HarnessScript& unlessExists(const eacp::FilePath& path)
    {
        return add("unlessExists", path.str());
    }

    // Starts another harness that sleeps, holding this one's stdout and stderr
    // open, and goes on without waiting for it.
    HarnessScript& spawnSleeping(int milliseconds)
    {
        return add("spawnSleeping", std::to_string(milliseconds));
    }

    HarnessScript& then(const HarnessScript& other)
    {
        for (const auto& argument: other.arguments)
            arguments.add(argument);

        return *this;
    }

    eacp::Processes::ProcessOptions options() const
    {
        auto options = eacp::Processes::ProcessOptions {};
        options.executable = EACP_SCRIPT_HARNESS;
        options.arguments = arguments;
        return options;
    }

private:
    template <typename... Texts>
    HarnessScript& add(const Texts&... texts)
    {
        (arguments.add(std::string {texts}), ...);
        return *this;
    }

    eacp::Vector<std::string> arguments;
};
