#include "ReportWindow.h"

#include <eacp/Core/App/Args.h>
#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/CrashReporting/CrashReporting.h>
#include <eacp/CrashReporting/Faults/Faults.h>
#include <eacp/CrashReporting/Faults/Matrix.h>
#include <eacp/UI/UI.h>

// An app that crashes itself on purpose, one button per way to die, so the
// crash reporter can be watched end to end: the fault, the handler process
// writing pending/<id>/report.json, and that handler's crashReported opening
// a window with the report in it.
//
//   SelfDestruct                                   the window of buttons
//   SelfDestruct --crash nullptr                   faults at once, no window
//   SelfDestruct --crash abort --thread background
//   SelfDestruct --crash abort -- --no-report-window
//   SelfDestruct --crash hang --hang-threshold-ms 500
//   SelfDestruct --crash nullptr --kill-handler    an unreported exit
//
// Everything after "--" goes to the handler process, whose own flag
// --no-report-window has it write the report and exit.

using namespace eacp;
using CrashReporting::Faults::ThreadChoice;

namespace
{
constexpr auto padding = 20.f;
constexpr auto titleHeight = 40.f;
constexpr auto titleFontSize = 26.f;
constexpr auto lineHeight = 22.f;
constexpr auto bodyFontSize = 13.f;
constexpr auto gridColumns = 2;
constexpr auto cardGap = 12.f;
constexpr auto cardPadding = 12.f;
constexpr auto cardCornerRadius = 10.f;
constexpr auto descriptionHeight = 22.f;
constexpr auto buttonGap = 8.f;
constexpr auto buttonsPerCard = 2.f;
constexpr auto handlerStatusRefreshHz = 2;
constexpr auto windowWidth = 640;
constexpr auto windowHeight = 900;
constexpr auto expectationHeight = 34.f;
constexpr auto dangerColour = UI::Color {0.86f, 0.27f, 0.25f};
constexpr auto runningColour = UI::Color {0.42f, 0.8f, 0.5f};
constexpr auto stoppedColour = UI::Color {1.f, 0.62f, 0.3f};
constexpr auto projectName = "eacp SelfDestruct";
constexpr auto noReportWindowFlag = "--no-report-window";
constexpr auto showReportFlag = "--show-report";
constexpr auto hangBeyondThreshold = std::chrono::milliseconds {2000};
constexpr auto demoEnvironment = "demo";
constexpr auto demoUserId = "self-destruct-user";
constexpr auto demoUserEmail = "user@example.com";
constexpr auto demoUserName = "selfdestruct";

struct SelfDestructCrashHandler final : CrashReporting::CrashHandler
{
    void crashReported(const CrashReporting::CrashReport& report) override
    {
        auto noReportWindow = false;
        auto cli = Args::Parser {"SelfDestruct crash handler"};
        cli.flag(noReportWindowFlag, noReportWindow, "write the report and exit");

        if (cli.exitCodeFor(cli.parse()) || noReportWindow)
            return;

        SelfDestruct::runReportWindow(report);
    }

    // The app is still running and the handler goes back to watching it, so
    // the window is a process of its own.
    void hangReported(const CrashReporting::CrashReport& report) override
    {
        auto noReportWindow = false;
        auto cli = Args::Parser {"SelfDestruct crash handler"};
        cli.flag(noReportWindowFlag, noReportWindow, "write the report and exit");

        if (cli.exitCodeFor(cli.parse()) || noReportWindow)
            return;

        auto options = Processes::ProcessOptions {};
        options.executable = Files::executablePath().str();
        options.arguments = {showReportFlag, report.folder.str()};
        options.detached = true;
        auto window = Processes::Process {options};
    }

    void pendingReportFound(const CrashReporting::CrashReport& report) override
    {
        Console::print("SelfDestruct: pending report " + report.id + " ("
                       + report.reason + ")");
    }
};

EACP_REGISTER_CRASH_HANDLER(SelfDestructCrashHandler);

std::string describeHandlerStatus()
{
    if (CrashReporting::isHandlerRunning())
        return "Handler process running";

    return "Handler process not running: a crash now goes unreported";
}

UI::Color getHandlerStatusColour()
{
    if (CrashReporting::isHandlerRunning())
        return runningColour;

    return stoppedColour;
}

// What the crash reporter makes of a fault here, from the expectations the
// tests check, so the app and the tests cannot disagree.
std::string describeExpectedOutcome(std::string_view fault, ThreadChoice thread)
{
    using CrashReporting::Faults::OutcomeStatus;
    auto outcome = CrashReporting::Faults::findExpectedOutcome(
        fault, thread, CrashReporting::Faults::getCurrentLane());

    if (!outcome)
        return "No expectation recorded for this platform";

    auto note = std::string {outcome->note};

    switch (outcome->status)
    {
        case OutcomeStatus::reported:
            return "Reported";
        case OutcomeStatus::reportedWithLimits:
            return "Reported, " + note;
        case OutcomeStatus::notReported:
            return "Not reported: " + note;
        case OutcomeStatus::notAvailable:
            return "Cannot be raised here: " + note;
        case OutcomeStatus::pendingPR:
            return "Not reported yet: " + note;
    }

    return {};
}

struct FaultCard final : UI::Component
{
    explicit FaultCard(const CrashReporting::Faults::Fault& fault)
        : label(std::string {fault.label})
        , mainThreadExpectation(
              describeExpectedOutcome(fault.name, ThreadChoice::main))
        , backgroundExpectation(
              describeExpectedOutcome(fault.name, ThreadChoice::background))
        , description(std::string {fault.description})
    {
        auto name = fault.name;
        mainThreadButton.onClick = [name]
        { CrashReporting::Faults::triggerFault(name, ThreadChoice::main); };
        backgroundButton.onClick = [name]
        { CrashReporting::Faults::triggerFault(name, ThreadChoice::background); };

        mainThreadButton.setAccentColour(dangerColour);
        backgroundButton.setAccentColour(dangerColour);
        description.setThemeColour(UI::ThemeKeys::dimText);
        label.setJustification(UI::Justification::Centred);
        description.setJustification(UI::Justification::Centred);

        for (auto* expectation: {&mainThreadExpectation, &backgroundExpectation})
        {
            expectation->setThemeColour(UI::ThemeKeys::dimText);
            expectation->setJustification(UI::Justification::Centred);
        }

        addChildren({label,
                     mainThreadButton,
                     backgroundButton,
                     mainThreadExpectation,
                     backgroundExpectation,
                     description});
    }

    void paint(UI::Graphics& g) override
    {
        g.setColour(UI::defaultTheme().get(UI::ThemeKeys::panel));
        g.fillRoundedRect(getLocalBounds(), cardCornerRadius);
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(cardPadding);
        label.setBounds(area.removeFromTop(lineHeight));
        description.setBounds(area.removeFromBottom(descriptionHeight));
        auto expectations = area.removeFromBottom(expectationHeight);
        auto buttonWidth = (area.w - buttonGap) / buttonsPerCard;
        mainThreadButton.setBounds(area.removeFromLeft(buttonWidth));
        mainThreadExpectation.setBounds(expectations.removeFromLeft(buttonWidth));
        area.removeFromLeft(buttonGap);
        expectations.removeFromLeft(buttonGap);
        backgroundButton.setBounds(area);
        backgroundExpectation.setBounds(expectations);
    }

    UI::Label label;
    UI::Button mainThreadButton {"Main thread"};
    UI::Button backgroundButton {"Background"};
    UI::Label mainThreadExpectation;
    UI::Label backgroundExpectation;
    UI::Label description;
};

struct FaultButtons final : UI::Component
{
    explicit FaultButtons(FilePath databaseDirectoryToShow)
        : database("Reports go to " + databaseDirectoryToShow.str())
    {
        title.setFontSize(titleFontSize);
        subtitle.setThemeColour(UI::ThemeKeys::dimText);
        database.setThemeColour(UI::ThemeKeys::dimText);
        addChildren({title, subtitle, database, handlerStatus});

        for (const auto& fault: CrashReporting::Faults::listFaults())
            addAndMakeVisible(cards.createNew(fault));

        showHandlerStatus();
    }

    void showHandlerStatus()
    {
        handlerStatus.setText(describeHandlerStatus());
        handlerStatus.setColour(getHandlerStatusColour());
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().get(UI::ThemeKeys::background));
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(padding);
        title.setBounds(area.removeFromTop(titleHeight));
        subtitle.setBounds(area.removeFromTop(lineHeight));
        handlerStatus.setBounds(area.removeFromBottom(lineHeight));
        database.setBounds(area.removeFromBottom(lineHeight));
        area.removeFromTop(padding);
        area.removeFromBottom(padding);
        layOutCards(area);
    }

    void layOutCards(UI::Rect area)
    {
        auto rowCount = (cards.size() + gridColumns - 1) / gridColumns;
        auto cardWidth = (area.w - cardGap * (gridColumns - 1)) / gridColumns;
        auto cardHeight = (area.h - cardGap * (rowCount - 1)) / rowCount;

        for (auto index = 0; index < cards.size(); ++index)
        {
            auto column = index % gridColumns;
            auto row = index / gridColumns;
            cards[index]->setBounds({area.x + column * (cardWidth + cardGap),
                                     area.y + row * (cardHeight + cardGap),
                                     cardWidth,
                                     cardHeight});
        }
    }

    UI::Label title {"SelfDestruct"};
    UI::Label subtitle {"Each button really crashes this app. Its handler "
                        "process writes the report and shows it."};
    UI::Label database;
    UI::Label handlerStatus;
    OwnedVector<FaultCard> cards;
    Threads::Timer statusTimer {[this] { showHandlerStatus(); },
                                handlerStatusRefreshHz};
};

struct FaultHost final : UI::ComponentHost
{
    explicit FaultHost(const FilePath& databaseDirectory)
        : root(databaseDirectory)
    {
        setFontPointSize(bodyFontSize);
        setRootComponent(root);
    }

    FaultButtons root;
};

Graphics::WindowOptions makeWindowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = windowWidth;
    options.height = windowHeight;
    options.title = "SelfDestruct";

    return options;
}

FilePath writeSessionLog()
{
    auto log = FilePath::appCacheDirectory() / "SelfDestruct.log";
    Files::removeFile(log);
    Files::appendFile(log, std::string_view {"SelfDestruct started\n"});

    return log;
}

void startCrashReporting(const FilePath& databaseDirectory,
                         const Vector<std::string>& handlerArguments,
                         std::chrono::milliseconds hangThreshold)
{
    auto options = CrashReporting::Options {};
    options.handlerArguments = handlerArguments;
    options.hangThreshold = hangThreshold;
    options.stackOverflowReporting =
        CrashReporting::StackOverflowReporting::everyThread;

    if (!databaseDirectory.empty())
        options.databaseDirectory = databaseDirectory;

    options.environment = demoEnvironment;
    options.attachments = {writeSessionLog()};

    CrashReporting::start(options);
    CrashReporting::setTag("project", projectName);
    CrashReporting::setExtra("launchedFrom", Files::executablePath().str());
    CrashReporting::setUser({demoUserId, demoUserEmail, demoUserName});
    CrashReporting::addBreadcrumb({"app", "crash reporting started"});
    CrashReporting::addBreadcrumb(
        {"app", "about to offer ways to crash", CrashReporting::Level::warning});
}

// Nothing is watched for a hang until the loop has beaten a few times.
Time::MS getDelayBeforeStalling()
{
    constexpr auto heartbeatsBeforeStalling = 3;
    auto delay =
        CrashReporting::Options {}.heartbeatInterval * heartbeatsBeforeStalling;
    return Time::MS {delay.count()};
}

// A main-thread stall quits once the fault returns; a background one leaves
// the main thread free, so the app runs as long as the stall would, then quits.
void runStallingFault(std::string_view name,
                      ThreadChoice thread,
                      bool pausesHangDetection)
{
    if (pausesHangDetection)
        CrashReporting::pauseHangDetection();

    CrashReporting::Faults::triggerFault(name, thread);

    if (pausesHangDetection)
        CrashReporting::resumeHangDetection();

    if (thread == ThreadChoice::main)
    {
        // Quitting from inside the callback that stalled sometimes leaves
        // [NSApp run] waiting; from a fresh one it always ends.
        Threads::callAsync([] { Apps::quit(); });
        return;
    }

    auto stall = CrashReporting::Faults::getHangDuration();
    Threads::callAfter(Time::MS {stall.count()}, [] { Apps::quit(); });
}

struct FaultApp
{
    FaultApp(const CrashReporting::Faults::Fault& fault,
             ThreadChoice thread,
             bool pausesHangDetection)
    {
        if (!fault.stallsItsThread)
        {
            CrashReporting::Faults::scheduleFault(fault.name, thread, Time::MS {0});
            return;
        }

        auto name = fault.name;
        Threads::callAfter(getDelayBeforeStalling(),
                           [name, thread, pausesHangDetection]
                           { runStallingFault(name, thread, pausesHangDetection); });
    }
};

int crashNow(std::string_view kind,
             std::string_view threadName,
             bool pausesHangDetection,
             bool killsHandler)
{
    auto fault = CrashReporting::Faults::findFault(kind);
    auto thread = CrashReporting::Faults::findThreadChoice(threadName);

    if (!fault || !fault->get().isAvailable || !thread)
    {
        Console::printError("SelfDestruct: no fault called " + std::string {kind}
                            + " on a " + std::string {threadName} + " thread here");
        return Args::badArguments;
    }

    if (killsHandler)
        CrashReporting::Faults::killHandlerProcess();

    if (*thread == ThreadChoice::main && !fault->get().stallsItsThread)
    {
        CrashReporting::Faults::triggerFault(kind, ThreadChoice::main);
        return 0;
    }

    return Apps::run<FaultApp>(fault->get(), *thread, pausesHangDetection);
}

int showReport(const FilePath& folder)
{
    auto report = CrashReporting::getDatabase().readReport(folder);

    if (!report)
    {
        Console::printError("SelfDestruct: no report in " + folder.str());
        return Args::badArguments;
    }

    return SelfDestruct::runReportWindow(*report);
}
} // namespace

int main(int argc, char* argv[])
{
    if (auto code = CrashReporting::runHandlerIfRequested(argc, argv))
        return *code;

    Apps::setCommandLineArgs(argc, argv);

    auto crashKind = std::string {};
    auto databaseDirectory = FilePath {};
    auto handlerArguments = Vector<std::string> {};
    auto hangThresholdMilliseconds =
        CrashReporting::Options {}.hangThreshold.count();
    auto reportToShow = FilePath {};
    auto pausesHangDetection = false;
    auto threadName = std::string {"main"};
    auto killsHandler = false;

    auto cli = Args::Parser {"SelfDestruct", "Crashes itself, on purpose."};
    cli.option("--crash", crashKind, "fault at once with no window");
    cli.option("--thread", threadName, "main or background");
    cli.flag("--kill-handler",
             killsHandler,
             "end the handler process first, so the crash goes unreported");
    cli.option("--crash-database", databaseDirectory, "where reports go");
    cli.option("--hang-threshold-ms",
               hangThresholdMilliseconds,
               "how long the main thread may stall; 0 turns hang detection off");
    cli.flag("--pause-hang-detection",
             pausesHangDetection,
             "pause hang detection around a hang or spin fault");
    cli.option(showReportFlag, reportToShow, "show the report in this folder");
    cli.positional(handlerArguments, "handler-argument", "after --, the handler's");

    if (auto code = cli.exitCodeFor(cli.parse()))
        return *code;

    if (!reportToShow.empty())
        return showReport(reportToShow);

    auto hangThreshold = std::chrono::milliseconds {hangThresholdMilliseconds};
    CrashReporting::Faults::setHangDuration(hangThreshold + hangBeyondThreshold);
    startCrashReporting(databaseDirectory, handlerArguments, hangThreshold);

    if (!crashKind.empty())
        return crashNow(crashKind, threadName, pausesHangDetection, killsHandler);

    return Graphics::runWindowedApp<FaultHost>(
        makeWindowOptions(), CrashReporting::getDatabase().getDirectory());
}
