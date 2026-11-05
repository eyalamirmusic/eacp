#include "Common.h"

#include <eacp/Core/App/Alert.h>

// Apps::showAlert without a dialog: the options as a presenter is handed them,
// the answer it gives, and the headless answer. The native dialogs are checked
// by hand with Apps/Graphics/AlertDemo.
//
// Every case installs a presenter or turns headless on, so no case can put a
// real dialog in front of a test run on a desktop.

using namespace nano;
using namespace eacp;

namespace AlertTests
{
using Apps::AlertIcon;
using Apps::AlertOptions;

struct PresenterGuard
{
    explicit PresenterGuard(Apps::AlertPresenter presenter)
    {
        Apps::setAlertPresenter(std::move(presenter));
    }

    ~PresenterGuard() { Apps::setAlertPresenter(nullptr); }
};

struct HeadlessGuard
{
    HeadlessGuard()
        : previous(Apps::getAppEnvironment().headless)
    {
        Apps::getAppEnvironment().headless = true;
    }

    ~HeadlessGuard() { Apps::getAppEnvironment().headless = previous; }

    bool previous;
};

struct Shown
{
    Vector<AlertOptions> alerts;
    Vector<Apps::AlertCallback> answers;
};

Apps::AlertPresenter recordInto(Shown& shown)
{
    return [&shown](const AlertOptions& options, Apps::AlertCallback onDismissed)
    {
        shown.alerts.add(options);
        shown.answers.add(std::move(onDismissed));
    };
}

template <typename Predicate>
bool pumpUntil(Predicate ready)
{
    return Threads::runEventLoopUntil(ready, Time::MS {2000});
}

void pumpBriefly()
{
    Threads::runEventLoopFor(Time::MS {50});
}

AlertOptions saveChanges()
{
    auto options = AlertOptions {};
    options.title = "Unsaved changes";
    options.message = "Save the document before closing?";
    options.icon = AlertIcon::warning;
    options.buttons = {"Save", "Don't Save", "Cancel"};
    options.defaultButton = 0;
    options.cancelButton = 2;
    return options;
}

auto tPresenterGetsTheOptions = test("Alert/presenterIsHandedTheOptionsAsGiven") = []
{
    auto shown = Shown {};
    auto guard = PresenterGuard {recordInto(shown)};

    Apps::showAlert(saveChanges());

    check(pumpUntil([&] { return shown.alerts.size() == 1; }));

    if (shown.alerts.size() != 1)
        return;

    auto& alert = shown.alerts[0];
    check(alert.title == "Unsaved changes");
    check(alert.message == "Save the document before closing?");
    check(alert.icon == AlertIcon::warning);
    check(alert.buttons.size() == 3);
    check(alert.buttons[0] == "Save");
    check(alert.buttons[1] == "Don't Save");
    check(alert.buttons[2] == "Cancel");
    check(alert.defaultButton == 0);
    check(alert.cancelButton == 2);
};

auto tEachIconArrives = test("Alert/eachIconReachesThePresenter") = []
{
    auto shown = Shown {};
    auto guard = PresenterGuard {recordInto(shown)};

    for (auto icon: {AlertIcon::info, AlertIcon::warning, AlertIcon::error})
    {
        auto options = AlertOptions {};
        options.icon = icon;
        Apps::showAlert(options);
    }

    check(pumpUntil([&] { return shown.alerts.size() == 3; }));

    if (shown.alerts.size() != 3)
        return;

    check(shown.alerts[0].icon == AlertIcon::info);
    check(shown.alerts[1].icon == AlertIcon::warning);
    check(shown.alerts[2].icon == AlertIcon::error);
};

auto tDefaultsAreInfoAndOk = test("Alert/defaultOptionsAreOneOkInfoAlert") = []
{
    auto options = Apps::Detail::resolveAlertOptions({});

    check(options.icon == AlertIcon::info);
    check(options.buttons.size() == 1);
    check(options.buttons.size() == 1 && options.buttons[0] == "OK");
    check(options.defaultButton == 0);
    check(!options.cancelButton.has_value());
};

auto tNoButtonsIsOk = test("Alert/noButtonsReachesThePresenterAsOneOk") = []
{
    auto shown = Shown {};
    auto guard = PresenterGuard {recordInto(shown)};

    auto options = AlertOptions {};
    options.title = "Saved";
    Apps::showAlert(options);

    check(pumpUntil([&] { return shown.alerts.size() == 1; }));
    check(shown.alerts.size() == 1 && shown.alerts[0].buttons.size() == 1
          && shown.alerts[0].buttons[0] == "OK");
};

auto tOutOfRangeDefault = test("Alert/outOfRangeDefaultIsTheFirstButton") = []
{
    for (auto index: {-1, 2, 99})
    {
        auto options = AlertOptions {};
        options.buttons = {"Yes", "No"};
        options.defaultButton = index;

        check(Apps::Detail::resolveAlertOptions(options).defaultButton == 0);
    }

    auto options = AlertOptions {};
    options.buttons = {"Yes", "No"};
    options.defaultButton = 1;
    check(Apps::Detail::resolveAlertOptions(options).defaultButton == 1);
};

auto tOutOfRangeCancel = test("Alert/outOfRangeCancelIsNoCancelButton") = []
{
    for (auto index: {-1, 2, 99})
    {
        auto options = AlertOptions {};
        options.buttons = {"Yes", "No"};
        options.cancelButton = index;

        check(!Apps::Detail::resolveAlertOptions(options).cancelButton);
    }

    auto options = AlertOptions {};
    options.buttons = {"Yes", "No"};
    options.cancelButton = 1;
    check(Apps::Detail::resolveAlertOptions(options).cancelButton == 1);

    auto onlyOk = AlertOptions {};
    onlyOk.cancelButton = 0;
    check(Apps::Detail::resolveAlertOptions(onlyOk).cancelButton == 0);
};

auto tCallbackGetsTheAnswer =
    test("Alert/callbackGetsTheButtonThePresenterAnswers") = []
{
    auto shown = Shown {};
    auto guard = PresenterGuard {recordInto(shown)};
    auto answers = Vector<int> {};

    Apps::showAlert(saveChanges(), [&](int button) { answers.add(button); });

    check(pumpUntil([&] { return shown.answers.size() == 1; }));

    if (shown.answers.size() != 1)
        return;

    shown.answers[0](1);

    check(answers.size() == 1 && answers[0] == 1);
};

auto tAnswerOnlyOnce = test("Alert/callbackFiresOnceHoweverOftenItIsAnswered") = []
{
    auto shown = Shown {};
    auto guard = PresenterGuard {recordInto(shown)};
    auto answers = Vector<int> {};

    Apps::showAlert(saveChanges(), [&](int button) { answers.add(button); });

    check(pumpUntil([&] { return shown.answers.size() == 1; }));

    if (shown.answers.size() != 1)
        return;

    shown.answers[0](2);
    shown.answers[0](0);
    shown.answers[0](1);

    check(answers.size() == 1 && answers[0] == 2);
};

auto tNeverBeforeReturn = test("Alert/showAlertReturnsBeforeAnythingIsShown") = []
{
    auto presented = 0;
    auto answers = 0;
    auto guard = PresenterGuard {[&](const AlertOptions&, Apps::AlertCallback done)
                                 {
                                     ++presented;
                                     done(0);
                                 }};

    Apps::showAlert({}, [&](int) { ++answers; });

    check(presented == 0);
    check(answers == 0);

    check(pumpUntil([&] { return answers == 1; }));
    check(presented == 1);
};

auto tPresenterOnMainThread =
    test("Alert/aWorkerThreadsAlertIsShownOnTheMainThread") = []
{
    auto presentedOnMain = std::atomic<int> {-1};
    auto answeredOnMain = std::atomic<int> {-1};

    auto guard = PresenterGuard {[&](const AlertOptions&, Apps::AlertCallback done)
                                 {
                                     presentedOnMain =
                                         Threads::isMainThread() ? 1 : 0;
                                     done(0);
                                 }};

    auto worker = std::thread(
        [&]
        {
            Apps::showAlert(
                {}, [&](int) { answeredOnMain = Threads::isMainThread() ? 1 : 0; });
        });

    worker.join();

    check(pumpUntil([&] { return answeredOnMain != -1; }));
    check(presentedOnMain == 1);
    check(answeredOnMain == 1);
};

auto tHeadlessAnswersDefault = test("Alert/headlessAnswersTheDefaultButton") = []
{
    auto guard = HeadlessGuard {};
    auto answers = Vector<int> {};

    auto options = saveChanges();
    options.defaultButton = 1;
    Apps::showAlert(options, [&](int button) { answers.add(button); });

    check(answers.empty());
    check(pumpUntil([&] { return !answers.empty(); }));

    pumpBriefly();

    check(answers.size() == 1 && answers[0] == 1);
};

auto tHeadlessOutOfRange =
    test("Alert/headlessAnswersTheFirstForAnOutOfRangeDefault") = []
{
    auto guard = HeadlessGuard {};
    auto answer = -1;

    auto options = saveChanges();
    options.defaultButton = 7;
    Apps::showAlert(options, [&](int button) { answer = button; });

    check(pumpUntil([&] { return answer != -1; }));
    check(answer == 0);
};

auto tHeadlessOnMainThread = test("Alert/headlessAnswerIsOnTheMainThread") = []
{
    auto guard = HeadlessGuard {};
    auto answeredOnMain = std::atomic<int> {-1};

    auto worker = std::thread(
        [&]
        {
            Apps::showAlert(saveChanges(),
                            [&](int)
                            { answeredOnMain = Threads::isMainThread() ? 1 : 0; });
        });

    worker.join();

    check(pumpUntil([&] { return answeredOnMain != -1; }));
    check(answeredOnMain == 1);
};

auto tHeadlessInOrder = test("Alert/headlessAlertsAreAnsweredInTheOrderShown") = []
{
    auto guard = HeadlessGuard {};
    auto order = Vector<std::string> {};

    for (auto name: {"first", "second", "third"})
    {
        auto options = AlertOptions {};
        options.title = name;
        Apps::showAlert(options, [&order, name](int) { order.add(name); });
    }

    check(pumpUntil([&] { return order.size() == 3; }));
    check(order.size() == 3 && order[0] == "first" && order[1] == "second"
          && order[2] == "third");
};

auto tNoCallback = test("Alert/anAlertNeedsNoCallback") = []
{
    {
        auto guard = HeadlessGuard {};
        Apps::showAlert(saveChanges());
        pumpBriefly();
    }

    auto shown = Shown {};
    auto guard = PresenterGuard {recordInto(shown)};

    Apps::showAlert(saveChanges());

    check(pumpUntil([&] { return shown.answers.size() == 1; }));

    if (shown.answers.size() == 1)
        shown.answers[0](1);
};

auto tPresenterBeatsHeadless = test("Alert/aPresenterIsAskedEvenWhenHeadless") = []
{
    auto headless = HeadlessGuard {};
    auto shown = Shown {};
    auto guard = PresenterGuard {recordInto(shown)};
    auto answer = -1;

    Apps::showAlert(saveChanges(), [&](int button) { answer = button; });

    check(pumpUntil([&] { return shown.answers.size() == 1; }));
    pumpBriefly();
    check(answer == -1);

    if (shown.answers.size() == 1)
        shown.answers[0](2);

    check(answer == 2);
};

auto tClearedPresenter = test("Alert/clearingThePresenterRestoresThePlatform") = []
{
    auto shown = Shown {};
    Apps::setAlertPresenter(recordInto(shown));
    Apps::setAlertPresenter(nullptr);

    auto headless = HeadlessGuard {};
    auto answer = -1;

    Apps::showAlert(saveChanges(), [&](int button) { answer = button; });

    check(pumpUntil([&] { return answer != -1; }));
    check(answer == 0);
    check(shown.alerts.empty());
};
} // namespace AlertTests
