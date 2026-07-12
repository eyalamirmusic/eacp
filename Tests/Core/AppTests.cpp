#include "Common.h"

#include <thread>

using namespace nano;
using eacp::Apps::App;
using eacp::Apps::getAppFactory;
using eacp::Apps::getGlobalApp;
using eacp::Apps::quit;
using eacp::Apps::restart;
using eacp::Threads::callAsync;
using eacp::Threads::runEventLoopFor;
using eacp::Threads::stopEventLoop;

namespace
{

struct CountedPayload
{
    static inline std::atomic<int> ctorCount {0};
    static inline std::atomic<int> dtorCount {0};

    CountedPayload() { ++ctorCount; }
    ~CountedPayload() { ++dtorCount; }
};

void resetCounters()
{
    CountedPayload::ctorCount = 0;
    CountedPayload::dtorCount = 0;
}

// App + factory live in process-wide singletons, so leaking state
// between cases would let earlier tests poison later ones. Each test
// calls this on entry to start from a known-empty baseline.
void resetAppState()
{
    getGlobalApp().reset();
    getAppFactory() = {};
    resetCounters();
}

void installCountedFactory()
{
    getAppFactory() = [] { getGlobalApp().create<App<CountedPayload>>(); };
}

} // namespace

auto tGlobalAppIsSingleton = test("App/getGlobalAppReturnsSameInstance") = []
{
    resetAppState();
    check(&getGlobalApp() == &getGlobalApp());
};

auto tFactoryIsSingleton = test("App/getAppFactoryReturnsSameInstance") = []
{
    resetAppState();
    check(&getAppFactory() == &getAppFactory());
};

auto tFactoryStartsEmpty = test("App/getAppFactoryStartsEmpty") = []
{
    resetAppState();
    check(!getAppFactory());
};

auto tRunStyleFactoryCreatesApp =
    test("App/factoryInvocationCreatesAppInstance") = []
{
    resetAppState();
    installCountedFactory();

    check(getGlobalApp().get() == nullptr);
    check(CountedPayload::ctorCount == 0);

    getAppFactory()();

    check(getGlobalApp().get() != nullptr);
    check(CountedPayload::ctorCount == 1);
    check(CountedPayload::dtorCount == 0);

    getGlobalApp().reset();
    check(CountedPayload::dtorCount == 1);

    resetAppState();
};

auto tRestartReplacesInstance =
    test("App/restartDestroysOldInstanceAndCreatesNew") = []
{
    resetAppState();
    installCountedFactory();

    auto stopped =
        runEventLoopFor(eacp::Time::MS {2000},
                        []
                        {
                            getAppFactory()();

                            callAsync(
                                []
                                {
                                    check(CountedPayload::ctorCount == 1);
                                    check(CountedPayload::dtorCount == 0);
                                    check(getGlobalApp().get() != nullptr);

                                    restart();

                                    // restart() posts its destroy+recreate via callAsync;
                                    // this callAsync is queued right after and runs once
                                    // that work is done (FIFO ordering on the runloop).
                                    callAsync(
                                        []
                                        {
                                            check(CountedPayload::ctorCount == 2);
                                            check(CountedPayload::dtorCount == 1);
                                            check(getGlobalApp().get() != nullptr);
                                            stopEventLoop();
                                        });
                                });
                        });

    check(stopped);
    resetAppState();
};

auto tRestartWithoutFactoryIsSafe = test("App/restartIsNoOpWhenFactoryIsEmpty") = []
{
    resetAppState();

    auto stopped =
        runEventLoopFor(eacp::Time::MS {2000},
                        []
                        {
                            restart();

                            callAsync(
                                []
                                {
                                    check(getGlobalApp().get() == nullptr);
                                    check(CountedPayload::ctorCount == 0);
                                    stopEventLoop();
                                });
                        });

    check(stopped);
    resetAppState();
};

auto tRestartFromAnyThread = test("App/restartIsSafeFromBackgroundThread") = []
{
    resetAppState();
    installCountedFactory();

    auto worker = std::thread();

    auto stopped = runEventLoopFor(eacp::Time::MS {2000},
                                   [&]
                                   {
                                       getAppFactory()();

                                       worker = std::thread(
                                           []
                                           {
                                               restart();
                                               callAsync([] { stopEventLoop(); });
                                           });
                                   });

    if (worker.joinable())
        worker.join();

    check(stopped);
    check(CountedPayload::ctorCount == 2);
    check(CountedPayload::dtorCount == 1);
    resetAppState();
};

// quit() only stops the loop; the app must survive until the loop has fully
// unwound and is destroyed afterwards (run<T>()'s destroyApp call), so no
// nested native pump can still be delivering events to its views.
auto tQuitStopsLoopAndKeepsAppUntilTeardown =
    test("App/quitStopsLoopWithoutDestroyingApp") = []
{
    resetAppState();
    installCountedFactory();

    auto stopped = runEventLoopFor(eacp::Time::MS {2000},
                                   []
                                   {
                                       getAppFactory()();
                                       callAsync([] { quit(); });
                                   });

    check(stopped);
    check(CountedPayload::ctorCount == 1);
    check(CountedPayload::dtorCount == 0);
    check(getGlobalApp().get() != nullptr);

    eacp::Apps::destroyApp();
    check(CountedPayload::dtorCount == 1);
    check(getGlobalApp().get() == nullptr);

    resetAppState();
};

auto tQuitWithReturnValueStoresIt = test("App/quitWithReturnValueStoresIt") = []
{
    resetAppState();
    installCountedFactory();
    eacp::Apps::setReturnValue(0);

    auto stopped = runEventLoopFor(eacp::Time::MS {2000},
                                   []
                                   {
                                       getAppFactory()();
                                       callAsync([] { quit(7); });
                                   });

    check(stopped);
    check(eacp::Apps::getReturnValue() == 7);

    eacp::Apps::destroyApp();
    eacp::Apps::setReturnValue(0);
    resetAppState();
};

// Test binaries are local builds — unsigned or linker ad-hoc signed — so the
// distribution check must say no. (The positive case needs a Developer
// ID/Authenticode-signed binary, which a local test run can't produce.)
auto tLocalBuildIsNotDistributionSigned =
    test("App/localBuildIsNotDistributionSigned") = []
{ check(!eacp::Apps::isDistributionSigned()); };
