#include "Process.h"

#include <thread>

namespace eacp::Processes
{
namespace
{
ProcessOptions optionsFor(const std::string& executable,
                          const Vector<std::string>& arguments)
{
    auto options = ProcessOptions {};
    options.executable = executable;
    options.arguments = arguments;
    return options;
}

bool isBreak(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool isQuote(char c)
{
    return c == '"' || c == '\'';
}

std::string unquoted(const std::string& token)
{
    auto start = std::size_t {isQuote(token.front()) ? 1u : 0u};
    auto end = token.size() - (isQuote(token.back()) ? 1u : 0u);

    if (end <= start)
        return {};

    return token.substr(start, end - start);
}
} // namespace

Process::Process(const std::string& executable, const Vector<std::string>& arguments)
    : Process(optionsFor(executable, arguments))
{
}

Vector<std::string> tokeniseCommandLine(std::string_view line)
{
    auto tokens = Vector<std::string> {};
    auto current = std::string {};
    auto inQuotes = false;

    auto finishToken = [&]
    {
        if (!current.empty())
            tokens.push_back(std::move(current));

        current.clear();
    };

    for (auto c: line)
    {
        if (!inQuotes && isBreak(c))
        {
            finishToken();
            continue;
        }

        if (c == '"')
            inQuotes = !inQuotes;

        current.push_back(c);
    }

    finishToken();

    if (!tokens.empty())
        tokens.front() = unquoted(tokens.front());

    return tokens;
}

ProcessResult run(ProcessOptions options)
{
    auto process = Process {std::move(options)};

    auto result = ProcessResult {};
    result.launched = process.launched();

    if (!result.launched)
        return result;

    process.closeInput();
    result.exitCode = process.wait();
    result.exited = true;
    result.output = process.output();
    result.errorOutput = process.errorOutput();
    return result;
}

ProcessResult run(const std::string& executable,
                  const Vector<std::string>& arguments)
{
    return run(optionsFor(executable, arguments));
}

Threads::Async<ProcessResult> runAsync(ProcessOptions options)
{
    auto promise = Threads::AsyncPromise<ProcessResult> {};

    std::thread(
        [promise, options = std::move(options)]() mutable
        {
            auto result = run(std::move(options));
            Threads::callAsync([promise, result = std::move(result)]() mutable
                               { promise.resolve(std::move(result)); });
        })
        .detach();

    return promise.get();
}
} // namespace eacp::Processes
