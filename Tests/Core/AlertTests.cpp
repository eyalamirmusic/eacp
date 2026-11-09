#include "Common.h"

#include <eacp/Core/App/Alert.h>

#include <memory>
#include <stdexcept>

// Apps::showAlert without a dialog: the options as a presenter is handed them,
// the answer it gives, the queue that shows one alert at a time, the
// unanswered rule, and the platforms' id-to-button mappings. The native dialogs
// are checked by hand with Apps/Graphics/AlertDemo.
//
// Every case runs inside an AlertIsolation: headless for its whole length, and
// on the way out it clears the presenter and pumps until every alert it raised
// has been answered, still headless. A case that fails half way can therefore
// never leave an alert queued for a later, non-headless turn to put in front
// of a desktop test run. Each case declares what its callbacks capture before
// the isolation, so the drain runs while those are still alive.

using namespace nano;
using namespace eacp;

namespace AlertTests
{
using Apps::AlertIcon;
using Apps::AlertOptions;
using Apps::Detail::MessageBoxButtons;
namespace Ids = Apps::Detail::WindowsIds;

// One continuous pump, so isEventLoopRunning() holds from `body` until `ready`:
// a worker's showAlert must never land between two pumps, where it would be
// answered at once as an alert with no loop.
template <typename Ready>
bool runLoopUntil(const Callback& body, Ready ready)
{
    auto poll = Threads::Timer(
        [&]
        {
            if (ready())
                Threads::stopEventLoop();
        },
        Time::MS {5});

    Threads::runEventLoopFor(Time::MS {2000}, body);
    return ready();
}

template <typename Ready>
bool pumpUntil(Ready ready)
{
    return runLoopUntil([] {}, ready);
}

void pumpBriefly()
{
    Threads::runEventLoopFor(Time::MS {50});
}

struct AlertIsolation
{
    explicit AlertIsolation(Apps::AlertPresenter presenter = {})
        : previous(Apps::getAppEnvironment().headless)
    {
        Apps::getAppEnvironment().headless = true;
        Apps::setAlertPresenter(std::move(presenter));
    }

    // A queue that does not drain fails the case that stranded it, rather than
    // a later one its callbacks would write into.
    ~AlertIsolation()
    {
        Apps::setAlertPresenter(nullptr);
        pumpUntil([] { return Apps::Detail::pendingAlertCount() == 0; });
        check(Apps::Detail::pendingAlertCount() == 0);
        Apps::getAppEnvironment().headless = previous;
    }

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

AlertOptions withButtons(Vector<std::string> buttons,
                         int defaultButton = 0,
                         std::optional<int> cancelButton = {})
{
    auto options = AlertOptions {};
    options.buttons = std::move(buttons);
    options.defaultButton = defaultButton;
    options.cancelButton = cancelButton;
    return Apps::Detail::resolveAlertOptions(options);
}

AlertOptions withoutCancel()
{
    auto options = saveChanges();
    options.cancelButton.reset();
    options.defaultButton = 1;
    return options;
}

// Options and resolution

auto tPresenterGetsTheOptions = test("Alert/presenterIsHandedTheOptionsAsGiven") = []
{
    auto shown = Shown {};
    auto isolation = AlertIsolation {recordInto(shown)};

    check(runLoopUntil([] { Apps::showAlert(saveChanges()); },
                       [&] { return shown.alerts.size() == 1; }));

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

    shown.answers[0](0);
};

auto tEachIconArrives = test("Alert/eachIconReachesThePresenter") = []
{
    auto shown = Shown {};
    auto isolation = AlertIsolation {
        [&shown](const AlertOptions& options, Apps::AlertCallback onDismissed)
        {
            shown.alerts.add(options);
            onDismissed(0);
        }};

    check(runLoopUntil(
        []
        {
            for (auto icon: {AlertIcon::info, AlertIcon::warning, AlertIcon::error})
            {
                auto options = AlertOptions {};
                options.icon = icon;
                Apps::showAlert(options);
            }
        },
        [&] { return shown.alerts.size() == 3; }));

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
    check(options.buttons.size() == 1 && options.buttons[0] == "OK");
    check(options.defaultButton == 0);
    check(!options.cancelButton.has_value());
};

auto tNoButtonsIsOk = test("Alert/noButtonsReachesThePresenterAsOneOk") = []
{
    auto shown = Shown {};
    auto isolation = AlertIsolation {recordInto(shown)};

    check(runLoopUntil(
        []
        {
            auto options = AlertOptions {};
            options.title = "Saved";
            Apps::showAlert(options);
        },
        [&] { return shown.alerts.size() == 1; }));

    check(shown.alerts.size() == 1 && shown.alerts[0].buttons.size() == 1
          && shown.alerts[0].buttons[0] == "OK");

    if (shown.answers.size() == 1)
        shown.answers[0](0);
};

auto tOutOfRangeDefault = test("Alert/outOfRangeDefaultIsTheFirstButton") = []
{
    for (auto index: {-1, 2, 99})
        check(withButtons({"Yes", "No"}, index).defaultButton == 0);

    check(withButtons({"Yes", "No"}, 1).defaultButton == 1);
};

auto tOutOfRangeCancel = test("Alert/outOfRangeCancelIsNoCancelButton") = []
{
    for (auto index: {-1, 2, 99})
        check(!withButtons({"Yes", "No"}, 0, index).cancelButton);

    check(withButtons({"Yes", "No"}, 0, 1).cancelButton == 1);
    check(withButtons({}, 0, 0).cancelButton == 0);
};

// The unanswered rule

auto tUnansweredIsCancel =
    test("Alert/unansweredIsTheCancelButtonWhenThereIsOne") = []
{
    check(Apps::Detail::unansweredButton(withButtons({"Keep", "Delete"}, 1, 0))
          == 0);
    check(Apps::Detail::unansweredButton(saveChanges()) == 2);
};

auto tUnansweredIsDefault =
    test("Alert/unansweredIsTheDefaultButtonWithoutCancel") = []
{
    check(Apps::Detail::unansweredButton(withButtons({"Keep", "Delete"}, 1)) == 1);
    check(Apps::Detail::unansweredButton(withButtons({})) == 0);
};

auto tAnswerForButton = test("Alert/aResponseThatIsNoButtonIsUnanswered") = []
{
    auto options = withButtons({"Keep", "Delete"}, 1, 0);

    check(Apps::Detail::answerForButton(options, 0) == 0);
    check(Apps::Detail::answerForButton(options, 1) == 1);

    // NSModalResponseStop (-1000) less NSAlertFirstButtonReturn (1000): a quit.
    check(Apps::Detail::answerForButton(options, -2000) == 0);
    check(Apps::Detail::answerForButton(options, 2) == 0);

    auto noCancel = withButtons({"Keep", "Delete"}, 1);
    check(Apps::Detail::answerForButton(noCancel, -2000) == 1);
};

// Windows' mappings

auto tTaskDialogButtons = test("Alert/taskDialogButtonIdsAreTheirIndexes") = []
{
    auto options = saveChanges();

    check(Apps::Detail::answerForTaskDialog(options, Ids::firstTaskDialogButton)
          == 0);
    check(Apps::Detail::answerForTaskDialog(options, Ids::firstTaskDialogButton + 1)
          == 1);
    check(Apps::Detail::answerForTaskDialog(options, Ids::firstTaskDialogButton + 2)
          == 2);
};

auto tTaskDialogCancel = test("Alert/taskDialogCancelIsTheCancelButton") = []
{
    check(Apps::Detail::answerForTaskDialog(withButtons({"Cancel", "Delete"}, 1, 0),
                                            Ids::cancel)
          == 0);
    check(Apps::Detail::answerForTaskDialog(withButtons({"Keep", "Delete"}, 1),
                                            Ids::cancel)
          == 1);
};

auto tTaskDialogOther = test("Alert/taskDialogIdsThatAreNoButtonAreUnanswered") = []
{
    auto options = withButtons({"Keep", "Delete"}, 1, 0);

    for (auto id: {0, Ids::ok, Ids::yes, Ids::firstTaskDialogButton + 2, -1})
        check(Apps::Detail::answerForTaskDialog(options, id) == 0);
};

auto tMessageBoxOne = test("Alert/messageBoxOneButtonIsOk") = []
{
    auto options = withButtons({});
    auto plan = Apps::Detail::planMessageBox(options);

    check(plan.buttons == MessageBoxButtons::ok);
    check(plan.answers.size() == 1 && plan.answers[0] == 0);
    check(!plan.labelsInText);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::ok) == 0);
    check(Apps::Detail::messageBoxText(options, plan) == options.message);
};

auto tMessageBoxCancelStandsForCancel =
    test("Alert/messageBoxCancelStandsForTheCancelButton") = []
{
    auto options = withButtons({"Cancel", "Delete"}, 1, 0);
    auto plan = Apps::Detail::planMessageBox(options);

    check(plan.buttons == MessageBoxButtons::okCancel);
    check(plan.answers.size() == 2 && plan.answers[0] == 1 && plan.answers[1] == 0);
    check(plan.defaultBoxButton == 0);
    check(plan.labelsInText);

    check(Apps::Detail::answerForMessageBox(options, plan, Ids::cancel) == 0);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::ok) == 1);

    auto text = Apps::Detail::messageBoxText(options, plan);
    check(text.find("OK = Delete") != std::string::npos);
    check(text.find("Cancel = Cancel") != std::string::npos);
};

auto tMessageBoxYesNo = test("Alert/messageBoxTwoButtonsWithoutCancelAreYesNo") = []
{
    auto options = withButtons({"Yes", "No"}, 1);
    auto plan = Apps::Detail::planMessageBox(options);

    check(plan.buttons == MessageBoxButtons::yesNo);
    check(plan.defaultBoxButton == 1);
    check(!plan.labelsInText);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::yes) == 0);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::no) == 1);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::cancel) == 1);
};

auto tMessageBoxThree = test("Alert/messageBoxThreeButtonsAreYesNoCancel") = []
{
    auto options = withButtons({"Cancel", "Save", "Don't Save"}, 1, 0);
    auto plan = Apps::Detail::planMessageBox(options);

    check(plan.buttons == MessageBoxButtons::yesNoCancel);
    check(plan.answers.size() == 3 && plan.answers[0] == 1 && plan.answers[1] == 2
          && plan.answers[2] == 0);
    check(plan.defaultBoxButton == 0);
    check(plan.labelsInText);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::yes) == 1);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::no) == 2);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::cancel) == 0);
};

auto tMessageBoxThreeNoCancel =
    test("Alert/messageBoxThreeButtonsWithoutCancelCannotBeEscaped") = []
{
    auto options = withButtons({"Save", "Discard", "Delete all"}, 0);
    auto plan = Apps::Detail::planMessageBox(options);

    check(plan.buttons == MessageBoxButtons::abortRetryIgnore);
    check(plan.answers.size() == 3 && plan.answers[0] == 0 && plan.answers[1] == 1
          && plan.answers[2] == 2);
    check(plan.defaultBoxButton == 0);
    check(plan.labelsInText);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::abort) == 0);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::retry) == 1);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::ignore) == 2);

    // Escape and the close box: nothing the box can send, so never the third.
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::cancel) == 0);

    auto text = Apps::Detail::messageBoxText(options, plan);
    check(text.find("Abort = Save") != std::string::npos);
    check(text.find("Ignore = Delete all") != std::string::npos);
};

auto tMessageBoxTooMany = test("Alert/messageBoxCannotOfferFiveButtons") = []
{
    auto options = withButtons({"One", "Two", "Three", "Four", "Five"}, 2, 4);
    auto plan = Apps::Detail::planMessageBox(options);

    check(plan.answers.empty());
    check(plan.labelsInText);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::ok) == 4);

    auto text = Apps::Detail::messageBoxText(options, plan);
    check(text.find("Five") != std::string::npos);
};

auto tMessageBoxUnknownId =
    test("Alert/messageBoxIdsThatAreNoButtonAreUnanswered") = []
{
    auto options = withButtons({"Cancel", "Delete"}, 1, 0);
    auto plan = Apps::Detail::planMessageBox(options);

    check(Apps::Detail::answerForMessageBox(options, plan, 0) == 0);
    check(Apps::Detail::answerForMessageBox(options, plan, Ids::yes) == 0);
};

// Answers

auto tCallbackGetsTheAnswer =
    test("Alert/callbackGetsTheButtonThePresenterAnswers") = []
{
    auto shown = Shown {};
    auto answers = Vector<int> {};
    auto isolation = AlertIsolation {recordInto(shown)};

    check(runLoopUntil(
        [&]
        {
            Apps::showAlert(saveChanges(), [&](int button) { answers.add(button); });
        },
        [&] { return shown.answers.size() == 1; }));

    if (shown.answers.size() != 1)
        return;

    shown.answers[0](1);

    check(answers.size() == 1 && answers[0] == 1);
};

auto tAnswerOnlyOnce = test("Alert/callbackFiresOnceHoweverOftenItIsAnswered") = []
{
    auto shown = Shown {};
    auto answers = Vector<int> {};
    auto isolation = AlertIsolation {recordInto(shown)};

    check(runLoopUntil(
        [&]
        {
            Apps::showAlert(saveChanges(), [&](int button) { answers.add(button); });
        },
        [&] { return shown.answers.size() == 1; }));

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
    auto returnedBeforeAnything = false;
    auto isolation =
        AlertIsolation {[&](const AlertOptions&, Apps::AlertCallback done)
                        {
                            ++presented;
                            done(0);
                        }};

    check(runLoopUntil(
        [&]
        {
            Apps::showAlert({}, [&](int) { ++answers; });
            returnedBeforeAnything = presented == 0 && answers == 0;
        },
        [&] { return answers == 1; }));

    check(returnedBeforeAnything);
    check(presented == 1);
};

auto tPresenterOnMainThread =
    test("Alert/aWorkerThreadsAlertIsShownOnTheMainThread") = []
{
    auto presentedOnMain = std::atomic<int> {-1};
    auto answeredOnMain = std::atomic<int> {-1};
    auto worker = std::thread();
    auto isolation =
        AlertIsolation {[&](const AlertOptions&, Apps::AlertCallback done)
                        {
                            presentedOnMain = Threads::isMainThread() ? 1 : 0;
                            done(0);
                        }};

    check(runLoopUntil(
        [&]
        {
            worker = std::thread(
                [&]
                {
                    Apps::showAlert(
                        {},
                        [&](int)
                        { answeredOnMain = Threads::isMainThread() ? 1 : 0; });
                });
        },
        [&] { return answeredOnMain != -1; }));

    if (worker.joinable())
        worker.join();

    check(presentedOnMain == 1);
    check(answeredOnMain == 1);
};

auto tWorkerAnswerReachesMain =
    test("Alert/aPresenterAnsweringFromAWorkerIsHeardOnTheMainThread") = []
{
    auto answeredOnMain = std::atomic<int> {-1};
    auto answer = std::atomic<int> {-1};
    auto answerer = std::thread();
    auto isolation =
        AlertIsolation {[&](const AlertOptions&, Apps::AlertCallback done)
                        { answerer = std::thread([done] { done(1); }); }};

    check(runLoopUntil(
        [&]
        {
            Apps::showAlert(saveChanges(),
                            [&](int button)
                            {
                                answer = button;
                                answeredOnMain = Threads::isMainThread() ? 1 : 0;
                            });
        },
        [&] { return answeredOnMain != -1; }));

    if (answerer.joinable())
        answerer.join();

    check(answeredOnMain == 1);
    check(answer == 1);
};

// The queue

auto tOneAtATime = test("Alert/theNextAlertWaitsForTheOneBeforeItsAnswer") = []
{
    auto shown = Shown {};
    auto order = Vector<std::string> {};
    auto isolation = AlertIsolation {recordInto(shown)};

    check(runLoopUntil(
        [&]
        {
            for (auto name: {"first", "second"})
            {
                auto options = AlertOptions {};
                options.title = name;
                Apps::showAlert(options, [&order, name](int) { order.add(name); });
            }
        },
        [&] { return shown.alerts.size() == 1; }));

    pumpBriefly();
    check(shown.alerts.size() == 1);
    check(Apps::Detail::pendingAlertCount() == 2);

    if (shown.answers.size() != 1)
        return;

    shown.answers[0](0);

    check(pumpUntil([&] { return shown.alerts.size() == 2; }));
    check(shown.alerts.size() == 2 && shown.alerts[1].title == "second");

    if (shown.answers.size() == 2)
        shown.answers[1](0);

    check(order.size() == 2 && order[0] == "first" && order[1] == "second");
};

auto tAlertFromCallback =
    test("Alert/anAlertRaisedFromAnAlertsCallbackFollowsIt") = []
{
    auto order = Vector<std::string> {};
    auto isolation = AlertIsolation {};

    check(runLoopUntil(
        [&]
        {
            auto first = AlertOptions {};
            first.title = "first";

            Apps::showAlert(first,
                            [&](int)
                            {
                                order.add("first");

                                auto second = AlertOptions {};
                                second.title = "second";
                                Apps::showAlert(second,
                                                [&](int) { order.add("second"); });
                            });
        },
        [&] { return order.size() == 2; }));

    check(order.size() == 2 && order[0] == "first" && order[1] == "second");
};

auto tSilentPresenter =
    test("Alert/aPresenterThatNeverAnswersHoldsTheQueueUntilReplaced") = []
{
    auto presented = 0;
    auto answers = Vector<int> {};
    auto held = Vector<Apps::AlertCallback> {};
    auto isolation =
        AlertIsolation {[&](const AlertOptions&, Apps::AlertCallback done)
                        {
                            ++presented;
                            held.add(std::move(done));
                        }};

    check(runLoopUntil(
        [&]
        {
            Apps::showAlert(withButtons({"Keep", "Delete"}, 1, 0),
                            [&](int button) { answers.add(button); });
            Apps::showAlert(withButtons({"Keep", "Delete"}, 1),
                            [&](int button) { answers.add(button); });
        },
        [&] { return presented == 1; }));

    pumpBriefly();
    check(presented == 1);
    check(answers.empty());

    Apps::setAlertPresenter(nullptr);

    check(pumpUntil([&] { return answers.size() == 2; }));
    check(answers.size() == 2 && answers[0] == 0 && answers[1] == 1);
};

// Headless and no loop

auto tHeadlessIsUnanswered = test("Alert/headlessAnswersTheUnansweredButton") = []
{
    auto answers = Vector<int> {};
    auto isolation = AlertIsolation {};

    check(runLoopUntil(
        [&]
        {
            Apps::showAlert(saveChanges(), [&](int button) { answers.add(button); });
            Apps::showAlert(withoutCancel(),
                            [&](int button) { answers.add(button); });
        },
        [&] { return answers.size() == 2; }));

    pumpBriefly();

    check(answers.size() == 2 && answers[0] == 2 && answers[1] == 1);
};

auto tHeadlessOutOfRange = test("Alert/headlessOutOfRangeDefaultIsTheFirst") = []
{
    auto answer = -1;
    auto isolation = AlertIsolation {};

    check(runLoopUntil(
        [&]
        {
            auto options = withoutCancel();
            options.defaultButton = 7;
            Apps::showAlert(options, [&](int button) { answer = button; });
        },
        [&] { return answer != -1; }));

    check(answer == 0);
};

auto tHeadlessOnMainThread = test("Alert/headlessAnswerIsOnTheMainThread") = []
{
    auto answeredOnMain = std::atomic<int> {-1};
    auto worker = std::thread();
    auto isolation = AlertIsolation {};

    check(runLoopUntil(
        [&]
        {
            worker = std::thread(
                [&]
                {
                    Apps::showAlert(
                        saveChanges(),
                        [&](int)
                        { answeredOnMain = Threads::isMainThread() ? 1 : 0; });
                });
        },
        [&] { return answeredOnMain != -1; }));

    if (worker.joinable())
        worker.join();

    check(answeredOnMain == 1);
};

auto tHeadlessInOrder = test("Alert/headlessAlertsAreAnsweredInTheOrderShown") = []
{
    auto order = Vector<std::string> {};
    auto isolation = AlertIsolation {};

    check(runLoopUntil(
        [&]
        {
            for (auto name: {"first", "second", "third"})
            {
                auto options = AlertOptions {};
                options.title = name;
                Apps::showAlert(options, [&order, name](int) { order.add(name); });
            }
        },
        [&] { return order.size() == 3; }));

    check(order.size() == 3 && order[0] == "first" && order[1] == "second"
          && order[2] == "third");
};

auto tNoCallback = test("Alert/anAlertWithNoCallbackIsStillAnswered") = []
{
    auto isolation = AlertIsolation {};

    check(runLoopUntil([] { Apps::showAlert(saveChanges()); },
                       [] { return Apps::Detail::pendingAlertCount() == 0; }));
};

auto tPresenterBeatsHeadless = test("Alert/aPresenterIsAskedEvenWhenHeadless") = []
{
    auto shown = Shown {};
    auto answer = -1;
    auto isolation = AlertIsolation {recordInto(shown)};

    check(runLoopUntil(
        [&]
        { Apps::showAlert(saveChanges(), [&](int button) { answer = button; }); },
        [&] { return shown.answers.size() == 1; }));

    pumpBriefly();
    check(answer == -1);

    if (shown.answers.size() == 1)
        shown.answers[0](1);

    check(pumpUntil([&] { return answer != -1; }));
    check(answer == 1);
};

auto tClearedPresenter = test("Alert/clearingThePresenterRestoresThePlatform") = []
{
    auto shown = Shown {};
    auto answer = -1;
    auto isolation = AlertIsolation {recordInto(shown)};

    Apps::setAlertPresenter(nullptr);

    check(runLoopUntil(
        [&]
        { Apps::showAlert(saveChanges(), [&](int button) { answer = button; }); },
        [&] { return answer != -1; }));

    check(answer == 2);
    check(shown.alerts.empty());
};

auto tNoLoop = test("Alert/withNoEventLoopRunningItIsAnsweredAtOnce") = []
{
    // A copy some other case attached as a hosted loop always has a loop.
    if (Threads::isEventLoopRunning())
        return;

    auto answer = -1;
    auto isolation = AlertIsolation {};

    Apps::showAlert(saveChanges(), [&](int button) { answer = button; });

    check(answer == 2);
    check(Apps::Detail::pendingAlertCount() == 0);
};

auto tWorkerNoLoop =
    test("Alert/aWorkersAlertWithNoLoopRunningIsAnsweredOnTheWorker") = []
{
    if (Threads::isEventLoopRunning())
        return;

    auto answer = std::atomic<int> {-1};
    auto answeredOnWorker = std::atomic<int> {-1};
    auto isolation = AlertIsolation {};

    auto worker = std::thread(
        [&]
        {
            auto workerId = std::this_thread::get_id();
            Apps::showAlert(saveChanges(),
                            [&, workerId](int button)
                            {
                                answer = button;
                                answeredOnWorker =
                                    std::this_thread::get_id() == workerId ? 1 : 0;
                            });
        });

    worker.join();

    check(answer == 2);
    check(answeredOnWorker == 1);
    check(Apps::Detail::pendingAlertCount() == 0);
};

// What lets the next alert in, and what an answer nobody gives becomes

auto tThrowingCallback =
    test("Alert/aCallbackThatThrowsStillLetsTheNextAlertIn") = []
{
    auto shown = Shown {};
    auto caught = false;
    auto secondAnswered = false;
    auto isolation = AlertIsolation {recordInto(shown)};

    check(runLoopUntil(
        [&]
        {
            Apps::showAlert(saveChanges(),
                            [](int) { throw std::runtime_error("first"); });
            Apps::showAlert(saveChanges(), [&](int) { secondAnswered = true; });
        },
        [&] { return shown.alerts.size() == 1; }));

    if (shown.answers.size() != 1)
        return;

    try
    {
        shown.answers[0](0);
    }
    catch (const std::runtime_error&)
    {
        caught = true;
    }

    check(caught);
    check(pumpUntil([&] { return shown.alerts.size() == 2; }));

    if (shown.answers.size() == 2)
        shown.answers[1](0);

    check(secondAnswered);
};

auto tNestedWait = test("Alert/aCallbackPumpingForTheNextAlertSeesItAnswered") = []
{
    auto secondAnswered = false;
    auto sawSecond = -1;
    auto isolation =
        AlertIsolation {[](const AlertOptions&, Apps::AlertCallback done)
                        { Threads::callAfter(Time::MS {10}, [done] { done(0); }); }};

    check(runLoopUntil(
        [&]
        {
            Apps::showAlert(saveChanges(),
                            [&](int)
                            {
                                for (auto slice = 0; slice < 75 && !secondAnswered;
                                     ++slice)
                                    Threads::runEventLoopFor(Time::MS {20});

                                sawSecond = secondAnswered ? 1 : 0;
                            });
            Apps::showAlert(saveChanges(), [&](int) { secondAnswered = true; });
        },
        [&] { return sawSecond != -1; }));

    check(sawSecond == 1);
};

auto tDroppedAnswer =
    test("Alert/anAnswerDroppedUnansweredAnswersTheUnansweredButton") = []
{
    auto answers = Vector<int> {};
    auto isolation =
        AlertIsolation {[](const AlertOptions&, Apps::AlertCallback) {}};

    check(runLoopUntil(
        [&]
        {
            Apps::showAlert(saveChanges(), [&](int button) { answers.add(button); });
            Apps::showAlert(withoutCancel(),
                            [&](int button) { answers.add(button); });
        },
        [&] { return answers.size() == 2; }));

    check(answers.size() == 2 && answers[0] == 2 && answers[1] == 1);
};

auto tGeneration =
    test("Alert/replacingThePresenterSparesTheAlertTheNewOneHolds") = []
{
    auto first = Shown {};
    auto second = Shown {};
    auto answers = Vector<std::string> {};
    auto isolation = AlertIsolation {recordInto(first)};

    check(runLoopUntil(
        [&]
        {
            for (auto name: {"A", "B"})
            {
                auto options = AlertOptions {};
                options.title = name;
                Apps::showAlert(options,
                                [&answers, name](int) { answers.add(name); });
            }
        },
        [&] { return first.answers.size() == 1; }));

    if (first.answers.size() != 1)
        return;

    // A's answer posts the next alert's turn; the worker's replacement posts its
    // abandon check behind it, so B reaches the new presenter first and the
    // check must leave it be.
    first.answers[0](0);

    auto worker = std::thread([&] { Apps::setAlertPresenter(recordInto(second)); });
    worker.join();

    check(pumpUntil([&] { return second.alerts.size() == 1; }));
    pumpBriefly();

    check(second.alerts.size() == 1 && second.alerts[0].title == "B");
    check(answers.size() == 1 && answers[0] == "A");
};

struct UnregistersOnDestruction
{
    explicit UnregistersOnDestruction(bool& doneToUse)
        : done(doneToUse)
    {
    }

    ~UnregistersOnDestruction()
    {
        Apps::setAlertPresenter(nullptr);
        done = true;
    }

    bool& done;
};

auto tPresenterDestructor =
    test("Alert/aPresenterWhoseDestructorUnregistersDoesNotDeadlock") = []
{
    auto unregistered = false;
    auto isolation = AlertIsolation {};

    {
        auto owner = std::make_shared<UnregistersOnDestruction>(unregistered);
        Apps::setAlertPresenter(
            [owner](const AlertOptions&, Apps::AlertCallback) {});
    }

    Apps::setAlertPresenter(nullptr);

    check(unregistered);
};
} // namespace AlertTests
