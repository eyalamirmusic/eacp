#include "Process.h"

#include <thread>
#include <utility>

namespace eacp::Processes
{
Process::Process(const std::string& executable, const Vector<std::string>& arguments)
    : Process(ProcessOptions {executable, arguments, {}, {}})
{
}

ProcessResult run(ProcessOptions options)
{
    auto process = Process {std::move(options)};

    auto result = ProcessResult {};
    result.launched = process.launched();

    if (!result.launched)
        return result;

    result.exitCode = process.wait();
    result.exited = true;
    result.output = process.output();
    result.errorOutput = process.errorOutput();
    return result;
}

ProcessResult run(const std::string& executable,
                  const Vector<std::string>& arguments)
{
    return run(ProcessOptions {executable, arguments, {}, {}});
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
