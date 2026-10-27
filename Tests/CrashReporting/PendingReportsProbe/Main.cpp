#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/App/Args.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/CrashReporting/CrashReporting.h>

// Starts the crash reporter on the database it is given and prints, one line
// each, how many pendingReportFound calls start() made, then the id of every
// report delivered once the loop runs. What Tests/CrashReporting reads.

using namespace eacp;

namespace
{
constexpr auto deliveryTimeout = Time::MS {5000};

Vector<std::string>& getDeliveredIds()
{
    static auto ids = Vector<std::string> {};
    return ids;
}

struct PendingReportsProbeHandler final : CrashReporting::CrashHandler
{
    void pendingReportFound(const CrashReporting::CrashReport& report) override
    {
        getDeliveredIds().add(report.id);
    }
};

EACP_REGISTER_CRASH_HANDLER(PendingReportsProbeHandler);
} // namespace

int main(int argc, char* argv[])
{
    if (auto code = CrashReporting::runHandlerIfRequested(argc, argv))
        return *code;

    Apps::setCommandLineArgs(argc, argv);

    auto databaseDirectory = std::string {};
    auto expectedCount = 0;
    auto cli = Args::Parser {"PendingReportsProbe"};
    cli.option("--database", databaseDirectory, "the crash database");
    cli.option("--expect", expectedCount, "how many reports to wait for");

    if (auto code = cli.exitCodeFor(cli.parse()))
        return *code;

    auto options = CrashReporting::Options {};
    options.databaseDirectory = FilePath {databaseDirectory};
    CrashReporting::start(options);

    Console::print("during start: " + std::to_string(getDeliveredIds().size()));

    auto& ids = getDeliveredIds();
    Threads::runEventLoopUntil(
        [&] { return static_cast<int>(ids.size()) >= expectedCount; },
        deliveryTimeout);

    for (const auto& id: ids)
        Console::print("delivered: " + id);

    return 0;
}
