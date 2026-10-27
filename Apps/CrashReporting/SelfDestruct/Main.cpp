#include "Faults.h"
#include "ReportWindow.h"

#include <eacp/Core/App/Args.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/CrashReporting/CrashReporting.h>
#include <eacp/UI/UI.h>

// An app that crashes itself on purpose, one button per way to die, so the
// crash reporter can be watched end to end: the fault, the handler process
// writing pending/<id>/report.json, and that handler's crashReported opening
// a window with the report in it.
//
//   SelfDestruct                                   the window of buttons
//   SelfDestruct --crash nullptr                   faults at once, no window
//   SelfDestruct --crash abort -- --no-report-window
//
// Everything after "--" goes to the handler process, whose own flag
// --no-report-window has it write the report and exit, which is what
// Tests/CrashReporting runs.

using namespace eacp;

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
constexpr auto handlerStatusRefreshHz = 2;
constexpr auto windowWidth = 640;
constexpr auto windowHeight = 560;
constexpr auto dangerColour = UI::Color {0.86f, 0.27f, 0.25f};
constexpr auto runningColour = UI::Color {0.42f, 0.8f, 0.5f};
constexpr auto stoppedColour = UI::Color {1.f, 0.62f, 0.3f};
constexpr auto projectName = "eacp SelfDestruct";
constexpr auto noReportWindowFlag = "--no-report-window";
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

struct FaultCard final : UI::Component
{
    explicit FaultCard(const SelfDestruct::Fault& fault)
        : button(std::string {fault.label})
        , description(std::string {fault.description})
    {
        button.onClick = fault.trigger;
        button.setAccentColour(dangerColour);
        description.setThemeColour(UI::ThemeKeys::dimText);
        description.setJustification(UI::Justification::Centred);
        addChildren({button, description});
    }

    void paint(UI::Graphics& g) override
    {
        g.setColour(UI::defaultTheme().get(UI::ThemeKeys::panel));
        g.fillRoundedRect(getLocalBounds(), cardCornerRadius);
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(cardPadding);
        description.setBounds(area.removeFromBottom(descriptionHeight));
        button.setBounds(area);
    }

    UI::Button button;
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

        for (const auto& fault: SelfDestruct::getFaults())
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
                         const Vector<std::string>& handlerArguments)
{
    auto options = CrashReporting::Options {};
    options.handlerArguments = handlerArguments;

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

int crashNow(std::string_view kind)
{
    auto fault = SelfDestruct::findFault(kind);

    if (!fault)
    {
        Console::printError("SelfDestruct: no fault called " + std::string {kind});
        return Args::badArguments;
    }

    fault->get().trigger();
    return 0;
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

    auto cli = Args::Parser {"SelfDestruct", "Crashes itself, on purpose."};
    cli.option("--crash", crashKind, "fault at once with no window");
    cli.option("--crash-database", databaseDirectory, "where reports go");
    cli.positional(handlerArguments, "handler-argument", "after --, the handler's");

    if (auto code = cli.exitCodeFor(cli.parse()))
        return *code;

    startCrashReporting(databaseDirectory, handlerArguments);

    if (!crashKind.empty())
        return crashNow(crashKind);

    return Graphics::runWindowedApp<FaultHost>(
        makeWindowOptions(), CrashReporting::getDatabase().getDirectory());
}
