#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/CrashReporting/FallbackReport.h>

#include <NanoTest/NanoTest.h>

#include <memory>

using namespace nano;
using namespace eacp;
using namespace eacp::CrashReporting;

namespace FallbackReportTests
{
constexpr auto moduleBase = std::uint64_t {0x100000000};
constexpr auto moduleSize = std::uint64_t {0x10000};
constexpr auto faultOffset = std::uint64_t {0x1234};
constexpr auto returnOffset = std::uint64_t {0x2345};
constexpr auto throwOffset = std::uint64_t {0x3456};
constexpr auto faultAddress = std::uint64_t {0x10};
constexpr auto threadId = std::uint64_t {0xABCDEF};
constexpr auto testProcessId = std::uint64_t {1};

std::unique_ptr<Detail::SharedState> makeCrashedState()
{
    auto state = std::make_unique<Detail::SharedState>();
    Detail::initialiseSharedState(*state, testProcessId);

    auto& crash = state->crash;
    Detail::copyBoundedText(crash.reason, "SIGSEGV");
    Detail::copyBoundedText(crash.message, "a \"quoted\"\\ message\nwith a newline");
    crash.faultAddress = faultAddress;
    crash.threadId = threadId;
    crash.frames[0] = moduleBase + faultOffset;
    crash.frames[1] = moduleBase + returnOffset;
    crash.frameCount = 2;
    crash.modules[0].base = moduleBase;
    crash.modules[0].size = moduleSize;
    Detail::copyBoundedText(crash.modules[0].path, "/Applications/App.app/App");
    crash.moduleCount = 1;

    Detail::writeSharedEntry(*state, state->tags, "tier", "pro");
    Detail::writeSharedEntry(*state, state->metadata, "appName", "App");
    Detail::writeSharedUser(*state, {"42", "someone@example.com", "someone"});
    return state;
}

std::optional<CrashReport> writeAndReadBack(const Detail::SharedState& state)
{
    auto database = TemporaryFile {};
    auto reports = ReportDatabase {database.path()};
    Files::createDirectories(reports.getPendingDirectory());

    Detail::prepareFallbackReport(reports);
    check(Detail::writeFallbackReport(state));
    check(reports.hasCrashedLastRun());

    auto pending = reports.readPendingReports();
    check(pending.size() == 1);

    if (pending.size() != 1)
        return std::nullopt;

    return pending[0];
}

auto tReadsBackWhatTheCrashWrote = test("FallbackReport/readsBackAsAReport") = []
{
    auto state = makeCrashedState();
    auto report = writeAndReadBack(*state);

    if (!report)
        return;

    check(report->capturedBy == CaptureSource::app);
    check(report->kind == ReportKind::crash);
    check(report->reason == "SIGSEGV");
    check(report->message == "a \"quoted\"\\ message\nwith a newline");
    check(report->faultAddress == faultAddress);
    check(report->crashingThread.threadId == threadId);
    check(report->crashingThread.frames.size() == 2);
    check(report->crashingThread.frames[0].modulePath
          == "/Applications/App.app/App");
    check(report->crashingThread.frames[0].moduleBase == moduleBase);
    check(report->crashingThread.frames[0].offset == faultOffset);
    check(report->crashingThread.frames[1].offset == returnOffset);
    check(report->tags.at("tier") == "pro");
    check(report->metadata.at("appName") == "App");
    check(report->user.email == "someone@example.com");
    check(!report->id.empty());
    check(report->timestamp > 0);
};

auto tPrefersTheThrowSite = test("FallbackReport/prefersTheThrowSite") = []
{
    auto state = makeCrashedState();
    state->crash.throwFrames[0] = moduleBase + throwOffset;
    state->crash.throwFrameCount = 1;

    auto report = writeAndReadBack(*state);

    if (!report)
        return;

    check(report->crashingThread.frames.size() == 1);
    check(report->crashingThread.frames[0].offset == throwOffset);
};
} // namespace FallbackReportTests
