#include "App.h"
#include "ReportWindow.h"

#include <eacp/Core/App/Args.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/CrashReporting/CrashReporting.h>
#include <eacp/CrashReporting/Faults/Faults.h>
#include <eacp/CrashReporting/Faults/Matrix.h>
#include <eacp/UI/UI.h>

#include <atomic>

// An app that crashes itself on purpose, one button per way to die, so the
// crash reporter can be watched end to end: the fault, the handler process
// writing pending/<id>/report.json, and that handler's reportWritten opening
// a window with the report in it.
//
//   SelfDestruct                                   the window of buttons
//   SelfDestruct --crash nullptr                   faults at once, no window
//   SelfDestruct --crash abort --thread background
//   SelfDestruct --crash abort -- --no-report-window
//   SelfDestruct --crash hang --hang-threshold-ms 500
//   SelfDestruct --crash deadlock --thread background   a watched worker's hang
//   SelfDestruct --crash nullptr --kill-handler    an unreported exit
//   SelfDestruct --event message                   a handled event, no crash
//   SelfDestruct --event exception
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
constexpr auto gridColumns = 3;
constexpr auto cardGap = 12.f;
constexpr auto cardPadding = 12.f;
constexpr auto cardCornerRadius = 10.f;
constexpr auto descriptionHeight = 22.f;
constexpr auto buttonGap = 8.f;
constexpr auto buttonsPerCard = 2.f;
constexpr auto handlerStatusRefreshHz = 2;
constexpr auto windowWidth = 900;
constexpr auto windowHeight = 960;
constexpr auto expectationHeight = 34.f;
constexpr auto dangerColour = UI::Color {0.86f, 0.27f, 0.25f};
constexpr auto runningColour = UI::Color {0.42f, 0.8f, 0.5f};
constexpr auto stoppedColour = UI::Color {1.f, 0.62f, 0.3f};
constexpr auto hangBeyondThreshold = Time::MS {2000};
constexpr auto encoderChunkDuration = Time::MS {10};
constexpr auto encoderStallDuration = Time::MS {10000};

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

// A titled card with two buttons side by side, each with what it will do.
struct CardContent
{
    std::string label;
    std::string description;
    std::string firstButtonText;
    std::string secondButtonText;
    std::string firstExpectation;
    std::string secondExpectation;
    Callback firstAction = [] {};
    Callback secondAction = [] {};
    UI::Color accentColour = dangerColour;
};

CardContent makeFaultCardContent(const CrashReporting::Faults::Fault& fault)
{
    auto name = fault.name;
    return {
        std::string {fault.label},
        std::string {fault.description},
        "Main thread",
        "Background",
        describeExpectedOutcome(name, ThreadChoice::main),
        describeExpectedOutcome(name, ThreadChoice::background),
        [name] { CrashReporting::Faults::triggerFault(name, ThreadChoice::main); },
        [name]
        { CrashReporting::Faults::triggerFault(name, ThreadChoice::background); }};
}

// Not a crash, so outside the crash matrix.
CardContent makeEventCardContent()
{
    return {"Handled event",
            "Reported without crashing; the app keeps running",
            "Message",
            "Caught exception",
            "Reported as an event",
            "Reported as an event",
            [] { CrashReporting::Faults::reportMessageEvent(); },
            [] { CrashReporting::Faults::reportCaughtException(); },
            runningColour};
}

// A worker that sends heartbeats is watched for hangs; one that never does is not.
struct BackgroundEncoder
{
    ~BackgroundEncoder() { isRunning = false; }

    void start()
    {
        if (isRunning.exchange(true))
            return;

        worker = Threads::Thread {[this] { runEncodeLoop(); }};
    }

    void stall() { stallRequested = true; }

    void runEncodeLoop()
    {
        while (isRunning)
        {
            encodeNextChunk();
            CrashReporting::sendHeartbeat();
        }
    }

    void encodeNextChunk()
    {
        if (stallRequested.exchange(false))
            Time::sleep(encoderStallDuration);

        Time::sleep(encoderChunkDuration);
    }

    std::atomic<bool> isRunning = false;
    std::atomic<bool> stallRequested = false;
    Threads::Thread worker;
};

CardContent makeEncoderCardContent(BackgroundEncoder& encoder)
{
    return {"Background encoder",
            "A worker loop that sends a heartbeat per chunk",
            "Start encoder",
            "Stall encoder",
            "Encodes until the app quits",
            "Reported as a hang on the encoder thread",
            [&encoder] { encoder.start(); },
            [&encoder] { encoder.stall(); }};
}

struct ActionCard final : UI::Component
{
    explicit ActionCard(const CardContent& content)
        : label(content.label)
        , mainThreadButton(content.firstButtonText)
        , backgroundButton(content.secondButtonText)
        , mainThreadExpectation(content.firstExpectation)
        , backgroundExpectation(content.secondExpectation)
        , description(content.description)
    {
        mainThreadButton.onClick = content.firstAction;
        backgroundButton.onClick = content.secondAction;

        mainThreadButton.setAccentColour(content.accentColour);
        backgroundButton.setAccentColour(content.accentColour);
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
    UI::Button mainThreadButton;
    UI::Button backgroundButton;
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
            addAndMakeVisible(cards.createNew(makeFaultCardContent(fault)));

        addAndMakeVisible(cards.createNew(makeEventCardContent()));
        addAndMakeVisible(cards.createNew(makeEncoderCardContent(encoder)));

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
    BackgroundEncoder encoder;
    OwnedVector<ActionCard> cards;
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

// Nothing is watched for a hang until the loop has sent a few heartbeats.
Time::MS getDelayBeforeStalling()
{
    constexpr auto heartbeatsBeforeStalling = 3;
    return CrashReporting::Options {}.heartbeatInterval * heartbeatsBeforeStalling;
}

// A main-thread stall quits once the fault returns; a background one leaves
// the main thread free, so the app runs as long as the stall would, then quits.
void runStallingFault(std::string_view name,
                      ThreadChoice thread,
                      bool pausesHangDetection)
{
    CrashReporting::Faults::setPausesBackgroundWatch(pausesHangDetection);

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
    Threads::callAfter(stall, [] { Apps::quit(); });
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

int printEventOutcome(const std::string& id)
{
    Console::print("SelfDestruct: still running after " + id);
    return id.empty() ? 1 : 0;
}

int reportEventNow(std::string_view kind)
{
    if (kind == "message")
        return printEventOutcome(CrashReporting::Faults::reportMessageEvent());

    if (kind == "exception")
        return printEventOutcome(CrashReporting::Faults::reportCaughtException());

    Console::printError("SelfDestruct: no event called " + std::string {kind});
    return Args::badArguments;
}
} // namespace

namespace SelfDestruct
{
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

std::optional<int> parseLaunchFlags(LaunchFlags& flags)
{
    auto hangThresholdMilliseconds = CrashReporting::Options {}.hangThreshold.count;

    auto cli = Args::Parser {"SelfDestruct", "Crashes itself, on purpose."};
    cli.option("--crash", flags.crashKind, "fault at once with no window");
    cli.option("--thread", flags.threadName, "main or background");
    cli.flag("--kill-handler",
             flags.killsHandler,
             "end the handler process first, so the crash goes unreported");
    cli.option("--crash-database", flags.databaseDirectory, "where reports go");
    cli.option("--hang-threshold-ms",
               hangThresholdMilliseconds,
               "how long the main thread may stall; 0 turns hang detection off");
    cli.flag("--pause-hang-detection",
             flags.pausesHangDetection,
             "pause the stalling thread's hang watch around its fault");
    cli.flag("--signals-only",
             flags.usesSignalsOnly,
             "macOS: no Mach exception port, the in-app signal handler only");
    cli.option(showReportFlag, flags.reportToShow, "show the report in this folder");
    cli.option(
        "--event", flags.eventKind, "message or exception: report it, no crash");
    cli.flag("--without-windows-error-reporting-helper",
             flags.withoutWindowsErrorReportingHelper,
             "Windows: skip the Windows Error Reporting helper, so fast-fail "
             "and heap-corruption crashes get no stack");
    cli.option("--windows-error-reporting-helper",
               flags.windowsErrorReportingHelper,
               "Windows: the Windows Error Reporting helper DLL to register, "
               "instead of the one beside the executable");
    cli.positional(
        flags.handlerArguments, "handler-argument", "after --, the handler's");

    if (auto code = cli.exitCodeFor(cli.parse()))
        return code;

    flags.hangThreshold = Time::MS {hangThresholdMilliseconds};
    CrashReporting::Faults::setHangDuration(flags.hangThreshold
                                            + hangBeyondThreshold);
    return {};
}

int runApp(const LaunchFlags& flags)
{
    if (!flags.eventKind.empty())
        return reportEventNow(flags.eventKind);

    if (!flags.crashKind.empty())
    {
        return crashNow(flags.crashKind,
                        flags.threadName,
                        flags.pausesHangDetection,
                        flags.killsHandler);
    }

    return Graphics::runWindowedApp<FaultHost>(
        makeWindowOptions(), CrashReporting::getDatabase().getDirectory());
}
} // namespace SelfDestruct
