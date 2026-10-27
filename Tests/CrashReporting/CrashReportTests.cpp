#include <eacp/CrashReporting/CrashReport.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp::CrashReporting;

namespace
{
constexpr auto addressAboveDoublePrecision = std::uint64_t {0xFFFF'8000'1234'5679};

// report.json as written before frames carried moduleUuid.
constexpr auto reportWithoutModuleUuid = R"({
  "id": "0f5e0c2a-8f5b-4d0e-9d6e-6b8a1a2b3c4d",
  "timestamp": 1793060881563,
  "reason": "SIGSEGV",
  "message": "",
  "faultAddress": "0x0",
  "crashingThread": {
    "threadId": 7,
    "frames": [{"modulePath": "/bin/app", "moduleBase": "0x100000000",
                "offset": "0x647c", "symbol": "main + 4"}]
  },
  "minidumpPath": "",
  "metadata": {},
  "attachments": [],
  "platform": "macOS",
  "capturedBy": "handler"
})";

CrashReport makeSampleReport()
{
    auto report = CrashReport {};
    report.id = "0f5e0c2a-8f5b-4d0e-9d6e-6b8a1a2b3c4d";
    report.timestamp = 1793060881563;
    report.reason = "SIGSEGV";
    report.message = "std::runtime_error: \"quoted\"\nnext line";
    report.faultAddress = addressAboveDoublePrecision;
    report.crashingThread.threadId = 42;
    report.crashingThread.frames.add({"/bin/app", 0x100000000, 0x647c, "main + 4"});
    report.metadata["project"] = "tests";
    report.tags["tier"] = "free";
    report.extra["build"] = "1234";
    report.user = {"42", "someone@example.com", "someone"};
    report.breadcrumbs.add({"ui", "clicked", Level::error, 1793060881000});
    report.attachments.add(eacp::FilePath {"/tmp/log.txt"});
    report.platform = "macOS";
    report.capturedBy = CaptureSource::exitCode;

    return report;
}
} // namespace

auto tCrashReportRoundTripsThroughJSON =
    test("CrashReportJSON/roundTripsThroughJSON") = []
{
    auto original = makeSampleReport();
    auto parsed = CrashReport::fromJSON(original.toJSON());

    check(parsed.has_value());
    check(parsed->id == original.id);
    check(parsed->timestamp == original.timestamp);
    check(parsed->reason == original.reason);
    check(parsed->message == original.message);
    check(parsed->faultAddress == addressAboveDoublePrecision);
    check(parsed->crashingThread.threadId == 42);
    check(parsed->crashingThread.frames.size() == 1);
    check(parsed->crashingThread.frames[0].offset == 0x647c);
    check(parsed->crashingThread.frames[0].symbol == "main + 4");
    check(parsed->metadata == original.metadata);
    check(parsed->tags == original.tags);
    check(parsed->extra == original.extra);
    check(parsed->user.email == "someone@example.com");
    check(parsed->breadcrumbs.size() == 1);
    check(parsed->breadcrumbs[0].level == Level::error);
    check(parsed->breadcrumbs[0].timestamp == 1793060881000);
    check(parsed->attachments.size() == 1);
    check(parsed->capturedBy == CaptureSource::exitCode);
};

auto tCrashReportReadsAReportWithoutModuleUuid =
    test("CrashReportJSON/readsAReportWithoutModuleUuid") = []
{
    auto parsed = CrashReport::fromJSON(reportWithoutModuleUuid);

    check(parsed.has_value());
    check(parsed->crashingThread.frames.size() == 1);
    check(parsed->crashingThread.frames[0].symbol == "main + 4");
    check(parsed->crashingThread.frames[0].moduleUuid.empty());

    auto reparsed = CrashReport::fromJSON(parsed->toJSON());
    check(reparsed.has_value());
    check(reparsed->crashingThread.frames[0].offset == 0x647c);
    check(reparsed->crashingThread.frames[0].moduleUuid.empty());
};

auto tCrashReportRefusesJunk = test("CrashReportJSON/refusesJunk") = []
{ check(!CrashReport::fromJSON("not json").has_value()); };

auto tCrashReportHexAddressesRoundTrip =
    test("CrashReportJSON/hexAddressesRoundTrip") = []
{
    auto text = formatHexAddress(addressAboveDoublePrecision);

    check(parseHexAddress(text) == addressAboveDoublePrecision);
    check(!parseHexAddress("0xzz").has_value());
};

auto tCrashReportHangRoundTrips = test("CrashReportJSON/hangRoundTrips") = []
{
    auto report = makeSampleReport();
    report.kind = ReportKind::hang;
    report.hangDurationMilliseconds = 10500;

    auto readBack = CrashReport::fromJSON(report.toJSON());
    check(readBack.has_value());
    check(readBack->kind == ReportKind::hang);
    check(readBack->hangDurationMilliseconds == 10500);
};

auto tCrashReportWithoutKindIsACrash =
    test("CrashReportJSON/withoutKindIsACrash") = []
{
    auto report = CrashReport::fromJSON(reportWithoutModuleUuid);
    check(report.has_value());
    check(report->kind == ReportKind::crash);
    check(report->hangDurationMilliseconds == 0);
};
